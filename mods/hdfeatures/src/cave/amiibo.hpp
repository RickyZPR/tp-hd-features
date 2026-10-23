#pragma once

// Cave of Shadows: the virtual Wolf Link amiibo's screens, drawn with RmlUi through UiService.
//
//  - Collection-screen prompt: a toast restyled (UI_SCOPE_OVERLAY sheet, scoped to this mod's toasts)
//    as HD's amiibo button prompt: the Wolf Link figure, the "amiibo" logo and the button to press. It is
//    only visible while the Collection screen is up (a second overlay sheet hides it otherwise).
//  - Scan screen: a modal dialog whose body recreates HD's zelda_amiibo_progress popup (parchment,
//    NFC glow, figure, message text) from res/cave/amiibo (layout.json coordinates scaled to the dialog).
//    While it is open the host blocks game input; the pause menu stays open underneath.
//  - Record: RML blocks for the run summary toast and the mod panel ("Cave of Shadows Record").
// Without res/cave/amiibo the same screens use plain text (no art).

#include "cave.hpp"

#include <mods/svc/ui.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace cave::amiibo {

ModResult initialize();
void shutdown();

bool art_available();

// Every toast of the cave goes through here: the host shows toasts one at a time in arrival order
// (dusk/ui/overlay.cpp), so the mod keeps an estimate of when its own queued toasts are done.
bool push_toast(const UiToastDesc& desc);
// A toast the rest of the mod pushed (the stamps' notices, via hdfeatures::note_toast): it is in the
// same queue, so the estimate counts it too.
void note_other_toast(std::uint32_t durationMs);

// Collection-screen prompt (`buttonLabel` e.g. "Z"). FIX (review "cave entry" #2): pushed only when none
// of the mod's own toasts is still queued or showing (prompt_ready), so prompts never pile up behind each
// other or behind the record/wallet notices; returns false when nothing was pushed.
bool prompt_ready();
bool show_prompt(const char* buttonLabel);
// True while the last prompt is (by the estimate) still queued or on screen.
bool prompt_pending();
// The prompt may only be seen while the Collection screen is up and idle (no scan screen, no sub-window):
// when `visible` turns false, an overlay sheet fades a still-living prompt toast out (toasts cannot be
// closed early), so it never lingers over gameplay or the warp-out. Cheap to call every tick.
void set_prompt_visible(bool visible);

// Opens the scan screen; false if the dialog could not be shown (the caller then decides at once).
bool open_scan(ScanMode mode);
bool scan_open();
// Advance the scan screen's timing (call once per game tick).
void tick();
// Close it programmatically (no result callback).
void cancel();

// RML helpers.
std::string escape(std::string_view text);
// HD message text -> RML paragraph(s): single line breaks become spaces, blank lines new paragraphs.
std::string paragraphs(std::string_view plain);
// <img> for a res/cave/amiibo file, or "" when the art is missing.
std::string image(const char* file, const char* cssClass);

}  // namespace cave::amiibo
