#pragma once

// HD Features: every sentence the game files' setup shows the player (the "Game files" line of the Mods panel and
// the toasts), and the line for a worker run that ended with a problem. Moved out of setup.cpp in wave 4, so the
// wording is in one place and the mapping can be tested without Dusklight (no SDK and no game headers here).
// Module R2.

#include "hdfiles/worker.hpp"

#include <string>

namespace hdfiles::setup {

// --- the player's lines (plan 1.2 with Corrections #7, #8, #1c, #14, #15; wave2_D section 3; wave3 G-L4) -----
inline constexpr const char* kLineSearching = "Looking for your Twilight Princess HD files…";
inline constexpr const char* kLineReady = "Game files ready.";
inline constexpr const char* kLineNeedFolder = "Choose your Twilight Princess HD folder to finish setup.";
inline constexpr const char* kLineUpdateNeedFolder = "HD Features was updated: choose your Twilight Princess HD folder again.";
inline constexpr const char* kLineInterrupted = "Setup stopped unexpectedly last time.";
inline constexpr const char* kLineOtherInstance = "Setting up in another Dusklight window.";
inline constexpr const char* kLineRemake = "The game files will be made again at the next start.";
inline constexpr const char* kLineRemakeNeedFolder = "Choose your Twilight Princess HD folder to make the game files again.";
inline constexpr const char* kLineDelete = "The game files will be deleted when Dusklight closes.";
inline constexpr const char* kLineDamaged =
    "A game file didn't pass the check: the game files will be made again at the next start.";
// The check needs this exact build (version and build id, Correction #8): a self-built 2.0.3 is refused too
// (wave 4 review L2).
inline constexpr const char* kLineDusklight = "HD Features needs the official Dusklight 2.0.3 release.";
inline constexpr const char* kLineDisc = "HD Features needs the USA GameCube disc (GZ2E01).";
inline constexpr const char* kLineGcCold = "Your GameCube disc doesn't match the supported USA version.";
inline constexpr const char* kLineGcRestart = "Restart Dusklight to finish setting up HD Features.";
inline constexpr const char* kLineEmptyFolder = "No Twilight Princess HD files in that folder.";
inline constexpr const char* kLineVersion =
    "This Twilight Princess HD copy is a different version: HD Features needs update v81.";
inline constexpr const char* kLineUpdateOnly =
    "Only the Twilight Princess HD update was found: HD Features needs the whole game.";
inline constexpr const char* kLineDamagedWua = "This Twilight Princess HD .wua can't be read.";
// wave3 G-L4: a TP HD file that exists but can't be read (a disk error, an online-only OneDrive file, a share).
inline constexpr const char* kLineHdUnreadable =
    "Some Twilight Princess HD files can't be read: copy the game to your PC and try again.";
inline constexpr const char* kLineSpace = "Not enough disk space: HD Features needs 200 MB free.";
inline constexpr const char* kLineCheck = "The game files didn't pass the check: please report this.";
inline constexpr const char* kLineOther = "Something went wrong making the game files: please report this.";
inline constexpr const char* kLineLoad = "Something went wrong loading the game files: please report this.";
inline constexpr const char* kLineUnreadable = "Couldn't read the game files: restart Dusklight.";

// While the worker makes the files (pct 0-99): a new set, or a mod update's new set.
std::string making_line(int pct);    // "Making the game files from your Twilight Princess HD (42%)…"
std::string updating_line(int pct);  // "Updating the game files (42%)…"

// Toasts (plan 1.2: at most one per state). RML: "&gt;" shows as ">".
inline constexpr const char* kToastSetup = "HD Features: setting up (one time only).";
inline constexpr const char* kToastReady = "HD Features is ready.";
inline constexpr const char* kToastNeedFolder = "HD Features: choose your Twilight Princess HD folder in Mods.";
inline constexpr const char* kToastUpdateNeedFolder = kLineUpdateNeedFolder;
// A run the mod started itself that failed, and (wave 4 review M1) a start that can't set up at all (another
// Dusklight build, another disc, ...) or that waits for "Try again" after a crash: the panel line says why.
inline constexpr const char* kToastProblem = "HD Features couldn't finish setting up: see Mods &gt; HD Features.";
inline constexpr const char* kToastRestart = "HD Features: restart Dusklight to finish setting up.";

// The panel line for a run that ended with worker::Outcome::Problem (or None): r.code with r.detail,
// r.candidate_kind / r.candidate_problem (detection) and r.reading_hd / r.picked.
std::string line_for(const worker::Result& r);

}  // namespace hdfiles::setup
