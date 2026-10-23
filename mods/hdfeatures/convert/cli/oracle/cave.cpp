// `hdf-convert test cave`: the oracle test of module CV (plan 3.3).
//
//   hdf-convert test cave --hd <TP HD title or content folder, or .wua> --gc <extracted GZ2E01 files folder>
//                         --oracle <pinned .dusk> [--golden <cave.tsv>] [--verbose]
//
//   The HD and GC inputs are read through inputs::CheckedSource (full SHA-256 before any parser), as in the mod.
//   (a) cave::build: the 56 overlay files = the oracle's overlay/ files (same set, byte for byte) and = the embedded
//       expected list; p.done = kBuildSteps.
//   (b) the same build with the 13 GC files in a MemorySource (as the mod reads them): the same 56 files.
//   (c) the parts on their own: build_particles = the oracle's Pscene206.jpc, build_obj_stamp = the oracle's
//       Obj_Stamp.arc (stamp.bmd at entry 4); parse_jpc + build_jpc of the oracle's Pscene206.jpc give it back;
//       particles.verify on the result (texture indices, pixels, needed ids, no trailing bytes).
//   (d) the GZ2E01 particle budget recomputed from the GC folder = inputs::kPsceneBudget (267,040).
//   (e) BuildInfo = the golden list from tools/dev/golden_cave.py (the Python spec): msg group, GC flows, Tag_ms
//       flows, convert stats per archive, totals, MEM0, particle stats, halved textures, resources and texture
//       blocks of Pscene206.jpc, Obj_Stamp entry indices, and every report, patch-log and log line (SHA-256).
//   (f) stop: an already stopped token gives Error{"stopped"}; stops 1-1000 ms after the start give "stopped"
//       (never a partial result), with the latency.
// Exit 0 when every part ran and passed, 1 when a check failed, 2 for usage or a wrong oracle, 3 when the golden
// part was skipped (no list; default <exe folder>/golden/cave.tsv).
#include "../cli.hpp"

#include "hdf/cave.hpp"
#include "hdf/facts.hpp"
#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"
#include "hdf/tex.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
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
using Files = std::vector<std::pair<std::string, Bytes>>;

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
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

std::string sha(View d) { return sha256_hex(d); }
std::string sha(const std::string& s) { return sha256_hex(View(reinterpret_cast<const u8*>(s.data()), s.size())); }
std::string num(u64 v) { return std::to_string(v); }

std::size_t first_diff(View a, View b) {
    std::size_t k = 0;
    while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
    return k;
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

std::string list16(const std::vector<u16>& v) {
    if (v.empty()) return "-";
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

// the golden rows this build gives (same format as tools/dev/golden_cave.py, the 'out' rows included)
std::vector<std::vector<std::string>> rows_of(const cave::BuildInfo& in, const Files& files, const cave::Jpc& built,
                                              const cave::Jpc& G) {
    std::vector<std::vector<std::string>> rows;
    std::vector<const std::pair<std::string, Bytes>*> sorted;
    for (const auto& f : files) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->first < b->first; });
    for (auto* f : sorted) rows.push_back({"out", f->first, num(f->second.size()), sha(f->second)});
    std::vector<std::string> fl;
    for (u16 v : in.gc_flows) fl.push_back(std::to_string(v));
    rows.push_back({"msg", std::to_string(in.msg_group), num(in.gc_flows.size()), sha(join(fl, ",")), list16(in.tagms_flows),
                    list16(in.drop_tagms)});
    for (const auto& [n, s] : in.conv)
        rows.push_back({"conv", n, num(s.models), num(s.mat_fixed), num(s.textures), num(s.no_source), num(s.bti), num(s.mdl3)});
    const cave::BuildTotals& t = in.totals;
    rows.push_back({"totals", num(t.models), num(t.mat_fixed), num(t.textures), num(t.no_source), num(t.bti), num(t.baked)});
    auto hexes = [](const std::vector<u32>& v) {
        std::string s;
        char b[16];
        for (std::size_t i = 0; i < v.size(); ++i) {
            std::snprintf(b, sizeof b, "0x%x", v[i]);
            s += (i ? "," : "") + std::string(b);
        }
        return s;
    };
    rows.push_back({"mem", hexes(in.mem_old), hexes(in.mem_new)});
    rows.push_back({"budget", std::string(cave::kPsceneLargestName), num(inputs::kPsceneLargest), num(inputs::kPsceneBudget)});
    const cave::ParticleStats& p = in.particles;
    rows.push_back({"pstats", num(p.need), num(p.resources), num(p.from_gc), num(p.from_hd), num(p.textures), num(p.tex_gc),
                    num(p.tex_hd), num(p.halved.size()), num(p.size), num(p.budget), num(p.shared_header_differs),
                    num(p.hd_total), num(p.gc_total)});
    for (u32 i : p.halved_index) rows.push_back({"phalf", num(i)});
    std::set<u16> gids;
    for (const cave::JpcResource& r : G.res) gids.insert(r.id);
    std::set<std::string> gnames;
    for (const cave::JpcTexture& x : G.tex) gnames.insert(x.name);
    for (std::size_t i = 0; i < built.res.size(); ++i) {
        char id[8];
        std::snprintf(id, sizeof id, "%04x", built.res[i].id);
        rows.push_back({"pres", num(i), id, gids.contains(built.res[i].id) ? "GC" : "HD", num(built.res[i].raw.size()),
                        sha(built.res[i].raw)});
    }
    for (std::size_t i = 0; i < built.tex.size(); ++i)
        rows.push_back({"ptex", num(i), gnames.contains(built.tex[i].name) ? "GC" : "HD", num(built.tex[i].raw.size()),
                        sha(built.tex[i].raw)});
    auto idx = [](const std::optional<u32>& v) { return v ? num(*v) : std::string("None"); };
    u64 os_size = 0;
    for (const auto& f : files)
        if (f.first == "overlay/res/Object/Obj_Stamp.arc") os_size = f.second.size();
    rows.push_back({"objstamp", idx(in.obj_stamp.bmd_index_hd), idx(in.obj_stamp.bmd_index), num(os_size)});
    const std::vector<std::string> stamplog{in.obj_stamp.log};
    for (const auto& [kind, lines] : std::vector<std::pair<std::string, const std::vector<std::string>*>>{
             {"report", &in.report}, {"patch", &in.patch_log}, {"log", &in.log}, {"stamplog", &stamplog}}) {
        rows.push_back({"lines", kind, num(lines->size()), sha(join(*lines, "\n"))});
        for (std::size_t i = 0; i < lines->size(); ++i) rows.push_back({"line", kind, num(i), sha((*lines)[i])});
    }
    return rows;
}

// particles.verify(d, need): structural problems of a built file
std::vector<std::string> verify(View d, const std::vector<u16>& need) {
    std::vector<std::string> probs;
    auto p = cave::parse_jpc(d);
    if (!p) return {"parse: " + p.error().detail};
    const std::size_t nt = p->tex.size();
    std::set<u16> ids;
    for (const cave::JpcResource& r : p->res) {
        ids.insert(r.id);
        for (u16 i : r.tdb)
            if (i >= nt) probs.push_back("resource references a texture past the table");
    }
    for (const cave::JpcTexture& t : p->tex) {
        const u64 exp = tex::gx_tex_size(t.h.format, t.h.width, t.h.height, std::max<u32>(1, t.h.mip_count));
        if (t.h.image_offset != 0x20 || t.raw.size() < 0x40 + exp) probs.push_back("texture without pixels");
    }
    for (u16 id : need)
        if (!ids.contains(id)) probs.push_back("missing id");
    if (p->end != d.size()) probs.push_back("trailing bytes after the texture table");
    return probs;
}

int run(const cli::OracleArgs& a) {
    if (!a.oracle) {
        std::fprintf(stderr, "hdf-convert test cave: --oracle is required (cache/oracle/hd_features-oracle-2.0.0.dusk)\n");
        return cli::kExitUsage;
    }
    if (!a.hd || !a.gc) {
        std::printf("hdf-convert test cave: skipped: needs --hd and --gc\n");
        return cli::kExitTodo;
    }
    const auto t_all = Clock::now();
    Run rs;
    rs.verbose = a.verbose;

    auto pin = cli::file_sha256(*a.oracle);
    if (!pin || pin->first != outputs::kOracleSize || pin->second != outputs::kOracleSha256) {
        std::fprintf(stderr, "hdf-convert test cave: the oracle is not the pinned file (%s): refused\n",
                     pin ? pin->second.c_str() : pin.error().detail.c_str());
        return cli::kExitUsage;
    }
    auto zip = ZipSource::open(*a.oracle);
    if (!zip) return cli::report("test cave", zip.error());
    const ZipSource& oracle = **zip;
    std::printf("oracle: %s (SHA-256 pinned)\n", cli::to_utf8(*a.oracle).c_str());

    auto hd_raw = cli::open_hd(*a.hd);
    if (!hd_raw) return cli::report("test cave", hd_raw.error());
    const inputs::CheckedSource hd(**hd_raw, inputs::hd_files(), "hd");
    FolderSource gc_folder(*a.gc);
    const inputs::CheckedSource gc(gc_folder, inputs::gc_files(), "gc");
    std::printf("inputs: hd %s; gc %s (both through CheckedSource)\n", hd.describe().c_str(), gc_folder.describe().c_str());

    // ------------------------------------------------------------------------------------- (a) build
    std::printf("(a) cave::build (GC from the folder); peak working set before it %.1f MB\n",
                static_cast<double>(cli::peak_working_set()) / 1048576.0);
    Progress prog;
    cave::BuildInfo info;
    auto t0 = Clock::now();
    auto built = cave::build(hd, gc, prog, {}, &info);
    const double t_build = since(t0);
    if (!built) {
        rs.check(false, "cave::build: " + built.error().code + " " + built.error().detail);
        std::printf("test cave: FAIL\n");
        return cli::kExitFail;
    }
    std::printf("  time cave::build: %s (peak: the build alone, the oracle files are read after it)\n",
                cli::time_line(t_build).c_str());
    {
        std::string parts;
        for (const auto& [k, s] : info.seconds) parts += (parts.empty() ? "" : ", ") + k + " " + std::to_string(s).substr(0, 5);
        std::printf("  time parts (s): %s\n", parts.c_str());
    }
    // the oracle's 56 overlay files
    std::map<std::string, Bytes> want;
    for (const std::string& n : oracle.list()) {
        if (n.rfind("overlay/", 0) != 0 || n.back() == '/') continue;
        auto b = oracle.read(n);
        if (!b) {
            rs.check(false, n + ": " + b.error().detail);
            continue;
        }
        want[n] = std::move(*b);
    }
    rs.check(want.size() == cave::kOverlayCount, "the oracle has 56 overlay files (" + num(want.size()) + ")");

    int same_n = 0, listed_n = 0;
    std::set<std::string> names;
    for (const auto& [rel, bytes] : *built) {
        names.insert(rel);
        auto it = want.find(rel);
        const bool same = it != want.end() && it->second == bytes;
        const outputs::ExpectedFile* e = outputs::find(rel);
        const bool listed = e && e->overlay && e->size == bytes.size() && e->sha256 == sha(bytes);
        same_n += same;
        listed_n += listed;
        std::string what = rel + ": " + num(bytes.size()) + " B";
        if (same) what += " identical to the oracle";
        else if (it != want.end())
            what += ", DIFF: oracle " + num(it->second.size()) + " B, first difference at " + num(first_diff(bytes, it->second));
        else
            what += ", not in the oracle";
        rs.check(same && listed, what + (listed ? ", = embedded sha256" : ", NOT the embedded sha256"));
    }
    bool same_set = names.size() == want.size();
    for (const auto& [n, b] : want) same_set = same_set && names.contains(n);
    rs.check(built->size() == cave::kOverlayCount && same_set, "the 56 overlay names = the oracle's (" + num(built->size()) + ")");
    rs.check(prog.done.load() == cave::kBuildSteps, "progress: p.done = kBuildSteps (" + num(prog.done.load()) + ")");
    std::printf("(a) %d/%zu overlay files identical to the oracle, %d/%zu = embedded list\n", same_n, built->size(), listed_n,
                built->size());

    // ------------------------------------------------------------------------------------- (b) GC from memory
    std::printf("(b) cave::build (GC from a MemorySource)\n");
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
        const inputs::CheckedSource gcm(mem, inputs::gc_files(), "gc");
        Progress p2;
        t0 = Clock::now();
        auto b2 = ok ? cave::build(hd, gcm, p2, {}) : Result<Files>(fail("io.read", "gc"));
        const double t2 = since(t0);
        rs.check(b2 && *b2 == *built, "GC memory = GC folder: the same 56 files in the same order (second run " +
                                           cli::time_line(t2) + ")");
    }

    // ------------------------------------------------------------------------------------- (c) parts
    std::printf("(c) the parts on their own\n");
    const std::string pscene = "overlay/res/Particle/" + std::string(cave::kParticleFile);
    const std::string ostamp = "overlay/res/Object/" + std::string(cave::kObjStamp) + ".arc";
    {
        auto hd206 = hd.read("res/Particle/Pscene206.jpc");
        auto gc200 = gc.read("res/Particle/Pscene200.jpc");
        auto gtx = hd.read("tex/Particle/Pscene206.jpc.gtx.gz");
        if (!hd206 || !gc200 || !gtx) {
            rs.check(false, "particle inputs");
        } else {
            t0 = Clock::now();
            auto pd = cave::build_particles(*gc200, *hd206, *gtx, inputs::kPsceneBudget);
            const double tp = since(t0);
            rs.check(pd && *pd == want[pscene], "build_particles = the oracle's Pscene206.jpc (" +
                                                     (pd ? num(pd->size()) : std::string("error")) + " B, " +
                                                     std::to_string(tp).substr(0, 5) + " s)");
            // need = facts & the HD ids: every one present in the result
            auto H = cave::parse_jpc(*hd206);
            std::vector<u16> need;
            if (H)
                for (const cave::JpcResource& r : H->res)
                    if (facts::is_cave_particle(r.id) && std::find(need.begin(), need.end(), r.id) == need.end())
                        need.push_back(r.id);
            const auto probs = verify(want[pscene], need);
            rs.check(H && probs.empty(), "particles.verify(oracle Pscene206.jpc, need): " + num(probs.size()) + " problems (" +
                                             num(need.size()) + " ids needed)");
        }
        auto P = cave::parse_jpc(want[pscene]);
        if (!P) {
            rs.check(false, "parse_jpc(oracle Pscene206.jpc): " + P.error().detail);
        } else {
            std::vector<Bytes> res, texs;
            for (const auto& r : P->res) res.push_back(r.raw);
            for (const auto& x : P->tex) texs.push_back(x.raw);
            auto again = cave::build_jpc(res, texs);
            rs.check(again && *again == want[pscene], "build_jpc(parse_jpc(oracle Pscene206.jpc)) = the file (" +
                                                          num(P->res.size()) + " resources, " + num(P->tex.size()) +
                                                          " textures)");
        }
        cave::ObjStampInfo osi;
        auto ob = cave::build_obj_stamp(hd, {}, &osi);
        rs.check(ob && *ob == want[ostamp], "build_obj_stamp = the oracle's Obj_Stamp.arc");
        rs.check(osi.bmd_index && *osi.bmd_index == cave::kStampBmdIndex && osi.bmd_index_hd == osi.bmd_index,
                 "Obj_Stamp: stamp.bmd stays at entry index 4 (HD " +
                     (osi.bmd_index_hd ? num(*osi.bmd_index_hd) : std::string("None")) + ")");
    }

    // ------------------------------------------------------------------------------------- (d) budget
    {
        const auto b = cli::pscene_budget(*a.gc);
        rs.check(b && *b == inputs::kPsceneBudget, "Pscene budget from the GC folder = " + num(inputs::kPsceneBudget) +
                                                       " (" + (b ? num(*b) : std::string("none")) + ")");
    }

    // ------------------------------------------------------------------------------------- (e) golden
    const fs::path golden_path = a.args && a.args->path("golden") ? *a.args->path("golden") : exe_dir() / "golden" / "cave.tsv";
    bool golden_ran = false;
    std::ifstream gf(golden_path, std::ios::binary);
    if (!gf) {
        std::printf("(e) golden list: skipped (%s not found); make it with\n"
                    "    python -I -B tools/dev/golden_cave.py --hd <..> --gc <..> --oracle <..> --out <build>/golden/cave.tsv\n",
                    cli::to_utf8(golden_path).c_str());
    } else {
        golden_ran = true;
        std::printf("(e) golden list (%s)\n", cli::to_utf8(golden_path).c_str());
        std::vector<std::vector<std::string>> gw;
        std::string line;
        while (std::getline(gf, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            gw.push_back(split(line, '\t'));
        }
        auto Bj = cave::parse_jpc(want[pscene]);
        auto gc200 = gc.read("res/Particle/Pscene200.jpc");
        auto Gj = gc200 ? cave::parse_jpc(*gc200) : Result<cave::Jpc>(fail("x", "x"));
        if (!Bj || !Gj) {
            rs.check(false, "(e) parse the particle files");
        } else {
            const auto got = rows_of(info, *built, *Bj, *Gj);
            std::map<std::string, std::pair<int, int>> per;  // kind -> (rows, differ)
            const std::size_t n = std::max(gw.size(), got.size());
            int shown = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const std::string kind = i < gw.size() ? gw[i][0] : got[i][0];
                auto& pc = per[kind];
                ++pc.first;
                const bool eq = i < gw.size() && i < got.size() && gw[i] == got[i];
                if (eq) continue;
                ++pc.second;
                if (shown++ < 12) {
                    std::printf("       row %zu want: %s\n", i + 1, i < gw.size() ? join(gw[i], "\t").c_str() : "-");
                    std::printf("       row %zu got:  %s\n", i + 1, i < got.size() ? join(got[i], "\t").c_str() : "-");
                    // a differing report/log line: show ours (it is this program's own output)
                    if (i < got.size() && got[i].size() == 4 && got[i][0] == "line") {
                        const std::string& k = got[i][1];
                        const std::size_t li = static_cast<std::size_t>(std::stoul(got[i][2]));
                        const std::vector<std::string>* v = k == "report" ? &info.report : k == "patch" ? &info.patch_log
                                                          : k == "log" ? &info.log : nullptr;
                        const std::string txt = v && li < v->size() ? (*v)[li] : k == "stamplog" ? info.obj_stamp.log : "";
                        std::printf("       ours: %s\n", txt.c_str());
                    }
                }
            }
            for (const auto& [kind, pc] : per) {
                char b[160];
                std::snprintf(b, sizeof b, "(e) golden %-8s %4d rows, %d differ", kind.c_str(), pc.first, pc.second);
                rs.check(pc.second == 0, b);
            }
            rs.check(gw.size() == got.size(), "(e) golden rows " + num(got.size()) + " = " + num(gw.size()));
        }
    }
    if (a.verbose) {
        for (const std::string& l : info.log) std::printf("       log: %s\n", l.c_str());
        std::printf("       log: %s\n", info.obj_stamp.log.c_str());
    }

    // ------------------------------------------------------------------------------------- (f) stop
    std::printf("(f) stop\n");
    {
        std::stop_source ss;
        ss.request_stop();
        Progress p3;
        auto s = cave::build(hd, gc, p3, ss.get_token());
        rs.check(!s && s.error().code == "stopped", "an already stopped token gives Error{\"stopped\"}");
        double worst = 0;
        int stopped = 0, total = 0;
        for (int ms : {1, 20, 100, 300, 600, 1000}) {
            std::stop_source s2;
            Progress p4;
            Result<Files> r = fail("x");
            std::thread th([&] { r = cave::build(hd, gc, p4, s2.get_token()); });
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            const auto ts = Clock::now();
            s2.request_stop();
            th.join();
            const double lat = since(ts);
            ++total;
            if (!r) {
                stopped += r.error().code == "stopped";
                worst = std::max(worst, lat);
            }
            rs.check(r ? *r == *built : r.error().code == "stopped",
                     "stop after " + std::to_string(ms) + " ms: " + (r ? "finished first, same 56 files" : r.error().code) +
                         ", join " + std::to_string(lat * 1000.0).substr(0, 5) + " ms");
        }
        std::printf("(f) %d of %d timed stops stopped, worst latency %.1f ms\n", stopped, total, worst * 1000.0);
    }

    std::printf("time test cave: %s\n", cli::time_line(since(t_all)).c_str());
    std::printf("test cave: %s (%d failure%s)%s\n", rs.failures == 0 ? "PASS" : "FAIL", rs.failures,
                rs.failures == 1 ? "" : "s", golden_ran ? "" : ", golden part skipped");
    if (rs.failures) return cli::kExitFail;
    return golden_ran ? cli::kExitOk : cli::kExitTodo;
}

}  // namespace

HDF_ORACLE_TEST("cave", "the 51 D_SB11 archives, @bg0084, @bg0085, O_gD_puL4, Obj_Stamp, Pscene206.jpc = oracle (GC folder and memory); parts alone; BuildInfo and every report/log line vs golden_cave.py; stop", &run);
