#pragma once

// The stamp slot on GC's Collection screen (dMenu_Collect2D_c), HD's "stam_7_n".
//
// HD added the slot at its grid cell (4,3), right of Letters in its re-laid-out Collection screen
// ([5][9] tag table at Zelda.rpx 0x10093e20). GC's grid (getItemTag, [y 0-5][x 0-6]) has the free
// cell (4,4) right of Letters (3,4); the slot goes there with GC slot metrics (Letters + 47 px,
// GC's Fishing -> Letters pitch). Everything the Collection screen does per cell (cursor, select
// cursor, mouse hover) is generic over getItemTag, so a post-hook on getItemTag plus the panes is
// enough; the rest mirrors HD:
//   - icon visible only with >= 1 stamp (HD isStampIconVisible), the empty frame always
//   - 2-digit counter (tens only from 10), gold tint (HD material C1 190,190,130)
//   - name 2800, description 2801 with "%d/%d" = (owned, 50); A = "Open" (1006) with >= 1 stamp
//   - A opens the book (HD: set2DVibration, no sound; SUB_IN comes with the transition)
//   - cursor movement from the slot as HD's cursorMove (0x029b76f4): Up -> the nearest cell of
//     the row above (HD Scent, GC Hidden Skills (3,3)), Down -> Save, Left -> Letters, Right ->
//     no move; into the slot: Right from Letters only (GC's own algorithm, as in HD). GC's
//     "Up from Save/Options returns to the previous cell" rule would re-enter the slot after Down
//     from it; HD has no such rule, so that move goes where GC goes without a previous cell.
//   - the item description moves right by one slot pitch and shrinks to fit, as HD did

namespace hdstamps::book::slot {

// Installs the dMenu_Collect2D_c hooks. Returns how many failed (the slot is only added when
// every required one is installed).
int install();
bool hooks_ok();
int hooks_installed();
int hooks_wanted();
void shutdown();

} // namespace hdstamps::book::slot
