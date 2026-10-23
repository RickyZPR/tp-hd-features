#pragma once

// Cave of Shadows: TP HD's pre-entry screen ("Cave of Shadows" / Save and begin / Cancel), shown after a
// successful virtual Wolf Link amiibo scan on the pause menu's Collection screen.
//
// In HD this screen is the save menu itself (dMenu_save_c use type 6, Layout/saveres.arc): the title bar,
// the gold plate with the amiibo's stored hearts and "Level B<n>" (dFile_amiibo_c on the info screen
// zelda_file_select_info_amiibo.blo), the Confirm/Back hints and the two buttons. "Save and begin" runs
// the normal save (pick a Quest Log, overwrite?, Saving..., Saved.) and only then sends Link to D_SB11;
// Cancel and B go back to the Collection screen. Research: scratch cosconfirm/hdlayout + hdcode reports.
//
// Here:
//  - the screen is a native J2D screen built from HD's own three layouts and zelda_file_select2.bck,
//    converted for the GC renderer by tools/pipeline/cosconfirm.py (res/cave/confirm/cosconfirm_gc.arc,
//    git-ignored), mounted with JKRMemArchive from a 32-byte-aligned buffer like the stamp book;
//  - it is hosted in the pause menu window (dMw_c) from a skip pre-hook on dMw_c::collect_move_proc,
//    the stamp book's pattern (fade out, window status 10, the Collection screen deleted at black);
//  - "Save and begin" hands over at black to the game's own pause-menu save (dMw_c SAVE_OPEN, the same
//    dMenu_save_c the Collection screen's Save uses): the .gci is written by the stock save path only.
//    Two hooks on dMenu_save_c make its ending HD's ("Saved." instead of "Saved. Continue playing?"); the
//    trip starts only after SaveService reported the written slot. Then, as in HD, the stage change to the
//    cave is issued from the save menu, which stays on screen until the scene changes (a pre-hook on
//    dMw_c::collect_save_move_proc holds it and the dMw_c::_delete pre-hook lets it go; without those hooks the
//    menu closes and the trip follows).
// Without the archive (or the host hooks) the scan opens the stock save menu directly (no HD screen):
// the trip still never starts without a successful save.

#include <cstdint>

class JKRArchive;

namespace cave::confirm {

// The virtual amiibo's application data as HD stores it (dFile_amiibo_c, amiibo area +0x11..+0x13).
struct AmiiboData {
    int level = 0;    // floor the last stored run ended on; <= 0: no "Level" line
    int life = 0;     // quarter hearts; <= 0: no plate at all (HD's "no data" variant)
    int maxLife = 0;  // heart containers x 5 (heart pieces)
};

// Hooks (from hooks::install_all / uninstall_all). Failures are recorded with cave::record_hook_failure.
void install_hooks();
void uninstall_hooks();
// mod_shutdown: leaves the pause menu in a state the unhooked game handles.
void shutdown();

// Loads and validates res/cave/confirm/cosconfirm_gc.arc once (about 11 MB, kept for the process: the
// renderer caches textures by address). Cheap after the first call. True when the archive is usable.
// The screen calls it at black, when the Collection screen has faded out for it (the one-time read is
// hidden by the fade); a failure there opens the stock save menu instead.
bool preload();
// The HD screen can be shown (archive ready and the three dMw_c hooks installed).
bool screen_available();
// The two dMenu_save_c hooks are installed (HD's "Saved." ending; without them the stock "Saved.
// Continue playing?" question closes the menu instead).
bool save_hooks_ok();
// HD dMw state 0x2D (0x02a1a1dc): after the trip's "Saved." the save menu stays on screen, ignoring input, while
// the stage change to the cave runs (its end status is 2, which only state 0x2E acts on). True while held; the
// hold ends when the pause menu is deleted with the play scene.
bool save_held();
// The stage change did not start (cave.cpp tick_entry timeout): close the held menu as GC's "Continue playing? Yes"
// does (status 4), so the pending stage change can run once the game is unpaused. When the scene change's fade is
// already running (mod shutdown), the menu is parked instead (GC SAVE_OPEN with status 3 does nothing) and goes with
// the scene, so GC's close never fades the screen back in under the scene change.
void release_save_hold(const char* why);
// The pause menu is gone without the dMw_c::_delete pre-hook having seen it: drop the pointer without touching it.
void forget_save_hold(const char* why);
// One line for the troubleshooting text ("" when everything is fine).
const char* problem();

// The screens' archive, shared with the "Cave of Shadows Record" screen (record.cpp): mounted (JKRMemArchive on
// the archive heap) while a screen exists; null when preload() failed. The buffer is never freed.
JKRArchive* mount_screen_archive();
void unmount_screen_archive(JKRArchive*& archive);
// After preload(): the archive also holds zelda_file_select_los_record.blo (archives built before the Record
// screen do not).
bool archive_has_record();
// The Record screen cannot use the archive: preload() was tried and failed, or the archive lacks
// zelda_file_select_los_record.blo (built before the Record screen: make the game files again).
bool archive_known_bad();

// From the idle Collection screen (pause menu in COLLECT_MOVE): fade out and show the HD screen.
// False (nothing changed) when it cannot be shown right now.
bool open(const AmiiboData& data);
// Fallback without the HD screen: the Collection screen's own Save (sub-window 1). False when the
// Collection screen is not up.
bool open_save();
// True while the HD screen or one of its fades runs.
bool busy();
// The feature is being switched off: leave the screen as Cancel as soon as its buttons take input.
void request_cancel();
// Once per mod_update: notices when the pause menu left the screen's state from outside.
void tick();

// Music (HD state 0x2C: stream 0x0200002F while the screen and the save after it are up).
// The trip was cancelled after the screen had closed (the save menu went back to the Collection
// screen): stop the stream and give the menu its music back.
void bgm_restore();
// The trip is over (Link arrived, or it was refused after the save): stop the stream if it still runs.
void bgm_stop();

}  // namespace cave::confirm
