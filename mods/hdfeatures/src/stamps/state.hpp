#pragma once

// Per-save stamp ownership, stored only in a SaveService blob (never in the .gci).

#include "stamp_data.hpp"

#include <mods/svc/config.h>

#include <cstdint>

namespace hdstamps {

// Blob "stamps" (DESIGN.md Mod 1): {u32 version = 1; u32 reserved; u64 owned;}, little-endian
// host layout. Readers accept any blob of at least this size with version >= 1 (future versions
// may append fields); anything else is treated as "no stamps yet".
struct StampBlobV1 {
    uint32_t version;
    uint32_t reserved;
    uint64_t owned;
};
static_assert(sizeof(StampBlobV1) == 16);

inline constexpr const char* kBlobName = "stamps";
inline constexpr uint32_t kBlobVersion = 1;

enum class GrantKind {
    Game,     // received in game (chest, NPC, shop): toast unless disabled
    Recovery, // retroactive award on load: summarized by the caller
};

// True while the mod should behave: the HD Features "Stamps" switch is on and the vanilla game mode
// is selected (no randomizer or other mode's save file). Updated every frame.
bool mod_active();
// The "Stamps" switch as applied by ../mod.cpp (default on). Off makes mod_active() false: the item
// ids go back to the game, the stage edits and Agitha's flow patch are removed, and the Collection
// slot / file-select plate are not added to the next screens, exactly as in another game mode.
void set_feature_enabled(bool enabled);
bool feature_enabled();
// Re-evaluates mod_active(); returns true if it changed this call.
bool refresh_active_state();
// True when a save slot is loaded and our blob store is usable.
bool slot_ready();

uint64_t owned_mask();
bool is_owned(int index);
int owned_count();

// Marks a stamp as owned, persists the blob and applies the Twili Midna rule. Returns true if
// the stamp (or Twili Midna) was newly added.
bool grant(int index, GrantKind kind);

// True once Agitha has 12 golden bugs (HD's condition for her stamp).
bool agitha_stamp_earned();

// Recomputes the retroactive awards (opened stamp chests, Malo Mart switch, Agitha hand-ins) and
// grants the missing ones silently. Returns the bits that were newly added.
uint64_t run_recovery();

// SaveService observers.
void on_new_save(uint32_t slot);
void on_save_loaded(uint32_t slot);
void on_save_written(uint32_t slot);

// Called once per frame by mod_update.
void state_update();

// Tries to pick up an already-loaded save (mod enabled or reloaded mid-game).
void attach_to_current_slot();

// Stamps of save slot `slot` (0-2) as they will be after that slot is loaded: its own blob
// (SaveService::peek_blob), minus the stamps the reload rule takes back (collected in play since
// that slot was last saved). -1 when the slot has no stamp blob. For the file-select counter.
int stored_stamp_count(uint32_t slot);
// The same stamps as a mask (bit i = stamp i). False (mask 0) when the slot has no stamp blob. For
// the file-select viewer (book mode 1).
bool stored_stamp_mask(uint32_t slot, uint64_t& outMask);

// Config: show a toast for every new stamp (default on).
bool toasts_enabled();
void set_toasts_enabled(bool enabled);
void register_config();
// 0 when ConfigService registration failed.
ConfigVarHandle toast_config_var();

// Toast helpers (no-ops when the UI is unavailable, or when toasts are disabled unless `force`:
// the Twili Midna notice that stands in for HD's reward sequence when the book cannot show it).
void toast_stamp(int index, bool force = false);
void toast_text(const char* title, const char* body, bool force = false);

} // namespace hdstamps
