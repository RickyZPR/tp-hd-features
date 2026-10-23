#pragma once

// HD Features (local.ricky.hdfeatures): Twilight Princess HD's stamps and Cave of Shadows as one
// Dusklight mod. Shared by the two features (src/stamps, namespace hdstamps; src/cave, namespace
// cave) and the mod's own glue (src/mod.cpp lifecycle and feature switches, src/panel.cpp Mods
// panel, menu-bar tab and window).

#include <mods/api.h>
#include <mods/svc/config.h>

#include <cstdint>
#include <string_view>

#ifndef HDFEATURES_MOD_ID
#define HDFEATURES_MOD_ID "local.ricky.hdfeatures"  // mod.json.in "id" (set by CMakeLists.txt)
#endif

namespace hdfeatures {

// The mod id: config keys (mod.local_ricky_hdfeatures.<name>), the mod save file
// (<card>/...gczelda2.mods/local.ricky.hdfeatures.json), mod:// image URLs and the mod-id
// attribute that scoped RCSS selects on.
inline constexpr const char* kModId = HDFEATURES_MOD_ID;
static_assert(std::string_view(HDFEATURES_MOD_ID) == "local.ricky.hdfeatures",
    "the RCSS selectors in src/cave/amiibo.cpp spell the mod id out");

// The About line of the Mods panel and of the cave's window tab (the project's disclaimer, short form).
inline constexpr const char* kAboutText =
    "Unofficial fan mod, not affiliated with the Dusklight team: please don't ask them for help with it. "
    "For help, see the tp-hd-features page on GitHub. "
    "Its game files are made on your PC from your own copies of the games.";

// --- feature switches (src/mod.cpp) ----------------------------------------------------------
// The "Stamps" and "Cave of Shadows" toggles (config vars stamps-enabled / cave-enabled, both on
// by default). A change is applied at the next safe moment of mod_update (held for a third of a
// second): on the title screen and the file select, or in free play once no menu, event, message or
// scene change is in progress (and, for the stamps, no stamp book is open; turning them off also waits
// until no stamp is being handed over and Link is not in a stage with one of HD's extra chests; for the
// cave, nobody is inside it: a player in the cave is sent back to the return point first).
enum class Feature { Stamps, Cave };
// What the toggle says now.
bool feature_wanted(Feature feature);
// Whether the feature is running (the toggle's value once it has been applied).
bool feature_running(Feature feature);
// The toggle changed and is waiting for a safe moment.
bool feature_pending(Feature feature);
// The feature could not start (the cave's save observer failed): the rest of the mod still runs.
bool feature_failed(Feature feature);
// The toggle is on, but the feature has not started yet because the game files are not ready (hdfiles::ready()):
// its panel lines say it waits for them, not that it is off (wave3 F5, L5).
bool feature_waits_for_files(Feature feature);
// Config var handles of the two toggles (0 when registration failed).
ConfigVarHandle feature_config_var(Feature feature);
// While a change is pending: what else it waits for besides "no menu, cutscene or loading", as a phrase
// that completes "Stamps turn off once ..." (e.g. "the stamp book is closed"), or null.
const char* feature_wait_reason(Feature feature);

// --- toasts -------------------------------------------------------------------------------
// The host shows every toast of the mod in one queue, one at a time (dusk/ui/overlay.cpp). The cave
// keeps an estimate of when that queue is empty (its amiibo prompt waits for it, cave/amiibo.cpp);
// every toast pushed outside cave/amiibo.cpp reports here after a successful push_toast.
void note_toast(std::uint32_t durationMs);

// --- the one Mods panel, menu-bar tab "HD Features" and its window (src/panel.cpp) -------------
// Registers the Mods panel (once, at mod load).
void panel_initialize();
// Per frame from mod_update: keeps the menu tab in step with the features (cheap).
void panel_update();
void panel_shutdown();
// The window has a "Cave of Shadows" tab while the cave runs and the stamp list's two tabs while the
// stamp list is offered (HD's book unavailable, or its debug option). `first` picks which comes
// first (the window opens on its first tab). `forceStamps`: include the stamp list even while HD's book
// is in use (the book could not open, stamps/book_host.cpp, or the panel's button); it stays in the
// window until the window closes. An open window that already has the wanted tabs is left as it is;
// one that lacks them is rebuilt. Returns true when the window is open afterwards.
bool open_window(Feature first, bool forceStamps = false);
void close_window();
// Called after a feature started or stopped: the menu tab is added or removed and an open window
// that no longer matches is closed.
void refresh_menu();

}  // namespace hdfeatures
