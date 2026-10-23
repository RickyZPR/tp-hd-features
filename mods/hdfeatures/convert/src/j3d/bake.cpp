// Module J: cave/bake_los.py (bake_model): the TP HD Cave of Shadows room placement (los.bin x, y, z, angleY)
// baked into a room model's vertices, normals and bounding boxes.
//
//   VTX1 POS        p' = RotY(angle) * p + T
//   VTX1 NRM / NBT  (x, z) rotated
//   SHP1 / JNT1     bbox and radius from the transformed POS entries each shape's display lists use
//
// Float order is Python's: rot(x, z, c, s) = (x * c + z * s, (-x) * s + z * c), then + T; quantized attributes
// are int(round(v * q)) (io::py_round_int, half to even); struct '>f' packs are double -> float casts. The angle
// table is the exact (round(cos(a), 9), round(sin(a), 9)) of the 4 angles los.bin uses, signed zero included.
// The radius squares with x * x where Python writes x ** 2 (CRT pow): IEEE multiplication is correctly rounded on
// every PC, so the bytes don't depend on the player's UCRT pow. Equal to the spec on all 43 room models and the
// test vectors (wave2_J_verify L2). Non-finite vertex data is outside the spec's exactness (the sign of a NaN can
// differ, J verify L1); hash-checked inputs exclude it (all 617,904 float POS/NRM/NBT values are finite).
#include "j3d_internal.hpp"

#include <algorithm>
#include <cmath>

namespace hdf::j3d {

namespace detail {

namespace {

// bake_los.COMP: struct code and size per VTX1 component type
struct Comp {
    char code;  // 'B' 'b' 'H' 'h' 'f'
    u32 size;
};
Comp comp_of(u32 comp) {
    switch (comp) {
        case 0: return {'B', 1};
        case 1: return {'b', 1};
        case 2: return {'H', 2};
        case 3: return {'h', 2};
        case 4: return {'f', 4};
        default: bad("COMP[" + std::to_string(comp) + "]");  // KeyError
    }
}

// bake_los.ATTR_ORDER.index(at) for POS, NRM, NBT
std::size_t attr_order_index(u32 at) { return at == 9 ? 0 : at == 10 ? 1 : 2; }

struct Fmt {
    u32 ct = 0, comp = 0;
    u8 frac = 0;
};
// fmts dict: attribute -> (ct, comp, frac); a repeated attribute keeps the last row (dict assignment)
struct Fmts {
    std::vector<std::pair<u32, Fmt>> rows;
    const Fmt* find(u32 at) const {
        for (const auto& [a, f] : rows)
            if (a == at) return &f;
        return nullptr;
    }
    const Fmt& at(u32 at) const {
        const Fmt* f = find(at);
        if (!f) bad("fmts[" + std::to_string(at) + "]");  // KeyError
        return *f;
    }
    void set(u32 at, const Fmt& f) {
        for (auto& [a, g] : rows)
            if (a == at) {
                g = f;
                return;
            }
        rows.emplace_back(at, f);
    }
};

// bake_model.esize(at)
u32 esize(const Fmts& fmts, u32 at) {
    const Fmt& f = fmts.at(at);
    if (at == 11 || at == 12) {
        switch (f.comp) {
            case 0: return 2;
            case 1: return 3;
            case 2: return 4;
            case 3: return 2;
            case 4: return 3;
            case 5: return 4;
            default: bad("colour comp " + std::to_string(f.comp));  // KeyError
        }
    }
    u32 n;
    if (at == 9) n = f.ct == 0 ? 2 : 3;
    else if (at == 10) n = f.ct == 0 ? 3 : 9;
    else if (at == 25) n = 9;
    else n = f.ct == 0 ? 1 : 2;
    return n * comp_of(f.comp).size;
}

// (round(math.cos(a), 9), round(math.sin(a), 9)) with a = ang * math.pi / 32768.0
struct CosSin {
    double c, s;
};
CosSin angle_table(int ang) {
    switch (ang) {
        case -0x8000: return {-1.0, -0.0};  // sin(-pi) = -1.2246e-16 rounds to -0.0
        case -0x4000: return {0.0, -1.0};   // cos(-pi/2) = 6.1e-17 rounds to 0.0
        case 0: return {1.0, 0.0};
        case 0x4000: return {0.0, 1.0};
        default: bad("bake angle " + std::to_string(ang) + " is not one of -0x8000, -0x4000, 0, 0x4000");
    }
}

// bake_los.rot(x, z, c, s)
inline void rot(double x, double z, double c, double s, double& ox, double& oz) {
    ox = x * c + z * s;
    oz = -x * s + z * c;
}

// struct.unpack_from('>' + fc, d, off) as a Python number (int for the integer codes, float for 'f')
double read_comp(View d, u64 off, char code) {
    switch (code) {
        case 'B': return static_cast<double>(u8_at(d, off));
        case 'b': return static_cast<double>(static_cast<i8>(u8_at(d, off)));
        case 'H': return static_cast<double>(u16_at(d, off));
        case 'h': return static_cast<double>(static_cast<i16>(u16_at(d, off)));
        default: return static_cast<double>(f32_at(d, off));
    }
}

void write_int(Bytes& d, u64 off, char code, i64 v) {
    switch (code) {
        case 'B':
        case 'b': put_u8(d, off, static_cast<u8>(v)); return;
        default: put_u16(d, off, static_cast<u16>(v)); return;
    }
}

struct BBox {
    std::array<double, 3> mn{}, mx{};
    double r = 0;
};

}  // namespace

// bake_los.bake_model(d, T, ang) -> (bytes, mn, mx, len(used[9]))
Baked bake_impl(View src, const std::array<float, 3>& t_in, int ang) {
    const CosSin cs = angle_table(ang);
    const double c = cs.c, s = cs.s;
    const std::array<double, 3> T{static_cast<double>(t_in[0]), static_cast<double>(t_in[1]),
                                  static_cast<double>(t_in[2])};
    Bytes d(src.begin(), src.end());
    const std::vector<Section> secs = sections_impl(d);
    const Section& vtx = section_of(secs, "VTX1");
    const u64 vo = vtx.offset;
    const u32 fmt_off = u32_at(d, vo + 8);
    if (!has(d, vo + 0xC, 13 * 4)) bad("VTX1 array offsets cut");  // struct.unpack_from('>13I', ...)
    std::array<u32, 13> arr_offs{};
    for (u32 k = 0; k < 13; ++k) arr_offs[k] = u32_at(d, vo + 0xC + 4 * k);
    Fmts fmts;
    for (u64 o = vo + fmt_off;; o += 0x10) {
        if (!has(d, o, 13)) bad("VTX1 format list cut");  // struct.unpack_from('>IIIB', d, o)
        const u32 at = u32_at(d, o);
        if (at == 0xFF) break;
        fmts.set(at, Fmt{u32_at(d, o + 4), u32_at(d, o + 8), u8_at(d, o + 12)});
    }

    // walk SHP1 display lists: per-shape referenced POS indices
    const Section& shp = section_of(secs, "SHP1");
    const u64 so = shp.offset;
    const u16 nshp = u16_at(d, so + 8);
    if (!has(d, so + 0xC, 8 * 4)) bad("SHP1 offsets cut");
    const u32 init_off = u32_at(d, so + 0xC), desc_off = u32_at(d, so + 0x18), dl_off = u32_at(d, so + 0x20),
              drawinit_off = u32_at(d, so + 0x28);
    std::vector<std::vector<u32>> shape_pos;  // sorted, unique (sets in Python)
    shape_pos.reserve(nshp);
    std::array<std::vector<u32>, 3> used;     // POS, NRM, NBT (sets in Python)
    bool other_mtx = false;
    std::vector<int> bad_mtx;
    for (u32 i = 0; i < nshp; ++i) {
        const u64 si = so + init_off + u64{i} * 0x28;
        if (!has(d, si, 10)) bad("SHP1 shape cut");  // '>BBHHHH'
        const u8 mtxtype = u8_at(d, si);
        const u16 ngrp = u16_at(d, si + 2), desc_idx = u16_at(d, si + 4), di = u16_at(d, si + 8);
        if (mtxtype != 0) {
            other_mtx = true;
            if (std::find(bad_mtx.begin(), bad_mtx.end(), int{mtxtype}) == bad_mtx.end()) bad_mtx.push_back(mtxtype);
        }
        std::vector<std::pair<u32, u32>> desc;
        for (u64 q = so + desc_off + desc_idx;; q += 8) {
            if (!has(d, q, 8)) bad("SHP1 vertex descriptor cut");
            const u32 at = u32_at(d, q), ty = u32_at(d, q + 4);
            if (at == 0xFF) break;
            desc.emplace_back(at, ty);
        }
        std::vector<u32> pos_here;
        for (u32 g = 0; g < ngrp; ++g) {
            const u64 gi = so + drawinit_off + (u64{di} + g) * 8;
            if (!has(d, gi, 8)) bad("SHP1 draw table cut");
            const u32 dsz = u32_at(d, gi), doff = u32_at(d, gi + 4);
            u64 p = so + dl_off + doff;
            const u64 end = p + dsz;
            while (p < end) {
                const u8 op = u8_at(d, p);
                if (op == 0) {
                    ++p;
                    continue;
                }
                const u8 prim = op & 0xF8;
                if (prim < 0x80 || prim > 0xB8) bad("bad GX opcode in shape " + std::to_string(i));
                const u16 n = u16_at(d, p + 1);
                p += 3;
                for (u32 v = 0; v < n; ++v) {
                    for (const auto& [at, ty] : desc) {
                        if (ty == 1) {
                            if (at == 9 || at == 10 || at == 25) bad("direct POS/NRM not supported");
                            p += at <= 8 ? 1 : esize(fmts, at);
                        } else {
                            u32 idx;
                            if (ty == 2) {
                                idx = u8_at(d, p);
                                p += 1;
                            } else {
                                idx = u16_at(d, p);
                                p += 2;
                            }
                            if (at == 9 || at == 10 || at == 25) used[attr_order_index(at)].push_back(idx);
                            if (at == 9) pos_here.push_back(idx);
                        }
                    }
                }
            }
        }
        std::sort(pos_here.begin(), pos_here.end());
        pos_here.erase(std::unique(pos_here.begin(), pos_here.end()), pos_here.end());
        shape_pos.push_back(std::move(pos_here));
    }
    if (other_mtx) {
        std::sort(bad_mtx.begin(), bad_mtx.end());
        std::string list;
        for (int m : bad_mtx) list += (list.empty() ? "" : ", ") + std::to_string(m);
        bad("non-normal shape matrix types [" + list + "] (billboards) - not baked");
    }
    for (auto& u : used) {
        std::sort(u.begin(), u.end());
        u.erase(std::unique(u.begin(), u.end()), u.end());
    }

    // transform arrays (POS, NRM, NBT), every used index in ascending order
    std::vector<std::array<double, 3>> newpos;  // parallel to used[0] (POS)
    std::vector<u8> have_pos;                   // newpos filled for used[0][k]
    for (const u32 at : {9u, 10u, 25u}) {
        const Fmt* fp = fmts.find(at);
        if (!fp) continue;
        const Fmt f = *fp;
        const Comp cp = comp_of(f.comp);
        const u64 base = vo + arr_offs[attr_order_index(at)];
        const u32 es = esize(fmts, at);
        const u32 ncomp = es / cp.size;
        const double q = f.comp != 4 ? std::ldexp(1.0, f.frac) : 1.0;  // (1 << frac) if comp != 4 else 1
        const std::vector<u32>& idxs = used[attr_order_index(at)];
        if (at == 9) {
            newpos.assign(idxs.size(), {});
            have_pos.assign(idxs.size(), 0);
        }
        std::vector<double> vals(ncomp), out(ncomp);
        for (std::size_t k = 0; k < idxs.size(); ++k) {
            const u64 off = base + u64{idxs[k]} * es;
            if (!has(d, off, u64{ncomp} * cp.size)) bad("vertex array entry cut");
            for (u32 j = 0; j < ncomp; ++j) {
                const double raw = read_comp(d, off + u64{j} * cp.size, cp.code);
                vals[j] = f.comp != 4 ? raw / q : raw;  // v / q: exact (q is a power of two)
            }
            if (at == 9) {
                // esize gives POS 2 or 3 components; x, y, z = vals[0], vals[1], vals[2] raises for 2
                if (ncomp != 3) bad("POS entry has fewer than 3 components");
                double x, z;
                rot(vals[0], vals[2], c, s, x, z);
                out[0] = x + T[0];
                out[1] = vals[1] + T[1];
                out[2] = z + T[2];
                newpos[k] = {out[0], out[1], out[2]};
                have_pos[k] = 1;
            } else {
                out = vals;
                for (u32 j = 0; j < ncomp; j += 3) {
                    if (j + 2 >= ncomp) bad("NRM/NBT entry is not a multiple of 3 components");  // vals[k + 2]
                    rot(vals[j], vals[j + 2], c, s, out[j], out[j + 2]);
                }
            }
            if (f.comp != 4) {
                i64 lo = 0, hi = 0;
                switch (cp.code) {
                    case 'B': lo = 0; hi = 255; break;
                    case 'b': lo = -128; hi = 127; break;
                    case 'H': lo = 0; hi = 65535; break;
                    default: lo = -32768; hi = 32767; break;
                }
                std::vector<i64> iv(out.size());
                for (std::size_t j = 0; j < out.size(); ++j) {
                    const double v = out[j] * q;
                    if (!std::isfinite(v)) bad("cannot round a non-finite value");  // int(round(inf/nan))
                    iv[j] = io::py_round_int(v);
                }
                for (const i64 v : iv)
                    if (v < lo || v > hi)
                        bad("quantized attr " + std::to_string(at) + " overflows after transform (frac " +
                            std::to_string(f.frac) + ")");
                for (u32 j = 0; j < ncomp; ++j) write_int(d, off + u64{j} * cp.size, cp.code, iv[j]);
            } else {
                for (u32 j = 0; j < ncomp; ++j) put_f32(d, off + u64{j} * 4, out[j]);
            }
        }
    }

    // bbox(idxs): mn, mx, r = max |p|
    auto pos_of = [&](u32 idx) -> const std::array<double, 3>& {
        auto it = std::lower_bound(used[0].begin(), used[0].end(), idx);
        const std::size_t k = static_cast<std::size_t>(it - used[0].begin());
        if (it == used[0].end() || *it != idx || k >= have_pos.size() || !have_pos[k])
            bad("newpos[" + std::to_string(idx) + "]");  // KeyError
        return newpos[k];
    };
    auto bbox = [&](const std::vector<u32>& idxs) {
        if (idxs.empty()) bad("min() arg is an empty sequence");
        BBox b;
        bool first = true;
        for (const u32 i : idxs) {
            const std::array<double, 3>& p = pos_of(i);
            const double r = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);  // x ** 2 (see the top)
            if (first) {
                b.mn = p;
                b.mx = p;
                b.r = r;
                first = false;
                continue;
            }
            // Python's min/max keep the first of equal values. Every position is p + T with T read from los.bin
            // (never -0.0, VERIFIED), so no coordinate is -0.0 and equal values are equal bits: order does not matter.
            for (std::size_t k = 0; k < 3; ++k) {
                if (p[k] < b.mn[k]) b.mn[k] = p[k];
                if (p[k] > b.mx[k]) b.mx[k] = p[k];
            }
            if (r > b.r) b.r = r;
        }
        return b;
    };
    auto put_box = [&](u64 off, const BBox& b) {  // struct.pack_into('>f3f3f', d, off, r, *(mn + mx))
        if (!has(d, off, 28)) bad("bbox write past the end");
        put_f32(d, off, b.r);
        for (std::size_t k = 0; k < 3; ++k) put_f32(d, off + 4 + 4 * k, b.mn[k]);
        for (std::size_t k = 0; k < 3; ++k) put_f32(d, off + 16 + 4 * k, b.mx[k]);
    };
    for (u32 i = 0; i < nshp; ++i) {
        if (shape_pos[i].empty()) continue;
        put_box(so + init_off + u64{i} * 0x28 + 0xC, bbox(shape_pos[i]));
    }
    const Section& jnt = section_of(secs, "JNT1");
    const u64 jo = jnt.offset;
    const u16 nj = u16_at(d, jo + 8);
    if (nj != 1) bad("expected 1 joint, got " + std::to_string(nj));
    const u32 eo = u32_at(d, jo + 0xC);
    std::vector<u32> all;  // set().union(*shape_pos)
    for (const auto& sp : shape_pos) all.insert(all.end(), sp.begin(), sp.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    const BBox jb = bbox(all);
    put_box(jo + eo + 0x24, jb);

    Baked r;
    r.data = std::move(d);
    r.min = jb.mn;
    r.max = jb.mx;
    r.vertices = static_cast<u32>(used[0].size());
    return r;
}

}  // namespace detail

using namespace detail;

Result<Baked> bake_los(View bmd, const std::array<float, 3>& t, int angle) {
    try {
        return bake_impl(bmd, t, angle);
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
}

}  // namespace hdf::j3d
