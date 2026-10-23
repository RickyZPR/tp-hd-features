// Module CV: the scene particle file of the Cave of Shadows (overlay/res/Particle/Pscene206.jpc).
//
// Ports tools/pipeline/particles.py: parse, build, tex_block, _convert_hd_texture, build_scene. needed_ids without a
// source checkout is facts.CAVE_PARTICLE_IDS & the HD 206 ids (facts::kCaveParticleIds); gc_budget is the GZ2E01
// constant inputs::kPsceneBudget (the caller passes it). The texture conversion is module T's hd2gc part
// (tex::convert_texture, tex::patch_header), exactly as test tex (d) proved on the 27 HD particle textures.
//
// build_scene's halving loop calls blocks() again after every halving. Each HD texture converts to the same bytes
// every time, so the port converts each (name, half) pair once and reuses it; the report gets the same lines as
// Python's repeated calls.
#include "cave_internal.hpp"

#include "hdf/facts.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace hdf::cave {

using namespace detail;

namespace {

constexpr char kMagic[8] = {'J', 'P', 'A', 'C', '2', '-', '1', '0'};  // particles.MAGIC

// struct.unpack_from needs [o, o + n) inside d
bool has(View d, u64 o, u64 n) { return o <= d.size() && d.size() - o >= n; }

// Python d[a:b] for 0 <= a, b (clipped)
Bytes slice(View d, u64 a, u64 b) {
    const u64 n = d.size();
    a = std::min(a, n);
    b = std::min(std::max(a, b), n);
    return Bytes(d.begin() + static_cast<std::ptrdiff_t>(a), d.begin() + static_cast<std::ptrdiff_t>(b));
}

}  // namespace

// particles.parse(d)
Result<Jpc> parse_jpc(View d) {
    if (d.size() < 8 || std::memcmp(d.data(), kMagic, 8) != 0) return bad("not a JPAC2-10 file");
    if (!has(d, 8, 8)) return bad("JPAC2-10 header cut");
    const u32 nres = io::be16(d, 8), ntex = io::be16(d, 10), texoff = io::be32(d, 12);
    Jpc out;
    u64 o = 0x10;
    out.res.reserve(nres);
    for (u32 i = 0; i < nres; ++i) {
        if (!has(d, o, 7)) return bad(fmt("resource %u header outside the file", i));  // '>HHBBB'
        JpcResource r;
        r.id = io::be16(d, o);
        const u32 nblk = io::be16(d, o + 2);
        const u32 ntdb = d[o + 6];
        const u64 st = o;
        o += 8;
        std::optional<u64> tdb_off;
        for (u32 k = 0; k < nblk; ++k) {
            if (!has(d, o + 4, 4)) return bad(fmt("resource %04x: block %u outside the file", r.id, k));
            const u32 sz = io::be32(d, o + 4);
            if (sz < 8) return bad(fmt("bad block size in resource %04x", r.id));
            if (std::memcmp(d.data() + o, "TDB1", 4) == 0) tdb_off = o + 8 - st;  // tag == b'TDB1' (last wins)
            o += sz;
        }
        r.raw = slice(d, st, o);  // raw = d[st:o]
        if (tdb_off) {
            if (*tdb_off > 0xFFFFFFFFull) return bad("TDB1 offset too large");
            r.tdb_off = static_cast<u32>(*tdb_off);
            if (ntdb) {  // list(struct.unpack_from('>%dH' % ntdb, raw, tdb_off))
                if (!has(r.raw, *tdb_off, 2ull * ntdb)) return bad(fmt("resource %04x: TDB1 outside the resource", r.id));
                for (u32 k = 0; k < ntdb; ++k) r.tdb.push_back(io::be16(r.raw, static_cast<std::size_t>(*tdb_off + 2 * k)));
            }
        }
        out.res.push_back(std::move(r));
    }
    if (o > texoff) return bad("resources overrun the texture table");
    o = texoff;
    out.tex.reserve(ntex);
    for (u32 i = 0; i < ntex; ++i) {
        if (!has(d, o, 4) || std::memcmp(d.data() + o, "TEX1", 4) != 0) return bad(fmt("texture %u: no TEX1 block", i));
        if (!has(d, o + 4, 4)) return bad(fmt("texture %u: size outside the file", i));
        const u32 sz = io::be32(d, o + 4);
        JpcTexture t;
        const Bytes nm = slice(d, o + 0xC, o + 0x20);  // .split(b'\0')[0]
        std::size_t n = 0;
        while (n < nm.size() && nm[n] != 0) ++n;
        t.name.assign(reinterpret_cast<const char*>(nm.data()), n);
        t.raw = slice(d, o, o + sz);
        if (!has(d, o + 0x20, 0x20)) return bad(fmt("texture %u: header outside the file", i));  // j3d.bti_header
        auto h = tex::bti_fields(d, static_cast<std::size_t>(o + 0x20));
        if (!h) return std::unexpected(h.error());
        t.h = *h;
        out.tex.push_back(std::move(t));
        o += sz;
    }
    out.end = o;
    return out;
}

// particles.build(resources, textures)
Result<Bytes> build_jpc(std::span<const Bytes> resources, std::span<const Bytes> textures) {
    if (resources.size() > 0xFFFF || textures.size() > 0xFFFF) return bad("too many resources or textures");
    u64 body = 0;
    for (const Bytes& r : resources) body += r.size();
    const u64 texoff = io::align(0x10 + body, 0x20);  // common.align(0x10 + len(body), 0x20)
    if (texoff > 0xFFFFFFFFull) return bad("particle file too large");
    u64 total = texoff;
    for (const Bytes& t : textures) total += t.size();
    Bytes out;
    out.reserve(static_cast<std::size_t>(total));
    out.insert(out.end(), kMagic, kMagic + 8);
    out.resize(0x10, 0);
    io::put16(out, 8, static_cast<u16>(resources.size()));
    io::put16(out, 10, static_cast<u16>(textures.size()));
    io::put32(out, 12, static_cast<u32>(texoff));
    for (const Bytes& r : resources) out.insert(out.end(), r.begin(), r.end());
    out.resize(static_cast<std::size_t>(texoff), 0);  // b'\0' * (texoff - len(out))
    for (const Bytes& t : textures) out.insert(out.end(), t.begin(), t.end());
    return out;
}

// particles.tex_block(name_hdr, timg, pixels)
Result<Bytes> tex_block(View name_hdr, View timg, View pixels) {
    Bytes blk(name_hdr.begin(), name_hdr.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(0x20, name_hdr.size())));
    blk.insert(blk.end(), timg.begin(), timg.end());
    blk.insert(blk.end(), pixels.begin(), pixels.end());
    blk.resize(blk.size() + (32 - pixels.size() % 32) % 32, 0);  // pixels + b'\0' * ((-len(pixels)) % 32)
    if (blk.size() < 8) return bad("tex_block: block shorter than 8 bytes");
    if (blk.size() > 0xFFFFFFFFull) return bad("tex_block: block too large");
    io::put32(blk, 4, static_cast<u32>(blk.size()));
    return blk;
}

namespace {

struct HdBlock {
    Bytes block;
    std::string report;  // the hd2gc.convert_texture line
};

// particles._convert_hd_texture(t, gtx, surf, half, report)
Result<HdBlock> convert_hd_texture(const JpcTexture& t, const tex::GtxSurface& surf, bool half) {
    tex::BtiFields h = t.h;  // h = dict(t['h'])
    if (half) {
        h.width = static_cast<u16>(std::max<u32>(8, h.width / 2u));    // max(8, h['w'] // 2)
        h.height = static_cast<u16>(std::max<u32>(8, h.height / 2u));  // max(8, h['h'] // 2)
    }
    auto c = tex::convert_texture(h, surf, 1);  // hd2gc.convert_texture(h, gtx, surf, 1, report, label)
    if (!c) return std::unexpected(c.error());
    if (t.raw.size() < 0x40) return bad("Pscene206 texture " + latin1_to_utf8(t.name) + ": block shorter than its header");
    // timg = hd2gc.patch_header(t['raw'][0x20:0x40], fmt, w, hh, nm, 0x20)
    const Bytes timg = tex::patch_header(View(t.raw).subspan(0x20, 0x20), c->format, c->width, c->height, c->levels, 0x20);
    auto blk = tex_block(View(t.raw).subspan(0, 0x20), timg, c->pixels);
    if (!blk) return std::unexpected(blk.error());
    HdBlock out;
    out.block = std::move(*blk);
    // "%s: %s %dx%d mips=%d <- GTX %s %dx%d level %d (%d levels)" (label 'Pscene206 tex %s' % t['name'])
    out.report = fmt("Pscene206 tex %s: %s %ux%u mips=%u <- GTX %s %ux%u level %d (%u levels)",
                     latin1_to_utf8(t.name).c_str(), gx_fmt_name(c->format).c_str(), c->width, c->height, c->levels,
                     tex::gx2_format_name(surf.format).c_str(), surf.width, surf.height, c->level0, c->levels);
    return out;
}

}  // namespace

// particles.build_scene(gc200, hd206, hd_gtx, need, budget) -> (bytes, report lines, stats)
Result<Bytes> build_scene(View gc200, View hd206, View hd_gtx, std::span<const u16> need, u64 budget,
                          std::vector<std::string>* report_out, ParticleStats* stats_out, std::stop_token stop) {
    auto Gr = parse_jpc(gc200);
    if (!Gr) return std::unexpected(Gr.error());
    auto Hr = parse_jpc(hd206);
    if (!Hr) return std::unexpected(Hr.error());
    const Jpc& G = *Gr;
    const Jpc& H = *Hr;
    std::map<u16, std::size_t> gres;  // {r['id']: r for r in G['res']} (last wins)
    for (std::size_t i = 0; i < G.res.size(); ++i) gres[G.res[i].id] = i;
    auto surfaces = tex::gtx_surfaces(hd_gtx);  // hdpack.gtx_surfaces(hd_gtx)['surfaces']
    if (!surfaces) return std::unexpected(surfaces.error());
    if (surfaces->size() != H.tex.size())
        return bad(fmt("Pscene206: %zu JPC textures but %zu GTX surfaces", H.tex.size(), surfaces->size()));
    std::map<std::string, std::size_t> gtex_by_name;  // {t['name']: t for t in G['tex']} (last wins)
    for (std::size_t i = 0; i < G.tex.size(); ++i) gtex_by_name[G.tex[i].name] = i;
    std::vector<bool> needed(0x10000, false);
    for (u16 id : need) needed[id] = true;

    struct Pick {
        const JpcResource* r;
        bool gc;
        std::vector<std::string> names;
    };
    std::vector<Pick> picked;
    std::vector<std::string> tex_names;
    std::map<std::string, bool> src_gc;  // src_kind: true = 'GC', false = 'HD'
    u32 differing = 0;
    for (const JpcResource& r : H.res) {  // order = [r for r in H['res'] if r['id'] in need]
        if (!needed[r.id]) continue;
        Pick pk;
        if (auto it = gres.find(r.id); it != gres.end()) {
            const JpcResource& g = G.res[it->second];
            for (u16 i : g.tdb) {
                if (i >= G.tex.size()) return bad(fmt("GC resource %04x references texture %u of %zu", g.id, i, G.tex.size()));
                pk.names.push_back(G.tex[i].name);
            }
            // g['raw'][:8] != r['raw'][:8] or len(g['raw']) != len(r['raw']) (equal lengths: equal-length heads)
            const auto n8 = static_cast<std::ptrdiff_t>(std::min<std::size_t>(8, g.raw.size()));
            if (g.raw.size() != r.raw.size() || !std::equal(g.raw.begin(), g.raw.begin() + n8, r.raw.begin()))
                ++differing;
            pk.r = &g;
            pk.gc = true;
        } else {
            for (u16 i : r.tdb) {
                if (i >= H.tex.size()) return bad(fmt("HD resource %04x references texture %u of %zu", r.id, i, H.tex.size()));
                pk.names.push_back(H.tex[i].name);
            }
            pk.r = &r;
            pk.gc = false;
        }
        for (const std::string& n : pk.names)
            if (!src_gc.contains(n)) {
                tex_names.push_back(n);
                src_gc[n] = gtex_by_name.contains(n);
            }
        picked.push_back(std::move(pk));
    }
    std::map<std::string, std::size_t> hd_index;  // {t['name']: i for i, t in enumerate(H['tex'])} (last wins)
    for (std::size_t i = 0; i < H.tex.size(); ++i) hd_index[H.tex[i].name] = i;
    std::map<std::string, u32> new_index;
    for (std::size_t i = 0; i < tex_names.size(); ++i) new_index[tex_names[i]] = static_cast<u32>(i);
    std::vector<Bytes> resources;
    for (const Pick& pk : picked) {
        Bytes raw = pk.r->raw;
        for (std::size_t k = 0; k < pk.names.size(); ++k) {  // struct.pack_into('>H', raw, r['tdb_off'] + 2 * k, ...)
            const u64 at = u64(*pk.r->tdb_off) + 2 * k;      // names non-empty => tdb_off is set
            if (at + 2 > raw.size()) return bad("TDB1 index outside the resource");
            const u32 v = new_index[pk.names[k]];
            if (v > 0xFFFF) return bad("texture index too large");
            io::put16(raw, static_cast<std::size_t>(at), static_cast<u16>(v));
        }
        resources.push_back(std::move(raw));
    }

    std::vector<std::string> report;
    std::set<std::string> half;
    // the converted HD blocks, by (name, half): blocks() gives the same bytes at every call
    std::map<std::pair<std::string, bool>, HdBlock> cache;
    auto blocks = [&]() -> Result<std::vector<const Bytes*>> {
        std::vector<const Bytes*> out;
        for (const std::string& n : tex_names) {
            if (src_gc[n]) {
                out.push_back(&G.tex[gtex_by_name[n]].raw);
                continue;
            }
            if (stop.stop_requested()) return fail("stopped", "cave particles");
            const bool h = half.contains(n);
            auto key = std::make_pair(n, h);
            auto it = cache.find(key);
            if (it == cache.end()) {
                const std::size_t i = hd_index.at(n);
                auto b = convert_hd_texture(H.tex[i], (*surfaces)[i], h);
                if (!b) return std::unexpected(b.error());
                it = cache.emplace(key, std::move(*b)).first;
            }
            report.push_back(it->second.report);
            out.push_back(&it->second.block);
        }
        return out;
    };
    auto make = [&](const std::vector<const Bytes*>& tb) -> Result<Bytes> {
        std::vector<Bytes> texs;
        texs.reserve(tb.size());
        for (const Bytes* b : tb) texs.push_back(*b);
        return build_jpc(resources, texs);
    };

    auto tb = blocks();
    if (!tb) return std::unexpected(tb.error());
    auto data = make(*tb);
    if (!data) return std::unexpected(data.error());
    // over budget: halve the largest HD-converted textures first, then stop (GC pixels are never touched)
    while (data->size() > budget) {
        // cands = sorted(((len(b), n) for b, n in zip(tb, tex_names) if src_kind[n] == 'HD' and n not in half
        //                 and len(b) > 0x40 + 512), reverse=True); cands[0] = the largest (len, name)
        const std::string* best = nullptr;
        u64 best_len = 0;
        for (std::size_t k = 0; k < tex_names.size(); ++k) {
            const std::string& n = tex_names[k];
            const u64 len = (*tb)[k]->size();
            if (src_gc[n] || half.contains(n) || len <= 0x40 + 512) continue;
            if (!best || len > best_len || (len == best_len && n > *best)) {
                best = &n;
                best_len = len;
            }
        }
        if (!best) break;
        half.insert(*best);
        report.push_back(fmt("Pscene206: %s re-encoded at half size (file %zu B > budget %llu B)",
                             latin1_to_utf8(*best).c_str(), data->size(), static_cast<unsigned long long>(budget)));
        tb = blocks();
        if (!tb) return std::unexpected(tb.error());
        data = make(*tb);
        if (!data) return std::unexpected(data.error());
    }
    ParticleStats st;
    st.resources = static_cast<u32>(resources.size());
    for (const Pick& pk : picked) (pk.gc ? st.from_gc : st.from_hd) += 1;
    st.textures = static_cast<u32>(tex_names.size());
    for (const std::string& n : tex_names) (src_gc[n] ? st.tex_gc : st.tex_hd) += 1;
    st.halved.assign(half.begin(), half.end());  // sorted(half)
    for (const std::string& n : st.halved) st.halved_index.push_back(static_cast<u32>(hd_index.at(n)));
    std::sort(st.halved_index.begin(), st.halved_index.end());
    st.size = data->size();
    st.budget = budget;
    st.shared_header_differs = differing;
    st.hd_total = static_cast<u32>(H.res.size());
    st.gc_total = static_cast<u32>(G.res.size());
    report.push_back(fmt("Pscene206: %u resources (%u from GC Pscene200, %u HD-only) of HD %u, %u textures (%u GC, %u HD), "
                         "%llu bytes (budget %llu)",
                         st.resources, st.from_gc, st.from_hd, st.hd_total, st.textures, st.tex_gc, st.tex_hd,
                         static_cast<unsigned long long>(st.size), static_cast<unsigned long long>(budget)));
    if (report_out) report_out->insert(report_out->end(), report.begin(), report.end());
    if (stats_out) *stats_out = std::move(st);
    return std::move(*data);
}

// cavebuild.build, scene particles part: universe, needed_ids (facts), build_scene
Result<Bytes> build_particles(View gc_pscene200, View hd_pscene206, View hd_gtx_gz, u64 budget,
                              std::vector<std::string>* report, ParticleStats* stats, std::stop_token stop) {
    auto gtx = io::read_any(hd_gtx_gz, stop);  // hdpack.read_any(...gtx.gz)
    if (!gtx) return std::unexpected(gtx.error());
    auto H = parse_jpc(hd_pscene206);  // universe = {r['id'] for r in particles.parse(hd206)['res']}
    if (!H) return std::unexpected(H.error());
    std::vector<bool> universe(0x10000, false);
    for (const JpcResource& r : H->res) universe[r.id] = true;
    std::vector<u16> need;  // set(facts.CAVE_PARTICLE_IDS) & set(universe)
    for (u16 id : facts::kCaveParticleIds)
        if (universe[id] && std::find(need.begin(), need.end(), id) == need.end()) need.push_back(id);
    std::vector<std::string> rep;
    rep.push_back(fmt("particles: %zu ids from the built-in table (facts.CAVE_PARTICLE_IDS; no source scan)", need.size()));
    ParticleStats st;
    auto data = build_scene(gc_pscene200, hd_pscene206, *gtx, need, budget, &rep, &st, stop);
    if (!data) return std::unexpected(data.error());
    st.need = static_cast<u32>(need.size());
    if (report) report->insert(report->end(), rep.begin(), rep.end());
    if (stats) *stats = std::move(st);
    return data;
}

Result<Bytes> build_particles(View gc_pscene200, View hd_pscene206, View hd_gtx_gz, u64 budget,
                              std::vector<std::string>* report) {
    return build_particles(gc_pscene200, hd_pscene206, hd_gtx_gz, budget, report, nullptr, {});
}

}  // namespace hdf::cave
