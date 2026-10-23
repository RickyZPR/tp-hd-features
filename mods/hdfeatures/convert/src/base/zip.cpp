// ZipSource (C2): a .zip / .dusk read through its central directory; stored and deflate entries, CRC-32 checked.
// CLI and tests only (the oracle .dusk); the mod never uses it. No ZIP64, no encryption (refused).
#include "hdf/source.hpp"

#include "base_util.hpp"

#include <zlib.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace hdf {

namespace {

u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

constexpr u32 kEocd = 0x06054b50, kCentral = 0x02014b50, kLocal = 0x04034b50;

}  // namespace

struct ZipSource::Impl {
    struct Entry {
        std::string name;  // as stored
        u16 method = 0, flags = 0;
        u32 crc = 0;
        u64 csize = 0, usize = 0, local = 0;
    };
    std::filesystem::path path;
    std::unique_ptr<base::RandomFile> file;
    std::vector<Entry> entries;                    // central-directory order (files only)
    std::vector<std::pair<std::string, std::size_t>> index;  // lower_key(name) -> entry, sorted
    std::mutex lock;

    const Entry* find(std::string_view name) const {
        const std::string key = lower_key(name);
        auto it = std::lower_bound(index.begin(), index.end(), key,
                                   [](const auto& e, const std::string& k) { return e.first < k; });
        if (it == index.end() || it->first != key) return nullptr;
        return &entries[it->second];
    }
};

ZipSource::ZipSource() = default;
ZipSource::~ZipSource() = default;

Result<std::unique_ptr<ZipSource>> ZipSource::open(const std::filesystem::path& file) {
    auto f = base::RandomFile::open(file);
    if (!f) return std::unexpected(f.error());
    const u64 size = (*f)->size();
    if (size < 22) return fail("zip.bad", "not a zip file");
    // End of central directory: the last 22 + up to 65535 comment bytes.
    const u64 tail = std::min<u64>(size, 22 + 65535);
    Bytes t(static_cast<std::size_t>(tail));
    if (auto ok = (*f)->read_at(size - tail, t.data(), tail); !ok) return std::unexpected(ok.error());
    std::size_t eocd = std::string::npos;
    for (std::size_t i = t.size() - 22 + 1; i-- > 0;)
        if (le32(t.data() + i) == kEocd && i + 22 + le16(t.data() + i + 20) == t.size()) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return fail("zip.bad", "no end-of-central-directory record");
    const u8* e = t.data() + eocd;
    const u32 count = le16(e + 10), cd_size = le32(e + 12), cd_off = le32(e + 16);
    if (le16(e + 4) != 0 || le16(e + 6) != 0 || le16(e + 8) != count) return fail("zip.bad", "multi-disk zip");
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) return fail("zip.bad", "ZIP64 is not supported");
    if (u64(cd_off) + cd_size > size - tail + eocd) return fail("zip.damaged", "central directory outside the file");
    Bytes cd(cd_size);
    if (auto ok = (*f)->read_at(cd_off, cd.data(), cd_size); !ok) return std::unexpected(ok.error());

    std::unique_ptr<ZipSource> z(new ZipSource());
    z->impl_ = std::make_unique<Impl>();
    Impl& im = *z->impl_;
    im.path = file;
    std::size_t p = 0;
    for (u32 i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || le32(cd.data() + p) != kCentral) return fail("zip.damaged", "bad central directory");
        const u8* c = cd.data() + p;
        Impl::Entry en;
        en.flags = le16(c + 8);
        en.method = le16(c + 10);
        en.crc = le32(c + 16);
        en.csize = le32(c + 20);
        en.usize = le32(c + 24);
        const u16 n = le16(c + 28), x = le16(c + 30), k = le16(c + 32);
        en.local = le32(c + 42);
        if (p + 46 + n + x + k > cd.size()) return fail("zip.damaged", "bad central directory");
        en.name.assign(reinterpret_cast<const char*>(c + 46), n);
        p += 46 + std::size_t(n) + x + k;
        if (en.csize == 0xFFFFFFFFu || en.usize == 0xFFFFFFFFu || en.local == 0xFFFFFFFFu)
            return fail("zip.bad", "ZIP64 is not supported");
        if (!en.name.empty() && en.name.back() == '/') continue;  // folder entry
        im.index.emplace_back(lower_key(en.name), im.entries.size());
        im.entries.push_back(std::move(en));
    }
    std::sort(im.index.begin(), im.index.end());
    im.file = std::move(*f);
    return z;
}

std::optional<u64> ZipSource::size(std::string_view path) const {
    const Impl::Entry* e = impl_->find(path);
    if (!e) return std::nullopt;
    return e->usize;
}

Result<Bytes> ZipSource::read(std::string_view path, std::stop_token stop) const {
    if (stop.stop_requested()) return fail("stopped");
    const Impl::Entry* e = impl_->find(path);
    if (!e) return fail("io.missing", std::string(path));
    if (e->flags & 1) return fail("zip.bad", "encrypted entry: " + e->name);
    if (e->method != 0 && e->method != 8) return fail("zip.bad", "unsupported compression: " + e->name);
    if (e->usize > base::kMaxFileBytes || e->csize > base::kMaxFileBytes) return fail("io.too_big", e->name);
    Bytes stored(static_cast<std::size_t>(e->csize));
    {
        std::lock_guard<std::mutex> g(impl_->lock);
        u8 lh[30];
        if (auto ok = impl_->file->read_at(e->local, lh, 30); !ok) return std::unexpected(ok.error());
        if (le32(lh) != kLocal) return fail("zip.damaged", "bad local header: " + e->name);
        const u64 data = e->local + 30 + le16(lh + 26) + le16(lh + 28);
        if (auto ok = impl_->file->read_at(data, stored.data(), stored.size()); !ok) return std::unexpected(ok.error());
    }
    Bytes out;
    if (e->method == 0) {
        out = std::move(stored);
    } else {
        out.resize(static_cast<std::size_t>(e->usize));
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail("zip.bad", "zlib init");
        zs.next_in = stored.data();
        zs.avail_in = static_cast<uInt>(stored.size());
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        int rc = Z_OK;
        while (rc == Z_OK) {
            if (stop.stop_requested()) {
                inflateEnd(&zs);
                return fail("stopped");
            }
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc == Z_BUF_ERROR && zs.avail_out == 0) break;
        }
        const bool ended = rc == Z_STREAM_END;
        const u64 produced = zs.total_out;
        inflateEnd(&zs);
        if (!ended || produced != e->usize) return fail("zip.damaged", "inflate failed: " + e->name);
    }
    if (out.size() != e->usize) return fail("zip.damaged", "size differs: " + e->name);
    uLong crc = ::crc32(0L, Z_NULL, 0);
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t n = std::min<std::size_t>(out.size() - done, std::size_t(1) << 30);
        crc = ::crc32(crc, out.data() + done, static_cast<uInt>(n));
        done += n;
    }
    if (u32(crc) != e->crc) return fail("zip.damaged", "CRC-32 differs: " + e->name);
    return out;
}

std::string ZipSource::describe() const { return "zip " + base::utf8_lossy(impl_->path.filename()); }

std::vector<std::string> ZipSource::list() const {
    std::vector<std::string> out;
    out.reserve(impl_->entries.size());
    for (const auto& e : impl_->entries) out.push_back(e.name);
    return out;
}

}  // namespace hdf
