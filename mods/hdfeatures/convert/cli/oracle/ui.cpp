// `hdf-convert test ui`: the oracle test of module U (plan 3.3).
//
//   hdf-convert test ui --oracle <pinned .dusk> --hd <TP HD title/content folder or .wua> [--golden <ui.tsv>]
//                       [--verbose]
//
// 1. ui::build_stamps / build_stampbook / build_confirm / build_amiibo with the default options (as module G
//    calls them) on the hash-checked HD inputs (CheckedSource): stampResR_gc.arc, cosconfirm_gc.arc, the 4 loose
//    BTIs, the 50 icons and layout.json byte-identical to the pinned oracle and to the embedded expected list;
//    the 58 kept PNGs (io::png_encode_rgba of the returned RGBA) byte-identical (a pixel-only difference is
//    accepted and reported, plan 4). Counts: 58 PNGs, 50 icons, 4 loose BTIs, 2 archives, 1 layout.json.
// 2. build_stamps and build_amiibo with Options::dropped: the same kept outputs, and the 20 dropped PNGs
//    (stamp_background.png + 19 amiibo PNGs) identical to the oracle's copies (extra check; all on the drop list).
// 3. Golden list (tools/dev/golden_ui.py, default <exe folder>/golden/ui.tsv): every made file and PNG (kept and
//    dropped) as the Python spec makes it from this PC's dump: size + SHA-256 only.
// 4. A stop request before each build gives Error{"stopped"}.
// Exit 0 pass, 1 fail, 2 usage, 3 skipped (no --hd, or no golden list).
#include "../cli.hpp"

#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"
#include "hdf/ui.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
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
    void quiet(bool ok, const std::string& what) {  // printed only when it fails or with --verbose
        if (!ok || verbose) std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
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

std::string join(const std::vector<std::string>& r) {
    std::string s;
    for (std::size_t i = 0; i < r.size(); ++i) s += (i ? "\t" : "") + r[i];
    return s;
}

bool starts(const std::string& s, std::string_view p) { return s.rfind(p, 0) == 0; }

// A made file in its final bytes (PNGs encoded as the driver will).
struct Made {
    std::string rel;
    Bytes bytes;
    bool png = false;
    u32 w = 0, h = 0;
    std::string pixel_hash;  // PNGs only
};

void add(std::vector<Made>& out, const ui::UiOut& u, bool dropped) {
    if (!dropped)  // the dropped run returns the same files again: only its dropped PNGs are new
        for (const auto& [rel, b] : u.files) out.push_back(Made{rel, b, false, 0, 0, {}});
    for (const ui::Png& p : dropped ? u.dropped : u.pngs)
        out.push_back(Made{p.rel, io::png_encode_rgba(p.w, p.h, p.rgba), true, p.w, p.h,
                           io::png_pixel_hash(p.w, p.h, p.rgba)});
}

struct Tally {
    int identical = 0, pixels = 0, diff = 0;
};

// Compares one made file with the oracle (and the embedded list unless it is a dropped output).
void compare(Run& r, Tally& t, const Source& oracle, const Made& m, bool expect_dropped) {
    auto want = oracle.read(m.rel);
    const bool same = want && *want == m.bytes;
    bool pixels = false;
    std::size_t first = 0;
    if (want && !same) {
        while (first < m.bytes.size() && first < want->size() && m.bytes[first] == (*want)[first]) ++first;
        if (m.png) pixels = io::png_pixel_hash(*want) == m.pixel_hash && !m.pixel_hash.empty();
    }
    const bool dropped = outputs::is_dropped(m.rel);
    const outputs::ExpectedFile* e = outputs::find(m.rel);
    const bool listed = expect_dropped ? dropped
                                       : (!dropped && e && (pixels || (e->size == m.bytes.size() &&
                                                                       e->sha256 == sha256_hex(m.bytes))));
    char b[384];
    if (same)
        std::snprintf(b, sizeof b, "%s: %zu B identical to the oracle%s", m.rel.c_str(), m.bytes.size(),
                      expect_dropped ? " (dropped output, extra check)" : ", = embedded sha256");
    else if (pixels)
        std::snprintf(b, sizeof b, "%s: PNG pixels identical, deflate bytes differ (%zu B vs oracle %zu B)",
                      m.rel.c_str(), m.bytes.size(), want->size());
    else
        std::snprintf(b, sizeof b, "%s: DIFF (%zu B vs oracle %zu B, first difference at %zu)", m.rel.c_str(),
                      m.bytes.size(), want ? want->size() : 0, first);
    if (same)
        ++t.identical;
    else if (pixels)
        ++t.pixels;
    else
        ++t.diff;
    if (same && listed)
        r.quiet(true, b);
    else
        r.check((same || pixels) && listed, std::string(b) + (listed ? "" : " [not on the expected/drop list]"));
}

int count_prefix(const std::vector<Made>& v, std::string_view prefix, std::string_view suffix) {
    return static_cast<int>(std::count_if(v.begin(), v.end(), [&](const Made& m) {
        return starts(m.rel, prefix) && m.rel.size() >= suffix.size() &&
               m.rel.compare(m.rel.size() - suffix.size(), suffix.size(), suffix) == 0;
    }));
}

int run(const cli::OracleArgs& a) {
    if (!a.oracle) {
        std::fprintf(stderr, "hdf-convert test ui: --oracle is required (cache/oracle/hd_features-oracle-2.0.0.dusk)\n");
        return cli::kExitUsage;
    }
    if (!a.hd) {
        std::printf("hdf-convert test ui: skipped: needs --hd\n");
        return cli::kExitTodo;
    }
    Run r;
    r.verbose = a.verbose;
    const auto t_all = Clock::now();

    // --- oracle (pinned)
    std::printf("oracle (%s)\n", cli::to_utf8(*a.oracle).c_str());
    auto pin = cli::file_sha256(*a.oracle);
    if (!pin || pin->first != outputs::kOracleSize || pin->second != outputs::kOracleSha256) {
        std::fprintf(stderr, "hdf-convert test ui: the oracle is not the pinned file (sha256 %.*s)\n",
                     int(outputs::kOracleSha256.size()), outputs::kOracleSha256.data());
        return cli::kExitUsage;
    }
    auto oracle = ZipSource::open(*a.oracle);
    if (!oracle) return cli::report("test ui", oracle.error());

    // --- inputs
    auto hd_raw = cli::open_hd(*a.hd);
    if (!hd_raw) return cli::report("test ui", hd_raw.error());
    inputs::CheckedSource hd(**hd_raw, inputs::hd_files(), "hd");
    std::printf("inputs: %s\n", (*hd_raw)->describe().c_str());

    // --- 1. the four builds with the default options
    std::printf("1. build (default options, as the driver)\n");
    using BuildFn = Result<ui::UiOut> (*)(const Source&, std::stop_token);
    const std::pair<const char*, BuildFn> builds[] = {{"build_stamps", &ui::build_stamps},
                                                      {"build_stampbook", &ui::build_stampbook},
                                                      {"build_confirm", &ui::build_confirm},
                                                      {"build_amiibo", &ui::build_amiibo}};
    std::vector<Made> made;
    std::vector<ui::UiOut> outs;
    double t_builds = 0;
    for (const auto& [name, fn] : builds) {
        const auto t0 = Clock::now();
        auto o = fn(hd, {});
        const double dt = since(t0);
        t_builds += dt;
        if (!o) {
            r.check(false, std::string(name) + ": " + o.error().code + " " + o.error().detail);
            return cli::kExitFail;
        }
        std::printf("  time %-16s %s (%zu files, %zu PNGs)\n", name, cli::time_line(dt).c_str(), o->files.size(),
                    o->pngs.size());
        r.check(o->dropped.empty(), std::string(name) + ": no dropped PNGs without Options::dropped");
        add(made, *o, false);
        outs.push_back(std::move(*o));
    }
    std::printf("  time all four: %.3f s\n", t_builds);
    Tally t1;
    for (const Made& m : made) compare(r, t1, **oracle, m, false);
    std::printf("  kept outputs: %d identical, %d PNG pixels only, %d differ (of %zu)\n", t1.identical, t1.pixels,
                t1.diff, made.size());
    {
        const int pngs = static_cast<int>(std::count_if(made.begin(), made.end(), [](const Made& m) { return m.png; }));
        const int icons = count_prefix(made, "res/stamps/stamp_icons/", ".bti");
        const int loose = count_prefix(made, "res/stamps/stampbook/", ".bti");
        const int arcs = count_prefix(made, "res/", "_gc.arc");
        const int json = count_prefix(made, "res/cave/amiibo/layout.json", "");
        char b[200];
        std::snprintf(b, sizeof b, "counts: %d PNGs (58), %d icons (50), %d loose BTIs (4), %d archives (2), "
                                   "%d layout.json (1), %zu files in all (115)", pngs, icons, loose, arcs, json,
                      made.size());
        r.check(pngs == 58 && icons == 50 && loose == 4 && arcs == 2 && json == 1 && made.size() == 115, b);
    }

    // --- 2. Options::dropped
    std::printf("2. build_stamps + build_amiibo with Options::dropped\n");
    std::vector<Made> dropped_made;
    {
        ui::Options opt;
        opt.dropped = true;
        const auto t0 = Clock::now();
        auto s = ui::build_stamps(hd, {}, opt);
        auto am = s ? ui::build_amiibo(hd, {}, opt) : Result<ui::UiOut>(fail("skipped"));
        const double dt = since(t0);
        if (!s || !am) {
            const Error& e = !s ? s.error() : am.error();
            r.check(false, "dropped builds: " + e.code + " " + e.detail);
        } else {
            std::printf("  time both: %s\n", cli::time_line(dt).c_str());
            auto same_kept = [](const ui::UiOut& x, const ui::UiOut& y) {
                if (x.files != y.files || x.pngs.size() != y.pngs.size()) return false;
                for (std::size_t i = 0; i < x.pngs.size(); ++i)
                    if (x.pngs[i].rel != y.pngs[i].rel || x.pngs[i].w != y.pngs[i].w || x.pngs[i].h != y.pngs[i].h ||
                        x.pngs[i].rgba != y.pngs[i].rgba)
                        return false;
                return true;
            };
            r.check(same_kept(*s, outs[0]) && same_kept(*am, outs[3]),
                    "kept outputs (files + PNG RGBA) identical with and without Options::dropped");
            add(dropped_made, *s, true);
            add(dropped_made, *am, true);
            Tally t2;
            for (const Made& m : dropped_made) compare(r, t2, **oracle, m, true);
            char b[200];
            std::snprintf(b, sizeof b, "dropped PNGs: %d identical, %d PNG pixels only, %d differ (of %zu; 20 expected)",
                          t2.identical, t2.pixels, t2.diff, dropped_made.size());
            r.check(t2.diff == 0 && dropped_made.size() == 20, b);
        }
    }

    // --- 3. golden list
    fs::path golden_path;
    if (auto gp = a.args ? a.args->path("golden") : std::nullopt) {
        golden_path = *gp;
    } else {
#ifdef _WIN32
        wchar_t exe[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        golden_path = fs::path(std::wstring(exe, n)).parent_path() / "golden" / "ui.tsv";
#endif
    }
    bool golden_ran = false;
    std::ifstream gf(golden_path, std::ios::binary);
    if (!gf) {
        std::printf("3. golden list: skipped (%s not found); make it with\n"
                    "    python -I -B tools/dev/golden_ui.py --hd <..> --out <build-conv*/golden/ui.tsv>\n",
                    cli::to_utf8(golden_path).c_str());
    } else {
        golden_ran = true;
        std::printf("3. golden list (%s)\n", cli::to_utf8(golden_path).c_str());
        std::vector<std::vector<std::string>> want, got;
        std::string line;
        while (std::getline(gf, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            want.push_back(split_tabs(line));
        }
        for (const std::vector<Made>* v : {&made, &dropped_made})
            for (const Made& m : *v)
                got.push_back({m.png ? "png" : "file", m.rel, std::to_string(m.bytes.size()), sha256_hex(m.bytes)});
        std::sort(want.begin(), want.end());
        std::sort(got.begin(), got.end());
        int differ = 0, shown = 0;
        const std::size_t n = std::max(want.size(), got.size());
        for (std::size_t i = 0; i < n; ++i) {
            const bool eq = i < want.size() && i < got.size() && want[i] == got[i];
            if (!eq) {
                ++differ;
                if (shown++ < 12) {
                    std::printf("       row %zu want: %s\n", i + 1, i < want.size() ? join(want[i]).c_str() : "-");
                    std::printf("       row %zu got:  %s\n", i + 1, i < got.size() ? join(got[i]).c_str() : "-");
                }
            }
        }
        char b[160];
        std::snprintf(b, sizeof b, "golden: %zu rows (135 expected), %d differ", want.size(), differ);
        r.check(differ == 0 && want.size() == got.size() && want.size() == 135, b);
    }

    // --- 4. stop
    std::printf("4. stop\n");
    {
        std::stop_source ss;
        ss.request_stop();
        for (const auto& [name, fn] : builds) {
            auto s = fn(hd, ss.get_token());
            r.check(!s && s.error().code == "stopped", std::string(name) + ": a stop request gives Error{\"stopped\"}");
        }
    }

    std::printf("test ui: %s, %d failure(s); %s\n", r.failures ? "FAIL" : "PASS", r.failures,
                cli::time_line(since(t_all)).c_str());
    if (r.failures) return cli::kExitFail;
    return golden_ran ? cli::kExitOk : cli::kExitTodo;
}

}  // namespace

HDF_ORACLE_TEST("ui", "stampResR_gc.arc, cosconfirm_gc.arc, 4 loose BTIs, 50 icons, layout.json, the 58 kept PNGs", &run);
