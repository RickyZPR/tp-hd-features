#pragma once

// Converted HD stamp-book assets (tools/pipeline/stampbook.py, git-ignored res/stamps/stampbook/):
//   stampResR_gc.arc       HD's four zelda_stamps_*.blo byte-identical + GC BTIs (RARC, uncompressed,
//                          32-byte aligned members, lowercase names)
//   stampicon.bti          Collection slot icon (clctresR's stampicon, IA8 128x128)
//   stampbutton.bti        file-select plate (fileSel.arc stamps_i)
//   stampbuticon.bti       file-select "+" glyph (stamps_p)
//   filesel_stampicon.bti  file-select plate icon (stamps_1)
//
// Everything is loaded once at mod init into 32-byte-aligned, writable buffers owned by this module
// for the mod's lifetime (JKRMemArchive::open writes the entries' index field in place; Aurora keys
// GPU textures by texel pointer, so the addresses must never be reused while the game runs). The
// archive is never copied into a JKR heap: it is wrapped in a JKRMemArchive only while the book is
// open (mount/unmount), because a mounted archive sits at the front of the global volume list.

#include <cstddef>
#include <cstdint>

struct ResTIMG;
class JKRArchive;

namespace hdstamps::book::assets {

// Loads and validates every file. Returns true when the archive is usable (the loose textures are
// optional and reported separately). Safe to call once; logs every problem.
bool load();
// Forgets the buffers without freeing them (texel addresses must never be reused while the game
// runs; see book_assets.cpp). The book must be closed.
void unload();

bool archive_ready();
size_t archive_size();
// Human-readable reason when archive_ready() is false ("" otherwise).
const char* archive_problem();

// Loose textures; nullptr when missing or invalid.
const ResTIMG* slot_icon();     // stampicon.bti
const ResTIMG* filesel_plate(); // stampbutton.bti
const ResTIMG* filesel_plus();  // stampbuticon.bti
const ResTIMG* filesel_icon();  // filesel_stampicon.bti

// JKRMemArchive(buffer, size, JKRMEMBREAK_FLAG_UNKNOWN0) on the archive heap (the flag never frees
// the buffer). nullptr when the archive is not ready or the mount failed.
JKRArchive* mount();
// Unmounts (and deletes) an archive returned by mount(); resets the pointer.
void unmount(JKRArchive*& archive);

} // namespace hdstamps::book::assets
