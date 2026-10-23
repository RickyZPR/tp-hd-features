#pragma once

// Hosting of the stamp book inside the pause-menu window (dMw_c), mirroring how TP HD hosts
// dMenu_Stamp_c (its states 0x21 STAMP_OPEN / 0x22 STAMP_MOVE / 0x23 STAMP_CLOSE) with GC's own
// sub-window idioms, without adding dMw_c states (the state tables are fixed game data):
//
// - Collection sub-window (HD mode 0): runs from a skip-pre-hook on dMw_c::collect_move_proc,
//   like GC's Letter/Fishing hosting: SUB_IN + 13-frame fade, field_0x144 = 10 / window status
//   10, delete the Collection screen, book, SUB_OUT + fade, field_0x144 = 3 / status 3,
//   re-create the Collection screen (cursor restored to the stamp slot).
// - Automatic opening from the field (HD modes 2/3): armed when the item-get window of a stamp
//   is destroyed with 1 or >= 49 stamps owned; requested from a pre-hook on dMw_c::key_wait_proc
//   in the same frame (pause status 8 = GC's own field -> sub-window path, Agitha's) under HD's
//   gates (those of Start: map status 0/1, window accept 3, no stage change pending, plus no
//   event/message) and reset at once when key_wait_proc does not take it; hosted on the
//   INSECT_AGITHA_* states while this mod owns them.
// - If the menu window ever returns to key_wait_proc while a book state is still owned (another
//   mod changed mMenuProc; key_wait_init wipes the 2D heap), the book is abandoned without
//   destructors instead of being deleted twice.

#include <cstdint>

class dMenu_Collect2D_c;

namespace hdstamps::book::host {

// Installs every dMw_c hook. Returns how many failed.
int install();
// The Collection-hosted book works (collect_move_proc, _draw and _delete hooks).
bool hooks_ok();
// Every hook of the automatic opening (key_wait_proc, the INSECT_AGITHA_* states, the item-get
// window destructor) is installed as well.
bool auto_open_hooks_ok();
int hooks_installed();
int hooks_wanted();

// Closes the book if open and leaves the menu window in a state the unhooked game handles.
void shutdown();

// Collection slot: A (or a mouse click) on the stamp slot of `screen`. Returns false when a
// transition or the book is already running.
bool request_open_from_collection(dMenu_Collect2D_c* screen);
// True while the book is open or any of its transitions runs.
bool busy();

// Drops an armed automatic opening (save loaded, new save, mode change).
void disarm();

} // namespace hdstamps::book::host
