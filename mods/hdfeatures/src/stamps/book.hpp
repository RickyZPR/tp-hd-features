#pragma once

// HD stamp collection screen ("stamp book"): Collection-screen slot, J2D book, hosting and the
// automatic openings after the 1st and 49th stamp. Spec: research/stampbook_spec.md with the
// mandatory amendments of research/stampbook/check.md.
//
//   book_assets  converted HD archive + loose BTIs (res/stamps/stampbook/)
//   stamp_book   the book itself (port of HD's dMenu_Stamp_c: 4 HD layouts, cursor, pages, reward)
//   book_host    dMw_c hosting: Collection sub-window (mode 0), direct open from the field (2/3),
//                arming in the item-get window destructor, draw, teardown
//   collect_slot Collection screen: 5th book slot "stam_7_n" right of Letters, counter digits,
//                HD name/description, A label, cursor movement to/from the slot
//   filesel      file-select Quest Log plate "stamps_n" with the N/50 counter, and the read-only
//                viewer (book mode 1) behind its "+"
//
// The old RmlUi stamp list (ui.cpp) stays as the fallback when any of this is unavailable and as a
// debug option.

#include <mods/api.h>
#include <mods/svc/config.h>

#include <cstdint>
#include <string>

namespace hdstamps::book {

enum class PostButton : int {
    Hd = 0,     // HD: visible, reachable with Down from row 1, A on it is silent (Miiverse is gone)
    Greyed = 1, // visible but greyed out and not selectable
    Hidden = 2, // not shown
};

enum class MenuScaling : uint8_t {
    GameCube = 0, // 4:3 canvas, black side bars
    Wii = 1,      // canvas stretched, "n_43" panes counter-scaled (what HD does at 16:9)
    Dusklight = 2,
};

// --- config (defaults reproduce HD) ------------------------------------------------------------
void register_config();
bool collection_slot_enabled(); // "collection_slot", default on
bool auto_open_enabled();       // "book_auto_open", default on (HD opens the book after the 1st and 49th stamp)
PostButton post_button();       // "book_post_button", 0 = HD (default), 1 = greyed, 2 = hidden
bool debug_stamp_list();        // "debug_stamp_list": the old RmlUi list as a menu-bar tab, default off
bool filesel_counter_enabled(); // "file_select_counter", default on
bool filesel_viewer_enabled();  // "file_select_viewer": + on the file select opens the book read-only (HD), default on
// 0 when the option could not be registered.
ConfigVarHandle slot_config_var();
ConfigVarHandle auto_open_config_var();
ConfigVarHandle post_button_config_var();
ConfigVarHandle debug_list_config_var();
ConfigVarHandle filesel_config_var();
ConfigVarHandle viewer_config_var();

// Dusklight's menu scaling setting is not exported to mods; the Collection screen's own layout
// (menuCollectWide) and the file select's (fileSelectWide) reveal it, so the book follows the mode
// last seen there. It is kept in the hidden option "last_menu_scaling" (default Wii) across
// sessions.
MenuScaling menu_scaling();
void set_menu_scaling(MenuScaling mode);
// File select: true when its screens are stretched (Wii or Dusklight scaling).
void note_wide_menus(bool wide);

// --- lifecycle --------------------------------------------------------------------------------
// Call after items::initialize() (HD text) and state. Never fails the mod: every missing piece is
// logged and the RmlUi fallback takes over.
ModResult initialize();
void shutdown();
void update();

// --- status ---------------------------------------------------------------------------------
bool book_available();            // the J2D book can be opened (archive + host hooks)
bool slot_available();            // the Collection slot is injected (slot hooks + icon + config)
bool integrated();                // slot_available() && book_available()
bool reward_sequence_available(); // the 49-stamp book sequence replaces the Twili Midna toast
std::string diagnostics();

// Save loaded / new save / game mode changed: drop any armed automatic opening.
void on_slot_changed();

} // namespace hdstamps::book
