#pragma once

// HD Features / Stamps: the feature's lifecycle, driven by ../mod.cpp.

namespace hdstamps {

// Registers the feature's options (config vars stamps-*). Once, at mod load, whether or not the
// "Stamps" switch is on, so the Mods panel can always show them.
void feature_register_config();
// Starts the feature (item tables, resolvers, hooks, the HD stamp book, save observer) the first
// time the "Stamps" switch is on. Never fails the mod: every missing piece is logged and skipped.
// Called with the switch state still as it was (on at load; off when the switch is turned on later):
// ../mod.cpp start_stamps then calls set_feature_enabled(true), which applies the item table if it is
// not applied yet. Either order works (refresh_active_state only acts on a change).
void feature_initialize();
bool feature_started();
// Per frame (mod_update). Runs while the switch is off too: everything is gated by mod_active().
void feature_update();
// True while the stamp book is open or in a transition (pause menu or file-select viewer): the HD
// Features switch waits for it, so the book is never cut off mid-way.
bool feature_busy();
// Why switching the feature off has to wait (null when it need not): a stamp is being handed over, or
// Link is in a stage with one of HD's extra chests (items.hpp). The text completes "Stamps turn off
// once ..." for the panel.
const char* feature_off_blocked();
// mod_shutdown only.
void feature_shutdown();

} // namespace hdstamps
