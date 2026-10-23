// `hdf-convert test j3d`: the oracle test of module J (plan 3.3).
//
//   hdf-convert test j3d --hd <TP HD title or content folder, or .wua> --oracle <pinned .dusk>
//                        [--golden <j3d.tsv>] [--golden-stage <stage.tsv>] [--verbose]
//
//   The HD inputs are read through inputs::CheckedSource (full SHA-256 before any parser), as in the mod. The 55
//   archives are the oracle's overlay/res/Stage/D_SB11/{STG_00,R00..R49}_00.arc and overlay/res/Object/{@bg0084,
//   @bg0085,O_gD_puL4,Obj_Stamp}.arc.
//   (a) models: j3d::convert_members of every archive, then j3d::bake_los of every model of a D_SB11 room with a
//       los floor > 0 (as cavebuild.build): the 55 converted models and the 5 stage BTIs = the oracle members,
//       byte for byte (43 baked, signed zero at -0x8000 included). Per member: convert_j3d, the MAT strides,
//       mat4fix, convert_bti and the bake (vertices, bbox) = the golden list, and convert_members = that chain.
//   (b) display-list sizes and model stats: pc_dl_bytes of the HD, converted and final models, model_stats of the
//       final models, convert stats and report lines = the golden list.
//   (c) MAT repack: mat4fix of the 55 final (= oracle) models repacks nothing and leaves the bytes unchanged.
//   (d) archives: the 55 rebuilt archives = the oracle's (objects: GC retail layout, or the HD entry layout for
//       O_gD_puL4 and Obj_Stamp; rooms: room.dzr from stage::patch_room; STG_00: stage.dzs from
//       stage::patch_stage fed with the 50 room ModelStats of j3d::arc_stats); arc_stats = the golden list.
//   (e) the 50 room ModelStats = module ST's golden 'stats' lines (with --golden-stage; default
//       <exe folder>/golden/stage.tsv when it exists).
//
// The golden list comes from tools/dev/golden_j3d.py (default <exe folder>/golden/j3d.tsv). Without it the oracle
// comparisons (a) (c) (d) still run and the exit code is 3. Exit 0 when every part ran and passed, 1 when a check
// failed, 2 for usage or a wrong oracle, 3 when golden parts were skipped.
#include "../cli.hpp"

#include "hdf/facts.hpp"
#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "hdf/j3d.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"
#include "hdf/stage.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
};

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

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

std::string sha(View d) { return sha256_hex(d); }
std::string sha(const std::string& s) { return sha256_hex(View(reinterpret_cast<const u8*>(s.data()), s.size())); }
std::string num(u64 v) { return std::to_string(v); }

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
std::string basename(const std::string& p) {
    const auto k = p.rfind('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}

// ---------------------------------------------------------------------------------------------- golden list
struct Golden {
    std::vector<std::vector<std::string>> rows;  // fields, kind first
    std::map<std::string, std::vector<std::string>> by_key;  // "kind a m [which]" -> fields
    std::string counts;

    const std::vector<std::string>* get(const std::string& key) const {
        auto it = by_key.find(key);
        return it == by_key.end() ? nullptr : &it->second;
    }
};

std::optional<Golden> read_golden(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    Golden g;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# counts ", 0) == 0) g.counts = line.substr(9);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> v = split(line, '\t');
        std::string key = v[0];
        const std::string& k = v[0];
        if (k == "angle" || k == "arc" || k == "st" || k == "built" || k == "arcst") key += " " + v[1];
        else if (k == "dl") key += " " + v[1] + " " + v[2] + " " + v[3];
        else if (v.size() > 2) key += " " + v[1] + " " + v[2];
        g.by_key[key] = v;
        g.rows.push_back(std::move(v));
    }
    return g;
}

fs::path exe_dir() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return fs::path(std::wstring(exe, n)).parent_path();
#else
    return {};
#endif
}

// ---------------------------------------------------------------------------------------------- archives
struct ArcJob {
    int index = 0;
    std::string oracle;   // overlay/res/...
    std::string hd;       // res/... (HD path; case as on disk when it comes from the golden list)
    std::string pack;     // "" = none
    bool same_layout = false;
    int room = -1;        // D_SB11 room number
    bool stg = false;
};

// the 55 archives in the golden list's order (sorted oracle names)
std::vector<ArcJob> archive_jobs(const ZipSource& oracle, const Source& hd, const Golden* golden) {
    std::vector<std::string> names;
    for (const std::string& n : oracle.list())
        if (ends_with(lower(n), ".arc") && (n.rfind("overlay/res/Stage/", 0) == 0 || n.rfind("overlay/res/Object/", 0) == 0))
            names.push_back(n);
    std::sort(names.begin(), names.end());
    std::vector<ArcJob> jobs;
    for (std::size_t i = 0; i < names.size(); ++i) {
        ArcJob j;
        j.index = static_cast<int>(i);
        j.oracle = names[i];
        j.hd = names[i].substr(std::strlen("overlay/"));
        if (golden)
            if (const auto* g = golden->get("arc " + std::to_string(i)); g && g->size() == 8 && (*g)[2] == j.oracle) {
                j.hd = (*g)[3];
                if ((*g)[4] != "-") j.pack = (*g)[4];
            }
        if (j.pack.empty()) {
            const std::string p = j.hd.substr(0, j.hd.size() - 4) + ".pack.gz";
            if (hd.size(p)) j.pack = p;
        }
        const std::string fn = basename(j.hd);
        j.same_layout = lower(fn) == "o_gd_pul4.arc" || lower(fn) == "obj_stamp.arc";
        const bool stage = j.hd.find("/Stage/") != std::string::npos || lower(j.hd).find("/stage/") != std::string::npos;
        if (stage && fn.size() >= 3 && (fn[0] == 'R' || fn[0] == 'r') && std::isdigit(static_cast<unsigned char>(fn[1])) &&
            std::isdigit(static_cast<unsigned char>(fn[2])))
            j.room = (fn[1] - '0') * 10 + (fn[2] - '0');
        j.stg = stage && lower(fn).rfind("stg", 0) == 0;
        jobs.push_back(j);
    }
    return jobs;
}

// the TMPK entry convert.convert_arc picks for a member (first name ending in "/<member path>.gtx")
long long gtx_index(const io::Tmpk* tm, const std::string& member_path) {
    if (!tm) return -1;
    const std::string want = "/" + lower(member_path) + ".gtx";
    for (std::size_t i = 0; i < tm->entries.size(); ++i)
        if (ends_with(lower(tm->entries[i].name), want)) return static_cast<long long>(i);
    return -1;
}

std::string dl_text(const Result<j3d::DlBytes>& r) {
    if (!r) return "err";
    return num(r->shapes) + "\t" + num(r->packets) + "\t" + num(r->raw) + "\t" + num(r->optimized) + "\t" + num(r->copy);
}

std::string stats_text(const ModelStats& s) {
    return num(s.arc) + "\t" + num(s.models) + "\t" + num(s.mat) + "\t" + num(s.shp) + "\t" + num(s.jnt) + "\t" +
           num(s.tex) + "\t" + num(s.anm) + "\t" + num(s.files) + "\t" + num(s.dl);
}

std::string tail(const std::vector<std::string>& v, std::size_t from) {
    std::vector<std::string> t(v.begin() + static_cast<std::ptrdiff_t>(std::min(from, v.size())), v.end());
    return join(t, "\t");
}

// ---------------------------------------------------------------------------------------------- the test
int run(const cli::OracleArgs& a) {
    if (!a.oracle || !a.hd) {
        std::fprintf(stderr, "hdf-convert test j3d: --hd and --oracle are required\n");
        return cli::kExitUsage;
    }
    const auto t_all = Clock::now();
    Run rs;
    rs.verbose = a.verbose;

    auto pin = cli::file_sha256(*a.oracle);
    if (!pin || pin->first != outputs::kOracleSize || pin->second != outputs::kOracleSha256) {
        std::fprintf(stderr, "hdf-convert test j3d: the oracle is not the pinned file (%s): refused\n",
                     pin ? pin->second.c_str() : pin.error().detail.c_str());
        return cli::kExitUsage;
    }
    auto zip = ZipSource::open(*a.oracle);
    if (!zip) return cli::report("test j3d", zip.error());
    const ZipSource& oracle = **zip;
    std::printf("oracle: %s (SHA-256 pinned)\n", cli::to_utf8(*a.oracle).c_str());

    auto hd_raw = cli::open_hd(*a.hd);
    if (!hd_raw) return cli::report("test j3d", hd_raw.error());
    const inputs::CheckedSource hd(**hd_raw, inputs::hd_files(), "hd");
    std::printf("hd: %s (read through CheckedSource)\n", hd.describe().c_str());

    const fs::path golden_path = a.args && a.args->path("golden") ? *a.args->path("golden") : exe_dir() / "golden" / "j3d.tsv";
    std::optional<Golden> golden;
    if (!golden_path.empty() && fs::exists(golden_path)) golden = read_golden(golden_path);
    std::printf("golden: %s%s\n", golden ? cli::to_utf8(golden_path).c_str() : "none (make it with python -I -B tools/dev/golden_j3d.py)",
                golden && !golden->counts.empty() ? (" (" + golden->counts + ")").c_str() : "");
    const fs::path gs_path = a.args && a.args->path("golden-stage") ? *a.args->path("golden-stage") : exe_dir() / "golden" / "stage.tsv";
    std::optional<Golden> golden_stage;
    if (!gs_path.empty() && fs::exists(gs_path)) golden_stage = read_golden(gs_path);
    const Golden* G = golden ? &*golden : nullptr;

    // the angle table the port hard-codes (src/j3d/bake.cpp) = the golden list's round(cos(a), 9), round(sin(a), 9).
    // On TP HD data the sign of the -0x8000 zero cannot show (every baked normal is quantized and every position
    // gets + T, VERIFIED: a +0.0 build still passes (a)-(e)); tests/j3d_test.cpp (model F, float normals) catches it.
    if (G) {
        static const std::map<int, std::pair<std::string, std::string>> kTable = {
            {-0x8000, {"bff0000000000000", "8000000000000000"}}, {-0x4000, {"0000000000000000", "bff0000000000000"}},
            {0, {"3ff0000000000000", "0000000000000000"}}, {0x4000, {"0000000000000000", "3ff0000000000000"}}};
        int same = 0;
        for (const auto& [ang, bits] : kTable) {
            const auto* g = G->get("angle " + std::to_string(ang));
            same += g && g->size() == 6 && (*g)[4] == bits.first && (*g)[5] == bits.second;
        }
        rs.check(same == 4, "angle table bits = golden (-0x8000 -> -1.0, -0.0)");
    }

    auto los_raw = hd.read("los.bin");
    if (!los_raw) return cli::report("test j3d", los_raw.error());
    auto los = stage::read_los(*los_raw);
    if (!los) return cli::report("test j3d", los.error());

    const std::vector<ArcJob> jobs = archive_jobs(oracle, hd, G);
    rs.check(jobs.size() == 55, "55 oracle archives under overlay/res/Stage and overlay/res/Object (" + num(jobs.size()) + ")");

    stage::PatchOptions opts;  // cavebuild.build: drop_tagms = [facts.CAVE_TAG_FLOW] on GZ2E01 (module ST's test)
    opts.drop_tagms = {facts::kCaveTagFlow};

    double t_read = 0, t_convert = 0, t_bake = 0, t_write = 0, t_stats = 0, t_check = 0, t_stage = 0;
    int models_same = 0, bti_same = 0, baked = 0, baked_neg = 0, models = 0, btis = 0;
    int golden_rows_ok = 0, golden_rows_bad = 0, dl_ok = 0, ms_ok = 0, idem_ok = 0, idem_models = 0;
    int arcs_same = 0, arcst_ok = 0, st_ok = 0, chain_ok = 0;
    std::map<int, ModelStats> room_stats;
    const ArcJob* stg_job = nullptr;
    io::Rarc stg_arc;
    io::Replace stg_repl;

    auto gcheck = [&](const std::string& key, const std::string& want_tail, std::size_t from, const std::string& what) {
        if (!G) return;
        const auto* g = G->get(key);
        const bool ok = g && tail(*g, from) == want_tail;
        if (ok) ++golden_rows_ok;
        else ++golden_rows_bad;
        rs.check(ok, what + " = golden" + (ok ? "" : " (got " + want_tail + ", golden " + (g ? tail(*g, from) : "-") + ")"));
    };

    for (const ArcJob& job : jobs) {
        const std::string A = std::to_string(job.index);
        auto t0 = Clock::now();
        auto raw = hd.read(job.hd);
        if (!raw) {
            rs.check(false, job.hd + ": " + raw.error().code + " " + raw.error().detail);
            continue;
        }
        std::optional<io::Tmpk> tm;
        if (!job.pack.empty()) {
            auto p = hd.read(job.pack);
            if (!p) {
                rs.check(false, job.pack + ": " + p.error().code + " " + p.error().detail);
                continue;
            }
            auto t = io::tmpk_from_gz(*p);
            if (!t) {
                rs.check(false, job.pack + ": " + t.error().detail);
                continue;
            }
            tm = std::move(*t);
        }
        auto ob = oracle.read(job.oracle);
        if (!ob) {
            rs.check(false, job.oracle + ": " + ob.error().detail);
            continue;
        }
        auto oa = io::rarc_read(*ob);
        if (!oa) {
            rs.check(false, job.oracle + ": " + oa.error().detail);
            continue;
        }
        t_read += since(t0);

        // (a) convert
        const std::string fn = basename(job.hd);
        t0 = Clock::now();
        auto conv = j3d::convert_members(*raw, tm ? &*tm : nullptr, fn);
        t_convert += since(t0);
        if (!conv) {
            rs.check(false, job.hd + ": convert_members: " + conv.error().code + " " + conv.error().detail);
            continue;
        }
        gcheck("st " + A,
               num(conv->stats.models) + "\t" + num(conv->stats.mat_fixed) + "\t" + num(conv->stats.textures) + "\t" +
                   num(conv->stats.no_source) + "\t" + num(conv->stats.bti) + "\t" + num(conv->stats.mdl3) + "\t" +
                   num(conv->report.size()) + "\t" + sha(join(conv->report, "\n")),
               2, "(b) " + fn + " convert stats + " + num(conv->report.size()) + " report lines");
        if (a.verbose)
            for (const std::string& l : conv->report) std::printf("       %s\n", l.c_str());

        // the los entry (cavebuild.build: e and e['floor'] > 0)
        std::optional<j3d::Bake> bake;
        if (job.room >= 0)
            if (const stage::LosEntry* e = los->by_room(static_cast<u32>(job.room)); e && e->floor > 0)
                bake = j3d::Bake{{e->x, e->y, e->z}, e->angle_y};
        if (G) {
            const auto* g = G->get("arc " + A);
            const std::string want = bake ? std::to_string(bake->angle) + "\t" + io::py_float_repr(bake->t[0]) + " " +
                                                 io::py_float_repr(bake->t[1]) + " " + io::py_float_repr(bake->t[2])
                                           : "-\t-";
            rs.check(g && g->size() == 8 && (*g)[5] == (job.same_layout ? "same" : "gc") && tail(*g, 6) == want,
                     "(d) " + fn + " layout and bake (" + want + ") = golden");
        }

        // per member: the chain through the public functions = golden, convert_members = that chain
        for (std::size_t mi = 0; mi < conv->arc.members.size(); ++mi) {
            const io::RarcMember& m = conv->arc.members[mi];
            auto it = conv->replace.find(m.path);
            if (it == conv->replace.end()) continue;
            const std::string M = A + " " + std::to_string(mi);
            const std::string ln = lower(m.name);
            const bool is_bti = ends_with(ln, ".bti");
            auto d = conv->arc.data(m);
            if (!d) {
                rs.check(false, m.path + ": " + d.error().detail);
                continue;
            }
            const long long gi = gtx_index(tm ? &*tm : nullptr, m.path);
            std::optional<View> gtx;
            if (gi >= 0) gtx = tm->data(tm->entries[static_cast<std::size_t>(gi)]);
            t0 = Clock::now();
            if (is_bti) {
                ++btis;
                auto nb = j3d::convert_bti(*d, gtx);
                const bool same_chain = nb && *nb == it->second;
                chain_ok += same_chain;
                rs.check(same_chain, "(a) " + fn + " #" + std::to_string(mi) + " convert_members = convert_bti");
                if (nb)
                    gcheck("bti " + M, num(d->size()) + "\t" + sha(*d) + "\t" + std::to_string(gi) + "\t" + num(nb->size()) + "\t" + sha(*nb),
                           3, "(a) " + fn + " #" + std::to_string(mi) + " convert_bti");
            } else {
                ++models;
                auto cj = j3d::convert_j3d(*d, gtx);
                std::string strides;
                Result<j3d::Mat4Fix> fx = std::unexpected(Error{"x", "x"});
                if (cj) {
                    auto secs = j3d::sections(*cj);
                    std::vector<std::string> sv;
                    if (secs)
                        for (const j3d::Section& s : *secs)
                            if (std::memcmp(s.tag.data(), "MAT3", 4) == 0 || std::memcmp(s.tag.data(), "MAT4", 4) == 0) {
                                auto es = j3d::mat_effective_stride(*cj, s.offset);
                                char b[16];
                                std::snprintf(b, sizeof b, "%x", es && *es ? **es : 0u);
                                sv.push_back(!es ? "err" : !*es ? "-" : b);
                            }
                    strides = sv.empty() ? "-" : join(sv, ",");
                    fx = j3d::mat4fix(*cj);
                }
                const bool same_chain = fx && fx->data == it->second;
                chain_ok += same_chain;
                rs.check(same_chain, "(a) " + fn + " #" + std::to_string(mi) + " convert_members = convert_j3d + mat4fix");
                if (cj && fx)
                    gcheck("model " + M,
                           num(d->size()) + "\t" + sha(*d) + "\t" + std::to_string(gi) + "\t" + num(cj->size()) + "\t" + sha(*cj) +
                               "\t" + strides + "\t" + std::to_string(fx->fixed) + "\t" + num(fx->data.size()) + "\t" + sha(fx->data),
                           3, "(a) " + fn + " #" + std::to_string(mi) + " convert_j3d, MAT strides " + strides + ", mat4fix");
                gcheck("dl " + M + " hd", dl_text(j3d::pc_dl_detail(*d)), 4, "(b) " + fn + " #" + std::to_string(mi) + " pc_dl_bytes (HD)");
                gcheck("dl " + M + " conv", dl_text(j3d::pc_dl_detail(it->second)), 4,
                       "(b) " + fn + " #" + std::to_string(mi) + " pc_dl_bytes (converted)");
            }
            t_check += since(t0);
        }

        // bake every model of a room with a los floor > 0
        if (bake) {
            for (std::size_t mi = 0; mi < conv->arc.members.size(); ++mi) {
                const io::RarcMember& m = conv->arc.members[mi];
                auto it = conv->replace.find(m.path);
                if (it == conv->replace.end()) continue;
                const std::string lp = lower(m.path);
                if (!ends_with(lp, ".bmd") && !ends_with(lp, ".bdl")) continue;
                t0 = Clock::now();
                auto b = j3d::bake_los(it->second, bake->t, bake->angle);
                t_bake += since(t0);
                if (!b) {
                    rs.check(false, "(a) " + fn + " bake: " + b.error().code + " " + b.error().detail);
                    continue;
                }
                ++baked;
                baked_neg += bake->angle == -0x8000;
                std::string mm, mx;
                for (int k = 0; k < 3; ++k) {
                    mm += (k ? " " : "") + io::py_float_repr(b->min[static_cast<std::size_t>(k)]);
                    mx += (k ? " " : "") + io::py_float_repr(b->max[static_cast<std::size_t>(k)]);
                }
                gcheck("bake " + A + " " + std::to_string(mi),
                       num(b->vertices) + "\t" + mm + "\t" + mx + "\t" + num(b->data.size()) + "\t" + sha(b->data), 3,
                       "(a) " + fn + " #" + std::to_string(mi) + " bake angle " + std::to_string(bake->angle));
                it->second = std::move(b->data);
            }
        }

        // the final members = the oracle members; DL sizes, model stats, mat4fix idempotent
        t0 = Clock::now();
        for (std::size_t mi = 0; mi < conv->arc.members.size(); ++mi) {
            const io::RarcMember& m = conv->arc.members[mi];
            auto it = conv->replace.find(m.path);
            if (it == conv->replace.end()) continue;
            const std::string M = A + " " + std::to_string(mi);
            const bool is_bti = ends_with(lower(m.name), ".bti");
            long long omi = -1;
            for (std::size_t k = 0; k < oa->members.size(); ++k)
                if (lower(oa->members[k].path) == lower(m.path)) {
                    omi = static_cast<long long>(k);
                    break;
                }
            Result<Bytes> od = omi >= 0 ? oa->data(oa->members[static_cast<std::size_t>(omi)]) : Result<Bytes>(std::unexpected(Error{"x", "x"}));
            const bool same = od && *od == it->second;
            std::string what = "(a) " + fn + " #" + std::to_string(mi) + (is_bti ? " BTI" : " model") + " = oracle (" +
                               num(it->second.size()) + " B)";
            if (!same && od) {
                std::size_t k = 0;
                while (k < it->second.size() && k < od->size() && it->second[k] == (*od)[k]) ++k;
                what += ", oracle " + num(od->size()) + " B, first difference at " + num(k);
            }
            rs.check(same, what);
            if (is_bti) bti_same += same;
            else models_same += same;
            gcheck("final " + M, std::to_string(omi) + "\t" + num(it->second.size()) + "\t" + sha(it->second), 3,
                   "(a) " + fn + " #" + std::to_string(mi) + " final");
            if (is_bti) continue;
            const auto dl = j3d::pc_dl_detail(it->second);
            const std::string dlt = dl_text(dl);
            if (G) {
                const auto* g = G->get("dl " + M + " final");
                dl_ok += g && tail(*g, 4) == dlt;
            }
            gcheck("dl " + M + " final", dlt, 4, "(b) " + fn + " #" + std::to_string(mi) + " pc_dl_bytes (final) " + dlt);
            const ModelStats ms = j3d::model_stats(it->second);
            const std::string mst = num(ms.mat) + "\t" + num(ms.shp) + "\t" + num(ms.jnt) + "\t" + num(ms.tex) + "\t" + num(ms.dl);
            if (G) {
                const auto* g = G->get("mstats " + M);
                ms_ok += g && tail(*g, 3) == mst;
            }
            gcheck("mstats " + M, mst, 3, "(b) " + fn + " #" + std::to_string(mi) + " model_stats");
            // (c) the MAT repack leaves the fixed (= oracle) model unchanged
            if (od) {
                auto fx = j3d::mat4fix(*od);
                ++idem_models;
                const bool idem = fx && fx->fixed == 0 && fx->data == *od;
                idem_ok += idem;
                rs.check(idem, "(c) " + fn + " #" + std::to_string(mi) + " mat4fix(oracle model): 0 repacked, unchanged");
                if (fx) gcheck("idem " + M, std::to_string(fx->fixed) + "\t" + (fx->data == *od ? "1" : "0"), 3,
                               "(c) " + fn + " #" + std::to_string(mi) + " mat4fix idempotent");
            }
        }
        t_check += since(t0);

        // (d) the archive as the pipeline writes it
        if (job.stg) {  // after the rooms: stage.dzs needs their ModelStats
            stg_job = &job;
            stg_arc = std::move(conv->arc);
            stg_repl = std::move(conv->replace);
            continue;
        }
        if (job.room >= 0) {
            if (const io::RarcMember* dm = conv->arc.get("room.dzr")) {
                auto dz = conv->arc.data(*dm);
                if (!dz) {
                    rs.check(false, fn + ": room.dzr: " + dz.error().detail);
                    continue;
                }
                t0 = Clock::now();
                auto pd = stage::patch_room(job.room, *dz, opts);
                t_stage += since(t0);
                if (!pd) {
                    rs.check(false, fn + ": patch_room: " + pd.error().detail);
                    continue;
                }
                if (*pd != *dz) conv->replace[dm->path] = std::move(*pd);
            }
        }
        t0 = Clock::now();
        auto built = job.same_layout ? io::rarc_rebuild_same_layout(conv->arc, conv->replace)
                                     : io::rarc_write(conv->arc, conv->replace);
        t_write += since(t0);
        if (!built) {
            rs.check(false, fn + ": write: " + built.error().detail);
            continue;
        }
        const bool same = *built == *ob;
        arcs_same += same;
        rs.check(same, "(d) " + fn + " rebuilt (" + num(built->size()) + " B) = oracle " + job.oracle);
        gcheck("built " + A, num(built->size()) + "\t" + sha(*built), 2, "(d) " + fn + " built");
        t0 = Clock::now();
        auto st = j3d::arc_stats(*built);
        t_stats += since(t0);
        if (!st) {
            rs.check(false, fn + ": arc_stats: " + st.error().detail);
            continue;
        }
        if (G) {
            const auto* g = G->get("arcst " + A);
            arcst_ok += g && tail(*g, 2) == stats_text(*st);
        }
        gcheck("arcst " + A, stats_text(*st), 2, "(d) " + fn + " arc_stats");
        if (job.room >= 0) {
            ModelStats s = *st;
            s.room = job.room;
            room_stats[job.room] = s;
        }
    }

    // STG_00: stage.dzs from stage::patch_stage with the 50 room ModelStats (module ST), as cavebuild.build
    if (!stg_job) {
        rs.check(false, "(d) STG_00.arc not found");
    } else {
        const ArcJob& job = *stg_job;
        const std::string A = std::to_string(job.index);
        std::vector<ModelStats> rooms;
        for (const auto& [rn, s] : room_stats) rooms.push_back(s);
        rs.check(rooms.size() == 50, "(d) 50 room ModelStats from j3d::arc_stats (" + num(rooms.size()) + ")");
        const io::RarcMember* dm = stg_arc.get("stage.dzs");
        auto dz = dm ? stg_arc.data(*dm) : Result<Bytes>(std::unexpected(Error{"x", "no stage.dzs"}));
        if (!dz) {
            rs.check(false, "(d) STG_00 stage.dzs: " + dz.error().detail);
        } else {
            auto t0 = Clock::now();
            auto pd = stage::patch_stage(*dz, rooms, *los, opts);
            t_stage += since(t0);
            if (!pd) {
                rs.check(false, "(d) patch_stage: " + pd.error().code + " " + pd.error().detail);
            } else {
                auto mt = stage::stage_mem_tables(*pd);
                const bool mem_ok = mt && mt->mem0 == std::vector<u32>{0x125b00, 0xf6240, 0x16d2c0};
                rs.check(mem_ok, "(d) MEM0 from the real ModelStats = 0x125b00 / 0xf6240 / 0x16d2c0");
                stg_repl[dm->path] = std::move(*pd);
                t0 = Clock::now();
                auto built = io::rarc_write(stg_arc, stg_repl);
                t_write += since(t0);
                auto ob = oracle.read(job.oracle);
                const bool same = built && ob && *built == *ob;
                arcs_same += same;
                rs.check(same, "(d) STG_00.arc rebuilt = oracle (stage.dzs from the real ModelStats)");
                if (built) {
                    gcheck("built " + A, num(built->size()) + "\t" + sha(*built), 2, "(d) STG_00 built");
                    auto st = j3d::arc_stats(*built);
                    if (st) {
                        if (G) {
                            const auto* g = G->get("arcst " + A);
                            arcst_ok += g && tail(*g, 2) == stats_text(*st);
                        }
                        gcheck("arcst " + A, stats_text(*st), 2, "(d) STG_00 arc_stats");
                    }
                }
            }
        }
    }

    // (e) the real ModelStats = module ST's golden list
    int st_rows = 0;
    if (golden_stage) {
        for (const auto& v : golden_stage->rows) {
            if (v.empty() || v[0] != "stats" || v.size() != 11) continue;
            ++st_rows;
            const int rn = std::stoi(v[1]);
            auto it = room_stats.find(rn);
            const bool ok = it != room_stats.end() && tail(v, 2) == stats_text(it->second);
            st_ok += ok;
            rs.check(ok, "(e) R" + std::to_string(rn) + " ModelStats = ST's golden 'stats' line");
        }
        rs.check(st_rows == 50 && st_ok == 50, "(e) 50/50 room ModelStats = ST's golden list (" + std::to_string(st_ok) + ")");
    }

    rs.check(models == 55 && models_same == 55, "(a) 55 converted models identical to the oracle (" + std::to_string(models_same) + "/" + std::to_string(models) + ")");
    rs.check(btis == 5 && bti_same == 5, "(a) 5 stage BTIs identical to the oracle (" + std::to_string(bti_same) + "/" + std::to_string(btis) + ")");
    rs.check(baked == 43 && baked_neg > 0, "(a) 43 models baked (" + std::to_string(baked) + "), " + std::to_string(baked_neg) + " at -0x8000");
    rs.check(idem_models == 55 && idem_ok == 55, "(c) mat4fix leaves the 55 fixed models unchanged (" + std::to_string(idem_ok) + ")");
    rs.check(arcs_same == 55, "(d) 55 rebuilt archives identical to the oracle (" + std::to_string(arcs_same) + ")");
    if (G) {
        rs.check(golden_rows_bad == 0, "golden rows: " + std::to_string(golden_rows_ok) + " equal, " + std::to_string(golden_rows_bad) + " differ");
        rs.check(golden_rows_ok + 4 + 55 == static_cast<int>(G->rows.size()),
                 "every golden row checked (" + std::to_string(golden_rows_ok + 4 + 55) + "/" + num(G->rows.size()) + ")");
    }

    std::printf("(a) models: %d/%d identical to the oracle (%d baked, %d at -0x8000), BTIs %d/%d; convert_members = "
                "public chain for %d members\n", models_same, models, baked, baked_neg, bti_same, btis, chain_ok);
    if (G) std::printf("(b) DL sizes %d/55, model stats %d/55, archive stats %d/55 = golden; %d golden rows equal, %d differ\n",
                       dl_ok, ms_ok, arcst_ok, golden_rows_ok, golden_rows_bad);
    std::printf("(c) mat4fix on the fixed models: %d/%d unchanged, 0 repacked\n", idem_ok, idem_models);
    std::printf("(d) archives: %d/%zu rebuilt = oracle (STG_00 with stage.dzs from the real ModelStats)\n", arcs_same, jobs.size());
    if (golden_stage) std::printf("(e) room ModelStats = ST's golden list: %d/%d\n", st_ok, st_rows);
    else std::printf("(e) skipped: no ST golden list (--golden-stage)\n");
    std::printf("time read + hash + inflate inputs + oracle: %.3f s\n", t_read);
    std::printf("time j3d: convert_members %.3f s, bake %.3f s, arc_stats %.3f s; rarc write %.3f s; stage patch %.3f s; "
                "test checks %.3f s\n", t_convert, t_bake, t_stats, t_write, t_stage, t_check);
    std::printf("time test j3d: %s\n", cli::time_line(since(t_all)).c_str());
    const bool all_ran = G != nullptr && golden_stage.has_value();
    std::printf("test j3d: %s (%d failure%s)%s\n", rs.failures == 0 ? "PASS" : "FAIL", rs.failures,
                rs.failures == 1 ? "" : "s", all_ran ? "" : ", golden parts skipped");
    if (rs.failures) return cli::kExitFail;
    return all_ran ? cli::kExitOk : cli::kExitTodo;
}

}  // namespace

HDF_ORACLE_TEST("j3d", "the 55 converted models (43 baked, signed zero at -0x8000) and 5 stage BTIs = oracle; DL sizes and model stats vs golden_j3d.py; mat4fix idempotent; the 55 archives rebuilt = oracle; room ModelStats for ST", &run);
