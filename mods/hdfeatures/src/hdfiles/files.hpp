#pragma once

// HD Features: access to the game files the mod makes from the player's own games (module R1; plan 2.7, 2.8
// with Corrections #10, #13).
//
// Since 2.1.0 the .dusk holds no game files. They are stored in the mod's data folder
// (<Dusklight data>\mod_data\local.ricky.hdfeatures\files-<set8>\..., hdfiles/cache.hpp) and every reader goes
// through this API instead of ResourceService:
//   - load(rel) / exists(rel): rel is the old res/-relative ResourceService path, e.g. "stamps/hdtext/messages.bin";
//   - image_src(rel): an absolute path for an RmlUi <img src>, replacing mod://<id>/res/<rel> (S1 spike 1);
//   - the 56 overlay files are added with OverlayService add_buffer (hdfiles/overlays.hpp).
// Every file is hashed against cache.json when it is read (Correction #10). Game thread only.
//
// Lifecycle (called from mod.cpp):
//   initialize()  in mod_initialize (hdfiles/setup.cpp), before the features start: data_dir, cache check, the
//                 56 overlays. There is no lazy start: before initialize() nothing is valid;
//   update()      first thing in every mod_update: Registered -> Ready (Correction #13);
//   shutdown()    in mod_shutdown, after the features.
// R2's setup code calls adopt() once its worker has committed a new cache in a mid-session mod_update.
//
// Std-only header: safe to include anywhere (before or after game headers).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdfiles {

namespace cache {
class Cache;  // hdfiles/cache.hpp (not included here: it pulls in the converter's headers)
}

// One overlay file handed over by the setup worker (hdfiles/worker.hpp). The worker hashes the bytes on its own
// thread (wave3 F4), so the game thread only compares sha256 with the stamp.
struct HandOver {
    std::string rel;                  // "overlay/res/..." as in the stamp
    std::vector<std::uint8_t> bytes;
    std::string sha256;               // of bytes, lower-case hex
};

enum class State : std::uint8_t {
    Off,         // initialize() has not run (or shutdown() ran): every reader below answers "no files"
    NoCache,     // no valid stored files yet (error_code() says why): the features wait for R2's setup
    Registered,  // stored files valid, overlays added: load/exists/image_src work; the overlays go live at
                 // the end of this frame
    Ready,       // a later mod_update saw the overlays live (DVDConvertPathToEntrynum >= 0)
    Failed,      // no data folder, the overlays could not be added, or a stored file failed its check
};

// mod_initialize. Returns the state reached (Registered when the stored files are valid).
State initialize();
// Every mod_update, before the feature switches.
void update();
// mod_shutdown (the loader removes the overlays by itself).
void shutdown();

// R2, game thread, after the worker made (hdf::commit()) or read a valid set: takes the cache the worker opened
// and checked, and adds the worker's 56 overlay buffers (moved in; no disk read, no hashing here: wave3 F4).
// Returns Registered on success; Ready follows in a later update(). On failure the state is Failed and
// error_code() says why.
State adopt(cache::Cache&& opened, std::vector<HandOver>&& overlays);

State state();
// The overlays are live: the features may start (mid-session start, Correction #13).
bool ready();
// The stored files are valid and the overlays are added (Registered or Ready). At mod_initialize this is the
// test for starting the features at load as before: the overlays are pushed before the game reads anything.
bool files_valid();
// Why the files are not valid: "cache.missing", "cache.set", "cache.bad", "cache.incomplete", "cache.hash",
// "host.data_dir", "overlay.add", ...; "" while valid.
std::string_view error_code();
std::string_view error_detail();
// A stored file failed its check after the start (Correction #10): the stamp was removed, so the next start
// makes the files again. The running features keep what they already loaded. A file that only can't be read
// now (locked, "io.read") is not damage: that file's fallback applies and the stamp stays.
bool damaged();

// The data folder (UTF-8, as HostService gave it; "" before initialize() or when it failed).
std::string_view data_dir();

// The file's bytes, checked against cache.json. False (and logged once) when the files are not valid, the
// path is not one of the stored files, or the check fails.
bool load(std::string_view rel, std::vector<std::uint8_t>& out);
// The file is one of the stored files and its bytes pass the check (read and hashed once per session).
bool exists(std::string_view rel);
// RML-escaped absolute path with '/' separators for <img src="...">, into the versioned files-<set8> folder
// (RmlUi keeps a texture by its source text for the whole session, S1). "" when exists(rel) is false.
std::string image_src(std::string_view rel);
// Why load(rel) / exists(rel) gave false, as one short sentence for the log and the debug window:
// "HD Features' game files are missing (rel)", "HD Features' game file didn't pass the check (rel)",
// "HD Features' game file can't be read now (rel)" or "rel is not one of HD Features' game files".
// "" when the file passed (or has not been read yet).
std::string problem(std::string_view rel);

}  // namespace hdfiles
