// Unit tests of module D (src/detect): Cemu XML parsers, product codes, drive classes, find() over synthetic
// Cemu setups (portable Cemu, mlc base + update, odd characters, entity-encoded paths, GamePaths scans, picked
// folders), budgets, stop, reparse points and offline files, open() and check_sizes(). Synthetic data only: the
// 136 "inputs" are zero-filled sparse files with the embedded sizes, .wua/.wux/.tmd files are hand-made headers.
// Temporary files go to a fresh folder under %TEMP% and are removed.
#include "test.hpp"

#include "hdf/detect.hpp"
#include "hdf/inputs.hpp"
#include "hdf/source.hpp"

#include "../src/base/base_util.hpp"
#include "../src/detect/detect_internal.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

using namespace hdf;
namespace fs = std::filesystem;
namespace di = hdf::detect::internal;
using detect::Candidate;

// ---------------------------------------------------------------------------------------------- helpers
template <class T>
std::string code_of(const Result<T>& r) {
    return r ? std::string("ok") : r.error().code;
}
template <class T>
std::string detail_of(const Result<T>& r) {
    return r ? std::string() : r.error().detail;
}

fs::path P(std::string_view utf8) { return di::path_from_utf8(utf8).value_or(fs::path("?invalid?")); }
std::string U(const fs::path& p) { return base::utf8_lossy(p, true); }

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("hdf-detect-" + std::to_string(rd()) + "-" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
#ifdef _WIN32
        // clear attributes set by the tests (offline) so the folder can be removed
        for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
            SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& p, std::string_view data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void write_bytes(const fs::path& p, const Bytes& data) {
    write_file(p, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

// A zero-filled file of `size` bytes; sparse on NTFS, so 136 of them cost no disk space.
void make_sized(const fs::path& p, u64 size) {
    fs::create_directories(p.parent_path());
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD ret = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
#else
    { std::ofstream f(p, std::ios::binary | std::ios::trunc); }
    fs::resize_file(p, size);
#endif
}

enum class Content { Full, LosOnly, None };

// The 136 inputs with their embedded sizes; `skip` left out, `wrong` one byte longer.
void make_content(const fs::path& content, Content mode, const std::set<std::string>& skip = {},
                  const std::set<std::string>& wrong = {}) {
    fs::create_directories(content);
    for (const inputs::InputFile& f : inputs::hd_files()) {
        const std::string path(f.path);
        if (mode == Content::None) break;
        if (mode == Content::LosOnly && path != "los.bin") continue;
        if (skip.count(path)) continue;
        make_sized(content / P(path), f.size + (wrong.count(path) ? 1 : 0));
    }
}

std::string meta_xml(std::string_view tid, std::string_view product, u32 version) {
    std::string s = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<menu type=\"complex\" access=\"777\">\n";
    s += "  <version type=\"unsignedInt\" length=\"4\">33</version>\n";
    s += "  <product_code type=\"string\" length=\"32\">" + std::string(product) + "</product_code>\n";
    s += "  <title_version type=\"unsignedInt\" length=\"4\">" + std::to_string(version) + "</title_version>\n";
    s += "  <title_id type=\"hexBinary\" length=\"8\">" + std::string(tid) + "</title_id>\n";
    s += "</menu>\n";
    return s;
}

// A Cemu title folder: code/, meta/meta.xml, content/.
void make_title(const fs::path& dir, std::string_view tid, std::string_view product, u32 version, Content mode,
                const std::set<std::string>& skip = {}, const std::set<std::string>& wrong = {}) {
    write_file(dir / "code" / "game.rpx", "rpx");
    write_file(dir / "meta" / "meta.xml", meta_xml(tid, product, version));
    make_content(dir / "content", mode, skip, wrong);
}

std::string xml_escape(std::string_view s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o.push_back(c);
    }
    return o;
}

struct TitleRow {
    std::string tid;
    u32 version;
    int format;
    fs::path path;
    std::string sub_path;
};

std::string title_list_xml(const std::vector<TitleRow>& rows) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<title_list>\n";
    for (const TitleRow& r : rows) {
        s += " <title titleId=\"" + r.tid + "\" version=\"" + std::to_string(r.version) +
             "\" sdk_version=\"21213\" group_id=\"000019e5\" app_type=\"80000000\">\n";
        s += "  <region>2</region>\n  <name>A &amp; B</name>\n";
        s += "  <format>" + std::to_string(r.format) + "</format>\n";
        s += "  <path>" + xml_escape(U(r.path)) + "</path>\n";
        if (!r.sub_path.empty()) s += "  <sub_path>" + r.sub_path + "</sub_path>\n";
        s += " </title>\n";
    }
    s += "</title_list>\n";
    return s;
}

std::string settings_xml(const std::optional<fs::path>& mlc, const std::vector<fs::path>& game_paths) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<content>\n    <logflag>0</logflag>\n";
    if (mlc) s += "    <mlc_path>" + xml_escape(U(*mlc)) + "</mlc_path>\n";
    s += "    <RecentLaunchFiles>\n        <Entry>X:/recent/never.wua</Entry>\n    </RecentLaunchFiles>\n";
    s += "    <GamePaths>\n";
    for (const fs::path& g : game_paths) s += "        <Entry>" + xml_escape(U(g)) + "</Entry>\n";
    s += "    </GamePaths>\n";
    s += "    <GraphicPack>\n        <Entry filename=\"graphicPacks/x/rules.txt\"/>\n    </GraphicPack>\n</content>\n";
    return s;
}

void put_be(Bytes& b, u64 v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void put_le(Bytes& b, u64 v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}

// ---------------------------------------------------------------------------------------------- synthetic .wua
// Block 0 (stored raw) holds the files that have bytes (meta.xml); every other file only has a size (offset 0),
// and the offset records describe enough blocks for the largest one. Nothing but meta.xml is ever read.
constexpr u64 kBlock = 65536;

struct WuaFile {
    std::string path;
    u64 size = 0;
    std::string data;  // non-empty: stored in block 0, size = data.size()
};

Bytes build_wua(const std::vector<WuaFile>& files) {
    struct Node {
        std::map<std::string, Node> dirs;
        std::map<std::string, std::pair<u64, u64>> files;
    };
    Node root;
    Bytes block(kBlock, 0);
    u64 at = 0, max_end = kBlock;
    for (const WuaFile& f : files) {
        std::pair<u64, u64> rec{0, f.size};
        if (!f.data.empty()) {
            std::copy(f.data.begin(), f.data.end(), block.begin() + static_cast<std::ptrdiff_t>(at));
            rec = {at, f.data.size()};
            at += f.data.size();
        }
        max_end = std::max(max_end, rec.first + rec.second);
        Node* n = &root;
        std::size_t i = 0;
        while (true) {
            const std::size_t j = f.path.find('/', i);
            if (j == std::string::npos) {
                n->files[f.path.substr(i)] = rec;
                break;
            }
            n = &n->dirs[f.path.substr(i, j - i)];
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
    struct Pending {
        const Node* node;
        std::size_t entry;
    };
    tree.resize(16);
    std::vector<Pending> queue{{&root, 0}};
    for (std::size_t q = 0; q < queue.size(); ++q) {
        const Node* n = queue[q].node;
        const std::size_t first = tree.size() / 16, count = n->dirs.size() + n->files.size();
        const std::size_t e = queue[q].entry * 16;
        for (int i = 0; i < 4; ++i) {
            tree[e + 4 + i] = static_cast<u8>(first >> (24 - 8 * i));
            tree[e + 8 + i] = static_cast<u8>(count >> (24 - 8 * i));
            tree[e + 12 + i] = 0;
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
    const u64 records = (max_end + 16 * kBlock - 1) / (16 * kBlock);
    Bytes out = block;
    const u64 off_rec = out.size();
    for (u64 r = 0; r < records; ++r) {
        put_be(out, r * 16 * kBlock, 8);
        for (int i = 0; i < 16; ++i) put_be(out, kBlock - 1, 2);
    }
    const u64 off_names = out.size();
    out.insert(out.end(), names.begin(), names.end());
    const u64 off_tree = out.size();
    out.insert(out.end(), tree.begin(), tree.end());
    const u64 off_end = out.size();
    const u64 secs[6][2] = {{0, kBlock}, {off_rec, records * 40}, {off_names, names.size()},
                            {off_tree, tree.size()}, {off_end, 0}, {off_end, 0}};
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

// One title of a synthetic .wua: <top>/content/<the 136 inputs> (+ meta/meta.xml when meta is set).
void add_wua_title(std::vector<WuaFile>& files, const std::string& top, Content mode, const std::string& meta = {},
                   const std::set<std::string>& skip = {}, const std::set<std::string>& wrong = {}) {
    if (!meta.empty()) files.push_back({top + "/meta/meta.xml", 0, meta});
    files.push_back({top + "/code/game.rpx", 3, {}});
    for (const inputs::InputFile& f : inputs::hd_files()) {
        const std::string path(f.path);
        if (mode == Content::None) break;
        if (mode == Content::LosOnly && path != "los.bin") continue;
        if (skip.count(path)) continue;
        files.push_back({top + "/content/" + path, f.size + (wrong.count(path) ? 1 : 0), {}});
    }
}

void make_wua(const fs::path& file, const std::vector<WuaFile>& files) { write_bytes(file, build_wua(files)); }

// A .wux header: "WUX0", the second magic, sector size 0x8000, one sector, index 0; the product code in sector 0.
Bytes wux_bytes(std::string_view product) {
    Bytes b;
    put_le(b, 0x30585557u, 4);
    put_le(b, 0x1099D02Eu, 4);
    put_le(b, 0x8000, 4);
    put_le(b, 0, 4);
    put_le(b, 0x8000, 8);
    put_le(b, 0, 8);
    put_le(b, 0, 4);  // index[0]
    b.resize(0x8000, 0);
    b.insert(b.end(), product.begin(), product.end());
    b.resize(0x8000 + 64, 0);
    return b;
}

Bytes tmd_bytes(u64 title_id) {
    Bytes b;
    put_be(b, 0x00010004u, 4);
    b.resize(0x18C, 0);
    put_be(b, title_id, 8);
    b.resize(0x300, 0);
    return b;
}

// One line per candidate: kind|origin|path|title_dir|update|problem (paths relative to root).
std::string rel(const fs::path& p, const fs::path& root) { return U(p.lexically_relative(root)); }

std::string summary(const std::vector<Candidate>& cs, const fs::path& root) {
    std::string s;
    for (const Candidate& c : cs) {
        s += std::string(detect::kind_name(c.kind)) + "|" + c.origin + "|" + rel(c.path, root) + "|" + c.title_dir + "|" +
             (c.update ? rel(*c.update, root) : std::string()) + "|" + c.problem + "\n";
    }
    return s;
}

struct Log {
    std::vector<std::string> lines;
    bool has(std::string_view part) const {
        for (const std::string& l : lines)
            if (l.find(part) != std::string::npos) return true;
        return false;
    }
};

detect::Env env_for(const fs::path& appdata, Log& log) {
    detect::Env env;
    env.appdata = appdata;
    env.localappdata = appdata / "none";
    env.log = [&log](std::string_view l) { log.lines.emplace_back(l); };
    return env;
}

// A Source that only answers sizes (check_sizes tests): the embedded sizes, minus `missing`, `wrong` + 1.
class SizeSource final : public Source {
public:
    std::set<std::string> missing, wrong;
    std::optional<u64> size(std::string_view path) const override {
        const inputs::InputFile* f = inputs::find_hd(path);
        if (!f || missing.count(std::string(f->path))) return std::nullopt;
        return f->size + (wrong.count(std::string(f->path)) ? 1 : 0);
    }
    Result<Bytes> read(std::string_view, std::stop_token) const override { return fail("io.missing"); }
    std::string describe() const override { return "sizes"; }
};

std::vector<std::string> first_inputs(std::size_t n) {
    std::vector<std::string> out;
    for (const inputs::InputFile& f : inputs::hd_files())
        if (out.size() < n) out.emplace_back(f.path);
    return out;
}

#ifdef _WIN32
// A real NTFS junction at `link` (made as an empty folder first) to the NT path `target` ("\??\C:\x",
// "\??\Volume{..}\"). No privilege needed. False when the file system refuses it.
bool make_junction(const fs::path& link, const std::wstring& target) {
    fs::create_directories(link);
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    // the print name: the target without "\??\"
    const std::wstring print = target.compare(0, 4, L"\\??\\") == 0 ? target.substr(4) : target;
    Bytes buf;
    const u64 path_bytes = (target.size() + 1 + print.size() + 1) * 2;
    put_le(buf, 0xA0000003u, 4);  // IO_REPARSE_TAG_MOUNT_POINT
    put_le(buf, 8 + path_bytes, 2);
    put_le(buf, 0, 2);
    put_le(buf, 0, 2);
    put_le(buf, target.size() * 2, 2);
    put_le(buf, (target.size() + 1) * 2, 2);
    put_le(buf, print.size() * 2, 2);
    for (wchar_t c : target) put_le(buf, static_cast<u16>(c), 2);
    put_le(buf, 0, 2);
    for (wchar_t c : print) put_le(buf, static_cast<u16>(c), 2);
    put_le(buf, 0, 2);
    DWORD ret = 0;
    const bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0,
                                    &ret, nullptr) != 0;
    CloseHandle(h);
    return ok;
}
#endif

// xml_unescape before fix pass F1 (a ';' searched over the whole rest of the text): the reference for the new one.
std::string slow_unescape(std::string_view s) {
    auto hexv = [](char d) { return d <= '9' ? d - '0' : (d | 0x20) - 'a' + 10; };
    auto is_hex = [](char d) { return (d >= '0' && d <= '9') || ((d | 0x20) >= 'a' && (d | 0x20) <= 'f'); };
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        const std::size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back('&');
            ++i;
            continue;
        }
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        std::string rep;
        if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos") rep = "'";
        else if (ent.size() >= 2 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const std::string_view digits = ent.substr(hex ? 2 : 1);
            u32 v = 0;
            bool ok = !digits.empty() && digits.size() <= (hex ? 6u : 7u);
            for (char d : digits) {
                if (!ok) break;
                if (hex ? !is_hex(d) : (d < '0' || d > '9')) ok = false;
                else v = v * (hex ? 16u : 10u) + static_cast<u32>(hex ? hexv(d) : d - '0');
            }
            if (ok && v != 0 && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) {
                if (v < 0x80) {
                    rep.push_back(static_cast<char>(v));
                } else if (v < 0x800) {
                    rep.push_back(static_cast<char>(0xC0 | (v >> 6)));
                    rep.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                } else if (v < 0x10000) {
                    rep.push_back(static_cast<char>(0xE0 | (v >> 12)));
                    rep.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
                    rep.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                } else {
                    rep.push_back(static_cast<char>(0xF0 | (v >> 18)));
                    rep.push_back(static_cast<char>(0x80 | ((v >> 12) & 0x3F)));
                    rep.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
                    rep.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                }
            }
        }
        if (!rep.empty()) {
            out += rep;
            i = semi + 1;
        } else {
            out.push_back('&');
            ++i;
        }
    }
    return out;
}

double secs_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

const std::string kUsaTop = "000500001019e500_v81";
const std::string kE = "\xC3\xA9";                     // é
const std::string kU = "\xC3\xBC";                     // ü
const std::string kJa = "\xE6\x97\xA5\xE6\x9C\xAC";   // 日本

}  // namespace

// ---------------------------------------------------------------------------------------------- parsers
HDF_TEST(detect_xml_unescape) {
    using detect::xml_unescape;
    HDF_CHECK_EQ(xml_unescape("a &amp; b"), std::string("a & b"));
    HDF_CHECK_EQ(xml_unescape("&lt;&gt;&quot;&apos;"), std::string("<>\"'"));
    HDF_CHECK_EQ(xml_unescape("&#233;&#xE9;&#XE9;"), kE + kE + kE);
    HDF_CHECK_EQ(xml_unescape("&#x1F600;"), std::string("\xF0\x9F\x98\x80"));
    HDF_CHECK_EQ(xml_unescape("C:&#x2F;x"), std::string("C:/x"));
    for (std::string_view kept : {"&#xD800;", "&#0;", "&#x110000;", "&bogus;", "& alone", "&amp", "&#;", "&#x;",
                                  "&#12a;", "&#xFFFFFFFFF;", "&"})
        HDF_CHECK_EQ(xml_unescape(kept), std::string(kept));
    HDF_CHECK_EQ(xml_unescape("&amp;amp;"), std::string("&amp;"));  // one level only
}

HDF_TEST(detect_parse_title_list) {
    const std::string xml =
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<title_list>\r\n"
        " <!-- <title titleId=\"000500001019E500\" version=\"1\"><format>1</format><path>C:/commented</path></title> -->\r\n"
        " <title titleId=\"000500001019e500\" version=\"81\" sdk_version=\"21213\">\r\n"
        "  <region>2</region>\r\n  <name>THE LEGEND OF ZELDA &amp; Co</name>\r\n  <format>3</format>\r\n"
        "  <path>D:/Games &amp; Stuff (Wii U)/Zelda " + kE + " " + kJa + "/TP HD.wua</path>\r\n"
        "  <sub_path>000500001019e500_v81</sub_path>\r\n </title>\r\n"
        " <title titleId='0005000e1019E600' version='17'><extra><path>X:/nested</path></extra>"
        "<format>1</format><path> E:/upd&#x2F;eur </path></title>\n"
        " <title titleId=\"0005000010143500\" version=\"0\"><format>2</format><path>F:/ww.wux</path></title>\n"
        " <title titleId=\"000500001019C800\"/>\n"
        " <title version=\"99999999999\"><format>x</format><path><![CDATA[C:/a & b]]></path></title>\n"
        "</title_list>\n";
    const auto t = detect::parse_title_list(xml);
    HDF_CHECK_EQ(t.size(), 5u);
    if (t.size() != 5) return;
    HDF_CHECK_EQ(t[0].title_id, std::string("000500001019e500"));
    HDF_CHECK_EQ(t[0].version, 81u);
    HDF_CHECK_EQ(t[0].format, 3);
    HDF_CHECK_EQ(t[0].path, "D:/Games & Stuff (Wii U)/Zelda " + kE + " " + kJa + "/TP HD.wua");
    HDF_CHECK_EQ(t[0].sub_path, std::string("000500001019e500_v81"));
    HDF_CHECK_EQ(t[1].title_id, std::string("0005000e1019E600"));
    HDF_CHECK_EQ(t[1].version, 17u);
    HDF_CHECK_EQ(t[1].format, 1);
    HDF_CHECK_EQ(t[1].path, std::string("E:/upd/eur"));  // trimmed; the nested <path> is not the title's
    HDF_CHECK_EQ(t[2].format, 2);
    HDF_CHECK_EQ(t[3].title_id, std::string("000500001019C800"));
    HDF_CHECK_EQ(t[3].format, 0);
    HDF_CHECK(t[3].path.empty());
    HDF_CHECK(t[4].title_id.empty());
    HDF_CHECK_EQ(t[4].version, 0u);  // too many digits
    HDF_CHECK_EQ(t[4].format, 0);
    HDF_CHECK_EQ(t[4].path, std::string("C:/a & b"));  // CDATA kept as is
    HDF_CHECK(detect::parse_title_list("").empty());
    HDF_CHECK(detect::parse_title_list("<title_list></title_list>").empty());
}

HDF_TEST(detect_parse_settings) {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<content>\n    <logflag>0</logflag>\n"
        "    <!-- <mlc_path>X:/old</mlc_path> -->\n"
        "    <Account><mlc_path>Z:/not-the-root-one</mlc_path></Account>\n"
        "    <mlc_path>G:/Cemu &amp; co/mlc01</mlc_path>\n"
        "    <mlc_path>H:/second</mlc_path>\n"
        "    <RecentLaunchFiles><Entry>F:/recent.wux</Entry></RecentLaunchFiles>\n"
        "    <GamePaths>\n        <Entry>F:/Wii U (games)</Entry>\n        <Entry>  C:/Spiele/Zelda " + kE +
        "  </Entry>\n        <Entry></Entry>\n        <Entry/>\n    </GamePaths>\n"
        "    <GraphicPack><Entry filename=\"graphicPacks/x/rules.txt\"/></GraphicPack>\n</content>\n";
    const detect::CemuSettings s = detect::parse_settings(xml);
    HDF_CHECK_EQ(s.mlc_path, std::string("G:/Cemu & co/mlc01"));
    HDF_CHECK_EQ(s.game_paths.size(), 2u);
    if (s.game_paths.size() == 2) {
        HDF_CHECK_EQ(s.game_paths[0], std::string("F:/Wii U (games)"));
        HDF_CHECK_EQ(s.game_paths[1], "C:/Spiele/Zelda " + kE);
    }
    const detect::CemuSettings e = detect::parse_settings("<content><GamePaths/></content>");
    HDF_CHECK(e.mlc_path.empty() && e.game_paths.empty());
    // this PC's perm_setting.xml shape: an empty <MlcPath>
    HDF_CHECK(di::xml_first_text("<?xml encoding=\"UTF-8\"?>\n<config>\n\t<MlcPath></MlcPath>\n</config>", "MlcPath").empty());
    HDF_CHECK_EQ(di::xml_first_text("<config><MlcPath> D:/m&amp;lc </MlcPath></config>", "MlcPath"), std::string("D:/m&lc"));
}

HDF_TEST(detect_parse_meta) {
    const detect::MetaXml m = detect::parse_meta(meta_xml("000500001019e500", "WUP-P-AZAE", 81));
    HDF_CHECK_EQ(m.title_id, std::string("000500001019E500"));
    HDF_CHECK_EQ(m.product_code, std::string("WUP-P-AZAE"));
    HDF_CHECK_EQ(m.title_version.value_or(999), 81u);
    const detect::MetaXml n = detect::parse_meta("<menu><title_id>12</title_id><title_version>x</title_version></menu>");
    HDF_CHECK(n.title_id.empty() && n.product_code.empty() && !n.title_version);
    // only the root's children count
    const detect::MetaXml d = detect::parse_meta("<menu><x><title_id>000500001019E500</title_id></x></menu>");
    HDF_CHECK(d.title_id.empty());
    HDF_CHECK(di::is_tphd_product("wup-p-azap"));
    HDF_CHECK(!di::is_tphd_product("WUP-P-BCZE"));
    HDF_CHECK_EQ(di::tphd_title_of_product("WUP-P-AZAE"), std::string("000500001019E500"));
    HDF_CHECK_EQ(di::tphd_title_of_product("WUP-P-AZAP"), std::string("000500001019E600"));
    HDF_CHECK_EQ(di::tphd_title_of_product("WUP-P-AZAJ"), std::string("000500001019C800"));
    HDF_CHECK(di::tphd_title_of_product("WUP-P-AZA").empty());
    const auto tt = di::top_title("0005000e1019e500_v81");
    HDF_CHECK(tt.has_value());
    if (tt) {
        HDF_CHECK_EQ(tt->first, std::string("0005000E1019E500"));
        HDF_CHECK_EQ(tt->second, 81u);
    }
    HDF_CHECK(!di::top_title("0005000e1019e500_V81"));
    HDF_CHECK(!di::top_title("0005000e1019e500_v"));
    HDF_CHECK(!di::top_title("0005000e1019e50_v81"));
}

HDF_TEST(detect_is_tphd_title) {
    for (std::string_view yes : {"000500001019E500", "0005000e1019e600", "000500001019c800", "0005000E1019C800"})
        HDF_CHECK(detect::is_tphd_title(yes));
    for (std::string_view no : {"0005000010143500", "1019E500", "", "000500001019E50G", "000500001019E5000",
                                "0005000e101c9500"})
        HDF_CHECK(!detect::is_tphd_title(no));
}

HDF_TEST(detect_path_from_utf8) {
    const auto p = di::path_from_utf8("C:/a " + kE + "/" + kJa + "/\xF0\x9F\x98\x80");
    HDF_CHECK(p.has_value());
#ifdef _WIN32
    if (p) HDF_CHECK(p->native() == std::wstring(L"C:/a \u00e9/\u65e5\u672c/\U0001F600"));
#endif
    for (std::string_view bad : {std::string_view("\xC3"), std::string_view("\xC0\xAF"), std::string_view("\xE0\x80\xAF"),
                                 std::string_view("\xED\xA0\x80"), std::string_view("\xF4\x90\x80\x80"),
                                 std::string_view("a\x01" "b"), std::string_view(""), std::string_view("\xFF"),
                                 std::string_view("a\0b", 3)})
        HDF_CHECK(!di::path_from_utf8(bad));
    HDF_CHECK(!di::path_from_utf8(std::string(40000, 'a')));
}

// Correction #1(b): truncated and garbage XML never crash or hang.
HDF_TEST(detect_garbage_never_crashes) {
    const std::string samples[] = {
        title_list_xml({{"000500001019e500", 81, 3, P("D:/a & b/" + kJa + ".wua"), kUsaTop}}),
        settings_xml(fs::path("G:/mlc"), {fs::path("F:/x"), fs::path("F:/y")}),
        meta_xml("000500001019E500", "WUP-P-AZAE", 81),
        "<a b='1' c=\"2\" d e=>text &amp; &#x41; <![CDATA[x]]> <!-- c --> <?p?> <!DOCTYPE x></a>",
    };
    std::size_t n = 0;
    for (const std::string& s : samples) {
        for (std::size_t len = 0; len <= s.size(); ++len) {
            const std::string_view part(s.data(), len);
            n += detect::parse_title_list(part).size() + detect::parse_settings(part).game_paths.size();
            n += detect::parse_meta(part).title_id.size() + detect::xml_unescape(part).size();
        }
    }
    std::mt19937 rng(12345);
    const std::string alphabet = "<>/=\"' &;#x!-[]?atitleIdpathformat_0123456789\xC3\xA9\n";
    for (int round = 0; round < 3000; ++round) {
        std::string s(1 + rng() % 200, ' ');
        for (char& c : s) c = alphabet[rng() % alphabet.size()];
        if (round % 7 == 0) {
            for (char& c : s) c = static_cast<char>(rng() & 0xFF);
        }
        n += detect::parse_title_list(s).size() + detect::parse_settings(s).game_paths.size();
        n += detect::parse_meta(s).product_code.size() + detect::xml_unescape(s).size();
        n += di::path_from_utf8(s).has_value() ? 1 : 0;
    }
    // deep nesting, a huge attribute without its closing quote, an unclosed comment
    std::string deep;
    for (int i = 0; i < 100000; ++i) deep += "<a>";
    n += detect::parse_title_list(deep).size() + detect::parse_settings(deep).game_paths.size();
    n += detect::parse_meta(deep).title_id.size();
    n += detect::parse_title_list("<title titleId=\"" + std::string(1 << 20, 'x')).size();
    n += detect::parse_settings("<content><!-- " + std::string(1 << 20, '-')).game_paths.size();
    HDF_CHECK(n < (std::size_t(1) << 40));  // only that it ran
}

// Fix pass F1: entity decoding is linear and gives the same result as before. Files of '&' at the read caps parse
// in well under a second (before: 66 s for a 4 MiB settings.xml, with no stop check), and a stop is quick.
HDF_TEST(detect_entities_linear) {
    // the same result as the old decoder: random text rich in '&', ';', '#', 'x', digits and entity letters
    std::mt19937 rng(777);
    const std::string alphabet = "&&&;;##xX0123456789abcdefABCDEFampltgquos \xC3\xA9";
    std::size_t differ = 0;
    for (int round = 0; round < 20000; ++round) {
        std::string s(1 + rng() % 64, ' ');
        for (char& c : s) c = alphabet[rng() % alphabet.size()];
        if (detect::xml_unescape(s) != slow_unescape(s)) ++differ;
    }
    // a ';' at every distance from the '&', around the 12-byte window
    for (std::string_view head : {"&", "&#", "&#x", "&#0", "&#x0", "&amp", "&#00000", "&#x00000"}) {
        for (std::size_t gap = 0; gap <= 16; ++gap) {
            for (char fill : {'1', '4', 'A', 'z', '&'}) {
                const std::string s = "a" + std::string(head) + std::string(gap, fill) + ";b&#65;";
                if (detect::xml_unescape(s) != slow_unescape(s)) ++differ;
            }
        }
    }
    HDF_CHECK_EQ(differ, std::size_t(0));
    HDF_CHECK_EQ(detect::xml_unescape("&#1114111;&#x10FFFF;&#0000065;"),
                 std::string("\xF4\x8F\xBF\xBF\xF4\x8F\xBF\xBF" "A"));

    const std::string amps(std::size_t(4) << 20, '&');
    auto t0 = std::chrono::steady_clock::now();
    HDF_CHECK(detect::xml_unescape(amps) == amps);
    const detect::CemuSettings s = detect::parse_settings("<content><mlc_path>" + amps + "</mlc_path></content>");
    HDF_CHECK_EQ(s.mlc_path.size(), std::size_t(64 * 1024));  // the kept text is capped
    const auto tl = detect::parse_title_list("<title_list><title titleId=\"" + amps + "\" version=\"1\"><path>" + amps +
                                             "</path></title></title_list>");
    HDF_CHECK_EQ(tl.size(), 1u);
    // the 16 MiB title list cap: every '&' sees a ';' at the edge of the window
    std::string edge;
    edge.reserve((std::size_t(16) << 20) + 16);
    while (edge.size() < (std::size_t(16) << 20)) edge += "&&&&&&&&&&&&;";
    HDF_CHECK(detect::xml_unescape(edge) == edge);
    const double parse_secs = secs_since(t0);
    HDF_CHECK(parse_secs < 5.0);  // about 0.1 s here; the old decoder needed many minutes for this
    if (parse_secs >= 5.0) std::printf("    (detect_entities_linear: parsing took %.2f s)\n", parse_secs);

    // find() on a 4 MiB settings.xml of '&' (the verifier's 66 s case)
    TempDir t;
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "settings.xml", "<content><mlc_path>" + std::string((std::size_t(4) << 20) - 64, '&') +
                                          "</mlc_path></content>");
    {
        Log log;
        t0 = std::chrono::steady_clock::now();
        HDF_CHECK(detect::find(env_for(t.path / "AppData", log), {}).empty());
        HDF_CHECK(secs_since(t0) < 2.0);
        HDF_CHECK(log.has("mlc_path is not a usable path"));  // 64 KiB of '&' is longer than any Windows path
    }
    // a stop while a 16 MiB title list of '&' is read and parsed is honoured at once
    write_file(cemu / "title_list_cache.xml",
               "<title_list><title titleId=\"000500001019E500\" version=\"81\"><format>1</format><path>" +
                   std::string((std::size_t(16) << 20) - 200, '&') + "</path></title></title_list>");
    {
        Log log;
        std::stop_source stop;
        std::vector<Candidate> got(1);
        t0 = std::chrono::steady_clock::now();
        std::thread worker([&] { got = detect::find(env_for(t.path / "AppData", log), stop.get_token()); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.request_stop();
        worker.join();
        const double join_secs = secs_since(t0);
        HDF_CHECK(join_secs < 2.0);
        HDF_CHECK(got.empty());
        if (join_secs >= 2.0) std::printf("    (detect_entities_linear: the join took %.2f s)\n", join_secs);
    }
}

// ---------------------------------------------------------------------------------------------- file headers
HDF_TEST(detect_disc_and_tmd_headers) {
    TempDir t;
    write_bytes(t.path / "tp.wux", wux_bytes("WUP-P-AZAE-02-55"));
    HDF_CHECK_EQ(di::disc_product_code(t.path / "tp.wux").value_or("none"), std::string("WUP-P-AZAE"));
    write_file(t.path / "tp.wud", "WUP-P-AZAJ-00-00" + std::string(48, '\0'));
    HDF_CHECK_EQ(di::disc_product_code(t.path / "tp.wud").value_or("none"), std::string("WUP-P-AZAJ"));
    write_file(t.path / "junk.wux", std::string(100, 'x'));
    HDF_CHECK(!di::disc_product_code(t.path / "junk.wux"));
    write_file(t.path / "short.wud", "WUP-P");
    HDF_CHECK(!di::disc_product_code(t.path / "short.wud"));
    Bytes cut = wux_bytes("WUP-P-AZAE-02-55");
    cut.resize(0x8004);  // the sector the index points at is cut off
    write_bytes(t.path / "cut.wux", cut);
    HDF_CHECK(!di::disc_product_code(t.path / "cut.wux"));
    Bytes huge = wux_bytes("WUP-P-AZAE-02-55");
    huge[0x10 + 7] = 0x7F;  // a disc size whose index table can't fit in the file
    write_bytes(t.path / "huge.wux", huge);
    HDF_CHECK(!di::disc_product_code(t.path / "huge.wux"));
    HDF_CHECK(!di::disc_product_code(t.path / "missing.wux"));
    write_bytes(t.path / "title.tmd", tmd_bytes(0x0005000E1019E500ull));
    HDF_CHECK_EQ(di::tmd_title_id(t.path / "title.tmd").value_or("none"), std::string("0005000E1019E500"));
    write_file(t.path / "bad.tmd", std::string(0x300, '\x01'));
    HDF_CHECK(!di::tmd_title_id(t.path / "bad.tmd"));
}

HDF_TEST(detect_drive_classes) {
#ifdef _WIN32
    using di::DriveClass;
    HDF_CHECK(di::drive_class(fs::path(L"\\\\hdf-test.invalid\\share\\x.wua")) == DriveClass::Remote);
    HDF_CHECK(di::drive_class(fs::path(L"//hdf-test.invalid/share")) == DriveClass::Remote);
    HDF_CHECK(di::drive_class(fs::path(L"\\\\?\\UNC\\hdf-test.invalid\\share")) == DriveClass::Remote);
    HDF_CHECK(di::drive_class(fs::path(L"relative\\x")) == DriveClass::Unknown);
    HDF_CHECK(di::drive_class(fs::path(L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\")) == DriveClass::Unknown);
    TempDir t;
    // the temp folder is on a fixed drive on any normal setup (VERIFIED here: C: is fixed)
    HDF_CHECK(di::drive_class(t.path) == DriveClass::Local);
    HDF_CHECK(di::drive_class(fs::path(L"\\\\?\\") += t.path.native()) == DriveClass::Local);
    // fix pass L3: the drive or share of a path, from the text alone
    HDF_CHECK_EQ(di::volume_key(fs::path(L"C:\\x\\y")), std::string("c:"));
    HDF_CHECK_EQ(di::volume_key(fs::path(L"\\\\?\\C:\\x")), std::string("c:"));
    HDF_CHECK_EQ(di::volume_key(fs::path(L"\\\\Server\\Share\\x")), std::string("\\\\server\\share"));
    HDF_CHECK_EQ(di::volume_key(fs::path(L"//server/share")), std::string("\\\\server\\share"));
    HDF_CHECK_EQ(di::volume_key(fs::path(L"\\\\?\\UNC\\server\\share\\x")), std::string("\\\\server\\share"));
    HDF_CHECK(di::volume_key(fs::path(L"relative\\x")).empty());
    HDF_CHECK(di::volume_key(fs::path(L"\\\\server")).empty());
    HDF_CHECK(di::volume_key(fs::path(L"\\\\server\\")).empty());
    HDF_CHECK(di::volume_key(fs::path(L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\x")).empty());
#endif
}

// ---------------------------------------------------------------------------------------------- check_sizes
HDF_TEST(detect_check_sizes_codes) {
    SizeSource s;
    HDF_CHECK_EQ(code_of(detect::check_sizes(s)), std::string("ok"));
    for (const std::string& p : first_inputs(12)) s.missing.insert(p);
    HDF_CHECK_EQ(code_of(detect::check_sizes(s)), std::string("hd.missing"));
    HDF_CHECK_EQ(detail_of(detect::check_sizes(s)), std::string("12 of 136"));
    SizeSource v;
    const auto f = first_inputs(5);
    v.wrong = {f[0], f[1], f[2]};
    HDF_CHECK_EQ(code_of(detect::check_sizes(v)), std::string("hd.version"));
    HDF_CHECK_EQ(detail_of(detect::check_sizes(v)), std::string("3 of 136 differ"));
    v.missing = {f[3], f[4]};
    HDF_CHECK_EQ(detail_of(detect::check_sizes(v)), std::string("3 of 136 differ, 2 missing"));
}

// ---------------------------------------------------------------------------------------------- find()
// %APPDATA%\Cemu\title_list_cache.xml with odd characters and entity-encoded paths; order folder, .wua, .wux.
HDF_TEST(detect_find_title_list_and_order) {
    TempDir t;
    const fs::path games = t.path / P("Games & Co (Wii U)");
    const fs::path wua = games / P("Zelda " + kE) / "TP HD.wua";
    std::vector<WuaFile> wf;
    add_wua_title(wf, kUsaTop, Content::Full, meta_xml("000500001019E500", "WUP-P-AZAE", 81));
    make_wua(wua, wf);
    write_file(games / "TP.wux", "encrypted");
    const fs::path folder = t.path / P("Spiele " + kU + " " + kJa) / "TP HD [x]";
    make_title(folder, "000500001019E500", "WUP-P-AZAE", 81, Content::Full);
    std::vector<WuaFile> other;
    other.push_back({"0005000010143500_v0/content/x.bin", 5, {}});
    make_wua(games / "WW HD.wua", other);
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "title_list_cache.xml",
               title_list_xml({{"000500001019e500", 81, 3, wua, kUsaTop},
                               {"000500001019e500", 81, 2, games / "TP.wux", {}},
                               {"0005000010143500", 0, 3, games / "WW HD.wua", "0005000010143500_v0"},
                               {"000500001019e500", 81, 1, folder, {}},
                               {"0005000c1019e500", 0, 1, t.path / "dlc", {}},
                               {"000500001019e500", 81, 1, t.path / "gone", {}}}));
    make_title(t.path / "dlc", "0005000C1019E500", "WUP-P-AZAE", 0, Content::LosOnly);  // a DLC id: never listed
    write_file(cemu / "settings.xml", settings_xml(t.path / "no-mlc", {}));
    Log log;
    const auto found = detect::find(env_for(t.path / "AppData", log), {});
    HDF_CHECK_EQ(summary(found, t.path),
                 "folder|title_list_cache|Spiele " + kU + " " + kJa + "/TP HD [x]|||\n"
                 "wua|title_list_cache|Games & Co (Wii U)/Zelda " + kE + "/TP HD.wua|000500001019e500_v81||\n"
                 "encrypted|title_list_cache|Games & Co (Wii U)/TP.wux|||encrypted .wux\n");
    HDF_CHECK(log.has("gone"));
    if (found.size() == 3) {
        HDF_CHECK_EQ(found[1].version, 81u);
        HDF_CHECK_EQ(found[1].title_id, std::string("000500001019E500"));
        HDF_CHECK_EQ(found[0].title_id, std::string("000500001019E500"));
        auto src = detect::open(found[1]);
        HDF_CHECK(src.has_value());
        if (src) HDF_CHECK_EQ(code_of(detect::check_sizes(**src)), std::string("ok"));
        HDF_CHECK_EQ(code_of(detect::open(found[2])), std::string("hd.encrypted"));
    }
}

// Correction #6: mlc base + update; a base folder or .wua outside the mlc + the mlc update; an update that
// makes things worse is dropped.
HDF_TEST(detect_find_mlc_base_update) {
    TempDir t;
    const fs::path mlc = t.path / P("mlc & " + kU) / "mlc01";
    const fs::path title = mlc / "usr" / "title";
    make_title(title / "00050000" / "1019e500", "000500001019E500", "WUP-P-AZAE", 0, Content::Full,
               {"res/layout/saveres.arc"}, {"los.bin"});
    write_file(title / "0005000e" / "1019e500" / "meta" / "meta.xml", meta_xml("0005000E1019E500", "WUP-P-AZAE", 81));
    make_sized(title / "0005000e" / "1019e500" / "content" / "los.bin", inputs::find_hd("los.bin")->size);
    make_sized(title / "0005000e" / "1019e500" / "content" / "res" / "layout" / "saveres.arc",
               inputs::find_hd("res/layout/saveres.arc")->size);
    // a base folder outside the mlc (title list format 1) and a base-only .wua, both missing saveres.arc
    const fs::path outside = t.path / "games" / "TP base";
    make_title(outside, "000500001019E500", "WUP-P-AZAE", 0, Content::Full, {"res/layout/saveres.arc"});
    std::vector<WuaFile> wf;
    add_wua_title(wf, "000500001019e500_v0", Content::Full, {}, {"res/layout/saveres.arc"});
    make_wua(t.path / "games" / "TP base.wua", wf);
    // EUR: a complete base, and an mlc update with a wrong-size los.bin
    const fs::path eur = t.path / "games" / "TP EUR";
    make_title(eur, "000500001019E600", "WUP-P-AZAP", 0, Content::Full);
    write_file(title / "0005000e" / "1019e600" / "meta" / "meta.xml", meta_xml("0005000E1019E600", "WUP-P-AZAP", 81));
    make_sized(title / "0005000e" / "1019e600" / "content" / "los.bin", inputs::find_hd("los.bin")->size + 7);
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "settings.xml", settings_xml(mlc, {}));
    write_file(cemu / "title_list_cache.xml",
               title_list_xml({{"000500001019e500", 0, 1, outside, {}},
                               {"000500001019e500", 0, 3, t.path / "games" / "TP base.wua", "000500001019e500_v0"},
                               {"000500001019e600", 0, 1, eur, {}}}));
    Log log;
    const auto found = detect::find(env_for(t.path / "AppData", log), {});
    const std::string upd = U(fs::path(P("mlc & " + kU)) / "mlc01/usr/title/0005000e/1019e500");
    HDF_CHECK_EQ(summary(found, t.path),
                 "folder|title_list_cache|games/TP base||" + upd + "|\n"
                 "folder|title_list_cache|games/TP EUR|||\n"
                 "mlc|mlc|" + U(fs::path(P("mlc & " + kU)) / "mlc01/usr/title/00050000/1019e500") + "||" + upd + "|\n"
                 "wua|title_list_cache|games/TP base.wua|000500001019e500_v0|" + upd + "|\n");
    HDF_CHECK(log.has("not used"));
    for (const Candidate& c : found) {
        if (c.title_id == "000500001019E500") HDF_CHECK_EQ(c.version, 81u);
        if (c.title_id == "000500001019E600") HDF_CHECK_EQ(c.version, 0u);
    }
    if (found.size() == 4) {
        auto src = detect::open(found[2]);
        HDF_CHECK(src.has_value());
        if (src) HDF_CHECK_EQ((*src)->size("LOS.BIN").value_or(0), inputs::find_hd("los.bin")->size);  // update wins
    }
}

// Correction #6(b): a .wua with _v0 and _v81 uses its own update (update first), never the mlc's.
HDF_TEST(detect_find_wua_own_update) {
    TempDir t;
    std::vector<WuaFile> wf;
    add_wua_title(wf, "000500001019e500_v0", Content::Full, meta_xml("000500001019E500", "WUP-P-AZAE", 0), {}, {"los.bin"});
    add_wua_title(wf, "0005000e1019e500_v81", Content::LosOnly, meta_xml("0005000E1019E500", "WUP-P-AZAE", 81));
    make_wua(t.path / "TP.wua", wf);
    const fs::path mlc = t.path / "mlc01";
    write_file(mlc / "usr/title/0005000e/1019e500/meta/meta.xml", meta_xml("0005000E1019E500", "WUP-P-AZAE", 81));
    make_sized(mlc / "usr/title/0005000e/1019e500/content/los.bin", 3);
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "settings.xml", settings_xml(mlc, {t.path}));
    Log log;
    const auto found = detect::find(env_for(t.path / "AppData", log), {});
    HDF_CHECK_EQ(summary(found, t.path), std::string("wua|game_paths|TP.wua|000500001019e500_v0||\n"));
    if (found.size() == 1) {
        HDF_CHECK_EQ(found[0].version, 81u);
        auto src = detect::open(found[0]);
        HDF_CHECK(src.has_value());
        if (src) HDF_CHECK_EQ((*src)->size("los.bin").value_or(0), inputs::find_hd("los.bin")->size);
    }
}

// A portable Cemu, picked: <Cemu>/portable/{settings,title_list_cache}.xml, <Cemu>/mlc01, and its GamePaths.
HDF_TEST(detect_find_portable_cemu) {
    TempDir t;
    const fs::path cemu = t.path / P("Cemu 2.x (portable) " + kE);
    write_file(cemu / "Cemu.exe", "MZ");
    make_title(cemu / "portable/mlc01/usr/title/00050000/1019e600", "000500001019E600", "WUP-P-AZAP", 0, Content::Full);
    std::vector<WuaFile> wf;
    add_wua_title(wf, kUsaTop, Content::Full);
    const fs::path wua = t.path / "wua" / "TP & co.wua";
    make_wua(wua, wf);
    const fs::path games = t.path / "Wii U";
    make_title(games / "TP [AZAE]", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    write_file(cemu / "portable/settings.xml", settings_xml(std::nullopt, {games}));
    write_file(cemu / "portable/title_list_cache.xml", title_list_xml({{"000500001019e500", 81, 3, wua, kUsaTop}}));
    // a usable copy in %APPDATA% that a pick must ignore
    write_file(t.path / "AppData/Cemu/title_list_cache.xml", title_list_xml({{"000500001019e500", 81, 3, wua, kUsaTop}}));
    for (const fs::path& pick : {cemu, cemu / "Cemu.exe", cemu / "portable" / "settings.xml"}) {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = pick;
        const auto found = detect::find(env, {});
        HDF_CHECK_EQ(summary(found, t.path),
                     "mlc|picked|" + U(fs::path(P("Cemu 2.x (portable) " + kE)) / "portable/mlc01/usr/title/00050000/1019e600") + "|||\n"
                     "wua|picked|wua/TP & co.wua|000500001019e500_v81||\n"
                     "folder|picked|Wii U/TP [AZAE]|||missing 135 of 136\n");
    }
    // legacy portable: settings.xml next to Cemu.exe
    const fs::path old = t.path / "Cemu 1.27";
    write_file(old / "Cemu.exe", "MZ");
    write_file(old / "settings.xml", settings_xml(std::nullopt, {games}));
    Log log;
    detect::Env env = env_for(t.path / "AppData", log);
    env.picked = old;
    HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), std::string("folder|picked|Wii U/TP [AZAE]|||missing 135 of 136\n"));
}

// GamePaths scan: title folders by meta.xml, other games not entered, .wux by product code, NUS by title.tmd,
// depth 4, links and offline files skipped.
HDF_TEST(detect_find_game_paths_scan) {
    TempDir t;
    const fs::path root = t.path / P("Wii U Games " + kJa);
    // the user's [AZAP01] shape: an update id in meta.xml, a full merged copy
    make_title(root / "TP HD merged [AZAP01]", "0005000E1019C800", "WUP-P-AZAP", 0,
               Content::Full);
    make_title(root / "BotW", "00050000101C9400", "WUP-P-ALZE", 208, Content::None);
    make_title(root / "TP DLC", "0005000C1019E500", "WUP-P-AZAE", 0, Content::LosOnly);  // fix pass I2: never listed
    make_title(root / "BotW" / "content" / "TP inside", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    write_bytes(root / "tp.wux", wux_bytes("WUP-P-AZAE-02-55"));
    write_bytes(root / "ww.wux", wux_bytes("WUP-P-BCZE-00-31"));
    write_file(root / "junk.wux", "not a disc");
    write_bytes(root / "nus" / "tp" / "title.tmd", tmd_bytes(0x0005000E1019E500ull));
    write_file(root / "nus" / "tp" / "00000000.app", "x");
    write_bytes(root / "nus" / "ww" / "title.tmd", tmd_bytes(0x0005000010143500ull));
    make_title(root / "a/b/c/TP deep", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    make_title(root / "a/b/c/d/TP too deep", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    std::vector<WuaFile> other;
    other.push_back({"0005000010143500_v0/content/x.bin", 5, {}});
    make_wua(root / "WW HD.wua", other);
    write_file(root / "broken.wua", "not a wua");
    // outside the root, reached only through a junction or an offline file
    make_title(t.path / "elsewhere" / "TP linked", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    bool junction = false, offline = false;
#ifdef _WIN32
    {
        junction = make_junction(root / "link", L"\\??\\" + (t.path / "elsewhere").native());
        std::vector<WuaFile> wf;
        add_wua_title(wf, kUsaTop, Content::Full);
        make_wua(root / "offline.wua", wf);
        offline = SetFileAttributesW((root / "offline.wua").c_str(), FILE_ATTRIBUTE_OFFLINE) != 0 &&
                  (GetFileAttributesW((root / "offline.wua").c_str()) & FILE_ATTRIBUTE_OFFLINE) != 0;
    }
#endif
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "settings.xml", settings_xml(std::nullopt, {root}));
    Log log;
    const auto found = detect::find(env_for(t.path / "AppData", log), {});
    const std::string r = U(fs::path(P("Wii U Games " + kJa)));
    HDF_CHECK_EQ(summary(found, t.path),
                 "folder|game_paths|" + r + "/TP HD merged [AZAP01]|||\n"
                 "folder|game_paths|" + r + "/a/b/c/TP deep|||missing 135 of 136\n"
                 "encrypted|game_paths|" + r + "/tp.wux|||encrypted .wux\n"
                 "encrypted|game_paths|" + r + "/nus/tp|||encrypted NUS\n");
    if (!found.empty()) HDF_CHECK_EQ(found[0].title_id, std::string("0005000E1019C800"));
    if (found.size() >= 3) HDF_CHECK_EQ(found[2].title_id, std::string("000500001019E500"));
    HDF_CHECK(log.has("junk.wux"));
    HDF_CHECK(log.has("broken.wua"));
    if (junction || offline) HDF_CHECK(log.has("link(s) or online-only item(s)"));
    if (!junction) std::printf("    (detect_find_game_paths_scan: no junction made, that check was skipped)\n");
    if (offline) {
        // a pick may use an offline file (the user chose it); the junction is still not followed
        Log plog;
        detect::Env env = env_for(t.path / "AppData", plog);
        env.picked = root / "offline.wua";
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), "wua|picked|" + r + "/offline.wua|" + kUsaTop + "||\n");
    } else {
        std::printf("    (detect_find_game_paths_scan: the offline attribute could not be set, that check was skipped)\n");
    }
}

// Plan 3.3 D picked-folder cases: title, content, folder with the .wua, parent; plus a file, an empty folder,
// a folder with only a dummy x.wux, and the remembered location first.
HDF_TEST(detect_find_picked_cases) {
    TempDir t;
    const fs::path base = t.path / "p";
    const fs::path wiiu = base / "Games" / P("Wii U (" + kE + ")");
    const fs::path title = wiiu / "TP [AZAE]";
    make_title(title, "000500001019E500", "WUP-P-AZAE", 81, Content::Full);
    std::vector<WuaFile> wf;
    add_wua_title(wf, kUsaTop, Content::Full);
    make_wua(wiiu / "TP (US).wua", wf);
    write_file(wiiu / "TP.wux", "dummy");
    fs::create_directories(t.path / "empty");
    write_file(t.path / "only wux" / "x.wux", "dummy");
    const std::string w = "p/Games/" + U(fs::path(P("Wii U (" + kE + ")")));
    const std::string three = "folder|picked|" + w + "/TP [AZAE]|||\n"
                              "wua|picked|" + w + "/TP (US).wua|" + kUsaTop + "||\n"
                              "encrypted|picked|" + w + "/TP.wux|||encrypted .wux\n";
    struct Case {
        fs::path pick;
        std::string expect;
    };
    const Case cases[] = {
        {title, "folder|picked|" + w + "/TP [AZAE]|||\n"},
        {title / "content", "folder|picked|" + w + "/TP [AZAE]/content|||\n"},
        {wiiu, three},
        {base / "Games", three},
        {base, three},  // three levels above the title folder
        {wiiu / "TP (US).wua", "wua|picked|" + w + "/TP (US).wua|" + kUsaTop + "||\n"},
        {wiiu / "TP.wux", "encrypted|picked|" + w + "/TP.wux|||encrypted .wux\n"},
        {t.path / "empty", ""},
        {t.path / "only wux", "encrypted|picked|only wux/x.wux|||encrypted .wux\n"},
        {t.path / "missing", ""},
    };
    // %APPDATA% lists another usable copy: a pick searches only the pick
    std::vector<WuaFile> other;
    add_wua_title(other, kUsaTop, Content::Full);
    make_wua(t.path / "elsewhere" / "auto.wua", other);
    write_file(t.path / "AppData/Cemu/title_list_cache.xml",
               title_list_xml({{"000500001019e500", 81, 3, t.path / "elsewhere" / "auto.wua", kUsaTop}}));
    for (const Case& c : cases) {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = c.pick;
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), c.expect);
    }
    // four levels above the title folder: only the .wua and the .wux (depth 3), not the title folder (depth 4)
    {
        const fs::path deep = t.path / "deep";
        make_title(deep / "a/b/c/TP", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = deep;
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), std::string());
        env.picked = deep / "a";
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), std::string("folder|picked|deep/a/b/c/TP|||missing 135 of 136\n"));
    }
    // remembered (no pick): searched first and listed first among the usable ones; auto-detect still runs
    Log log;
    detect::Env env = env_for(t.path / "AppData", log);
    env.remembered = wiiu / "TP (US).wua";
    HDF_CHECK_EQ(summary(detect::find(env, {}), t.path),
                 "wua|remembered|" + w + "/TP (US).wua|" + kUsaTop + "||\n"
                 "wua|title_list_cache|elsewhere/auto.wua|" + kUsaTop + "||\n");
}

// Correction #5: entry and time budgets, the stop token, remote paths never touched by auto-detect.
HDF_TEST(detect_budgets_stop_and_remote) {
    TempDir t;
    const fs::path root = t.path / "many";
    for (int i = 0; i < 300; ++i) fs::create_directories(root / ("d" + std::to_string(1000 + i)));
    make_title(root / "zzz TP", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "settings.xml", settings_xml(std::nullopt, {root}));
    {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.entry_budget = 100;
        HDF_CHECK(detect::find(env, {}).empty());
        HDF_CHECK(log.has("entry budget"));
        env.entry_budget = 20000;
        HDF_CHECK_EQ(detect::find(env, {}).size(), 1u);
        env.time_budget = std::chrono::milliseconds(-1);
        Log tlog;
        env.log = [&tlog](std::string_view l) { tlog.lines.emplace_back(l); };
        HDF_CHECK(detect::find(env, {}).empty());
        HDF_CHECK(tlog.has("time budget"));
    }
    {
        std::stop_source s;
        s.request_stop();
        Log log;
        HDF_CHECK(detect::find(env_for(t.path / "AppData", log), s.get_token()).empty());
    }
#ifdef _WIN32
    {
        // UNC paths in the title list and GamePaths: skipped from the text alone (an unreachable server would
        // block a stat for many seconds)
        write_file(cemu / "title_list_cache.xml",
                   title_list_xml({{"000500001019e500", 81, 3, fs::path(L"\\\\hdf-test.invalid\\share\\TP.wua"), kUsaTop},
                                   {"000500001019e500", 81, 1, fs::path(L"//hdf-test.invalid/share/TP"), {}}}));
        write_file(cemu / "settings.xml",
                   settings_xml(fs::path(L"\\\\hdf-test.invalid\\mlc01"), {fs::path(L"\\\\hdf-test.invalid\\games")}));
        Log log;
        const auto t0 = std::chrono::steady_clock::now();
        HDF_CHECK(detect::find(env_for(t.path / "AppData", log), {}).empty());
        HDF_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        HDF_CHECK(log.has("network drive"));
    }
#endif
}

HDF_TEST(detect_open_errors) {
    TempDir t;
    Candidate e;
    e.kind = Candidate::Encrypted;
    e.problem = "encrypted .wux";
    HDF_CHECK_EQ(code_of(detect::open(e)), std::string("hd.encrypted"));
    Candidate f;
    f.kind = Candidate::Folder;
    f.path = t.path / "missing";
    HDF_CHECK_EQ(code_of(detect::open(f)), std::string("hd.not_found"));
    f.path = t.path;
    f.update = t.path / "missing";
    HDF_CHECK_EQ(code_of(detect::open(f)), std::string("hd.not_found"));
    write_file(t.path / "junk.wua", "junk");
    Candidate w;
    w.kind = Candidate::Wua;
    w.path = t.path / "junk.wua";
    HDF_CHECK_EQ(code_of(detect::open(w)), std::string("wua.bad"));
    std::vector<WuaFile> other;
    other.push_back({"0005000010143500_v0/content/x.bin", 5, {}});
    make_wua(t.path / "ww.wua", other);
    w.path = t.path / "ww.wua";
    HDF_CHECK_EQ(code_of(detect::open(w)), std::string("hd.not_tphd"));
    // a picked damaged .wua is listed with its problem, never as usable
    Log log;
    detect::Env env = env_for(t.path / "AppData", log);
    env.picked = t.path / "junk.wua";
    const auto found = detect::find(env, {});
    HDF_CHECK_EQ(found.size(), 1u);
    if (!found.empty()) HDF_CHECK(found[0].problem.starts_with("damaged .wua: wua.bad"));
    HDF_CHECK_EQ(std::string(detect::kind_name(Candidate::Mlc)), std::string("mlc"));
}

// fix-wave1 L1: names that are not valid UTF-16 never make a scan throw; such folders are skipped.
HDF_TEST(detect_names_not_utf16) {
#ifdef _WIN32
    TempDir t;
    std::wstring bad = L"x";
    bad.push_back(wchar_t(0xD800));
    const fs::path root = t.path / "games";
    make_title(root / fs::path(bad + L" TP"), "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    make_title(root / fs::path(bad + L" dir") / "TP", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    make_title(root / "TP ok", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    std::error_code ec;
    if (!fs::exists(root / fs::path(bad + L" TP"), ec)) HDF_SKIP("this file system refuses such names");
    Log log;
    detect::Env env = env_for(t.path / "AppData", log);
    env.picked = root;
    HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), std::string("folder|picked|games/TP ok|||missing 135 of 136\n"));
#endif
}

// ---------------------------------------------------------------------------------------------- fix pass
// L1: a stray *.app no longer hides a folder's .wua/.wux files and sub-folders; NUS files alone are still NUS.
HDF_TEST(detect_nus_needs_nus_files_alone) {
    TempDir t;
    const fs::path pick = t.path / "pick";
    write_bytes(pick / "x.wux", wux_bytes("WUP-P-AZAE-02-55"));
    write_bytes(pick / "sub" / "y.wux", wux_bytes("WUP-P-AZAE-02-55"));
    write_file(pick / "readme.app", "not a NUS file");
    std::vector<WuaFile> wf;
    add_wua_title(wf, kUsaTop, Content::Full);
    make_wua(pick / "TP.wua", wf);
    for (const char* n : {"00000000.app", "00000000.h3", "title.tik", "title.cert"}) write_file(pick / "nus" / n, "x");
    Log log;
    detect::Env env = env_for(t.path / "AppData", log);
    env.picked = pick;
    HDF_CHECK_EQ(summary(detect::find(env, {}), t.path),
                 "wua|picked|pick/TP.wua|" + kUsaTop + "||\n"
                 "encrypted|picked|pick/x.wux|||encrypted .wux\n"
                 "encrypted|picked|pick/nus|||encrypted NUS\n"
                 "encrypted|picked|pick/sub/y.wux|||encrypted .wux\n");
}

// L5: the TP HD update that Cemu lists next to a .wux base is "update only" and comes after the .wux.
HDF_TEST(detect_update_only_last) {
    TempDir t;
    const fs::path mlc = t.path / "mlc01";
    const fs::path upd = mlc / "usr" / "title" / "0005000e" / "1019e500";
    write_file(upd / "meta" / "meta.xml", meta_xml("0005000E1019E500", "WUP-P-AZAE", 81));
    make_sized(upd / "content" / "los.bin", inputs::find_hd("los.bin")->size);
    const fs::path wux = t.path / "games" / "TP.wux";
    write_bytes(wux, wux_bytes("WUP-P-AZAE-02-55"));
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "settings.xml", settings_xml(mlc, {}));
    write_file(cemu / "title_list_cache.xml",
               title_list_xml({{"0005000e1019e500", 81, 1, upd, {}}, {"000500001019e500", 81, 2, wux, {}}}));
    const std::string u = "mlc01/usr/title/0005000e/1019e500";
    {
        Log log;
        HDF_CHECK_EQ(summary(detect::find(env_for(t.path / "AppData", log), {}), t.path),
                     "encrypted|title_list_cache|games/TP.wux|||encrypted .wux\n"
                     "folder|title_list_cache|" + u + "|||update only: missing 135 of 136\n");
    }
    // with a base .wua the update is layered over it (usable); the update alone is still listed last
    std::vector<WuaFile> wf;
    add_wua_title(wf, "000500001019e500_v0", Content::Full, {}, {}, {"los.bin"});
    make_wua(t.path / "games" / "TP base.wua", wf);
    write_file(cemu / "title_list_cache.xml",
               title_list_xml({{"0005000e1019e500", 81, 1, upd, {}},
                               {"000500001019e500", 0, 3, t.path / "games" / "TP base.wua", "000500001019e500_v0"},
                               {"000500001019e500", 81, 2, wux, {}}}));
    Log log;
    HDF_CHECK_EQ(summary(detect::find(env_for(t.path / "AppData", log), {}), t.path),
                 "wua|title_list_cache|games/TP base.wua|000500001019e500_v0|" + u + "|\n"
                 "encrypted|title_list_cache|games/TP.wux|||encrypted .wux\n"
                 "folder|title_list_cache|" + u + "|||update only: missing 135 of 136\n");
}

// Wave 4, F1: a merged copy whose meta.xml carries the update id (as this PC's [AZAP01] folder) with one file
// missing is "missing 1 of 136" and comes before an encrypted .wux. With most files missing it is "update only".
// An update installed in Cemu's mlc is "update only" even when it holds most of the files.
HDF_TEST(detect_update_id_copy_missing_files) {
    TempDir t;
    const fs::path games = t.path / "games";
    write_bytes(games / "TP.wux", wux_bytes("WUP-P-AZAE-02-55"));
    const fs::path merged = games / "TP HD merged [AZAP01]";
    make_title(merged, "0005000E1019C800", "WUP-P-AZAP", 0, Content::Full, {"res/object/filesel.arc"});
    {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = games;
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path),
                     "folder|picked|games/TP HD merged [AZAP01]|||missing 1 of 136\n"
                     "encrypted|picked|games/TP.wux|||encrypted .wux\n");
    }
    // the same copy with one file of another size and one missing: another version, still not "update only"
    make_sized(merged / "content" / "res" / "object" / "filesel.arc", inputs::find_hd("res/object/filesel.arc")->size);
    make_sized(merged / "content" / "los.bin", inputs::find_hd("los.bin")->size + 1);
    fs::remove(merged / "content" / "res" / "msgus" / "bmgres.arc");
    {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = games;
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path),
                     "folder|picked|games/TP HD merged [AZAP01]|||different version: 1 of 136 differ, 1 missing\n"
                     "encrypted|picked|games/TP.wux|||encrypted .wux\n");
    }
    // most files missing (68 of 136 is not "most"; 69 is): the update alone, ranked after the .wux
    std::set<std::string> half, most;
    for (const inputs::InputFile& f : inputs::hd_files()) {
        if (half.size() < 68) half.insert(std::string(f.path));
        if (most.size() < 69) most.insert(std::string(f.path));
    }
    const fs::path upd2 = games / "TP HD update";
    make_title(upd2, "0005000E1019E500", "WUP-P-AZAE", 81, Content::Full, half);
    fs::remove_all(merged);
    {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = games;
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path),
                     "folder|picked|games/TP HD update|||missing 68 of 136\n"
                     "encrypted|picked|games/TP.wux|||encrypted .wux\n");
    }
    fs::remove_all(upd2);
    make_title(upd2, "0005000E1019E500", "WUP-P-AZAE", 81, Content::Full, most);
    {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = games;
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path),
                     "encrypted|picked|games/TP.wux|||encrypted .wux\n"
                     "folder|picked|games/TP HD update|||update only: missing 69 of 136\n");
    }
    // a base-id copy with most files missing is never "update only"
    fs::remove_all(upd2);
    make_title(upd2, "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    {
        Log log;
        detect::Env env = env_for(t.path / "AppData", log);
        env.picked = games;
        HDF_CHECK_EQ(summary(detect::find(env, {}), t.path),
                     "folder|picked|games/TP HD update|||missing 135 of 136\n"
                     "encrypted|picked|games/TP.wux|||encrypted .wux\n");
    }
    // Cemu's installed update with all but one file: still the update alone (its folder says so)
    const fs::path mlc = t.path / "mlc01";
    const fs::path upd = mlc / "usr" / "title" / "0005000e" / "1019e500";
    make_title(upd, "0005000E1019E500", "WUP-P-AZAE", 81, Content::Full, {"los.bin"});
    const fs::path cemu = t.path / "AppData" / "Cemu";
    write_file(cemu / "settings.xml", settings_xml(mlc, {}));
    write_file(cemu / "title_list_cache.xml",
               title_list_xml({{"0005000e1019e500", 81, 1, upd, {}}, {"000500001019e500", 81, 2, games / "TP.wux", {}}}));
    {
        Log log;
        HDF_CHECK_EQ(summary(detect::find(env_for(t.path / "AppData", log), {}), t.path),
                     "encrypted|title_list_cache|games/TP.wux|||encrypted .wux\n"
                     "folder|title_list_cache|mlc01/usr/title/0005000e/1019e500|||update only: missing 1 of 136\n");
    }
}

// L2: an update found elsewhere is not layered over a base that is already at that version (a merged _v81 .wua).
HDF_TEST(detect_no_update_over_merged_base) {
    TempDir t;
    const fs::path mlc = t.path / "mlc01";
    const fs::path upd = mlc / "usr" / "title" / "0005000e" / "1019e500";
    write_file(upd / "meta" / "meta.xml", meta_xml("0005000E1019E500", "WUP-P-AZAE", 81));
    make_sized(upd / "content" / "los.bin", inputs::find_hd("los.bin")->size);
    std::vector<WuaFile> base, merged;
    add_wua_title(base, "000500001019e500_v0", Content::Full);
    make_wua(t.path / "games" / "TP base.wua", base);
    add_wua_title(merged, kUsaTop, Content::Full, meta_xml("000500001019E500", "WUP-P-AZAE", 81));
    make_wua(t.path / "games" / "TP merged.wua", merged);
    write_file(t.path / "AppData" / "Cemu" / "settings.xml", settings_xml(mlc, {t.path / "games"}));
    Log log;
    const auto found = detect::find(env_for(t.path / "AppData", log), {});
    HDF_CHECK_EQ(summary(found, t.path),
                 "wua|game_paths|games/TP base.wua|000500001019e500_v0|mlc01/usr/title/0005000e/1019e500|\n"
                 "wua|game_paths|games/TP merged.wua|" + kUsaTop + "||\n");
    HDF_CHECK(log.has("not layered"));
    for (const Candidate& c : found) HDF_CHECK_EQ(c.version, 81u);
}

// L3: the paths in a picked Cemu's files get the auto-detect rules (network paths skipped from the text alone) but
// keep the "picked" origin; the picked drive itself is still searched.
HDF_TEST(detect_picked_cemu_files_auto_rules) {
#ifdef _WIN32
    TempDir t;
    const fs::path cemu = t.path / "Cemu";
    write_file(cemu / "Cemu.exe", "MZ");
    const fs::path games = t.path / "games";
    make_title(games / "TP", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    write_file(cemu / "settings.xml", settings_xml(fs::path(L"\\\\hdf-test.invalid\\mlc01"),
                                                   {fs::path(L"\\\\hdf-test.invalid\\games"), games}));
    write_file(cemu / "title_list_cache.xml",
               title_list_xml({{"000500001019e500", 81, 3, fs::path(L"\\\\hdf-test.invalid\\share\\TP.wua"), kUsaTop}}));
    Log log;
    detect::Env env = env_for(t.path / "AppData", log);
    env.picked = cemu;
    const auto t0 = std::chrono::steady_clock::now();
    HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), std::string("folder|picked|games/TP|||missing 135 of 136\n"));
    HDF_CHECK(secs_since(t0) < 2.0);
    HDF_CHECK(log.has("hdf-test.invalid\\share\\TP.wua (network drive)") || log.has("hdf-test.invalid/share/TP.wua (network drive)"));
    HDF_CHECK(log.has("mlc01 (network drive)"));
    HDF_CHECK(log.has("games (network drive)"));
#endif
}

// L4: in auto-detect a root that is itself a link is used only when its target is on a local fixed drive; a
// picked link is the player's choice.
HDF_TEST(detect_root_links) {
#ifdef _WIN32
    TempDir t;
    make_title(t.path / "real" / "TP", "000500001019E500", "WUP-P-AZAE", 81, Content::LosOnly);
    const bool local = make_junction(t.path / "jlocal", L"\\??\\" + (t.path / "real").native());
    const bool vol = make_junction(t.path / "jvol", L"\\??\\Volume{00000000-0000-0000-0000-000000000000}\\");
    if (!local) HDF_SKIP("this file system refuses junctions");
    const di::LinkInfo li = di::link_info(t.path / "jlocal");
    HDF_CHECK(li.link && li.target.has_value());
    if (li.target) {
        HDF_CHECK(di::drive_class(*li.target) == di::DriveClass::Local);
        HDF_CHECK(li.target->filename() == fs::path("real"));
    }
    HDF_CHECK(!di::link_info(t.path / "real").link);
    HDF_CHECK(!di::link_info(t.path / "missing").link);
    write_file(t.path / "AppData" / "Cemu" / "settings.xml",
               settings_xml(std::nullopt, {t.path / "jlocal", t.path / "jvol"}));
    Log log;
    HDF_CHECK_EQ(summary(detect::find(env_for(t.path / "AppData", log), {}), t.path),
                 std::string("folder|game_paths|jlocal/TP|||missing 135 of 136\n"));
    if (vol) HDF_CHECK(log.has("jvol (link target: unknown drive)"));
    else std::printf("    (detect_root_links: no volume junction made, that check was skipped)\n");
    Log plog;
    detect::Env env = env_for(t.path / "AppData", plog);
    env.picked = t.path / "jlocal";
    HDF_CHECK_EQ(summary(detect::find(env, {}), t.path), std::string("folder|picked|jlocal/TP|||missing 135 of 136\n"));
#endif
}

// I3: a log callback that throws never stops the search and never escapes find().
HDF_TEST(detect_log_may_throw) {
    TempDir t;
    std::vector<WuaFile> wf;
    add_wua_title(wf, kUsaTop, Content::Full);
    make_wua(t.path / "TP.wua", wf);
    write_file(t.path / "AppData" / "Cemu" / "title_list_cache.xml",
               title_list_xml({{"000500001019e500", 81, 3, t.path / "gone.wua", kUsaTop},
                               {"000500001019e500", 81, 3, t.path / "TP.wua", kUsaTop}}));
    detect::Env env;
    env.appdata = t.path / "AppData";
    env.localappdata = t.path / "none";
    int calls = 0;
    for (int kind = 0; kind < 2; ++kind) {
        env.log = [&calls, kind](std::string_view) {
            ++calls;
            if (kind == 0) throw std::runtime_error("log");
            throw 42;
        };
        bool threw = false;
        std::vector<Candidate> found;
        try {
            found = detect::find(env, {});
        } catch (...) {
            threw = true;
        }
        HDF_CHECK(!threw);
        HDF_CHECK_EQ(summary(found, t.path), "wua|title_list_cache|TP.wua|" + kUsaTop + "||\n");
    }
    HDF_CHECK(calls >= 2);
}

// I7: a Cemu file over its read cap is noted, not skipped silently.
HDF_TEST(detect_cemu_file_over_cap) {
    TempDir t;
    const fs::path cemu = t.path / "AppData" / "Cemu";
    make_sized(cemu / "settings.xml", (u64(4) << 20) + 1);
    make_sized(cemu / "title_list_cache.xml", (u64(16) << 20) + 1);
    Log log;
    HDF_CHECK(detect::find(env_for(t.path / "AppData", log), {}).empty());
    HDF_CHECK(log.has("settings.xml: io.too_big"));
    HDF_CHECK(log.has("title_list_cache.xml: io.too_big"));
}