#pragma once

// HD Features: setting up the game files on the player's PC (module R2; plan 1.2, 1.3, 2.3 with Corrections
// #1c, #4, #5, #7, #8, #9, #14, #15, #16, #19; wave3_R2_verify F2, F3, F6).
//
// The state machine on the game thread. It decides whether the stored files (hdfiles/files.hpp, R1) can be used,
// and when they can't, it reads the 13 GameCube inputs, runs the worker (hdfiles/worker.hpp) and hands its
// result to R1 (hdfiles::adopt). It owns the "Game files" line of the Mods panel (panel_files.cpp), the folder
// picker and the troubleshooting requests. The features wait (mod.cpp: hdfiles::ready()) until the files are
// live, then start through the existing switch machinery.
//
// Called from mod.cpp:
//   register_config()  mod_initialize, first;
//   initialize()       mod_initialize, before the features (true: the features may start at load as before);
//   tick()             every mod_update, right after hdfiles::update(), before the switches;
//   stop_worker()      mod_shutdown, first (stop + join, Correction #4);
//   shutdown()         mod_shutdown, last (a delete asked for in Troubleshooting, Correction #14).
//
// Config vars (mod.local_ricky_hdfeatures.<name>):
//   files-hd_folder                 STRING  the folder chosen with "Choose your Twilight Princess HD folder"
//   files-ignore_dusklight_version  BOOL    hidden: skip the Dusklight version check (testing, Correction #8)
//   files-test                      STRING  hidden: developer test steps, '+' separated: "no_autodetect" (only the
//                                           chosen / remembered folder is searched), "pick@<n>", "try_again@<n>",
//                                           "remake@<n>", "delete@<n>" (press that button at mod_update n; with
//                                           --cvar file.pickerOverride=<folder> the pick needs no dialog),
//                                           "dusklight=<version>" (check the version as if Dusklight said this)
//
// Std-only header. Game thread only.

#include <string>

namespace hdfiles::setup {

void register_config();
bool initialize();
void tick();
void stop_worker();
void shutdown();

// --- the "Game files" section of the Mods panel (panel_files.cpp) -------------------------------------------
// The one sentence for the current state (plan 1.2 with Corrections #7, #8, #1c, #14, #15).
std::string line();
// 0..1 while the files are being made, below 0 otherwise.
float progress();
// The folder button and "Try again" are shown while the files are not ready and setup can still make them; they
// stay in place (disabled when they don't apply) during a run, so a focused button never disappears.
bool show_setup_buttons();
// The folder button: as above, and also while the files are ready after "Make the game files again" found no
// usable TP HD (the kept files are in use; a pick asks for the remake again at the next start).
bool show_folder_button();
bool can_choose_folder();
bool can_try_again();
// "Make the game files again" / "Delete the game files": shown while the files are ready, enabled until one of
// them was pressed (disabled, not hidden: hiding the pressed button made the Mods window jump, wave3_R2_verify F2).
bool show_manage_buttons();
bool can_manage();
// The buttons.
void choose_folder();
void try_again();
void request_remake();
void request_delete();

}  // namespace hdfiles::setup
