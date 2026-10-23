// Unit tests of module TX (text) on synthetic data only: a made-up font, made-up English texts and message
// graphs. The expected values between the "vectors" markers were made by the Python spec (tools/pipeline/hdtext.py)
// with `python -I -B tools/dev/golden_text.py vectors`; the random generators below repeat that script's.
// Robustness: corrupted BMG/BFN/text bytes give an error or a value, never a crash (Correction #1).
#include "test.hpp"

#include "hdf/facts.hpp"
#include "hdf/io.hpp"
#include "hdf/source.hpp"
#include "hdf/text.hpp"

#include "../src/text/text_internal.hpp"

#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

using hdf::Bytes;
using hdf::u16;
using hdf::u32;
using hdf::u64;
using hdf::u8;
using hdf::View;
namespace io = hdf::io;
namespace td = hdf::text::detail;

// ---- vectors: made by `python -I -B tools/dev/golden_text.py vectors` (Python 3.10.6; synthetic data only)
constexpr const char* kBfnHex =
    "464f4e5462666e31000002700000000600000000000000000000000000000000494e4631000000200000001400040016001a"
    "0020000000000000000000000000474c593100000020002000ff001800180000000000000000000000000000000057494431"
    "000001cc000000df0006000d0014000a00110007000e0015000b00120008000f0016000c0013000900100006000d0014000a"
    "00110007000e0015000b00120008000f0016000c0013000900100006000d0014000a00110007000e0015000b00120008000f"
    "0016000c0013000900100006000d0014000a00110007000e0015000b00120008000f0016000c0013000900100006000d0014"
    "000a00110007000e0015000b00120008000f0016000c0013000900100006000d0014000a00110007000e0015000b00120008"
    "000f0016000c0013000900100006000d0014000a00110007000e0015000b00120008000f0016000c0013000900100006000d"
    "0014000a00110007000e0015000b00120008000f0016000c0013000900100006000d0014000a00110007000e0015000b0012"
    "0008000f0016000c0013000900100006000d0014000a00110007000e0015000b00120008000f0016000c0013000900100006"
    "000d0014000a00110007000e0015000b00120008000f0016000c0013000900100006000d0014000a00110007000e0015000b"
    "00120008000f0016000c0013000900100006000d0014000a00110007000e0015000b00120008000f0016000c001300090010"
    "0006000d00144d4150310000001000010041005a00004d41503100000024000200300039000a00c800c900ca00cb00cc00cd"
    "00ce00cf00d000d14d415031000000100000002000ff0000"
    ;
constexpr int kFontCell = 22, kFontCellW = 24;
constexpr double kFontName = 0x1.1555555555556p+6;
constexpr double kFontChars[256] = {
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4, 0x1.6155555555555p+4,
    0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3,
    0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2, 0x1.cd55555555555p+3, 0x1.5200000000000p+4,
    0x1.7155555555555p+3, 0x1.2400000000000p+4, 0x1.1555555555556p+3, 0x1.ec00000000000p+3,
    0x1.6155555555555p+4, 0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3,
    0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3, 0x1.0555555555556p+4,
    0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3,
    0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2, 0x1.2400000000000p+4, 0x1.1555555555556p+3,
    0x1.ec00000000000p+3, 0x1.6155555555555p+4, 0x1.9000000000000p+3, 0x1.3355555555555p+4,
    0x1.3400000000000p+3, 0x1.0555555555556p+4, 0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3,
    0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2,
    0x1.cd55555555555p+3, 0x1.5200000000000p+4, 0x1.7155555555555p+3, 0x1.2400000000000p+4,
    0x1.1555555555556p+3, 0x1.ec00000000000p+3, 0x1.6155555555555p+4, 0x1.9000000000000p+3,
    0x1.3355555555555p+4, 0x1.3400000000000p+3, 0x1.0555555555556p+4, 0x1.b000000000000p+2,
    0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4,
    0x1.ed55555555555p+2, 0x1.cd55555555555p+3, 0x1.5200000000000p+4, 0x1.7155555555555p+3,
    0x1.2400000000000p+4, 0x1.1555555555556p+3, 0x1.ec00000000000p+3, 0x1.6155555555555p+4,
    0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3, 0x1.0555555555556p+4,
    0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3,
    0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2, 0x1.cd55555555555p+3, 0x1.5200000000000p+4,
    0x1.7155555555555p+3, 0x1.2400000000000p+4, 0x1.1555555555556p+3, 0x1.ec00000000000p+3,
    0x1.6155555555555p+4, 0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3,
    0x1.0555555555556p+4, 0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4,
    0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2, 0x1.cd55555555555p+3,
    0x1.5200000000000p+4, 0x1.7155555555555p+3, 0x1.2400000000000p+4, 0x1.1555555555556p+3,
    0x1.ec00000000000p+3, 0x1.6155555555555p+4, 0x1.9000000000000p+3, 0x1.3355555555555p+4,
    0x1.3400000000000p+3, 0x1.0555555555556p+4, 0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3,
    0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2,
    0x1.cd55555555555p+3, 0x1.5200000000000p+4, 0x1.7155555555555p+3, 0x1.2400000000000p+4,
    0x1.1555555555556p+3, 0x1.ec00000000000p+3, 0x1.6155555555555p+4, 0x1.9000000000000p+3,
    0x1.3355555555555p+4, 0x1.3400000000000p+3, 0x1.0555555555556p+4, 0x1.b000000000000p+2,
    0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4,
    0x1.ed55555555555p+2, 0x1.cd55555555555p+3, 0x1.5200000000000p+4, 0x1.7155555555555p+3,
    0x1.2400000000000p+4, 0x1.1555555555556p+3, 0x1.ec00000000000p+3, 0x1.6155555555555p+4,
    0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3, 0x1.0555555555556p+4,
    0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3,
    0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2, 0x1.cd55555555555p+3, 0x1.5200000000000p+4,
    0x1.7155555555555p+3, 0x1.2400000000000p+4, 0x1.1555555555556p+3, 0x1.ec00000000000p+3,
    0x1.6155555555555p+4, 0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3,
    0x1.0555555555556p+4, 0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4,
    0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2, 0x1.cd55555555555p+3,
    0x1.5200000000000p+4, 0x1.7155555555555p+3, 0x1.2400000000000p+4, 0x1.1555555555556p+3,
    0x1.ec00000000000p+3, 0x1.6155555555555p+4, 0x1.9000000000000p+3, 0x1.3355555555555p+4,
    0x1.3400000000000p+3, 0x1.0555555555556p+4, 0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3,
    0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2,
    0x1.cd55555555555p+3, 0x1.5200000000000p+4, 0x1.7155555555555p+3, 0x1.2400000000000p+4,
    0x1.1555555555556p+3, 0x1.ec00000000000p+3, 0x1.6155555555555p+4, 0x1.9000000000000p+3,
    0x1.3355555555555p+4, 0x1.3400000000000p+3, 0x1.0555555555556p+4, 0x1.b000000000000p+2,
    0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3, 0x1.14aaaaaaaaaabp+4,
    0x1.ed55555555555p+2, 0x1.cd55555555555p+3, 0x1.5200000000000p+4, 0x1.7155555555555p+3,
    0x1.2400000000000p+4, 0x1.1555555555556p+3, 0x1.ec00000000000p+3, 0x1.6155555555555p+4,
    0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3, 0x1.0555555555556p+4,
    0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4, 0x1.52aaaaaaaaaabp+3,
    0x1.14aaaaaaaaaabp+4, 0x1.ed55555555555p+2, 0x1.cd55555555555p+3, 0x1.5200000000000p+4,
    0x1.7155555555555p+3, 0x1.2400000000000p+4, 0x1.1555555555556p+3, 0x1.ec00000000000p+3,
    0x1.6155555555555p+4, 0x1.9000000000000p+3, 0x1.3355555555555p+4, 0x1.3400000000000p+3,
    0x1.0555555555556p+4, 0x1.b000000000000p+2, 0x1.aeaaaaaaaaaabp+3, 0x1.42aaaaaaaaaabp+4,
};
struct WidthCase { const char* hex; int sequential; int compensated; };
const WidthCase kWidthCases[] = {
    {"2827264a3a56483d213b6b3821573d7e286d2a7d7b75643c4b49697768395e", 457, 456},
    {"4523375e36207d4040596670274f5e", 211, 210},
    {"75666059212537297667283b3b45287d78612f3e4a746d464d432c4a205a76216344", 459, 460},
    {"5d676d342b4230443b346964453a374d653e44586f2e2f34", 289, 288},
};
struct RewrapCase { const char* in; int limit; const char* out; const char* note; };
const RewrapCase kRewrapHand[] = {
    {"53686f7274206c696e652e00", 300,
     "53686f7274206c696e652e00",
     nullptr},
    {"54686973206973206f6e652076657279206c6f6e67206c696e6520746861742077696c6c206365727461696e6c79206e6f742066697420696e746f20746865206e6172726f772077696e646f7720617420616c6c2e00", 200,
     "54686973206973206f6e6520766572790a6c6f6e67206c696e6520746861740a77696c6c206365727461696e6c790a6e6f742066697420696e746f207468650a6e6172726f772077696e646f770a617420616c6c2e00",
     "lines re-wrapped to the GC window width (widest 1069 -> 200 px, limit 200; 1 -> 6 lines, 2 pages)"},
    {"46697273742073656e74656e636520686572652e0a5365636f6e642073656e74656e6365206973206120626974206c6f6e676572207468616e207468652077696e646f7720616c6c6f77732e0a54686972642e00", 220,
     "46697273742073656e74656e63650a686572652e0a0a0a5365636f6e642073656e74656e63652069730a6120626974206c6f6e676572207468616e0a7468652077696e646f770a616c6c6f77732e0a54686972642e00",
     "lines re-wrapped to the GC window width (widest 671 -> 213 px, limit 220; 3 -> 9 lines, 3 pages)"},
    {"4f6e652074776f20746872656520666f757220666976652073697820736576656e206569676874206e696e652074656e20656c6576656e207477656c766520746869727465656e20666f75727465656e206669667465656e207369787465656e20736576656e7465656e20656967687465656e206e696e657465656e207477656e7479207477656e74796f6e65207477656e747974776f207477656e747974687265652e00", 150,
     "4f6e652074776f0a746872656520666f75720a66697665207369780a736576656e2065696768740a6e696e652074656e0a656c6576656e0a7477656c76650a746869727465656e0a666f75727465656e0a6669667465656e0a7369787465656e0a736576656e7465656e0a656967687465656e0a6e696e657465656e0a7477656e74790a7477656e74796f6e650a7477656e747974776f0a7477656e747974687265652e00",
     "lines re-wrapped to the GC window width (widest 2087 -> 191 px, limit 150; 1 -> 18 lines, 5 pages)"},
    {"1a05000001426967205368696e79205469746c651a050000020a596f7520676f7420746865207665727920626967207368696e79207468696e6721204974206973207368696e7920616e642062696720616e6420697420676f6573206f6e2e00", 180,
     "1a05000001426967205368696e79205469746c651a050000020a596f7520676f74207468650a7665727920626967207368696e790a7468696e67210a4974206973207368696e7920616e640a62696720616e6420697420676f65730a6f6e2e00",
     "lines re-wrapped to the GC window width (widest 782 -> 170 px, limit 180; 2 -> 7 lines, 2 pages)"},
    {"1a06ff000006353020527570656573201a050000002077616e74732069740a5468652073686f70206861732061206c6f6e67206465736372697074696f6e206f6620746865206974656d20666f722073616c652e00", 160,
     "1a06ff0000063530205275706565730a1a050000002077616e74730a69740a5468652073686f700a6861732061206c6f6e670a6465736372697074696f6e0a6f6620746865206974656d0a666f722073616c652e00",
     "lines re-wrapped to the GC window width (widest 697 -> 152 px, limit 160; 2 -> 8 lines, 2 pages)"},
    {"497420636f73747320312c3030302052757065657320616e64206d6f726520616e64206d6f726520312c3030302052757065657320616761696e2e00", 150,
     "497420636f7374730a312c303030205275706565730a616e64206d6f726520616e640a6d6f72650a312c303030205275706565730a616761696e2e00",
     "lines re-wrapped to the GC window width (widest 780 -> 167 px, limit 150; 1 -> 6 lines, 2 pages)"},
    {"412073656c656374696f6e20746861742069732066617220746f6f207769646520666f72207468652077696e646f77201a050000082079657300", 120,
     "412073656c656374696f6e20746861742069732066617220746f6f207769646520666f72207468652077696e646f77201a050000082079657300",
     "selection message wider than GC (664 > 120 px): left as is"},
    {"546f70206c696e652074686174206973206c6f6e6720656e6f75676820746f20777261702061726f756e642e0a0a4166746572206120626c616e6b206c696e652c206d6f7265206c6f6e67207465787420666f6c6c6f777320686572652e00", 170,
     "546f70206c696e650a74686174206973206c6f6e670a656e6f75676820746f0a777261700a61726f756e642e0a0a416674657220610a626c616e6b206c696e652c0a6d6f7265206c6f6e670a7465787420666f6c6c6f77730a686572652e00",
     "lines re-wrapped to the GC window width (widest 648 -> 167 px, limit 170; 3 -> 11 lines, 3 pages)"},
    {"537061636573206f6e6c79206e6578743a0a20202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020200a456e64206f662074686520746578742c207768696368206973206c6f6e672e00", 140,
     "5370616365730a6f6e6c79206e6578743a0a20202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020200a0a456e64206f66207468650a746578742c0a77686963682069730a6c6f6e672e00",
     "lines re-wrapped to the GC window width (widest 389 -> 338 px, limit 140; 3 -> 8 lines, 2 pages)"},
    {"53656e74656e6365206f6e65206973206c6f6e672e2053656e74656e63652074776f206973206c6f6e6720746f6f2e2053656e74656e6365207468726565206973206c6f6e672e2053656e74656e636520666f75722e20466976652e2053697820676f6573206f6e20616e64206f6e2e20536576656e20697320686572652e2045696768742e00", 120,
     "53656e74656e63650a6f6e652069730a6c6f6e672e0a0a53656e74656e63650a74776f2069730a6c6f6e6720746f6f2e0a0a53656e74656e63650a74687265652069730a6c6f6e672e0a0a53656e74656e63650a666f75722e0a466976652e0a0a53697820676f6573206f6e0a616e64206f6e2e0a536576656e2069730a686572652e0a45696768742e00",
     "lines re-wrapped to the GC window width (widest 1580 -> 119 px, limit 120; 1 -> 21 lines, 6 pages)"},
    {"4c696e65206f6e652e0a4c696e652074776f2e0a4c696e652074687265652e0a4c696e6520666f75722e0a4c696e65206669766520697320746865207374617274206f6620706167652074776f20616e64206973206c6f6e672e00", 160,
     "4c696e65206f6e652e0a4c696e652074776f2e0a4c696e652074687265652e0a4c696e6520666f75722e0a4c696e6520666976652069730a7468652073746172740a6f6620706167650a74776f20616e642069730a6c6f6e672e00",
     "lines re-wrapped to the GC window width (widest 576 -> 144 px, limit 160; 5 -> 9 lines, 3 pages)"},
    {"1a0500000020616e64201a050000002077656e7420746f207468652063617665206f6620736861646f777320616e6420666f756e6420612068656172742070696563652074686572652e00", 150,
     "1a0500000020616e640a1a050000002077656e740a746f207468650a63617665206f660a736861646f777320616e640a666f756e6420610a68656172740a70696563650a74686572652e00",
     "lines re-wrapped to the GC window width (widest 970 -> 139 px, limit 150; 1 -> 9 lines, 3 pages)"},
    {"547261696c696e67206e65776c696e6573207468617420617265206c6f6e6720656e6f75676820746f206e656564206120777261702e0a0a0a00", 150,
     "547261696c696e670a6e65776c696e65730a74686174206172650a6c6f6e6720656e6f7567680a746f206e65656420610a777261702e00",
     "lines re-wrapped to the GC window width (widest 685 -> 128 px, limit 150; 4 -> 6 lines, 2 pages)"},
};
struct RewrapRandom { u32 seed; const char* in16; int limit; const char* out16; const char* note; };
const RewrapRandom kRewrapRandom[] = {
    {1, "f29fd448d8d6d231", 214, "26944046d1b06054", "lines re-wrapped to the GC window width (widest 718 -> 204 px, limit 214; 23 -> 35 lines, 9 pages)"},
    {2, "ce681262484fe5e7", 124, "00c5b3f010b6d34b", "lines re-wrapped to the GC window width (widest 374 -> 204 px, limit 124; 14 -> 18 lines, 5 pages)"},
    {3, "002320bb4382858a", 105, "cb4fd32e489eebda", "lines re-wrapped to the GC window width (widest 451 -> 204 px, limit 105; 19 -> 38 lines, 10 pages)"},
    {4, "0961f1453067d14e", 267, "0961f1453067d14e", nullptr},
    {5, "d30896a37f258b55", 172, "191bddf9415217c9", "lines re-wrapped to the GC window width (widest 398 -> 167 px, limit 172; 11 -> 20 lines, 5 pages)"},
    {6, "52d168610bd1745f", 319, "08d852036d839ca3", "lines re-wrapped to the GC window width (widest 457 -> 296 px, limit 319; 3 -> 2 lines, 1 page)"},
    {7, "3fc37ca2cb22cb6c", 212, "652490393bcf7aa4", "lines re-wrapped to the GC window width (widest 961 -> 273 px, limit 212; 14 -> 19 lines, 5 pages)"},
    {8, "6a3b253ec7c7225f", 142, "6a3b253ec7c7225f", "selection message wider than GC (1156 > 142 px): left as is"},
    {9, "0fdccb9c8eb366ac", 254, "0fdccb9c8eb366ac", "selection message wider than GC (384 > 254 px): left as is"},
    {10, "d7a1fb588029c520", 352, "d8d1bd53db888c73", "lines re-wrapped to the GC window width (widest 901 -> 352 px, limit 352; 13 -> 19 lines, 5 pages)"},
    {11, "097c42e104ce5c96", 337, "a4034cb1b10d23b3", "lines re-wrapped to the GC window width (widest 410 -> 333 px, limit 337; 6 -> 6 lines, 2 pages)"},
    {12, "3722918ec014413f", 219, "63be31253de8e234", "lines re-wrapped to the GC window width (widest 298 -> 207 px, limit 219; 33 -> 34 lines, 9 pages)"},
    {13, "51aa92161492c877", 342, "51aa92161492c877", nullptr},
    {14, "a1b01a7a37832a03", 232, "44ffdb59c929a188", "lines re-wrapped to the GC window width (widest 275 -> 218 px, limit 232; 26 -> 30 lines, 8 pages)"},
    {15, "b8bfbcc1936f234c", 252, "b8bfbcc1936f234c", nullptr},
    {16, "0f9f0b8f40a01d24", 256, "ebdb99aff8ff6f0e", "lines re-wrapped to the GC window width (widest 621 -> 239 px, limit 256; 14 -> 20 lines, 5 pages)"},
    {17, "fb061746e742dc28", 286, "fb061746e742dc28", "selection message wider than GC (620 > 286 px): left as is"},
    {18, "8c0cc0252a9a9130", 347, "96ecd0635c603b1d", "lines re-wrapped to the GC window width (widest 665 -> 303 px, limit 347; 6 -> 10 lines, 3 pages)"},
    {19, "6015c319720bc26f", 271, "d833a6359237095c", "lines re-wrapped to the GC window width (widest 1046 -> 269 px, limit 271; 18 -> 26 lines, 7 pages)"},
    {20, "0a3fc66ff1664458", 254, "fec6b87d0be1317d", "lines re-wrapped to the GC window width (widest 399 -> 242 px, limit 254; 5 -> 9 lines, 3 pages)"},
    {21, "758cbee9e89553bf", 337, "758cbee9e89553bf", "selection message wider than GC (1349 > 337 px): left as is"},
    {22, "2accdeefcd2a7e16", 245, "1f9a43298c748953", "lines re-wrapped to the GC window width (widest 598 -> 225 px, limit 245; 1 -> 3 lines, 1 page)"},
    {23, "88aad9513eb9153f", 79, "88aad9513eb9153f", "selection message wider than GC (599 > 79 px): left as is"},
    {24, "d0f40a27fe42b93c", 200, "fbbf73df39c17aba", "lines re-wrapped to the GC window width (widest 233 -> 148 px, limit 200; 2 -> 2 lines, 1 page)"},
    {25, "f306a7a01d5cc752", 121, "f306a7a01d5cc752", "selection message wider than GC (514 > 121 px): left as is"},
    {26, "09722876325ee192", 80, "09722876325ee192", "selection message wider than GC (779 > 80 px): left as is"},
    {27, "d98bc82982ab2f03", 246, "f880f02f1ebdafc3", "lines re-wrapped to the GC window width (widest 525 -> 245 px, limit 246; 12 -> 19 lines, 5 pages)"},
    {28, "dd83c6fb4f0619fe", 116, "0c7f3c9de79a287c", "lines re-wrapped to the GC window width (widest 1080 -> 204 px, limit 116; 12 -> 34 lines, 9 pages)"},
    {29, "c4cb3c418957644a", 334, "bd95888a9650e195", "lines re-wrapped to the GC window width (widest 641 -> 325 px, limit 334; 6 -> 10 lines, 3 pages)"},
    {30, "9a348ea7f0f21fc4", 141, "1d60103fbe7a910f", "lines re-wrapped to the GC window width (widest 1059 -> 204 px, limit 141; 16 -> 32 lines, 8 pages)"},
    {31, "ee199c445b6d3f87", 225, "f7fdf969150ac2cc", "lines re-wrapped to the GC window width (widest 229 -> 222 px, limit 225; 4 -> 4 lines, 1 page)"},
    {32, "aefdea80e8f4ace7", 177, "68533a9aa07cd32e", "lines re-wrapped to the GC window width (widest 689 -> 171 px, limit 177; 14 -> 23 lines, 6 pages)"},
    {33, "7f149099ef8c1963", 166, "7f149099ef8c1963", "selection message wider than GC (617 > 166 px): left as is"},
    {34, "9357eff3a6fc41ab", 180, "103cfd98e492c9af", "lines re-wrapped to the GC window width (widest 359 -> 164 px, limit 180; 13 -> 17 lines, 5 pages)"},
    {35, "6491365292348009", 309, "39f86a112101fe50", "lines re-wrapped to the GC window width (widest 687 -> 308 px, limit 309; 25 -> 31 lines, 8 pages)"},
    {36, "aa1b26f0f805caf1", 280, "4f752ada5379da3d", "lines re-wrapped to the GC window width (widest 876 -> 274 px, limit 280; 7 -> 12 lines, 3 pages)"},
    {37, "bb73cddfa6900b3e", 84, "cb5c793faaffad8c", "lines re-wrapped to the GC window width (widest 588 -> 98 px, limit 84; 14 -> 32 lines, 8 pages)"},
    {38, "f65c965cfced86ca", 85, "87a3f772287cd2cf", "lines re-wrapped to the GC window width (widest 740 -> 98 px, limit 85; 3 -> 13 lines, 4 pages)"},
    {39, "cdb97a378378dc74", 106, "b66b9ae2e3675807", "lines re-wrapped to the GC window width (widest 408 -> 204 px, limit 106; 17 -> 29 lines, 8 pages)"},
    {40, "c2d000aa12a4000d", 63, "c2d000aa12a4000d", "selection message wider than GC (914 > 63 px): left as is"},
    {41, "5e5f3822ca7cc4df", 202, "5e5f3822ca7cc4df", "selection message wider than GC (438 > 202 px): left as is"},
    {42, "ca8bd33430ae663d", 215, "366608f499ef0d89", "lines re-wrapped to the GC window width (widest 390 -> 215 px, limit 215; 27 -> 31 lines, 8 pages)"},
    {43, "73699d175f58b5a5", 269, "cfd2ccbb76c273eb", "lines re-wrapped to the GC window width (widest 342 -> 202 px, limit 269; 11 -> 9 lines, 3 pages)"},
    {44, "214b162a078c8040", 164, "214b162a078c8040", "selection message wider than GC (1258 > 164 px): left as is"},
    {45, "98741c31285dc5fb", 329, "98741c31285dc5fb", nullptr},
    {46, "4f06938d381e33d7", 113, "4f06938d381e33d7", "selection message wider than GC (1046 > 113 px): left as is"},
    {47, "bf9cd23892b99186", 186, "bf9cd23892b99186", "selection message wider than GC (382 > 186 px): left as is"},
    {48, "16d028431adbd631", 270, "16d028431adbd631", "selection message wider than GC (683 > 270 px): left as is"},
    {49, "0b4216e6ada3ee5b", 160, "0b4216e6ada3ee5b", "selection message wider than GC (734 > 160 px): left as is"},
    {50, "b0dfbfc22bfc1739", 242, "b0dfbfc22bfc1739", "selection message wider than GC (666 > 242 px): left as is"},
    {51, "b24713b539e05b5a", 228, "dbea5e99d792ff59", "lines re-wrapped to the GC window width (widest 1291 -> 214 px, limit 228; 9 -> 19 lines, 5 pages)"},
    {52, "4509cc9f32109354", 302, "88b343f96059a514", "lines re-wrapped to the GC window width (widest 624 -> 292 px, limit 302; 14 -> 11 lines, 3 pages)"},
    {53, "0624d68f41851efe", 155, "0624d68f41851efe", "selection message wider than GC (1253 > 155 px): left as is"},
    {54, "b263d74c9e3b93b8", 103, "56c2273ad843a2ff", "lines re-wrapped to the GC window width (widest 394 -> 204 px, limit 103; 4 -> 12 lines, 3 pages)"},
    {55, "0a81f4b7e1f209ac", 294, "0a81f4b7e1f209ac", "selection message wider than GC (945 > 294 px): left as is"},
    {56, "a43b4d30a68ace58", 343, "a43b4d30a68ace58", nullptr},
    {57, "8f5425ed52ce910c", 208, "8f5425ed52ce910c", "selection message wider than GC (226 > 208 px): left as is"},
    {58, "457ea2007730c6f8", 94, "c8a1c11ba92629b6", "lines re-wrapped to the GC window width (widest 123 -> 62 px, limit 94; 5 -> 3 lines, 1 page)"},
    {59, "e83111a6068d8e7f", 93, "e83111a6068d8e7f", "selection message wider than GC (1541 > 93 px): left as is"},
    {60, "613898b78349b31b", 243, "613898b78349b31b", "selection message wider than GC (560 > 243 px): left as is"},
    {61, "53ddd67ec437b790", 181, "d70e204f84964449", "lines re-wrapped to the GC window width (widest 540 -> 204 px, limit 181; 7 -> 12 lines, 3 pages)"},
    {62, "5ef15769f30c3958", 299, "b79f2b6fa79d7aa8", "lines re-wrapped to the GC window width (widest 604 -> 293 px, limit 299; 16 -> 18 lines, 5 pages)"},
    {63, "31f6dd393f5bcebc", 124, "efa92f5cc01eb7f2", "lines re-wrapped to the GC window width (widest 459 -> 84 px, limit 124; 3 -> 8 lines, 2 pages)"},
    {64, "872eb9ad75b27b39", 244, "624c5bb4f521af2c", "lines re-wrapped to the GC window width (widest 887 -> 229 px, limit 244; 16 -> 24 lines, 6 pages)"},
    {65, "b0c9617cc49a1b38", 220, "b0c9617cc49a1b38", nullptr},
    {66, "650169e1970c7385", 296, "5a568cca4d37f46f", "lines re-wrapped to the GC window width (widest 690 -> 296 px, limit 296; 7 -> 10 lines, 3 pages)"},
    {67, "006be8db5b59ef20", 168, "006be8db5b59ef20", "selection message wider than GC (611 > 168 px): left as is"},
    {68, "07e567a685c47470", 293, "07e567a685c47470", "selection message wider than GC (394 > 293 px): left as is"},
    {69, "f40f8f6e2911447d", 327, "84dd2d4ade67066a", "lines re-wrapped to the GC window width (widest 483 -> 320 px, limit 327; 20 -> 24 lines, 6 pages)"},
    {70, "ab25206356a35c9f", 350, "ab25206356a35c9f", nullptr},
    {71, "0c4c6a41bae36936", 276, "0c4c6a41bae36936", "selection message wider than GC (711 > 276 px): left as is"},
    {72, "cdef50a982ca34cf", 313, "cdef50a982ca34cf", nullptr},
    {73, "bf48fb4de494fb3f", 152, "bf48fb4de494fb3f", "selection message wider than GC (435 > 152 px): left as is"},
    {74, "e49a918709c09313", 320, "e49a918709c09313", nullptr},
    {75, "e09246f02bf397da", 250, "e09246f02bf397da", "selection message wider than GC (344 > 250 px): left as is"},
    {76, "d73721efcc14a326", 296, "a35a4a3f2161c603", "lines re-wrapped to the GC window width (widest 762 -> 288 px, limit 296; 14 -> 22 lines, 6 pages)"},
    {77, "7f50d26b02f65825", 64, "022967b36834d319", "lines re-wrapped to the GC window width (widest 707 -> 204 px, limit 64; 7 -> 20 lines, 5 pages)"},
    {78, "d3e5051c8e1e4628", 332, "ec7f645ef05f6663", "lines re-wrapped to the GC window width (widest 799 -> 331 px, limit 332; 14 -> 15 lines, 4 pages)"},
    {79, "09345e2885defe27", 137, "d227825993b448ef", "lines re-wrapped to the GC window width (widest 381 -> 131 px, limit 137; 4 -> 11 lines, 3 pages)"},
    {80, "69d80e580c96e069", 226, "12b632c019ca5668", "lines re-wrapped to the GC window width (widest 939 -> 273 px, limit 226; 16 -> 23 lines, 6 pages)"},
    {81, "24158373ab5932e6", 245, "24158373ab5932e6", nullptr},
    {82, "6c77a71373e327d6", 239, "6b5e4e3ebea03054", "lines re-wrapped to the GC window width (widest 887 -> 236 px, limit 239; 16 -> 24 lines, 6 pages)"},
    {83, "491fdd6788ccd4da", 338, "745ba31a3858da14", "lines re-wrapped to the GC window width (widest 483 -> 326 px, limit 338; 26 -> 25 lines, 7 pages)"},
    {84, "b1680a705c727f2e", 274, "e65943bc5e576019", "lines re-wrapped to the GC window width (widest 462 -> 264 px, limit 274; 23 -> 23 lines, 6 pages)"},
    {85, "a15dc8048f17e63b", 218, "2c028f861cc90ff5", "lines re-wrapped to the GC window width (widest 528 -> 207 px, limit 218; 18 -> 30 lines, 8 pages)"},
    {86, "467ea07528c2d20e", 186, "162ad06dbfb46b1d", "lines re-wrapped to the GC window width (widest 499 -> 204 px, limit 186; 7 -> 11 lines, 3 pages)"},
    {87, "2c43d3988ba50c24", 290, "47f9fbfbf197d0f9", "lines re-wrapped to the GC window width (widest 721 -> 290 px, limit 290; 18 -> 19 lines, 5 pages)"},
    {88, "520ca03001dabc63", 149, "520ca03001dabc63", "selection message wider than GC (555 > 149 px): left as is"},
    {89, "3fac87e02a7a356f", 60, "9208b59cc458137e", "lines re-wrapped to the GC window width (widest 1135 -> 228 px, limit 60; 13 -> 31 lines, 8 pages)"},
    {90, "ca38f00e98907335", 131, "7e4cd6c9105fe648", "lines re-wrapped to the GC window width (widest 397 -> 158 px, limit 131; 3 -> 8 lines, 2 pages)"},
    {91, "6761b9ea83182fac", 291, "f92ec1fdefd6cae5", "lines re-wrapped to the GC window width (widest 658 -> 253 px, limit 291; 7 -> 11 lines, 3 pages)"},
    {92, "9c799c2b5c484e84", 95, "9c799c2b5c484e84", "selection message wider than GC (908 > 95 px): left as is"},
    {93, "ff4efed19d12ab95", 159, "fb49481fda902e37", "lines re-wrapped to the GC window width (widest 606 -> 167 px, limit 159; 15 -> 17 lines, 5 pages)"},
    {94, "9d6c53d7ef9e3f29", 293, "6c9ee7164316b8db", "lines re-wrapped to the GC window width (widest 679 -> 288 px, limit 293; 17 -> 21 lines, 6 pages)"},
    {95, "e06bb2de7a5649e4", 282, "e06bb2de7a5649e4", nullptr},
    {96, "73a1ac5ab5516fad", 332, "73a1ac5ab5516fad", "selection message wider than GC (749 > 332 px): left as is"},
    {97, "9e3619c195c782af", 344, "9e3619c195c782af", nullptr},
    {98, "95a6ce6496c85421", 240, "d767a9f430ea4dd1", "lines re-wrapped to the GC window width (widest 477 -> 228 px, limit 240; 8 -> 12 lines, 3 pages)"},
    {99, "14a26e8f19ad3e98", 120, "14a26e8f19ad3e98", nullptr},
    {100, "1f3ea59c6a2db7de", 105, "26eb59b91d1cf4e2", "lines re-wrapped to the GC window width (widest 483 -> 127 px, limit 105; 12 -> 22 lines, 6 pages)"},
    {101, "0e8b199662cbd03e", 89, "5536496d7d0f7cd6", "lines re-wrapped to the GC window width (widest 607 -> 104 px, limit 89; 24 -> 45 lines, 12 pages)"},
    {102, "50402b6cbd7577dc", 99, "a97d57d43b97dbaa", "lines re-wrapped to the GC window width (widest 293 -> 204 px, limit 99; 10 -> 18 lines, 5 pages)"},
    {103, "6dea0f6e2803658e", 84, "6dea0f6e2803658e", "selection message wider than GC (649 > 84 px): left as is"},
    {104, "8f365b554088c000", 185, "53b407ae811ef238", "lines re-wrapped to the GC window width (widest 315 -> 204 px, limit 185; 5 -> 7 lines, 2 pages)"},
    {105, "39fc95170c0d551d", 281, "3bcfb2a98b6d2af4", "lines re-wrapped to the GC window width (widest 344 -> 280 px, limit 281; 21 -> 22 lines, 6 pages)"},
    {106, "58c68caba50f1d60", 63, "89c7d48d39445eeb", "lines re-wrapped to the GC window width (widest 255 -> 63 px, limit 63; 1 -> 4 lines, 1 page)"},
    {107, "ffb3bb73b1c6af5e", 289, "b15219c331adf485", "lines re-wrapped to the GC window width (widest 344 -> 270 px, limit 289; 10 -> 13 lines, 4 pages)"},
    {108, "c6a7f1702ff0fb91", 165, "cf736fd743936e29", "lines re-wrapped to the GC window width (widest 657 -> 204 px, limit 165; 24 -> 46 lines, 12 pages)"},
    {109, "43288d0188be24cb", 129, "43288d0188be24cb", "selection message wider than GC (279 > 129 px): left as is"},
    {110, "3c1535417d56fc7a", 216, "877101d74abd9b64", "lines re-wrapped to the GC window width (widest 721 -> 194 px, limit 216; 14 -> 26 lines, 7 pages)"},
    {111, "e1338559735930d0", 312, "e1338559735930d0", "selection message wider than GC (737 > 312 px): left as is"},
    {112, "fec8e411158d957f", 336, "fec8e411158d957f", "selection message wider than GC (551 > 336 px): left as is"},
    {113, "09e113dc5670b55c", 76, "2537381aa880e970", "lines re-wrapped to the GC window width (widest 188 -> 93 px, limit 76; 9 -> 13 lines, 4 pages)"},
    {114, "7b30adeba80e4e56", 241, "7b30adeba80e4e56", "selection message wider than GC (358 > 241 px): left as is"},
    {115, "ff14cbb09fc9370b", 278, "ff14cbb09fc9370b", nullptr},
    {116, "8db322979baec2a7", 152, "c638ed5b1c068bbc", "lines re-wrapped to the GC window width (widest 785 -> 143 px, limit 152; 14 -> 20 lines, 5 pages)"},
    {117, "4e304879d7a5ef9d", 324, "4e304879d7a5ef9d", "selection message wider than GC (777 > 324 px): left as is"},
    {118, "5c9f2aa8a4864d2e", 342, "be12594a2c0a826e", "lines re-wrapped to the GC window width (widest 614 -> 342 px, limit 342; 7 -> 5 lines, 2 pages)"},
    {119, "1783cc5b5e0ff29e", 85, "0ae06bb5613cade8", "lines re-wrapped to the GC window width (widest 610 -> 167 px, limit 85; 13 -> 39 lines, 10 pages)"},
    {120, "eb692a013837b2c9", 137, "efb6d24da5adb17e", "lines re-wrapped to the GC window width (widest 438 -> 142 px, limit 137; 6 -> 10 lines, 3 pages)"},
    {121, "47aed2a6d3f98fa0", 345, "ba9917638e67e72e", "lines re-wrapped to the GC window width (widest 746 -> 314 px, limit 345; 19 -> 22 lines, 6 pages)"},
    {122, "a41c9f4b16f213f3", 343, "9d2a24a1c1c4f704", "lines re-wrapped to the GC window width (widest 779 -> 337 px, limit 343; 1 -> 3 lines, 1 page)"},
    {123, "141559c50dd62393", 289, "06c6d6f3fe3125cd", "lines re-wrapped to the GC window width (widest 430 -> 268 px, limit 289; 24 -> 32 lines, 8 pages)"},
    {124, "f4c6fd93fc3c9b41", 258, "7558063104b5644d", "lines re-wrapped to the GC window width (widest 1249 -> 258 px, limit 258; 20 -> 32 lines, 8 pages)"},
    {125, "7b6e2b441dcc7303", 127, "6877feb57611ffe4", "lines re-wrapped to the GC window width (widest 990 -> 204 px, limit 127; 15 -> 25 lines, 7 pages)"},
    {126, "e1d7b51dd5bd089b", 98, "e1d7b51dd5bd089b", "selection message wider than GC (935 > 98 px): left as is"},
    {127, "e8f18852875a728b", 251, "e8f18852875a728b", "selection message wider than GC (302 > 251 px): left as is"},
    {128, "a838cf8cc1bf928a", 226, "19d2b27b1ffda485", "lines re-wrapped to the GC window width (widest 539 -> 220 px, limit 226; 16 -> 23 lines, 6 pages)"},
    {129, "a374b9ad602d1fdc", 233, "a374b9ad602d1fdc", nullptr},
    {130, "72c42d0e76050c3b", 244, "5a30066f1ce828c8", "lines re-wrapped to the GC window width (widest 427 -> 230 px, limit 244; 16 -> 15 lines, 4 pages)"},
    {131, "5115ad2c56361102", 233, "176342b218fdafc5", "lines re-wrapped to the GC window width (widest 353 -> 170 px, limit 233; 2 -> 3 lines, 1 page)"},
    {132, "17a5c6c25c3e1a17", 153, "17a5c6c25c3e1a17", "selection message wider than GC (757 > 153 px): left as is"},
    {133, "8a8d07bf21cb1041", 110, "c40276aae92d47df", "lines re-wrapped to the GC window width (widest 543 -> 98 px, limit 110; 28 -> 48 lines, 12 pages)"},
    {134, "aae61deacdb8dbc4", 257, "3d71a20280a1f130", "lines re-wrapped to the GC window width (widest 499 -> 247 px, limit 257; 19 -> 16 lines, 4 pages)"},
    {135, "2ff0c6f566d09be5", 184, "dae302a0e12db9f6", "lines re-wrapped to the GC window width (widest 270 -> 182 px, limit 184; 21 -> 26 lines, 7 pages)"},
    {136, "2436dcb1a9ead1a0", 232, "2436dcb1a9ead1a0", "selection message wider than GC (320 > 232 px): left as is"},
    {137, "e654765590967cb4", 118, "31266c61c4c77aee", "lines re-wrapped to the GC window width (widest 617 -> 204 px, limit 118; 17 -> 39 lines, 10 pages)"},
    {138, "7da2a2dc7808a2fa", 106, "441ade8c9befeb6a", "lines re-wrapped to the GC window width (widest 266 -> 103 px, limit 106; 6 -> 10 lines, 3 pages)"},
    {139, "20c95678794f24ed", 66, "2d56eb5a48250811", "lines re-wrapped to the GC window width (widest 1117 -> 158 px, limit 66; 9 -> 37 lines, 10 pages)"},
    {140, "b3945c9aab39eb1c", 352, "562cf934a767b77f", "lines re-wrapped to the GC window width (widest 480 -> 290 px, limit 352; 1 -> 2 lines, 1 page)"},
    {141, "5d57873195bdd353", 178, "0206b47344d03d3f", "lines re-wrapped to the GC window width (widest 492 -> 204 px, limit 178; 17 -> 17 lines, 5 pages)"},
    {142, "dc079b76705bfa27", 81, "dc079b76705bfa27", "selection message wider than GC (733 > 81 px): left as is"},
    {143, "9ff009271d46a4a9", 229, "1a372c8c69edd688", "lines re-wrapped to the GC window width (widest 459 -> 209 px, limit 229; 8 -> 12 lines, 3 pages)"},
    {144, "f4ca6629b95532b9", 277, "fd59074404938ca9", "lines re-wrapped to the GC window width (widest 615 -> 277 px, limit 277; 21 -> 26 lines, 7 pages)"},
    {145, "88a7e39b40148c23", 88, "a91913773903bf4f", "lines re-wrapped to the GC window width (widest 182 -> 124 px, limit 88; 15 -> 17 lines, 5 pages)"},
    {146, "21de28bcbc62feb9", 219, "21de28bcbc62feb9", "selection message wider than GC (265 > 219 px): left as is"},
    {147, "191bcb4b9b5ec86b", 321, "191bcb4b9b5ec86b", nullptr},
    {148, "d55a96831109a7a2", 94, "62fd2a3eea46cf66", "lines re-wrapped to the GC window width (widest 545 -> 98 px, limit 94; 21 -> 32 lines, 8 pages)"},
    {149, "02a7be287023947c", 184, "02a7be287023947c", nullptr},
    {150, "8ed8c68f59dd3bd3", 95, "663611077a144c0f", "lines re-wrapped to the GC window width (widest 425 -> 98 px, limit 95; 13 -> 22 lines, 6 pages)"},
    {151, "c1a532e8d2711ba2", 115, "c1a532e8d2711ba2", "selection message wider than GC (750 > 115 px): left as is"},
    {152, "c0e2bf3c40a6c70b", 87, "ce23f743c8152aef", "lines re-wrapped to the GC window width (widest 573 -> 204 px, limit 87; 8 -> 13 lines, 4 pages)"},
    {153, "61a6d48f519cad57", 349, "1f3f38cda73d9b25", "lines re-wrapped to the GC window width (widest 554 -> 338 px, limit 349; 10 -> 10 lines, 3 pages)"},
    {154, "c2c556ac812e37be", 269, "0b129ab26a5c9a07", "lines re-wrapped to the GC window width (widest 273 -> 266 px, limit 269; 10 -> 8 lines, 2 pages)"},
    {155, "86954ee28c700393", 356, "f5842e2a710f8bae", "lines re-wrapped to the GC window width (widest 737 -> 340 px, limit 356; 9 -> 11 lines, 3 pages)"},
    {156, "86cac3bb89937159", 202, "f55fbf9401e5c44f", "lines re-wrapped to the GC window width (widest 217 -> 157 px, limit 202; 1 -> 2 lines, 1 page)"},
    {157, "d6aeacfe14706ab7", 332, "0b46726ca6c48055", "lines re-wrapped to the GC window width (widest 733 -> 302 px, limit 332; 10 -> 14 lines, 4 pages)"},
    {158, "857c2515245a7220", 317, "857c2515245a7220", "selection message wider than GC (661 > 317 px): left as is"},
    {159, "993429b5a1f90b28", 335, "c6db184908265872", "lines re-wrapped to the GC window width (widest 548 -> 304 px, limit 335; 8 -> 9 lines, 3 pages)"},
    {160, "d2d1578ef61e454b", 157, "d2d1578ef61e454b", "selection message wider than GC (713 > 157 px): left as is"},
    {161, "7bac24fe95b008ab", 220, "7bac24fe95b008ab", "selection message wider than GC (381 > 220 px): left as is"},
    {162, "e1a556fa4bc81fc5", 215, "e1a556fa4bc81fc5", "selection message wider than GC (763 > 215 px): left as is"},
    {163, "7d13a51ce8cc7d52", 200, "7d13a51ce8cc7d52", "selection message wider than GC (292 > 200 px): left as is"},
    {164, "1527bf03884a77ff", 171, "faf4ed5893806ac0", "lines re-wrapped to the GC window width (widest 1093 -> 204 px, limit 171; 8 -> 22 lines, 6 pages)"},
    {165, "7b60418e32f167c8", 173, "4dc28f2c13589049", "lines re-wrapped to the GC window width (widest 501 -> 158 px, limit 173; 3 -> 3 lines, 1 page)"},
    {166, "73e9066a507e8d43", 312, "7629453ca8301da8", "lines re-wrapped to the GC window width (widest 410 -> 275 px, limit 312; 10 -> 10 lines, 3 pages)"},
    {167, "7601d504963cb229", 315, "7601d504963cb229", "selection message wider than GC (780 > 315 px): left as is"},
    {168, "945b459a0441c6f8", 202, "945b459a0441c6f8", "selection message wider than GC (730 > 202 px): left as is"},
    {169, "0dd969b270409c44", 223, "0dd969b270409c44", "selection message wider than GC (406 > 223 px): left as is"},
    {170, "f1a3edcc8d633149", 313, "c2ad36e8195ed73c", "lines re-wrapped to the GC window width (widest 369 -> 278 px, limit 313; 10 -> 10 lines, 3 pages)"},
    {171, "f5ef6b8f9e928484", 250, "c7810d45d9812e42", "lines re-wrapped to the GC window width (widest 642 -> 250 px, limit 250; 18 -> 22 lines, 6 pages)"},
    {172, "cf9b15a0546ae432", 253, "cf9b15a0546ae432", nullptr},
    {173, "48601bd0958c7121", 270, "48601bd0958c7121", nullptr},
    {174, "cef442e939094a9e", 223, "1d3f07c567e99d2b", "lines re-wrapped to the GC window width (widest 290 -> 202 px, limit 223; 3 -> 2 lines, 1 page)"},
    {175, "f3d975937f2c6fdc", 347, "1d25217a590073d3", "lines re-wrapped to the GC window width (widest 976 -> 324 px, limit 347; 9 -> 10 lines, 3 pages)"},
    {176, "630cec06a3eb5233", 179, "630cec06a3eb5233", "selection message wider than GC (1037 > 179 px): left as is"},
    {177, "3c0d3091b7e67174", 352, "9f342e0c0aa76f6f", "lines re-wrapped to the GC window width (widest 417 -> 304 px, limit 352; 9 -> 9 lines, 3 pages)"},
    {178, "6471e033b2d6af26", 315, "46813a93db1e8d49", "lines re-wrapped to the GC window width (widest 384 -> 267 px, limit 315; 25 -> 25 lines, 7 pages)"},
    {179, "5664e95577d0a03c", 262, "acff446492fb90fe", "lines re-wrapped to the GC window width (widest 308 -> 197 px, limit 262; 4 -> 4 lines, 1 page)"},
    {180, "f6172f2a0b8363d6", 129, "9b531d97f2cd8109", "lines re-wrapped to the GC window width (widest 550 -> 204 px, limit 129; 14 -> 31 lines, 8 pages)"},
    {181, "89fabeac0796fea8", 306, "89fabeac0796fea8", nullptr},
    {182, "1370e6541a045503", 245, "31cc478eb3f75e35", "lines re-wrapped to the GC window width (widest 426 -> 236 px, limit 245; 12 -> 11 lines, 3 pages)"},
    {183, "d267e8013ab4b51d", 91, "219637d79013e84f", "lines re-wrapped to the GC window width (widest 92 -> 86 px, limit 91; 1 -> 1 lines, 1 page)"},
    {184, "9ba7ebceb694421f", 138, "ec46dcc5647b1841", "lines re-wrapped to the GC window width (widest 377 -> 204 px, limit 138; 14 -> 18 lines, 5 pages)"},
    {185, "2d3107fa854b54c0", 331, "2d3107fa854b54c0", nullptr},
    {186, "80913c5b67cbf601", 157, "80913c5b67cbf601", "selection message wider than GC (360 > 157 px): left as is"},
    {187, "81cf275b44770c24", 175, "e4af49e159f3ce85", "lines re-wrapped to the GC window width (widest 657 -> 167 px, limit 175; 13 -> 21 lines, 6 pages)"},
    {188, "d27d4a0b41296f5b", 205, "11cd52f4e1ba2fc8", "lines re-wrapped to the GC window width (widest 382 -> 194 px, limit 205; 1 -> 2 lines, 1 page)"},
    {189, "0f8997b920aab026", 183, "0f8997b920aab026", "selection message wider than GC (590 > 183 px): left as is"},
    {190, "58562c254f238761", 223, "58562c254f238761", nullptr},
    {191, "7eca9db8fb52cafb", 325, "7eca9db8fb52cafb", "selection message wider than GC (764 > 325 px): left as is"},
    {192, "35e59ba0a150a1d1", 244, "35e59ba0a150a1d1", "selection message wider than GC (389 > 244 px): left as is"},
    {193, "e9b8b53f761fa14a", 76, "0421670e9a992ace", "lines re-wrapped to the GC window width (widest 601 -> 204 px, limit 76; 7 -> 19 lines, 5 pages)"},
    {194, "6c13c7d731485670", 295, "f4ab859a67a02c9a", "lines re-wrapped to the GC window width (widest 776 -> 284 px, limit 295; 23 -> 27 lines, 7 pages)"},
    {195, "e9efa4bbc5e8e0bf", 305, "6e43d1c5b1a7f4eb", "lines re-wrapped to the GC window width (widest 384 -> 246 px, limit 305; 8 -> 12 lines, 3 pages)"},
    {196, "6cc56730cb07c9a1", 79, "6cc56730cb07c9a1", "selection message wider than GC (437 > 79 px): left as is"},
    {197, "77c67f9af710673c", 324, "6fa6d1e182e82f7c", "lines re-wrapped to the GC window width (widest 793 -> 300 px, limit 324; 5 -> 7 lines, 2 pages)"},
    {198, "3921448f88cc5c9d", 146, "292e4def840e6f6d", "lines re-wrapped to the GC window width (widest 669 -> 246 px, limit 146; 15 -> 19 lines, 5 pages)"},
    {199, "7a4c550235b6646b", 327, "7a4c550235b6646b", "selection message wider than GC (1410 > 327 px): left as is"},
    {200, "2ead93aed0846df7", 348, "6c7a3adc67fca373", "lines re-wrapped to the GC window width (widest 761 -> 326 px, limit 348; 10 -> 14 lines, 4 pages)"},
};
constexpr int kRewrapRandomChanged = 121;
constexpr const char* kLimitBmgSha[2] = {"eb4ee3b2764a3889d7155e1895d87a5698455ee58fab59987f2d1b0aa4aa2239", "a166a70714becf9444128109ae22036eeebf7c0847387484ab98741908673083"};
const std::pair<int, int> kLimits[] = {{0, 1184}, {1, 1184}, {2, 898}, {5, 1005}, {7, 1294}, {8, 1184}, {9, 1184}, {13, 1089}, {14, 747}, {15, 867}, {16, 1184}};
constexpr std::size_t kGcCharCount = 1;
constexpr const char* kFlowBmgSha = "c1ae626a1be04c2a7ca934a8368fb19a972f965fe24da74e6c4284a7c195d8b9";
const std::pair<u32, u16> kFlows[] = {{100, 0}, {200, 12}, {300, 14}, {50, 8}, {60, 16}};
struct Reach { u32 start; std::vector<u16> order; };
const Reach kReach[] = {
    {0, {0, 1, 2, 4, 5, 6, 7, 3}},
    {1, {1, 2, 4, 5, 6, 7, 3}},
    {4, {4, 5, 6, 7}},
    {9, {9, 1, 2, 4, 5, 6, 7, 3}},
    {12, {12, 11, 10, 9, 1, 2, 4, 5, 6, 7, 3}},
    {14, {14, 13}},
    {8, {8}},
    {16, {16}},
    {99, {}},
    {65535, {}},
};
const std::pair<u32, std::vector<u32>> kPred[] = {
    {1, {0, 9}},
    {2, {1}},
    {3, {1}},
    {4, {2}},
    {5, {4}},
    {6, {4}},
    {7, {6}},
    {9, {9, 10}},
    {10, {11}},
    {11, {12}},
    {13, {14}},
    {65535, {4, 5}},
};
struct Hood { u32 node; int back; std::vector<u32> keep; std::vector<u32> flows; };
const Hood kHood[] = {
    {4, 6, {0, 1, 2, 4, 5, 6, 7, 9, 10, 11, 12}, {100, 200}},
    {10, 6, {1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12}, {200}},
    {13, 6, {13, 14}, {300}},
    {7, 1, {6, 7}, {100, 200}},
    {11, 0, {1, 2, 3, 4, 5, 6, 7, 9, 10, 11}, {200}},
    {16, 6, {16}, {60}},
};
const char* const kNodeJson[] = {
    "{\"n\":0,\"type\":\"message\",\"subtype\":0,\"hd_entry\":0,\"msg_id\":7001,\"next\":1,\"unknown\":0,\"message\":\"k:7001\",\"action\":\"native\",\"gc_index\":3,\"raw\":\"0100000000010000\"}",
    "{\"n\":1,\"type\":\"branch\",\"query\":1,\"query_name\":\"FLOW_QUERY_EVENT_FLAG\",\"param\":110,\"results\":[2,3],\"hd_only\":false,\"label\":{\"index\":110,\"hd_only\":false,\"name\":\"M_077\",\"bit\":\"0x0d04\",\"desc\":\"shadow crystal obtained (Link can transform)\"},\"raw\":\"02020001006e0000\"}",
    "{\"n\":2,\"type\":\"event\",\"event\":0,\"event_name\":\"FLOW_EVENT_SET_EVENT_FLAG\",\"params\":\"033d006e\",\"p0\":829,\"p1\":110,\"hd_only\":true,\"next\":4,\"labels\":[{\"index\":829,\"hd_only\":true,\"bit\":\"0x7080\",\"meaning\":\"arrival intro pending (entry bookkeeping; R19 TagEv valid flag; flow 1120 clears it)\"},{\"index\":110,\"hd_only\":false,\"name\":\"M_077\",\"bit\":\"0x0d04\",\"desc\":\"shadow crystal obtained (Link can transform)\"}],\"raw\":\"03000002033d006e\"}",
    "{\"n\":3,\"type\":\"event\",\"event\":9,\"event_name\":\"FLOW_EVENT_JUMP_FLOW\",\"params\":\"00010002\",\"p0\":1,\"p1\":2,\"hd_only\":false,\"next\":null,\"jump_flow\":65538,\"raw\":\"0309000000010002\"}",
    "{\"n\":4,\"type\":\"branch\",\"query\":54,\"query_name\":\"HD_QUERY055\",\"param\":78,\"results\":[5,null,6],\"hd_only\":true,\"note\":\"HD_QUERY055 (HD 0x02a64650: 1 if !0x7001, 2 if 0x7001&&0x5420&&!0x7002, 3 if 0x7002&&0x7101&&!0x7004, else 0)\",\"raw\":\"02030036004e0003\"}",
    "{\"n\":5,\"type\":\"message\",\"subtype\":0,\"hd_entry\":5,\"msg_id\":null,\"next\":null,\"unknown\":65280,\"message\":null,\"gc_index\":null,\"raw\":\"01000005ffffff00\"}",
    "{\"n\":6,\"type\":\"event\",\"event\":44,\"event_name\":\"HD_EVENT044\",\"params\":\"00000000\",\"p0\":0,\"p1\":0,\"hd_only\":true,\"next\":7,\"note\":\"HD_EVENT044 Withdraw (HD 0x02a65c84: warp sounds, then the game-over/reset path; no flags)\",\"raw\":\"032c000600000000\"}",
    "{\"n\":7,\"type\":\"event\",\"event\":1,\"event_name\":\"FLOW_EVENT_CLEAR_EVENT_FLAG\",\"params\":\"00000336\",\"p0\":0,\"p1\":822,\"hd_only\":true,\"next\":null,\"labels\":[{\"index\":822,\"hd_only\":true,\"bit\":\"0x7008\",\"meaning\":\"Great Fairy reward given (flow 1122)\"}],\"raw\":\"0301ffff00000336\"}",
    "{\"n\":8,\"type\":\"type7\",\"raw\":\"0701020304050607\"}",
    "{\"n\":9,\"type\":\"branch\",\"query\":60,\"query_name\":\"HD_QUERY061\",\"param\":1,\"results\":[9,1],\"hd_only\":true,\"note\":\"HD-only query\",\"raw\":\"0202003c00010007\"}",
    "{\"n\":10,\"type\":\"message\",\"subtype\":1,\"hd_entry\":1,\"msg_id\":7002,\"next\":9,\"unknown\":0,\"message\":\"k:7002\",\"action\":\"native\",\"gc_index\":3,\"raw\":\"0101000100090000\"}",
    "{\"n\":11,\"type\":\"branch\",\"query\":1,\"query_name\":\"FLOW_QUERY_EVENT_FLAG\",\"param\":822,\"results\":[10],\"hd_only\":true,\"label\":{\"index\":822,\"hd_only\":true,\"bit\":\"0x7008\",\"meaning\":\"Great Fairy reward given (flow 1122)\"},\"raw\":\"0201000103360009\"}",
    "{\"n\":12,\"type\":\"event\",\"event\":45,\"event_name\":\"HD_EVENT045\",\"params\":\"00010002\",\"p0\":1,\"p1\":2,\"hd_only\":true,\"next\":11,\"note\":\"HD_EVENT045 (HD 0x02a65f74: demo unsuspend by its parameter; flow 1120 no-message path)\",\"raw\":\"032d000a00010002\"}",
    "{\"n\":13,\"type\":\"event\",\"event\":5,\"event_name\":\"FLOW_EVENT_REMOVE_HEARTS\",\"params\":\"00000000\",\"p0\":0,\"p1\":0,\"hd_only\":false,\"next\":null,\"raw\":\"0305006300000000\"}",
    "{\"n\":14,\"type\":\"branch\",\"query\":5,\"query_name\":\"FLOW_QUERY_TALK_DISTANCE\",\"param\":0,\"results\":[13,null],\"hd_only\":false,\"raw\":\"020200050000000b\"}",
    "{\"n\":15,\"type\":\"event\",\"event\":43,\"event_name\":\"HD_EVENT043\",\"params\":\"00000000\",\"p0\":0,\"p1\":0,\"hd_only\":true,\"next\":null,\"note\":\"HD_EVENT043 (HD 0x02a65c30; only on an unreachable node)\",\"raw\":\"032bffff00000000\"}",
    "{\"n\":16,\"type\":\"event\",\"event\":0,\"event_name\":\"FLOW_EVENT_SET_EVENT_FLAG\",\"params\":\"004e0000\",\"p0\":78,\"p1\":0,\"hd_only\":false,\"next\":null,\"labels\":[{\"index\":78,\"hd_only\":false,\"name\":\"M_045\",\"bit\":\"0x0904\",\"desc\":\"Lakebed Temple cleared\"}],\"raw\":\"0300ffff004e0000\"}",
};
constexpr const char* kHdOnlyJson = "[\"event FLOW_EVENT_SET_EVENT_FLAG @2\",\"branch HD_QUERY055 @4\",\"event HD_EVENT044 @6\",\"event FLOW_EVENT_CLEAR_EVENT_FLAG @7\",\"branch HD_QUERY061 @9\",\"branch FLOW_QUERY_EVENT_FLAG @11\",\"event HD_EVENT045 @12\",\"event HD_EVENT043 @15\"]";
constexpr const char* kMsgHdHex =
    "4d455347626d6731000003800000000201000000000000000000000000000000494e463100000100000b0014000000000000"
    "0001000a00000000000000000000000000000000000e000b00000000000003000000000000000000001c000c000004090000"
    "000000000000000000000029000d000002000000000000000000000000000035000e033e0000000000000000000000000000"
    "0045000f0000000000000000000000000000000000e300100000000000000000000000000000000000e40012000000000000"
    "0000000000000000000001720013000000070000000000000000000000000200001402bc0009000000000000000000000000"
    "022000150000000000000000000000000000000000000000000000000000000000000000000044415431000002600048656c"
    "6c6f2074686572652e004368616e67656420746578742e0057686f2061726520796f753f00596f7520676f7420697421004c"
    "6162656c20746f6f20686967682e001a09030000000012345072657373201a0507000620636166e920e8201a05070063616e"
    "64201a050000102c201a0500000f2c206f72201a0500000e201a050900011a07ff00000102201a050700092093719420a920"
    "8520616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d77726170706564206279"
    "207468652077696e646f772077696474682072756c650000616e642074686973206c696e65206973206c6f6e6720656e6f75"
    "676820746f2062652072652d77726170706564206279207468652077696e646f772077696474682072756c6520616e642074"
    "686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d7772617070656420627920746865207769"
    "6e646f772077696474682072756c6500616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f206265"
    "2072652d77726170706564206279207468652077696e646f772077696474682072756c6520616e642074686973206c696e65"
    "206973206c6f6e6720656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f7720776964"
    "74682072756c65001a09030000000000015265766f20746167206f6e6c7920646966666572732e0049636f6e73201a050000"
    "0a201a05030006201a0506000a1a0500002220818d00000000000000000000000000000000000000000000000000"
    ;
constexpr const char* kMsgGcHex =
    "4d455347626d6731000001c00000000201000000000000000000000000000000494e4631000000a000060014000000000000"
    "0001000a00000000000000000000000000000000000e000b00000000000000000000000000000000001d000c000005090000"
    "000000000000000000000022000d000001000000000000000000000000000030001200000000000000000000000000000000"
    "00c7001402bc0009000000000000000000000000000000000000000000000000000000000000000000004441543100000100"
    "0048656c6c6f2074686572652e004f726967696e616c20746578742e0057686f3f00596f7520676f74207468617421001a09"
    "03000000000007616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d7772617070"
    "6564206279207468652077696e646f772077696474682072756c6520616e642074686973206c696e65206973206c6f6e6720"
    "656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f772077696474682072756c650052"
    "65766f20746167206f6e6c7920646966666572732e000000000000000000000000000000000000000000000000000000"
    ;
struct MsgCase { u16 id; const char* use; bool none; const char* action; const char* entry; const char* text;
                 int gc_index; const char* preview; std::vector<const char*> notes; };
const MsgCase kMsgCases[] = {
    {10, "ui", false, "native", "0000000000000000000000000000000000000000",
     "48656c6c6f2074686572652e00",
     0, "Hello there.",
     {}},
    {11, "ui", false, "override", "0000000000000000000000000300000000000000",
     "4368616e6765640a746578742e00",
     1, "Changed text.",
     {"lines re-wrapped to the GC window width (widest 172 -> 89 px, limit 160; 1 -> 2 lines, 1 page)"}},
    {12, "flow", false, "register", "0000000000000000040900000000000000000000",
     "57686f206172650a796f753f00",
     2, "Who are you?",
     {"lines re-wrapped to the GC window width (widest 174 -> 109 px, limit 140; 1 -> 2 lines, 1 page)"}},
    {13, "item-get", false, "override", "0000000000000000020000000000000000000000",
     "596f7520676f742069742100",
     3, "You got it!",
     {}},
    {14, "ui", false, "register", "0000000000000000000000000000000000000000",
     "4c6162656c20746f6f0a686967682e00",
     -1, "Label too high.",
     {"lines re-wrapped to the GC window width (widest 195 -> 116 px, limit 160; 1 -> 2 lines, 1 page)", "INF1 event_label 830 >= 822 -> 0"}},
    {15, "ui", false, "register", "0000000000000000000000000000000000000000",
     "5072657373201a0500000d0a6361666520e820616e640a1a05000010206f72201a0500000f0a5354415254202271220a286329202e2e2e0a616e642074686973206c696e650a6973206c6f6e6720656e6f7567680a746f2062650a72652d777261707065640a6279207468650a77696e646f772077696474680a72756c6500",
     -1, "Press [L] cafe \xc3""\xa8"" and [Y] or [X]  START \"q\" (c) ... and this line is long enough to be re-wrapped by the window width rule",
     {"strip {3,0} Wii message-id override (00001234)", "HD {7,6} (ZL) -> GC {0,13} L", "remove unknown HD {7,99}", "HD {7,9} (+ button) -> \"START\" (GC wording)", "HD \"{0,16}, {0,15}, or {0,14}\" (Y/X/R) -> GC \"{0,16} or {0,15}\"", "char 0xe9 -> 'e'", "remove tag {9,1} unknown to v2.0.3", "remove tag {255,0} unknown to v2.0.3", "char 0x93 -> '\"'", "char 0x94 -> '\"'", "char 0xa9 -> '(c)'", "char 0x85 -> '...'", "lines re-wrapped to the GC window width (widest 1450 -> 168 px, limit 160; 1 -> 12 lines, 3 pages)"}},
    {16, "ui", true, "", "", "", -1, "", {}},
    {17, "ui", true, "", "", "", -1, "", {}},
    {18, "ui", false, "native", "0000000000000000000000000000000000000000",
     "616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f772077696474682072756c6520616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f772077696474682072756c6500",
     4, "and this line is long enough to be re-wrapped by the window width rule and this line is long enough to be re-wrapped by the window width rule",
     {}},
    {19, "ui", false, "register", "0000000000000000000700000000000000000000",
     "616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f772077696474682072756c6520616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f772077696474682072756c6500",
     -1, "and this line is long enough to be re-wrapped by the window width rule and this line is long enough to be re-wrapped by the window width rule",
     {}},
    {20, "shop", false, "native", "00000000000002bc000900000000000000000000",
     "5265766f20746167206f6e6c7920646966666572732e00",
     5, "Revo tag only differs.",
     {"strip {3,0} Wii message-id override (00000001)"}},
    {21, "ui", false, "register", "0000000000000000000000000000000000000000",
     "49636f6e73201a0500000a201a050300060a1a0506000a1a05000022203f3f00",
     -1, "Icons [A] [D-Pad Up] * {horse} ??",
     {"char 0x81 -> '?'", "char 0x8d -> '?'", "lines re-wrapped to the GC window width (widest 278 -> 138 px, limit 160; 1 -> 2 lines, 1 page)"}},
};
constexpr const char* kBinHex =
    "48445458010000000a00000000000a0000000d00000000000000000000000000000000000000000048656c6c6f2074686572"
    "652e0000000000000b0001000e0000000000000000000000000003000000000000004368616e6765640a746578742e000000"
    "00000c0002000d00000000000000000004090000000000000000000057686f206172650a796f753f0000000000000d000100"
    "0c000000000000000000020000000000000000000000596f7520676f74206974210000000e00020010000000000000000000"
    "0000000000000000000000004c6162656c20746f6f0a686967682e0000000f0002007f000000000000000000000000000000"
    "0000000000005072657373201a0500000d0a6361666520e820616e640a1a05000010206f72201a0500000f0a535441525420"
    "2271220a286329202e2e2e0a616e642074686973206c696e650a6973206c6f6e6720656e6f7567680a746f2062650a72652d"
    "777261707065640a6279207468650a77696e646f772077696474680a72756c6500000000120000008e000000000000000000"
    "000000000000000000000000616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d"
    "77726170706564206279207468652077696e646f772077696474682072756c6520616e642074686973206c696e6520697320"
    "6c6f6e6720656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f772077696474682072"
    "756c650000000000130002008e000000000000000000000700000000000000000000616e642074686973206c696e65206973"
    "206c6f6e6720656e6f75676820746f2062652072652d77726170706564206279207468652077696e646f7720776964746820"
    "72756c6520616e642074686973206c696e65206973206c6f6e6720656e6f75676820746f2062652072652d77726170706564"
    "206279207468652077696e646f772077696474682072756c65000000000014000000170000000000000002bc000900000000"
    "0000000000005265766f20746167206f6e6c7920646966666572732e00000000150002002000000000000000000000000000"
    "000000000000000049636f6e73201a0500000a201a050300060a1a0506000a1a05000022203f3f00"
    ;
constexpr const char* kTagKnown =  // groups (0, 1, 2, 3, 4, 5, 6, 7, 255) x ids 0-69
    "1111111111111111111111111111111111111111111111111111111111111111000000"
    "1111111111111111111111111111111111111111111111111111111111111111111111"
    "1111111111111111111111111111111111111111111111111111111111111111111111"
    "1111111111111111111110000000000000000000000000000000000000000000000000"
    "1111111111111100000000000000000000000000000000000000000000000000000000"
    "1001000111111110000000000000000000000000000000000000000000000000000000"
    "1111111111110000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000"
    "1110000000000000000000000000000000000000000000000000000000000000000000"
    ;
const std::pair<const char*, bool> kValid[] = {{"", false}, {"00", true}, {"6100", true}, {"61", false}, {"61006200", false}, {"1a0500000000", true}, {"1a0400000000", false}, {"1a06ff00000100", true}, {"1a07ff0000010200", false}, {"1a09ff00000102030400", true}, {"1a05ff000000", false}, {"1a07ff0001010200", true}, {"1a2000000000", false}, {"1a", false}};
// ---- end of vectors

Bytes unhex(std::string_view h) {
    Bytes b;
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(static_cast<u8>(v(h[i]) * 16 + v(h[i + 1])));
    return b;
}

std::string hex(View b) { return hdf::to_hex(b); }

// golden_text.Lcg
struct Lcg {
    u64 s;
    explicit Lcg(u64 seed) : s(seed) {}
    u32 operator()(u32 n) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>((s >> 33) % n);
    }
};

const char* const kWords[] = {"the", "a", "Link", "Rupee", "Rupees", "50", "1,000", "sword", "and", "shield", "cave",
                              "of", "shadows", "go", "now", "fairy", "heart", "piece", "you", "got", "it", "wolf",
                              "Midna", "is", "very", "longerwordhere", "x", "\xe9t\xe9"};

void put(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }

// golden_text.rand_text
Bytes rand_text(Lcg& r) {
    Bytes out;
    const u32 n = 3 + r(70);
    for (u32 k = 0; k < n; ++k) {
        const u32 x = r(100);
        if (x < 55) {
            put(out, kWords[r(static_cast<u32>(std::size(kWords)))]);
            out.push_back(' ');
        } else if (x < 63) {
            out.push_back(".!?,"[r(4)]);
            out.push_back(' ');
        } else if (x < 71) {
            out.push_back('\n');
        } else if (x < 74) {
            put(out, std::string_view("\x1a\x05\x00\x00\x00", 5));
        } else if (x < 76) {
            put(out, std::string_view("\x1a\x05\x00\x00\x01", 5));
        } else if (x < 78) {
            put(out, std::string_view("\x1a\x05\x00\x00\x02", 5));
        } else if (x < 80) {
            put(out, std::string_view("\x1a\x06\xff\x00\x00\x06", 6));
        } else if (x < 82) {
            put(out, std::string_view("\x1a\x05\x00\x00\x0a", 5));
        } else if (x < 84) {
            put(out, std::string_view("\x1a\x06\xff\x00\x00\x01", 6));
        } else if (x < 85) {
            put(out, std::string_view("\x1a\x05\x00\x00\x08", 5));
        } else if (x < 90) {
            put(out, "  ");
        } else {
            put(out, "\n\n");
        }
    }
    out.push_back(0);
    return out;
}

void be16(Bytes& b, u32 v) {
    b.push_back(static_cast<u8>(v >> 8));
    b.push_back(static_cast<u8>(v));
}
void be32(Bytes& b, u32 v) {
    be16(b, v >> 16);
    be16(b, v & 0xFFFF);
}

// golden_text.attrs
Bytes attrs(u8 box = 0, u8 speaker = 0, u16 label = 0, u8 item_no = 0) {
    Bytes a(14, 0);
    a[0] = static_cast<u8>(label >> 8);
    a[1] = static_cast<u8>(label);
    a[2] = speaker;
    a[3] = box;
    a[6] = item_no;
    return a;
}

struct BmgEntry {
    u16 id;
    Bytes attrs;
    Bytes text;
};

// golden_text.synth_bmg
Bytes synth_bmg(const std::vector<BmgEntry>& entries, const std::vector<Bytes>* nodes = nullptr,
                const std::vector<u16>* edges = nullptr, const std::vector<std::pair<u32, u16>>* flows = nullptr) {
    auto sec = [](const char* tag, const Bytes& body) {
        Bytes b(tag, tag + 4);
        be32(b, 0);
        b.insert(b.end(), body.begin(), body.end());
        while (b.size() % 32) b.push_back(0);
        const u32 n = static_cast<u32>(b.size());
        b[4] = static_cast<u8>(n >> 24);
        b[5] = static_cast<u8>(n >> 16);
        b[6] = static_cast<u8>(n >> 8);
        b[7] = static_cast<u8>(n);
        return b;
    };
    Bytes dat{0}, inf;
    for (const BmgEntry& e : entries) {
        be32(inf, static_cast<u32>(dat.size()));
        be16(inf, e.id);
        inf.insert(inf.end(), e.attrs.begin(), e.attrs.end());
        dat.insert(dat.end(), e.text.begin(), e.text.end());
    }
    std::vector<Bytes> secs;
    Bytes ib;
    be16(ib, static_cast<u32>(entries.size()));
    be16(ib, 20);
    be32(ib, 0);
    ib.insert(ib.end(), inf.begin(), inf.end());
    secs.push_back(sec("INF1", ib));
    secs.push_back(sec("DAT1", dat));
    if (nodes) {
        Bytes fb;
        be16(fb, static_cast<u32>(nodes->size()));
        be16(fb, static_cast<u32>(edges->size()));
        be32(fb, 0);
        for (const Bytes& n : *nodes) fb.insert(fb.end(), n.begin(), n.end());
        for (u16 e : *edges) be16(fb, e);
        secs.push_back(sec("FLW1", fb));
    }
    if (flows) {
        Bytes lb;
        be16(lb, static_cast<u32>(flows->size()));
        be16(lb, 8);
        be32(lb, 0);
        for (const auto& [v, i] : *flows) {
            be32(lb, v);
            be16(lb, i);
            be16(lb, 0);
        }
        secs.push_back(sec("FLI1", lb));
    }
    Bytes body;
    for (const Bytes& s : secs) body.insert(body.end(), s.begin(), s.end());
    Bytes out = {'M', 'E', 'S', 'G', 'b', 'm', 'g', '1'};
    be32(out, static_cast<u32>(0x20 + body.size()));
    be32(out, static_cast<u32>(secs.size()));
    out.push_back(1);
    out.resize(0x20, 0);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// golden_text.rand_bmg
Bytes rand_bmg(u64 seed, u32 count, const std::vector<u8>& kinds) {
    Lcg r(seed);
    std::vector<BmgEntry> e;
    for (u32 i = 0; i < count; ++i) {
        const u8 box = kinds[r(static_cast<u32>(kinds.size()))];
        e.push_back({static_cast<u16>(i + 1), attrs(box), rand_text(r)});
    }
    e.push_back({static_cast<u16>(count + 1), attrs(kinds[0]), Bytes{'a', 'b', 'c', 0x1a, 0x09, 0x00}});
    e.push_back({static_cast<u16>(count + 2), attrs(kinds[0]), Bytes{0}});
    return synth_bmg(e);
}

Bytes nd(std::initializer_list<int> v) {
    Bytes b;
    for (int x : v) b.push_back(static_cast<u8>(x));
    return b;
}
#define U16(v) ((v) >> 8) & 0xFF, (v) & 0xFF

// golden_text.FLOW_NODES / FLOW_EDGES / FLOW_FLI
Bytes flow_bmg() {
    static const std::vector<Bytes> nodes = {
        nd({1, 0, U16(0), U16(1), U16(0)}),         nd({2, 2, U16(1), U16(110), U16(0)}),
        nd({3, 0, U16(2), U16(829), U16(110)}),     nd({3, 9, 0, 0, 0, 1, 0, 2}),
        nd({2, 3, U16(54), U16(78), U16(3)}),       nd({1, 0, U16(5), U16(0xFFFF), U16(0xFF00)}),
        nd({3, 44, U16(6), 0, 0, 0, 0}),            nd({3, 1, U16(0xFFFF), 0, 0, U16(822)}),
        nd({7, 1, 2, 3, 4, 5, 6, 7}),               nd({2, 2, U16(60), U16(1), U16(7)}),
        nd({1, 1, U16(1), U16(9), U16(0)}),         nd({2, 1, U16(1), U16(822), U16(9)}),
        nd({3, 45, U16(10), U16(1), U16(2)}),       nd({3, 5, U16(99), 0, 0, 0, 0}),
        nd({2, 2, U16(5), 0, 0, U16(11)}),          nd({3, 43, U16(0xFFFF), 0, 0, 0, 0}),
        nd({3, 0, U16(0xFFFF), 0, 78, 0, 0}),
    };
    static const std::vector<u16> edges = {2, 3, 4, 5, 0xFFFF, 6, 7, 9, 1, 10, 11, 13};
    static const std::vector<std::pair<u32, u16>> fli = {{100u << 16 | 1, 0}, {200u << 16, 12}, {300u << 16 | 7, 14},
                                                         {100u << 16, 5},     {50u << 16, 8},   {60u << 16, 16}};
    return synth_bmg({{7001, attrs(), Bytes{'O', 'n', 'e', 0}}, {7002, attrs(), Bytes{'T', 'w', 'o', 0}}}, &nodes,
                     &edges, &fli);
}

td::FontMetrics font() {
    auto fm = td::font_metrics(unhex(kBfnHex));
    HDF_CHECK(fm.has_value());
    return fm ? *fm : td::FontMetrics{};
}

io::JsonStyle compact() { return io::JsonStyle{.indent = -1, .ensure_ascii = false, .item_sep = ",", .key_sep = ":"}; }

}  // namespace

HDF_TEST(text_ascii_table_and_cp1252) {
    std::string all;
    for (int c = 0x80; c < 0x100; ++c) {
        if (c > 0x80) all += '\n';
        all += td::ascii_fallback(static_cast<u8>(c));
    }
    HDF_CHECK_EQ(hdf::sha256_hex(View(reinterpret_cast<const u8*>(all.data()), all.size())),
                 std::string("9f83d410e5371ed927d4462eda3ebec167f5aa2ae5a9bd3cc0eaca4a0c1332a6"));
    HDF_CHECK(td::ascii_fallback(0xA9) == "(c)");
    HDF_CHECK(td::ascii_fallback(0xE9) == "e");
    HDF_CHECK(td::cp1252(0x80) == U'€');
    HDF_CHECK(td::cp1252(0x81) == U'�');
    HDF_CHECK(td::cp1252(0x9F) == U'Ÿ');
    HDF_CHECK(td::cp1252(0xE9) == U'é');
    HDF_CHECK(td::cp1252('A') == U'A');
}

HDF_TEST(text_tokens_encode) {
    const Bytes t = {'H', 'i', 0x1a, 0x07, 0xff, 0x00, 0x00, 0x01, 0x02, '!', 0, 'x', 0};
    auto toks = td::tokens(t);
    HDF_CHECK(toks.has_value());
    if (!toks) return;
    HDF_CHECK_EQ(toks->size(), std::size_t(4));  // stops at the NUL
    HDF_CHECK((*toks)[2] == td::Tok::t(255, 0, Bytes{1, 2}));
    HDF_CHECK_EQ(hex(td::encode(*toks)), hex(Bytes(t.begin(), t.begin() + 11)));
    HDF_CHECK(!td::tokens(Bytes{'a', 0x1a, 0x09, 0x00}).has_value());  // truncated control tag
    HDF_CHECK(!td::tokens(Bytes{0x1a, 0x04, 0, 0, 0}).has_value());     // length < 5
    HDF_CHECK(td::tokens(Bytes{}).has_value());
    HDF_CHECK_EQ(td::show(t), std::string("Hi{255,0:0102}!"));
}

HDF_TEST(text_gc_tag_known_and_valid_encoded) {
    const int groups[] = {0, 1, 2, 3, 4, 5, 6, 7, 255};
    std::string got;
    for (int g : groups)
        for (int i = 0; i < 70; ++i) got += td::gc_tag_known(static_cast<u8>(g), static_cast<u16>(i)) ? '1' : '0';
    HDF_CHECK_EQ(got, std::string(kTagKnown));
    for (const auto& [h, want] : kValid) HDF_CHECK_EQ(td::valid_encoded_text(unhex(h)), want);
}

HDF_TEST(text_font_metrics) {
    const td::FontMetrics fm = font();
    HDF_CHECK_EQ(fm.cell, kFontCell);
    HDF_CHECK_EQ(fm.cell_w, kFontCellW);
    HDF_CHECK(fm.name == kFontName);
    int bad = 0;
    for (int c = 0; c < 256; ++c)
        if (!(fm.chars[static_cast<std::size_t>(c)] == kFontChars[c]) || !(fm.char_w(c) == kFontChars[c])) ++bad;
    HDF_CHECK_EQ(bad, 0);
    HDF_CHECK(!td::font_metrics(Bytes{'F', 'O', 'N', 'T'}).has_value());
}

// FontMetrics.width is a plain left-to-right sum (hdtext.py:568): these 4 texts round differently with math.fsum.
HDF_TEST(text_width_is_a_sequential_sum) {
    const td::FontMetrics fm = font();
    for (const auto& c : kWidthCases) {
        td::Toks toks;
        for (u8 b : unhex(c.hex)) toks.push_back(td::Tok::ch(b));
        HDF_CHECK_EQ(fm.width(toks), c.sequential);
        HDF_CHECK(c.sequential != c.compensated);
    }
}

HDF_TEST(text_rewrap_handmade) {
    const td::FontMetrics fm = font();
    for (const auto& c : kRewrapHand) {
        auto r = td::rewrap(unhex(c.in), c.limit, fm);
        HDF_CHECK(r.has_value());
        if (!r) continue;
        HDF_CHECK_EQ(hex(r->text), std::string(c.out));
        HDF_CHECK_EQ(r->note.value_or("None"), std::string(c.note ? c.note : "None"));
    }
}

HDF_TEST(text_rewrap_random_200) {
    const td::FontMetrics fm = font();
    int changed = 0;
    for (const auto& c : kRewrapRandom) {
        Lcg r(c.seed);
        const Bytes text = rand_text(r);
        const int limit = 60 + static_cast<int>(r(300));
        HDF_CHECK_EQ(hdf::sha256_hex(text).substr(0, 16), std::string(c.in16));
        HDF_CHECK_EQ(limit, c.limit);
        auto w = td::rewrap(text, limit, fm);
        if (std::string(c.out16) == "error") {
            HDF_CHECK(!w.has_value());
            continue;
        }
        HDF_CHECK(w.has_value());
        if (!w) continue;
        changed += w->text != text;
        HDF_CHECK_EQ(hdf::sha256_hex(w->text).substr(0, 16), std::string(c.out16));
        HDF_CHECK_EQ(w->note.value_or("None"), std::string(c.note ? c.note : "None"));
    }
    HDF_CHECK_EQ(changed, kRewrapRandomChanged);
}

HDF_TEST(text_gc_line_limits) {
    const td::FontMetrics fm = font();
    const Bytes b1 = rand_bmg(7, 140, {0, 0, 0, 0, 9, 9, 13, 1, 2, 7, 5});
    const Bytes b2 = rand_bmg(8, 40, {0, 14, 15});
    HDF_CHECK_EQ(hdf::sha256_hex(b1), std::string(kLimitBmgSha[0]));
    HDF_CHECK_EQ(hdf::sha256_hex(b2), std::string(kLimitBmgSha[1]));
    auto m1 = td::parse_bmg(b1, "b1");
    auto m2 = td::parse_bmg(b2, "b2");
    HDF_CHECK(m1.has_value() && m2.has_value());
    if (!m1 || !m2) return;
    const auto lim = td::gc_line_limits({&*m1, &*m2}, fm);
    std::vector<std::pair<int, int>> got(lim.limit.begin(), lim.limit.end());
    HDF_CHECK((got == std::vector<std::pair<int, int>>(std::begin(kLimits), std::end(kLimits))));
    std::set<u8> chars;
    for (const td::Bmg* b : {&*m1, &*m2})
        for (const td::Entry& e : b->entries)
            if (auto t = td::tokens(e.text))
                for (const td::Tok& k : *t)
                    if (!k.tag && k.c >= 0x80) chars.insert(k.c);
    HDF_CHECK_EQ(chars.size(), kGcCharCount);
}

HDF_TEST(text_flows) {
    const Bytes fb = flow_bmg();
    HDF_CHECK_EQ(hdf::sha256_hex(fb), std::string(kFlowBmgSha));
    auto b = td::parse_bmg(fb, "flows");
    HDF_CHECK(b.has_value());
    if (!b) return;
    HDF_CHECK((b->flows == std::vector<std::pair<u32, u16>>(std::begin(kFlows), std::end(kFlows))));
    for (const auto& r : kReach) HDF_CHECK(td::reachable(*b, r.start) == r.order);
    const auto pred = td::predecessors(*b);
    HDF_CHECK((pred == std::map<u32, std::vector<u32>>(std::begin(kPred), std::end(kPred))));
    for (const auto& h : kHood) {
        HDF_CHECK(td::neighbourhood(*b, h.node, h.back) == h.keep);
        HDF_CHECK(td::flows_reaching(*b, h.node) == h.flows);
    }
    const td::MsgRef ref = [](std::optional<u16> mid) -> hdf::Result<io::JsonObject> {
        io::JsonObject r;
        if (!mid) {
            r.emplace_back("message", io::Json());
            r.emplace_back("gc_index", io::Json());
            return r;
        }
        r.emplace_back("message", "k:" + std::to_string(*mid));
        r.emplace_back("action", "native");
        r.emplace_back("gc_index", 3);
        return r;
    };
    io::JsonArray nodes;
    for (u32 i = 0; i < b->nodes.size(); ++i) {
        auto n = td::decode_node(*b, i, ref);
        HDF_CHECK(n.has_value());
        if (!n) return;
        HDF_CHECK_EQ(io::json_dumps(io::Json(*n), compact()), std::string(kNodeJson[i]));
        nodes.emplace_back(std::move(*n));
    }
    HDF_CHECK_EQ(io::json_dumps(io::Json(td::hd_only_summary(nodes)), compact()), std::string(kHdOnlyJson));
    HDF_CHECK(!td::decode_node(*b, 99, ref).has_value());
}

HDF_TEST(text_build_message_and_bin) {
    const td::FontMetrics fm = font();
    auto H = td::parse_bmg(unhex(kMsgHdHex), "hd");
    auto G = td::parse_bmg(unhex(kMsgGcHex), "gc");
    HDF_CHECK(H.has_value() && G.has_value());
    if (!H || !G) return;
    td::Ctx ctx;
    ctx.gc_chars = {0xE8};
    ctx.fm = &fm;
    ctx.limits.limit = {{0, 160}, {9, 140}};
    std::vector<td::Message> msgs;
    for (const auto& c : kMsgCases) {
        auto m = td::build_message(*H, *G, "zel_00", c.id, "cave", c.use, 0, ctx);
        HDF_CHECK(m.has_value());
        if (!m) continue;
        HDF_CHECK_EQ(m->has_value(), !c.none);
        if (!m->has_value() || c.none) continue;
        const td::Message& g = **m;
        HDF_CHECK_EQ(std::string(td::action_name(g.action)), std::string(c.action));
        HDF_CHECK_EQ(hex(g.entry), std::string(c.entry));
        HDF_CHECK_EQ(hex(g.text), std::string(c.text));
        HDF_CHECK_EQ(g.gc_index ? int(*g.gc_index) : -1, c.gc_index);
        HDF_CHECK_EQ(g.preview, std::string(c.preview));
        HDF_CHECK_EQ(g.notes.size(), c.notes.size());
        for (std::size_t i = 0; i < g.notes.size() && i < c.notes.size(); ++i)
            HDF_CHECK_EQ(g.notes[i], std::string(c.notes[i]));
        msgs.push_back(g);
    }
    HDF_CHECK_EQ(hex(td::write_bin(msgs)), std::string(kBinHex));
}

HDF_TEST(text_stage_msg_group) {
    auto dzs = [](bool with_stag, u8 group) {
        Bytes d;
        be32(d, 2);
        d.insert(d.end(), {'R', 'T', 'B', 'L'});
        be32(d, 0);
        be32(d, 28);
        d.insert(d.end(), {'S', 'T', 'A', 'G'});
        be32(d, 1);
        be32(d, with_stag ? 28 : 0);
        if (!with_stag) d[16] = 'X';
        d.resize(28 + 0x30, 0);
        d[28 + 0x28] = group;
        return d;
    };
    const std::vector<std::pair<std::string, Bytes>> e1 = {{"dzs/stage.dzs", dzs(true, 5)}};
    const Bytes arc = io::rarc_build("stg", e1);
    auto g = td::stage_msg_group(arc);
    HDF_CHECK(g.has_value() && g->has_value() && **g == 5);
    const std::vector<std::pair<std::string, Bytes>> e2 = {{"dzs/stage.dzs", dzs(false, 5)}};
    auto n = td::stage_msg_group(io::rarc_build("stg", e2));
    HDF_CHECK(n.has_value() && !n->has_value());
    const std::vector<std::pair<std::string, Bytes>> e3 = {{"dzs/other.dzs", dzs(true, 5)}};
    auto o = td::stage_msg_group(io::rarc_build("stg", e3));
    HDF_CHECK(o.has_value() && !o->has_value());
    Bytes cut = dzs(true, 5);
    cut.resize(30);
    const std::vector<std::pair<std::string, Bytes>> e4 = {{"dzs/stage.dzs", cut}};
    HDF_CHECK(!td::stage_msg_group(io::rarc_build("stg", e4)).has_value());
    HDF_CHECK(!td::stage_msg_group(Bytes{1, 2, 3}).has_value());
}

HDF_TEST(text_stamp_items_are_the_facts) {
    const auto v = td::stamp_items();
    HDF_CHECK(v.size() == hdf::facts::kStampItemIds.size() &&
              std::equal(v.begin(), v.end(), hdf::facts::kStampItemIds.begin()));
}

// Correction #1: corrupted bytes give an error or a value, never a crash.
HDF_TEST(text_corrupt_inputs_never_crash) {
    const td::FontMetrics fm = font();
    std::mt19937 rng(1234);
    const Bytes good[] = {flow_bmg(), unhex(kMsgHdHex), rand_bmg(9, 20, {0, 9})};
    int parsed = 0;
    for (int round = 0; round < 600; ++round) {
        Bytes b = good[round % 3];
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips; ++k) b[rng() % b.size()] = static_cast<u8>(rng());
        if (round % 5 == 0) b.resize(rng() % b.size());
        auto m = td::parse_bmg(b, "fuzz");
        if (!m) continue;
        ++parsed;
        for (const td::Entry& e : m->entries) (void)td::rewrap(e.text, 100, fm);
        for (u32 i = 0; i < m->nodes.size(); ++i)
            (void)td::decode_node(*m, i, [](std::optional<u16>) { return hdf::Result<io::JsonObject>(io::JsonObject{}); });
        for (const auto& [f, s] : m->flows) (void)td::reachable(*m, s);
        if (!m->nodes.empty()) (void)td::neighbourhood(*m, static_cast<u32>(rng() % m->nodes.size()));
    }
    HDF_CHECK(parsed > 0);
    const Bytes bfn = unhex(kBfnHex);
    for (int round = 0; round < 300; ++round) {
        Bytes b = bfn;
        for (int k = 0; k < 4; ++k) b[rng() % b.size()] = static_cast<u8>(rng());
        if (round % 3 == 0) b.resize(rng() % b.size());
        (void)td::font_metrics(b);
    }
    for (int round = 0; round < 500; ++round) {
        Bytes t(rng() % 120);
        for (u8& x : t) x = static_cast<u8>(rng() % 4 == 0 ? 0x1a : rng());
        (void)td::rewrap(t, 50 + static_cast<int>(rng() % 300), fm);
        (void)td::convert_text(t, {});
        (void)td::preview(t);
    }
}

HDF_TEST(text_build_refuses_missing_inputs) {
    hdf::MemorySource hd("hd"), gc("gc");
    auto r = hdf::text::build(hd, gc, {}, nullptr);
    HDF_CHECK(!r.has_value());
    std::stop_source ss;
    ss.request_stop();
    auto s = hdf::text::build(hd, gc, ss.get_token(), nullptr);
    HDF_CHECK(!s.has_value() && s.error().code == "stopped");
}
