// Unit tests of module C2 (src/base): SHA-256, paths, sources, sinks, .wua and .zip readers, input and output
// checks. Synthetic data only (no game data); temporary files go to a fresh folder under %TEMP% and are removed.
#include "test.hpp"

#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include "../src/base/base_util.hpp"
#include "../src/base/sha256_portable.hpp"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <string>

namespace {

using namespace hdf;
namespace fs = std::filesystem;

Bytes bytes_of(std::string_view s) { return Bytes(s.begin(), s.end()); }

// The error code by value ("ok" when there is none): HDF_CHECK_EQ binds references, so never pass it a member
// of a temporary Result.
template <class T>
std::string code_of(const Result<T>& r) {
    return r ? std::string("ok") : r.error().code;
}

std::string portable_hex(View d) {
    base::Sha256Portable h;
    h.update(d.data(), d.size());
    const auto out = h.finish();
    return to_hex(out);
}

// A fresh folder under %TEMP%, removed when the test ends.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("hdf-selftest-" + std::to_string(rd()) + "-" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& p, View data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void put_be(Bytes& b, u64 v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void put_le(Bytes& b, u64 v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}

// ---------------------------------------------------------------------------------------------- synthetic .wua
// Block 0 is stored raw; block 1 is a hand-made zstd frame (a raw block of 1000 bytes, then an RLE block).
constexpr u64 kBlock = 65536;

Bytes block0() {
    Bytes b(kBlock);
    for (u64 i = 0; i < kBlock; ++i) b[i] = static_cast<u8>((i * 31 + 7) & 0xFF);
    return b;
}
Bytes block1() {
    Bytes b(kBlock, 0xAB);
    for (u64 i = 0; i < 1000; ++i) b[i] = static_cast<u8>((i * 7) & 0xFF);
    return b;
}
Bytes block1_zstd() {
    const Bytes plain = block1();
    Bytes f;
    put_le(f, 0xFD2FB528u, 4);  // magic
    f.push_back(0x60);          // single segment, 2-byte frame content size, no checksum, no dictionary
    put_le(f, kBlock - 256, 2);
    put_le(f, 0u | (0u << 1) | (1000u << 3), 3);  // raw block, 1000 bytes
    f.insert(f.end(), plain.begin(), plain.begin() + 1000);
    put_le(f, 1u | (1u << 1) | (u32(kBlock - 1000) << 3), 3);  // last, RLE block
    f.push_back(0xAB);
    return f;
}

struct WuaSpec {
    std::vector<std::pair<std::string, std::pair<u64, u64>>> files;  // full path -> (offset, size)
};

// The standard fixture: base v0 and update v81 of the USA title, plus one non-TP HD title.
WuaSpec standard_spec() {
    WuaSpec s;
    s.files = {
        {"000500001019e500_v0/content/los.bin", {0, 70000}},             // spans block 0 and block 1
        {"000500001019e500_v0/content/res/Stage/A.arc", {70010, 100}},
        {"0005000e1019e500_v81/content/los.bin", {70000, 10}},
        {"0005000010101a00_v0/content/other.bin", {70200, 20}},
    };
    return s;
}

Bytes wua_plain() {
    Bytes d = block0();
    const Bytes b1 = block1();
    d.insert(d.end(), b1.begin(), b1.end());
    return d;
}

Bytes build_wua(const WuaSpec& spec) {
    // folder tree: path -> children (std::map: sorted)
    struct Node {
        std::map<std::string, Node> dirs;
        std::map<std::string, std::pair<u64, u64>> files;
    };
    Node root;
    for (const auto& [path, rec] : spec.files) {
        Node* n = &root;
        std::size_t i = 0;
        while (true) {
            const std::size_t j = path.find('/', i);
            if (j == std::string::npos) {
                n->files[path.substr(i)] = rec;
                break;
            }
            n = &n->dirs[path.substr(i, j - i)];
            i = j + 1;
        }
    }
    Bytes names, tree;
    auto add_name = [&](const std::string& s) {
        const u32 off = static_cast<u32>(names.size());
        names.push_back(static_cast<u8>(s.size()));
        names.insert(names.end(), s.begin(), s.end());
        return off;
    };
    // breadth-first: each folder's children are contiguous
    struct Pending {
        const Node* node;
        std::size_t entry;
    };
    tree.resize(16);  // root
    std::vector<Pending> queue{{&root, 0}};
    for (std::size_t q = 0; q < queue.size(); ++q) {
        const Node* n = queue[q].node;
        const std::size_t first = tree.size() / 16;
        const std::size_t count = n->dirs.size() + n->files.size();
        // patch the folder entry (its type|name word stays)
        const std::size_t at = queue[q].entry * 16;
        for (int i = 0; i < 4; ++i) {
            tree[at + 4 + i] = static_cast<u8>(first >> (24 - 8 * i));
            tree[at + 8 + i] = static_cast<u8>(count >> (24 - 8 * i));
            tree[at + 12 + i] = 0;
        }
        for (const auto& [name, child] : n->dirs) {
            const std::size_t idx = tree.size() / 16;
            put_be(tree, add_name(name), 4);
            put_be(tree, 0, 12);
            queue.push_back({&child, idx});
        }
        for (const auto& [name, rec] : n->files) {
            put_be(tree, 0x80000000u | add_name(name), 4);
            put_be(tree, rec.first & 0xFFFFFFFFu, 4);
            put_be(tree, rec.second & 0xFFFFFFFFu, 4);
            put_be(tree, ((rec.second >> 32) << 16) | (rec.first >> 32), 4);
        }
    }
    const Bytes b0 = block0(), b1 = block1_zstd();
    Bytes out;
    out.insert(out.end(), b0.begin(), b0.end());
    out.insert(out.end(), b1.begin(), b1.end());
    const u64 data_size = out.size();
    const u64 off_rec = out.size();
    put_be(out, 0, 8);
    put_be(out, kBlock - 1, 2);
    put_be(out, b1.size() - 1, 2);
    for (int i = 2; i < 16; ++i) put_be(out, 0, 2);
    const u64 off_names = out.size();
    out.insert(out.end(), names.begin(), names.end());
    const u64 off_tree = out.size();
    out.insert(out.end(), tree.begin(), tree.end());
    const u64 off_end = out.size();
    const u64 secs[6][2] = {{0, data_size}, {off_rec, 40}, {off_names, names.size()}, {off_tree, tree.size()},
                            {off_end, 0}, {off_end, 0}};
    for (const auto& s : secs) {
        put_be(out, s[0], 8);
        put_be(out, s[1], 8);
    }
    for (int i = 0; i < 32; ++i) out.push_back(0);
    put_be(out, out.size() + 16, 8);
    put_be(out, 0x61BF3A01u, 4);
    put_be(out, 0x169F52D6u, 4);
    return out;
}

// A .wua with an empty data section and the given names and tree sections (fix-wave1 M1 probes).
Bytes wua_from_sections(const Bytes& names, const Bytes& tree) {
    Bytes out = names;
    out.insert(out.end(), tree.begin(), tree.end());
    const u64 end = out.size();
    const u64 secs[6][2] = {{0, 0}, {0, 0}, {0, names.size()}, {names.size(), tree.size()}, {end, 0}, {end, 0}};
    for (const auto& s : secs) {
        put_be(out, s[0], 8);
        put_be(out, s[1], 8);
    }
    for (int i = 0; i < 32; ++i) out.push_back(0);
    put_be(out, out.size() + 16, 8);
    put_be(out, 0x61BF3A01u, 4);
    put_be(out, 0x169F52D6u, 4);
    return out;
}

// One name in ZArchive form: 1-byte length, or 2 bytes ((b0 & 0x7F) | b1 << 7) from 128 bytes on.
u32 add_wua_name(Bytes& names, std::string_view s) {
    const u32 off = static_cast<u32>(names.size());
    if (s.size() < 128) {
        names.push_back(static_cast<u8>(s.size()));
    } else {
        names.push_back(static_cast<u8>(0x80 | (s.size() & 0x7F)));
        names.push_back(static_cast<u8>(s.size() >> 7));
    }
    names.insert(names.end(), s.begin(), s.end());
    return off;
}

// depth (>= 1) nested folders that all use ONE stored name of folder_len bytes, then `files` empty files in the
// last folder: the shape that made path keys grow far beyond the file (review finding M1).
Bytes wua_chain(std::size_t folder_len, u32 depth, u32 files) {
    Bytes names, tree;
    const u32 folder_name = add_wua_name(names, std::string(folder_len, 'a'));
    std::vector<u32> file_names;
    for (u32 i = 0; i < files; ++i) file_names.push_back(add_wua_name(names, "f" + std::to_string(i)));
    put_be(tree, 0, 4);  // root folder -> entry 1
    put_be(tree, 1, 4);
    put_be(tree, 1, 4);
    put_be(tree, 0, 4);
    for (u32 d = 0; d < depth; ++d) {
        put_be(tree, folder_name, 4);
        put_be(tree, 2 + d, 4);  // first child: the next entry
        put_be(tree, d + 1 < depth ? 1 : files, 4);
        put_be(tree, 0, 4);
    }
    for (u32 i = 0; i < files; ++i) {
        put_be(tree, 0x80000000u | file_names[i], 4);
        put_be(tree, 0, 12);  // offset 0, size 0
    }
    return wua_from_sections(names, tree);
}

Bytes slice(const Bytes& d, u64 off, u64 n) {
    return Bytes(d.begin() + static_cast<std::ptrdiff_t>(off), d.begin() + static_cast<std::ptrdiff_t>(off + n));
}

// ---------------------------------------------------------------------------------------------- synthetic .zip
Bytes raw_deflate(const Bytes& in) {
    z_stream zs{};
    deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&zs, static_cast<uLong>(in.size())));
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

Bytes build_zip(const std::vector<std::pair<std::string, Bytes>>& entries) {
    Bytes out, cd;
    u16 count = 0;
    for (const auto& [name, data] : entries) {
        const bool folder = !name.empty() && name.back() == '/';
        const bool deflated = !folder && data.size() > 16;
        const Bytes stored = deflated ? raw_deflate(data) : data;
        const u32 crc = static_cast<u32>(::crc32(::crc32(0L, Z_NULL, 0), data.data(), static_cast<uInt>(data.size())));
        const u32 local = static_cast<u32>(out.size());
        put_le(out, 0x04034b50u, 4);
        put_le(out, 20, 2);
        put_le(out, 0, 2);
        put_le(out, deflated ? 8 : 0, 2);
        put_le(out, 0, 4);
        put_le(out, crc, 4);
        put_le(out, stored.size(), 4);
        put_le(out, data.size(), 4);
        put_le(out, name.size(), 2);
        put_le(out, 0, 2);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), stored.begin(), stored.end());
        put_le(cd, 0x02014b50u, 4);
        put_le(cd, 20, 2);
        put_le(cd, 20, 2);
        put_le(cd, 0, 2);
        put_le(cd, deflated ? 8 : 0, 2);
        put_le(cd, 0, 4);
        put_le(cd, crc, 4);
        put_le(cd, stored.size(), 4);
        put_le(cd, data.size(), 4);
        put_le(cd, name.size(), 2);
        put_le(cd, 0, 2);
        put_le(cd, 0, 2);
        put_le(cd, 0, 2);
        put_le(cd, 0, 2);
        put_le(cd, 0, 4);
        put_le(cd, local, 4);
        cd.insert(cd.end(), name.begin(), name.end());
        ++count;
    }
    const u32 cd_off = static_cast<u32>(out.size());
    out.insert(out.end(), cd.begin(), cd.end());
    put_le(out, 0x06054b50u, 4);
    put_le(out, 0, 4);
    put_le(out, count, 2);
    put_le(out, count, 2);
    put_le(out, cd.size(), 4);
    put_le(out, cd_off, 4);
    put_le(out, 0, 2);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- SHA-256
HDF_TEST(base_sha256_known_vectors) {
    HDF_CHECK_EQ(sha256_hex({}), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    HDF_CHECK_EQ(sha256_hex(bytes_of("abc")),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    HDF_CHECK_EQ(sha256_hex(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
                 std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const Bytes million(1000000, 'a');
    HDF_CHECK_EQ(sha256_hex(million), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    HDF_CHECK_EQ(portable_hex(million), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

HDF_TEST(base_sha256_streaming_and_portable_agree) {
    std::mt19937 rng(1234);
    Bytes data(300000);
    for (u8& b : data) b = static_cast<u8>(rng());
    for (std::size_t n : {0u, 1u, 55u, 56u, 63u, 64u, 65u, 119u, 120u, 1000u, 65536u, 300000u}) {
        const View v(data.data(), n);
        const std::string one = sha256_hex(v);
        HDF_CHECK_EQ(one, portable_hex(v));
        Sha256 h;
        std::size_t i = 0, step = 1;
        while (i < n) {
            const std::size_t take = std::min(step, n - i);
            h.update(View(data.data() + i, take));
            i += take;
            step = step * 3 + 1;
        }
        HDF_CHECK_EQ(h.finish_hex(), one);
    }
}

HDF_TEST(base_hex_keys_and_rel_paths) {
    const u8 b[] = {0x00, 0x0f, 0xa5, 0xff};
    HDF_CHECK_EQ(to_hex(b), std::string("000fa5ff"));
    HDF_CHECK_EQ(lower_key("/Res\\Stage/D_SB11/R00_00.ARC"), std::string("res/stage/d_sb11/r00_00.arc"));
    HDF_CHECK(base::safe_rel("overlay/res/Stage/D_SB11/R00_00.arc"));
    HDF_CHECK(base::safe_rel("res/cave/amiibo/@x.png"));
    for (const char* bad : {"", "/abs", "a//b", "a/./b", "../x", "a/../b", "C:/x", "a\\b", "a/b/", "nul\x01"})
        HDF_CHECK(!base::safe_rel(bad));
}

// ---------------------------------------------------------------------------------------------- memory, layers
HDF_TEST(base_memory_source_and_layers) {
    MemorySource m("test");
    m.add("Res/A.bin", bytes_of("one"));
    m.add("res/b.bin", bytes_of("bee"));
    m.add("RES/a.BIN", bytes_of("uno"));  // replaces
    const std::vector<std::string> keys = m.list();
    HDF_CHECK_EQ(keys.size(), 2u);
    HDF_CHECK_EQ(keys[0], std::string("res/a.bin"));
    HDF_CHECK_EQ(m.total_bytes(), 6u);
    HDF_CHECK_EQ(m.size("res/A.bin").value_or(0), 3u);
    HDF_CHECK(m.read("RES/A.BIN").value() == bytes_of("uno"));
    HDF_CHECK_EQ(code_of(m.read("res/c.bin")), std::string("io.missing"));
    std::stop_source stopped;
    stopped.request_stop();
    HDF_CHECK_EQ(code_of(m.read("res/a.bin", stopped.get_token())), std::string("stopped"));

    auto update = std::make_unique<MemorySource>("update");
    update->add("res/a.bin", bytes_of("new"));
    auto basel = std::make_unique<MemorySource>("base");
    basel->add("res/a.bin", bytes_of("old"));
    basel->add("res/only_base.bin", bytes_of("b"));
    LayeredSource l;
    l.add(std::move(update));
    l.add(std::move(basel));
    HDF_CHECK_EQ(l.layers(), 2u);
    HDF_CHECK(l.read("res/a.bin").value() == bytes_of("new"));
    HDF_CHECK(l.read("res/only_base.bin").value() == bytes_of("b"));
    HDF_CHECK(!l.size("res/none"));
    HDF_CHECK_EQ(code_of(l.read("res/none")), std::string("io.missing"));
    HDF_CHECK_EQ(l.describe(), std::string("update over base"));

    MemorySink sink;
    HDF_CHECK(sink.write("res/x.bin", bytes_of("1")).has_value());
    HDF_CHECK(sink.write("res/x.bin", bytes_of("22")).has_value());
    HDF_CHECK(!sink.write("../x", bytes_of("1")).has_value());
    HDF_CHECK_EQ(sink.files.size(), 1u);
    HDF_CHECK(sink.files[0].second == bytes_of("22"));
}

// ---------------------------------------------------------------------------------------------- folders
HDF_TEST(base_folder_source_and_sink) {
    TempDir t;
    FolderSink sink(t.path / "out");
    HDF_CHECK(sink.write("res/Stage/D_SB11/R00_00.arc", bytes_of("room")).has_value());
    HDF_CHECK(sink.write("res/Stage/D_SB11/R00_00.arc", bytes_of("room2")).has_value());  // overwrite
    HDF_CHECK(sink.write("los.bin", bytes_of("L")).has_value());
    HDF_CHECK(!sink.write("../escape.bin", bytes_of("x")).has_value());
    HDF_CHECK(!sink.write("C:/x.bin", bytes_of("x")).has_value());
    HDF_CHECK(!fs::exists(t.path / "escape.bin"));

    FolderSource src(t.path / "out");
    HDF_CHECK_EQ(src.size("RES/stage/d_sb11/r00_00.ARC").value_or(0), 5u);
    HDF_CHECK(src.read("res/stage/D_SB11/R00_00.arc").value() == bytes_of("room2"));
    HDF_CHECK(!src.size("res/stage"));       // a folder is not a file
    HDF_CHECK(!src.size("los.bin/x"));       // a file is not a folder
    HDF_CHECK(!src.size("../out/los.bin"));  // no climbing out
    HDF_CHECK_EQ(code_of(src.read("res/none.bin")), std::string("io.missing"));
    auto names = src.list();
    HDF_CHECK(names.has_value());
    HDF_CHECK_EQ(names->size(), 2u);
    HDF_CHECK_EQ((*names)[0], std::string("los.bin"));
    HDF_CHECK_EQ((*names)[1], std::string("res/Stage/D_SB11/R00_00.arc"));
    // a file read in several 4 MiB pieces
    Bytes big(9u << 20);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 13);
    HDF_CHECK(sink.write("big.bin", big).has_value());
    HDF_CHECK(src.read("BIG.BIN").value() == big);
}

HDF_TEST(base_names_not_utf16_are_skipped) {
    // fix-wave1 L1: NTFS allows a name with an unpaired UTF-16 surrogate; path::u8string() throws on it. One
    // such name in a listed folder must not make size/read/list/open_hd_location throw.
#ifdef _WIN32
    HDF_CHECK_EQ(base::utf8_exact(fs::path(L"é\U0001F600\\x"), true).value_or("?"),
                 std::string("\xC3\xA9\xF0\x9F\x98\x80/x"));
    std::wstring bad = L"x";
    bad.push_back(wchar_t(0xD800));
    HDF_CHECK(!base::utf8_exact(fs::path(bad + L".bin")).has_value());
    HDF_CHECK_EQ(base::utf8_lossy(fs::path(bad + L".bin")), std::string("x\xEF\xBF\xBD.bin"));
    TempDir t;
    const fs::path content = t.path / "Title" / "content";
    write_file(content / "los.bin", bytes_of("L"));
    write_file(content / fs::path(bad + L".bin"), bytes_of("B"));
    write_file(content / fs::path(bad + L"dir") / "y.bin", bytes_of("Y"));
    std::error_code ec;
    if (!fs::exists(content / fs::path(bad + L".bin"), ec)) HDF_SKIP("this file system refuses such names");
    FolderSource src(content);
    HDF_CHECK_EQ(src.size("LOS.bin").value_or(0), 1u);
    HDF_CHECK(src.read("los.bin").has_value());
    HDF_CHECK(!src.size("y.bin"));
    auto names = src.list();
    HDF_CHECK(names.has_value());
    if (names) HDF_CHECK_EQ(names->size(), 1u);  // only los.bin
    // a title folder whose own name is not valid UTF-16, found through its parent
    const fs::path odd = t.path / "parent" / fs::path(bad + L" title");
    write_file(odd / "content" / "los.bin", bytes_of("O"));
    auto hd = inputs::open_hd_location(t.path / "parent");
    HDF_CHECK(hd.has_value());
    if (hd) {
        HDF_CHECK((*hd)->read("los.bin").value() == bytes_of("O"));
        HDF_CHECK(!(*hd)->describe().empty());
    }
    HDF_CHECK(inputs::open_hd_location(t.path / "Title").has_value());
#endif
}

HDF_TEST(base_open_hd_location_folders) {
    TempDir t;
    // a title folder with content/, as Cemu keeps it
    write_file(t.path / "Title" / "content" / "los.bin", bytes_of("base"));
    write_file(t.path / "Title" / "meta" / "meta.xml", bytes_of("<x/>"));
    for (const fs::path& p : {t.path / "Title", t.path / "Title" / "content", t.path}) {
        auto s = inputs::open_hd_location(p);
        HDF_CHECK(s.has_value());
        if (s) HDF_CHECK((*s)->read("LOS.BIN").value() == bytes_of("base"));
    }
    // mlc: base + update next to it, update first
    const fs::path mlc = t.path / "mlc01" / "usr" / "title";
    write_file(mlc / "00050000" / "1019e500" / "content" / "los.bin", bytes_of("base"));
    write_file(mlc / "00050000" / "1019e500" / "content" / "res" / "x.bin", bytes_of("x"));
    write_file(mlc / "0005000e" / "1019e500" / "content" / "los.bin", bytes_of("v81"));
    auto m = inputs::open_hd_location(mlc / "00050000" / "1019e500");
    HDF_CHECK(m.has_value());
    if (m) {
        HDF_CHECK_EQ((*m)->layers(), 2u);
        HDF_CHECK((*m)->read("los.bin").value() == bytes_of("v81"));
        HDF_CHECK((*m)->read("res/x.bin").value() == bytes_of("x"));
    }
    // refusals
    write_file(t.path / "dump.wux", bytes_of("encrypted"));
    HDF_CHECK_EQ(code_of(inputs::open_hd_location(t.path / "dump.wux")), std::string("hd.encrypted"));
    fs::create_directories(t.path / "empty");
    HDF_CHECK_EQ(code_of(inputs::open_hd_location(t.path / "empty")), std::string("hd.not_found"));
    write_file(t.path / "nus" / "00000000.app", bytes_of("x"));
    HDF_CHECK_EQ(code_of(inputs::open_hd_location(t.path / "nus")), std::string("hd.encrypted"));
    HDF_CHECK_EQ(code_of(inputs::open_hd_location(t.path / "missing")), std::string("hd.not_found"));
    write_file(t.path / "junk.wua", bytes_of("not an archive"));
    HDF_CHECK_EQ(code_of(inputs::open_hd_location(t.path / "junk.wua")), std::string("wua.bad"));
}

// ---------------------------------------------------------------------------------------------- .wua
HDF_TEST(base_wua_read_and_layers) {
    TempDir t;
    const fs::path file = t.path / "game.wua";
    write_file(file, build_wua(standard_spec()));
    auto a = WuaArchive::open(file);
    HDF_CHECK(a.has_value());
    if (!a) return;
    const std::vector<std::string> tops = (*a)->tops();
    HDF_CHECK_EQ(tops.size(), 3u);
    HDF_CHECK_EQ(tops[0], std::string("0005000010101a00_v0"));
    HDF_CHECK_EQ((*a)->file_count(), 4u);
    HDF_CHECK((*a)->has_folder("000500001019E500_v0/content"));
    HDF_CHECK(!(*a)->has_folder("000500001019e500_v0/cont"));
    const Bytes plain = wua_plain();
    HDF_CHECK((*a)->read("000500001019e500_v0/content/los.bin").value() == slice(plain, 0, 70000));
    HDF_CHECK_EQ((*a)->size("000500001019e500_v0/content/res/stage/a.arc").value_or(0), 100u);
    HDF_CHECK((*a)->read("000500001019e500_v0/content/res/stage/a.arc").value() == slice(plain, 70010, 100));
    HDF_CHECK_EQ(code_of((*a)->read("nothing")), std::string("io.missing"));

    auto hd = inputs::open_hd_location(file);
    HDF_CHECK(hd.has_value());
    if (!hd) return;
    HDF_CHECK_EQ((*hd)->layers(), 2u);  // the non-TP HD title is left out
    HDF_CHECK((*hd)->read("LOS.BIN").value() == slice(plain, 70000, 10));  // the update wins
    HDF_CHECK((*hd)->read("res/Stage/A.arc").value() == slice(plain, 70010, 100));
    HDF_CHECK(!(*hd)->size("other.bin"));

    // only a non-TP HD title
    WuaSpec other;
    other.files = {{"0005000010101a00_v0/content/other.bin", {0, 20}}};
    write_file(t.path / "other.wua", build_wua(other));
    HDF_CHECK_EQ(code_of(inputs::open_hd_location(t.path / "other.wua")), std::string("hd.not_tphd"));
}

HDF_TEST(base_wua_damaged_archives) {
    TempDir t;
    const Bytes good = build_wua(standard_spec());
    const u64 n = good.size();
    auto open_bytes = [&](const Bytes& b) {
        write_file(t.path / "x.wua", b);
        return WuaArchive::open(t.path / "x.wua");
    };
    HDF_CHECK(open_bytes(good).has_value());
    HDF_CHECK_EQ(code_of(open_bytes(slice(good, 0, 100))), std::string("wua.bad"));      // too short
    Bytes b = good;
    b[n - 1] ^= 1;  // magic
    HDF_CHECK_EQ(code_of(open_bytes(b)), std::string("wua.bad"));
    b = good;
    b[n - 9] ^= 1;  // recorded total size
    HDF_CHECK_EQ(code_of(open_bytes(b)), std::string("wua.damaged"));
    b = good;
    b.insert(b.begin(), 0);  // shifted: sizes and offsets no longer agree
    HDF_CHECK(!open_bytes(b).has_value());
    // the tree: entry 0 points at itself
    const u64 tree_off = (u64(good[n - 144 + 48]) << 56) | (u64(good[n - 144 + 49]) << 48) | (u64(good[n - 144 + 50]) << 40) |
                         (u64(good[n - 144 + 51]) << 32) | (u64(good[n - 144 + 52]) << 24) | (u64(good[n - 144 + 53]) << 16) |
                         (u64(good[n - 144 + 54]) << 8) | u64(good[n - 144 + 55]);
    b = good;
    for (int i = 0; i < 4; ++i) b[tree_off + 4 + i] = 0;  // root's first child = 0 (itself)
    HDF_CHECK_EQ(code_of(open_bytes(b)), std::string("wua.damaged"));
    b = good;
    b[tree_off + 8] = 0x7F;  // root child count far beyond the tree
    HDF_CHECK_EQ(code_of(open_bytes(b)), std::string("wua.damaged"));
    // a broken zstd frame in block 1: open works, reading that block fails cleanly
    b = good;
    b[kBlock + 4] = 0xFF;  // frame header descriptor: reserved bit set
    auto a = open_bytes(b);
    HDF_CHECK(a.has_value());
    if (a) {
        HDF_CHECK_EQ(code_of((*a)->read("000500001019e500_v0/content/res/stage/a.arc")), std::string("wua.damaged"));
        HDF_CHECK(!(*a)->read("000500001019e500_v0/content/los.bin").has_value());
    }
}

HDF_TEST(base_wua_garbage_never_crashes) {
    // Correction #1(b): random damage anywhere must end in an error or in bytes, never in a crash or a hang.
    TempDir t;
    const Bytes good = build_wua(standard_spec());
    std::mt19937 rng(77);
    int opened = 0, failed = 0;
    for (int round = 0; round < 300; ++round) {
        Bytes b = good;
        const int flips = 1 + int(rng() % 8);
        for (int k = 0; k < flips; ++k) {
            // mostly the index area (offset records, names, tree, footer), sometimes the data
            const u64 lo = (rng() % 4 == 0) ? 0 : kBlock;
            const u64 pos = lo + rng() % (b.size() - lo);
            b[pos] = static_cast<u8>(rng());
        }
        if (rng() % 10 == 0) b.resize(b.size() - 1 - rng() % 300);
        write_file(t.path / "g.wua", b);
        auto a = WuaArchive::open(t.path / "g.wua");
        if (!a) {
            ++failed;
            continue;
        }
        ++opened;
        for (const auto& [path, rec] : standard_spec().files) (void)(*a)->read(path);
        for (const std::string& top : (*a)->tops()) (void)(*a)->has_folder(top + "/content");
    }
    HDF_CHECK_EQ(opened + failed, 300);
}

HDF_TEST(base_wua_tree_amplification_refused) {
    // fix-wave1 M1: a small file must not make WuaArchive::open build path keys far larger than the file.
    TempDir t;
    int n = 0;
    auto open_bytes = [&](const Bytes& b) {  // a new file each time: an open archive keeps its file locked
        const fs::path p = t.path / ("m" + std::to_string(n++) + ".wua");
        write_file(p, b);
        return WuaArchive::open(p);
    };
    // names up to 255 bytes (2-byte length form) and real-sized trees still open
    auto ok = open_bytes(wua_chain(255, 3, 10));
    HDF_CHECK(ok.has_value());
    if (ok) HDF_CHECK_EQ((*ok)->file_count(), 10u);
    auto ok2 = open_bytes(wua_chain(200, 4, 500));  // 500 keys of about 810 B: within 1 MiB + 16 x (names + tree)
    HDF_CHECK(ok2.has_value());
    if (ok2) HDF_CHECK_EQ((*ok2)->file_count(), 500u);
    // a name over 255 bytes
    HDF_CHECK_EQ(code_of(open_bytes(wua_chain(256, 1, 1))), std::string("wua.damaged"));
    // a full path over 1 KiB (5 x 251 B)
    HDF_CHECK_EQ(code_of(open_bytes(wua_chain(250, 5, 1))), std::string("wua.damaged"));
    // 5,000 keys of about 810 B (4 MB) from a 110 KB index: over the budget
    const Bytes many = wua_chain(200, 4, 5000);
    HDF_CHECK(many.size() < 120000);
    HDF_CHECK_EQ(code_of(open_bytes(many)), std::string("wua.damaged"));
    // the review's probe shape: 31 folders of 32,767 B, 300 files (asked for about 300 MB of keys before)
    const auto t0 = std::chrono::steady_clock::now();
    HDF_CHECK_EQ(code_of(open_bytes(wua_chain(32767, 31, 300))), std::string("wua.damaged"));
    HDF_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

// ---------------------------------------------------------------------------------------------- .zip
HDF_TEST(base_zip_source) {
    TempDir t;
    Bytes big(5000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i % 17);
    const Bytes zip = build_zip({{"mod.json", bytes_of("{}")}, {"res/", {}}, {"res/Cave/big.bin", big}});
    write_file(t.path / "a.dusk", zip);
    auto z = ZipSource::open(t.path / "a.dusk");
    HDF_CHECK(z.has_value());
    if (!z) return;
    const std::vector<std::string> names = (*z)->list();
    HDF_CHECK_EQ(names.size(), 2u);  // the folder entry is not listed
    HDF_CHECK_EQ(names[1], std::string("res/Cave/big.bin"));
    HDF_CHECK((*z)->read("mod.json").value() == bytes_of("{}"));
    HDF_CHECK((*z)->read("RES/cave/BIG.bin").value() == big);
    HDF_CHECK_EQ((*z)->size("res/cave/big.bin").value_or(0), 5000u);
    HDF_CHECK_EQ(code_of((*z)->read("nope")), std::string("io.missing"));

    Bytes bad = zip;
    bad[30 + 8] ^= 0x55;  // the first byte of the stored mod.json data: CRC-32 differs
    write_file(t.path / "b.dusk", bad);
    auto zb = ZipSource::open(t.path / "b.dusk");
    HDF_CHECK(zb.has_value());
    if (zb) HDF_CHECK_EQ(code_of((*zb)->read("mod.json")), std::string("zip.damaged"));
    write_file(t.path / "c.dusk", slice(zip, 0, zip.size() - 10));
    HDF_CHECK_EQ(code_of(ZipSource::open(t.path / "c.dusk")), std::string("zip.bad"));
}

// ---------------------------------------------------------------------------------------------- inputs
HDF_TEST(base_input_tables) {
    HDF_CHECK_EQ(inputs::hd_files().size(), inputs::kHdCount);
    HDF_CHECK_EQ(inputs::gc_files().size(), inputs::kGcCount);
    HDF_CHECK(inputs::find_hd("LOS.BIN") != nullptr);
    HDF_CHECK(inputs::find_hd("/res/Stage/D_SB11/STG_00.arc") != nullptr);
    HDF_CHECK(inputs::find_hd("res/object/logous.pack.gz") == nullptr);  // art only, not a converter input
    HDF_CHECK(inputs::find_gc("res/Particle/Pscene200.jpc") != nullptr);
    HDF_CHECK(inputs::find_gc("res/Stage/D_SB01/STG_00.arc") == nullptr);  // only fed a log line (plan 2.5)
    for (const inputs::InputFile& f : inputs::hd_files()) {
        HDF_CHECK_EQ(f.sha256.size(), 64u);
        HDF_CHECK(f.size > 0);
        HDF_CHECK_EQ(lower_key(f.path), std::string(f.path));
    }
}

HDF_TEST(base_input_checks_and_checked_source) {
    const Bytes a = bytes_of("alpha"), b = bytes_of("bravo!");
    const std::string ha = sha256_hex(a), hb = sha256_hex(b);
    const inputs::InputFile list[] = {{"res/a.bin", 5, ha}, {"res/b.bin", 6, hb}, {"res/c.bin", 3, ha}};
    MemorySource m;
    m.add("res/a.bin", a);
    m.add("res/b.bin", bytes_of("bravo?"));  // right size, other bytes
    auto sizes = inputs::check_sizes(m, list);
    HDF_CHECK_EQ(sizes.total, 3u);
    HDF_CHECK_EQ(sizes.present, 2u);
    HDF_CHECK_EQ(sizes.size_ok, 2u);
    HDF_CHECK(!sizes.ok());
    auto hashes = inputs::check_hashes(m, list, nullptr);
    HDF_CHECK(hashes.has_value());
    HDF_CHECK_EQ(hashes->hash_ok, 1u);
    HDF_CHECK_EQ(hashes->problems.size(), 2u);
    Progress p;
    m.add("res/b.bin", b);
    m.add("res/c.bin", bytes_of("toolong"));
    auto h2 = inputs::check_hashes(m, list, &p);
    HDF_CHECK_EQ(h2->hash_ok, 2u);
    HDF_CHECK_EQ(p.total.load(), 14u);
    HDF_CHECK_EQ(p.done.load(), 14u);
    std::stop_source s;
    s.request_stop();
    HDF_CHECK_EQ(code_of(inputs::check_hashes(m, list, nullptr, s.get_token())), std::string("stopped"));

    inputs::CheckedSource c(m, list, "hd");
    HDF_CHECK(c.read("RES/A.BIN").value() == a);
    HDF_CHECK(c.read("res/b.bin").value() == b);
    HDF_CHECK_EQ(code_of(c.read("res/c.bin")), std::string("hd.size"));
    HDF_CHECK_EQ(code_of(c.read("res/z.bin")), std::string("hd.unlisted"));
    HDF_CHECK(!c.size("res/z.bin"));
    m.add("res/a.bin", bytes_of("alphA"));
    HDF_CHECK_EQ(code_of(c.read("res/a.bin")), std::string("hd.hash"));
    MemorySource empty;
    inputs::CheckedSource ce(empty, list, "gc");
    HDF_CHECK_EQ(code_of(ce.read("res/a.bin")), std::string("gc.missing"));
}

// ---------------------------------------------------------------------------------------------- outputs
HDF_TEST(base_output_tables_and_set_id) {
    const auto exp = outputs::expected();
    HDF_CHECK_EQ(exp.size(), outputs::kKeptCount);
    HDF_CHECK_EQ(outputs::drop_list().size(), outputs::kDroppedCount);
    HDF_CHECK_EQ(outputs::compute_set_id(exp), std::string(outputs::embedded_set_id()));
    HDF_CHECK(outputs::find("overlay/res/Stage/D_SB11/STG_00.arc") != nullptr);
    HDF_CHECK(outputs::find("overlay/res/Stage/D_SB11/STG_00.arc")->overlay);
    HDF_CHECK(outputs::find("res/cave/hdtext/messages.bin") != nullptr);
    HDF_CHECK(outputs::find("res/cave/floors.json") == nullptr);
    HDF_CHECK(outputs::is_dropped("res/cave/floors.json"));
    HDF_CHECK(outputs::is_dropped("res/cave/amiibo/paper.png"));
    HDF_CHECK(!outputs::is_dropped("res/cave/amiibo/popup.png"));
    HDF_CHECK(outputs::find("res/cave/amiibo/popup.png") != nullptr);
    // kept and dropped never overlap
    for (std::string_view d : outputs::drop_list()) HDF_CHECK(outputs::find(d) == nullptr);
    // the set id moves when anything in the list moves
    std::vector<outputs::ExpectedFile> v(exp.begin(), exp.end());
    v[3].size += 1;
    HDF_CHECK(outputs::compute_set_id(v) != std::string(outputs::embedded_set_id()));
    std::vector<outputs::ExpectedFile> r(exp.rbegin(), exp.rend());  // order does not matter
    HDF_CHECK_EQ(outputs::compute_set_id(r), std::string(outputs::embedded_set_id()));
}

HDF_TEST(base_output_check_records) {
    std::vector<OutputRecord> made;
    for (const outputs::ExpectedFile& e : outputs::expected())
        made.push_back({std::string(e.rel), e.size, std::string(e.sha256), e.overlay});
    auto all = outputs::check(made);
    HDF_CHECK(all.pass());
    HDF_CHECK_EQ(all.identical, 176u);
    made.erase(made.begin());                                  // missing
    made[0].sha256[0] = made[0].sha256[0] == '0' ? '1' : '0';  // differs
    made.push_back({"res/cave/floors.json", 1, "x", false});   // dropped by design
    made.push_back({"res/extra.bin", 1, "x", false});          // extra
    auto r = outputs::check(made);
    HDF_CHECK(!r.pass());
    HDF_CHECK_EQ(r.identical, 174u);
    HDF_CHECK_EQ(r.missing, 1u);
    HDF_CHECK_EQ(r.diff, 1u);
    HDF_CHECK_EQ(r.dropped, 1u);
    HDF_CHECK_EQ(r.extra, 1u);
    HDF_CHECK(r.summary().find("FAIL") != std::string::npos);
}

HDF_TEST(base_compare_refuses_other_oracles) {
    TempDir t;
    write_file(t.path / "fake.dusk", build_zip({{"mod.json", bytes_of("{}")}}));
    fs::create_directories(t.path / "out");
    HDF_CHECK_EQ(code_of(outputs::compare(t.path / "out", t.path / "fake.dusk")), std::string("oracle.pin"));
    HDF_CHECK_EQ(code_of(outputs::compare(t.path / "out", t.path / "none.dusk")), std::string("oracle.missing"));
    // a folder oracle must hold the pinned bytes
    fs::create_directories(t.path / "oracle");
    HDF_CHECK_EQ(code_of(outputs::compare(t.path / "out", t.path / "oracle")), std::string("oracle.pin"));
}
