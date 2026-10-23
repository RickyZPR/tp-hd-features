// Unit tests of module C1 (io) on synthetic data only. The expected values in io_vectors.inc were made by the
// Python spec (tools/dev/gen_io_vectors.py: yaz0.py, rarc.py, rarc_write.py, rarcx.py, common.png_bytes,
// zlib/gzip/json of Python 3.10). Robustness: random corruptions of valid data must give an error or a value,
// never a crash (Correction #1).
#include "test.hpp"

#include "hdf/io.hpp"
#include "hdf/source.hpp"

#include <zlib.h>

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

struct ReprCase {
    u64 bits;
    const char* repr;
};
struct RoundCase {
    u64 x;
    int n;
    u64 r;
};
struct RoundIntCase {
    u64 x;
    long long r;
};
struct DivCase {
    long long a, b, q, m;
};
struct MemberCase {
    const char* path;
    unsigned flags, offset, size, id;
};
struct EntryIndexCase {
    const char* key;
    int ei;
};

#include "io_vectors.inc"

Bytes unhex(std::string_view h) {
    Bytes out(h.size() / 2);
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<u8>((nib(h[2 * i]) << 4) | nib(h[2 * i + 1]));
    return out;
}

Bytes bytes_of(std::string_view s) { return Bytes(s.begin(), s.end()); }

double from_bits(u64 b) { return std::bit_cast<double>(b); }
u64 to_bits(double d) { return std::bit_cast<u64>(d); }

bool sha256_available() {
    return hdf::sha256_hex(bytes_of("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}

// gzip with zlib's own encoder (windowBits 31)
Bytes gzip_of(View in, int level = 6) {
    z_stream zs{};
    deflateInit2(&zs, level, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&zs, static_cast<uLong>(in.size())) + 32);
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

// A TMPK pack built by hand: names and data at the given places.
Bytes make_tmpk(const std::vector<std::pair<std::string, Bytes>>& files, u32 alignment = 0x40) {
    const u32 n = static_cast<u32>(files.size());
    Bytes b(0x10 + n * 16, 0);
    std::memcpy(b.data(), "TMPK", 4);
    io::put32(b, 4, n);
    io::put32(b, 8, alignment);
    std::vector<u32> name_off;
    for (const auto& f : files) {
        name_off.push_back(static_cast<u32>(b.size()));
        b.insert(b.end(), f.first.begin(), f.first.end());
        b.push_back(0);
    }
    for (u32 i = 0; i < n; ++i) {
        b.resize(static_cast<std::size_t>(io::align(b.size(), alignment)), 0);
        const u32 off = static_cast<u32>(b.size());
        b.insert(b.end(), files[i].second.begin(), files[i].second.end());
        io::put32(b, 0x10 + i * 16, name_off[i]);
        io::put32(b, 0x10 + i * 16 + 4, off);
        io::put32(b, 0x10 + i * 16 + 8, static_cast<u32>(files[i].second.size()));
    }
    return b;
}

// Flips random bytes / truncates; the callback must not crash.
template <class F>
void mutate(View good, int rounds, unsigned seed, F&& fn) {
    std::mt19937 rng(seed);
    for (int r = 0; r < rounds; ++r) {
        Bytes b(good.begin(), good.end());
        const int kind = static_cast<int>(rng() % 3);
        if (kind == 0 && !b.empty()) {
            b.resize(rng() % b.size());
        } else {
            const int flips = 1 + static_cast<int>(rng() % 6);
            for (int k = 0; k < flips && !b.empty(); ++k) b[rng() % b.size()] = static_cast<u8>(rng());
        }
        fn(View(b));
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------- Python numbers
HDF_TEST(io_py_float_repr_vectors) {
    for (const ReprCase& c : kRepr) HDF_CHECK_EQ(io::py_float_repr(from_bits(c.bits)), std::string(c.repr));
    HDF_CHECK_EQ(io::py_float_repr(0.0), std::string("0.0"));
    HDF_CHECK_EQ(io::py_float_repr(-0.0), std::string("-0.0"));
    HDF_CHECK_EQ(io::py_float_repr(INFINITY), std::string("inf"));
    HDF_CHECK_EQ(io::py_float_repr(-INFINITY), std::string("-inf"));
    HDF_CHECK_EQ(io::py_float_repr(NAN), std::string("nan"));
}

HDF_TEST(io_py_round_vectors) {
    int bad = 0;
    for (const RoundCase& c : kRound) {
        const double got = io::py_round(from_bits(c.x), c.n);
        if (to_bits(got) != c.r) {
            if (++bad <= 5)
                hdf::test::fail(__FILE__, __LINE__,
                                "round(" + io::py_float_repr(from_bits(c.x)) + ", " + std::to_string(c.n) + ") = " +
                                    io::py_float_repr(got) + ", Python " + io::py_float_repr(from_bits(c.r)));
        }
    }
    HDF_CHECK_EQ(bad, 0);
    HDF_CHECK(std::isinf(io::py_round(INFINITY, 2)));
    HDF_CHECK(std::isnan(io::py_round(NAN, 2)));
    HDF_CHECK_EQ(io::py_round(1.5, 400), 1.5);
    HDF_CHECK(to_bits(io::py_round(-1.5, -400)) == to_bits(-0.0));
}

HDF_TEST(io_py_round_int_and_floor) {
    for (const RoundIntCase& c : kRoundInt) HDF_CHECK_EQ(io::py_round_int(from_bits(c.x)), static_cast<hdf::i64>(c.r));
    for (const DivCase& c : kDiv) {
        HDF_CHECK_EQ(io::floor_div(c.a, c.b), static_cast<hdf::i64>(c.q));
        HDF_CHECK_EQ(io::floor_mod(c.a, c.b), static_cast<hdf::i64>(c.m));
    }
}

// ---------------------------------------------------------------------------------------------- JSON
namespace {
io::Json sample_json() {
    using io::Json;
    using io::JsonArray;
    using io::JsonObject;
    JsonArray list{Json(0), Json(-1), Json(static_cast<hdf::i64>(123456789012)), Json(true), Json(false), Json(nullptr),
                   Json(JsonArray{}), Json(JsonObject{}), Json(JsonArray{Json(JsonArray{})}),
                   Json(JsonObject{{"a", Json(JsonArray{})}})};
    JsonArray floats{Json(0.0), Json(-0.0), Json(1.0), Json(1e16), Json(1e-05), Json(0.1), Json(-299.0), Json(369.5),
                     Json(2.5e-7), Json(1e22), Json(123.25), Json(static_cast<double>(NAN)), Json(static_cast<double>(INFINITY)),
                     Json(-static_cast<double>(INFINITY))};
    JsonObject deep{{"deep", Json(nullptr)}};
    JsonObject k{{"z", Json(1)}, {"a", Json(JsonArray{Json(1.5), Json("x"), Json(deep)})}};
    JsonObject nested{{"k", Json(k)}, {"\xc3\xbc", Json("key non-ascii")}};
    return Json(JsonObject{
        {"version", Json(3)},
        {"note", Json("quote\" back\\slash /solidus")},
        {"ctrl", Json("\b\f\n\r\t\x01\x1f\x7f")},
        {"uni", Json("\xc3\xa9\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80\xe2\x80\xa8")},
        {"empty_s", Json("")},
        {"list", Json(list)},
        {"floats", Json(floats)},
        {"nested", Json(nested)},
    });
}
}  // namespace

HDF_TEST(io_json_styles_match_python) {
    const io::Json v = sample_json();
    HDF_CHECK_EQ(io::json_dumps(v, {.indent = 1, .item_sep = ","}), std::string(kJson_indent1));
    HDF_CHECK_EQ(io::json_dumps(v, {.ensure_ascii = false, .item_sep = ",", .key_sep = ":"}),
                 std::string(kJson_compact_utf8));
    HDF_CHECK_EQ(io::json_dumps(v), std::string(kJson_default));
    HDF_CHECK_EQ(io::json_dumps(v, {.indent = 2, .ensure_ascii = false, .item_sep = ","}),
                 std::string(kJson_indent2_utf8));
    using io::Json;
    const std::vector<Json> items{Json(io::JsonObject{{"id", Json(1)}, {"text", Json("caf\xc3\xa9 \xe6\x97\xa5")}}),
                                  Json(io::JsonArray{Json(1), Json(2.5), Json(nullptr)}), Json("str"),
                                  Json(io::JsonObject{})};
    HDF_CHECK_EQ(io::json_lines(items, {.ensure_ascii = false, .item_sep = ",", .key_sep = ":"}),
                 std::string(kJsonLines));
    HDF_CHECK_EQ(io::json_lines({}, {}), std::string("[\n\n]\n"));
}

// ---------------------------------------------------------------------------------------------- Yaz0
HDF_TEST(io_yaz0_python_stream) {
    const Bytes stream = unhex(kYaz0Stream);
    HDF_CHECK(io::is_yaz0(stream));
    auto d = io::yaz0_decode(stream);
    HDF_CHECK(d.has_value());
    if (d) HDF_CHECK(*d == unhex(kYaz0Plain));
    // a declared size shorter than the stream: the last copy is clamped, as yaz0.py
    Bytes shorter = stream;
    io::put32(shorter, 4, static_cast<u32>(unhex(kYaz0Plain).size() - 5));
    auto c = io::yaz0_decode(shorter);
    HDF_CHECK(c.has_value());
    if (c) HDF_CHECK(*c == unhex(kYaz0Clamped));
}

HDF_TEST(io_yaz0_hand_made) {
    // "AB" literal, then back-reference dist 2 len 6 (overlapping) -> "ABABABAB", then long form len 0x12+1
    Bytes s = bytes_of("Yaz0");
    s.resize(16, 0);
    io::put32(s, 4, 8 + 19);
    s.push_back(0xC0);  // L L B B
    s.push_back('A');
    s.push_back('B');
    s.push_back(0x40);  // n=4 -> len 6, dist = 1 + 1 = 2
    s.push_back(0x01);
    s.push_back(0x00);  // n=0 -> long; dist 1
    s.push_back(0x00);
    s.push_back(0x01);  // len 0x12 + 1 = 19
    auto d = io::yaz0_decode(s);
    HDF_CHECK(d.has_value());
    if (d) HDF_CHECK_EQ(std::string(d->begin(), d->end()), std::string("ABABABAB") + std::string(19, 'B'));
    // errors
    HDF_CHECK(!io::is_yaz0(bytes_of("Yaz0\0\0\0\1")));  // < 16 bytes
    Bytes before = s;
    before[16] = 0x00;  // first item is a back-reference: before the start
    HDF_CHECK(!io::yaz0_decode(before).has_value());
    Bytes trunc(s.begin(), s.end() - 2);
    HDF_CHECK(!io::yaz0_decode(trunc).has_value());
    Bytes huge = s;
    io::put32(huge, 4, 0xFFFFFFF0u);  // more than this input can make
    HDF_CHECK(!io::yaz0_decode(huge).has_value());
    // maybe_yaz0: copy when not Yaz0, Yay0 refused
    auto plain = io::maybe_yaz0(bytes_of("RARC plain data!!"));
    HDF_CHECK(plain.has_value() && *plain == bytes_of("RARC plain data!!"));
    Bytes yay0 = bytes_of("Yay0");
    yay0.resize(32, 0);
    HDF_CHECK(!io::maybe_yaz0(yay0).has_value());
}

HDF_TEST(io_yaz0_corrupt_never_crashes) {
    const Bytes stream = unhex(kYaz0Stream);
    int ok = 0, err = 0;
    mutate(stream, 3000, 1, [&](View b) { (io::yaz0_decode(b) ? ok : err)++; });
    HDF_CHECK_EQ(ok + err, 3000);
}

// ---------------------------------------------------------------------------------------------- zlib / gzip
HDF_TEST(io_deflate9_and_crc_match_python) {
    HDF_CHECK(io::deflate9({}) == unhex(kDeflate9Empty));
    HDF_CHECK(io::deflate9(bytes_of("hello hello hello hello")) == unhex(kDeflate9Hello));
    Bytes b;
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 256; ++i) b.push_back(static_cast<u8>(i));
    HDF_CHECK(io::deflate9(b) == unhex(kDeflate9Bytes));
    HDF_CHECK_EQ(io::crc32(bytes_of("123456789")), static_cast<u32>(kCrc123456789));
    HDF_CHECK_EQ(io::crc32(bytes_of("6789"), io::crc32(bytes_of("12345"))), static_cast<u32>(kCrc123456789));
}

HDF_TEST(io_gzip_members_padding_errors) {
    auto two = io::gzip_inflate(unhex(kGzipTwo));
    HDF_CHECK(two.has_value());
    if (two) HDF_CHECK(*two == unhex(kGzipTwoPlain));
    // > 1 MB output (several stop-check steps), single member
    Bytes big(3 * 1024 * 1024 + 123);
    std::mt19937 rng(7);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<u8>((i / 97) ^ (rng() & 3));
    const Bytes gz = gzip_of(big);
    auto d = io::gzip_inflate(gz);
    HDF_CHECK(d.has_value() && *d == big);
    auto any = io::read_any(gz);
    HDF_CHECK(any.has_value() && *any == big);
    auto same = io::read_any(bytes_of("not gzip"));
    HDF_CHECK(same.has_value() && *same == bytes_of("not gzip"));
    // stop requested: "stopped"
    std::stop_source src;
    src.request_stop();
    auto st = io::gzip_inflate(gz, src.get_token());
    HDF_CHECK(!st.has_value() && st.error().code == "stopped");
    // truncated, trailing garbage, bad CRC
    HDF_CHECK(!io::gzip_inflate(View(gz).first(gz.size() - 10)).has_value());
    Bytes garbage = gz;
    garbage.push_back(0);
    garbage.push_back(7);
    HDF_CHECK(!io::gzip_inflate(garbage).has_value());
    Bytes badcrc = gz;
    badcrc[badcrc.size() - 6] ^= 0x55;
    HDF_CHECK(!io::gzip_inflate(badcrc).has_value());
    HDF_CHECK(!io::gzip_inflate(bytes_of("plain")).has_value());
}

HDF_TEST(io_gzip_corrupt_never_crashes) {
    const Bytes gz = gzip_of(bytes_of(std::string(5000, 'x') + "tail data for the test"));
    int n = 0;
    mutate(gz, 2000, 2, [&](View b) {
        (void)io::gzip_inflate(b);
        ++n;
    });
    HDF_CHECK_EQ(n, 2000);
}

// ---------------------------------------------------------------------------------------------- PNG
HDF_TEST(io_png_bytes_match_python) {
    const Bytes rgba = unhex(kPngRgba);
    const Bytes png = io::png_encode_rgba(5, 3, rgba);
    HDF_CHECK(png == unhex(kPngBytes));
    HDF_CHECK(io::png_encode_rgba(5, 4, rgba).empty());  // wrong size
    auto dec = io::png_decode(png);
    HDF_CHECK(dec.has_value());
    if (dec) {
        HDF_CHECK_EQ(dec->first[0], 5u);
        HDF_CHECK_EQ(dec->first[1], 3u);
        HDF_CHECK(dec->second == rgba);
    }
    if (!sha256_available()) HDF_SKIP("todo: Sha256 (C2) is still a stub");
    HDF_CHECK_EQ(io::png_pixel_hash(5, 3, rgba), std::string(kPngPixelHash));
    HDF_CHECK_EQ(io::png_pixel_hash(png), std::string(kPngPixelHash));
}

HDF_TEST(io_png_decode_all_filters) {
    // 4x5 image, one row per filter type 0..4, encoded by hand
    const u32 w = 4, h = 5, stride = w * 4;
    Bytes rgba(w * h * 4);
    std::mt19937 rng(3);
    for (u8& b : rgba) b = static_cast<u8>(rng());
    Bytes raw;
    Bytes prev(stride, 0);
    for (u32 y = 0; y < h; ++y) {
        const u8 ft = static_cast<u8>(y);
        raw.push_back(ft);
        const u8* line = rgba.data() + y * stride;
        for (u32 x = 0; x < stride; ++x) {
            const int a = x >= 4 ? line[x - 4] : 0, b = prev[x], c = x >= 4 ? prev[x - 4] : 0;
            int pred = 0;
            if (ft == 1) pred = a;
            if (ft == 2) pred = b;
            if (ft == 3) pred = (a + b) >> 1;
            if (ft == 4) {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            }
            raw.push_back(static_cast<u8>(line[x] - pred));
        }
        prev.assign(line, line + stride);
    }
    // re-wrap the encoder's chunks around a hand-made IDAT
    Bytes png = io::png_encode_rgba(w, h, rgba);
    const Bytes idat = io::deflate9(raw);
    Bytes made(png.begin(), png.begin() + 33);  // signature + IHDR chunk
    auto chunk = [&](const char* t, View d) {
        Bytes c(4);
        io::put32(c, 0, static_cast<u32>(d.size()));
        c.insert(c.end(), t, t + 4);
        c.insert(c.end(), d.begin(), d.end());
        const u32 crc = io::crc32(View(c).subspan(4));
        c.resize(c.size() + 4);
        io::put32(c, c.size() - 4, crc);
        made.insert(made.end(), c.begin(), c.end());
    };
    chunk("IDAT", View(idat).first(idat.size() / 2));  // split over two IDAT chunks
    chunk("IDAT", View(idat).subspan(idat.size() / 2));
    chunk("IEND", {});
    auto dec = io::png_decode(made);
    HDF_CHECK(dec.has_value());
    if (dec) HDF_CHECK(dec->second == rgba);
    Bytes badcrc = made;
    badcrc[40] ^= 1;
    HDF_CHECK(!io::png_decode(badcrc).has_value());
    int n = 0;
    mutate(made, 2000, 4, [&](View b) {
        (void)io::png_decode(b);
        (void)io::png_pixel_hash(b);
        ++n;
    });
    HDF_CHECK_EQ(n, 2000);
}

HDF_TEST(io_png_huge_sizes_refused) {
    // fix-wave1 L3: w * h * 4 wraps to 0 for w = h = 2^31; an empty buffer must not pass the size check.
    HDF_CHECK(io::png_encode_rgba(1u << 31, 1u << 31, View{}).empty());
    HDF_CHECK(io::png_encode_rgba(0xFFFFFFFFu, 0xFFFFFFFFu, View{}).empty());
    HDF_CHECK_EQ(io::png_pixel_hash(1u << 31, 1u << 31, View{}), std::string());
    const Bytes px{1, 2, 3, 4};
    HDF_CHECK(!io::png_encode_rgba(1, 1, px).empty());
    // png_decode: a valid 1x1 PNG whose IHDR claims sizes where h * (4w + 1) wrapped below the 1 GiB limit
    // (w = 2,147,463,648, h = 2,147,503,648 gives 547,503,648 mod 2^64) or is simply too large
    const Bytes png = io::png_encode_rgba(1, 1, px);
    const u32 sizes[][2] = {{2147463648u, 2147503648u}, {1u << 31, 1u << 31}, {0xFFFFFFFFu, 1}, {1, 0xFFFFFFFFu},
                            {0x4000000u, 4}};
    for (const auto& s : sizes) {
        Bytes b = png;
        io::put32(b, 16, s[0]);  // IHDR data starts at 16 (signature 8, length 4, type 4)
        io::put32(b, 20, s[1]);
        io::put32(b, 29, io::crc32(View(b).subspan(12, 17)));
        auto d = io::png_decode(b);
        HDF_CHECK(!d.has_value());
        if (!d) HDF_CHECK_EQ(d.error().detail, std::string("PNG: image too large"));
    }
}

// ---------------------------------------------------------------------------------------------- RARC
namespace {

// A RARC made by hand from node and entry tables (no file data), for the walk limits (fix-wave1 L2).
struct HandNode {
    std::string name;
    u16 count;
    u32 first;
};
struct HandEntry {
    std::string name;
    u8 flags;
    u32 off;  // node index for a folder
};

Bytes hand_rarc(const std::vector<HandNode>& nodes, const std::vector<HandEntry>& entries) {
    Bytes strs{'.', 0, '.', '.', 0};
    auto sid = [&](const std::string& s) -> u32 {
        if (s == ".") return 0;
        if (s == "..") return 2;
        const u32 o = static_cast<u32>(strs.size());
        strs.insert(strs.end(), s.begin(), s.end());
        strs.push_back(0);
        return o;
    };
    std::vector<u32> nn, en;
    for (const HandNode& n : nodes) nn.push_back(sid(n.name));
    for (const HandEntry& e : entries) en.push_back(sid(e.name));
    const u32 ib = 0x20, node_rel = 0x20;
    const u32 ent_rel = node_rel + static_cast<u32>(nodes.size()) * 16;
    const u32 str_rel = ent_rel + static_cast<u32>(entries.size()) * 20;
    const u32 data_rel = static_cast<u32>(io::align(str_rel + strs.size(), 32));
    Bytes b(ib + data_rel, 0);
    std::memcpy(b.data(), "RARC", 4);
    io::put32(b, 4, static_cast<u32>(b.size()));
    io::put32(b, 8, ib);
    io::put32(b, 12, data_rel);
    io::put32(b, ib, static_cast<u32>(nodes.size()));
    io::put32(b, ib + 4, node_rel);
    io::put32(b, ib + 8, static_cast<u32>(entries.size()));
    io::put32(b, ib + 12, ent_rel);
    io::put32(b, ib + 16, static_cast<u32>(strs.size()));
    io::put32(b, ib + 20, str_rel);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::size_t o = ib + node_rel + i * 16;
        std::memcpy(b.data() + o, "DIR ", 4);
        io::put32(b, o + 4, nn[i]);
        io::put16(b, o + 10, nodes[i].count);
        io::put32(b, o + 12, nodes[i].first);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t o = ib + ent_rel + i * 20;
        io::put32(b, o + 4, (static_cast<u32>(entries[i].flags) << 24) | en[i]);
        io::put32(b, o + 8, entries[i].off);
    }
    std::memcpy(b.data() + ib + str_rel, strs.data(), strs.size());
    return b;
}

// nodes 0..depth: node k lists entry k; entries 0..depth-1 are folders (entry k -> node k + 1), the last a file
Bytes rarc_chain(u32 depth) {
    std::vector<HandNode> nodes;
    std::vector<HandEntry> entries;
    for (u32 k = 0; k <= depth; ++k) {
        nodes.push_back({k == 0 ? "arc" : "d" + std::to_string(k), 1, k});
        if (k < depth) {
            entries.push_back({"d" + std::to_string(k + 1), io::kRarcDir, k + 1});
        } else {
            entries.push_back({"x.bin", io::kRarcFile, 0});
        }
    }
    return hand_rarc(nodes, entries);
}

}  // namespace

HDF_TEST(io_rarc_walk_limits) {
    // fix-wave1 L2: the walk can't be made to build more members than entries, nest without limit or copy a
    // huge name once per entry. Real archives stay far below every limit (VERIFIED fix-wave1).
    // folders sharing one entry range: 2 x 2 members <= 5 entries is accepted (as Python), 3 x 2 > 5 is not
    const std::vector<HandEntry> ents{{"a", io::kRarcDir, 1}, {"b", io::kRarcDir, 2}, {"c", io::kRarcDir, 3},
                                      {"x.bin", io::kRarcFile, 0}, {"y.bin", io::kRarcFile, 0}};
    auto two = io::rarc_read(hand_rarc({{"arc", 2, 0}, {"a", 2, 3}, {"b", 2, 3}, {"c", 2, 3}}, ents));
    HDF_CHECK(two.has_value());
    if (two) HDF_CHECK_EQ(two->members.size(), 4u);
    auto three = io::rarc_read(hand_rarc({{"arc", 3, 0}, {"a", 2, 3}, {"b", 2, 3}, {"c", 2, 3}}, ents));
    HDF_CHECK(!three.has_value());
    // names: 255 bytes accepted, 256 refused
    auto n255 = io::rarc_read(hand_rarc({{"arc", 1, 0}}, {{std::string(255, 'n'), io::kRarcFile, 0}}));
    HDF_CHECK(n255.has_value());
    if (n255) HDF_CHECK_EQ(n255->members.size(), 1u);
    HDF_CHECK(!io::rarc_read(hand_rarc({{"arc", 1, 0}}, {{std::string(256, 'n'), io::kRarcFile, 0}})).has_value());
    // depth: 64 folders below the root accepted, 65 refused
    auto deep = io::rarc_read(rarc_chain(64));
    HDF_CHECK(deep.has_value());
    if (deep) {
        HDF_CHECK_EQ(deep->members.size(), 1u);
        HDF_CHECK(io::rarc_entry_index(*deep, "x.bin").has_value());
    }
    HDF_CHECK(!io::rarc_read(rarc_chain(65)).has_value());
}

HDF_TEST(io_rarc_build_matches_python) {
    std::vector<std::pair<std::string, Bytes>> entries;
    Bytes room(40);
    for (int i = 0; i < 40; ++i) room[i] = static_cast<u8>(i);
    const hdf::Result<hdf::io::Rarc> ref = io::rarc_read(unhex(kRarcBuilt));
    HDF_CHECK(ref.has_value());
    if (!ref) return;
    // the same entries as gen_io_vectors.py, taken from the Python-built archive
    const char* order[] = {"dzb/room.dzb", "bmdr/model.bmd", "bmdr/sub/x.bti", "readme.txt", "dzb/a.bin", "Kcl/Kcl.kcl"};
    for (const char* p : order) {
        const io::RarcMember* m = ref->get(std::string("arc/") + p);
        HDF_CHECK(m != nullptr);
        if (!m) return;
        const View r = ref->raw(*m);
        entries.emplace_back(p, Bytes(r.begin(), r.end()));
    }
    HDF_CHECK(entries[0].second == room);
    HDF_CHECK(io::rarc_build("arc", entries) == unhex(kRarcBuilt));
    HDF_CHECK_EQ(io::rarc_name_hash("."), static_cast<u16>(kNameHash[0]));
    HDF_CHECK_EQ(io::rarc_name_hash(".."), static_cast<u16>(kNameHash[1]));
    HDF_CHECK_EQ(io::rarc_name_hash("room.dzb"), static_cast<u16>(kNameHash[2]));
    HDF_CHECK_EQ(io::rarc_name_hash("a_much_longer_file_name_for_the_hash.bmd"), static_cast<u16>(kNameHash[3]));
}

HDF_TEST(io_rarc_read_members_and_lookup) {
    auto a = io::rarc_read(unhex(kRarcBuilt));
    HDF_CHECK(a.has_value());
    if (!a) return;
    HDF_CHECK(!a->was_compressed);
    HDF_CHECK_EQ(a->root_name(), std::string_view("arc"));
    HDF_CHECK_EQ(a->members.size(), std::size(kRarcMembers));
    for (std::size_t i = 0; i < a->members.size() && i < std::size(kRarcMembers); ++i) {
        const io::RarcMember& m = a->members[i];
        HDF_CHECK_EQ(m.path, std::string(kRarcMembers[i].path));
        HDF_CHECK_EQ(unsigned(m.flags), kRarcMembers[i].flags);
        HDF_CHECK_EQ(m.offset, kRarcMembers[i].offset);
        HDF_CHECK_EQ(m.size, kRarcMembers[i].size);
        HDF_CHECK_EQ(unsigned(m.id), kRarcMembers[i].id);
    }
    for (const EntryIndexCase& c : kEntryIndex) {
        const auto ei = io::rarc_entry_index(*a, c.key);
        HDF_CHECK_EQ(ei ? int(*ei) : -1, c.ei);
    }
    HDF_CHECK(a->get("ROOM.DZB") != nullptr);
    HDF_CHECK(a->get("nothing.bin") == nullptr);
    // GC layout round trip and same-layout rebuild of a GC-layout archive
    auto w = io::rarc_write(*a);
    HDF_CHECK(w.has_value() && *w == unhex(kRarcBuilt));
    auto s = io::rarc_rebuild_same_layout(*a);
    HDF_CHECK(s.has_value() && *s == unhex(kRarcBuilt));
    // a Yaz0-compressed archive reads the same
    auto y = io::rarc_read(unhex(kRarcYaz0));
    HDF_CHECK(y.has_value() && y->was_compressed && y->buf == a->buf);
}

HDF_TEST(io_rarc_same_layout_and_replace_match_python) {
    auto a = io::rarc_read(unhex(kRarcHd));
    HDF_CHECK(a.has_value());
    if (!a) return;
    const io::RarcMember* m = a->get("model.bmd");
    HDF_CHECK(m != nullptr && (m->flags & io::kRarcCompressed));
    io::Replace rep;
    rep.emplace("arc/readme.txt", bytes_of("NEW-README"));
    rep.emplace("dzb/a.bin", Bytes(7, 'Z'));
    auto same = io::rarc_rebuild_same_layout(*a, rep);
    HDF_CHECK(same.has_value() && *same == unhex(kRarcHdSameLayout));
    auto gc = io::rarc_write(*a, rep);
    HDF_CHECK(gc.has_value() && *gc == unhex(kRarcHdGcLayout));
    auto plain = io::rarc_rebuild_same_layout(*a);
    HDF_CHECK(plain.has_value() && *plain == unhex(kRarcHdPlain));
}

HDF_TEST(io_rarc_corrupt_never_crashes) {
    const Bytes good = unhex(kRarcHd);
    int n = 0;
    mutate(good, 3000, 5, [&](View b) {
        auto a = io::rarc_read(b);
        if (a) {
            (void)io::rarc_write(*a);
            (void)io::rarc_rebuild_same_layout(*a);
            for (const io::RarcMember& m : a->members) (void)a->data(m);
            (void)io::rarc_entry_index(*a, "x.bti");
        }
        ++n;
    });
    HDF_CHECK_EQ(n, 3000);
    HDF_CHECK(!io::rarc_read(bytes_of("RARC")).has_value());
    HDF_CHECK(!io::rarc_read(bytes_of("not an archive at all, really")).has_value());
}

// ---------------------------------------------------------------------------------------------- TMPK
HDF_TEST(io_tmpk_read_and_get) {
    const Bytes pack = make_tmpk({{"tex/res/Stage/X/R00_00.arc/bmdr/model.bmd.gtx", bytes_of("GTX-A")},
                                  {"geo/res/Stage/X/R00_00.arc/bmdr/model.bmd.gmx", bytes_of("GMX-B")},
                                  {"tex/other.bti.gtx", bytes_of("BTI")}});
    auto t = io::tmpk_read(pack);
    HDF_CHECK(t.has_value());
    if (!t) return;
    HDF_CHECK_EQ(t->count, 3u);
    HDF_CHECK_EQ(t->alignment, 0x40u);
    HDF_CHECK_EQ(t->entries[1].name, std::string("geo/res/Stage/X/R00_00.arc/bmdr/model.bmd.gmx"));
    auto g = t->get("tex/res/Stage/X/R00_00.arc/bmdr/model.bmd.gtx");  // exact
    HDF_CHECK(g && std::string(g->begin(), g->end()) == "GTX-A");
    g = t->get("bmdr/model.bmd.gmx");  // "/"-suffix
    HDF_CHECK(g && std::string(g->begin(), g->end()) == "GMX-B");
    g = t->get("other.bti.gtx");  // basename
    HDF_CHECK(g && std::string(g->begin(), g->end()) == "BTI");
    HDF_CHECK(!t->get("model.bmd").has_value());
    // .pack.gz
    auto z = io::tmpk_from_gz(gzip_of(pack));
    HDF_CHECK(z.has_value() && z->buf == pack && z->entries.size() == 3);
    // a size past the end is clamped (Python slicing), never read out of bounds
    Bytes clipped = pack;
    io::put32(clipped, 0x10 + 2 * 16 + 8, 0x7FFFFFFF);
    auto c = io::tmpk_read(clipped);
    HDF_CHECK(c.has_value() && c->data(c->entries[2]).size() == 3);
    HDF_CHECK(!io::tmpk_read(bytes_of("TMPK")).has_value());
    int n = 0;
    mutate(pack, 2000, 6, [&](View b) {
        auto r = io::tmpk_read(Bytes(b.begin(), b.end()));
        if (r)
            for (const io::TmpkEntry& e : r->entries) (void)r->data(e);
        ++n;
    });
    HDF_CHECK_EQ(n, 2000);
}
