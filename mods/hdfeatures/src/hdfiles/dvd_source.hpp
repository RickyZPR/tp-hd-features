#pragma once

// HD Features: the 13 GameCube inputs, read from the disc Dusklight runs (module R2; plan 2.3 step 4, 2.5,
// Correction #1a).
//
// mod_initialize only, on the game thread, before any overlay of this mod is pushed: at a cold start no mod's
// overlay is live yet, so the bytes are the disc's (S1 spike 3). In a mid-session mod_initialize (installed or
// updated while Dusklight runs) another mod's overlay may already replace one of them (S1 spike 8): the hash
// check then fails and setup asks for a restart.
//
// Std-only header (the DVD calls are in dvd_source.cpp).

#include "hdf/core.hpp"
#include "hdf/source.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hdfiles::dvd {

// The running disc's id, e.g. "GZ2E01" (game name + company from DVDGetCurrentDiskID), "" when unknown.
std::string disc_id();

struct GcInputs {
    std::shared_ptr<const hdf::MemorySource> source;  // the 13 files, keys hdf::lower_key(path)
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    double seconds = 0;
};

// Reads the 13 files of hdf::inputs::gc_files() with DVDOpen / DVDReadPrio and checks each one's size and
// SHA-256 against the embedded list. Errors: "gc.read" (a file can't be opened or read), "gc.size", "gc.hash";
// detail "<n> of 13 differ; first: <path>". The worker gets the source; generate() checks it again.
hdf::Result<GcInputs> read_gc_inputs();

}  // namespace hdfiles::dvd
