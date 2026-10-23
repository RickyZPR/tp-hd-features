// src/base helpers (C2): bounded file reads, random-access file, path checks, hex and path keys.
#include "base_util.hpp"

#include "hdf/source.hpp"

#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hdf {

std::string to_hex(View data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.resize(data.size() * 2);
    for (std::size_t i = 0; i < data.size(); ++i) {
        s[2 * i] = kDigits[data[i] >> 4];
        s[2 * i + 1] = kDigits[data[i] & 15];
    }
    return s;
}

std::string lower_key(std::string_view path) {
    std::string s;
    s.reserve(path.size());
    for (char c : path) s.push_back(c == '\\' ? '/' : base::ascii_lower(c));
    std::size_t start = 0;
    while (start < s.size() && s[start] == '/') ++start;
    return s.substr(start);
}

namespace base {

namespace {

// UTF-16 (Windows) or bytes (elsewhere) -> UTF-8. lossy: an unpaired surrogate becomes U+FFFD; otherwise it
// makes the result empty-optional.
std::optional<std::string> utf8_of(const std::filesystem::path& p, bool generic, bool lossy) {
#ifdef _WIN32
    const std::wstring& w = p.native();
    std::string out;
    out.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        u32 c = static_cast<u16>(w[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && static_cast<u16>(w[i + 1]) >= 0xDC00 &&
            static_cast<u16>(w[i + 1]) <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<u16>(w[i + 1]) - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            if (!lossy) return std::nullopt;
            c = 0xFFFD;
        }
        if (generic && c == '\\') c = '/';
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
#else
    (void)generic;
    (void)lossy;
    return p.native();  // POSIX names are bytes; '/' is already the separator
#endif
}

}  // namespace

std::optional<std::string> utf8_exact(const std::filesystem::path& p, bool generic) {
    return utf8_of(p, generic, false);
}

std::string utf8_lossy(const std::filesystem::path& p, bool generic) { return *utf8_of(p, generic, true); }

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

bool safe_rel(std::string_view rel) {
    if (rel.empty() || rel.size() > 400) return false;
    std::size_t parts = 0, i = 0;
    while (i <= rel.size()) {
        std::size_t j = rel.find('/', i);
        if (j == std::string_view::npos) j = rel.size();
        const std::string_view part = rel.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part)
            if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':') return false;
        if (++parts > 32) return false;
        i = j + 1;
    }
    return true;
}

#ifdef _WIN32

namespace {
Error win_error(const char* code, const std::filesystem::path& p) {
    const DWORD e = GetLastError();
    std::error_code ec(int(e), std::system_category());
    return Error{code, utf8_lossy(p.filename()) + ": " + ec.message()};
}
}  // namespace

Result<std::unique_ptr<RandomFile>> RandomFile::open(const std::filesystem::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return fail("io.missing", utf8_lossy(p.filename()));
        SetLastError(e);
        return std::unexpected(win_error("io.open", p));
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        Error err = win_error("io.read", p);
        CloseHandle(h);
        return std::unexpected(std::move(err));
    }
    std::unique_ptr<RandomFile> f(new RandomFile());
    f->handle_ = h;
    f->size_ = static_cast<u64>(sz.QuadPart);
    return f;
}

RandomFile::~RandomFile() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

Result<void> RandomFile::read_at(u64 offset, u8* out, u64 n) {
    if (offset > size_ || n > size_ - offset) return fail("io.read", "read past the end of the file");
    while (n > 0) {
        const DWORD want = static_cast<DWORD>(n < kReadPiece ? n : kReadPiece);
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), out, want, &got, &ov)) {
            std::error_code ec(int(GetLastError()), std::system_category());
            return fail("io.read", ec.message());
        }
        if (got == 0) return fail("io.read", "unexpected end of file");
        out += got;
        offset += got;
        n -= got;
    }
    return {};
}

#else

Result<std::unique_ptr<RandomFile>> RandomFile::open(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return fail("io.missing", p.filename().string());
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return fail("io.open", p.filename().string());
    const u64 size = std::filesystem::file_size(p, ec);
    if (ec) {
        std::fclose(f);
        return fail("io.read", p.filename().string());
    }
    std::unique_ptr<RandomFile> r(new RandomFile());
    r->handle_ = f;
    r->size_ = size;
    return r;
}

RandomFile::~RandomFile() {
    if (handle_) std::fclose(static_cast<std::FILE*>(handle_));
}

Result<void> RandomFile::read_at(u64 offset, u8* out, u64 n) {
    if (offset > size_ || n > size_ - offset) return fail("io.read", "read past the end of the file");
    std::FILE* f = static_cast<std::FILE*>(handle_);
    if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) return fail("io.read", "seek failed");
    while (n > 0) {
        const std::size_t want = static_cast<std::size_t>(n < kReadPiece ? n : kReadPiece);
        const std::size_t got = std::fread(out, 1, want, f);
        if (got == 0) return fail("io.read", "unexpected end of file");
        out += got;
        n -= got;
    }
    return {};
}

#endif

Result<Bytes> read_file(const std::filesystem::path& p, std::stop_token stop, u64 max) {
    auto f = RandomFile::open(p);
    if (!f) return std::unexpected(f.error());
    const u64 size = (*f)->size();
    if (size > max) return fail("io.too_big", std::to_string(size) + " bytes");
    Bytes out(static_cast<std::size_t>(size));
    u64 done = 0;
    while (done < size) {
        if (stop.stop_requested()) return fail("stopped");
        const u64 n = (size - done) < kReadPiece ? (size - done) : kReadPiece;
        if (auto ok = (*f)->read_at(done, out.data() + done, n); !ok) return std::unexpected(ok.error());
        done += n;
    }
    return out;
}

Result<std::pair<u64, std::string>> sha256_file(const std::filesystem::path& p, std::stop_token stop) {
    auto f = RandomFile::open(p);
    if (!f) return std::unexpected(f.error());
    const u64 size = (*f)->size();
    Bytes buf(static_cast<std::size_t>(size < kReadPiece ? size : kReadPiece));
    Sha256 h;
    u64 done = 0;
    while (done < size) {
        if (stop.stop_requested()) return fail("stopped");
        const u64 n = (size - done) < kReadPiece ? (size - done) : kReadPiece;
        if (auto ok = (*f)->read_at(done, buf.data(), n); !ok) return std::unexpected(ok.error());
        h.update(View(buf.data(), static_cast<std::size_t>(n)));
        done += n;
    }
    return std::pair<u64, std::string>{size, h.finish_hex()};
}

}  // namespace base
}  // namespace hdf
