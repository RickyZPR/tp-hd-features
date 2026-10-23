#pragma once

// Cave of Shadows: TP HD's "Cave of Shadows Record" screen, shown after an exit portal's warp-in cutscene.
//
// In HD (Zelda.rpx, research/cos_portal_spec.md §1.7-1.8 + this pass's disassembly, VERIFIED unless noted):
//  - the l8warp's SCENE_CHG sets dMw+0x166; the pause-menu window's idle state (0x02a19214) then switches to
//    state 0x2F (init 0x02a17bd8: window status 10, SE 0xA8 Z2SE_SY_FILE_MENU_SLIDE_IN, fade out, menu capture,
//    dMenu_save_c use type 7), whose _open case 7 (0x02a06288) sets header 1258, A hint 1061 "Skip", no B hint,
//    the plate + record panel animations 4020->4030, dFile_amiibo_c::setData(figure, 1) and the record panel
//    dLos_record_c (0x029a17d8 screen setup, 0x029a275c values) on losrpt_n with zelda_file_select_los_record.blo;
//  - proc 0x41 waits for the animations; proc 0x42 runs the record's reveal (0x029a29d8: hearts from the figure's
//    to the current life, the heart difference, "NEW Record!", the amiibo rows, then time / max combo / damage
//    counting up from the previous record, improvements in gold FFE061; A skips) and then writes the record into
//    the save (0xA98..0xAA4) and asks message 1261 (store in the figure?) when the life is
//    higher than the figure's (0x029a202c), else shows "Close" (1263);
//  - Yes (0x47): the figure gets the record's floor / life / max life, NFC popup 1567, then 1568 (first time,
//    save byte 0xABB) or 1569; No (0x46/0x4a): 1262 (leave without saving?) Yes/No (No or B
//    goes back to 1261); every end saves the game (0x02a08494) and calls dComIfGs_gameStart (0x029076e4).
//
// Here the same screen is a native J2D screen (like confirm.cpp, from the same converted saveres archive plus
// zelda_file_select_los_record.blo), hosted in the pause-menu window from a skip pre-hook on its idle process
// dMw_c::key_wait_proc. The game save is the game's own pause-menu save screen (log list, Saving..., HD's
// "Saved." ending; DEVIATION: HD writes the current log without a list, but Dusklight's save service only stores
// the mod's "cave" blob with a save made through that screen), after which the menu closes and cave.cpp sends
// Link to the save's return place (HD dComIfGs_gameStart).

#include "cave.hpp"
#include "confirm.hpp"

class dMenu_save_c;

namespace cave::record {

struct Input {
    RecordValues cur;               // this run (HD proc 0x42 values)
    bool hasPrev = false;           // HD: the save's previous record exists (0xA9F > 0)
    RecordValues prev;              // the previous record (shown first, counted up from)
    confirm::AmiiboData figure;     // the virtual Wolf Link amiibo as stored now (rec+0x28/0x2c/0x30)
};

// Hooks (dMw_c::key_wait_proc / _draw / _delete / collect_save_close_proc); failures only make available()
// false.
void install_hooks();
void uninstall_hooks();
// mod_shutdown: drop the screen and give the pause-menu window back to the game.
void shutdown();

// The screen can be hosted (its three dMw_c hooks are installed and the archive is not known to lack it).
bool available();
// Portal SCENE_CHG (HD dMw+0x166): open the screen at the window's next idle frame. False when it cannot.
bool request(const Input& input);
// Requested, on screen, or in the save that follows it (until cave.cpp's return has been issued).
bool busy();
// Once per mod_update.
void tick();

// confirm.cpp's dMenu_save_c hooks: the save menu the Record screen handed over to (HD's "Saved." ending).
bool save_menu(const dMenu_save_c* menu);
// cave.cpp's save observer: that save menu wrote its slot.
void on_slot_written();

// cave.cpp: the virtual amiibo's write popup closed (`written` = the figure was written).
void amiibo_closed(bool written);

}  // namespace cave::record
