// `hdf-convert test detect`: the oracle test of module D (plan 3.3 D). Owned by module D.
//
//   hdf-convert test detect --hd <TP HD title folder> --wua <TP HD .wua> [--wux <TP HD .wux>]
//                           [--golden <build dir>/golden/detect.tsv] [--appdata <d>] [--localappdata <d>] [--verbose]
//
// 1. Auto-detect as the mod sees it (%APPDATA% / %LOCALAPPDATA% from the environment or the options): the .wua
//    (title_list_cache) and the title folder (GamePaths) are usable, the .wux is encrypted, every candidate is
//    TP HD, usable ones come first with the folder before the .wua, encrypted ones last.
// 2. detect::open() of each usable candidate: the 136 inputs by SHA-256 (inputs::check_hashes).
// 3. Picked locations (plan 3.3 D): the title folder, its content folder, the .wua's folder, that folder's parent,
//    the .wua file, the .wux file; then the .wua as the remembered location (listed first).
// 4. --golden (tools/dev/golden_detect.py, the Python spec on this PC): the title list and settings parsers give
//    the same entries, and every usable / encrypted candidate of hdf_inputs.detect_hd is found the same way.
// Each step prints its wall time and the peak working set. Exit 0 pass, 1 fail, 2 usage, 3 skipped (no --hd/--wua).
// The synthetic fixtures (portable Cemu, mlc base + update, odd characters, entity-encoded paths, budgets,
// links, offline files) are unit tests: `hdf-convert self-test detect`.
#include "../cli.hpp"

#include "hdf/detect.hpp"
#include "hdf/inputs.hpp"
#include "hdf/source.hpp"

#include "../../src/base/base_util.hpp"
#include "../../src/detect/detect_internal.hpp"

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
namespace di = hdf::detect::internal;
using detect::Candidate;
using Clock = std::chrono::steady_clock;

struct Run {
    int failures = 0;
    void check(bool ok, const std::string& what) {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) ++failures;
    }
};

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

void step_time(const char* what, Clock::time_point t0) {
    std::printf("  time %s: %s\n", what, cli::time_line(since(t0)).c_str());
}

std::string key_sha(const fs::path& p) {
    const std::string k = di::path_key(p);
    return sha256_hex(View(reinterpret_cast<const u8*>(k.data()), k.size()));
}

std::string text_sha(std::string_view s) { return sha256_hex(View(reinterpret_cast<const u8*>(s.data()), s.size())); }

std::optional<fs::path> env_path(const wchar_t* name) {
#ifdef _WIN32
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return fs::path(v);
#else
    (void)name;
    return std::nullopt;
#endif
}

void print_list(const std::vector<Candidate>& cs) {
    for (const Candidate& c : cs)
        std::printf("       %-9s %-17s %s%s%s%s\n", std::string(detect::kind_name(c.kind)).c_str(), c.origin.c_str(),
                    cli::to_utf8(c.path).c_str(), c.title_dir.empty() ? "" : (" [" + c.title_dir + "]").c_str(),
                    c.update ? (" + update " + cli::to_utf8(*c.update)).c_str() : "",
                    c.problem.empty() ? "" : ("  (" + c.problem + ")").c_str());
}

const Candidate* by_path(const std::vector<Candidate>& cs, const fs::path& p) {
    const std::string k = di::path_key(p);
    for (const Candidate& c : cs)
        if (di::path_key(c.path) == k) return &c;
    return nullptr;
}

std::size_t index_of(const std::vector<Candidate>& cs, const Candidate* c) {
    return c ? static_cast<std::size_t>(c - cs.data()) : cs.size();
}

bool all_tphd(const std::vector<Candidate>& cs) {
    for (const Candidate& c : cs)
        if (!c.title_id.empty() && !detect::is_tphd_title(c.title_id)) return false;
    return true;
}

std::optional<std::string> read_text(const fs::path& p) {
    auto r = base::read_file(p, {}, u64(16) << 20);
    if (!r) return std::nullopt;
    return std::string(r->begin(), r->end());
}

struct Paths {
    std::optional<fs::path> hd, wua, wux;
};

// 1. + 2.
std::vector<Candidate> auto_detect(Run& r, const detect::Env& env, const Paths& p, bool verbose) {
    std::printf("auto-detect (Cemu title list, mlc, GamePaths)\n");
    const auto t0 = Clock::now();
    const std::vector<Candidate> found = detect::find(env, {});
    step_time("find", t0);
    print_list(found);
    r.check(all_tphd(found), "every candidate is TP HD (" + std::to_string(found.size()) + " found)");
    const Candidate* folder = p.hd ? by_path(found, *p.hd) : nullptr;
    const Candidate* wua = p.wua ? by_path(found, *p.wua) : nullptr;
    const Candidate* wux = p.wux ? by_path(found, *p.wux) : nullptr;
    if (p.hd)
        r.check(folder && folder->kind == Candidate::Folder && folder->origin == "game_paths" && folder->problem.empty(),
                "the title folder: usable folder from GamePaths");
    if (p.wua)
        r.check(wua && wua->kind == Candidate::Wua && wua->origin == "title_list_cache" && wua->problem.empty() &&
                    wua->title_dir == "000500001019e500_v81" && wua->version == 81 &&
                    wua->title_id == "000500001019E500",
                "the .wua: usable, from title_list_cache.xml, top 000500001019e500_v81, version 81");
    if (p.wux)
        r.check(wux && wux->kind == Candidate::Encrypted && wux->origin == "title_list_cache" &&
                    wux->problem == "encrypted .wux",
                "the .wux: encrypted, from title_list_cache.xml");
    if (folder && wua) r.check(index_of(found, folder) < index_of(found, wua), "the folder comes before the .wua");
    bool ranked = true;
    int last = 0;
    for (const Candidate& c : found) {
        // usable, readable with a problem, encrypted, update-only (fix pass L5)
        const int rank = c.problem.starts_with("update only") ? 3
                         : c.kind == Candidate::Encrypted   ? 2
                                                            : (c.problem.empty() ? 0 : 1);
        if (rank < last) ranked = false;
        last = rank;
    }
    r.check(ranked, "usable first, then problems, encrypted, update-only last");
    // 2. open() + the 136 hashes for each usable candidate
    for (const Candidate& c : found) {
        if (!c.problem.empty()) continue;
        const auto t1 = Clock::now();
        auto src = detect::open(c);
        if (!src) {
            r.check(false, "open " + cli::to_utf8(c.path) + ": " + src.error().code + " " + src.error().detail);
            continue;
        }
        auto rep = inputs::check_hashes(**src, inputs::hd_files(), nullptr, {});
        r.check(rep && rep->ok() && rep->hash_ok == rep->total && rep->total == 136,
                "open(" + std::string(detect::kind_name(c.kind)) + ") gives the 136 inputs by SHA-256: " +
                    (rep ? std::to_string(rep->hash_ok) : std::string("error")) + "/136");
        if (verbose) std::printf("       %s\n", (*src)->describe().c_str());
        step_time("open + hashes", t1);
    }
    return found;
}

// 3.
void picked(Run& r, const detect::Env& base_env, const Paths& p) {
    std::printf("picked and remembered locations\n");
    struct Case {
        std::string what;
        fs::path pick;
        std::vector<std::pair<fs::path, Candidate::Kind>> usable;  // in order
        std::optional<fs::path> encrypted;
        bool exact;
    };
    std::vector<Case> cases;
    if (p.hd) {
        cases.push_back({"the title folder", *p.hd, {{*p.hd, Candidate::Folder}}, {}, true});
        cases.push_back({"its content folder", *p.hd / "content", {{*p.hd / "content", Candidate::Folder}}, {}, true});
    }
    if (p.wua) {
        std::vector<std::pair<fs::path, Candidate::Kind>> both;
        if (p.hd && di::path_key(p.hd->parent_path()) == di::path_key(p.wua->parent_path()))
            both.push_back({*p.hd, Candidate::Folder});
        both.push_back({*p.wua, Candidate::Wua});
        std::optional<fs::path> enc;
        if (p.wux && di::path_key(p.wux->parent_path()) == di::path_key(p.wua->parent_path())) enc = p.wux;
        cases.push_back({"the folder with the .wua", p.wua->parent_path(), both, enc, false});
        cases.push_back({"that folder's parent", p.wua->parent_path().parent_path(), both, enc, false});
        cases.push_back({"the .wua file", *p.wua, {{*p.wua, Candidate::Wua}}, {}, true});
    }
    if (p.wux) cases.push_back({"the .wux file", *p.wux, {}, p.wux, true});
    for (const Case& c : cases) {
        detect::Env env = base_env;
        env.picked = c.pick;
        const auto t0 = Clock::now();
        const std::vector<Candidate> found = detect::find(env, {});
        const double secs = since(t0);
        bool ok = all_tphd(found);
        std::size_t usable = 0;
        for (const Candidate& f : found) {
            if (f.origin != "picked") ok = false;
            if (f.problem.empty()) ++usable;
        }
        for (std::size_t i = 0; i < c.usable.size(); ++i) {
            ok = ok && i < found.size() && di::path_key(found[i].path) == di::path_key(c.usable[i].first) &&
                 found[i].kind == c.usable[i].second && found[i].problem.empty();
        }
        if (c.encrypted) {
            const Candidate* e = by_path(found, *c.encrypted);
            ok = ok && e && e->kind == Candidate::Encrypted && e->title_id == "000500001019E500";
        }
        if (c.exact) ok = ok && found.size() == c.usable.size() + (c.encrypted ? 1 : 0);
        else ok = ok && usable == c.usable.size();
        char line[256];
        std::snprintf(line, sizeof line, "pick %s: %zu candidate(s), %zu usable, %.3f s", c.what.c_str(), found.size(),
                      usable, secs);
        r.check(ok, line);
        if (!ok) print_list(found);
    }
    if (p.wua) {
        detect::Env env = base_env;
        env.remembered = *p.wua;
        const std::vector<Candidate> found = detect::find(env, {});
        r.check(!found.empty() && di::path_key(found[0].path) == di::path_key(*p.wua) && found[0].origin == "remembered" &&
                    found[0].problem.empty(),
                "remembered .wua: searched first and listed first");
    }
}

// 4.
void golden(Run& r, const fs::path& file, const detect::Env& env, const std::vector<Candidate>& found) {
    std::printf("golden list (Python spec, tools/dev/golden_detect.py)\n");
    auto text = read_text(file);
    if (!text) {
        r.check(false, "read " + cli::to_utf8(file));
        return;
    }
    std::map<std::string, fs::path> dirs;  // config dir key sha -> dir
    for (const fs::path& d : {env.appdata / "Cemu", env.localappdata / "Cemu"}) dirs[key_sha(d)] = d;
    std::map<std::string, std::vector<std::string>> titles;  // dir sha -> expected lines
    std::istringstream in(*text);
    std::string line;
    u32 found_lines = 0, enc_lines = 0, check_lines = 0, settings_lines = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::size_t i = 0;
        while (true) {
            const std::size_t j = line.find('\t', i);
            f.push_back(line.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (j == std::string::npos) break;
            i = j + 1;
        }
        if (f[0] == "title" && f.size() == 6) {
            titles[f[1]].push_back(f[2] + "\t" + f[3] + "\t" + f[4] + "\t" + f[5]);
        } else if (f[0] == "settings" && f.size() == 5) {
            ++settings_lines;
            const auto d = dirs.find(f[1]);
            const auto xml = d == dirs.end() ? std::nullopt : read_text(d->second / "settings.xml");
            const detect::CemuSettings s = xml ? detect::parse_settings(*xml) : detect::CemuSettings{};
            std::string joined;
            for (std::size_t k = 0; k < s.game_paths.size(); ++k) joined += (k ? "\n" : "") + s.game_paths[k];
            r.check(xml && (s.mlc_path.empty() ? "-" : text_sha(s.mlc_path)) == f[2] &&
                        std::to_string(s.game_paths.size()) == f[3] && text_sha(joined) == f[4],
                    "settings.xml: mlc_path and " + f[3] + " GamePaths entr(y/ies) as the spec");
        } else if (f[0] == "found" && f.size() == 3) {
            ++found_lines;
            bool hit = false;
            for (const Candidate& c : found)
                if (key_sha(c.path) == f[1] && c.problem.empty() && c.kind != Candidate::Encrypted) hit = true;
            r.check(hit, "spec candidate (" + f[2] + ") found and usable");
        } else if (f[0] == "encrypted" && f.size() == 2) {
            ++enc_lines;
            bool hit = false;
            for (const Candidate& c : found)
                if (key_sha(c.path) == f[1] && c.kind == Candidate::Encrypted) hit = true;
            r.check(hit, "spec encrypted note found as encrypted");
        } else if (f[0] == "check" && f.size() == 3) {
            ++check_lines;
            if (f[2] == "skipped_zstd") {
                std::printf("  skip spec check of a .wua (this Python has no zstd)\n");
                continue;
            }
            bool usable = false, present = false;
            for (const Candidate& c : found)
                if (key_sha(c.path) == f[1]) {
                    present = true;
                    usable = c.problem.empty();
                }
            if (present) r.check(usable == (f[2] == "ok"), "spec check '" + f[2] + "' = usable in C++");
            else std::printf("  note: a spec check target is not an auto-detect candidate (picked cases cover it)\n");
        }
    }
    for (const auto& [dir_sha, expect] : titles) {
        const auto d = dirs.find(dir_sha);
        const auto xml = d == dirs.end() ? std::nullopt : read_text(d->second / "title_list_cache.xml");
        std::vector<std::string> got;
        if (xml)
            for (const detect::TitleListEntry& e : detect::parse_title_list(*xml))
                got.push_back(di::upper_ascii(e.title_id) + "\t" + std::to_string(e.format) + "\t" +
                              std::to_string(e.version) + "\t" + text_sha(e.path));
        r.check(got == expect, "title_list_cache.xml: " + std::to_string(got.size()) + " entries = the spec's " +
                                   std::to_string(expect.size()));
    }
    std::printf("  golden: %zu title list(s), %u settings, %u found, %u encrypted, %u checks\n", titles.size(),
                settings_lines, found_lines, enc_lines, check_lines);
}

int run(const cli::OracleArgs& a) {
    Paths p;
    p.hd = a.hd;
    p.wua = a.args->path("wua");
    p.wux = a.args->path("wux");
    if (!p.hd && !p.wua) {
        std::fprintf(stderr, "hdf-convert test detect: give --hd <TP HD title folder> and/or --wua <TP HD .wua> "
                             "(skipped)\n");
        return cli::kExitTodo;
    }
    detect::Env env;
    if (auto d = a.args->path("appdata")) env.appdata = *d;
    else if (auto e = env_path(L"APPDATA")) env.appdata = *e;
    if (auto d = a.args->path("localappdata")) env.localappdata = *d;
    else if (auto e = env_path(L"LOCALAPPDATA")) env.localappdata = *e;
    std::vector<std::string> notes;
    env.log = [&notes](std::string_view l) { notes.emplace_back(l); };
    Run r;
    const auto t0 = Clock::now();
    const std::vector<Candidate> found = auto_detect(r, env, p, a.verbose);
    if (a.verbose)
        for (const std::string& n : notes) std::printf("       log: %s\n", n.c_str());
    picked(r, env, p);
    if (auto g = a.args->path("golden")) golden(r, *g, env, found);
    else std::printf("golden list: not given (make it with tools/dev/golden_detect.py)\n");
    std::printf("test detect: %s, %d failure(s), %s\n", r.failures ? "FAIL" : "PASS", r.failures,
                cli::time_line(since(t0)).c_str());
    return r.failures ? cli::kExitFail : cli::kExitOk;
}

}  // namespace

HDF_ORACLE_TEST("detect", "this PC: the .wua (title_list_cache) and [AZAP01] (GamePaths) usable, the .wux encrypted; picked cases; golden list", &run);
