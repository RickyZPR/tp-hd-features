// HD Features: access to the stored game files (see files.hpp). Module R1.

// Library headers first, no game headers at all (stamps/prelude.hpp explains why the order matters).
#include "hdfiles/files.hpp"

#include "hdfiles/cache.hpp"
#include "hdfiles/overlays.hpp"

#include "hdf/generate.hpp"
#include "hdf/outputs.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <mods/api.h>
#include <mods/service.hpp>
#include <mods/svc/host.h>
#include <mods/svc/log.hpp>
#include <mods/svc/overlay.h>

// The two services the stored files need (plan 3.3 R1). Imported here so mod.cpp keeps its own list; do not
// import them a second time elsewhere. HostService minor 2 (data_dir) is what Dusklight v2.0.3 serves.
IMPORT_SERVICE(HostService, svc_host);
IMPORT_SERVICE(OverlayService, svc_overlay);

namespace hdfiles {
namespace {

namespace fs = std::filesystem;

State g_state = State::Off;
int g_updatesSinceAdd = 0;      // mod_update calls since the overlays were added
std::string g_dataDirUtf8;
fs::path g_dataDir;
std::optional<cache::Cache> g_cache;
std::string g_code, g_detail;
bool g_damaged = false;
std::string g_imageRoot;        // "<data_dir>/files-<set8>/" with '/' separators, RML-escaped later

// The last read of a stored file this session (load / exists / image_src).
enum class Check : std::uint8_t {
    Passed,
    Damaged,     // "cache.hash" / "cache.incomplete": the stamp was dropped (mark_damaged)
    Unreadable,  // "io.read" (locked, access denied): that file's fallback this session, the stamp stays
};
std::unordered_map<std::string, Check> g_checked;  // stamp rel "res/..." -> its last check
std::unordered_set<std::string> g_loggedMissing;
std::unordered_set<std::string> g_loggedUnreadable;

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void set_error(const hdf::Error& error) {
    g_code = error.code;
    g_detail = error.detail;
}

std::string rel_of(std::string_view resRel) {
    std::string rel = "res/";
    rel.append(resRel);
    return rel;
}

// The stored bytes are wrong or gone: only these drop the stamp (Correction #10). "io.read" does not
// (wave3_R1_verify L1): a short lock by antivirus or the indexer must not make all the files again.
bool is_damage(const hdf::Error& error) {
    return error.code == "cache.hash" || error.code == "cache.incomplete";
}

// A stored file failed its check after the overlays were added (Correction #10).
void mark_damaged(const hdf::Error& error) {
    if (!g_damaged) {
        g_damaged = true;
        const bool dropped = cache::drop(g_dataDir);
        mods::log::error("game files: a stored file failed its check ({}: {}); {}", error.code, error.detail,
            dropped ? "they will be made again at the next start" : "cache.json could not be removed");
    }
}

// A read of a stored file failed: damage drops the stamp, anything else only skips that file.
void note_failure(const cache::Entry& entry, const hdf::Error& error) {
    if (is_damage(error)) {
        g_checked[entry.rel] = Check::Damaged;
        mark_damaged(error);
        return;
    }
    g_checked[entry.rel] = Check::Unreadable;
    if (g_loggedUnreadable.insert(entry.rel).second) {
        mods::log::warn("game files: can't read {} now ({}: {}); its fallback is used, the stored files are kept",
            entry.rel, error.code, error.detail);
    }
}

bool get_data_dir() {
    if (!g_dataDirUtf8.empty()) {
        return true;
    }
    const char* path = nullptr;
    if (svc_host == nullptr || svc_host->header.minor_version < 2 || svc_host->data_dir == nullptr ||
        svc_host->data_dir(mod_ctx, &path) != MOD_OK || path == nullptr || path[0] == '\0')
    {
        return false;
    }
    g_dataDirUtf8 = path;
    g_dataDir = cache::u8_path(g_dataDirUtf8);
    return !g_dataDir.empty();
}

void set_image_root() {
    std::string root = g_dataDirUtf8;
    for (char& c : root) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (!root.empty() && root.back() != '/') {
        root.push_back('/');
    }
    root += g_cache->stamp().folder;
    root.push_back('/');
    g_imageRoot = std::move(root);
}

void log_not_valid() {
    if (g_code == "cache.missing") {
        mods::log::info("game files: none stored yet; the stamps and the cave wait for them");
    } else if (g_code == "cache.set") {
        mods::log::info("game files: stored by another HD Features version ({}); they must be made again", g_detail);
    } else {
        mods::log::warn("game files: the stored files can't be used ({}: {}); they must be made again", g_code,
            g_detail);
    }
}

}  // namespace

State initialize() {
    if (g_state != State::Off) {
        return g_state;
    }
    const auto t0 = std::chrono::steady_clock::now();
    g_code.clear();
    g_detail.clear();
    g_damaged = false;
    g_checked.clear();
    g_loggedMissing.clear();
    g_loggedUnreadable.clear();
    if (!get_data_dir()) {
        g_state = State::Failed;
        set_error(hdf::Error{"host.data_dir", "HostService gave no data folder"});
        mods::log::error("game files: Dusklight gave the mod no data folder; the stamps and the cave stay off");
        return g_state;
    }
    auto opened = cache::Cache::open(g_dataDir);
    if (!opened) {
        g_state = State::NoCache;
        set_error(opened.error());
        log_not_valid();
        return g_state;
    }
    g_cache.emplace(std::move(*opened));
    const double checkSeconds = seconds_since(t0);
    auto added = overlays::register_from_cache(*g_cache);
    if (!added) {
        set_error(added.error());
        const bool hashProblem = is_damage(added.error());
        const bool dropped = hashProblem && cache::drop(g_dataDir);
        g_cache.reset();
        g_state = hashProblem ? State::NoCache : State::Failed;
        if (g_code == "io.read") {
            // Not damage: the stamp stays and the next start tries again.
            mods::log::error("game files: can't read the stored files now ({}); the stamps and the cave stay off "
                             "until the next start", g_detail);
        } else {
            mods::log::error("game files: {} ({}){}", g_code, g_detail, dropped ? "; they will be made again" : "");
        }
        return g_state;
    }
    const std::uint32_t cleaned = cache::cleanup_leftovers(*g_cache);
    set_image_root();
    g_state = State::Registered;
    g_updatesSinceAdd = 0;
    mods::log::info("game files: {} files (set {}) in {}; {} overlays added, {:.1f} MB (check {:.3f} s, read {:.3f} s, "
                    "add {:.3f} s){}",
        g_cache->file_count(), g_cache->set8(), cache::short_path(g_cache->files_dir()), added->files,
        static_cast<double>(added->bytes) / 1048576.0, checkSeconds, added->read_seconds, added->add_seconds,
        cleaned != 0 ? fmt::format("; {} leftover folder(s) removed", cleaned) : std::string());
    return g_state;
}

void update() {
    if (g_state != State::Registered) {
        return;
    }
    // Correction #13: never in the mod_update (or mod_initialize) that added the buffers; from the next
    // mod_update on, once the game's file table has the probe path.
    if (g_updatesSinceAdd < 1) {
        ++g_updatesSinceAdd;
        return;
    }
    ++g_updatesSinceAdd;
    if (overlays::probe_live()) {
        g_state = State::Ready;
        mods::log::info("game files: overlays live after {} update(s)", g_updatesSinceAdd);
    } else if (g_updatesSinceAdd == 600) {
        // wave 4 review I7: normally live after 2 updates. Only another mod that hides the probe path could keep
        // it away; the features then keep waiting, so the log says why.
        mods::log::warn("game files: the overlays are still not live after 600 updates (the game finds no {}); "
                        "another mod may hide it, so the stamps and the cave wait", overlays::kProbePath);
    }
}

void shutdown() {
    // The loader removes the overlay registrations itself when the mod goes away.
    g_state = State::Off;
    g_cache.reset();
    g_checked.clear();
    g_loggedMissing.clear();
    g_loggedUnreadable.clear();
    g_imageRoot.clear();
}

State adopt(cache::Cache&& opened, std::vector<HandOver>&& buffers) {
    if (!get_data_dir()) {
        g_state = State::Failed;
        set_error(hdf::Error{"host.data_dir", "HostService gave no data folder"});
        return g_state;
    }
    if (g_state == State::Registered || g_state == State::Ready) {
        return g_state;  // already valid (nothing to swap in this session)
    }
    // wave3 F4: the worker opened and checked the stamp (cache::Cache::open) and hashed the buffers on its own
    // thread, so this frame only compares hashes and calls add_buffer.
    g_cache.emplace(std::move(opened));
    auto added = overlays::register_buffers(*g_cache, std::move(buffers));
    if (!added) {
        set_error(added.error());
        g_cache.reset();
        g_state = State::Failed;
        mods::log::error("game files: {} ({})", g_code, g_detail);
        return g_state;
    }
    g_code.clear();
    g_detail.clear();
    g_damaged = false;
    g_checked.clear();
    g_loggedMissing.clear();
    g_loggedUnreadable.clear();
    set_image_root();
    g_state = State::Registered;
    g_updatesSinceAdd = 0;
    mods::log::info("game files: {} files (set {}); {} overlays added from memory, {:.1f} MB (compare {:.3f} s, add "
                    "{:.3f} s)",
        g_cache->file_count(), g_cache->set8(), added->files, static_cast<double>(added->bytes) / 1048576.0,
        added->read_seconds, added->add_seconds);
    return g_state;
}

State state() {
    return g_state;
}

bool ready() {
    return g_state == State::Ready;
}

bool files_valid() {
    return g_state == State::Registered || g_state == State::Ready;
}

std::string_view error_code() {
    return g_code;
}

std::string_view error_detail() {
    return g_detail;
}

bool damaged() {
    return g_damaged;
}

std::string_view data_dir() {
    return g_dataDirUtf8;
}

bool load(std::string_view resRel, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!files_valid()) {
        return false;
    }
    const std::string rel = rel_of(resRel);
    const cache::Entry* entry = g_cache->find(rel);
    if (entry == nullptr || entry->overlay) {
        if (g_loggedMissing.insert(rel).second) {
            mods::log::warn("game files: {} is not one of the stored files", rel);
        }
        return false;
    }
    auto data = g_cache->read(*entry);
    if (!data) {
        note_failure(*entry, data.error());
        return false;
    }
    g_checked[entry->rel] = Check::Passed;
    out = std::move(*data);
    return true;
}

bool exists(std::string_view resRel) {
    if (!files_valid()) {
        return false;
    }
    const cache::Entry* entry = g_cache->find(rel_of(resRel));
    if (entry == nullptr || entry->overlay) {
        return false;
    }
    if (const auto it = g_checked.find(entry->rel); it != g_checked.end()) {
        return it->second == Check::Passed;
    }
    // Read and hashed once per session: RmlUi reads the images itself, so this is their check (Correction #10).
    // A file that can't be read now keeps its fallback for the rest of the session (the stamp stays).
    auto data = g_cache->read(*entry);
    if (!data) {
        note_failure(*entry, data.error());
        return false;
    }
    g_checked[entry->rel] = Check::Passed;
    return true;
}

std::string image_src(std::string_view resRel) {
    if (!exists(resRel)) {
        return {};
    }
    const std::string path = g_imageRoot + g_cache->find(rel_of(resRel))->rel;
    std::string out;
    out.reserve(path.size() + 16);
    for (const char c : path) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;"; break;
            case '\\': out += '/'; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string problem(std::string_view resRel) {
    const std::string rel(resRel);
    if (!files_valid()) {
        return "HD Features' game files are missing (" + rel + ")";
    }
    const cache::Entry* entry = g_cache->find(rel_of(resRel));
    if (entry == nullptr || entry->overlay) {
        return rel + " is not one of HD Features' game files";
    }
    const auto it = g_checked.find(entry->rel);
    if (it == g_checked.end() || it->second == Check::Passed) {
        return {};
    }
    return it->second == Check::Damaged ? "HD Features' game file didn't pass the check (" + rel + ")" :
                                          "HD Features' game file can't be read now (" + rel + ")";
}

}  // namespace hdfiles
