// LayeredSource (hdf_inputs.HdSource: update layers first), MemorySource and MemorySink (C2).
#include "hdf/source.hpp"

#include "base_util.hpp"

#include <algorithm>

namespace hdf {

// ---------------------------------------------------------------------------------------------- LayeredSource
void LayeredSource::add(std::unique_ptr<Source> layer) {
    if (layer) layers_.push_back(std::move(layer));
}

std::optional<u64> LayeredSource::size(std::string_view path) const {
    for (const auto& l : layers_)
        if (auto n = l->size(path)) return n;
    return std::nullopt;
}

Result<Bytes> LayeredSource::read(std::string_view path, std::stop_token stop) const {
    for (const auto& l : layers_)
        if (l->size(path)) return l->read(path, stop);
    return fail("io.missing", std::string(path));
}

std::string LayeredSource::describe() const {
    if (layers_.empty()) return "no layers";
    std::string s;
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        if (i) s += " over ";
        s += layers_[i]->describe();
    }
    return s;
}

// ---------------------------------------------------------------------------------------------- MemorySource
MemorySource::MemorySource(std::string description) : description_(std::move(description)) {}

namespace {
template <class V>
auto find_key(V& files, std::string_view key) {
    return std::lower_bound(files.begin(), files.end(), key,
                            [](const auto& e, std::string_view k) { return std::string_view(e.first) < k; });
}
}  // namespace

void MemorySource::add(std::string_view path, Bytes data) {
    std::string key = lower_key(path);
    auto it = find_key(files_, key);
    if (it != files_.end() && it->first == key) {
        it->second = std::move(data);
        return;
    }
    files_.emplace(it, std::move(key), std::move(data));
}

std::optional<u64> MemorySource::size(std::string_view path) const {
    const std::string key = lower_key(path);
    auto it = find_key(files_, key);
    if (it == files_.end() || it->first != key) return std::nullopt;
    return it->second.size();
}

Result<Bytes> MemorySource::read(std::string_view path, std::stop_token stop) const {
    if (stop.stop_requested()) return fail("stopped");
    const std::string key = lower_key(path);
    auto it = find_key(files_, key);
    if (it == files_.end() || it->first != key) return fail("io.missing", std::string(path));
    return it->second;
}

std::string MemorySource::describe() const { return description_; }

std::vector<std::string> MemorySource::list() const {
    std::vector<std::string> out;
    out.reserve(files_.size());
    for (const auto& f : files_) out.push_back(f.first);
    return out;
}

u64 MemorySource::total_bytes() const {
    u64 n = 0;
    for (const auto& f : files_) n += f.second.size();
    return n;
}

// ---------------------------------------------------------------------------------------------- MemorySink
Result<void> MemorySink::write(std::string_view rel, View data) {
    if (!base::safe_rel(rel)) return fail("io.write", "unsafe output path: " + std::string(rel));
    for (auto& f : files)
        if (f.first == rel) {
            f.second.assign(data.begin(), data.end());
            return {};
        }
    files.emplace_back(std::string(rel), Bytes(data.begin(), data.end()));
    return {};
}

}  // namespace hdf
