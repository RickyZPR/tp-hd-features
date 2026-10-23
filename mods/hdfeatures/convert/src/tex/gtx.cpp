// Module T: GTX (Gfx2) surface walk and GMX2 section walk. Port of formats/hdpack.py gtx_surfaces, gmx_sections.
// Every read is bounds-checked: where Python's struct.unpack_from would raise, this returns Error{"fmt.tex"}.
#include "hdf/tex.hpp"

#include "hdf/io.hpp"

#include <cstdio>

namespace hdf::tex {

namespace {

bool fits(View d, u64 o, u64 n) { return o <= d.size() && n <= d.size() - o; }

}  // namespace

// hdpack.GX2_FMT
std::string gx2_format_name(u32 f) {
    switch (f) {
        case 0x01: return "R8_UNORM";
        case 0x07: return "R8G8_UNORM";
        case 0x08: return "R5G6B5";
        case 0x0A: return "R5G5B5A1";
        case 0x0B: return "R4G4B4A4";
        case 0x1A: return "R8G8B8A8_UNORM";
        case 0x41A: return "R8G8B8A8_SRGB";
        case 0x31: return "BC1_UNORM";
        case 0x431: return "BC1_SRGB";
        case 0x32: return "BC2_UNORM";
        case 0x432: return "BC2_SRGB";
        case 0x33: return "BC3_UNORM";
        case 0x433: return "BC3_SRGB";
        case 0x34: return "BC4_UNORM";
        case 0x234: return "BC4_SNORM";
        case 0x35: return "BC5_UNORM";
        case 0x235: return "BC5_SNORM";
        case 0x02: return "R4G4_UNORM";
        case 0x05: return "R16_UNORM";
        case 0x19: return "R10G10B10A2";
        case 0x80E: return "R32_FLOAT";
        default: {
            char b[24];
            std::snprintf(b, sizeof b, "0x%x", f);
            return b;
        }
    }
}

// hdpack.gtx_surfaces(buf)
Result<Gtx> gtx_read(View buf) {
    if (buf.size() < 4 || buf[0] != 'G' || buf[1] != 'f' || buf[2] != 'x' || buf[3] != '2')
        return fail("fmt.tex", "not Gfx2");
    if (!fits(buf, 4, 20)) return fail("fmt.tex", "Gfx2 header cut");
    Gtx g;
    const u32 hsz = io::be32(buf, 4);
    g.major = io::be32(buf, 8);
    g.minor = io::be32(buf, 12);
    g.gpu = io::be32(buf, 16);
    u64 o = hsz;
    const u32 t_surf = g.major >= 7 ? 0x0Bu : 0x0Au;
    const u32 t_img = t_surf + 1, t_mip = t_surf + 2;
    GtxSurface* cur = nullptr;
    while (o + 0x20 <= buf.size()) {
        if (!(buf[o] == 'B' && buf[o + 1] == 'L' && buf[o + 2] == 'K' && buf[o + 3] == '{')) break;
        const u32 bh = io::be32(buf, o + 4);
        const u32 btype = io::be32(buf, o + 16);
        const u32 bsize = io::be32(buf, o + 20);
        const u64 d0 = o + bh;
        ++g.blocks;
        if (btype == t_surf) {
            // struct.unpack_from(">16I", buf, d0) and (">13I", buf, d0 + 0x40)
            if (!fits(buf, d0, 0x74)) return fail("fmt.tex", "surface block cut");
            GtxSurface s;
            u32 v[16];
            for (int k = 0; k < 16; ++k) v[k] = io::be32(buf, d0 + 4u * k);
            s.dim = v[0];
            s.width = v[1];
            s.height = v[2];
            s.depth = v[3];
            s.mips = v[4];
            s.format = v[5];
            s.aa = v[6];
            s.use = v[7];
            s.image_size = v[8];
            s.mip_size = v[10];
            s.tile_mode = v[12];
            s.swizzle = v[13];
            s.alignment = v[14];
            s.pitch = v[15];
            if (bsize >= 0x88) {
                // buf[d0 + 0x84:d0 + 0x88]; a shorter slice would make bytes.fromhex give < 4 entries
                if (!fits(buf, d0 + 0x84, 4)) return fail("fmt.tex", "compSel cut");
                for (int k = 0; k < 4; ++k) s.comp_sel[k] = buf[d0 + 0x84 + k];
                s.has_comp_sel = true;
            }
            for (int k = 0; k < 13; ++k) s.mip_offsets[k] = io::be32(buf, d0 + 0x40 + 4u * k);
            s.gtx = buf;
            g.surfaces.push_back(s);
            cur = &g.surfaces.back();
        } else if (btype == t_img && cur != nullptr) {
            cur->image_block = bsize;
            cur->image_off = d0;
            cur->has_image = true;
            const u64 b = d0 < buf.size() ? d0 : buf.size();
            const u64 n = b + bsize <= buf.size() ? bsize : buf.size() - b;
            cur->image = buf.subspan(static_cast<std::size_t>(b), static_cast<std::size_t>(n));
        } else if (btype == t_mip && cur != nullptr) {
            cur->mip_block = bsize;
            cur->mip_off = d0;
            cur->has_mip = true;
            const u64 b = d0 < buf.size() ? d0 : buf.size();
            const u64 n = b + bsize <= buf.size() ? bsize : buf.size() - b;
            cur->mip_data = buf.subspan(static_cast<std::size_t>(b), static_cast<std::size_t>(n));
        } else if (btype == 1) {
            break;
        }
        // wave2_T_verify L1: a block with header size 0 and data size 0 never moves o (Python loops forever, and a
        // surface block would grow the list without limit). Hash-checked inputs never have one.
        if (u64(bh) + bsize == 0) return fail("fmt.tex", "Gfx2 block with no size");
        o = d0 + bsize;
    }
    return g;
}

Result<std::vector<GtxSurface>> gtx_surfaces(View pack_member) {
    auto g = gtx_read(pack_member);
    if (!g) return std::unexpected(g.error());
    return std::move(g->surfaces);
}

// hdpack.gmx_sections(buf)
Result<std::vector<GmxSection>> gmx_sections(View buf) {
    if (buf.size() < 4 || buf[0] != 'G' || buf[1] != 'M' || buf[2] != 'X' || buf[3] != '2')
        return fail("fmt.tex", "not GMX2");
    std::vector<GmxSection> secs;
    u64 o = 8;
    while (o + 8 <= buf.size()) {
        GmxSection s;
        for (int k = 0; k < 4; ++k) {
            const u8 c = buf[o + k];
            s.tag += c < 0x80 ? static_cast<char>(c) : '?';  // .decode("ascii", "replace") (U+FFFD shown as '?')
        }
        s.offset = static_cast<u32>(o);
        s.size = io::be32(buf, o + 4);
        secs.push_back(s);
        if (s.tag == "ENDX" || s.size == 0) break;
        o += s.size;
    }
    return secs;
}

}  // namespace hdf::tex
