// Unit tests of module G (src/gen/*): synthetic data only. The driver runs with small synthetic input and output
// lists (gen::Config), so no game file is needed:
//   - fix-wave1 L4: an unlisted, missing, wrong-size or wrong-hash input never reaches a step, also when the file
//     changes between the input pass and the step's read;
//   - fix-wave1 L9: the steps run under FE_TONEAREST, and the caller's rounding mode comes back;
//   - options, errors, progress, the emitter (PNG, overlays, drop list, duplicates);
//   - cache.json: writer/reader round trip, garbage and cut copies, validate, commit (with the replace retries),
//     prepare_files, cleanup, and random stops of the whole protocol.
#include "test.hpp"

#include "../src/gen/gen_internal.hpp"

#include "hdf/io.hpp"
#include "hdf/source.hpp"

#include <atomic>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace hdf;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("hdf-gen-" + std::to_string(rd()) + "-" + std::to_string(t));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Bytes bytes_of(std::string_view s) { return Bytes(s.begin(), s.end()); }

Bytes pattern(std::size_t n, u8 seed) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<u8>(seed + i * 7 + (i >> 3));
    return b;
}

// A synthetic input list: paths (lower-case, sorted), sizes, SHA-256 of the given contents.
struct Inputs {
    std::vector<std::string> paths, hashes;
    std::vector<inputs::InputFile> list;
    void build(const std::vector<std::pair<std::string, Bytes>>& files) {
        auto sorted = files;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        paths.clear();
        hashes.clear();
        list.clear();
        paths.reserve(sorted.size());
        hashes.reserve(sorted.size());
        for (const auto& [p, d] : sorted) {
            paths.push_back(p);
            hashes.push_back(sha256_hex(d));
        }
        for (std::size_t i = 0; i < sorted.size(); ++i)
            list.push_back(inputs::InputFile{paths[i], sorted[i].second.size(), hashes[i]});
    }
};

// A synthetic expected-output list.
struct Expected {
    std::vector<std::string> rels, hashes;
    std::vector<outputs::ExpectedFile> list;
    std::string set;
    void build(const std::vector<std::pair<std::string, Bytes>>& files) {
        auto sorted = files;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        rels.clear();
        hashes.clear();
        list.clear();
        rels.reserve(sorted.size());
        hashes.reserve(sorted.size());
        for (const auto& [r, d] : sorted) {
            rels.push_back(r);
            hashes.push_back(sha256_hex(d));
        }
        for (std::size_t i = 0; i < sorted.size(); ++i)
            list.push_back(outputs::ExpectedFile{rels[i], sorted[i].second.size(), hashes[i], rels[i].starts_with("overlay/")});
        set = outputs::compute_set_id(list);
    }
};

// Returns `good` bytes for the first `good_reads` reads of each path, then `bad` bytes (same size): a file that
// changes between the input pass and a step's read.
class FlipSource final : public Source {
public:
    FlipSource(const MemorySource& good, const MemorySource& bad, int good_reads)
        : good_(good), bad_(bad), good_reads_(good_reads) {}
    std::optional<u64> size(std::string_view path) const override { return good_.size(path); }
    Result<Bytes> read(std::string_view path, std::stop_token stop) const override {
        int n;
        {
            std::lock_guard<std::mutex> g(lock_);
            n = reads_[lower_key(path)]++;
        }
        return n < good_reads_ ? good_.read(path, stop) : bad_.read(path, stop);
    }
    std::string describe() const override { return "flip"; }

private:
    const MemorySource& good_;
    const MemorySource& bad_;
    int good_reads_;
    mutable std::mutex lock_;
    mutable std::map<std::string, int> reads_;
};

// What a spy step saw.
struct Spy {
    int calls = 0;
    std::vector<std::pair<std::string, Bytes>> got;  // every buffer a read returned
    std::vector<std::string> errors;                 // every read error code
    std::string hd_describe, gc_describe;
    int round_mode = -1;
    double nearbyint_2_5 = 0;
};

// The synthetic world: 3 HD files, 1 GC file, 2 outputs (one overlay).
struct World {
    std::vector<std::pair<std::string, Bytes>> hd_files = {
        {"los.bin", pattern(64, 1)}, {"res/a.bin", pattern(1000, 2)}, {"res/stage/b.arc", pattern(300, 3)}};
    std::vector<std::pair<std::string, Bytes>> gc_files = {{"res/msgus/bmgres.arc", pattern(128, 4)}};
    std::vector<std::pair<std::string, Bytes>> out_files = {{"overlay/res/Object/x.arc", bytes_of("overlay bytes")},
                                                           {"res/cave/y.bin", bytes_of("res bytes")}};
    Inputs hd_list, gc_list;
    Expected expected;
    MemorySource hd{"hd"}, gc{"gc"};
    World() {
        hd_list.build(hd_files);
        gc_list.build(gc_files);
        expected.build(out_files);
        for (const auto& [p, d] : hd_files) hd.add(p, d);
        for (const auto& [p, d] : gc_files) gc.add(p, d);
        hd.add("res/secret.bin", bytes_of("not on the list"));  // present in the source, unlisted
    }

    // A config whose one step reads every input (and an unlisted one), then writes the expected outputs.
    gen::Config config(Spy& spy) const {
        gen::Config c;
        c.hd_list = hd_list.list;
        c.gc_list = gc_list.list;
        c.inputs_weight = 4;
        c.check_weight = 1;
        c.skip_dropped = false;
        const auto* self = this;
        c.steps.push_back(gen::Step{GenStage::Cave, "spy", 10, 2, false,
                                    [self, &spy](const Source& h, const Source& g, gen::Emitter& out, Progress&,
                                                 std::stop_token stop) -> Result<void> {
                                        ++spy.calls;
                                        spy.hd_describe = h.describe();
                                        spy.gc_describe = g.describe();
                                        spy.round_mode = std::fegetround();
                                        volatile double x = 2.5;
                                        spy.nearbyint_2_5 = std::nearbyint(x);
                                        std::optional<Error> first;
                                        auto take = [&](const Source& s, const std::string& p) {
                                            auto d = s.read(p, stop);
                                            if (d) {
                                                spy.got.emplace_back(p, *d);
                                            } else {
                                                spy.errors.push_back(d.error().code);
                                                if (!first && d.error().code != "hd.unlisted") first = d.error();
                                            }
                                        };
                                        for (const auto& [p, d] : self->hd_files) take(h, p);
                                        for (const auto& [p, d] : self->gc_files) take(g, p);
                                        take(h, "res/secret.bin");
                                        if (first) return std::unexpected(*first);
                                        for (const auto& [rel, d] : self->out_files)
                                            if (auto ok = out.file(rel, d); !ok) return ok;
                                        return {};
                                    }});
        const auto exp = expected.list;
        c.check = [exp](std::span<const OutputRecord> made, bool partial) {
            return gen::check_against(exp, made, partial);
        };
        return c;
    }

    // Every buffer the spy got equals the listed file's bytes.
    bool spy_saw_only_good(const Spy& spy) const {
        for (const auto& [p, d] : spy.got) {
            bool ok = false;
            for (const auto& [q, e] : hd_files) ok = ok || (p == q && d == e);
            for (const auto& [q, e] : gc_files) ok = ok || (p == q && d == e);
            if (!ok) return false;
        }
        return true;
    }
};

template <class T>
std::string code_of(const Result<T>& r) {
    return r ? std::string("ok") : r.error().code;
}

}  // namespace

HDF_TEST(gen_set_id_and_names) {
    HDF_CHECK_EQ(std::string(set_id()), std::string(outputs::embedded_set_id()));
    HDF_CHECK_EQ(set_id().size(), std::size_t(64));
    HDF_CHECK_EQ(folder_name(set_id()), "files-" + std::string(set_id().substr(0, 8)));
    HDF_CHECK_EQ(folder_name("abc"), std::string("files-abc"));
    HDF_CHECK_EQ(stage_name(GenStage::Inputs), std::string_view("inputs"));
    HDF_CHECK_EQ(stage_name(GenStage::Tables), std::string_view("tables"));
    HDF_CHECK_EQ(stage_name(GenStage::Done), std::string_view("done"));
    // the production steps: build_assets.py's order, 176 files, every stage named once
    const gen::Config& c = gen::production();
    std::vector<std::string> names;
    u32 files = 0;
    for (const gen::Step& s : c.steps) {
        names.emplace_back(s.name);
        files += s.files;
        HDF_CHECK_EQ(std::string(stage_name(s.stage)), std::string(s.name));
        HDF_CHECK(s.weight > 0 && s.run);
    }
    HDF_CHECK((names == std::vector<std::string>{"cave", "tables", "stamps", "text", "amiibo", "stampbook", "confirm"}));
    HDF_CHECK_EQ(files, u32(outputs::kKeptCount));
    HDF_CHECK_EQ(c.hd_list.size(), inputs::kHdCount);
    HDF_CHECK_EQ(c.gc_list.size(), inputs::kGcCount);
}

// fix-wave1 L4: the steps only see CheckedSources, and only the listed bytes.
HDF_TEST(gen_steps_only_get_checked_inputs) {
    World w;
    Spy spy;
    const gen::Config cfg = w.config(spy);
    MemorySink sink;
    Progress p;
    auto r = gen::run(w.hd, w.gc, sink, p, {}, {}, cfg);
    HDF_CHECK_EQ(code_of(r), std::string("ok"));
    HDF_CHECK_EQ(spy.calls, 1);
    HDF_CHECK(spy.hd_describe.find("hash-checked") != std::string::npos);
    HDF_CHECK(spy.gc_describe.find("hash-checked") != std::string::npos);
    HDF_CHECK_EQ(spy.got.size(), std::size_t(4));  // 3 HD + 1 GC
    HDF_CHECK(w.spy_saw_only_good(spy));
    HDF_CHECK((spy.errors == std::vector<std::string>{"hd.unlisted"}));  // res/secret.bin never reached the step
    for (const auto& [p2, d] : spy.got) HDF_CHECK(p2 != "res/secret.bin");
    HDF_CHECK_EQ(sink.files.size(), std::size_t(2));
    if (r) {
        HDF_CHECK_EQ(r->files.size(), std::size_t(2));
        HDF_CHECK_EQ(r->overlays.size(), std::size_t(1));
    }
}

HDF_TEST(gen_bad_inputs_never_reach_a_step) {
    World w;
    struct Case {
        const char* what;
        std::string path;  // "" = no change
        int kind;          // 0 wrong bytes, 1 missing, 2 wrong size
        bool gc;
        const char* code;
    };
    const Case cases[] = {
        {"HD wrong bytes", "res/a.bin", 0, false, "hd.hash"},
        {"HD missing", "res/stage/b.arc", 1, false, "hd.missing"},
        {"HD wrong size", "los.bin", 2, false, "hd.size"},
        {"GC wrong bytes", "res/msgus/bmgres.arc", 0, true, "gc.hash"},
        {"GC missing", "res/msgus/bmgres.arc", 1, true, "gc.missing"},
    };
    for (const Case& k : cases) {
        MemorySource hd("hd"), gc("gc");
        for (const auto& [p, d] : w.hd_files)
            if (k.gc || p != k.path || k.kind != 1) hd.add(p, d);
        for (const auto& [p, d] : w.gc_files)
            if (!k.gc || p != k.path || k.kind != 1) gc.add(p, d);
        MemorySource& target = k.gc ? gc : hd;
        if (k.kind == 0) {
            auto d = target.read(k.path);
            HDF_CHECK(d.has_value());
            if (d) {
                (*d)[d->size() / 2] ^= 0x01;
                target.add(k.path, *d);
            }
        } else if (k.kind == 2) {
            auto d = target.read(k.path);
            if (d) {
                d->push_back(0);
                target.add(k.path, *d);
            }
        }
        Spy spy;
        MemorySink sink;
        Progress p;
        auto r = gen::run(hd, gc, sink, p, {}, {}, w.config(spy));
        HDF_CHECK_EQ(code_of(r), std::string(k.code));
        HDF_CHECK_EQ(spy.calls, 0);  // the step never ran
        HDF_CHECK(sink.files.empty());
        HDF_CHECK_EQ(p.stage.load(), static_cast<u32>(GenStage::Inputs));
        if (!r) HDF_CHECK(r.error().detail.find(k.path) != std::string::npos);
    }
    // all problems are counted, the first kind wins: missing before size before hash
    MemorySource hd("hd");
    hd.add("los.bin", pattern(65, 1));         // wrong size
    hd.add("res/a.bin", pattern(1000, 9));     // wrong bytes
    Progress p;
    auto r = gen::check_inputs(hd, w.hd_list.list, "hd", p, 0, 10, {});
    HDF_CHECK_EQ(code_of(r), std::string("hd.missing"));
    if (!r) HDF_CHECK(r.error().detail.find("1 of 3 files missing, 1 wrong size, 1 differ") != std::string::npos);
}

// A file that changes after the input pass: the step's read through the CheckedSource refuses it.
HDF_TEST(gen_changed_input_after_the_pass_is_refused) {
    World w;
    MemorySource bad_hd("bad");
    for (const auto& [p, d] : w.hd_files) {
        Bytes x = d;
        x[0] ^= 0xFF;
        bad_hd.add(p, x);
    }
    FlipSource flip(w.hd, bad_hd, 1);  // the input pass gets the good bytes, every later read the changed ones
    Spy spy;
    MemorySink sink;
    Progress p;
    auto r = gen::run(flip, w.gc, sink, p, {}, {}, w.config(spy));
    HDF_CHECK_EQ(code_of(r), std::string("hd.hash"));
    HDF_CHECK_EQ(spy.calls, 1);
    HDF_CHECK(w.spy_saw_only_good(spy));  // only the GC file came through
    HDF_CHECK_EQ(spy.got.size(), std::size_t(1));
    HDF_CHECK(sink.files.empty());
    if (!r) HDF_CHECK(r.error().detail.rfind("spy: ", 0) == 0);
}

// The production driver refuses empty and wrong sources at the input pass, before any converter.
HDF_TEST(gen_generate_refuses_wrong_sources) {
    MemorySource empty_hd("hd"), empty_gc("gc");
    MemorySink sink;
    Progress p;
    auto r = generate(empty_hd, empty_gc, sink, p, {});
    HDF_CHECK_EQ(code_of(r), std::string("gc.missing"));  // GC first
    if (!r) HDF_CHECK(r.error().detail.rfind("13 of 13 files missing", 0) == 0);
    HDF_CHECK(sink.files.empty());
    HDF_CHECK_EQ(p.stage.load(), static_cast<u32>(GenStage::Inputs));
    MemorySource gc("gc");
    for (const inputs::InputFile& f : inputs::gc_files()) gc.add(f.path, Bytes(16, 0));
    auto r2 = generate(empty_hd, gc, sink, p, {});
    HDF_CHECK_EQ(code_of(r2), std::string("gc.size"));
    std::stop_source s;
    s.request_stop();
    HDF_CHECK_EQ(code_of(generate(empty_hd, empty_gc, sink, p, s.get_token())), std::string("stopped"));
    GenOptions only;
    only.only = {"cave", "nope"};
    auto r3 = generate(empty_hd, empty_gc, sink, p, {}, only);
    HDF_CHECK_EQ(code_of(r3), std::string("gen.only"));
}

// fix-wave1 L9: the steps run round-to-nearest whatever the caller's mode, and the caller's mode comes back.
HDF_TEST(gen_rounding_mode_is_nearest_inside) {
    World w;
    for (int mode : {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO, FE_TONEAREST}) {
        Spy spy;
        MemorySink sink;
        Progress p;
        std::fesetround(mode);
        auto r = gen::run(w.hd, w.gc, sink, p, {}, {}, w.config(spy));
        const int after = std::fegetround();
        std::fesetround(FE_TONEAREST);
        HDF_CHECK_EQ(code_of(r), std::string("ok"));
        HDF_CHECK_EQ(spy.round_mode, FE_TONEAREST);
        HDF_CHECK_EQ(spy.nearbyint_2_5, 2.0);
        HDF_CHECK_EQ(after, mode);
    }
}

HDF_TEST(gen_options_errors_and_progress) {
    World w;
    // two more steps: one that fails, one that throws; keep_going runs them all and returns the first failure
    Spy spy;
    gen::Config cfg = w.config(spy);
    int after_calls = 0;
    std::vector<u32> seen_done;
    cfg.steps.insert(cfg.steps.begin(),
                     gen::Step{GenStage::Text, "fails", 2, 0, false,
                               [](const Source&, const Source&, gen::Emitter&, Progress&, std::stop_token) -> Result<void> {
                                   return fail("fmt.text", "synthetic");
                               }});
    cfg.steps.push_back(gen::Step{GenStage::Confirm, "throws", 3, 0, false,
                                  [](const Source&, const Source&, gen::Emitter&, Progress&, std::stop_token) -> Result<void> {
                                      throw std::runtime_error("boom");
                                  }});
    cfg.steps.push_back(gen::Step{GenStage::Stampbook, "after", 5, 0, false,
                                  [&](const Source&, const Source&, gen::Emitter&, Progress& p, std::stop_token) -> Result<void> {
                                      ++after_calls;
                                      seen_done.push_back(p.done);
                                      return {};
                                  }});
    {
        MemorySink sink;
        Progress p;
        auto r = gen::run(w.hd, w.gc, sink, p, {}, {}, cfg);
        HDF_CHECK_EQ(code_of(r), std::string("fmt.text"));
        HDF_CHECK_EQ(spy.calls, 0);  // not keep_going: stops at the first failure
        if (!r) HDF_CHECK_EQ(r.error().detail, std::string("fails: synthetic"));
    }
    {
        MemorySink sink;
        Progress p;
        GenOptions o;
        o.keep_going = true;
        auto r = gen::run(w.hd, w.gc, sink, p, {}, o, cfg);
        HDF_CHECK_EQ(code_of(r), std::string("fmt.text"));
        HDF_CHECK_EQ(spy.calls, 1);
        HDF_CHECK_EQ(after_calls, 1);
        HDF_CHECK_EQ(sink.files.size(), std::size_t(2));  // the spy's files are written
        if (!r) HDF_CHECK(r.error().detail.find("2 step(s) failed: fails, throws") != std::string::npos);
        // inputs 4 + fails 2 + spy 10 + throws 3 = 19 before "after"
        HDF_CHECK((seen_done == std::vector<u32>{19}));
    }
    {
        // --only: just the spy; partial check
        MemorySink sink;
        Progress p;
        GenOptions o;
        o.only = {"spy"};
        Spy spy2;
        gen::Config c2 = w.config(spy2);
        c2.steps.push_back(cfg.steps.front());  // "fails", not selected
        auto r = gen::run(w.hd, w.gc, sink, p, {}, o, c2);
        HDF_CHECK_EQ(code_of(r), std::string("ok"));
        HDF_CHECK_EQ(p.total.load(), u32(4 + 10 + 1));
        HDF_CHECK_EQ(p.done.load(), p.total.load());
        HDF_CHECK_EQ(p.stage.load(), static_cast<u32>(GenStage::Done));
        if (r) {
            HDF_CHECK_EQ(r->stage_seconds.size(), std::size_t(3));  // inputs, spy, check
            HDF_CHECK(!r->log.empty());
        }
    }
    {
        // a step that sees a stop gives "stopped", whatever it returns
        std::stop_source s;
        gen::Config c3 = w.config(spy);
        c3.steps.insert(c3.steps.begin(), gen::Step{GenStage::Cave, "stopper", 1, 0, false,
                                                    [&s](const Source&, const Source&, gen::Emitter&, Progress&,
                                                         std::stop_token) -> Result<void> {
                                                        s.request_stop();
                                                        return fail("io.read", "interrupted");
                                                    }});
        MemorySink sink;
        Progress p;
        GenOptions o;
        o.keep_going = true;
        HDF_CHECK_EQ(code_of(gen::run(w.hd, w.gc, sink, p, s.get_token(), o, c3)), std::string("stopped"));
    }
}

HDF_TEST(gen_emitter_png_overlays_drops_duplicates) {
    World w;
    gen::Config cfg;
    cfg.hd_list = w.hd_list.list;
    cfg.gc_list = w.gc_list.list;
    cfg.skip_dropped = true;
    const Bytes rgba = pattern(3 * 2 * 4, 5);
    std::vector<u32> progress;
    cfg.steps.push_back(gen::Step{GenStage::Stamps, "pngs", 8, 4, false,
                                  [&](const Source&, const Source&, gen::Emitter& out, Progress& p, std::stop_token) -> Result<void> {
                                      if (auto ok = out.png("res/z.png", 3, 2, rgba); !ok) return ok;
                                      progress.push_back(p.done);
                                      if (auto ok = out.file("overlay/res/a.arc", bytes_of("A")); !ok) return ok;
                                      progress.push_back(p.done);
                                      if (auto ok = out.file("res/cave/floors.json", bytes_of("{}")); !ok) return ok;  // dropped
                                      progress.push_back(p.done);
                                      return out.file("res/m.bin", bytes_of("M"));
                                  }});
    cfg.check = [](std::span<const OutputRecord>, bool) -> Result<void> { return {}; };
    for (bool keep : {true, false}) {
        MemorySink sink;
        Progress p;
        progress.clear();
        GenOptions o;
        o.keep_overlays = keep;
        auto r = gen::run(w.hd, w.gc, sink, p, {}, o, cfg);
        HDF_CHECK(r.has_value());
        if (!r) continue;
        HDF_CHECK_EQ(r->files.size(), std::size_t(3));  // floors.json is on the drop list: not written
        HDF_CHECK_EQ(sink.files.size(), std::size_t(3));
        HDF_CHECK_EQ(r->files[0].rel, std::string("overlay/res/a.arc"));  // sorted by rel
        HDF_CHECK(r->files[0].overlay && !r->files[1].overlay);
        HDF_CHECK_EQ(r->overlays.size(), keep ? std::size_t(1) : std::size_t(0));
        // the PNG = common.png_bytes of the pixels, and its record hashes those bytes
        const Bytes png = io::png_encode_rgba(3, 2, rgba);
        HDF_CHECK_EQ(r->files[2].rel, std::string("res/z.png"));
        HDF_CHECK_EQ(r->files[2].sha256, sha256_hex(png));
        auto dec = io::png_decode(sink.files[0].second);
        HDF_CHECK(dec && dec->second == rgba);
        // progress inside the step: 1 + 1 + 1 of 4 files -> 2, 4, 4 of 8 units after the inputs' 1
        HDF_CHECK((progress == std::vector<u32>{1 + 2, 1 + 4, 1 + 4}));
        HDF_CHECK(r->log.size() >= 3);
    }
    gen::Config dup = cfg;
    dup.steps[0].run = [](const Source&, const Source&, gen::Emitter& out, Progress&, std::stop_token) -> Result<void> {
        if (auto ok = out.file("res/m.bin", bytes_of("1")); !ok) return ok;
        return out.file("res/m.bin", bytes_of("2"));
    };
    MemorySink sink;
    Progress p;
    HDF_CHECK_EQ(code_of(gen::run(w.hd, w.gc, sink, p, {}, {}, dup)), std::string("out.duplicate"));
    dup.steps[0].run = [](const Source&, const Source&, gen::Emitter& out, Progress&, std::stop_token) -> Result<void> {
        return out.png("res/bad.png", 4, 4, Bytes(10));
    };
    HDF_CHECK_EQ(code_of(gen::run(w.hd, w.gc, sink, p, {}, {}, dup)), std::string("out.png"));
}

// src/gen/png.cpp gives io::png_encode_rgba's bytes (one compress2 call) while it deflates in 64 KiB steps.
HDF_TEST(gen_png_matches_io) {
    std::mt19937 rng(3);
    auto image = [&](u32 w, u32 h, int kind) {
        Bytes px(std::size_t(w) * h * 4);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                u8* p = &px[(std::size_t(y) * w + x) * 4];
                switch (kind) {
                    case 0:  // noise
                        for (int c = 0; c < 4; ++c) p[c] = static_cast<u8>(rng());
                        break;
                    case 1:  // gradients with a transparent border, like UI art
                        p[0] = static_cast<u8>(x * 255 / std::max<u32>(1, w - 1));
                        p[1] = static_cast<u8>(y * 255 / std::max<u32>(1, h - 1));
                        p[2] = static_cast<u8>((x ^ y) & 0xFF);
                        p[3] = (x < 8 || y < 8 || x + 8 >= w || y + 8 >= h) ? 0 : 255;
                        break;
                    default:  // flat blocks with a little noise (long matches, the slow case at level 9)
                        p[0] = static_cast<u8>((x / 32) * 40);
                        p[1] = static_cast<u8>((y / 16) * 30);
                        p[2] = (rng() % 50 == 0) ? static_cast<u8>(rng()) : 128;
                        p[3] = 255;
                        break;
                }
            }
        return px;
    };
    const struct {
        u32 w, h;
        int kind;
    } cases[] = {{1, 1, 0}, {7, 3, 1}, {0, 0, 1}, {64, 64, 0}, {300, 200, 1}, {300, 200, 2}, {513, 257, 0},
                 {1024, 449, 1}, {1024, 449, 2}, {231, 232, 2}};
    for (const auto& c : cases) {
        const Bytes px = image(c.w, c.h, c.kind);
        auto mine = gen::png_encode(c.w, c.h, px);
        const Bytes theirs = io::png_encode_rgba(c.w, c.h, px);
        HDF_CHECK(mine.has_value());
        if (mine) HDF_CHECK(*mine == theirs);
    }
    HDF_CHECK_EQ(code_of(gen::png_encode(4, 4, Bytes(10))), std::string("out.png"));
    std::stop_source s;
    s.request_stop();
    HDF_CHECK_EQ(code_of(gen::png_encode(2, 2, Bytes(16), s.get_token())), std::string("stopped"));
    // a stop while a big image is deflated is seen within a step (64 KiB of input)
    const Bytes big = image(2048, 1024, 2);
    std::stop_source s2;
    std::thread stopper([&s2] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        s2.request_stop();
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto r = gen::png_encode(2048, 1024, big, s2.get_token());
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    stopper.join();
    HDF_CHECK_EQ(code_of(r), std::string("stopped"));
    HDF_CHECK(secs < 0.2);
}

HDF_TEST(gen_check_against) {
    Expected e;
    e.build({{"res/a", bytes_of("a")}, {"res/b", bytes_of("bb")}, {"overlay/res/c", bytes_of("ccc")}});
    auto rec = [](std::string rel, std::string_view d) {
        return OutputRecord{rel, d.size(), sha256_hex(bytes_of(d)), rel.starts_with("overlay/")};
    };
    std::vector<OutputRecord> all = {rec("overlay/res/c", "ccc"), rec("res/a", "a"), rec("res/b", "bb")};
    HDF_CHECK(gen::check_against(e.list, all, false).has_value());
    auto diff = all;
    diff[1] = rec("res/a", "x");
    auto r = gen::check_against(e.list, diff, false);
    HDF_CHECK_EQ(code_of(r), std::string("out.check"));
    if (!r) HDF_CHECK(r.error().detail.find("DIFF res/a") != std::string::npos);
    auto missing = all;
    missing.pop_back();
    HDF_CHECK_EQ(code_of(gen::check_against(e.list, missing, false)), std::string("out.check"));
    HDF_CHECK(gen::check_against(e.list, missing, true).has_value());  // partial: fine
    auto extra = all;
    extra.push_back(rec("res/zz", "z"));
    HDF_CHECK_EQ(code_of(gen::check_against(e.list, extra, false)), std::string("out.check"));
    HDF_CHECK_EQ(code_of(gen::check_against(e.list, extra, true)), std::string("out.check"));
}

HDF_TEST(gen_stamp_json_round_trip) {
    Stamp s;
    s.format = kStampFormat;
    s.mod = std::string(kModId);
    s.mod_version = "2.1.0";
    s.converter = kConverterVersion;
    s.set = std::string(64, 'b');
    s.folder = folder_name(s.set);
    s.made = "2026-10-22T12:00:00Z";
    s.seconds = 7.4567;
    s.dusklight = std::string(kDusklightVersion);
    s.gc = "GZ2E01";
    s.hd.kind = "wua";
    // wave 4: a neutral path (a user-profile path trips the release's personal-details scan)
    s.hd.location = "D:\\Spiele\\Zoë\\Games (Wii U) & \"TP\"\\漢字 \U0001F600\\Twilight Princess HD (US).wua";
    s.hd.title = "000500001019e500";
    s.hd.version = 81;
    s.files = {{"overlay/res/Object/@bg0084.arc", {281248, std::string(64, 'c')}}, {"res/x.png", {0, ""}}};
    const std::string text = gen::stamp_json(s);
    for (char ch : text) HDF_CHECK(static_cast<unsigned char>(ch) < 0x80);  // ensure_ascii
    auto back = gen::parse_stamp(text);
    HDF_CHECK(back.has_value());
    if (!back) return;
    HDF_CHECK_EQ(back->hd.location, s.hd.location);
    HDF_CHECK(back->files == s.files);
    HDF_CHECK_EQ(back->seconds, 7.457);  // rounded to 3 decimals
    HDF_CHECK_EQ(back->set, s.set);
    HDF_CHECK_EQ(back->folder, s.folder);
    HDF_CHECK_EQ(back->made, s.made);
    HDF_CHECK_EQ(back->hd.version, 81u);
    HDF_CHECK_EQ(back->mod_version, std::string("2.1.0"));
    HDF_CHECK_EQ(gen::stamp_json(*back), gen::stamp_json([&] { Stamp t = s; t.seconds = 7.457; return t; }()));
    // an empty file list is still an object
    Stamp e = s;
    e.files.clear();
    auto eb = gen::parse_stamp(gen::stamp_json(e));
    HDF_CHECK(eb && eb->files.empty());
}

HDF_TEST(gen_stamp_parser_survives_garbage) {
    Stamp s;
    s.format = 1;
    s.mod = std::string(kModId);
    s.converter = 1;
    s.set = std::string(64, 'd');
    s.folder = folder_name(s.set);
    for (int i = 0; i < 20; ++i) s.files.push_back({"res/f" + std::to_string(i), {u64(i) * 1000, std::string(64, 'e')}});
    const std::string text = gen::stamp_json(s);
    // every cut is refused (only the final newline may go)
    const std::size_t body = text.find_last_not_of('\n') + 1;
    u32 refused = 0;
    for (std::size_t n = 0; n < body; ++n) {
        auto r = gen::parse_stamp(std::string_view(text).substr(0, n));
        if (!r && r.error().code == "cache.bad") ++refused;
    }
    HDF_CHECK_EQ(refused, u32(body));
    HDF_CHECK(gen::parse_stamp(std::string_view(text).substr(0, body)).has_value());
    // random byte changes: a value or cache.bad, never a crash
    std::mt19937 rng(7);
    u32 ok = 0, bad = 0;
    for (int i = 0; i < 3000; ++i) {
        std::string t = text;
        const int edits = 1 + int(rng() % 4);
        for (int k = 0; k < edits; ++k) {
            const std::size_t at = rng() % t.size();
            switch (rng() % 3) {
                case 0: t[at] = static_cast<char>(rng() & 0xFF); break;
                case 1: t.erase(at, 1 + rng() % 8); break;
                default: t.insert(at, 1, "{}[]\",:\\u0-9e"[rng() % 13]); break;
            }
            if (t.empty()) t = "x";
        }
        auto r = gen::parse_stamp(t);
        if (r) ++ok;
        else if (r.error().code == "cache.bad") ++bad;
    }
    HDF_CHECK_EQ(ok + bad, 3000u);  // no other error, no crash
    HDF_CHECK(bad > 1000);
    // limits and odd values
    HDF_CHECK_EQ(code_of(gen::parse_stamp(std::string(200, '[') + std::string(200, ']'))),
                 std::string("cache.bad"));
    auto with = [&](const std::string& from, const std::string& to) {
        std::string t = text;
        const auto at = t.find(from);
        if (at != std::string::npos) t.replace(at, from.size(), to);
        auto r = gen::parse_stamp(t);
        return r ? std::string("ok") : r.error().code;
    };
    HDF_CHECK_EQ(with("\"format\": 1", "\"format\": 2"), std::string("cache.bad"));
    HDF_CHECK_EQ(with("\"format\": 1", "\"format\": \"1\""), std::string("cache.bad"));
    HDF_CHECK_EQ(with("\"format\": 1", "\"format\": 1.0"), std::string("cache.bad"));
    HDF_CHECK_EQ(with("\"res/f1\": [1000,", "\"res/f0\": [1000,"), std::string("cache.bad"));  // listed twice
    HDF_CHECK_EQ(with("[2000,", "[-2000,"), std::string("cache.bad"));
    HDF_CHECK_EQ(with("[2000,", "[2000, 3,"), std::string("cache.bad"));
    HDF_CHECK_EQ(with("\"mod\": ", "\"mod\": \"\\ud800\", \"x\": "), std::string("cache.bad"));  // lone surrogate
    HDF_CHECK_EQ(with("\"mod\": ", "\"zz\": [1, 2.5e3, true, false, null, {\"a\": \"\\u00e9\\ud83d\\ude00\"}], \"mod\": "),
                 std::string("ok"));
    HDF_CHECK_EQ(code_of(gen::parse_stamp(text + " x")), std::string("cache.bad"));
    HDF_CHECK_EQ(code_of(gen::parse_stamp("[]")), std::string("cache.bad"));
    HDF_CHECK_EQ(code_of(gen::parse_stamp(std::string((std::size_t(4) << 20) + 1, ' '))),
                 std::string("cache.bad"));
}

namespace {

// The protocol on a synthetic world in a temp folder: prepare, run (writes the 2 outputs), commit.
Result<GenOut> synthetic_protocol(const World& w, const fs::path& data, std::stop_token stop, const gen::Config& cfg) {
    if (stop.stop_requested()) return fail("stopped");
    auto files = gen::prepare_files_with(data, w.expected.set, stop);
    if (!files) return std::unexpected(files.error());
    FolderSink sink(*files, stop);
    Progress p;
    auto made = gen::run(w.hd, w.gc, sink, p, stop, {}, cfg);
    if (!made) return made;
    StampInfo info;
    info.mod_version = "test";
    if (auto ok = gen::commit_with(data, *files, *made, info, w.expected.list, w.expected.set, stop); !ok)
        return std::unexpected(ok.error());
    return made;
}

std::string state_of(const World& w, const fs::path& data) {
    auto s = read_stamp(data);
    if (!s) return s.error().code;
    auto v = gen::validate_stamp_with(data, *s, w.expected.list, w.expected.set);
    return v ? std::string("valid") : v.error().code;
}

}  // namespace

HDF_TEST(gen_commit_validate_prepare_cleanup) {
    World w;
    Spy spy;
    const gen::Config cfg = w.config(spy);
    TempDir t;
    const fs::path data = t.path / "mod_data" / "local.ricky.hdfeatures";
    HDF_CHECK_EQ(state_of(w, data), std::string("cache.missing"));
    auto made = synthetic_protocol(w, data, {}, cfg);
    HDF_CHECK_EQ(code_of(made), std::string("ok"));
    if (!made) return;
    HDF_CHECK_EQ(state_of(w, data), std::string("valid"));
    const fs::path files = data / folder_name(w.expected.set);
    HDF_CHECK(fs::exists(files / "overlay" / "res" / "Object" / "x.arc"));
    HDF_CHECK(!fs::exists(data / kStampTmpFile));
    auto s = read_stamp(data);
    HDF_CHECK(s && s->dusklight == kDusklightVersion && s->made.size() == 20 && s->mod_version == "test");
    // the production validate refuses the synthetic stamp: another set
    if (s) HDF_CHECK_EQ(code_of(validate_stamp(data, *s)), std::string("cache.set"));

    // commit refuses: a record that differs, a file missing on disk, a folder that is not files-<set8>
    StampInfo info;
    GenOut changed = *made;
    changed.files[0].sha256[0] = changed.files[0].sha256[0] == '0' ? '1' : '0';
    HDF_CHECK_EQ(code_of(gen::commit_with(data, files, changed, info, w.expected.list, w.expected.set)), std::string("out.check"));
    std::error_code ec;
    fs::rename(files / "res" / "cave" / "y.bin", t.path / "y.bin", ec);
    HDF_CHECK_EQ(code_of(gen::commit_with(data, files, *made, info, w.expected.list, w.expected.set)), std::string("out.missing"));
    HDF_CHECK_EQ(state_of(w, data), std::string("cache.incomplete"));  // validate sees it too
    fs::rename(t.path / "y.bin", files / "res" / "cave" / "y.bin", ec);
    fs::resize_file(files / "res" / "cave" / "y.bin", 3, ec);
    HDF_CHECK_EQ(state_of(w, data), std::string("cache.incomplete"));
    fs::create_directories(data / "files-other", ec);
    HDF_CHECK_EQ(code_of(gen::commit_with(data, data / "files-other", *made, info, w.expected.list, w.expected.set)),
                 std::string("cache.folder"));

    // validate: list and folder changes
    if (s) {
        Stamp x = *s;
        x.folder = "..";
        HDF_CHECK_EQ(code_of(gen::validate_stamp_with(data, x, w.expected.list, w.expected.set)), std::string("cache.bad"));
        x = *s;
        x.files.pop_back();
        HDF_CHECK_EQ(code_of(gen::validate_stamp_with(data, x, w.expected.list, w.expected.set)), std::string("cache.bad"));
        x = *s;
        x.mod = "other.mod";
        HDF_CHECK_EQ(code_of(gen::validate_stamp_with(data, x, w.expected.list, w.expected.set)), std::string("cache.bad"));
    }

    // a remake: prepare drops the stamp first (it names the folder), then empties the folder
    auto prepared = gen::prepare_files_with(data, w.expected.set);
    HDF_CHECK(prepared && fs::is_empty(*prepared, ec));
    HDF_CHECK_EQ(state_of(w, data), std::string("cache.missing"));
    // a stamp that can't be read is dropped too
    { std::ofstream(data / "cache.json") << "{garbage"; }
    HDF_CHECK(gen::prepare_files_with(data, w.expected.set).has_value());
    HDF_CHECK(!fs::exists(data / "cache.json"));

    // a full run again, then cleanup of leftovers (files-other made above, a tmp-*, a stale .tmp)
    fs::create_directories(data / "tmp-abc" / "x", ec);
    { std::ofstream(data / "tmp-abc" / "x" / "f.bin") << "x"; }
    { std::ofstream(data / "files-readme.txt") << "kept: a file, not a folder"; }
    HDF_CHECK(synthetic_protocol(w, data, {}, cfg).has_value());  // commit runs cleanup
    HDF_CHECK_EQ(state_of(w, data), std::string("valid"));
    HDF_CHECK(!fs::exists(data / "files-other") && !fs::exists(data / "tmp-abc"));
    HDF_CHECK(fs::exists(data / "files-readme.txt"));
    // cleanup without a stamp removes every files-* / tmp-* folder
    fs::create_directories(data / "tmp-2", ec);
    HDF_CHECK_EQ(cleanup(data, nullptr), 2u);
    HDF_CHECK(!fs::exists(files));
    HDF_CHECK_EQ(state_of(w, data), std::string("cache.incomplete"));
}

#ifdef _WIN32
// Correction #9: the replace of cache.json is retried while another process holds the file without sharing.
HDF_TEST(gen_replace_file_retries) {
    TempDir t;
    const fs::path from = t.path / "cache.json.tmp", to = t.path / "cache.json";
    { std::ofstream(to) << "old"; }
    { std::ofstream(from) << "new"; }
    HANDLE h = CreateFileW(to.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HDF_CHECK(h != INVALID_HANDLE_VALUE);
    HDF_CHECK_EQ(code_of(gen::replace_file(from, to, 0)), std::string("io.write"));  // no retries: refused
    std::thread release([h] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CloseHandle(h);
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto r = gen::replace_file(from, to, 5000);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    release.join();
    HDF_CHECK_EQ(code_of(r), std::string("ok"));
    HDF_CHECK(s > 0.3 && s < 3.0);
    std::ifstream f(to);
    std::string text;
    f >> text;
    HDF_CHECK_EQ(text, std::string("new"));
    HDF_CHECK(!fs::exists(from));
}
#endif

#ifdef _WIN32
// Wave 4, G L3: a stop ends the retries of prepare_files, cleanup and replace_file while another process holds a
// file without sharing (antivirus, the indexer), instead of up to about 5 s each.
HDF_TEST(gen_stop_ends_the_retries) {
    TempDir t;
    const fs::path data = t.path / "data";
    const std::string set(64, 'a');
    const fs::path locked = data / folder_name(set) / "res" / "x.bin";
    fs::create_directories(locked.parent_path());
    { std::ofstream(locked) << "x"; }
    HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HDF_CHECK(h != INVALID_HANDLE_VALUE);
    const auto stop_after = [](std::stop_source& s, int ms) {
        return std::jthread([&s, ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            s.request_stop();
        });
    };
    const auto since = [](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    {
        std::stop_source s;
        s.request_stop();  // already stopped: nothing is touched
        HDF_CHECK_EQ(code_of(gen::prepare_files_with(data, set, s.get_token())), std::string("stopped"));
        HDF_CHECK(fs::exists(locked));
    }
    {
        std::stop_source s;
        auto stopper = stop_after(s, 300);
        const auto t0 = std::chrono::steady_clock::now();
        auto r = gen::prepare_files_with(data, set, s.get_token());  // the old files-<set8> can't be removed
        const double secs = since(t0);
        stopper.join();
        HDF_CHECK_EQ(code_of(r), std::string("stopped"));
        HDF_CHECK(secs > 0.2 && secs < 1.5);
    }
    {
        std::stop_source s;
        auto stopper = stop_after(s, 300);
        const auto t0 = std::chrono::steady_clock::now();
        const u32 n = cleanup(data, nullptr, s.get_token());  // the same folder is a leftover: 1 s of retries
        const double secs = since(t0);
        stopper.join();
        HDF_CHECK_EQ(n, 0u);
        HDF_CHECK(secs > 0.2 && secs < 0.9);
    }
    {
        const fs::path from = t.path / "c.tmp", to = t.path / "c.json";
        { std::ofstream(to) << "old"; }
        { std::ofstream(from) << "new"; }
        HANDLE h2 = CreateFileW(to.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HDF_CHECK(h2 != INVALID_HANDLE_VALUE);
        std::stop_source s;
        auto stopper = stop_after(s, 300);
        const auto t0 = std::chrono::steady_clock::now();
        auto r = gen::replace_file(from, to, 5000, s.get_token());
        const double secs = since(t0);
        stopper.join();
        CloseHandle(h2);
        HDF_CHECK_EQ(code_of(r), std::string("stopped"));
        HDF_CHECK(secs > 0.2 && secs < 1.5);
        HDF_CHECK(fs::exists(from));  // nothing replaced
    }
    CloseHandle(h);
    // once the file is free, the same calls work
    HDF_CHECK_EQ(cleanup(data, nullptr, {}), 1u);
    HDF_CHECK(gen::prepare_files_with(data, set, {}).has_value());
}
#endif

// Wave 4, G L2: FolderSink with a stop token checks it before a file and between its 4 MiB pieces; a stopped
// write deletes its partial file.
HDF_TEST(gen_folder_sink_stops_between_pieces) {
    TempDir t;
    const Bytes small = bytes_of("small");
    {
        std::stop_source s;
        s.request_stop();
        FolderSink sink(t.path, s.get_token());
        HDF_CHECK_EQ(code_of(sink.write("a/b.bin", small)), std::string("stopped"));
        HDF_CHECK(!fs::exists(t.path / "a" / "b.bin"));
        FolderSink plain(t.path);  // no token: as before
        HDF_CHECK_EQ(code_of(plain.write("a/b.bin", small)), std::string("ok"));
        HDF_CHECK(fs::exists(t.path / "a" / "b.bin"));
    }
    // a stop that comes while a 48 MiB file (12 pieces) is written: the watcher stops as soon as the file exists
    const Bytes big(std::size_t(48) << 20, 0x5A);
    int stopped = 0, finished = 0;
    for (int i = 0; i < 5 && stopped == 0; ++i) {
        std::stop_source s;
        const std::string name = "big" + std::to_string(i) + ".bin";
        const fs::path f = t.path / name;
        std::atomic<bool> done{false};
        std::jthread watcher([&] {
            while (!done) {
                std::error_code ec;
                if (fs::exists(f, ec)) {
                    s.request_stop();
                    return;
                }
                std::this_thread::yield();
            }
        });
        FolderSink sink(t.path, s.get_token());
        auto r = sink.write(name, big);
        done = true;
        watcher.join();
        std::error_code ec;
        if (code_of(r) == "stopped") {
            ++stopped;
            HDF_CHECK(!fs::exists(f, ec));
        } else {
            ++finished;
            HDF_CHECK_EQ(code_of(r), std::string("ok"));
            HDF_CHECK_EQ(fs::file_size(f, ec), u64(big.size()));
        }
    }
    HDF_CHECK(stopped > 0);
}

// Wave 4, G L4: a listed input with the right size that can't be read is "<kind>.read" (a disk error, an
// online-only file), with the source's own error in the detail; a damaged .wua block keeps "wua.*".
HDF_TEST(gen_unreadable_input_is_kind_read) {
    class FailRead final : public Source {
    public:
        FailRead(const Source& inner, std::string path, Error e) : inner_(inner), path_(std::move(path)), e_(std::move(e)) {}
        std::optional<u64> size(std::string_view p) const override { return inner_.size(p); }
        Result<Bytes> read(std::string_view p, std::stop_token stop) const override {
            if (lower_key(p) == path_) return std::unexpected(e_);
            return inner_.read(p, stop);
        }
        std::string describe() const override { return "fail-read"; }

    private:
        const Source& inner_;
        std::string path_;
        Error e_;
    };
    World w;
    struct Case {
        const char* code;
        bool gc;
        const char* want;
    };
    const Case cases[] = {{"io.read", false, "hd.read"},     {"io.open", false, "hd.read"},
                          {"io.too_big", false, "hd.read"},  {"wua.damaged", false, "wua.damaged"},
                          {"io.read", true, "gc.read"}};
    for (const Case& k : cases) {
        const std::string path = k.gc ? "res/msgus/bmgres.arc" : "res/a.bin";
        const FailRead bad(k.gc ? static_cast<const Source&>(w.gc) : static_cast<const Source&>(w.hd), path,
                           Error{k.code, "synthetic detail"});
        Spy spy;
        MemorySink sink;
        Progress p;
        auto r = k.gc ? gen::run(w.hd, bad, sink, p, {}, {}, w.config(spy)) : gen::run(bad, w.gc, sink, p, {}, {}, w.config(spy));
        HDF_CHECK_EQ(code_of(r), std::string(k.want));
        HDF_CHECK_EQ(spy.calls, 0);
        if (!r) {
            HDF_CHECK(r.error().detail.find("1 unreadable; first: " + path + " (" + k.code + ")") != std::string::npos);
            HDF_CHECK(r.error().detail.find(std::string("; read error: ") + k.code + ": synthetic detail") !=
                      std::string::npos);
        }
    }
    // a missing file still comes first; "io.missing" from a read counts as missing
    MemorySource hd("hd");
    for (const auto& [p2, d] : w.hd_files)
        if (p2 != "los.bin") hd.add(p2, d);
    const FailRead bad(hd, "res/a.bin", Error{"io.read", "x"});
    Progress p;
    auto r = gen::check_inputs(bad, w.hd_list.list, "hd", p, 0, 10, {});
    HDF_CHECK_EQ(code_of(r), std::string("hd.missing"));
    const FailRead gone(w.hd, "res/a.bin", Error{"io.missing", "x"});
    HDF_CHECK_EQ(code_of(gen::check_inputs(gone, w.hd_list.list, "hd", p, 0, 10, {})), std::string("hd.missing"));
}

// Wave 4, G L1: commit reads its own stamp text back first. The writer passes a lone surrogate (WTF-8) through
// as \udXXX, which the reader refuses; such a location is dropped, so the next start never refuses the stamp.
HDF_TEST(gen_commit_drops_a_location_the_reader_refuses) {
    World w;
    Spy spy;
    const gen::Config cfg = w.config(spy);
    TempDir t;
    const fs::path data = t.path / "data";
    auto files = gen::prepare_files_with(data, w.expected.set);
    HDF_CHECK(files.has_value());
    if (!files) return;
    FolderSink sink(*files);
    Progress p;
    auto made = gen::run(w.hd, w.gc, sink, p, {}, {}, cfg);
    HDF_CHECK(made.has_value());
    if (!made) return;
    StampInfo info;
    info.mod_version = "test";
    info.hd.kind = "folder";
    info.hd.title = "000500001019e500";
    info.hd.location = "C:/Games/TP \xED\xA0\x80 HD";  // a lone high surrogate, as WTF-8
    {
        Stamp s;
        s.format = kStampFormat;
        s.mod = std::string(kModId);
        s.converter = kConverterVersion;
        s.hd = info.hd;
        HDF_CHECK_EQ(code_of(gen::parse_stamp(gen::stamp_json(s))), std::string("cache.bad"));  // the L1 case
    }
    HDF_CHECK_EQ(code_of(gen::commit_with(data, *files, *made, info, w.expected.list, w.expected.set)), std::string("ok"));
    auto back = read_stamp(data);
    HDF_CHECK(back.has_value());
    if (back) {
        HDF_CHECK_EQ(back->hd.location, std::string());
        HDF_CHECK_EQ(back->hd.kind, std::string("folder"));
        HDF_CHECK_EQ(back->hd.title, std::string("000500001019e500"));
        HDF_CHECK_EQ(code_of(gen::validate_stamp_with(data, *back, w.expected.list, w.expected.set)), std::string("ok"));
    }
    // a normal non-ASCII location is kept as it is
    info.hd.location = "D:/Spiele/Zelda \xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC/TP.wua";
    HDF_CHECK_EQ(code_of(gen::commit_with(data, *files, *made, info, w.expected.list, w.expected.set)), std::string("ok"));
    back = read_stamp(data);
    HDF_CHECK(back.has_value());
    if (back) HDF_CHECK_EQ(back->hd.location, info.hd.location);
}

// Random stops of the whole protocol on the synthetic world: the stamp is missing or valid, never on a half
// folder (the CI-safe twin of `hdf-convert test gen cancel`).
HDF_TEST(gen_random_stops_never_leave_a_stamp_on_a_half_folder) {
    World w;
    Spy spy;
    gen::Config cfg = w.config(spy);
    // a slower step that writes 16 more files, so stops land inside the writes too
    std::vector<std::pair<std::string, Bytes>> more = w.out_files;
    for (int i = 0; i < 16; ++i) more.emplace_back("res/many/f" + std::to_string(i) + ".bin", pattern(2000 + i, u8(i)));
    w.expected.build(more);
    cfg.steps.push_back(gen::Step{GenStage::Stamps, "many", 20, 16, false,
                                  [](const Source&, const Source&, gen::Emitter& out, Progress&, std::stop_token) -> Result<void> {
                                      for (int i = 0; i < 16; ++i) {
                                          std::this_thread::sleep_for(std::chrono::microseconds(300));
                                          if (auto ok = out.file("res/many/f" + std::to_string(i) + ".bin", pattern(2000 + i, u8(i))); !ok)
                                              return ok;
                                      }
                                      return {};
                                  }});
    const auto exp = w.expected.list;
    cfg.check = [exp](std::span<const OutputRecord> made, bool partial) { return gen::check_against(exp, made, partial); };
    TempDir t;
    const fs::path data = t.path / "data";
    const auto t0 = std::chrono::steady_clock::now();
    HDF_CHECK(synthetic_protocol(w, data, {}, cfg).has_value());
    const double full = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> when(0.0, 1.2 * full);
    int valid = 0, missing = 0, other = 0, stopped = 0;
    for (int i = 0; i < 20; ++i) {
        std::stop_source stop;
        std::atomic<bool> done{false};
        std::string outcome;
        std::jthread worker([&] {
            auto r = synthetic_protocol(w, data, stop.get_token(), cfg);
            outcome = r ? std::string("ok") : r.error().code;
            done = true;
        });
        const auto ts = std::chrono::steady_clock::now();
        const double d = when(rng);
        while (!done && std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count() < d)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        stop.request_stop();
        worker.join();
        if (outcome == "stopped") ++stopped;
        const std::string st = state_of(w, data);
        if (st == "valid") ++valid;
        else if (st == "cache.missing") ++missing;
        else ++other;
        HDF_CHECK(outcome == "ok" || outcome == "stopped");
    }
    HDF_CHECK_EQ(other, 0);
    HDF_CHECK_EQ(valid + missing, 20);
    HDF_CHECK(stopped > 0);
}
