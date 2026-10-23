// hdf_convert: concrete sources and sinks, SHA-256 (owner C2; src/base/*).
//
// Ports tools/dist/hdf_inputs.py (Wua reader, folder lookup, layering). Correction #1(b): the .wua container
// reader and the folder lookup see unchecked bytes, so they bounds-check everything and never trust sizes or
// offsets from the file. Correction #5: FolderSource and WuaSource read in pieces of 4 MB or less and check
// the stop token between pieces.
#pragma once

#include "hdf/core.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hdf {

// ---------------------------------------------------------------------------------------------- SHA-256
// Windows CNG (BCryptHash) in the library (plan 3.3 C2). Byte hashes everywhere (Correction #17).
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(View data);
    std::array<u8, 32> finish();  // the object can't be updated after finish()
    std::string finish_hex();     // lower-case hex

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string sha256_hex(View data);
std::string to_hex(View data);  // lower-case

// "Res/Stage/x.ARC" or "/res/stage/x.arc" -> "res/stage/x.arc": '\\' -> '/', leading '/' dropped, ASCII lower.
std::string lower_key(std::string_view path);

// The vendored library versions actually linked (zlib "1.3.2", zstd "1.5.7"), for the log and the CLI.
struct LibVersions {
    std::string zlib, zstd;
};
LibVersions lib_versions();

// ---------------------------------------------------------------------------------------------- sources
// A folder on disk (an extracted GC "files" folder, a TP HD "content" folder, a Cemu title's content).
// Case-insensitive like common.exists_ci; the real names found are cached.
class FolderSource final : public Source {
public:
    explicit FolderSource(std::filesystem::path root);
    ~FolderSource() override;
    std::optional<u64> size(std::string_view path) const override;
    Result<Bytes> read(std::string_view path, std::stop_token stop = {}) const override;
    std::string describe() const override;
    const std::filesystem::path& root() const { return root_; }
    // every file under root, '/' paths as found on disk (compare, tests)
    Result<std::vector<std::string>> list(std::stop_token stop = {}) const;

private:
    std::filesystem::path root_;
    struct Cache;
    std::unique_ptr<Cache> cache_;
};

// Cemu's compressed Wii U archive (ZArchive v1, hdf_inputs.Wua): 64 KiB blocks, each stored raw or as one zstd
// frame; footer 144 bytes (6 sections, magic 0x169F52D6, version 0x61BF3A01). One open file, shared by the
// WuaSource layers made from it.
class WuaArchive {
public:
    static Result<std::shared_ptr<WuaArchive>> open(const std::filesystem::path& file);
    ~WuaArchive();
    WuaArchive(const WuaArchive&) = delete;
    WuaArchive& operator=(const WuaArchive&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::vector<std::string> tops() const;  // sorted top folder names, e.g. "000500001019e500_v81"
    std::optional<u64> size(std::string_view lower_path) const;  // full path inside the archive, lower-case
    bool has_folder(std::string_view lower_path) const;           // some file lies under "<lower_path>/"
    std::size_t file_count() const;
    Result<Bytes> read(std::string_view lower_path, std::stop_token stop = {}) const;

private:
    WuaArchive();
    std::filesystem::path path_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    mutable std::mutex lock_;
};

// One title of a .wua: the paths under "<top>/content/".
class WuaSource final : public Source {
public:
    WuaSource(std::shared_ptr<const WuaArchive> archive, std::string top);
    std::optional<u64> size(std::string_view path) const override;
    Result<Bytes> read(std::string_view path, std::stop_token stop = {}) const override;
    std::string describe() const override;
    const std::string& top() const { return top_; }

private:
    std::shared_ptr<const WuaArchive> archive_;
    std::string top_;
};

// Layers searched in order; the first layer that has a path wins. Updates go first, then the base
// (hdf_inputs.HdSource; Correction #6: base folder or .wua + update in the mlc, or a .wua with _v0 and _v81).
class LayeredSource final : public Source {
public:
    LayeredSource() = default;
    void add(std::unique_ptr<Source> layer);
    std::size_t layers() const { return layers_.size(); }
    std::optional<u64> size(std::string_view path) const override;
    Result<Bytes> read(std::string_view path, std::stop_token stop = {}) const override;
    std::string describe() const override;

private:
    std::vector<std::unique_ptr<Source>> layers_;
};

// Files held in memory. The mod's DvdSource fills one with the 13 GC inputs on the game thread; the CLI's
// --gc-mode memory does the same from a folder. Keys are lower_key(path).
class MemorySource final : public Source {
public:
    explicit MemorySource(std::string description = "memory");
    void add(std::string_view path, Bytes data);  // replaces an existing entry
    std::optional<u64> size(std::string_view path) const override;
    Result<Bytes> read(std::string_view path, std::stop_token stop = {}) const override;
    std::string describe() const override;
    std::vector<std::string> list() const;  // keys, sorted
    u64 total_bytes() const;

private:
    std::string description_;
    std::vector<std::pair<std::string, Bytes>> files_;  // sorted by key
};

// A .zip (.dusk) read through the central directory; stored and deflate entries. CLI and tests only (the
// oracle .dusk); never used by the mod.
class ZipSource final : public Source {
public:
    static Result<std::unique_ptr<ZipSource>> open(const std::filesystem::path& file);
    ~ZipSource() override;
    std::optional<u64> size(std::string_view path) const override;
    Result<Bytes> read(std::string_view path, std::stop_token stop = {}) const override;
    std::string describe() const override;
    std::vector<std::string> list() const;  // file entry names as stored, in central-directory order (no folders)

private:
    ZipSource();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------------------------- sinks
// Writes root/rel, creating folders. rel must be relative and must not contain "..". On Windows each file is
// flushed to disk (FlushFileBuffers) before write() returns, so a stamp committed afterwards never names data
// that a power cut could still lose (fix-wave1 L5).
// Wave 4 (wave2_G_verify L2): with a stop token, write() checks it before each file and between its pieces of
// 4 MiB or less; a stopped write returns "stopped" and deletes the partly written file (the folder has no stamp
// yet, so nothing names it).
class FolderSink final : public Sink {
public:
    explicit FolderSink(std::filesystem::path root);
    FolderSink(std::filesystem::path root, std::stop_token stop);
    Result<void> write(std::string_view rel, View data) override;
    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
    std::stop_token stop_;
};

// Keeps every file in memory, in write order (overlays for the hand-over, tests).
class MemorySink final : public Sink {
public:
    Result<void> write(std::string_view rel, View data) override;
    std::vector<std::pair<std::string, Bytes>> files;
};

}  // namespace hdf
