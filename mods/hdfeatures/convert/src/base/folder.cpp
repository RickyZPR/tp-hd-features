// FolderSource (case-insensitive, common.exists_ci / hdf_inputs._exists_ci) and FolderSink (C2).
#include "hdf/source.hpp"

#include "base_util.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hdf {

namespace {

// UTF-8 -> path; nothing for bytes that are not valid UTF-8 (the conversion throws on them on Windows).
std::optional<std::filesystem::path> path_of(std::string_view utf8) {
    try {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The parts of a lower_key path, or nothing if it is not a plain relative path.
std::optional<std::vector<std::string_view>> split_key(std::string_view key) {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i <= key.size()) {
        std::size_t j = key.find('/', i);
        if (j == std::string_view::npos) j = key.size();
        const std::string_view part = key.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return std::nullopt;
        parts.push_back(part);
        if (parts.size() > 32) return std::nullopt;
        i = j + 1;
    }
    if (parts.empty()) return std::nullopt;
    return parts;
}

constexpr std::size_t kMaxListing = 200000;  // entries kept per folder listing

}  // namespace

// Real names per folder, keyed by the lower-case folder path under root ("" = root).
struct FolderSource::Cache {
    struct Entry {
        std::string real;
        bool dir = false;
    };
    using Listing = std::map<std::string, Entry, std::less<>>;  // lower name -> real name
    std::mutex lock;
    std::map<std::string, Listing, std::less<>> dirs;

    // The real path of a regular file under root, found without regard to case.
    std::optional<std::filesystem::path> resolve(const std::filesystem::path& root, std::string_view path);
    static Listing list_folder(const std::filesystem::path& dir);
};

FolderSource::FolderSource(std::filesystem::path root) : root_(std::move(root)), cache_(std::make_unique<Cache>()) {}
FolderSource::~FolderSource() = default;

FolderSource::Cache::Listing FolderSource::Cache::list_folder(const std::filesystem::path& dir) {
    Listing listing;
    std::error_code ec;
    std::vector<std::pair<std::string, bool>> names;
    for (std::filesystem::directory_iterator d(dir, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && d != end; d.increment(ec)) {
        auto name = base::utf8_exact(d->path().filename());
        if (!name) continue;  // not valid UTF-16 (fix-wave1 L1): never one of the listed input names
        std::error_code tec;
        const bool is_dir = d->is_directory(tec);  // a fixed lookup may pass links (list() does not)
        names.emplace_back(std::move(*name), is_dir);
        if (names.size() >= kMaxListing) break;
    }
    // Deterministic when two names differ only in case (case-sensitive folders): the first in byte order.
    std::sort(names.begin(), names.end());
    for (auto& [name, is_dir] : names)
        listing.try_emplace(base::lower_ascii(name), Entry{name, is_dir});
    return listing;
}

std::optional<std::filesystem::path> FolderSource::Cache::resolve(const std::filesystem::path& root,
                                                                  std::string_view path) {
    const std::string key = lower_key(path);
    auto parts = split_key(key);
    if (!parts) return std::nullopt;
    std::lock_guard<std::mutex> g(lock);
    std::filesystem::path real = root;
    std::string dir_key;
    for (std::size_t k = 0; k < parts->size(); ++k) {
        const std::string_view part = (*parts)[k];
        auto it = dirs.find(dir_key);
        bool fresh = false;
        if (it == dirs.end()) {
            it = dirs.emplace(dir_key, list_folder(real)).first;
            fresh = true;
        }
        auto e = it->second.find(part);
        if (e == it->second.end() && !fresh) {
            // The folder may have changed since it was listed (e.g. a file written later): list it once more.
            it->second = list_folder(real);
            e = it->second.find(part);
        }
        if (e == it->second.end()) return std::nullopt;
        const bool last = k + 1 == parts->size();
        if (!last && !e->second.dir) return std::nullopt;
        auto part_path = path_of(e->second.real);  // listed names are valid UTF-8
        if (!part_path) return std::nullopt;
        real /= *part_path;
        if (!dir_key.empty()) dir_key += '/';
        dir_key += part;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(real, ec)) return std::nullopt;
    return real;
}

std::optional<u64> FolderSource::size(std::string_view path) const {
    auto real = cache_->resolve(root_, path);
    if (!real) return std::nullopt;
    std::error_code ec;
    const u64 n = std::filesystem::file_size(*real, ec);
    if (ec) return std::nullopt;
    return n;
}

Result<Bytes> FolderSource::read(std::string_view path, std::stop_token stop) const {
    if (stop.stop_requested()) return fail("stopped");
    auto real = cache_->resolve(root_, path);
    if (!real) return fail("io.missing", std::string(path));
    return base::read_file(*real, stop);
}

std::string FolderSource::describe() const { return "folder " + base::utf8_lossy(root_, true); }

Result<std::vector<std::string>> FolderSource::list(std::stop_token stop) const {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root_, std::filesystem::directory_options::skip_permission_denied,
                                                     ec);
    if (ec) return fail("io.open", base::utf8_lossy(root_) + ": " + ec.message());
    std::size_t n = 0;
    for (std::filesystem::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) return fail("io.read", ec.message());
        if ((++n & 255) == 0 && stop.stop_requested()) return fail("stopped");
        std::error_code tec;
        if (it->is_symlink(tec)) {
            it.disable_recursion_pending();  // no reparse points (Correction #5)
            continue;
        }
        if (!it->is_regular_file(tec)) continue;
        auto rel = base::utf8_exact(it->path().lexically_relative(root_), true);
        if (!rel) continue;  // a name that is not valid UTF-16 (fix-wave1 L1)
        out.push_back(std::move(*rel));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------------------------- FolderSink
FolderSink::FolderSink(std::filesystem::path root) : root_(std::move(root)) {}

FolderSink::FolderSink(std::filesystem::path root, std::stop_token stop)
    : root_(std::move(root)), stop_(std::move(stop)) {}

Result<void> FolderSink::write(std::string_view rel, View data) {
    if (stop_.stop_requested()) return fail("stopped");
    if (!base::safe_rel(rel)) return fail("io.write", "unsafe output path: " + std::string(rel));
    const auto rel_path = path_of(rel);
    if (!rel_path) return fail("io.write", "output path is not UTF-8: " + std::string(rel));
    const std::filesystem::path full = root_ / *rel_path;
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    if (ec) return fail("io.write", std::string(rel) + ": " + base::os_error_text(ec));
#ifdef _WIN32
    // fix-wave1 L5: the data reaches the disk (FlushFileBuffers) before write() returns, so a cache.json
    // committed after the last write never names data that is still only in the OS cache.
    HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::error_code wec(int(GetLastError()), std::system_category());
        return fail("io.write", std::string(rel) + ": " + base::os_error_text(wec));
    }
    std::size_t done = 0;
    bool ok = true, stopped = false;
    while (ok && done < data.size()) {
        if (done > 0 && stop_.stop_requested()) {  // wave 4, G L2: between the 4 MiB pieces
            stopped = true;
            break;
        }
        const DWORD n = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, std::size_t(base::kReadPiece)));
        DWORD wrote = 0;
        ok = WriteFile(h, data.data() + done, n, &wrote, nullptr) && wrote == n;
        done += wrote;
    }
    if (ok && !stopped) ok = FlushFileBuffers(h) != 0;
    const DWORD err = ok ? 0 : GetLastError();
    const bool closed = CloseHandle(h) != 0;
    if (stopped) {
        DeleteFileW(full.c_str());  // a partly written file: never left for a later check to meet
        return fail("stopped");
    }
    if (!closed && ok) return fail("io.write", std::string(rel) + ": close failed");
    if (!ok) {
        std::error_code wec(int(err), std::system_category());
        return fail("io.write", std::string(rel) + ": " + base::os_error_text(wec));
    }
    return {};
#else
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    if (!f) return fail("io.write", std::string(rel) + ": can't create the file");
    std::size_t done = 0;
    while (done < data.size()) {
        if (done > 0 && stop_.stop_requested()) {
            f.close();
            std::error_code rec;
            std::filesystem::remove(full, rec);
            return fail("stopped");
        }
        const std::size_t n = std::min<std::size_t>(data.size() - done, std::size_t(base::kReadPiece));
        f.write(reinterpret_cast<const char*>(data.data() + done), static_cast<std::streamsize>(n));
        if (!f) return fail("io.write", std::string(rel) + ": write failed");
        done += n;
    }
    f.close();
    if (!f) return fail("io.write", std::string(rel) + ": close failed");
    return {};
#endif
}

}  // namespace hdf
