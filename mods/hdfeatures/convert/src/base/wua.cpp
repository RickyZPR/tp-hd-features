// WuaArchive / WuaSource (C2): Cemu's .wua (ZArchive v1), ported from tools/dist/hdf_inputs.py class Wua.
//
// Layout (big-endian): data | offset records | names | file tree | meta directory | meta data | footer (144 B).
// Footer: 6 x (u64 offset, u64 size), 32-byte integrity hash, u64 total size, u32 version 0x61BF3A01,
// u32 magic 0x169F52D6. Data: 64 KiB blocks, each stored raw (stored size 65536) or as one zstd frame.
// Offset record (40 B) per 16 blocks: u64 base offset, 16 x u16 (stored size - 1). Tree entry (16 B):
// u32 type|name offset (bit 31 = file); file: u32 offset low, u32 size low, u16 size high, u16 offset high;
// folder: u32 first child, u32 count, u32 reserved. Entry 0 is the root folder. Names: 1-byte length, or
// 2 bytes when the first has bit 7 (length = (b0 & 0x7F) | b1 << 7), then UTF-8 bytes.
//
// Correction #1(b): the container is read before any hash check, so every offset, size, index and name is
// bounds-checked here, and nothing in the file can make the reader loop or recurse without limit. Memory stays
// bounded (fix-wave1 M1): each section is read only when it lies inside the file (64 MB at most), a name is at
// most 255 bytes, a full path at most 1 KiB, and all path keys together (file keys + folder prefixes) at most
// 1 MiB + 16 x (names + tree) bytes, capped at 64 MiB. The real TP HD .wua uses 0.25 MB of its 3 MB budget
// (longest name 24 B, longest path 68 B; VERIFIED fix-wave1). Correction #5: reads of at most 16 blocks (1 MiB
// of output) between stop checks.
#include "hdf/source.hpp"

#include "base_util.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace hdf {

namespace {

constexpr u64 kBlock = 64 * 1024;
constexpr u64 kFooter = 144;
constexpr u32 kMagic = 0x169F52D6;
constexpr u32 kVersion = 0x61BF3A01;
constexpr u64 kRecordBytes = 40;
constexpr u64 kBlocksPerRecord = 16;
constexpr u64 kMaxSection = u64(64) << 20;  // names / tree / offset records: far above any real archive
constexpr u32 kMaxDepth = 32;
constexpr u64 kBatchBlocks = 16;            // 1 MiB of output between stop checks
constexpr u64 kMaxName = 255;               // bytes per name (real archives: 24 B TP HD, 42 B WW HD)
constexpr u64 kMaxPath = 1024;              // bytes per full path (real: 68 B / 93 B)
constexpr u64 kKeyBudgetBase = u64(1) << 20;  // path-key budget: base + 16 x (names + tree) bytes,
constexpr u64 kKeyBudgetPerIndexByte = 16;    // capped at kKeyBudgetCap (real: about 2 x (names + tree))
constexpr u64 kKeyBudgetCap = u64(64) << 20;

u64 rd64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
u32 rd32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]); }
u16 rd16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }

struct Section {
    u64 off = 0, size = 0;
};

}  // namespace

struct WuaArchive::Impl {
    struct FileRec {
        u64 offset = 0, size = 0;
    };
    std::unique_ptr<base::RandomFile> file;
    Section data;
    Bytes offrec;
    std::map<std::string, FileRec, std::less<>> files;  // lower full path -> record
    std::vector<std::string> tops;
    ZSTD_DCtx* dctx = nullptr;
    // the last two decoded blocks (files that share a block)
    struct Cached {
        u64 index = ~u64(0);
        Bytes data;
    };
    Cached cache[2];
    int next_slot = 0;

    ~Impl() {
        if (dctx) ZSTD_freeDCtx(dctx);
    }

    u64 block_count() const { return offrec.size() / kRecordBytes * kBlocksPerRecord; }

    // Stored location of block bi inside the data section.
    Result<std::pair<u64, u64>> locate(u64 bi) const {
        const u64 rec = bi / kBlocksPerRecord;
        if ((rec + 1) * kRecordBytes > offrec.size()) return fail("wua.damaged", "block outside the offset records");
        const u8* r = offrec.data() + rec * kRecordBytes;
        u64 off = rd64(r);
        const u64 k = bi % kBlocksPerRecord;
        for (u64 j = 0; j < k; ++j) off += u64(rd16(r + 8 + 2 * j)) + 1;
        const u64 csz = u64(rd16(r + 8 + 2 * k)) + 1;
        if (off > data.size || csz > data.size - off) return fail("wua.damaged", "block outside the data section");
        return std::pair<u64, u64>{off, csz};
    }

    Result<void> decode(View stored, Bytes& out) {
        if (stored.size() == kBlock) {
            out.assign(stored.begin(), stored.end());
            return {};
        }
        out.resize(kBlock);
        const std::size_t n = ZSTD_decompressDCtx(dctx, out.data(), out.size(), stored.data(), stored.size());
        if (ZSTD_isError(n)) return fail("wua.damaged", std::string("zstd: ") + ZSTD_getErrorName(n));
        out.resize(n);
        return {};
    }

    const Bytes* cached(u64 bi) const {
        for (const Cached& c : cache)
            if (c.index == bi) return &c.data;
        return nullptr;
    }

    Bytes& remember(u64 bi) {
        Cached& c = cache[next_slot];
        next_slot ^= 1;
        c.index = bi;
        return c.data;
    }

    Result<Bytes> read_range(u64 off, u64 size, std::stop_token stop) {
        if (size > base::kMaxFileBytes) return fail("io.too_big", std::to_string(size) + " bytes");
        Bytes out(static_cast<std::size_t>(size));
        if (size == 0) return out;
        const u64 end = off + size;
        u64 b = off / kBlock;
        const u64 last = (end - 1) / kBlock;
        Bytes stored;
        auto copy_part = [&](u64 bi, const Bytes& blk) -> Result<void> {
            const u64 bstart = bi * kBlock;
            const u64 from = std::max(off, bstart);
            const u64 to = std::min(end, bstart + blk.size());
            if (to <= from) return fail("wua.damaged", "short block");
            std::memcpy(out.data() + (from - off), blk.data() + (from - bstart), static_cast<std::size_t>(to - from));
            if (to < std::min(end, bstart + kBlock)) return fail("wua.damaged", "short block");
            return {};
        };
        while (b <= last) {
            if (stop.stop_requested()) return fail("stopped");
            if (const Bytes* c = cached(b)) {
                if (auto ok = copy_part(b, *c); !ok) return std::unexpected(ok.error());
                ++b;
                continue;
            }
            // One read for a run of blocks stored back to back (at most kBatchBlocks).
            auto first = locate(b);
            if (!first) return std::unexpected(first.error());
            std::vector<std::pair<u64, u64>> locs{*first};
            u64 e = b;
            while (e < last && locs.size() < kBatchBlocks && !cached(e + 1)) {
                auto next = locate(e + 1);
                if (!next) return std::unexpected(next.error());
                if (next->first != locs.back().first + locs.back().second) break;
                locs.push_back(*next);
                ++e;
            }
            const u64 run = locs.back().first + locs.back().second - locs.front().first;
            stored.resize(static_cast<std::size_t>(run));
            if (auto ok = file->read_at(data.off + locs.front().first, stored.data(), run); !ok)
                return std::unexpected(ok.error());
            for (u64 i = 0; i < locs.size(); ++i) {
                const View piece(stored.data() + (locs[i].first - locs.front().first),
                                 static_cast<std::size_t>(locs[i].second));
                Bytes& blk = remember(b + i);
                if (auto ok = decode(piece, blk); !ok) {
                    cache[0].index = cache[1].index = ~u64(0);
                    return std::unexpected(ok.error());
                }
                if (auto ok = copy_part(b + i, blk); !ok) return std::unexpected(ok.error());
            }
            b = e + 1;
        }
        return out;
    }
};

WuaArchive::WuaArchive() = default;
WuaArchive::~WuaArchive() = default;

Result<std::shared_ptr<WuaArchive>> WuaArchive::open(const std::filesystem::path& path) {
    auto f = base::RandomFile::open(path);
    if (!f) return std::unexpected(f.error());
    const u64 fsize = (*f)->size();
    if (fsize < kFooter) return fail("wua.bad", "not a .wua archive (too small)");
    u8 ft[kFooter];
    if (auto ok = (*f)->read_at(fsize - kFooter, ft, kFooter); !ok) return std::unexpected(ok.error());
    if (rd32(ft + 140) != kMagic || rd32(ft + 136) != kVersion) return fail("wua.bad", "not a Cemu .wua archive");
    if (rd64(ft + 128) != fsize) return fail("wua.damaged", "the archive's recorded size differs from the file size");
    Section sec[6];
    for (int i = 0; i < 6; ++i) {
        sec[i].off = rd64(ft + 16 * i);
        sec[i].size = rd64(ft + 16 * i + 8);
        if (sec[i].off > fsize - kFooter || sec[i].size > fsize - kFooter - sec[i].off)
            return fail("wua.damaged", "a section lies outside the file");
    }
    const Section& s_off = sec[1];
    const Section& s_names = sec[2];
    const Section& s_tree = sec[3];
    if (s_off.size > kMaxSection || s_names.size > kMaxSection || s_tree.size > kMaxSection)
        return fail("wua.damaged", "section too large");
    if (s_off.size % kRecordBytes != 0 || s_tree.size % 16 != 0 || s_tree.size < 16)
        return fail("wua.damaged", "bad section size");

    std::shared_ptr<WuaArchive> a(new WuaArchive());
    a->path_ = path;
    a->impl_ = std::make_unique<Impl>();
    Impl& im = *a->impl_;
    im.data = sec[0];
    im.offrec.resize(static_cast<std::size_t>(s_off.size));
    Bytes names(static_cast<std::size_t>(s_names.size)), tree(static_cast<std::size_t>(s_tree.size));
    if (auto ok = (*f)->read_at(s_off.off, im.offrec.data(), s_off.size); !ok) return std::unexpected(ok.error());
    if (auto ok = (*f)->read_at(s_names.off, names.data(), s_names.size); !ok) return std::unexpected(ok.error());
    if (auto ok = (*f)->read_at(s_tree.off, tree.data(), s_tree.size); !ok) return std::unexpected(ok.error());

    const u64 entries = s_tree.size / 16;
    const u64 data_limit = im.block_count() * kBlock;  // uncompressed bytes the offset records can describe
    auto name_at = [&](u32 off, std::string& out) -> bool {
        if (off >= names.size()) return false;
        u64 n = names[off];
        u64 p = off + 1;
        if (n & 0x80) {
            if (p >= names.size()) return false;
            n = (n & 0x7F) | (u64(names[p]) << 7);
            ++p;
        }
        if (n == 0 || n > kMaxName || n > names.size() - p) return false;
        out.assign(reinterpret_cast<const char*>(names.data() + p), static_cast<std::size_t>(n));
        if (out == "." || out == ".." || out.find('/') != std::string::npos || out.find('\0') != std::string::npos)
            return false;
        return true;
    };

    // Iterative walk; every entry may be reached once (a garbage tree can't loop or share children).
    std::vector<u8> seen(static_cast<std::size_t>(entries), 0);
    struct Dir {
        u64 index;
        std::string prefix;
        u32 depth;
    };
    if (rd32(tree.data()) & 0x80000000u) return fail("wua.damaged", "the root is not a folder");
    std::vector<Dir> stack{{0, std::string(), 0}};
    seen[0] = 1;
    std::map<std::string, bool> tops;
    std::string name;
    // fix-wave1 M1: every file key and folder prefix built below counts against this budget, so a small file
    // whose entries all name the same long folder chain can't make the reader allocate gigabytes.
    const u64 key_budget = std::min(kKeyBudgetCap, kKeyBudgetBase + kKeyBudgetPerIndexByte * (s_names.size + s_tree.size));
    u64 key_bytes = 0;
    while (!stack.empty()) {
        Dir d = std::move(stack.back());
        stack.pop_back();
        const u8* e = tree.data() + d.index * 16;
        const u64 start = rd32(e + 4), count = rd32(e + 8);
        if (start > entries || count > entries - start) return fail("wua.damaged", "folder outside the tree");
        for (u64 k = start; k < start + count; ++k) {
            if (seen[k]) return fail("wua.damaged", "tree entry reached twice");
            seen[k] = 1;
            const u8* c = tree.data() + k * 16;
            const u32 t = rd32(c);
            if (!name_at(t & 0x7FFFFFFFu, name)) return fail("wua.damaged", "bad name");
            if (d.prefix.size() + name.size() > kMaxPath) return fail("wua.damaged", "path too long");
            key_bytes += d.prefix.size() + name.size() + 1;
            if (key_bytes > key_budget) return fail("wua.damaged", "file tree too large for the archive");
            std::string full = d.prefix + name;
            if (t & 0x80000000u) {
                const u32 hi = rd32(c + 12);
                const u64 off = u64(rd32(c + 4)) | (u64(hi & 0xFFFF) << 32);
                const u64 size = u64(rd32(c + 8)) | (u64(hi >> 16) << 32);
                if (off > data_limit || size > data_limit - off) return fail("wua.damaged", "file outside the data");
                const std::size_t slash = full.find('/');
                if (slash != std::string::npos) tops.emplace(full.substr(0, slash), true);
                im.files.insert_or_assign(lower_key(full), Impl::FileRec{off, size});
            } else {
                if (d.depth + 1 > kMaxDepth) return fail("wua.damaged", "folders nested too deep");
                stack.push_back(Dir{k, full + "/", d.depth + 1});
            }
        }
    }
    for (const auto& t : tops) im.tops.push_back(t.first);  // std::map: sorted, unique
    im.dctx = ZSTD_createDCtx();
    if (!im.dctx) return fail("wua.bad", "zstd: out of memory");
    im.file = std::move(*f);
    return a;
}

std::vector<std::string> WuaArchive::tops() const { return impl_->tops; }

std::optional<u64> WuaArchive::size(std::string_view lower_path) const {
    const std::string key = lower_key(lower_path);
    auto it = impl_->files.find(key);
    if (it == impl_->files.end()) return std::nullopt;
    return it->second.size;
}

bool WuaArchive::has_folder(std::string_view lower_path) const {
    std::string prefix = lower_key(lower_path);
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    prefix += '/';
    auto it = impl_->files.lower_bound(prefix);
    return it != impl_->files.end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

std::size_t WuaArchive::file_count() const { return impl_->files.size(); }

Result<Bytes> WuaArchive::read(std::string_view lower_path, std::stop_token stop) const {
    const std::string key = lower_key(lower_path);
    auto it = impl_->files.find(key);
    if (it == impl_->files.end()) return fail("io.missing", std::string(lower_path));
    std::lock_guard<std::mutex> g(lock_);
    return impl_->read_range(it->second.offset, it->second.size, stop);
}

// ---------------------------------------------------------------------------------------------- WuaSource
WuaSource::WuaSource(std::shared_ptr<const WuaArchive> archive, std::string top)
    : archive_(std::move(archive)), top_(std::move(top)) {}

namespace {
std::string wua_key(std::string_view top, std::string_view path) {
    return lower_key(top) + "/content/" + lower_key(path);
}
}  // namespace

std::optional<u64> WuaSource::size(std::string_view path) const { return archive_->size(wua_key(top_, path)); }

Result<Bytes> WuaSource::read(std::string_view path, std::stop_token stop) const {
    return archive_->read(wua_key(top_, path), stop);
}

std::string WuaSource::describe() const {
    return "wua " + base::utf8_lossy(archive_->path().filename()) + " [" + top_ + "]";
}

}  // namespace hdf
