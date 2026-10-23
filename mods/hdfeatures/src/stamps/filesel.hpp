#pragma once

// File select: HD's Quest Log stamp plate (fileSel.arc zelda_file_select_details.blo, group
// "stamps_n" under menu_n: plate, stamp icon, "N/50", "+" glyph) for the file whose details are
// shown, read from that slot's own "stamps" blob (SaveService::peek_blob). HD also moved the 3D
// model base (mset_p_n) and kamen_n to make room; the same moves are applied.
//
// HD's read-only viewer (book mode 1, HD file-select state 0x2C): in the file's Start/Copy/Delete
// menu, + (Start) with at least 1 stamp plays 0xAA, fades out and opens the book on that file's own
// stamps; B fades back to the menu. It runs from a skip-pre-hook on dFile_select_c::menuSelect (the
// file select's state while it is open), draws instead of dFile_select_c::_draw, and is built on
// its own 512 KB child heap of the game heap. Option "file_select_viewer" (default on); the "+" is
// only shown while the viewer can open.

namespace hdstamps::book::filesel {

// Returns how many hooks failed.
int install();
void shutdown();
void update();
// Save slots changed (cached counts are re-read).
void invalidate();
// The viewer hooks are installed and the book, the plate and the option allow it.
bool viewer_ready();
// The viewer is open or fading (the HD Features switch waits for it to close).
bool viewer_open();

} // namespace hdstamps::book::filesel
