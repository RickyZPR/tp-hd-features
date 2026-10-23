// Module CV: the Cave of Shadows (HD stage D_SB11) overlay files.
//
// Ports tools/pipeline/cavebuild.py (build up to the object archives, _list_stage, gc_flow_ids) and the Obj_Stamp
// part of tools/pipeline/stampbuild.py (build, step 1). The steps are the other modules' (each one proved
// byte-identical by its own oracle test): j3d::convert_members (convert.convert_arc), j3d::bake_los
// (bake_los.bake_model), stage::patch_room / stage::patch_stage (cave_patch + the memfit MEM0 plan),
// j3d::arc_stats (memfit.arc_stats), io::rarc_write / io::rarc_rebuild_same_layout (rarc_write.build_from_rarc,
// rarcx.rebuild_same_layout), and particles.cpp here.
//
// Not ported (not module CV's outputs): the res/ files at the end of cavebuild.build (floors.json and
// colossal_wallet.png are on the drop list; hd_tags.bin is stage::hd_tags_bin), the --hd-gates objects and the
// particle source scan (src_root is None for the oracle, plan 7.2 #3). actor_names only feeds that scan: skipped.
//
// _list_stage(hd) lists the D_SB11 folder: sorted '*.arc', STG* first, then R*. TP HD USA v81 has exactly STG_00.arc
// and R00_00.arc..R49_00.arc there (VERIFIED, and the inputs are hash-checked), so the list is fixed. Report labels
// use these names, as Python uses the names on disk.
#include "cave_internal.hpp"

#include "hdf/facts.hpp"
#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "hdf/j3d.hpp"
#include "hdf/stage.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>

namespace hdf::cave {

using namespace detail;

std::string bmgres_path(int group) {  // 'bmgres.arc' if group == 0 else 'bmgres%d.arc' % group
    return group == 0 ? std::string("res/Msgus/bmgres.arc") : fmt("res/Msgus/bmgres%d.arc", group);
}

// cavebuild.gc_flow_ids(gc_files, group), from the archive bytes
Result<std::vector<u16>> gc_flow_ids(View bmgres_arc) {
    auto a = io::rarc_read(bmgres_arc);
    if (!a) return std::unexpected(a.error());
    const io::RarcMember* bm = nullptr;  // [m for m in a.files() if m.name.lower().endswith('.bmg')][0]
    for (const io::RarcMember& m : a->members)
        if (ends_with(lower(m.name), ".bmg")) {
            bm = &m;
            break;
        }
    if (!bm) return bad("bmgres: no .bmg member");
    auto bd = a->data(*bm);
    if (!bd) return std::unexpected(bd.error());
    const View b = *bd;
    auto has = [&](u64 o, u64 n) { return o <= b.size() && b.size() - o >= n; };
    if (!has(0xC, 4)) return bad("bmg: header cut");
    const u32 n = io::be32(b, 0xC);
    u64 o = 0x20;
    std::set<u16> flows;
    for (u32 k = 0; k < n; ++k) {
        if (!has(o + 4, 4)) return bad(fmt("bmg: section %u outside the file", k));
        const u32 sz = io::be32(b, o + 4);
        if (has(o, 4) && std::memcmp(b.data() + o, "FLI1", 4) == 0) {  // tag = b[o:o + 4]
            if (!has(o + 8, 2)) return bad("bmg: FLI1 count outside the file");
            const u32 cnt = io::be16(b, o + 8);
            for (u32 i = 0; i < cnt; ++i) {
                const u64 at = o + 0x10 + u64(i) * 8;
                if (!has(at, 4)) return bad("bmg: FLI1 entry outside the file");
                flows.insert(static_cast<u16>(io::be32(b, at) >> 16));
            }
        }
        if (sz == 0) break;  // every later pass would read this same section again: the set can't change
        o += sz;
    }
    return std::vector<u16>(flows.begin(), flows.end());
}

namespace {

constexpr std::string_view kStageDir = "res/Stage/D_SB11/";
constexpr std::string_view kObjDir = "res/Object/";
constexpr std::string_view kOutStage = "overlay/res/Stage/D_SB11/";
constexpr std::string_view kOutObject = "overlay/res/Object/";
constexpr std::string_view kOutParticle = "overlay/res/Particle/";

// _list_stage: STG first, then the rooms (see the file comment)
std::vector<std::string> list_stage() {
    std::vector<std::string> names{"STG_00.arc"};
    for (int r = 0; r < kRoomCount; ++r) names.push_back(fmt("R%02d_00.arc", r));
    return names;
}

// convert.convert_arc(arc_path): the archive and its '<arc>.pack.gz' when that exists. The pack is read and inflated
// only when the archive is converted (one at a time: an inflated room pack is up to about 24 MB).
struct ArcInput {
    Bytes arc;
    std::optional<io::Tmpk> pack;
};
Result<std::optional<io::Tmpk>> read_pack(const Source& hd, const std::string& arc_path, std::stop_token stop) {
    const std::string pack = arc_path.substr(0, arc_path.size() - 4) + ".pack.gz";  // arc_path[:-4] + '.pack.gz'
    if (!hd.size(pack)) return std::optional<io::Tmpk>{};                           // os.path.exists(pack_path)
    auto p = hd.read(pack, stop);
    if (!p) return std::unexpected(p.error());
    auto t = io::tmpk_from_gz(*p, stop);  // hdpack.Tmpk(hdpack.read_any(pack_path))
    if (!t) return std::unexpected(t.error());
    return std::optional<io::Tmpk>(std::move(*t));
}
Result<ArcInput> read_arc_input(const Source& hd, const std::string& arc_path, std::stop_token stop, bool with_pack) {
    ArcInput in;
    auto raw = hd.read(arc_path, stop);
    if (!raw) return std::unexpected(raw.error());
    in.arc = std::move(*raw);
    if (with_pack) {
        auto t = read_pack(hd, arc_path, stop);
        if (!t) return std::unexpected(t.error());
        in.pack = std::move(*t);
    }
    return in;
}

std::string hex_list(const std::vector<u32>& v) {  // repr(['0x%x' % x for x in v])
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", '" : "'") + fmt("0x%x", v[i]) + "'";
    return s + "]";
}

void add_totals(BuildTotals& t, const j3d::ConvertStats& s) {  // for k in st: if k in tot: tot[k] += st[k]
    t.models += s.models;
    t.mat_fixed += s.mat_fixed;
    t.textures += s.textures;
    t.no_source += s.no_source;
    t.bti += s.bti;
}

}  // namespace

// stampbuild.build, step 1: Obj_Stamp.arc with the HD entry layout
Result<Bytes> build_obj_stamp(const Source& hd, std::stop_token stop, ObjStampInfo* info) {
    const std::string p = std::string(kObjDir) + std::string(kObjStamp) + ".arc";  // common.exists_ci(...)
    if (!hd.size(p)) return fail("hd.missing", p);
    auto in = read_arc_input(hd, p, stop, true);
    if (!in) return std::unexpected(in.error());
    auto c = j3d::convert_members(in->arc, in->pack ? &*in->pack : nullptr, std::string(kObjStamp) + ".arc", stop);
    if (!c) return std::unexpected(c.error());
    auto b = io::rarc_rebuild_same_layout(c->arc, c->replace);  // rarcx.rebuild_same_layout(a, repl)
    if (!b) return std::unexpected(b.error());
    if (info) {
        info->bmd_index_hd = io::rarc_entry_index(c->arc, "stamp.bmd");  // rarcx.entry_index(a, 'stamp.bmd')
        auto ob = io::rarc_read(*b);                                     // rarc.Rarc(b, 'out')
        info->bmd_index = ob ? io::rarc_entry_index(*ob, "stamp.bmd") : std::nullopt;
        info->conv = c->stats;
        info->report = c->report;
        auto idx = [](const std::optional<u32>& v) { return v ? std::to_string(*v) : std::string("None"); };
        info->log = fmt("stamps: Obj_Stamp.arc %zu bytes, stamp.bmd entry index %s (HD %s), %u model(s), MAT repack %u",
                        b->size(), idx(info->bmd_index).c_str(), idx(info->bmd_index_hd).c_str(), c->stats.models,
                        c->stats.mat_fixed);
    }
    return std::move(*b);
}

Result<Bytes> build_obj_stamp(const Source& hd, std::stop_token stop) { return build_obj_stamp(hd, stop, nullptr); }

// cavebuild.build(hd, gc_files, out, log) + the Obj_Stamp part of stampbuild.build
Result<std::vector<std::pair<std::string, Bytes>>> build(const Source& hd, const Source& gc, Progress& p,
                                                         std::stop_token stop, BuildInfo* info_out) {
    BuildInfo info;
    std::vector<std::pair<std::string, Bytes>> files;
    // seconds per part (BuildInfo::seconds): steady_clock deltas added into named buckets
    using Clock = std::chrono::steady_clock;
    double t_inputs = 0, t_convert = 0, t_bake = 0, t_patch = 0, t_write = 0, t_particles = 0, t_objects = 0;
    auto t0 = Clock::now();
    auto lap = [&](double& bucket) {
        const auto t = Clock::now();
        bucket += std::chrono::duration<double>(t - t0).count();
        t0 = t;
    };
    auto stopped = [&]() { return stop.stop_requested(); };
    auto step = [&]() { p.done.fetch_add(1, std::memory_order_relaxed); };

    if (stopped()) return fail("stopped", "cave");
    const std::vector<std::string> names = list_stage();  // sdir, names = _list_stage(hd)
    auto los_raw = hd.read("los.bin", stop);              // los = losbin.load(os.path.join(hd, 'los.bin'))
    if (!los_raw) return std::unexpected(los_raw.error());
    auto los = stage::read_los(*los_raw);
    if (!los) return std::unexpected(los.error());

    // HD (pre-patch) data: actor records per room, STAG message group, Tag_ms flows. The archives are read once
    // and kept for the conversion loop below (Python reads them twice; the bytes are the same).
    std::vector<ArcInput> inputs;
    inputs.reserve(names.size());
    std::map<int, std::vector<stage::Record>> hd_records;
    std::optional<Bytes> hd_stage_dzs;
    for (const std::string& n : names) {
        if (stopped()) return fail("stopped", "cave");
        auto in = read_arc_input(hd, std::string(kStageDir) + n, stop, false);
        if (!in) return std::unexpected(in.error());
        auto a = io::rarc_read(in->arc);
        if (!a) return std::unexpected(a.error());
        if (n.rfind("STG", 0) == 0) {
            const io::RarcMember* m = a->get("stage.dzs");  // a.get('stage.dzs').data()
            if (!m) return bad("D_SB11 " + n + ": no stage.dzs");
            auto d = a->data(*m);
            if (!d) return std::unexpected(d.error());
            hd_stage_dzs = std::move(*d);
        } else if (const io::RarcMember* m = a->get("room.dzr")) {
            auto d = a->data(*m);
            if (!d) return std::unexpected(d.error());
            auto r = stage::records(*d);
            if (!r) return std::unexpected(r.error());
            hd_records[std::stoi(n.substr(1, 2))] = std::move(*r);
        }
        inputs.push_back(std::move(*in));
    }
    lap(t_inputs);
    if (!hd_stage_dzs) return bad("D_SB11: no STG archive");
    auto stg = stage::stag(*hd_stage_dzs);  // stagedata.stag(hd_stage_dzs)['msgGroup']
    if (!stg) return std::unexpected(stg.error());
    info.msg_group = stg->msg_group;
    auto bmg = gc.read(bmgres_path(info.msg_group), stop);  // gc_flow_ids(gc_files, msg_group)
    if (!bmg) return std::unexpected(bmg.error());
    auto gc_flows = gc_flow_ids(*bmg);
    if (!gc_flows) return std::unexpected(gc_flows.error());
    info.gc_flows = *gc_flows;
    std::set<u16> tagms;  // sorted({r['ang'][2] & 0xFFFF for rr in hd_records.values() for r in rr if r['name'] == 'Tag_ms'})
    for (const auto& [rn, rr] : hd_records)
        for (const stage::Record& r : rr)
            if (r.name == "Tag_ms") tagms.insert(static_cast<u16>(r.ang[2] & 0xFFFF));
    info.tagms_flows.assign(tagms.begin(), tagms.end());
    for (u16 f : info.tagms_flows)  // [f for f in tagms_flows if f not in gc_flows]
        if (!std::binary_search(info.gc_flows.begin(), info.gc_flows.end(), f)) info.drop_tagms.push_back(f);
    lap(t_inputs);
    info.log.push_back(fmt("cave: %zu archives, los.bin %zu entries, msg group %d, Tag_ms flows %s (HD-only: %s)",
                           names.size(), los->entries.size(), int(info.msg_group), py_int_list(info.tagms_flows).c_str(),
                           py_int_list(info.drop_tagms).c_str()));

    stage::PatchOptions opts;  // cavebuild.build opts (the stage.hpp defaults) with drop_tagms
    opts.drop_tagms = info.drop_tagms;

    // convert, bake, patch room.dzr, write (rooms); STG_00 waits for the room stats
    std::vector<std::pair<std::string, Bytes>> built;
    std::vector<ModelStats> room_stats;
    std::optional<j3d::Converted> stg_conv;
    std::string stg_name;
    for (std::size_t k = 0; k < names.size(); ++k) {
        const std::string& n = names[k];
        if (stopped()) return fail("stopped", "cave");
        ArcInput& in = inputs[k];
        lap(t_patch);
        auto pack = read_pack(hd, std::string(kStageDir) + n, stop);
        if (!pack) return std::unexpected(pack.error());
        lap(t_inputs);
        auto c = j3d::convert_members(in.arc, *pack ? &**pack : nullptr, n, stop);  // convert.convert_arc(p)
        if (!c) return std::unexpected(c.error());
        lap(t_convert);
        in = ArcInput{};  // the HD bytes are no longer needed
        pack->reset();
        info.report.insert(info.report.end(), c->report.begin(), c->report.end());
        info.conv.emplace_back(n, c->stats);
        add_totals(info.totals, c->stats);
        if (n.rfind("STG", 0) == 0) {
            stg_conv = std::move(*c);
            stg_name = n;
            continue;
        }
        const int rn = std::stoi(n.substr(1, 2));
        const stage::LosEntry* e = los->by_room(static_cast<u32>(rn));
        if (e && e->floor > 0) {
            std::set<std::string> seen;  // for path in list(repl): each key once, in insertion (member) order
            for (const io::RarcMember& m : c->arc.members) {
                auto it = c->replace.find(m.path);
                if (it == c->replace.end() || !seen.insert(m.path).second) continue;
                const std::string lp = lower(m.path);
                if (!ends_with(lp, ".bmd") && !ends_with(lp, ".bdl")) continue;
                if (stopped()) return fail("stopped", "cave");
                auto b = j3d::bake_los(it->second, {e->x, e->y, e->z}, e->angle_y);
                if (!b) return std::unexpected(b.error());
                it->second = std::move(b->data);
                ++info.totals.baked;
                info.report.push_back(fmt("%s:%s baked los T=(%s,%s,%s) ang=%d -> bbox (%s,%s,%s)-(%s,%s,%s)", n.c_str(),
                                          m.path.c_str(), py_fixed(e->x, 0).c_str(), py_fixed(e->y, 0).c_str(),
                                          py_fixed(e->z, 0).c_str(), int(e->angle_y), py_fixed(b->min[0], 0).c_str(),
                                          py_fixed(b->min[1], 0).c_str(), py_fixed(b->min[2], 0).c_str(),
                                          py_fixed(b->max[0], 0).c_str(), py_fixed(b->max[1], 0).c_str(),
                                          py_fixed(b->max[2], 0).c_str()));
            }
        }
        lap(t_bake);
        if (const io::RarcMember* m = c->arc.get("room.dzr")) {
            auto d = c->arc.data(*m);
            if (!d) return std::unexpected(d.error());
            auto nd = stage::patch_room(rn, *d, opts, &info.patch_log);
            if (!nd) return std::unexpected(nd.error());
            if (*nd != *d) c->replace[m->path] = std::move(*nd);  // if nd != m.data(): repl[m.path] = nd
        }
        lap(t_patch);
        auto b = io::rarc_write(c->arc, c->replace);  // rarc_write.build_from_rarc(a, repl)
        if (!b) return std::unexpected(b.error());
        auto st = j3d::arc_stats(*b);  // room_stats = {int(n[1:3]): memfit.arc_stats(b) ...}
        if (!st) return std::unexpected(st.error());
        st->room = rn;
        room_stats.push_back(*st);
        built.emplace_back(n, std::move(*b));
        lap(t_write);
        step();
    }
    info.log.push_back(fmt("cave: converted %u models (%u MAT blocks repacked 0x14E->0x14C, %u baked), %u textures, %u BTI, "
                           "%u without source",
                           info.totals.models, info.totals.mat_fixed, info.totals.baked, info.totals.textures,
                           info.totals.bti, info.totals.no_source));
    if (!stg_conv) return bad("D_SB11: no STG archive");

    // MEM0: block of every room after the MEC0 rewrite, then size each block for its largest room
    const io::RarcMember* dzs_m = stg_conv->arc.get("stage.dzs");
    if (!dzs_m) return bad("D_SB11 " + stg_name + ": no stage.dzs");
    auto dzs = stg_conv->arc.data(*dzs_m);
    if (!dzs) return std::unexpected(dzs.error());
    auto mem_old = stage::stage_mem_tables(*dzs);
    if (!mem_old) return std::unexpected(mem_old.error());
    auto nd = stage::patch_stage(*dzs, room_stats, *los, opts, &info.patch_log);  // no-mem pass, plan, final pass
    if (!nd) return std::unexpected(nd.error());
    auto mem_new = stage::stage_mem_tables(*nd);
    if (!mem_new) return std::unexpected(mem_new.error());
    info.mem_old = mem_old->mem0;
    info.mem_new = mem_new->mem0;
    {
        std::string pcb = "[";
        u64 sum_new = 0, sum_old = 0;
        for (std::size_t i = 0; i < info.mem_new.size(); ++i) {
            const u64 v = stage::block_bytes_pc(info.mem_new[i]);
            pcb += (i ? ", " : "") + std::to_string(v);
            sum_new += v;
        }
        pcb += "]";
        for (u32 v : info.mem_old) sum_old += stage::block_bytes_pc(v);
        info.log.push_back(fmt("cave: MEM0 %s -> %s (PC blocks %s bytes, total %s MiB; was %s MiB)",
                               hex_list(info.mem_old).c_str(), hex_list(info.mem_new).c_str(), pcb.c_str(),
                               py_fixed(static_cast<double>(sum_new) / 1048576.0, 2).c_str(),
                               py_fixed(static_cast<double>(sum_old) / 1048576.0, 2).c_str()));
    }
    stg_conv->replace[dzs_m->path] = std::move(*nd);
    lap(t_patch);
    {
        auto b = io::rarc_write(stg_conv->arc, stg_conv->replace);
        if (!b) return std::unexpected(b.error());
        built.emplace_back(stg_name, std::move(*b));
        lap(t_write);
        step();
    }
    stg_conv.reset();
    for (auto& [n, b] : built) files.emplace_back(std::string(kOutStage) + n, std::move(b));
    built.clear();

    // scene particles (Pscene206.jpc, GC format, see particles.cpp)
    if (stopped()) return fail("stopped", "cave");
    {
        auto hd206 = hd.read("res/Particle/" + std::string(kParticleFile), stop);
        if (!hd206) return std::unexpected(hd206.error());
        auto gc200 = gc.read("res/Particle/Pscene200.jpc", stop);
        if (!gc200) return std::unexpected(gc200.error());
        auto gtx = hd.read("tex/Particle/" + std::string(kParticleFile) + ".gtx.gz", stop);
        if (!gtx) return std::unexpected(gtx.error());
        std::vector<std::string> prep;  // prep (needed_ids) + prep2 (build_scene)
        auto pdata = build_particles(*gc200, *hd206, *gtx, inputs::kPsceneBudget, &prep, &info.particles, stop);
        if (!pdata) return std::unexpected(pdata.error());
        files.emplace_back(std::string(kOutParticle) + std::string(kParticleFile), std::move(*pdata));
        info.report.insert(info.report.end(), prep.begin(), prep.end());
        info.log.push_back("cave: " + prep.front());
        info.log.push_back(fmt("cave: %s (budget = GC %s %llu B - %llu)", prep.back().c_str(),
                               std::string(kPsceneLargestName).c_str(),
                               static_cast<unsigned long long>(inputs::kPsceneLargest),
                               static_cast<unsigned long long>(inputs::kPsceneSafety)));
        if (!info.particles.halved.empty()) {
            std::string h;
            for (std::size_t i = 0; i < info.particles.halved.size(); ++i)
                h += (i ? ", " : "") + latin1_to_utf8(info.particles.halved[i]);
            info.log.push_back("cave: particle textures stored at half size to fit the heap: " + h);
        }
        lap(t_particles);
        step();
    }

    // object archives: OBJ_STAGE (GC retail layout) + OBJ_ITEM (HD entry layout kept); no --hd-gates
    std::vector<std::string> objs;
    for (std::string_view o : kObjStage) objs.emplace_back(o);
    for (std::string_view o : kObjItem) objs.emplace_back(o);
    std::vector<std::string> done_names;
    for (const std::string& o : objs) {
        if (stopped()) return fail("stopped", "cave");
        const std::string fn = o + ".arc";
        const std::string path = std::string(kObjDir) + fn;  // common.exists_ci(odir, o + '.arc')
        if (!hd.size(path)) return fail("hd.missing", "HD object " + o + ".arc not found");
        auto in = read_arc_input(hd, path, stop, true);
        if (!in) return std::unexpected(in.error());
        auto c = j3d::convert_members(in->arc, in->pack ? &*in->pack : nullptr, fn, stop);
        if (!c) return std::unexpected(c.error());
        info.report.insert(info.report.end(), c->report.begin(), c->report.end());
        info.conv.emplace_back(fn, c->stats);
        const bool same = std::find(kObjItem.begin(), kObjItem.end(), o) != kObjItem.end();
        auto b = same ? io::rarc_rebuild_same_layout(c->arc, c->replace) : io::rarc_write(c->arc, c->replace);
        if (!b) return std::unexpected(b.error());
        files.emplace_back(std::string(kOutObject) + fn, std::move(*b));
        done_names.push_back(fn);
        step();
    }
    std::sort(done_names.begin(), done_names.end());  // ', '.join(sorted(res['objects']))
    {
        std::string s;
        for (std::size_t i = 0; i < done_names.size(); ++i) s += (i ? ", " : "") + done_names[i];
        info.log.push_back(fmt("cave: %zu object archives: %s", objs.size(), s.c_str()));
    }

    // stampbuild.build step 1: Obj_Stamp.arc
    if (stopped()) return fail("stopped", "cave");
    auto os = build_obj_stamp(hd, stop, &info.obj_stamp);
    if (!os) return std::unexpected(os.error());
    files.emplace_back(std::string(kOutObject) + std::string(kObjStamp) + ".arc", std::move(*os));
    lap(t_objects);
    step();
    info.seconds = {{"inputs", t_inputs}, {"convert", t_convert}, {"bake", t_bake},     {"patch", t_patch},
                    {"write", t_write},   {"particles", t_particles}, {"objects", t_objects}};

    if (info_out) *info_out = std::move(info);
    return files;
}

Result<std::vector<std::pair<std::string, Bytes>>> build(const Source& hd, const Source& gc, Progress& p,
                                                         std::stop_token stop) {
    return build(hd, gc, p, stop, nullptr);
}

}  // namespace hdf::cave
