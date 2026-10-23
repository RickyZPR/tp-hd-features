// `hdf-convert test base`: the oracle test of module C2 (plan 3.3).
//
//   hdf-convert test base --oracle <pinned .dusk> [--hd <[AZAP01] folder>] [--wua <TP HD .wua>] [--gc <files>]
//                         [--work <build-*/ or scratch folder>] [--verbose]
//
// 1. Tables: 136 + 13 inputs, 176 kept + 30 dropped outputs with the plan's totals, set id = the embedded one.
// 2. --gc: the Pscene budget recomputed from the extraction = 267,040, and the 13 GC inputs by SHA-256.
// 3. --hd and --wua: check-inputs on each (136 by SHA-256), then the 136 files byte for byte between them.
// 4. compare of the oracle against itself = 176 identical + 30 dropped by design = 206/206.
// 5. --work: seed-cache into <work>/seed, then compare that data folder against the oracle (176 identical).
// Each step prints its wall time and the peak working set. Exit 0 pass, 1 fail, 2 usage.
#include "../cli.hpp"

#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

using namespace hdf;
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

void step_time(const char* what, Clock::time_point t0) {
    std::printf("  time %s: %s\n", what, cli::time_line(since(t0)).c_str());
}

void tables(Run& r) {
    std::printf("tables\n");
    u64 kept = 0, overlay = 0;
    std::size_t overlays = 0;
    for (const outputs::ExpectedFile& e : outputs::expected()) {
        kept += e.size;
        if (e.overlay) {
            ++overlays;
            overlay += e.size;
        }
    }
    r.check(inputs::hd_files().size() == 136 && inputs::gc_files().size() == 13, "136 HD + 13 GC inputs listed");
    r.check(outputs::expected().size() == 176 && kept == 74408567, "176 kept files, 74,408,567 B");
    r.check(overlays == 56 && overlay == 50327200, "56 overlays, 50,327,200 B");
    r.check(outputs::drop_list().size() == 30, "30 files on the drop list");
    const std::string set = outputs::compute_set_id(outputs::expected());
    r.check(set == outputs::embedded_set_id(), "set id " + set.substr(0, 16) + "... = the embedded one");
}

int check_list(Run& r, const char* what, const Source& src, std::span<const inputs::InputFile> list) {
    auto rep = inputs::check_hashes(src, list, nullptr, {});
    if (!rep) {
        r.check(false, std::string(what) + ": " + rep.error().code + " " + rep.error().detail);
        return 1;
    }
    char b[160];
    std::snprintf(b, sizeof b, "%s: %u files, %u present, %u right size, %u right hash", what, rep->total, rep->present,
                  rep->size_ok, rep->hash_ok);
    r.check(rep->ok() && rep->hash_ok == rep->total, b);
    for (const inputs::Problem& p : rep->problems) std::printf("       %s: %s\n", p.path.c_str(), p.what.c_str());
    return rep->ok() ? 0 : 1;
}

int run(const cli::OracleArgs& a) {
    if (!a.oracle) {
        std::fprintf(stderr, "hdf-convert test base: --oracle is required (cache/oracle/hd_features-oracle-2.0.0.dusk)\n");
        return cli::kExitUsage;
    }
    Run r;
    r.verbose = a.verbose;
    const auto t_all = Clock::now();
    tables(r);

    if (a.gc) {
        std::printf("gc inputs (%s)\n", cli::to_utf8(*a.gc).c_str());
        const auto t0 = Clock::now();
        const auto budget = cli::pscene_budget(*a.gc);
        r.check(budget && *budget == inputs::kPsceneBudget,
                "Pscene budget " + (budget ? std::to_string(*budget) : std::string("?")) + " = 267040");
        FolderSource gc(*a.gc);
        check_list(r, "gc", gc, inputs::gc_files());
        step_time("gc", t0);
    }

    std::unique_ptr<Source> hd_folder, hd_wua;
    if (a.hd) {
        std::printf("hd folder (%s)\n", cli::to_utf8(*a.hd).c_str());
        const auto t0 = Clock::now();
        auto s = cli::open_hd(*a.hd);
        if (!s) {
            r.check(false, "open: " + s.error().code + " " + s.error().detail);
        } else {
            std::printf("  %s\n", (*s)->describe().c_str());
            check_list(r, "hd folder", **s, inputs::hd_files());
            hd_folder = std::move(*s);
        }
        step_time("hd folder", t0);
    }
    const auto wua = a.args ? a.args->path("wua") : std::nullopt;
    if (wua) {
        std::printf("hd .wua (%s)\n", cli::to_utf8(*wua).c_str());
        const auto t0 = Clock::now();
        auto s = cli::open_hd(*wua);
        if (!s) {
            r.check(false, "open: " + s.error().code + " " + s.error().detail);
        } else {
            std::printf("  %s\n", (*s)->describe().c_str());
            check_list(r, "hd wua", **s, inputs::hd_files());
            hd_wua = std::move(*s);
        }
        step_time("hd wua", t0);
    }
    if (hd_folder && hd_wua) {
        std::printf("hd folder vs .wua\n");
        const auto t0 = Clock::now();
        u32 same = 0;
        for (const inputs::InputFile& f : inputs::hd_files()) {
            auto x = hd_folder->read(f.path);
            auto y = hd_wua->read(f.path);
            if (x && y && *x == *y) {
                ++same;
            } else {
                std::printf("       differs: %.*s\n", int(f.path.size()), f.path.data());
            }
        }
        r.check(same == inputs::kHdCount, std::to_string(same) + "/136 files byte-identical between the folder and the .wua");
        step_time("folder vs wua", t0);
    }

    {
        std::printf("compare oracle vs itself\n");
        const auto t0 = Clock::now();
        auto rep = outputs::compare(*a.oracle, *a.oracle, {});
        if (!rep) {
            r.check(false, rep.error().code + " " + rep.error().detail);
        } else {
            u32 dropped_identical = 0;
            for (const outputs::Verdict& v : rep->rows) {
                if (v.kind == outputs::Verdict::Dropped && v.detail == "made, identical") ++dropped_identical;
                if (a.verbose || (v.kind != outputs::Verdict::Identical && v.kind != outputs::Verdict::Dropped))
                    std::printf("       %d %s %s\n", int(v.kind), v.rel.c_str(), v.detail.c_str());
            }
            std::printf("  %s\n", rep->summary().c_str());
            r.check(rep->pass() && rep->identical == 176 && rep->dropped == 30 && rep->rows.size() == 206,
                    "206/206: 176 identical + 30 dropped by design");
            r.check(dropped_identical == 30, "the 30 dropped files are identical too");
        }
        step_time("compare", t0);
    }

    if (a.work) {
        std::printf("seed-cache + compare (%s)\n", cli::to_utf8(*a.work).c_str());
        const auto t0 = Clock::now();
        const std::filesystem::path data_dir = *a.work / "seed";
        auto seeded = cli::seed_cache(*a.oracle, data_dir, "2.1.0", {});
        if (!seeded) {
            r.check(false, "seed-cache: " + seeded.error().code + " " + seeded.error().detail);
        } else {
            r.check(seeded->files == 176 && seeded->bytes == 74408567,
                    "seed-cache wrote 176 files, 74,408,567 B" +
                        std::string(seeded->stamp_from_cli ? " (cache.json by the CLI)" : " (cache.json by hdf::commit)"));
            std::error_code ec;
            r.check(std::filesystem::is_regular_file(data_dir / "cache.json", ec), "cache.json exists");
            auto rep = outputs::compare(data_dir, *a.oracle, {});
            if (!rep) {
                r.check(false, "compare: " + rep.error().code + " " + rep.error().detail);
            } else {
                std::printf("  %s\n", rep->summary().c_str());
                r.check(rep->pass() && rep->identical == 176 && rep->dropped == 30 && rep->extra == 0,
                        "the seeded data folder = the oracle (176 identical, 30 not made by design)");
            }
        }
        step_time("seed", t0);
    }

    std::printf("time test base: %s\n", cli::time_line(since(t_all)).c_str());
    std::printf("test base: %s (%d failure%s)\n", r.failures == 0 ? "PASS" : "FAIL", r.failures,
                r.failures == 1 ? "" : "s");
    return r.failures == 0 ? cli::kExitOk : cli::kExitFail;
}

}  // namespace

HDF_ORACLE_TEST("base", "tables; check-inputs on the folder (--hd) and the .wua (--wua), 136 identical; compare oracle vs itself = 206/206; seed-cache (--work)", &run);
