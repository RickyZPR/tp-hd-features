// Module J: critic/mat4fix.py (fix_j3d) and the parts of critic/matstride.py it uses (_ELEM, _U8, _U16, _MODES,
// _block_info, _counts, material_errors, effective_stride).
//
// HD MAT4 material init entries are 0x14E bytes (a trailing u16 mPolygonOffsetIdx); GC MAT3 entries are 0x14C.
// fix_j3d repacks a block only when its span says 0x14E and its contents (effective_stride) agree, so running it
// on an already repacked model changes nothing but the tag (idempotent).
#include "j3d_internal.hpp"

#include <algorithm>
#include <set>

namespace hdf::j3d {

namespace detail {

namespace {

// matstride._ELEM: J3DMaterialBlock offset table index -> element size
struct Elem {
    int k;
    i64 size;
};
constexpr Elem kElem[] = {{4, 4},  {5, 4},  {6, 1},     {7, 8},  {8, 4},  {9, 0x34}, {10, 1}, {11, 4}, {12, 4},
                          {13, 0x64}, {14, 0x64}, {15, 2}, {16, 4}, {17, 8}, {18, 4}, {19, 1},  {20, 0x14}, {21, 4},
                          {22, 4}, {23, 0x2C}, {24, 8}, {25, 4}, {26, 4}, {27, 1}, {28, 1}, {29, 0x10}};
// matstride._U8: (field offset, table)
constexpr std::pair<u32, int> kU8[] = {{1, 4}, {2, 6}, {3, 10}, {4, 19}, {5, 27}, {6, 26}, {7, 28}};
// matstride._U16: (field offset, count, table)
struct U16Field {
    u32 off;
    u32 n;
    int table;
};
constexpr U16Field kU16[] = {{0x08, 2, 5},   {0x0C, 4, 7},   {0x14, 2, 8},   {0x28, 8, 11},  {0x48, 8, 13},
                             {0x84, 8, 15},  {0x94, 4, 18},  {0xBC, 16, 16}, {0xDC, 4, 17},  {0xE4, 16, 20},
                             {0x104, 16, 21}, {0x124, 4, 22}, {0x144, 1, 23}, {0x146, 1, 24}, {0x148, 1, 25},
                             {0x14A, 1, 29}};

struct BlockInfo {
    u32 size = 0;
    u16 num = 0;
    std::array<u32, 30> offs{};
    i64 uniq = 0;
};

// matstride._block_info(b, o) (also the reads of mat4fix.fix_j3d, in the same order)
BlockInfo block_info(View b, u64 o) {
    BlockInfo bi;
    bi.size = u32_at(b, o + 4);
    bi.num = u16_at(b, o + 8);
    if (!has(b, o + 0xC, 30 * 4)) bad("MAT offsets cut");  // struct.unpack_from('>30I', ...)
    for (u32 k = 0; k < 30; ++k) bi.offs[k] = u32_at(b, o + 0xC + 4 * k);
    const u64 ids = o + bi.offs[1];
    if (!has(b, ids, u64{bi.num} * 2)) bad("MAT index table cut");  // struct.unpack_from('>%dH' % num, ...)
    if (bi.num == 0) bad("MAT block without materials");               // max() of an empty sequence
    u32 mx = 0;
    for (u32 i = 0; i < bi.num; ++i) mx = std::max<u32>(mx, u16_at(b, ids + 2 * u64{i}));
    bi.uniq = i64{mx} + 1;
    return bi;
}

// matstride._counts(offs, size): table index -> number of elements
std::array<i64, 30> counts(const std::array<u32, 30>& offs, u32 size) {
    std::set<u32> nz;
    for (u32 x : offs)
        if (x) nz.insert(x);
    std::array<i64, 30> cnt{};
    for (const Elem& e : kElem) {
        const u32 at = offs[static_cast<std::size_t>(e.k)];
        if (!at) {
            cnt[static_cast<std::size_t>(e.k)] = 0;
            continue;
        }
        // nxt = min([x for x in nz if x > offs[k]] + [size])
        i64 nxt = size;
        auto it = nz.upper_bound(at);
        if (it != nz.end()) nxt = std::min<i64>(nxt, *it);
        cnt[static_cast<std::size_t>(e.k)] = io::floor_div(nxt - i64{at}, e.size);
    }
    return cnt;
}

// matstride.material_errors(b, o, stride) != []: every entry is read (a read past the end raises, as in Python,
// even after an error was found).
bool material_errors(View b, u64 o, u64 stride) {
    const BlockInfo bi = block_info(b, o);
    const std::array<i64, 30> cnt = counts(bi.offs, bi.size);
    const u64 base = o + bi.offs[0];
    bool any = false;
    for (i64 i = 0; i < bi.uniq; ++i) {
        const u64 e = base + static_cast<u64>(i) * stride;
        if (e + 0x14C > o + bi.size) {
            any = true;
            break;
        }
        const u8 mode = u8_at(b, e);
        if (mode < 1 || mode > 0x7F) any = true;  // _MODES = 1..0x7F
        for (const auto& [fo, t] : kU8) {
            const u8 v = u8_at(b, e + fo);
            if (v != 0xFF && i64{v} >= cnt[static_cast<std::size_t>(t)]) any = true;
        }
        for (const U16Field& f : kU16)
            for (u32 k = 0; k < f.n; ++k) {
                const u16 v = u16_at(b, e + f.off + 2 * k);
                if (v != 0xFFFF && i64{v} >= cnt[static_cast<std::size_t>(f.table)]) any = true;
            }
    }
    return any;
}

}  // namespace

// matstride.effective_stride(b, o)
std::optional<u32> effective_stride_impl(View b, u64 o) {
    const BlockInfo bi = block_info(b, o);
    const i64 span = i64{bi.offs[1]} - i64{bi.offs[0]};
    if (bi.uniq == 1) {
        const u64 base = o + bi.offs[0];
        const bool hd_span = 0 <= span - 0x14E && span - 0x14E < 0x20;
        bool trailing_zero = true;  // trailing = b[base + 0x14C:base + 0x14E] if hd_span else b'\0\0'
        if (hd_span) {
            const View t = slice(b, base + 0x14C, base + 0x14E);
            trailing_zero = t.size() == 2 && t[0] == 0 && t[1] == 0;
        }
        if (material_errors(b, o, 0x14C)) return std::nullopt;
        return (hd_span && !trailing_zero) ? 0x14Eu : 0x14Cu;
    }
    const bool ok_c = !material_errors(b, o, 0x14C);
    const bool ok_e = (span >= bi.uniq * 0x14E) && !material_errors(b, o, 0x14E);
    if (ok_c && !ok_e) return 0x14Cu;
    if (ok_e && !ok_c) return 0x14Eu;
    if (ok_c && ok_e) return (0 <= span - bi.uniq * 0x14E && span - bi.uniq * 0x14E < 0x20) ? 0x14Eu : 0x14Cu;
    return std::nullopt;
}

// mat4fix.fix_j3d(d) -> (bytes, changed)
Mat4Fix mat4fix_impl(View d) {
    Mat4Fix r;
    Bytes& b = r.data;
    b.assign(d.begin(), d.end());
    const u32 n = u32_at(b, 0xC);
    u64 o = 0x20;
    for (u32 k = 0; k < n; ++k) {
        const View tag = slice(b, o, o + 4);
        const u32 size = u32_at(b, o + 4);
        const bool mat = tag.size() == 4 && (std::memcmp(tag.data(), "MAT3", 4) == 0 || std::memcmp(tag.data(), "MAT4", 4) == 0);
        if (mat) {
            const BlockInfo bi = block_info(b, o);
            const i64 span = i64{bi.offs[1]} - i64{bi.offs[0]};
            const i64 uniq = bi.uniq;
            const i64 rem = span - uniq * 0x14E;
            if (0 <= rem && rem < 0x20 && span - uniq * 0x14C >= 2 * uniq && effective_stride_impl(b, o) == 0x14Eu) {
                const u64 base = o + bi.offs[0];
                Bytes blob;
                blob.reserve(static_cast<std::size_t>(uniq) * 0x14E);
                for (i64 i = 0; i < uniq; ++i) {
                    const u64 e = base + static_cast<u64>(i) * 0x14E;
                    const View ent = slice(b, e, e + 0x14C);
                    blob.insert(blob.end(), ent.begin(), ent.end());
                }
                blob.insert(blob.end(), static_cast<std::size_t>(2 * uniq), u8{0});
                slice_assign(b, base, base + static_cast<u64>(uniq) * 0x14E, blob);
                ++r.fixed;
            }
            slice_assign(b, o, o + 4, sv_bytes("MAT3"));
        }
        if (size == 0) break;
        o += size;
    }
    return r;
}

}  // namespace detail

using namespace detail;

Result<Mat4Fix> mat4fix(View bmd) {
    try {
        return mat4fix_impl(bmd);
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
}

Result<std::optional<u32>> mat_effective_stride(View bmd, u64 mat_block_offset) {
    try {
        return effective_stride_impl(bmd, mat_block_offset);
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
}

}  // namespace hdf::j3d
