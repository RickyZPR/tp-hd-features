// hdf-convert, module C2 helpers: --time output, the Pscene budget, --hd opening, seed-cache.
#include "cli.hpp"

#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace hdf::cli {

u64 peak_working_set() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return pmc.PeakWorkingSetSize;
#endif
    return 0;
}

std::string time_line(double seconds) {
    char b[128];
    std::snprintf(b, sizeof b, "wall %.3f s, peak working set %.1f MB", seconds,
                  static_cast<double>(peak_working_set()) / (1024.0 * 1024.0));
    return b;
}

std::optional<u64> pscene_budget(const std::filesystem::path& gc) {
    std::error_code ec;
    u64 largest = 0;
    bool any = false;
    for (std::filesystem::directory_iterator d(gc / "res" / "Particle", ec), end; !ec && d != end; d.increment(ec)) {
        std::string n = to_utf8(d->path().filename());
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (n.rfind("pscene", 0) != 0 || n.size() < 4 || n.substr(n.size() - 4) != ".jpc") continue;
        std::error_code sec;
        const u64 sz = d->file_size(sec);
        if (sec) continue;
        largest = std::max(largest, sz);
        any = true;
    }
    if (!any || largest < inputs::kPsceneSafety) return std::nullopt;
    return largest - inputs::kPsceneSafety;
}

Result<std::unique_ptr<Source>> open_hd(const std::filesystem::path& p) {
    auto s = inputs::open_hd_location(p);
    if (!s) return std::unexpected(s.error());
    return std::unique_ptr<Source>(std::move(*s));
}

Result<std::pair<u64, std::string>> file_sha256(const std::filesystem::path& p, std::stop_token stop) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return fail("io.open", to_utf8(p));
    Sha256 h;
    Bytes buf(std::size_t(4) << 20);
    u64 n = 0;
    while (f) {
        if (stop.stop_requested()) return fail("stopped");
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::size_t got = static_cast<std::size_t>(f.gcount());
        if (got == 0) break;
        h.update(View(buf.data(), got));
        n += got;
    }
    if (f.bad()) return fail("io.read", to_utf8(p));
    return std::pair<u64, std::string>{n, h.finish_hex()};
}

namespace {

std::string json_str(std::string_view s) {
    std::string o = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += ch;  // UTF-8 kept as is
        }
    }
    return o + "\"";
}

std::string utc_now() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

// cache.json as plan 2.4 shows it (format 1), written to cache.json.tmp and then moved over cache.json
// (MoveFileExW REPLACE_EXISTING | WRITE_THROUGH, retried for about 5 s: Correction #9).
Result<void> write_stamp(const std::filesystem::path& data_dir, const std::filesystem::path& files_dir,
                         const GenOut& out, const StampInfo& info) {
    const std::string set(outputs::embedded_set_id());
    std::string j = "{\n";
    j += " \"format\": " + std::to_string(kStampFormat) + ",\n";
    j += " \"mod\": " + json_str(kModId) + ",\n";
    j += " \"mod_version\": " + json_str(info.mod_version) + ",\n";
    j += " \"converter\": " + std::to_string(kConverterVersion) + ",\n";
    j += " \"set\": " + json_str(set) + ",\n";
    j += " \"folder\": " + json_str(to_utf8(files_dir.filename())) + ",\n";
    j += " \"made\": " + json_str(info.made) + ",\n";
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.3f", out.seconds);
    j += std::string(" \"seconds\": ") + secs + ",\n";
    j += " \"dusklight\": " + json_str(info.dusklight) + ",\n";
    j += " \"gc\": " + json_str(info.gc) + ",\n";
    j += " \"hd\": {\"kind\": " + json_str(info.hd.kind) + ", \"location\": " + json_str(info.hd.location) +
         ", \"title\": " + json_str(info.hd.title) + ", \"version\": " + std::to_string(info.hd.version) + "},\n";
    j += " \"files\": {\n";
    for (std::size_t i = 0; i < out.files.size(); ++i) {
        const OutputRecord& r = out.files[i];
        j += "  " + json_str(r.rel) + ": [" + std::to_string(r.size) + ", " + json_str(r.sha256) + "]" +
             (i + 1 < out.files.size() ? ",\n" : "\n");
    }
    j += " }\n}\n";

    const std::filesystem::path tmp = data_dir / "cache.json.tmp", dst = data_dir / "cache.json";
    // FolderSink flushes the file to disk before it returns (fix-wave1 L5); the outputs were written with it too.
    FolderSink stamp_sink(data_dir);
    if (auto ok = stamp_sink.write("cache.json.tmp", View(reinterpret_cast<const u8*>(j.data()), j.size())); !ok)
        return std::unexpected(ok.error());
#ifdef _WIN32
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
        const DWORD e = GetLastError();
        if ((e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) || attempt >= 50) {
            std::error_code ec(int(e), std::system_category());
            return fail("io.write", "cache.json: " + ec.message());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#else
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    if (ec) return fail("io.write", "cache.json: " + ec.message());
#endif
    return {};
}

}  // namespace

Result<SeedResult> seed_cache(const std::filesystem::path& oracle, const std::filesystem::path& data_dir,
                              std::string_view mod_version, std::stop_token stop) {
    const auto t0 = std::chrono::steady_clock::now();
    auto pin = file_sha256(oracle, stop);
    if (!pin) return std::unexpected(pin.error());
    if (pin->first != outputs::kOracleSize || pin->second != outputs::kOracleSha256)
        return fail("oracle.pin", to_utf8(oracle.filename()) + " is not the pinned oracle (sha256 " + pin->second + ")");
    const std::string set(outputs::embedded_set_id());
    if (outputs::compute_set_id(outputs::expected()) != set) return fail("tables", "set id of the embedded list differs");
    auto zip = ZipSource::open(oracle);
    if (!zip) return std::unexpected(zip.error());

    SeedResult res;
    res.files_dir = data_dir / ("files-" + set.substr(0, 8));
    FolderSink sink(res.files_dir);
    GenOut out;
    for (const outputs::ExpectedFile& e : outputs::expected()) {
        auto data = (*zip)->read(e.rel, stop);
        if (!data) return std::unexpected(data.error());
        const std::string sha = sha256_hex(*data);
        if (data->size() != e.size || sha != e.sha256) return fail("oracle.pin", "oracle entry differs: " + std::string(e.rel));
        if (auto ok = sink.write(e.rel, *data); !ok) return std::unexpected(ok.error());
        out.files.push_back(OutputRecord{std::string(e.rel), e.size, sha, e.overlay});
        ++res.files;
        res.bytes += e.size;
    }
    const outputs::CompareReport check = outputs::check(out.files);
    if (!check.pass() || check.identical != outputs::kKeptCount) return fail("out.check", check.summary());
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    StampInfo info;
    info.mod_version = std::string(mod_version);
    info.dusklight = std::string(kDusklightVersion);  // "2.0.3.0", as svc_host->version (fix-wave1 L6)
    info.gc = std::string(inputs::kGcGameId);
    info.made = utc_now();
    info.hd.kind = "seed";
    info.hd.location = to_utf8(oracle);
    auto committed = commit(data_dir, res.files_dir, out, info);
    if (!committed && committed.error().code == "todo") {
        res.stamp_from_cli = true;
        committed = write_stamp(data_dir, res.files_dir, out, info);
    }
    if (!committed) return std::unexpected(committed.error());
    return res;
}

}  // namespace hdf::cli
