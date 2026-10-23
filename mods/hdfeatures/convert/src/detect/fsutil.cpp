// Module D: file-system side of detection. Correction #5: listings with an entry and time budget and stop
// checks, attributes without recall (OFFLINE / RECALL_ON_OPEN / RECALL_ON_DATA_ACCESS), reparse points
// reported (never followed by the scans), drive classes from GetDriveTypeW, and only small bounded reads
// (disc and TMD headers). fix-wave1 L1: names that are not valid UTF-16 are left out (base::utf8_exact).
#include "detect_internal.hpp"

#include "hdf/source.hpp"

#include "../base/base_util.hpp"

#include <algorithm>
#include <cwctype>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace hdf::detect::internal {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr u32 kChildListing = 5000;  // child_ci: entries looked at when the direct path is absent

#ifdef _WIN32
constexpr DWORD kCloudAttrs = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                              FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
#endif

u32 rd32le(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
u64 rd64le(const u8* p) { return u64(rd32le(p)) | (u64(rd32le(p + 4)) << 32); }
u32 rd32be(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]); }

// The budget check between entries: stop token always, the clock every 32 entries.
ListStatus spend(Budget& b) {
    if (b.stop.stop_requested()) return ListStatus::Stopped;
    if (b.entries_left == 0) return ListStatus::Budget;
    --b.entries_left;
    ++b.used;
    if ((b.used & 31) == 0 && Clock::now() > b.deadline) return ListStatus::Time;
    return ListStatus::Ok;
}

bool product_ok(const u8* p) {  // "WUP-" then 6 printable ASCII characters
    if (p[0] != 'W' || p[1] != 'U' || p[2] != 'P' || p[3] != '-') return false;
    for (int i = 4; i < 10; ++i)
        if (p[i] < 0x21 || p[i] > 0x7E) return false;
    return true;
}

}  // namespace

ListStatus list_dir(const fs::path& dir, Budget& b, std::vector<DirEntry>& out) {
    out.clear();
    if (b.stop.stop_requested()) return ListStatus::Stopped;
    if (Clock::now() > b.deadline) return ListStatus::Time;
#ifdef _WIN32
    std::wstring pattern = dir.native();
    if (pattern.empty()) return ListStatus::Unreadable;
    if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return ListStatus::Unreadable;
    ListStatus st = ListStatus::Ok;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        st = spend(b);
        if (st != ListStatus::Ok) break;
        auto name = base::utf8_exact(fs::path(n));
        if (!name || name->empty()) continue;
        DirEntry e;
        e.name = std::move(*name);
        const DWORD a = fd.dwFileAttributes;
        e.dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.reparse = (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.link = e.reparse && IsReparseTagNameSurrogate(fd.dwReserved0);
        e.cloud = (a & kCloudAttrs) != 0;
        e.size = (u64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    std::error_code ec;
    ListStatus st = ListStatus::Ok;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return ListStatus::Unreadable;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        st = spend(b);
        if (st != ListStatus::Ok) break;
        auto name = base::utf8_exact(it->path().filename());
        if (!name || name->empty()) continue;
        DirEntry e;
        e.name = std::move(*name);
        std::error_code tec;
        e.link = it->is_symlink(tec);
        e.reparse = e.link;
        e.dir = !e.link && it->is_directory(tec);
        e.size = e.dir ? 0 : it->file_size(tec);
        out.push_back(std::move(e));
    }
#endif
    std::sort(out.begin(), out.end(), [](const DirEntry& x, const DirEntry& y) { return x.name < y.name; });
    return st;
}

PathInfo path_info(const fs::path& p) {
    PathInfo r;
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return r;
    r.exists = true;
    r.dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    r.reparse = (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    r.cloud = (a & kCloudAttrs) != 0;
#else
    std::error_code ec;
    const fs::file_status s = fs::status(p, ec);
    if (ec || !fs::exists(s)) return r;
    r.exists = true;
    r.dir = fs::is_directory(s);
    r.reparse = fs::is_symlink(fs::symlink_status(p, ec));
#endif
    return r;
}

DriveClass drive_class(const fs::path& p) {
#ifdef _WIN32
    std::wstring s = p.native();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    auto starts = [&](std::wstring_view pre) {
        if (s.size() < pre.size()) return false;
        for (std::size_t i = 0; i < pre.size(); ++i)
            if (std::towupper(s[i]) != std::towupper(pre[i])) return false;
        return true;
    };
    if (starts(L"\\\\?\\UNC\\")) return DriveClass::Remote;
    if (starts(L"\\\\?\\") || starts(L"\\\\.\\")) s = s.substr(4);
    else if (starts(L"\\\\")) return DriveClass::Remote;  // \\server\share
    if (s.size() < 2 || s[1] != L':' || !((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')))
        return DriveClass::Unknown;
    const wchar_t root[4] = {s[0], L':', L'\\', 0};
    switch (GetDriveTypeW(root)) {
        case DRIVE_FIXED:
        case DRIVE_RAMDISK: return DriveClass::Local;
        case DRIVE_REMOTE: return DriveClass::Remote;
        case DRIVE_REMOVABLE: return DriveClass::Removable;
        case DRIVE_CDROM: return DriveClass::Optical;
        default: return DriveClass::Unknown;
    }
#else
    (void)p;
    return DriveClass::Local;  // no drive types here; the budgets still apply
#endif
}

std::string volume_key(const fs::path& p) {
#ifdef _WIN32
    std::wstring s = p.native();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    auto starts = [&](std::wstring_view pre) {
        if (s.size() < pre.size()) return false;
        for (std::size_t i = 0; i < pre.size(); ++i)
            if (std::towupper(s[i]) != std::towupper(pre[i])) return false;
        return true;
    };
    if (starts(L"\\\\?\\UNC\\")) s = L"\\\\" + s.substr(8);
    else if (starts(L"\\\\?\\") || starts(L"\\\\.\\")) s = s.substr(4);
    std::wstring key;
    if (starts(L"\\\\")) {  // \\server\share
        const std::size_t server_end = s.find(L'\\', 2);
        if (server_end == std::wstring::npos || server_end == 2) return {};
        std::size_t share_end = s.find(L'\\', server_end + 1);
        if (share_end == std::wstring::npos) share_end = s.size();
        if (share_end == server_end + 1) return {};
        key = s.substr(0, share_end);
    } else if (s.size() >= 2 && s[1] == L':' && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'))) {
        key = s.substr(0, 2);
    } else {
        return {};
    }
    for (wchar_t& c : key) c = static_cast<wchar_t>(std::towlower(c));
    return base::utf8_lossy(fs::path(key));
#else
    (void)p;
    return {};
#endif
}

LinkInfo link_info(const fs::path& p) {
    LinkInfo r;
#ifdef _WIN32
    // FILE_FLAG_OPEN_REPARSE_POINT: the link itself is opened, its target is never touched
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    std::vector<u8> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()),
                                    &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 8 || got > buf.size()) return r;
    const u32 tag = rd32le(buf.data());
    if (!IsReparseTagNameSurrogate(tag)) return r;  // cloud, dedup, compressed files: data on this disk or recalled
    r.link = true;
    // REPARSE_DATA_BUFFER: tag, length, reserved, then SubstituteNameOffset/Length, PrintNameOffset/Length (u16);
    // a symbolic link has a u32 Flags (1 = relative) before its PathBuffer.
    std::size_t path_at = 0;
    bool relative = false;
    if (tag == IO_REPARSE_TAG_MOUNT_POINT) {
        path_at = 16;
    } else if (tag == IO_REPARSE_TAG_SYMLINK) {
        path_at = 20;
        if (got < path_at) return r;
        relative = (rd32le(buf.data() + 16) & 1) != 0;
    } else {
        return r;  // another kind of link: its target is not known
    }
    if (got < path_at) return r;
    const std::size_t sub_off = u32(buf[8]) | (u32(buf[9]) << 8), sub_len = u32(buf[10]) | (u32(buf[11]) << 8);
    if (sub_len == 0 || sub_len % 2 != 0 || path_at + sub_off + sub_len > got) return r;
    std::wstring t(sub_len / 2, L'\0');
    for (std::size_t i = 0; i < t.size(); ++i) {
        const u8* q = buf.data() + path_at + sub_off + 2 * i;
        t[i] = static_cast<wchar_t>(u32(q[0]) | (u32(q[1]) << 8));
    }
    if (relative) {
        r.target = p.parent_path() / fs::path(t);
        return r;
    }
    // NT names: \??\C:\x, \??\UNC\server\share\x, \??\Volume{guid}\x
    if (t.size() >= 8 && t.compare(0, 4, L"\\??\\") == 0 && (t[4] == L'U' || t[4] == L'u') &&
        (t[5] == L'N' || t[5] == L'n') && (t[6] == L'C' || t[6] == L'c') && t[7] == L'\\')
        t = L"\\\\" + t.substr(8);
    else if (t.compare(0, 4, L"\\??\\") == 0)
        t = L"\\\\?\\" + t.substr(4);
    r.target = fs::path(t);
#else
    std::error_code ec;
    if (!fs::is_symlink(fs::symlink_status(p, ec))) return r;
    r.link = true;
    fs::path t = fs::read_symlink(p, ec);
    if (ec || t.empty()) return r;
    r.target = t.is_relative() ? p.parent_path() / t : t;
#endif
    return r;
}

std::string_view drive_class_name(DriveClass c) {
    switch (c) {
        case DriveClass::Local: return "local";
        case DriveClass::Remote: return "network";
        case DriveClass::Removable: return "removable";
        case DriveClass::Optical: return "CD/DVD";
        default: return "unknown";
    }
}

std::optional<std::string> disc_product_code(const fs::path& file) {
    auto f = base::RandomFile::open(file);
    if (!f) return std::nullopt;
    const u64 size = (*f)->size();
    u8 head[0x24] = {};
    if (size < 10) return std::nullopt;
    const u64 n = size < sizeof head ? size : sizeof head;
    if (!(*f)->read_at(0, head, n)) return std::nullopt;
    u64 at = 0;  // .wud: the product code is the first 10 bytes of the disc
    if (n >= 0x24 && rd32le(head) == 0x30585557u && rd32le(head + 4) == 0x1099D02Eu) {  // "WUX0"
        const u64 sector = rd32le(head + 8);
        const u64 disc = rd64le(head + 0x10);
        if (sector < 0x100 || sector > (u64(1) << 24) || (sector & (sector - 1)) != 0 || disc == 0) return std::nullopt;
        const u64 sectors = disc / sector + (disc % sector ? 1 : 0);
        if (sectors > size / 4) return std::nullopt;  // the index table can't be larger than the file
        const u64 table_end = 0x20 + sectors * 4;
        const u64 data = (table_end + sector - 1) / sector * sector;
        const u64 index0 = rd32le(head + 0x20);
        if (index0 >= sectors) return std::nullopt;
        at = data + index0 * sector;
    }
    if (at > size || size - at < 10) return std::nullopt;
    u8 code[10];
    if (!(*f)->read_at(at, code, sizeof code)) return std::nullopt;
    if (!product_ok(code)) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(code), sizeof code);
}

std::optional<std::string> tmd_title_id(const fs::path& file) {
    auto f = base::RandomFile::open(file);
    if (!f) return std::nullopt;
    u8 sig[4];
    if ((*f)->size() < 0x300 || !(*f)->read_at(0, sig, 4)) return std::nullopt;
    u64 at = 0;
    switch (rd32be(sig)) {
        case 0x00010004u: at = 0x18C; break;  // RSA-2048: 4 + 0x100 signature + 0x3C padding, then +0x4C
        case 0x00010003u: at = 0x28C; break;  // RSA-4096: 4 + 0x200 + 0x3C, then +0x4C
        default: return std::nullopt;
    }
    u8 id[8];
    if (!(*f)->read_at(at, id, 8)) return std::nullopt;
    return upper_ascii(to_hex(View(id, 8)));
}

std::optional<fs::path> child_ci(const fs::path& dir, std::string_view name) {
    auto part = path_from_utf8(name);
    if (!part) return std::nullopt;
    const fs::path direct = dir / *part;
    if (path_info(direct).exists) return direct;
#ifdef _WIN32
    return std::nullopt;  // Windows names are already compared without case
#else
    Budget b;
    b.entries_left = kChildListing;
    b.deadline = Clock::now() + std::chrono::seconds(1);
    std::vector<DirEntry> es;
    if (list_dir(dir, b, es) != ListStatus::Ok) return std::nullopt;
    const std::string want = base::lower_ascii(name);
    for (const DirEntry& e : es)
        if (base::lower_ascii(e.name) == want) {
            if (auto q = path_from_utf8(e.name)) return dir / *q;
        }
    return std::nullopt;
#endif
}

std::optional<fs::path> path_ci(const fs::path& dir, std::string_view rel) {
    fs::path cur = dir;
    std::size_t i = 0;
    while (i <= rel.size()) {
        std::size_t j = rel.find('/', i);
        if (j == std::string_view::npos) j = rel.size();
        if (j > i) {
            auto next = child_ci(cur, rel.substr(i, j - i));
            if (!next) return std::nullopt;
            cur = std::move(*next);
        }
        i = j + 1;
    }
    return cur;
}

std::string path_key(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    std::string k = base::lower_ascii(base::utf8_lossy(abs.lexically_normal(), true));
    while (k.size() > 1 && k.back() == '/') k.pop_back();
    return k;
}

}  // namespace hdf::detect::internal
