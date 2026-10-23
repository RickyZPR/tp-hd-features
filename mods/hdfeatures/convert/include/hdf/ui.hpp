// hdf_convert: stamp images, stamp book, pre-entry screen, amiibo screens (module U "ui"; src/ui/*).
//
// Ports: pipeline/stampbuild.py (_gtx_png, _stamp_rows, PNG part of build), gen_stamp_table.build_icons,
// pipeline/stampbook.py build half (Source, Blo, _place, _resrefs, build), pipeline/cosconfirm.py build half
// (_timg, _source_info, build), pipeline/hdtext.py (build_amiibo, parse_blo, _gtx_rgba, _tmpk_find, _name_tab,
// _res_ref, _mat1). The manifests (book.json, confirm.json, stamps.json) and the 21 unused PNGs are on the
// drop list and are not made. layout.json uses cp932 for the 3 placeholder strings (7.3).
//
// Wave 2 (module U) additions, all additive: Options, UiOut::dropped and the build_* overloads that take Options
// (the CLI's oracle test uses them to check the dropped PNGs too). _stamp_rows and _source_info only feed the
// dropped manifests, so they are not ported (see research/dropin/wave2_U.md).
// Threads: each call is independent and keeps no state; the four may run in parallel on one Source if its read()
// is thread-safe (core.hpp). Peak memory (wave2_U_verify L1): build_stampbook reads res/Object/fileSel.pack.gz
// (24,238,112 B inflated, the largest these read), and build_confirm keeps every pack it loads until it ends
// (saveres.pack.gz, 19.9 MB, plus up to 3 fallbacks). Measured peak working set, one build per fresh process:
// stamps 36 MB, stampbook 68 MB, confirm 84 MB, amiibo 49 MB; all 4 in parallel 150-155 MB (budget 400 MB).
#pragma once

#include "hdf/core.hpp"

#include <string>
#include <utility>
#include <vector>

namespace hdf::ui {

// An image to store as PNG. The driver encodes it (io::png_encode_rgba) and hashes the bytes.
struct Png {
    std::string rel;   // e.g. "res/stamps/stamps/stamp00.png"
    u32 w = 0, h = 0;
    Bytes rgba;
};

struct UiOut {
    std::vector<Png> pngs;
    std::vector<std::pair<std::string, Bytes>> files;  // rel -> bytes
    std::vector<Png> dropped;                          // (U) only with Options::dropped: PNGs on the drop list
};

// (U) Diagnostics for the CLI. The driver uses the defaults.
struct Options {
    bool dropped = false;  // also return the dropped PNGs (stamp_background.png, the 19 unused amiibo PNGs)
};

// hd: CheckedSource over the 136 HD inputs.
Result<UiOut> build_stamps(const Source& hd, std::stop_token stop);     // 52 PNGs + 50 icons (stamp_icons/*.bti)
Result<UiOut> build_stampbook(const Source& hd, std::stop_token stop);  // stampResR_gc.arc + 4 loose BTIs
Result<UiOut> build_confirm(const Source& hd, std::stop_token stop);    // cave/confirm/cosconfirm_gc.arc
Result<UiOut> build_amiibo(const Source& hd, std::stop_token stop);     // cave/amiibo/layout.json + 6 PNGs

// (U) The same with options.
Result<UiOut> build_stamps(const Source& hd, std::stop_token stop, const Options& opt);
Result<UiOut> build_amiibo(const Source& hd, std::stop_token stop, const Options& opt);

}  // namespace hdf::ui
