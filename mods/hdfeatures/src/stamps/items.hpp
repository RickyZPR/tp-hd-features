#pragma once

// Everything that turns the 50 NOENTRY item ids into working stamp items: item table patch,
// chest resolvers, HD-only chests, give observation, get-item text, icons and NPC/shop triggers.

#include <mods/api.h>

namespace hdstamps::items {

// Registers resolvers, the give observer, message overrides and hooks. Each piece that fails is
// logged and skipped; the mod keeps running with fewer features.
ModResult initialize();
// Restores dItem_data::item_resource and removes the stage edits.
void shutdown();
// Per-frame work: model archive detection, queued NPC/shop gives.
void update();

// Called by the state module.
void on_active_changed(bool active);
void on_slot_changed();
void on_stamp_owned(int index);

// True while a stamp is waiting to be handed over (an NPC or shop stamp queued for its get-item
// scene). Switching the feature off would drop it (and Gengle's is never recovered on load).
bool gives_in_flight();
// True while Link is in a stage that has one of HD's extra chests (StageService edits): such a chest
// may already be in a loaded room, and with the item ids handed back it would give the game's empty
// item. Switching off waits until the stage is left.
bool hd_chest_stage_loaded();

// Diagnostics for the UI.
bool model_archive_found();
// True when Obj_Stamp.arc exists but failed validation (no usable stamp.bmd).
bool model_archive_invalid();
bool tables_applied();
int custom_icon_count();
int hooks_installed();
int hooks_wanted();
// True when res/stamps/hdtext/messages.bin (HD's own text, made from the player's HD copy) loaded.
bool hd_text_loaded();
// True while HD's stamp lines are patched into Agitha's flow 1900.
bool agitha_dialogue_active();
// True when HD's text is loaded but Agitha's lines cannot be patched in (no FlowService, or a
// failure that was logged); she then hands over her stamp through the regular get-item scene.
bool agitha_dialogue_unavailable();

} // namespace hdstamps::items
