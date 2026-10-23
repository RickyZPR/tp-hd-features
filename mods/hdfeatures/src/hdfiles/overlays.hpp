#pragma once

// HD Features: the 56 overlay files as runtime overlays (module R1; plan 2.6 Option A, Corrections #10, #13).
//
// Each stored "overlay/res/..." file becomes OverlayService add_buffer("/res/..."): the host copies the bytes
// and keeps them for the session; the loader removes the registrations when the mod is disabled, reloaded or
// fails. add_buffer only marks the overlay set dirty: the game's file table is rebuilt at the end of the frame,
// so a new path has no entry number in the mod_update that added it and has one in the next (S1 spike 7).
// Game thread only.

#include "hdfiles/cache.hpp"
#include "hdfiles/files.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdfiles::overlays {

// The readiness probe (Correction #13): one of the 51 D_SB11 files, all of them new paths on GZ2E01.
inline constexpr const char* kProbePath = "/res/Stage/D_SB11/STG_00.arc";

struct Report {
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    double read_seconds = 0;   // register_from_cache: read + hash; register_buffers: the hash comparison
    double add_seconds = 0;    // the add_buffer calls
};

// "overlay/res/Stage/x.arc" -> "/res/Stage/x.arc"; "" when rel is not an overlay path.
std::string disc_path(std::string_view rel);

// Reads every overlay file of the cache (each one hashed against the stamp, Correction #10) and adds it.
// All or nothing: on any failure the buffers added so far are removed again. Errors: the cache's read errors
// ("cache.hash", "cache.incomplete", "io.read"), "overlay.count" (not the 56 expected), "overlay.add".
hdf::Result<Report> register_from_cache(const cache::Cache& cache);

// R2's hand-over after the worker made or read the files (no disk read): rel "overlay/res/..." -> bytes, as in
// hdf::GenOut::overlays, with the SHA-256 the worker computed from those bytes (wave3 F4: no hashing on the game
// thread). Each buffer must equal the stamp's entry (size + SHA-256), each entry exactly once, all or nothing as
// above. Errors: "overlay.count" (not 56, or a path twice), "cache.hash", "overlay.add". The vectors are freed as
// they are added.
hdf::Result<Report> register_buffers(const cache::Cache& cache, std::vector<HandOver>&& files);

// Removes every overlay this module added (the loader also does it by itself at disable/reload).
void remove_all();
std::size_t count();

// DVDConvertPathToEntrynum(kProbePath) >= 0. Never call it in mod_initialize as a readiness test: in a
// mid-session init the old generation's entries still exist (S1 spike 7).
bool probe_live();

// The game's frame counter (g_Counter.mCounter0): +1 at the end of every game frame, after the mods' updates
// and the overlay rebuild. Not 0 in mod_initialize: a mid-session start (setup.cpp, S1 spike 8).
std::uint32_t frame_counter();

}  // namespace hdfiles::overlays
