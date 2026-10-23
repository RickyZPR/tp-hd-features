// HD Features: the stored game files (see cache.hpp). Module R1.

#include "hdfiles/cache.hpp"

#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hdfiles::cache {
namespace fs = std::filesystem;

fs::path u8_path(std::string_view utf8) {
    try {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    } catch (...) {
        return {};
    }
}

std::string path_utf8(const fs::path& path) {
    try {
        const std::u8string s = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    } catch (...) {
        return {};
    }
}

hdf::Result<Cache> Cache::open(const fs::path& data_dir) {
    auto stamp = hdf::read_stamp(data_dir);
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    if (auto ok = hdf::validate_stamp(data_dir, *stamp); !ok) {
        return std::unexpected(ok.error());
    }
    Cache c;
    c.data_dir_ = data_dir;
    c.files_dir_ = data_dir / u8_path(stamp->folder);
    c.stamp_ = std::move(*stamp);
    c.entries_.reserve(c.stamp_.files.size());
    for (const auto& [rel, file] : c.stamp_.files) {
        Entry e;
        e.rel = rel;
        e.size = file.first;
        e.sha256 = file.second;
        e.overlay = rel.starts_with("overlay/");
        c.index_.emplace(hdf::lower_key(rel), c.entries_.size());
        c.entries_.push_back(std::move(e));
    }
    return c;
}

const Entry* Cache::find(std::string_view rel) const {
    const auto it = index_.find(hdf::lower_key(rel));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

fs::path Cache::path_of(const Entry& entry) const {
    return files_dir_ / u8_path(entry.rel);
}

hdf::Result<hdf::Bytes> Cache::read(const Entry& entry) const {
    const fs::path p = path_of(entry);
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        // Gone only when the file system says so; a lock or an access error is "io.read" (not damage: the
        // stamp stays, wave3_R1_verify L1).
        std::error_code ec;
        if (!fs::exists(p, ec) && !ec) {
            return hdf::fail("cache.incomplete", "missing " + entry.rel);
        }
        return hdf::fail("io.read", "can't open " + entry.rel);
    }
    // One byte more than expected tells a longer file apart (never read an unexpected size whole).
    hdf::Bytes data(static_cast<std::size_t>(entry.size) + 1);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    const std::size_t got = static_cast<std::size_t>(in.gcount());
    if (in.bad()) {
        return hdf::fail("io.read", "read error in " + entry.rel);
    }
    if (got != entry.size) {
        return hdf::fail("cache.hash", entry.rel + ": other size");
    }
    data.resize(got);
    if (hdf::sha256_hex(data) != entry.sha256) {
        return hdf::fail("cache.hash", entry.rel + ": other bytes");
    }
    return data;
}

hdf::Result<hdf::Bytes> Cache::read(std::string_view rel) const {
    const Entry* e = find(rel);
    if (e == nullptr) {
        return hdf::fail("cache.unlisted", std::string(rel));
    }
    return read(*e);
}

std::vector<const Entry*> Cache::overlays() const {
    std::vector<const Entry*> out;
    for (const Entry& e : entries_) {
        if (e.overlay) {
            out.push_back(&e);
        }
    }
    return out;
}

std::uint64_t Cache::overlay_bytes() const {
    std::uint64_t n = 0;
    for (const Entry& e : entries_) {
        n += e.overlay ? e.size : 0;
    }
    return n;
}

bool another_instance_is_making(const fs::path& data_dir) {
#ifdef _WIN32
    const fs::path lock = data_dir / L"lock";
    std::error_code ec;
    if (!fs::exists(lock, ec)) {
        return false;
    }
    HANDLE h = CreateFileW(lock.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_SHARING_VIOLATION;
    }
    CloseHandle(h);
    return false;
#else
    (void)data_dir;
    return false;
#endif
}

std::uint32_t cleanup_leftovers(const Cache& cache) {
    if (another_instance_is_making(cache.data_dir())) {
        return 0;
    }
    try {
        return hdf::cleanup(cache.data_dir(), &cache.stamp());
    } catch (...) {
        return 0;
    }
}

bool drop(const fs::path& data_dir) {
    std::error_code ec;
    const fs::path stamp = data_dir / u8_path(hdf::kStampFile);
    fs::remove(stamp, ec);
    return !fs::exists(stamp, ec);
}

namespace {

#ifdef _WIN32
std::wstring env(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}
#endif

bool starts_with_ci(std::wstring_view s, std::wstring_view prefix) {
    if (prefix.empty() || s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        wchar_t a = s[i] == L'\\' ? L'/' : s[i];
        wchar_t b = prefix[i] == L'\\' ? L'/' : prefix[i];
        if (a < 0x80 && b < 0x80) {
            a = static_cast<wchar_t>(std::towlower(a));
            b = static_cast<wchar_t>(std::towlower(b));
        }
        if (a != b) {
            return false;
        }
    }
    return s.size() == prefix.size() || s[prefix.size()] == L'\\' || s[prefix.size()] == L'/';
}

}  // namespace

std::string short_path(const fs::path& path) {
    try {
#ifdef _WIN32
        const std::wstring s = path.native();
        const std::pair<const wchar_t*, const wchar_t*> roots[] = {
            {L"APPDATA", L"%APPDATA%"}, {L"LOCALAPPDATA", L"%LOCALAPPDATA%"}, {L"USERPROFILE", L"%USERPROFILE%"}};
        for (const auto& [name, label] : roots) {
            const std::wstring root = env(name);
            if (starts_with_ci(s, root)) {
                return path_utf8(fs::path(std::wstring(label) + s.substr(root.size())));
            }
        }
#endif
        const fs::path parent = path.parent_path();
        return path_utf8(parent.filename() / path.filename());
    } catch (...) {
        return {};
    }
}

}  // namespace hdfiles::cache
