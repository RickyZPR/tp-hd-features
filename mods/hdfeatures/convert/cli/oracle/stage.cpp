// `hdf-convert test stage`: the oracle test of module ST (plan 3.3).
//
//   hdf-convert test stage --hd <TP HD title or content folder, or .wua> --oracle <pinned .dusk>
//                          [--golden <stage.tsv>] [--verbose]
//
//   The HD inputs are read through inputs::CheckedSource (full SHA-256 before any parser), as in the mod.
//   (a) los.bin: read_los = the golden list (count, header, every entry; floats as Python repr()).
//   (b) records: stagedata.records of every D_SB11 room.dzr = the golden list (43 rooms have one).
//   (c) rooms: patch_room of each of the 50 HD room archives' room.dzr = the room.dzr inside the oracle's
//       overlay/res/Stage/D_SB11/Rnn_00.arc (43 patched; the 7 rooms without a room.dzr have none in the oracle
//       either), and output + report lines = the golden list.
//   (d) stage: ModelStats of the 50 built rooms from the golden list (module J's numbers; until J lands they come
//       from memfit.arc_stats in Python), estimate = golden, MEC0/MEM0 plan = golden, patch_stage = the oracle's
//       STG_00.arc stage.dzs, MEM0 = 0x125b00 / 0xf6240 / 0x16d2c0.
//   (e) hd_tags_bin and hd_chests_bin = res/cave/hd_tags.bin and res/stamps/stamps/hd_chests.bin of the oracle.
//
// The golden list comes from tools/dev/golden_stage.py (default <exe folder>/golden/stage.tsv). Without it (a),
// (b) and (d) are skipped, (c) and (e) still compare with the oracle, and the exit code is 3.
// Exit 0 when every part ran and passed, 1 when a part failed, 2 for usage or a wrong oracle, 3 when skipped.
#include "../cli.hpp"

#include "hdf/facts.hpp"
#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"
#include "hdf/stage.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using namespace hdf;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

struct Run {
    int failures = 0;
    bool verbose = false;
    void check(bool ok, const std::string& what) {
        if (!ok || verbose) std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) ++failures;
    }
    void ok(const std::string& what) { std::printf("  ok   %s\n", what.c_str()); }
};

std::string room_name(int r) {
    char b[16];
    std::snprintf(b, sizeof b, "R%02d_00.arc", r);
    return b;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += '\n';
        s += v[i];
    }
    return s;
}

std::string sha(View d) { return sha256_hex(d); }
std::string sha(const std::string& s) { return sha256_hex(View(reinterpret_cast<const u8*>(s.data()), s.size())); }

// Python '%r' of a float32 read with struct ('>f'): repr of the double
std::string r(float f) { return io::py_float_repr(static_cast<double>(f)); }

// ---------------------------------------------------------------------------------------------- golden list
struct Golden {
    std::multimap<std::string, std::vector<std::string>> rows;  // kind -> fields after the kind
    const std::vector<std::string>* one(const std::string& kind) const {
        auto it = rows.find(kind);
        return it == rows.end() ? nullptr : &it->second;
    }
    std::map<int, std::vector<std::string>> by_room(const std::string& kind) const {
        std::map<int, std::vector<std::string>> m;
        auto [lo, hi] = rows.equal_range(kind);
        for (auto it = lo; it != hi; ++it)
            if (!it->second.empty()) m[std::stoi(it->second[0])] = it->second;
        return m;
    }
};

std::optional<Golden> read_golden(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    Golden g;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> v = split(line, '\t');
        const std::string kind = v[0];
        v.erase(v.begin());
        g.rows.emplace(kind, std::move(v));
    }
    return g;
}

fs::path default_golden() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return fs::path(std::wstring(exe, n)).parent_path() / "golden" / "stage.tsv";
#else
    return {};
#endif
}

// ---------------------------------------------------------------------------------------------- inputs
struct Arc {
    std::optional<io::Rarc> arc;
    std::optional<Bytes> dzr;  // room.dzr / stage.dzs data, if the archive has it
};

Result<Arc> load_arc(const Source& src, const std::string& path, std::string_view member) {
    auto raw = src.read(path);
    if (!raw) return std::unexpected(raw.error());
    auto a = io::rarc_read(*raw);
    if (!a) return std::unexpected(a.error());
    Arc out;
    if (const io::RarcMember* m = a->get(member)) {
        auto d = a->data(*m);
        if (!d) return std::unexpected(d.error());
        out.dzr = std::move(*d);
    }
    out.arc = std::move(*a);
    return out;
}

// ---------------------------------------------------------------------------------------------- the test
int run(const cli::OracleArgs& a) {
    if (!a.oracle || !a.hd) {
        std::fprintf(stderr, "hdf-convert test stage: --hd and --oracle are required\n");
        return cli::kExitUsage;
    }
    const auto t_all = Clock::now();
    Run rs;
    rs.verbose = a.verbose;

    // the pinned oracle
    auto pin = cli::file_sha256(*a.oracle);
    if (!pin || pin->first != outputs::kOracleSize || pin->second != outputs::kOracleSha256) {
        std::fprintf(stderr, "hdf-convert test stage: the oracle is not the pinned file (%s): refused\n",
                     pin ? pin->second.c_str() : pin.error().detail.c_str());
        return cli::kExitUsage;
    }
    auto zip = ZipSource::open(*a.oracle);
    if (!zip) return cli::report("test stage", zip.error());
    const Source& oracle = **zip;
    std::printf("oracle: %s (SHA-256 pinned)\n", cli::to_utf8(*a.oracle).c_str());

    auto hd_raw = cli::open_hd(*a.hd);
    if (!hd_raw) return cli::report("test stage", hd_raw.error());
    const inputs::CheckedSource hd(**hd_raw, inputs::hd_files(), "hd");
    std::printf("hd: %s (read through CheckedSource)\n", hd.describe().c_str());

    const fs::path golden_path = a.args && a.args->path("golden") ? *a.args->path("golden") : default_golden();
    std::optional<Golden> golden;
    if (!golden_path.empty() && fs::exists(golden_path)) golden = read_golden(golden_path);
    std::printf("golden: %s\n", golden ? cli::to_utf8(golden_path).c_str()
                                        : "none (make it with python -I -B tools/dev/golden_stage.py; (a) (b) (d) skipped)");

    // HD inputs: los.bin, STG_00.arc, the 50 room archives
    const auto t_read = Clock::now();
    auto los_raw = hd.read("los.bin");
    if (!los_raw) return cli::report("test stage", los_raw.error());
    auto stg = load_arc(hd, "res/Stage/D_SB11/STG_00.arc", "stage.dzs");
    if (!stg) return cli::report("test stage", stg.error());
    std::vector<Arc> rooms_hd(50);
    for (int r = 0; r < 50; ++r) {
        auto x = load_arc(hd, "res/Stage/D_SB11/" + room_name(r), "room.dzr");
        if (!x) return cli::report("test stage", x.error());
        rooms_hd[static_cast<std::size_t>(r)] = std::move(*x);
    }
    std::printf("  time read + hash 52 HD inputs: %s\n", cli::time_line(since(t_read)).c_str());
    double t_work = 0;  // the module's own functions only

    // (a) los.bin
    bool a_ran = false;
    auto t0 = Clock::now();
    auto los = stage::read_los(*los_raw);
    t_work += since(t0);
    if (!los) return cli::report("test stage", los.error());
    std::printf("(a) los.bin: %u entries, %llu bytes\n", los->count, (unsigned long long)los->size);
    if (golden) {
        if (const auto* g = golden->one("los")) {
            a_ran = true;
            std::string lines;
            for (const stage::LosEntry& e : los->entries) {
                char b[96];
                std::snprintf(b, sizeof b, "%u %u %d %d %d ", e.index, e.room, e.next, e.prev, e.floor);
                lines += b;
                lines += r(e.x) + " " + r(e.y) + " " + r(e.z) + " " + std::to_string(e.unk1c) + " " +
                         std::to_string(e.angle_y) + "\n";
            }
            const bool same = g->size() == 5 && (*g)[0] == std::to_string(los->count) &&
                              (*g)[1] == std::to_string(los->unk4) && (*g)[2] == std::to_string(los->unk8) &&
                              (*g)[3] == std::to_string(los->size) && (*g)[4] == sha(lines);
            rs.check(same, "(a) los.bin count, header, size and all entries = golden");
            if (same) rs.ok("(a) los.bin = golden (" + std::to_string(los->count) + " entries)");
        }
    }
    auto chain = stage::floor_chain(*los);
    if (chain) std::printf("    floor chain: %zu rooms from floor 1\n", chain->size());

    // (b) records
    bool b_ran = false;
    {
        int with_dzr = 0, same = 0;
        const auto g = golden ? golden->by_room("records") : std::map<int, std::vector<std::string>>{};
        for (int rn = 0; rn < 50; ++rn) {
            const Arc& x = rooms_hd[static_cast<std::size_t>(rn)];
            if (!x.dzr) continue;
            ++with_dzr;
            t0 = Clock::now();
            auto recs = stage::records(*x.dzr);
            t_work += since(t0);
            if (!recs) {
                rs.check(false, "(b) R" + std::to_string(rn) + ": " + recs.error().detail);
                continue;
            }
            if (!golden) continue;
            b_ran = true;
            std::string lines;
            for (const stage::Record& rec : *recs) {
                char b[160];
                std::snprintf(b, sizeof b, "%s %d %u %u %s %u ", rec.tag.c_str(), rec.layer, rec.index, rec.offset,
                              rec.name.c_str(), rec.params);
                lines += b;
                lines += r(rec.pos[0]) + " " + r(rec.pos[1]) + " " + r(rec.pos[2]);
                std::snprintf(b, sizeof b, " %d %d %d %u\n", rec.ang[0], rec.ang[1], rec.ang[2], unsigned(rec.set_id));
                lines += b;
            }
            auto it = g.find(rn);
            const bool ok = it != g.end() && it->second.size() == 3 && it->second[1] == std::to_string(recs->size()) &&
                            it->second[2] == sha(lines);
            rs.check(ok, "(b) R" + std::to_string(rn) + " records = golden (" + std::to_string(recs->size()) + ")");
            same += ok;
        }
        std::printf("(b) records: %d rooms have a room.dzr", with_dzr);
        if (golden) std::printf(", %d/%zu equal the golden list", same, g.size());
        std::printf("\n");
        if (golden) rs.check(same == with_dzr && static_cast<std::size_t>(same) == g.size(), "(b) every room's records = golden");
    }

    // options as cavebuild.build: drop_tagms = HD-only Tag_ms flows = [facts.CAVE_TAG_FLOW] on GZ2E01
    stage::PatchOptions opts;
    opts.drop_tagms = {facts::kCaveTagFlow};
    if (golden) {
        if (const auto* g = golden->one("flows")) {
            const bool ok = g->size() == 3 && (*g)[2] == std::to_string(facts::kCaveTagFlow);
            rs.check(ok, "golden HD-only Tag_ms flows = [facts::kCaveTagFlow] (" + (g->size() == 3 ? (*g)[2] : "?") + ")");
        }
    }

    // (c) rooms
    {
        const auto t_part = Clock::now();
        const auto g = golden ? golden->by_room("room") : std::map<int, std::vector<std::string>>{};
        int patched = 0, identical = 0, golden_same = 0, none_both = 0;
        for (int rn = 0; rn < 50; ++rn) {
            const Arc& x = rooms_hd[static_cast<std::size_t>(rn)];
            auto o = load_arc(oracle, "overlay/res/Stage/D_SB11/" + room_name(rn), "room.dzr");
            if (!o) {
                rs.check(false, "(c) oracle " + room_name(rn) + ": " + o.error().code + " " + o.error().detail);
                continue;
            }
            if (!x.dzr) {
                rs.check(!o->dzr, "(c) R" + std::to_string(rn) + ": no room.dzr in HD, none in the oracle");
                none_both += !o->dzr;
                continue;
            }
            std::vector<std::string> rep;
            t0 = Clock::now();
            auto out = stage::patch_room(rn, *x.dzr, opts, &rep);
            t_work += since(t0);
            if (!out) {
                rs.check(false, "(c) R" + std::to_string(rn) + ": " + out.error().code + " " + out.error().detail);
                continue;
            }
            ++patched;
            const bool same = o->dzr && *o->dzr == *out;
            identical += same;
            std::string what = "(c) R" + std::to_string(rn) + " room.dzr = oracle (" + std::to_string(out->size()) + " B)";
            if (!same && o->dzr) {
                std::size_t k = 0;
                while (k < out->size() && k < o->dzr->size() && (*out)[k] == (*o->dzr)[k]) ++k;
                what += ", oracle " + std::to_string(o->dzr->size()) + " B, first difference at " + std::to_string(k);
            }
            rs.check(same, what);
            if (golden) {
                auto it = g.find(rn);
                const bool ok = it != g.end() && it->second.size() == 5 && it->second[1] == std::to_string(out->size()) &&
                                it->second[2] == sha(*out) && it->second[3] == std::to_string(rep.size()) &&
                                it->second[4] == sha(join(rep));
                golden_same += ok;
                rs.check(ok, "(c) R" + std::to_string(rn) + " output + " + std::to_string(rep.size()) +
                                  " report lines = golden");
            }
            if (a.verbose)
                for (const std::string& l : rep) std::printf("       %s\n", l.c_str());
        }
        std::printf("(c) rooms: %d room.dzr patched, %d identical to the oracle; %d rooms without a room.dzr (none in "
                    "the oracle either)", patched, identical, none_both);
        if (golden) std::printf("; %d/%zu output + report = golden", golden_same, g.size());
        std::printf("\n");
        rs.check(patched == 43 && identical == 43 && none_both == 7, "(c) 43 room.dzr identical + 7 without one = 50 rooms");
        if (golden) rs.check(golden_same == 43, "(c) 43 outputs and report lines = golden");
        std::printf("  time (c): %s\n", cli::time_line(since(t_part)).c_str());
    }

    // (d) stage.dzs with MEM0
    bool d_ran = false;
    if (!stg->dzr) {
        rs.check(false, "(d) the HD STG_00.arc has no stage.dzs");
    } else if (golden) {
        const auto t_part = Clock::now();
        const auto stats_rows = golden->by_room("stats");
        const auto need_rows = golden->by_room("need");
        std::vector<ModelStats> rooms;
        int need_same = 0;
        for (const auto& [rn, v] : stats_rows) {
            if (v.size() != 10) continue;
            ModelStats s;
            s.room = rn;
            s.arc = std::stoull(v[1]);
            s.models = static_cast<u32>(std::stoul(v[2]));
            s.mat = static_cast<u32>(std::stoul(v[3]));
            s.shp = static_cast<u32>(std::stoul(v[4]));
            s.jnt = static_cast<u32>(std::stoul(v[5]));
            s.tex = static_cast<u32>(std::stoul(v[6]));
            s.anm = static_cast<u32>(std::stoul(v[7]));
            s.files = static_cast<u32>(std::stoul(v[8]));
            s.dl = std::stoull(v[9]);
            auto it = need_rows.find(rn);
            need_same += it != need_rows.end() && it->second.size() == 2 && it->second[1] == std::to_string(stage::estimate(s));
            rooms.push_back(s);
        }
        d_ran = true;
        rs.check(rooms.size() == 50, "(d) 50 room ModelStats in the golden list (" + std::to_string(rooms.size()) + ")");
        rs.check(need_same == 50, "(d) estimate = memfit.estimate for " + std::to_string(need_same) + "/50 rooms");

        // MEC0 after the rewrite and the plan, as cavebuild.build
        auto nomem = stage::patch_stage_mem(*stg->dzr, *los, opts, std::nullopt);
        auto old_t = stage::stage_mem_tables(*stg->dzr);
        if (!nomem || !old_t) {
            rs.check(false, "(d) no-mem pass / stage_mem_tables failed");
        } else {
            auto new_t = stage::stage_mem_tables(*nomem);
            std::string mec_s, old_s, new_s;
            if (new_t)
                for (const auto& [room, blk] : new_t->mec) mec_s += (mec_s.empty() ? "" : ",") + std::to_string(room) + ":" + std::to_string(blk);
            const std::vector<u32> plan = new_t ? stage::plan_mem0(rooms, new_t->mec, old_t->mem0.size()) : std::vector<u32>{};
            char b[32];
            for (u32 v : old_t->mem0) {
                std::snprintf(b, sizeof b, "0x%x", v);
                old_s += (old_s.empty() ? "" : ",") + std::string(b);
            }
            for (u32 v : plan) {
                std::snprintf(b, sizeof b, "0x%x", v);
                new_s += (new_s.empty() ? "" : ",") + std::string(b);
            }
            const auto* gm = golden->one("mec");
            rs.check(gm && gm->size() == 1 && (*gm)[0] == mec_s, "(d) MEC0 after the rewrite = golden");
            const auto* g0 = golden->one("mem0");
            rs.check(g0 && g0->size() == 2 && (*g0)[0] == old_s && (*g0)[1] == new_s,
                      "(d) MEM0 plan " + old_s + " -> " + new_s + " = golden");
        }

        std::vector<std::string> rep;
        t0 = Clock::now();
        auto out = stage::patch_stage(*stg->dzr, rooms, *los, opts, &rep);
        t_work += since(t0);
        if (!out) {
            rs.check(false, "(d) patch_stage: " + out.error().code + " " + out.error().detail);
        } else {
            auto o = load_arc(oracle, "overlay/res/Stage/D_SB11/STG_00.arc", "stage.dzs");
            const bool same = o && o->dzr && *o->dzr == *out;
            rs.check(same, "(d) stage.dzs = the oracle's (" + std::to_string(out->size()) + " B)");
            auto mt = stage::stage_mem_tables(*out);
            const bool mem_ok = mt && mt->mem0 == std::vector<u32>{0x125b00, 0xf6240, 0x16d2c0};
            std::string mem_s;
            if (mt)
                for (u32 v : mt->mem0) {
                    char b[32];
                    std::snprintf(b, sizeof b, "0x%x", v);
                    mem_s += (mem_s.empty() ? "" : " / ") + std::string(b);
                }
            rs.check(mem_ok, "(d) MEM0 = 0x125b00 / 0xf6240 / 0x16d2c0 (" + mem_s + ")");
            const auto* gs = golden->one("stage");
            rs.check(gs && gs->size() == 4 && (*gs)[0] == std::to_string(out->size()) && (*gs)[1] == sha(*out) &&
                          (*gs)[2] == std::to_string(rep.size()) && (*gs)[3] == sha(join(rep)),
                      "(d) stage.dzs + " + std::to_string(rep.size()) + " report lines = golden");
            std::printf("(d) stage.dzs: %s, MEM0 %s, %zu report lines\n", same ? "identical" : "DIFFERS", mem_s.c_str(),
                        rep.size());
            if (a.verbose)
                for (const std::string& l : rep) std::printf("       %s\n", l.c_str());
        }
        std::printf("  time (d): %s\n", cli::time_line(since(t_part)).c_str());
    } else {
        std::printf("(d) skipped: the ModelStats come from the golden list until module J lands\n");
    }

    // (e) res/ binaries
    {
        const auto t_part = Clock::now();
        struct Bin {
            const char* what;
            const char* rel;
            const char* golden_kind;
            Result<Bytes> (*fn)(const Source&);
        };
        const Bin bins[] = {{"hd_tags.bin", "res/cave/hd_tags.bin", "hdtags", &stage::hd_tags_bin},
                            {"hd_chests.bin", "res/stamps/stamps/hd_chests.bin", "hdchests", &stage::hd_chests_bin}};
        for (const Bin& bin : bins) {
            t0 = Clock::now();
            auto out = bin.fn(hd);
            t_work += since(t0);
            if (!out) {
                rs.check(false, std::string("(e) ") + bin.what + ": " + out.error().code + " " + out.error().detail);
                continue;
            }
            auto o = oracle.read(bin.rel);
            const bool same = o && *o == *out;
            rs.check(same, std::string("(e) ") + bin.what + " = the oracle's " + bin.rel + " (" +
                                std::to_string(out->size()) + " B)");
            if (golden) {
                const auto* g = golden->one(bin.golden_kind);
                rs.check(g && g->size() == 2 && (*g)[0] == std::to_string(out->size()) && (*g)[1] == sha(*out),
                          std::string("(e) ") + bin.what + " = golden");
            }
            std::printf("(e) %s: %s (%zu B)\n", bin.what, same ? "identical" : "DIFFERS", out->size());
        }
        std::printf("  time (e) (reads + hashes 9 HD archives): %s\n", cli::time_line(since(t_part)).c_str());
    }

    std::printf("time stage functions only: %.3f s\n", t_work);
    std::printf("time test stage: %s\n", cli::time_line(since(t_all)).c_str());
    const bool all_ran = a_ran && b_ran && d_ran;
    std::printf("test stage: %s (%d failure%s)%s\n", rs.failures == 0 ? "PASS" : "FAIL", rs.failures,
                rs.failures == 1 ? "" : "s", all_ran ? "" : ", golden parts skipped");
    if (rs.failures) return cli::kExitFail;
    return all_ran ? cli::kExitOk : cli::kExitTodo;
}

}  // namespace

HDF_ORACLE_TEST("stage", "stage.dzs (MEM0 0x125b00/0xf6240/0x16d2c0), the 43 room.dzr of the 50 D_SB11 rooms, hd_tags.bin, hd_chests.bin; los/records/report lines vs golden_stage.py", &run);
