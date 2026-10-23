// `hdf-convert test text`: the oracle test of module TX (plan 3.3).
//
//   hdf-convert test text --oracle <pinned .dusk> --hd <TP HD title/content folder or .wua> --gc <GC files folder>
//                         [--golden <text.tsv>] [--verbose]
//
// 1. text::build on the hash-checked inputs (CheckedSource over the HD source and a GC FolderSource):
//    res/cave/hdtext/messages.bin, res/cave/hdtext/flows.json and res/stamps/hdtext/messages.bin byte-identical to
//    the pinned oracle and to the embedded expected list; the dropped res/stamps/hdtext/flows.json too (extra).
// 2. The same build with the 13 GC files in a MemorySource (as the mod reads them): identical outputs.
// 3. Golden list (tools/dev/golden_text.py, default <exe folder>/golden/text.tsv): the line-limit table and the GC
//    sample counts, stage groups, GC charset, font metrics, the 80 re-wrapped texts (hash + note), every message
//    record (text, entry, preview hashes) and the output hashes, all equal to the Python spec.
// 4. A stop request before the build gives Error{"stopped"}.
// Exit 0 pass, 1 fail, 2 usage, 3 skipped (no --hd/--gc, or no golden list).
#include "../cli.hpp"

#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"
#include "hdf/text.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace hdf;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Run {
    int failures = 0;
    bool verbose = false;
    void check(bool ok, const std::string& what) {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) ++failures;
    }
};

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t s = 0;
    while (true) {
        const std::size_t t = line.find('\t', s);
        out.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
        if (t == std::string::npos) break;
        s = t + 1;
    }
    return out;
}

const char* kActions[] = {"native", "override", "register"};

struct Outputs {
    std::vector<std::pair<std::string, Bytes>> files;  // rel -> bytes, the 3 kept + stamps flows.json (dropped)
};

Outputs outputs_of(const text::Output& o, const text::TextDiag& d) {
    return Outputs{{{"res/cave/hdtext/messages.bin", o.cave_messages_bin},
                    {"res/cave/hdtext/flows.json", o.cave_flows_json},
                    {"res/stamps/hdtext/messages.bin", o.stamps_messages_bin},
                    {"res/stamps/hdtext/flows.json", d.stamps_flows_json}}};
}

// The golden rows as the C++ side computes them (same format as tools/dev/golden_text.py).
std::vector<std::vector<std::string>> rows_of(const text::TextDiag& d, const Outputs& outs) {
    std::vector<std::vector<std::string>> rows;
    std::map<int, int> samples(d.line_samples.begin(), d.line_samples.end());
    for (const auto& [k, v] : d.line_limits)
        rows.push_back({"limit", std::to_string(k), std::to_string(v), std::to_string(samples.count(k) ? samples[k] : 0)});
    for (const auto& [n, g] : d.stage_groups) rows.push_back({"group", n, std::to_string(g)});
    rows.push_back({"chars", std::to_string(d.gc_chars.size()), sha256_hex(d.gc_chars)});
    rows.push_back({"font", std::to_string(d.font_cell), std::to_string(d.font_cell_w), io::py_float_repr(d.font_name)});
    for (const auto& r : d.rewraps)
        rows.push_back({"rewrap", r.part, r.file, std::to_string(r.id), std::to_string(r.box), std::to_string(r.limit),
                        std::to_string(r.text.size()), sha256_hex(r.text), r.note});
    std::map<std::string, int> n;
    for (const auto& m : d.messages) {
        const std::string_view pv(m.preview);
        rows.push_back({"message", m.part, std::to_string(n[m.part]++), m.file, std::to_string(m.group),
                        std::to_string(m.id), kActions[m.action < 3 ? m.action : 2], std::to_string(m.text.size()),
                        sha256_hex(m.text), sha256_hex(View(m.entry.data(), m.entry.size())),
                        sha256_hex(View(reinterpret_cast<const u8*>(pv.data()), pv.size()))});
    }
    for (const auto& [rel, b] : outs.files) rows.push_back({"file", rel, std::to_string(b.size()), sha256_hex(b)});
    return rows;
}

std::string join(const std::vector<std::string>& r) {
    std::string s;
    for (std::size_t i = 0; i < r.size(); ++i) s += (i ? "\t" : "") + r[i];
    return s;
}

int run(const cli::OracleArgs& a) {
    if (!a.oracle) {
        std::fprintf(stderr, "hdf-convert test text: --oracle is required (cache/oracle/hd_features-oracle-2.0.0.dusk)\n");
        return cli::kExitUsage;
    }
    if (!a.hd || !a.gc) {
        std::printf("hdf-convert test text: skipped: needs --hd and --gc\n");
        return cli::kExitTodo;
    }
    Run r;
    r.verbose = a.verbose;
    const auto t_all = Clock::now();

    // --- oracle (pinned)
    std::printf("oracle (%s)\n", cli::to_utf8(*a.oracle).c_str());
    auto pin = cli::file_sha256(*a.oracle);
    if (!pin || pin->first != outputs::kOracleSize || pin->second != outputs::kOracleSha256) {
        std::fprintf(stderr, "hdf-convert test text: the oracle is not the pinned file (sha256 %.*s)\n",
                     int(outputs::kOracleSha256.size()), outputs::kOracleSha256.data());
        return cli::kExitUsage;
    }
    auto oracle = ZipSource::open(*a.oracle);
    if (!oracle) return cli::report("test text", oracle.error());

    // --- inputs
    auto hd_raw = cli::open_hd(*a.hd);
    if (!hd_raw) return cli::report("test text", hd_raw.error());
    FolderSource gc_folder(*a.gc);
    inputs::CheckedSource hd(**hd_raw, inputs::hd_files(), "hd");
    inputs::CheckedSource gc(gc_folder, inputs::gc_files(), "gc");
    std::printf("inputs: %s; gc %s\n", (*hd_raw)->describe().c_str(), gc_folder.describe().c_str());

    // --- 1. build, compare with the oracle and the embedded list
    std::printf("1. build (GC from the folder)\n");
    text::TextDiag diag;
    auto t0 = Clock::now();
    auto out = text::build(hd, gc, {}, &diag);
    const double t_build = since(t0);
    if (!out) {
        r.check(false, "text::build: " + out.error().code + " " + out.error().detail);
        return cli::kExitFail;
    }
    std::printf("  time build: %s (inside build: %.3f s)\n", cli::time_line(t_build).c_str(), diag.seconds);
    const Outputs outs = outputs_of(*out, diag);
    for (const auto& [rel, bytes] : outs.files) {
        auto want = oracle.value()->read(rel);
        const bool same = want && *want == bytes;
        std::size_t first = 0;
        if (want && !same)
            while (first < bytes.size() && first < want->size() && bytes[first] == (*want)[first]) ++first;
        const outputs::ExpectedFile* e = outputs::find(rel);
        const bool dropped = outputs::is_dropped(rel);
        const bool listed = dropped || (e && e->size == bytes.size() && e->sha256 == sha256_hex(bytes));
        char b[256];
        if (same)
            std::snprintf(b, sizeof b, "%s: %zu B identical to the oracle%s", rel.c_str(), bytes.size(),
                          dropped ? " (dropped output, extra check)" : ", = embedded sha256");
        else
            std::snprintf(b, sizeof b, "%s: DIFF (%zu B vs oracle %zu B, first difference at %zu)", rel.c_str(),
                          bytes.size(), want ? want->size() : 0, first);
        r.check(same && listed, b);
    }

    // --- 2. GC from memory (as the mod: the 13 GC files read on the game thread into a MemorySource)
    std::printf("2. build (GC from a MemorySource)\n");
    {
        MemorySource mem("gc memory");
        bool ok = true;
        for (const inputs::InputFile& f : inputs::gc_files()) {
            auto b = gc_folder.read(f.path);
            if (!b) {
                ok = false;
                break;
            }
            mem.add(f.path, std::move(*b));
        }
        inputs::CheckedSource gcm(mem, inputs::gc_files(), "gc");
        text::TextDiag d2;
        t0 = Clock::now();
        auto out2 = ok ? text::build(hd, gcm, {}, &d2) : Result<text::Output>(fail("io.read", "gc"));
        const double t2 = since(t0);
        const bool same = out2 && out2->cave_messages_bin == out->cave_messages_bin &&
                          out2->cave_flows_json == out->cave_flows_json &&
                          out2->stamps_messages_bin == out->stamps_messages_bin &&
                          d2.stamps_flows_json == diag.stamps_flows_json;
        r.check(same, "GC memory = GC folder (3 outputs + stamps flows.json), second run " +
                          cli::time_line(t2));
    }

    // --- 3. golden list
    fs::path golden_path;
    if (auto gp = a.args ? a.args->path("golden") : std::nullopt) {
        golden_path = *gp;
    } else {
#ifdef _WIN32
        wchar_t exe[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        golden_path = fs::path(std::wstring(exe, n)).parent_path() / "golden" / "text.tsv";
#endif
    }
    bool golden_ran = false;
    std::ifstream gf(golden_path, std::ios::binary);
    if (!gf) {
        std::printf("3. golden list: skipped (%s not found); make it with\n"
                    "    python -I -B tools/dev/golden_text.py --hd <..> --gc <..> --out <build-conv*/golden/text.tsv>\n",
                    cli::to_utf8(golden_path).c_str());
    } else {
        golden_ran = true;
        std::printf("3. golden list (%s)\n", cli::to_utf8(golden_path).c_str());
        std::vector<std::vector<std::string>> want;
        std::string line;
        while (std::getline(gf, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            want.push_back(split_tabs(line));
        }
        const auto got = rows_of(diag, outs);
        std::map<std::string, std::pair<int, int>> per;  // kind -> (rows, mismatches)
        const std::size_t n = std::max(want.size(), got.size());
        int shown = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string kind = i < want.size() ? want[i][0] : got[i][0];
            auto& pc = per[kind];
            ++pc.first;
            const bool eq = i < want.size() && i < got.size() && want[i] == got[i];
            if (!eq) {
                ++pc.second;
                if (shown++ < 12) {
                    std::printf("       row %zu want: %s\n", i + 1, i < want.size() ? join(want[i]).c_str() : "-");
                    std::printf("       row %zu got:  %s\n", i + 1, i < got.size() ? join(got[i]).c_str() : "-");
                }
            }
        }
        for (const auto& [kind, pc] : per) {
            char b[160];
            std::snprintf(b, sizeof b, "golden %-8s %3d rows, %d differ", kind.c_str(), pc.first, pc.second);
            r.check(pc.second == 0, b);
        }
        r.check(want.size() == got.size(), "golden rows " + std::to_string(got.size()) + " = " + std::to_string(want.size()));
        r.check(diag.rewraps.size() == 80, "re-wrapped texts: " + std::to_string(diag.rewraps.size()) + " (plan: 80)");
        r.check(diag.line_limits.size() == 15, "line-limit table: " + std::to_string(diag.line_limits.size()) + " box kinds");
    }

    // --- 4. stop
    std::printf("4. stop\n");
    {
        std::stop_source ss;
        ss.request_stop();
        auto s = text::build(hd, gc, ss.get_token(), nullptr);
        r.check(!s && s.error().code == "stopped", "a stop request gives Error{\"stopped\"}");
    }

    std::printf("test text: %s, %d failure(s); %s\n", r.failures ? "FAIL" : "PASS", r.failures,
                cli::time_line(since(t_all)).c_str());
    if (r.failures) return cli::kExitFail;
    return golden_ran ? cli::kExitOk : cli::kExitTodo;
}

}  // namespace

HDF_ORACLE_TEST("text", "cave messages.bin + flows.json, stamps messages.bin; line-limit table and the 80 re-wraps", &run);
