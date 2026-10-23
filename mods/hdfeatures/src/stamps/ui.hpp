#pragma once

// RmlUi side of the stamps: this feature's part of the HD Features Mods panel (status, options,
// diagnostics) and the old two-tab stamp list (10x5 grid, per-stamp checklist with hints) as tabs of the
// HD Features window, plus RML helpers for toasts. ../panel.cpp owns the panel, the menu-bar tab and the
// window and calls the functions below.
//
// The stamp list is not HD (HD's book is the Collection-screen slot, book.hpp). It is only offered when
// the HD book is unavailable (then the HD Features menu-bar tab carries it) and with the
// "stamps-debug_stamp_list" option.

#include <mods/api.h>

#include <cstdint>
#include <string>

namespace hdstamps::ui {

// Checks the bundled stamp pictures. The feature's first start (feature.cpp).
ModResult initialize();
void shutdown();
void update();

// Marks cached UI text as stale (new stamp, slot change, ...).
void notify_state_changed();

// Opens the HD Features window with the stamp list (first), even while HD's book is in use: the panel's
// button and the fallback when the book could not open (book_host.cpp). The list stays in the window
// until it is closed.
void open_stamp_book();
// True while the stamp list is offered (feature running, and HD's book unavailable or the debug option).
bool stamp_list_offered();
// True while the stamp list can be shown at all (the feature started and its switch is on).
bool stamp_list_available();

// Mods panel part: the sections "Stamps", "Stamps: where to find them" and "Stamps: options".
// build_panel resets the element handles, so call it before build_troubleshooting.
void build_panel(std::uint64_t pane);
// Two lines for the panel's shared Troubleshooting section.
void build_troubleshooting(std::uint64_t pane);
void update_panel();

// The stamp list's tabs of the HD Features window.
const char* window_rcss();          // scoped to the hds-* classes; the window's UiWindowDesc::rcss
const char* collection_tab_title(); // HD's "Stamp Collection" (2800) when HD's text is bundled
ModResult build_collection(ModContext* ctx, std::uint64_t window, std::uint64_t left, std::uint64_t right,
    void* userData, ModError* error);
ModResult update_collection(ModContext* ctx, void* userData, ModError* error);
ModResult build_checklist(ModContext* ctx, std::uint64_t window, std::uint64_t left, std::uint64_t right,
    void* userData, ModError* error);
// The window was closed or rebuilt: drop the tabs' element handles.
void window_closed();

// <img> for res/stamps/stamps/stampNN.png sized in dp, or an empty string if the PNG is not bundled.
std::string stamp_image_rml(int index, int sizeDp, bool dim);
std::string escape_rml(const char* text);
bool stamp_png_available(int index);

} // namespace hdstamps::ui
