// `hdf-convert test gen`: the oracle test of module G (plan 3.3 G, plan 4 pass criteria).
//
//   hdf-convert test gen --hd <[AZAP01] folder> --gc <GC files> --oracle <pinned .dusk> --work <build-*/ or scratch>
//                        [--wua <TP HD .wua>] [--cancels 20] [--kills 10] [--seed <n>] [part]
//
// Parts (default: all of them, in this order):
//   make         the whole protocol (prepare_files -> generate -> commit) from the folder + GC folder: the 176
//                records = the embedded list, the 56 overlays kept, Progress at the end, compare = the oracle
//                (176 identical + 30 dropped by design), cache.json valid, nothing else in the data folder
//   determinism  two more runs: the two folders are identical file by file (and equal to the first run)
//   sources      GC from memory (MemorySource of the 13 files, as the mod's DvdSource) = GC from the folder;
//                HD from the .wua (--wua) = HD from the folder; HD from the content folder = HD from the title folder
//   stamp        validate_stamp and read_stamp on the made folder, changed in memory and on disk (set, folder, list,
//                a missing file, a cut file, garbage); cleanup of leftover folders; prepare_files drops the stamp
//   cancel       --cancels (20) runs stopped at random times (uniform over 0..1.1 x a full run) in one data folder:
//                after each, cache.json is either missing or valid on a complete, byte-identical folder
//   kill         --kills (10) child processes (`test gen make-one`) killed with TerminateProcess at random times:
//                the same rule (Windows only)
//   make-one     (the child of `kill`) the protocol once into --work
// Exit 0 pass, 1 fail, 2 usage. Outputs go only to --work.
#include "../cli.hpp"

#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace hdf;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

struct Run {
    int failures = 0;
    void check(bool ok, const std::string& what) {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        std::fflush(stdout);
        if (!ok) ++failures;
    }
    void note(const std::string& what) {
        std::printf("       %s\n", what.c_str());
        std::fflush(stdout);
    }
};

std::string err_text(const Error& e) { return e.code + (e.detail.empty() ? "" : " " + e.detail); }

// "valid" / "ok", or the error code
template <class T>
std::string code_of(const Result<T>& r) {
    if (r) return "ok";
    return r.error().code;
}

// ---------------------------------------------------------------------------------------------- the protocol
struct Made {
    GenOut out;
    Progress progress;  // final values
    double seconds = 0;
};

// What the mod's worker does: a fresh files-<set8>, generate into it, commit cache.json (Correction #9). Every
// step gets the stop token (wave 4, G L2 + L3: FolderSink between its 4 MiB pieces, the retries of
// prepare_files / commit / cleanup).
Result<GenOut> protocol(const Source& hd, const Source& gc, const fs::path& data_dir, std::stop_token stop,
                        Progress& p, const std::string& kind) {
    if (stop.stop_requested()) return fail("stopped");
    auto files_dir = prepare_files(data_dir, stop);
    if (!files_dir) return std::unexpected(files_dir.error());
    FolderSink sink(*files_dir, stop);
    auto made = generate(hd, gc, sink, p, stop);
    if (!made) return std::unexpected(made.error());
    StampInfo info;
    info.mod_version = "test";
    info.hd.kind = kind;
    info.hd.location = "(test)";
    if (auto ok = commit(data_dir, *files_dir, *made, info, stop); !ok) return std::unexpected(ok.error());
    return made;
}

Result<std::unique_ptr<Source>> gc_in_memory(const fs::path& gc) {
    FolderSource folder(gc);
    auto mem = std::make_unique<MemorySource>("GC inputs in memory");
    for (const inputs::InputFile& f : inputs::gc_files()) {
        auto data = folder.read(f.path);
        if (!data) return std::unexpected(data.error());
        mem->add(f.path, std::move(*data));
    }
    return std::unique_ptr<Source>(std::move(mem));
}

// rel -> sha256 of every file under root
std::map<std::string, std::string> tree(const fs::path& root) {
    std::map<std::string, std::string> out;
    FolderSource src(root);
    auto names = src.list();
    if (!names) return out;
    for (const std::string& n : *names) {
        auto data = src.read(n);
        out[n] = data ? sha256_hex(*data) : std::string("unreadable");
    }
    return out;
}

// The state a data folder may be in after any stop or kill: no stamp, or a valid stamp on a folder whose files
// are exactly the 176 expected ones, byte for byte. Returns "" when that holds, else what is wrong.
struct StateCheck {
    bool stamp_valid = false;
    std::string problem;    // "" = the rule holds
    std::string stamp_err;  // read/validate error code when not valid
};

StateCheck check_state(const fs::path& data_dir) {
    StateCheck r;
    auto s = read_stamp(data_dir);
    if (!s) {
        r.stamp_err = s.error().code;
        if (s.error().code != "cache.missing") r.problem = "cache.json is there but unreadable: " + err_text(s.error());
        return r;
    }
    if (auto ok = validate_stamp(data_dir, *s); !ok) {
        r.stamp_err = ok.error().code;
        r.problem = "a stamp names an incomplete folder: " + err_text(ok.error());
        return r;
    }
    r.stamp_valid = true;
    const auto files = tree(data_dir / s->folder);
    if (files.size() != outputs::kKeptCount)
        r.problem = "valid stamp, but the folder holds " + std::to_string(files.size()) + " files";
    for (const outputs::ExpectedFile& e : outputs::expected()) {
        auto it = files.find(std::string(e.rel));
        if (it == files.end() || it->second != e.sha256) {
            r.problem = "valid stamp, but " + std::string(e.rel) + (it == files.end() ? " is missing" : " differs");
            break;
        }
    }
    return r;
}

// Only cache.json and files-<set8> in the data folder (no tmp-*, no .tmp, no other files-*).
bool data_dir_clean(const fs::path& data_dir, std::string* what) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator d(data_dir, ec), end; !ec && d != end; d.increment(ec))
        names.push_back(cli::to_utf8(d->path().filename()));
    std::sort(names.begin(), names.end());
    const std::vector<std::string> want = {"cache.json", folder_name(set_id())};
    if (what) {
        what->clear();
        for (const std::string& n : names) *what += (what->empty() ? "" : ", ") + n;
    }
    return names == want;
}

bool same_records(const GenOut& a, const GenOut& b) {
    if (a.files.size() != b.files.size()) return false;
    for (std::size_t i = 0; i < a.files.size(); ++i)
        if (a.files[i].rel != b.files[i].rel || a.files[i].size != b.files[i].size ||
            a.files[i].sha256 != b.files[i].sha256)
            return false;
    return true;
}

// ---------------------------------------------------------------------------------------------- parts
struct Ctx {
    const cli::OracleArgs* a = nullptr;
    Run r;
    std::unique_ptr<Source> hd, gc;
    fs::path work;
    std::optional<GenOut> first;  // the 'make' run (records only)
    double full_seconds = 0;
};

std::optional<GenOut> one_run(Ctx& c, const Source& hd, const Source& gc, const fs::path& dir, const std::string& what,
                              Progress* progress_out = nullptr) {
    Progress p;
    const auto t0 = Clock::now();
    auto made = protocol(hd, gc, dir, {}, p, what);
    const double s = since(t0);
    if (!made) {
        c.r.check(false, what + ": " + err_text(made.error()));
        return std::nullopt;
    }
    if (progress_out) {
        progress_out->stage = p.stage.load();
        progress_out->done = p.done.load();
        progress_out->total = p.total.load();
    }
    c.r.note(what + ": " + std::to_string(made->files.size()) + " files, " + cli::time_line(s));
    return std::move(*made);
}

void part_make(Ctx& c) {
    std::printf("make (HD folder, GC folder)\n");
    const fs::path dir = c.work / "make";
    Progress p;
    const auto t0 = Clock::now();
    auto made = one_run(c, *c.hd, *c.gc, dir, "make", &p);
    c.full_seconds = since(t0);
    if (!made) return;
    for (const auto& [stage, s] : made->stage_seconds) c.r.note("time " + stage + ": " + std::to_string(s) + " s");
    for (const std::string& l : made->log) c.r.note("log: " + l);

    const outputs::CompareReport chk = outputs::check(made->files);
    c.r.check(chk.pass() && chk.identical == outputs::kKeptCount && made->files.size() == outputs::kKeptCount,
              "records: " + chk.summary());
    u64 overlay_bytes = 0;
    u32 overlay_ok = 0;
    for (const auto& [rel, data] : made->overlays) {
        overlay_bytes += data.size();
        const outputs::ExpectedFile* e = outputs::find(rel);
        if (e && e->overlay && e->size == data.size() && e->sha256 == sha256_hex(data)) ++overlay_ok;
    }
    c.r.check(made->overlays.size() == outputs::kOverlayCount && overlay_ok == outputs::kOverlayCount &&
                  overlay_bytes == outputs::kOverlayBytes,
              std::to_string(made->overlays.size()) + " overlays kept in memory, " + std::to_string(overlay_bytes) +
                  " B, " + std::to_string(overlay_ok) + " equal to the embedded list");
    c.r.check(p.stage == static_cast<u32>(GenStage::Done) && p.done == p.total && p.total > 0,
              "progress: stage done, " + std::to_string(p.done) + "/" + std::to_string(p.total));
    c.r.check(std::is_sorted(made->files.begin(), made->files.end(),
                             [](const OutputRecord& x, const OutputRecord& y) { return x.rel < y.rel; }),
              "records sorted by rel");

    auto rep = outputs::compare(dir, *c.a->oracle, {});
    if (!rep) {
        c.r.check(false, "compare: " + err_text(rep.error()));
    } else {
        for (const outputs::Verdict& v : rep->rows)
            if (v.kind != outputs::Verdict::Identical && v.kind != outputs::Verdict::Dropped)
                c.r.note(v.rel + ": " + v.detail);
        c.r.check(rep->pass() && rep->identical == 176 && rep->dropped == 30 && rep->extra == 0 &&
                      rep->png_pixels == 0 && rep->rows.size() == 206,
                  "compare with the oracle: " + rep->summary());
    }
    auto s = read_stamp(dir);
    c.r.check(s && validate_stamp(dir, *s).has_value(), "cache.json reads back and is valid");
    if (s) {
        c.r.check(s->set == set_id() && s->set == outputs::embedded_set_id() && s->folder == folder_name(set_id()) &&
                      s->files.size() == 176 && s->format == kStampFormat && s->converter == kConverterVersion &&
                      s->mod == kModId && s->dusklight == kDusklightVersion && s->gc == "GZ2E01" &&
                      s->mod_version == "test" && s->made.size() == 20,
                  "cache.json fields: set " + s->set.substr(0, 8) + ", folder " + s->folder + ", " +
                      std::to_string(s->files.size()) + " files, made " + s->made);
    }
    const StateCheck st = check_state(dir);
    c.r.check(st.stamp_valid && st.problem.empty(), "the 176 files on disk = the embedded list" +
                                                       (st.problem.empty() ? std::string() : ": " + st.problem));
    std::string names;
    const bool clean = data_dir_clean(dir, &names);  // before the message is built (argument order)
    c.r.check(clean, "data folder holds only cache.json and the files folder (" + names + ")");
    c.first = std::move(*made);
}

void part_determinism(Ctx& c) {
    std::printf("determinism (two more runs, HD folder, GC folder)\n");
    auto a = one_run(c, *c.hd, *c.gc, c.work / "det1", "run 1");
    auto b = one_run(c, *c.hd, *c.gc, c.work / "det2", "run 2");
    if (!a || !b) return;
    c.r.check(same_records(*a, *b), "the two runs give the same 176 records");
    if (c.first) c.r.check(same_records(*a, *c.first), "and the same as the 'make' run");
    const auto ta = tree(c.work / "det1" / folder_name(set_id())), tb = tree(c.work / "det2" / folder_name(set_id()));
    c.r.check(ta == tb && ta.size() == outputs::kKeptCount, "the two files folders are identical: " +
                                                                std::to_string(ta.size()) + " vs " +
                                                                std::to_string(tb.size()) + " files" +
                                                                (ta == tb ? "" : " (differ)"));
    // cache.json differs only in "made" and "seconds"
    auto sa = read_stamp(c.work / "det1"), sb = read_stamp(c.work / "det2");
    c.r.check(sa && sb && sa->files == sb->files && sa->set == sb->set && sa->folder == sb->folder,
              "the two stamps list the same files");
}

void part_sources(Ctx& c) {
    std::printf("sources\n");
    if (!c.first) {
        auto a = one_run(c, *c.hd, *c.gc, c.work / "make", "reference run (folder + folder)");
        if (!a) return;
        c.first = std::move(*a);
    }
    if (auto mem = gc_in_memory(*c.a->gc)) {
        auto m = one_run(c, *c.hd, **mem, c.work / "gcmem", "GC from memory");
        c.r.check(m && same_records(*m, *c.first), "GC from memory = GC from the folder (176 records)");
        if (m) c.r.check(tree(c.work / "gcmem" / folder_name(set_id())) == tree(c.work / "make" / folder_name(set_id())),
                         "the two files folders are identical");
    } else {
        c.r.check(false, "GC in memory: " + err_text(mem.error()));
    }
    const auto wua = c.a->args ? c.a->args->path("wua") : std::nullopt;
    if (wua) {
        auto w = cli::open_hd(*wua);
        if (!w) {
            c.r.check(false, ".wua: " + err_text(w.error()));
        } else {
            c.r.note((*w)->describe());
            auto m = one_run(c, **w, *c.gc, c.work / "wua", "HD from the .wua");
            c.r.check(m && same_records(*m, *c.first), "HD from the .wua = HD from the folder (176 records)");
            if (m) c.r.check(tree(c.work / "wua" / folder_name(set_id())) == tree(c.work / "make" / folder_name(set_id())),
                             "the two files folders are identical");
        }
    } else {
        c.r.note("no --wua: the .wua case is skipped");
    }
    std::error_code ec;
    if (fs::is_directory(*c.a->hd / "content", ec)) {
        auto h = cli::open_hd(*c.a->hd / "content");
        if (h) {
            auto m = one_run(c, **h, *c.gc, c.work / "content", "HD from the content folder");
            c.r.check(m && same_records(*m, *c.first), "HD from the content folder = HD from the title folder");
        }
    }
}

void part_stamp(Ctx& c) {
    std::printf("stamp (validate, read, cleanup, prepare)\n");
    const fs::path dir = c.work / "make";
    auto s = read_stamp(dir);
    if (!s) {
        c.r.check(false, "read_stamp on the 'make' folder: " + err_text(s.error()) + " (run 'make' first)");
        return;
    }
    auto code = [&](const Stamp& x) {
        auto v = validate_stamp(dir, x);
        return v ? std::string("valid") : v.error().code;
    };
    c.r.check(code(*s) == "valid", "the made stamp is valid");
    {
        Stamp x = *s;
        x.set[0] = x.set[0] == '0' ? '1' : '0';
        c.r.check(code(x) == "cache.set", "another set id -> cache.set");
        x = *s;
        x.folder = "../make/" + s->folder;
        c.r.check(code(x) == "cache.bad", "a folder other than files-<set8> -> cache.bad");
        x = *s;
        x.files.pop_back();
        c.r.check(code(x) == "cache.bad", "175 files listed -> cache.bad");
        x = *s;
        x.files[3].second.second[5] ^= 1;
        c.r.check(code(x) == "cache.bad", "one listed hash changed -> cache.bad");
        x = *s;
        x.files[7].second.first += 1;
        c.r.check(code(x) == "cache.bad", "one listed size changed -> cache.bad");
        x = *s;
        x.format = 2;
        c.r.check(code(x) == "cache.bad", "format 2 -> cache.bad");
        x = *s;
        x.converter = kConverterVersion + 1;
        c.r.check(code(x) == "cache.bad", "another converter version -> cache.bad");
    }
    const fs::path files = dir / s->folder;
    const outputs::ExpectedFile& victim = outputs::expected()[10];
    const fs::path vp = files / cli::u8path(victim.rel), aside = c.work / "aside.bin";
    std::error_code ec;
    fs::rename(vp, aside, ec);
    c.r.check(!ec && code(*s) == "cache.incomplete", "a missing file -> cache.incomplete (" + std::string(victim.rel) + ")");
    fs::copy_file(aside, vp, ec);
    fs::resize_file(vp, victim.size - 1, ec);
    c.r.check(!ec && code(*s) == "cache.incomplete", "a file one byte short -> cache.incomplete");
    fs::remove(vp, ec);
    fs::rename(aside, vp, ec);
    c.r.check(!ec && code(*s) == "valid" && check_state(dir).problem.empty(), "restored: valid again");

    // read_stamp on garbage, cut and missing files (a scratch data folder)
    const fs::path bad = c.work / "stampbad";
    fs::remove_all(bad, ec);
    fs::create_directories(bad, ec);
    c.r.check(code_of(read_stamp(bad)) == "cache.missing", "no cache.json -> cache.missing");
    std::string text;
    {
        std::ifstream f(dir / "cache.json", std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(f), {});
    }
    auto put = [&](std::string_view t) {
        std::ofstream f(bad / "cache.json", std::ios::binary | std::ios::trunc);
        f.write(t.data(), static_cast<std::streamsize>(t.size()));
    };
    u32 cut_bad = 0, cuts = 0;
    for (std::size_t n = 0; n < text.size(); n += 97) {  // every cut of the real stamp is refused
        put(std::string_view(text).substr(0, n));
        ++cuts;
        auto x = read_stamp(bad);
        if (!x && x.error().code == "cache.bad") ++cut_bad;
    }
    c.r.check(cut_bad == cuts, std::to_string(cut_bad) + "/" + std::to_string(cuts) + " cut copies of cache.json -> cache.bad");
    put(text);
    auto again = read_stamp(bad);
    c.r.check(again && again->files == s->files && again->set == s->set && again->made == s->made,
              "a full copy reads back the same");

    // cleanup: leftover files-* / tmp-* folders go, the named folder and other files stay
    fs::create_directories(dir / "files-deadbeef" / "res", ec);
    fs::create_directories(dir / "tmp-1234", ec);
    fs::create_directories(dir / "FILES-0BADF00D", ec);
    put(text);
    { std::ofstream(dir / "files-note.txt") << "x"; }
    { std::ofstream(dir / kStampTmpFile) << "{"; }
    const u32 n = cleanup(dir, &*s);
    c.r.check(n == 3 && !fs::exists(dir / "files-deadbeef") && !fs::exists(dir / "tmp-1234") &&
                  !fs::exists(dir / "FILES-0BADF00D") && !fs::exists(dir / kStampTmpFile) &&
                  fs::exists(dir / "files-note.txt") && check_state(dir).stamp_valid,
              "cleanup removed " + std::to_string(n) + " leftover folders and the .tmp, kept the named folder");
    fs::remove(dir / "files-note.txt", ec);

    // prepare_files: the stamp naming files-<set8> goes first, then the folder is made empty
    auto prepared = prepare_files(dir);
    std::error_code ec2;
    c.r.check(prepared && !fs::exists(dir / "cache.json") && fs::is_directory(*prepared) &&
                  fs::is_empty(*prepared, ec2) && code_of(read_stamp(dir)) == "cache.missing",
              "prepare_files dropped the stamp, then emptied " + folder_name(set_id()));
    // a stamp of another set (an older mod version) is kept by prepare_files
    Stamp old = *s;
    old.set = std::string(64, 'a');
    old.folder = folder_name(old.set);
    fs::create_directories(dir / old.folder, ec);
    {
        std::ofstream f(dir / "cache.json", std::ios::binary);
        f << "{\"format\": 1, \"mod\": \"local.ricky.hdfeatures\", \"converter\": 1, \"set\": \"" << old.set
          << "\", \"folder\": \"" << old.folder << "\", \"files\": {}}";
    }
    auto prepared2 = prepare_files(dir);
    auto kept = read_stamp(dir);
    c.r.check(prepared2 && kept && kept->set == old.set && fs::exists(dir / old.folder) &&
                  code_of(validate_stamp(dir, *kept)) == "cache.set",
              "prepare_files keeps a stamp of another set (cache.set at start)");
    fs::remove_all(dir / old.folder, ec);
    fs::remove(dir / "cache.json", ec);
}

void part_cancel(Ctx& c) {
    const int n = std::stoi(c.a->args->get("cancels").value_or("20"));
    const unsigned seed = static_cast<unsigned>(std::stoul(c.a->args->get("seed").value_or("20261022")));
    std::printf("cancel (%d random stops, seed %u)\n", n, seed);
    const fs::path dir = c.work / "cancel";
    std::error_code ec;
    fs::remove_all(dir, ec);
    // a full run first: the start state has a valid stamp (a remake of the same set must drop it first)
    const auto t0 = Clock::now();
    if (!one_run(c, *c.hd, *c.gc, dir, "full run before the stops")) return;
    const double full = since(t0);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> when(0.0, 1.1 * full);
    int valid = 0, missing = 0, bad = 0, stopped = 0, finished = 0, over_50ms = 0;
    double worst_latency = 0;
    for (int i = 0; i < n; ++i) {
        const double delay = when(rng);
        std::stop_source stop;
        std::string outcome = "?";
        Progress p;
        std::atomic<bool> running{true};
        Clock::time_point t_end;
        std::jthread worker([&] {
            auto made = protocol(*c.hd, *c.gc, dir, stop.get_token(), p, "cancel");
            outcome = made ? std::string("finished") : made.error().code;
            t_end = Clock::now();
            running = false;
        });
        const auto t_start = Clock::now();
        while (running && since(t_start) < delay) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool was_running = running;
        const auto t_stop = Clock::now();
        stop.request_stop();
        worker.join();
        const double latency = was_running ? std::chrono::duration<double>(t_end - t_stop).count() : 0.0;
        worst_latency = std::max(worst_latency, latency);
        if (latency >= 0.05) ++over_50ms;
        if (outcome == "finished") ++finished;
        else if (outcome == "stopped") ++stopped;
        const StateCheck st = check_state(dir);
        if (!st.problem.empty()) ++bad;
        else if (st.stamp_valid) ++valid;
        else ++missing;
        char line[320];
        std::snprintf(line, sizeof line, "stop %2d at %6.3f s (stage %-9s %3u/%3u): %-8s -> %s, stop took %.1f ms%s%s", i + 1,
                      delay, std::string(stage_name(static_cast<GenStage>(p.stage.load()))).c_str(), p.done.load(),
                      p.total.load(), outcome.c_str(), st.stamp_valid ? "valid stamp, files complete" : "no stamp",
                      latency * 1000.0, st.problem.empty() ? "" : "  PROBLEM: ", st.problem.c_str());
        c.r.note(line);
    }
    c.r.check(bad == 0, std::to_string(n) + " stops: " + std::to_string(missing) + " left no stamp, " +
                            std::to_string(valid) + " a valid stamp on a complete folder, " + std::to_string(bad) +
                            " a stamp on a half folder");
    c.r.check(stopped > 0, std::to_string(stopped) + " runs stopped mid-way, " + std::to_string(finished) + " finished first");
    char lat[96];
    // plan 2.3: a stop check at least every 50 ms of work; the limit leaves room for the scheduler and one flush
    std::snprintf(lat, sizeof lat, "worst stop latency %.1f ms (limit 100 ms)", worst_latency * 1000.0);
    c.r.check(worst_latency < 0.1, lat);
    // wave2_G_verify L2: the 50 ms of plan 2.3, reported (the scheduler can add a few ms on a busy PC)
    c.r.note(std::to_string(over_50ms) + " of " + std::to_string(n) + " stops took 50 ms or more (plan 2.3: 50 ms)");
    // and the folder can always be made again afterwards
    auto last = one_run(c, *c.hd, *c.gc, dir, "full run after the stops");
    const StateCheck st = check_state(dir);
    std::string names;
    const bool clean = data_dir_clean(dir, &names);
    c.r.check(last && st.stamp_valid && st.problem.empty() && clean,
              "a run after the stops gives a valid, complete, clean folder (" + names + ")");
}

#ifdef _WIN32
std::wstring quote(const fs::path& p) {
    std::wstring s = p.native();
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return L"\"" + s + L"\"";
}

void part_kill(Ctx& c) {
    const int n = std::stoi(c.a->args->get("kills").value_or("10"));
    const unsigned seed = static_cast<unsigned>(std::stoul(c.a->args->get("seed").value_or("20261022"))) + 1;
    std::printf("kill (%d child processes killed at random times, seed %u)\n", n, seed);
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (len == 0 || len >= std::size(exe)) {
        c.r.check(false, "GetModuleFileNameW");
        return;
    }
    const fs::path dir = c.work / "kill";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::wstring cmd = quote(fs::path(exe)) + L" test gen make-one --hd " + quote(*c.a->hd) + L" --gc " +
                             quote(*c.a->gc) + L" --work " + quote(dir);
    auto start = [&]() -> std::optional<PROCESS_INFORMATION> {
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::wstring line = cmd;
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return std::nullopt;
        return pi;
    };
    // one child to the end: its time sets the range of the kills
    const auto t0 = Clock::now();
    auto pi0 = start();
    if (!pi0) {
        c.r.check(false, "CreateProcessW");
        return;
    }
    WaitForSingleObject(pi0->hProcess, INFINITE);
    DWORD code0 = 99;
    GetExitCodeProcess(pi0->hProcess, &code0);
    CloseHandle(pi0->hThread);
    CloseHandle(pi0->hProcess);
    const double full = since(t0);
    c.r.check(code0 == 0 && check_state(dir).stamp_valid, "a child run to the end: exit " + std::to_string(code0) +
                                                               ", " + std::to_string(full) + " s, valid stamp");
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> when(0.0, 1.1 * full);
    int valid = 0, missing = 0, bad = 0, killed = 0;
    for (int i = 0; i < n; ++i) {
        const double delay = when(rng);
        auto pi = start();
        if (!pi) {
            c.r.check(false, "CreateProcessW");
            return;
        }
        const DWORD w = WaitForSingleObject(pi->hProcess, static_cast<DWORD>(delay * 1000.0));
        bool k = false;
        if (w == WAIT_TIMEOUT) {
            k = TerminateProcess(pi->hProcess, 99) != 0;
            WaitForSingleObject(pi->hProcess, INFINITE);
        }
        if (k) ++killed;
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
        const StateCheck st = check_state(dir);
        if (!st.problem.empty()) ++bad;
        else if (st.stamp_valid) ++valid;
        else ++missing;
        char line[320];
        std::snprintf(line, sizeof line, "kill %2d at %6.3f s: %s -> %s%s%s", i + 1, delay, k ? "killed" : "ended first",
                      st.stamp_valid ? "valid stamp, files complete" : "no stamp", st.problem.empty() ? "" : "  PROBLEM: ",
                      st.problem.c_str());
        c.r.note(line);
    }
    c.r.check(bad == 0, std::to_string(n) + " kills: " + std::to_string(missing) + " left no stamp, " +
                            std::to_string(valid) + " a valid stamp on a complete folder, " + std::to_string(bad) +
                            " a stamp on a half folder");
    c.r.check(killed > 0, std::to_string(killed) + " children killed mid-way");
}
#endif

int make_one(Ctx& c) {
    Progress p;
    auto made = protocol(*c.hd, *c.gc, c.work, {}, p, "make-one");
    if (!made) {
        std::fprintf(stderr, "make-one: %s\n", err_text(made.error()).c_str());
        return cli::kExitFail;
    }
    return cli::kExitOk;
}

int run(const cli::OracleArgs& a) {
    if (!a.hd || !a.gc || !a.work || !a.args) {
        std::fprintf(stderr, "hdf-convert test gen: --hd, --gc and --work are required (and --oracle for make)\n");
        return cli::kExitUsage;
    }
    const std::string part = a.args->positional.size() > 1 ? a.args->positional[1] : std::string("all");
    static constexpr const char* kParts[] = {"all", "make", "determinism", "sources", "stamp", "cancel", "kill", "make-one"};
    if (std::find_if(std::begin(kParts), std::end(kParts), [&](const char* p) { return part == p; }) == std::end(kParts)) {
        std::fprintf(stderr, "hdf-convert test gen: unknown part '%s'\n", part.c_str());
        return cli::kExitUsage;
    }
    Ctx c;
    c.a = &a;
    c.work = *a.work;
    auto hd = cli::open_hd(*a.hd);
    if (!hd) return cli::report("test gen", hd.error());
    c.hd = std::move(*hd);
    c.gc = std::make_unique<FolderSource>(*a.gc);
    if (part == "make-one") return make_one(c);
    if ((part == "all" || part == "make") && !a.oracle) {
        std::fprintf(stderr, "hdf-convert test gen: --oracle is required for 'make'\n");
        return cli::kExitUsage;
    }
    std::printf("test gen: HD %s\n", c.hd->describe().c_str());
    const auto t_all = Clock::now();
    const bool all = part == "all";
    if (all || part == "make") part_make(c);
    if (all || part == "determinism") part_determinism(c);
    if (all || part == "sources") part_sources(c);
    if (all || part == "stamp") part_stamp(c);
    if (all || part == "cancel") part_cancel(c);
#ifdef _WIN32
    if (all || part == "kill") part_kill(c);
#endif
    std::printf("time test gen: %s\n", cli::time_line(since(t_all)).c_str());
    std::printf("test gen: %s (%d failure%s)\n", c.r.failures == 0 ? "PASS" : "FAIL", c.r.failures,
                c.r.failures == 1 ? "" : "s");
    return c.r.failures == 0 ? cli::kExitOk : cli::kExitFail;
}

}  // namespace

HDF_ORACLE_TEST("gen", "make = oracle (176 + 30 dropped); 2 runs identical; GC memory = folder; .wua = folder; stamp checks; 20 cancels; 10 kills", &run);
