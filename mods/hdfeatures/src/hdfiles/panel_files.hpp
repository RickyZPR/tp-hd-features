#pragma once

// HD Features: the "Game files" section of the Mods panel and its two Troubleshooting buttons (module R2; plan
// 1.2, Corrections #14, #16; the S1 consequence 9a picker: a button that calls FileService pick_folder).
//
//   Game files                          one sentence (hdfiles/setup.hpp line()), a progress bar while the files
//                                       are made, "Choose your Twilight Princess HD folder" and "Try again"
//                                       when they apply
//   Troubleshooting                     "Make the game files again", "Delete the game files" (files ready)
//
// Called by ../panel.cpp: build() and build_troubleshooting() from the panel's build callback (they take the
// handles of that build), update() from its update callback, the only place the elements change (Correction
// #16: handles die at every rebuild). Std-only header.

#include <cstdint>

namespace hdfiles::panel {

void build(std::uint64_t pane);
void build_troubleshooting(std::uint64_t pane);
void update();

}  // namespace hdfiles::panel
