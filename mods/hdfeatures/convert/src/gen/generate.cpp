// Module G: the driver (tools/build_assets.py main(): the build part, in its order; its CLI, install, .gitignore,
// validation table and summary are not ported, plan 3.3). Every input is hashed in full before any step runs
// (Correction #1a), and the steps only see inputs::CheckedSource (fix-wave1 L4). FE_TONEAREST is set at entry
// (fix-wave1 L9). Outputs go through the caller's Sink (FolderSink flushes each file, fix-wave1 L5).
#include "gen_internal.hpp"

#include "hdf/cave.hpp"
#include "hdf/io.hpp"
#include "hdf/source.hpp"
#include "hdf/stage.hpp"
#include "hdf/text.hpp"
#include "hdf/ui.hpp"

#include <algorithm>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <utility>

namespace hdf {

std::string_view stage_name(GenStage s) {
    switch (s) {
        case GenStage::Idle: return "idle";
        case GenStage::Inputs: return "inputs";
        case GenStage::Cave: return "cave";
        case GenStage::Stamps: return "stamps";
        case GenStage::Text: return "text";
        case GenStage::Amiibo: return "amiibo";
        case GenStage::Stampbook: return "stampbook";
        case GenStage::Confirm: return "confirm";
        case GenStage::Tables: return "tables";
        case GenStage::Check: return "check";
        case GenStage::Done: return "done";
    }
    return "?";
}

std::string_view set_id() {
    // plan 2.4 with Correction #17: SHA-256 over the sorted "rel \t size \t sha256 \n" lines of the embedded list
    static const std::string id = outputs::compute_set_id(outputs::expected());
    return id;
}

std::string folder_name(std::string_view set) { return "files-" + std::string(set.substr(0, 8)); }

namespace gen {
namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

std::string fmt(const char* f, auto... args) {
    char b[512];
    std::snprintf(b, sizeof b, f, args...);
    return b;
}

// fix-wave1 L9: std::nearbyint and all float math follow the thread's rounding mode. Set round-to-nearest for
// the whole run and give the caller its own mode back.
class RoundingGuard {
public:
    RoundingGuard() : old_(std::fegetround()) { std::fesetround(FE_TONEAREST); }
    ~RoundingGuard() {
        if (old_ >= 0) std::fesetround(old_);
    }
    RoundingGuard(const RoundingGuard&) = delete;
    RoundingGuard& operator=(const RoundingGuard&) = delete;

private:
    int old_;
};

// The driver side of Emitter: hash, write, record, keep the overlays, advance the progress.
class DriverEmitter final : public Emitter {
public:
    DriverEmitter(Sink& sink, GenOut& res, Progress& p, std::stop_token stop, const Config& cfg, bool keep_overlays)
        : sink_(sink), res_(res), p_(p), stop_(std::move(stop)), cfg_(cfg), keep_overlays_(keep_overlays) {}

    void begin(const Step& s, u32 base) {
        step_ = &s;
        base_ = base;
        files_ = 0;
        bytes_ = 0;
    }
    u32 files() const { return files_; }
    u64 bytes() const { return bytes_; }
    u32 dropped() const { return dropped_; }

    Result<void> file(std::string rel, Bytes data) override {
        if (stop_.stop_requested()) return fail("stopped");
        if (cfg_.skip_dropped && outputs::is_dropped(rel)) {  // on the drop list (plan 4): not written
            ++dropped_;
            return {};
        }
        if (!seen_.insert(rel).second) return fail("out.duplicate", rel);
        OutputRecord r{rel, data.size(), sha256_hex(data), rel.starts_with("overlay/")};
        if (auto ok = sink_.write(rel, data); !ok) return ok;
        ++files_;
        bytes_ += r.size;
        if (r.overlay && keep_overlays_) res_.overlays.emplace_back(rel, std::move(data));
        res_.files.push_back(std::move(r));
        if (step_ && !step_->self_progress && step_->files > 0) {
            const u64 part = u64(step_->weight) * std::min<u64>(files_, step_->files) / step_->files;
            p_.done = base_ + static_cast<u32>(part);
        }
        return {};
    }

    Result<void> png(std::string rel, u32 w, u32 h, View rgba) override {
        if (stop_.stop_requested()) return fail("stopped");
        if (u64(w) * h * 4 != rgba.size()) return fail("out.png", rel + ": RGBA size differs from " + fmt("%ux%u", w, h));
        // common.png_bytes = io::png_encode_rgba's bytes, with stop checks inside the deflate (src/gen/png.cpp)
        auto b = png_encode(w, h, rgba, stop_);
        if (!b) {
            if (b.error().code == "stopped") return std::unexpected(b.error());
            return fail(b.error().code, rel + ": " + b.error().detail);
        }
        return file(std::move(rel), std::move(*b));
    }

    void log(std::string line) override { res_.log.push_back(std::move(line)); }

private:
    Sink& sink_;
    GenOut& res_;
    Progress& p_;
    std::stop_token stop_;
    const Config& cfg_;
    bool keep_overlays_;
    const Step* step_ = nullptr;
    u32 base_ = 0, files_ = 0, dropped_ = 0;
    u64 bytes_ = 0;
    std::set<std::string, std::less<>> seen_;
};

Result<void> put_ui(Emitter& out, Result<ui::UiOut> made) {
    if (!made) return std::unexpected(made.error());
    for (ui::Png& png : made->pngs) {
        if (auto ok = out.png(std::move(png.rel), png.w, png.h, png.rgba); !ok) return ok;
        Bytes().swap(png.rgba);  // one image in memory less
    }
    for (auto& [rel, data] : made->files)
        if (auto ok = out.file(std::move(rel), std::move(data)); !ok) return ok;
    return {};
}

// ---------------------------------------------------------------------------------------------- the steps
// build_assets.main, in its order. Weights: progress units, about 0.03 s each on this PC with a warm disk cache
// (cave::build adds 1 per file, so one cave file is the unit; research/dropin/wave2_G.md has the timings).

// cavebuild.build(hd, gc, cave_out, log): the 56 overlays (Obj_Stamp from stampbuild too, module CV).
Result<void> step_cave(const Source& hd, const Source& gc, Emitter& out, Progress& p, std::stop_token stop) {
    cave::BuildInfo info;
    auto files = cave::build(hd, gc, p, stop, &info);
    if (!files) return std::unexpected(files.error());
    for (std::string& l : info.log) out.log(std::move(l));  // "cave: ..." lines, Python's text
    if (!info.obj_stamp.log.empty()) out.log(info.obj_stamp.log);  // "stamps: Obj_Stamp.arc ..."
    for (auto& [rel, data] : *files)
        if (auto ok = out.file(std::move(rel), std::move(data)); !ok) return ok;
    return {};
}

// cavebuild.build's res/cave/hd_tags.bin and stampbuild.build's res/stamps/stamps/hd_chests.bin (module ST).
Result<void> step_tables(const Source& hd, const Source&, Emitter& out, Progress&, std::stop_token stop) {
    auto tags = stage::hd_tags_bin(hd);
    if (!tags) return std::unexpected(tags.error());
    if (auto ok = out.file("res/cave/hd_tags.bin", std::move(*tags)); !ok) return ok;
    if (stop.stop_requested()) return fail("stopped");
    auto chests = stage::hd_chests_bin(hd);
    if (!chests) return std::unexpected(chests.error());
    return out.file("res/stamps/stamps/hd_chests.bin", std::move(*chests));
}

// stampbuild.build's PNGs + gen_stamp_table.build_icons (module U).
Result<void> step_stamps(const Source& hd, const Source&, Emitter& out, Progress&, std::stop_token stop) {
    return put_ui(out, ui::build_stamps(hd, stop));
}

// hdtext.build: the cave text and the stamps text (module TX).
Result<void> step_text(const Source& hd, const Source& gc, Emitter& out, Progress&, std::stop_token stop) {
    auto t = text::build(hd, gc, stop);
    if (!t) return std::unexpected(t.error());
    if (auto ok = out.file("res/cave/hdtext/messages.bin", std::move(t->cave_messages_bin)); !ok) return ok;
    if (auto ok = out.file("res/cave/hdtext/flows.json", std::move(t->cave_flows_json)); !ok) return ok;
    return out.file("res/stamps/hdtext/messages.bin", std::move(t->stamps_messages_bin));
}

// hdtext.build_amiibo (module U).
Result<void> step_amiibo(const Source& hd, const Source&, Emitter& out, Progress&, std::stop_token stop) {
    return put_ui(out, ui::build_amiibo(hd, stop));
}

// stampbook.build (module U).
Result<void> step_stampbook(const Source& hd, const Source&, Emitter& out, Progress&, std::stop_token stop) {
    return put_ui(out, ui::build_stampbook(hd, stop));
}

// cosconfirm.build (module U).
Result<void> step_confirm(const Source& hd, const Source&, Emitter& out, Progress&, std::stop_token stop) {
    return put_ui(out, ui::build_confirm(hd, stop));
}

Config make_production() {
    Config c;
    c.hd_list = inputs::hd_files();
    c.gc_list = inputs::gc_files();
    // Weights from `hdf-convert make --time` on this PC (warm): inputs 0.20 s, cave 1.96 s (56 files: the unit is
    // about 0.035 s), tables 0.02 s, stamps 0.66 s, text 0.05 s, amiibo 0.35 s, stampbook 0.23 s, confirm 0.17 s.
    c.inputs_weight = 6;
    c.check_weight = 1;
    c.steps = {
        Step{GenStage::Cave, "cave", cave::kBuildSteps, static_cast<u32>(cave::kOverlayCount), true, &step_cave},
        Step{GenStage::Tables, "tables", 1, 2, false, &step_tables},
        Step{GenStage::Stamps, "stamps", 19, 102, false, &step_stamps},
        Step{GenStage::Text, "text", 1, 3, false, &step_text},
        Step{GenStage::Amiibo, "amiibo", 10, 7, false, &step_amiibo},
        Step{GenStage::Stampbook, "stampbook", 6, 5, false, &step_stampbook},
        Step{GenStage::Confirm, "confirm", 5, 1, false, &step_confirm},
    };
    c.check = [](std::span<const OutputRecord> made, bool partial) {
        return check_against(outputs::expected(), made, partial);
    };
    c.skip_dropped = true;
    return c;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

}  // namespace

const Config& production() {
    static const Config c = make_production();
    return c;
}

// ---------------------------------------------------------------------------------------------- input pass
Result<InputTotals> check_inputs(const Source& src, std::span<const inputs::InputFile> list, std::string_view kind,
                                 Progress& p, u32 base, u32 weight, std::stop_token stop) {
    u64 total = 0;
    for (const inputs::InputFile& f : list) total += f.size;
    u64 seen = 0;
    u32 missing = 0, wrong_size = 0, differ = 0, unreadable = 0;
    std::string first;
    std::optional<Error> read_error;
    auto note = [&](std::string_view path, const std::string& what) {
        if (first.empty()) first = std::string(path) + " (" + what + ")";
    };
    InputTotals t;
    for (const inputs::InputFile& f : list) {
        if (stop.stop_requested()) return fail("stopped");
        const auto n = src.size(f.path);
        if (!n) {
            ++missing;
            note(f.path, "missing");
        } else if (*n != f.size) {
            ++wrong_size;
            note(f.path, fmt("size %llu, expected %llu", static_cast<unsigned long long>(*n),
                             static_cast<unsigned long long>(f.size)));
        } else {
            auto data = src.read(f.path, stop);
            if (!data) {
                if (data.error().code == "stopped" || stop.stop_requested()) return fail("stopped");
                if (data.error().code == "io.missing") {
                    ++missing;
                    note(f.path, "missing");
                } else {
                    ++unreadable;
                    note(f.path, data.error().code);
                    if (!read_error) read_error = data.error();
                }
            } else if (data->size() != f.size) {
                ++wrong_size;
                note(f.path, fmt("read %zu bytes, expected %llu", data->size(), static_cast<unsigned long long>(f.size)));
            } else if (sha256_hex(*data) != f.sha256) {
                ++differ;
                note(f.path, "SHA-256 differs");
            } else {
                ++t.files;
                t.bytes += f.size;
            }
        }
        seen += f.size;
        if (total > 0) p.done = base + static_cast<u32>(u64(weight) * seen / total);
    }
    if (missing + wrong_size + differ + unreadable == 0) return t;
    const std::string k(kind);
    // wave2_G_verify L4: a listed file with the right size that can't be read (a disk error, an online-only file)
    // is "<kind>.read", so the panel speaks about the player's files and not about the mod. A damaged .wua block
    // keeps its "wua.*" code (the panel's damaged-.wua line). The source's own error is in the detail.
    const auto read_code = [&k](const Error& e) { return e.code.starts_with("wua.") ? e.code : k + ".read"; };
    std::string code =
        missing ? k + ".missing" : wrong_size ? k + ".size" : unreadable ? read_code(*read_error) : k + ".hash";
    std::string detail = fmt("%u of %zu files missing, %u wrong size, %u differ, %u unreadable; first: ", missing,
                             list.size(), wrong_size, differ, unreadable) + first;
    if (read_error)
        detail += "; read error: " + read_error->code + (read_error->detail.empty() ? "" : ": " + read_error->detail);
    return fail(std::move(code), std::move(detail));
}

Result<void> check_against(std::span<const outputs::ExpectedFile> expected, std::span<const OutputRecord> made,
                           bool partial) {
    std::map<std::string_view, const outputs::ExpectedFile*> want;
    for (const outputs::ExpectedFile& e : expected) want.emplace(e.rel, &e);
    u32 identical = 0, diff = 0, extra = 0;
    std::string first;
    std::set<std::string_view> have;
    for (const OutputRecord& m : made) {
        have.insert(m.rel);
        auto it = want.find(m.rel);
        if (it == want.end()) {
            ++extra;
            if (first.empty()) first = "extra " + m.rel;
        } else if (m.size != it->second->size || m.sha256 != it->second->sha256) {
            ++diff;
            if (first.empty())
                first = fmt("DIFF %s: size %llu vs %llu", m.rel.c_str(), static_cast<unsigned long long>(m.size),
                            static_cast<unsigned long long>(it->second->size)) +
                        (m.size == it->second->size ? ", SHA-256 differs" : "");
        } else {
            ++identical;
        }
    }
    u32 missing = 0;
    if (!partial) {
        for (const outputs::ExpectedFile& e : expected) {
            if (have.count(e.rel)) continue;
            ++missing;
            if (first.empty()) first = "missing " + std::string(e.rel);
        }
    }
    if (diff == 0 && extra == 0 && missing == 0 && (partial || identical == expected.size())) return {};
    return fail("out.check", fmt("%u identical, %u DIFF, %u missing, %u extra; first: ", identical, diff, missing,
                                 extra) + first);
}

// ---------------------------------------------------------------------------------------------- the driver
Result<GenOut> run(const Source& hd, const Source& gc, Sink& out, Progress& p, std::stop_token stop,
                   const GenOptions& options, const Config& cfg) {
    RoundingGuard rounding;
    const auto t0 = Clock::now();

    // GenOptions::only (CLI --only): step names
    for (const std::string& name : options.only) {
        const bool known = std::any_of(cfg.steps.begin(), cfg.steps.end(), [&](const Step& s) { return s.name == name; });
        if (!known) {
            std::vector<std::string> names;
            for (const Step& s : cfg.steps) names.emplace_back(s.name);
            return fail("gen.only", "unknown step '" + name + "' (steps: " + join(names) + ")");
        }
    }
    std::vector<const Step*> steps;
    for (const Step& s : cfg.steps)
        if (options.only.empty() || std::find(options.only.begin(), options.only.end(), s.name) != options.only.end())
            steps.push_back(&s);
    u32 total = cfg.inputs_weight + cfg.check_weight;
    for (const Step* s : steps) total += s->weight;
    p.stage = static_cast<u32>(GenStage::Inputs);
    p.done = 0;
    p.total = total;

    GenOut res;
    // ------------------------------------------------------------------ inputs (Correction #1a)
    {
        const auto t = Clock::now();
        u64 gc_bytes = 0, hd_bytes = 0;
        for (const inputs::InputFile& f : cfg.gc_list) gc_bytes += f.size;
        for (const inputs::InputFile& f : cfg.hd_list) hd_bytes += f.size;
        const u32 w_gc = (gc_bytes + hd_bytes) ? static_cast<u32>(u64(cfg.inputs_weight) * gc_bytes / (gc_bytes + hd_bytes)) : 0;
        // GC first: 1.1 MB, and a wrong disc is the cheaper failure to report
        auto g = check_inputs(gc, cfg.gc_list, "gc", p, 0, w_gc, stop);
        if (!g) return std::unexpected(g.error());
        auto h = check_inputs(hd, cfg.hd_list, "hd", p, w_gc, cfg.inputs_weight - w_gc, stop);
        if (!h) return std::unexpected(h.error());
        const double s = since(t);
        res.stage_seconds.emplace_back("inputs", s);
        res.log.push_back(fmt("inputs: %llu GC + %llu HD files, %llu B, SHA-256 equal to the embedded lists (%.3f s)",
                              static_cast<unsigned long long>(g->files), static_cast<unsigned long long>(h->files),
                              static_cast<unsigned long long>(g->bytes + h->bytes), s));
    }
    const inputs::CheckedSource chd(hd, cfg.hd_list, "hd");
    const inputs::CheckedSource cgc(gc, cfg.gc_list, "gc");

    // ------------------------------------------------------------------ the steps
    u32 base = cfg.inputs_weight;
    p.done = base;
    DriverEmitter em(out, res, p, stop, cfg, options.keep_overlays);
    std::optional<Error> first_failure;
    std::vector<std::string> failed;
    for (const Step* s : steps) {
        if (stop.stop_requested()) return fail("stopped");
        p.stage = static_cast<u32>(s->stage);
        em.begin(*s, base);
        const auto t = Clock::now();
        Result<void> r;
        try {
            r = s->run(chd, cgc, em, p, stop);
        } catch (const std::exception& e) {
            r = fail("gen.exception", e.what());
        } catch (...) {
            r = fail("gen.exception", "unknown exception");
        }
        const double secs = since(t);
        res.stage_seconds.emplace_back(std::string(s->name), secs);
        if (!r) {
            if (r.error().code == "stopped" || stop.stop_requested()) return fail("stopped");
            Error e{r.error().code, std::string(s->name) + ": " + r.error().detail};
            res.log.push_back(fmt("%.*s: FAILED %s (%.3f s)", int(s->name.size()), s->name.data(),
                                  (e.code + " " + e.detail).c_str(), secs));
            if (!options.keep_going) return std::unexpected(std::move(e));
            if (!first_failure) first_failure = std::move(e);
            failed.emplace_back(s->name);
        } else {
            res.log.push_back(fmt("%.*s: %u files, %llu B (%.3f s)", int(s->name.size()), s->name.data(), em.files(),
                                  static_cast<unsigned long long>(em.bytes()), secs));
        }
        base += s->weight;
        p.done = base;
    }

    // ------------------------------------------------------------------ check
    if (stop.stop_requested()) return fail("stopped");
    p.stage = static_cast<u32>(GenStage::Check);
    const auto tc = Clock::now();
    std::stable_sort(res.files.begin(), res.files.end(),
                     [](const OutputRecord& a, const OutputRecord& b) { return a.rel < b.rel; });
    if (first_failure) {
        first_failure->detail += " (" + std::to_string(failed.size()) + " step(s) failed: " + join(failed) + ")";
        return std::unexpected(std::move(*first_failure));
    }
    if (auto ok = cfg.check(res.files, !options.only.empty()); !ok) return std::unexpected(ok.error());
    const double sc = since(tc);
    res.stage_seconds.emplace_back("check", sc);
    res.log.push_back(fmt("check: %zu files equal to the embedded list%s (%.3f s)", res.files.size(),
                          options.only.empty() ? "" : " (partial: --only)", sc));
    if (em.dropped()) res.log.push_back(fmt("check: %u file(s) on the drop list were not written", em.dropped()));
    p.done = total;
    p.stage = static_cast<u32>(GenStage::Done);
    res.seconds = since(t0);
    return res;
}

}  // namespace gen

Result<GenOut> generate(const Source& hd, const Source& gc, Sink& out, Progress& p, std::stop_token stop,
                        const GenOptions& options) {
    return gen::run(hd, gc, out, p, std::move(stop), options, gen::production());
}

}  // namespace hdf
