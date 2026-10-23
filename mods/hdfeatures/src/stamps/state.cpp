#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "state.hpp"

#include "book.hpp"
#include "hdtext.hpp"
#include "items.hpp"
#include "ui.hpp"

#include "hdfeatures.hpp"

#include <mods/svc/config.h>
#include <mods/svc/log.hpp>
#include <mods/svc/save.h>
#include <mods/svc/ui.h>

#include "d/actor/d_a_npc4.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "m_Do/m_Do_MemCard.h"

#include <fmt/format.h>

#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace hdstamps {
namespace {

constexpr int kSlotCount = 3;

uint64_t g_owned = 0;
bool g_slotReady = false;
bool g_active = false;
bool g_activeKnown = false;
// The HD Features "Stamps" switch (../mod.cpp): off gates the feature exactly like another game mode.
bool g_featureEnabled = true;
int g_slotProbeTimer = 0;
int g_maloPollTimer = 0;
int g_activePollTimer = 0;
ConfigVarHandle g_toastVar = 0;

// Save slot g_owned belongs to (0-2), or -1 when unknown (mod attached mid-game).
int g_slot = -1;

// HD keeps stamps in the save file, so a stamp collected after the last save is gone again after
// a reset. SaveService instead keeps its blob store in memory for the whole session (it is read
// from disk once per save name, svc/save.cpp load_save_store) and any later save of any slot
// flushes it. To match HD, stamps granted in play are remembered per slot until that slot is saved;
// if the slot is loaded again in the same session with its blob exactly as this mod left it, those
// stamps are taken back (chest stamps then come back when the chest is opened again; recovery
// re-adds anything the save itself supports). If the blob differs (slot copied over in file
// select), nothing is reverted.
struct SlotHistory {
    bool known = false;   // `stored` is valid
    uint64_t stored = 0;  // last value this mod wrote to (or read from) the slot's blob
    uint64_t unsaved = 0; // stamps granted in play since the slot was last saved or loaded
};
std::array<SlotHistory, kSlotCount> g_history{};

bool valid_slot(int slot) {
    return slot >= 0 && slot < kSlotCount;
}

// Save switch that HD sets when the Malo Mart stamp is bought (Castle Town, table 0x09).
constexpr int kMaloMartSaveTbl = 0x09;
constexpr int kMaloMartSwitch = 0x7F;
// Agitha's "golden bug handed in" labels (daNpcIns_c l_insectParams evt_bit_no, d_a_npc_ins.cpp).
constexpr uint32_t kFirstInsectLabel = 0x191;
constexpr uint32_t kLastInsectLabel = 0x1A8;
// HD's Agitha stamp is the reward for 12 golden bugs (research/final_web.md; HD zel_04 flows
// 1813/1814 branch on an HD-only query before setting the temp flag that flow 1900 turns into the
// stamp). A converted test save with 8 bugs handed in has no Agitha stamp in HD, which fits.
constexpr int kAgithaBugsNeeded = 12;

// The vanilla game mode's save name (dusk/game_mode.hpp kDefaultGameModeSaveName). Every other game
// mode has its own save name: the randomizer ("randomizer", reference/randomizer/src/session.cpp:515),
// Dusklight's built-in Speedrun mode ("gczelda2-speedrun", dusk/speedrun.cpp) and mods' modes such as
// dawnlight's Boss Rush ("gczelda2-bossrush"). HD Stamps only runs on vanilla saves.
constexpr char kVanillaSaveName[] = "gczelda2";

bool vanilla_save_selected() {
    const char* name = g_mDoMemCd_control.getFileName();
    return name != nullptr && std::strcmp(name, kVanillaSaveName) == 0;
}

uint64_t read_blob(bool& outFound) {
    outFound = false;
    size_t size = 0;
    if (svc_save->get_blob(mod_ctx, kBlobName, nullptr, &size) != MOD_OK) {
        return 0;
    }
    outFound = true;
    if (size < sizeof(StampBlobV1) || size > SAVE_BLOB_BUDGET_BYTES) {
        mods::log::warn("stamp blob has unexpected size {}; starting from an empty collection", size);
        return 0;
    }
    std::vector<uint8_t> buffer(size);
    size_t got = buffer.size();
    if (svc_save->get_blob(mod_ctx, kBlobName, buffer.data(), &got) != MOD_OK ||
        got < sizeof(StampBlobV1))
    {
        mods::log::warn("could not read the stamp blob; starting from an empty collection");
        return 0;
    }
    StampBlobV1 blob{};
    std::memcpy(&blob, buffer.data(), sizeof(blob));
    if (blob.version == 0) {
        mods::log::warn("stamp blob has version 0; ignoring it");
        return 0;
    }
    if (blob.version > kBlobVersion) {
        mods::log::info("stamp blob version {} is newer than {}; reading its v1 fields", blob.version,
            kBlobVersion);
    }
    return blob.owned & kAllStampsMask;
}

bool write_blob() {
    if (!g_slotReady) {
        return false;
    }
    const StampBlobV1 blob{kBlobVersion, 0, g_owned & kAllStampsMask};
    const ModResult result = svc_save->set_blob(mod_ctx, kBlobName, &blob, sizeof(blob));
    if (result != MOD_OK) {
        mods::log::warn("failed to store the stamp blob (result {})", static_cast<int>(result));
        if (result == MOD_UNAVAILABLE) {
            g_slotReady = false;
        }
        return false;
    }
    if (valid_slot(g_slot)) {
        g_history[g_slot].known = true;
        g_history[g_slot].stored = blob.owned;
    }
    return true;
}

// Twili Midna: HD's item functions set bit 49 once 49 stamps are owned.
bool apply_twili_rule() {
    if ((g_owned & kTwiliMidnaBit) != 0) {
        return false;
    }
    if (std::popcount(g_owned & (kAllStampsMask & ~kTwiliMidnaBit)) >= kStampCount - 1) {
        g_owned |= kTwiliMidnaBit;
        return true;
    }
    return false;
}

// Only dSv_save_c::mSave (the save data proper) is read here. The live per-stage copy
// (dSv_info_c::mMemory) and the current stage's STAG info are not consulted: outside the play
// scene they can be stale, and every chest opened while the mod was off reaches mSave as soon as
// its stage is left or the game is saved, so the next load recovers it anyway.
bool chest_opened(const StampDef& def) {
    return def.saveTbl >= 0 && def.tboxNo >= 0 && dComIfGs_isSaveTbox(def.saveTbl, def.tboxNo);
}

bool malo_mart_switch_set() {
    return dComIfGs_isSaveSwitch(kMaloMartSaveTbl, kMaloMartSwitch);
}

int golden_bugs_handed_in() {
    int count = 0;
    for (uint32_t label = kFirstInsectLabel; label <= kLastInsectLabel; ++label) {
        if (daNpcF_chkEvtBit(label)) {
            ++count;
        }
    }
    return count;
}

uint64_t compute_recovery_bits() {
    uint64_t bits = 0;
    for (const StampDef& def : kStamps) {
        if ((def.source == StampSource::Chest || def.source == StampSource::HdChest) &&
            chest_opened(def))
        {
            bits |= stamp_bit(def.index);
        }
    }
    if (malo_mart_switch_set()) {
        bits |= stamp_bit(kMaloMartStampIndex);
    }
    if (agitha_stamp_earned()) {
        bits |= stamp_bit(kAgithaStampIndex);
    }
    return bits;
}

std::string names_of(uint64_t bits, size_t maxNames) {
    std::string out;
    size_t shown = 0;
    size_t total = 0;
    for (int i = 0; i < kStampCount; ++i) {
        if ((bits & stamp_bit(i)) == 0) {
            continue;
        }
        ++total;
        if (shown < maxNames) {
            if (!out.empty()) {
                out += ", ";
            }
            out += hdtext::stamp_name(i); // HD's name when res/stamps/hdtext is bundled
            ++shown;
        }
    }
    if (total > shown) {
        out += fmt::format(" and {} more", total - shown);
    }
    return out;
}

void load_current_slot(int slot, const char* why) {
    bool found = false;
    uint64_t owned = read_blob(found);
    uint64_t reverted = 0;
    if (valid_slot(slot)) {
        SlotHistory& history = g_history[slot];
        if (history.known && history.unsaved != 0) {
            if (owned == history.stored) {
                reverted = owned & history.unsaved;
                owned &= ~history.unsaved;
            } else {
                mods::log::info("slot {} changed since this mod last wrote it; keeping its stamps", slot);
            }
        }
        history.unsaved = 0;
    }
    g_owned = owned;
    g_slot = slot;
    g_slotReady = true;
    items::on_slot_changed();
    book::on_slot_changed();

    if (!found || reverted != 0) {
        // Make sure the blob exists so slot_ready() can tell "no slot" from "no blob", and store
        // the reverted collection.
        write_blob();
    } else if (valid_slot(slot)) {
        g_history[slot].known = true;
        g_history[slot].stored = g_owned;
    }
    if (reverted != 0) {
        mods::log::info("slot {}: {} stamp(s) collected after the last save were taken back: {}", slot,
            std::popcount(reverted), names_of(reverted, 64));
    }

    const uint64_t recovered = run_recovery();
    mods::log::info("{}: {} stamp(s) owned{}", why, owned_count(),
        recovered != 0 ? fmt::format(" ({} recovered from the save)", std::popcount(recovered)) :
                         std::string{});
    if (recovered != 0) {
        const std::string body = fmt::format("Recovered {} stamp(s) from this save: {}. ({}/{})",
            std::popcount(recovered), ui::escape_rml(names_of(recovered, 6).c_str()), owned_count(),
            kStampCount);
        toast_text("Stamps", body.c_str());
    }
    ui::notify_state_changed();
}

} // namespace

bool mod_active() {
    return g_activeKnown && g_active;
}

bool agitha_stamp_earned() {
    return golden_bugs_handed_in() >= kAgithaBugsNeeded;
}

bool refresh_active_state() {
    const bool vanilla = vanilla_save_selected();
    const bool active = g_featureEnabled && vanilla;
    if (g_activeKnown && active == g_active) {
        return false;
    }
    g_activeKnown = true;
    g_active = active;
    if (active) {
        mods::log::info("vanilla save selected: stamps enabled");
    } else if (!g_featureEnabled) {
        // wave3 L5: at load without the game files the switch is on and the stamps only wait (../mod.cpp).
        mods::log::info("{}", hdfeatures::feature_waits_for_files(hdfeatures::Feature::Stamps) ?
                                  "stamps stay inactive until the game files are ready" :
                                  "the Stamps switch is off: stamps stay inactive");
        g_slotReady = false;
    } else {
        mods::log::info("non-vanilla game mode selected (save name '{}'): stamps stay inactive",
            g_mDoMemCd_control.getFileName() != nullptr ? g_mDoMemCd_control.getFileName() : "");
        g_slotReady = false;
    }
    items::on_active_changed(active);
    ui::notify_state_changed();
    return true;
}

bool slot_ready() {
    return mod_active() && g_slotReady;
}

void set_feature_enabled(bool enabled) {
    g_featureEnabled = enabled;
    if (!enabled) {
        g_slotProbeTimer = 0;
    }
    refresh_active_state();
}

bool feature_enabled() {
    return g_featureEnabled;
}

uint64_t owned_mask() {
    return g_owned & kAllStampsMask;
}

bool is_owned(int index) {
    return index >= 0 && index < kStampCount && (g_owned & stamp_bit(index)) != 0;
}

int owned_count() {
    return std::popcount(owned_mask());
}

bool grant(int index, GrantKind kind) {
    if (index < 0 || index >= kStampCount || !slot_ready()) {
        return false;
    }
    const uint64_t before = g_owned;
    const bool newlyOwned = (g_owned & stamp_bit(index)) == 0;
    if (newlyOwned) {
        g_owned |= stamp_bit(index);
    }
    const bool twili = apply_twili_rule();
    const uint64_t added = g_owned & ~before;
    if (added == 0) {
        return false;
    }
    if (kind == GrantKind::Game && valid_slot(g_slot)) {
        g_history[g_slot].unsaved |= added; // taken back if the slot is reloaded without saving
    }
    write_blob();
    items::on_stamp_owned(index);
    if (kind == GrantKind::Game) {
        if (newlyOwned) {
            mods::log::info("stamp {} collected: {}/{}", index, owned_count(), kStampCount);
            toast_stamp(index); // off by default: HD's only notice is the item-get window
        }
        if (twili) {
            mods::log::info("all other stamps owned: Twili Midna stamp unlocked");
            // HD presents Twili Midna with the stamp book's reward sequence (book_host, mode 3);
            // the toast is only the fallback when that sequence cannot run. When it is missing for
            // a technical reason (assets, hooks) the notice is shown whatever the toasts option
            // says; when the automatic opening is turned off, the toasts option decides.
            if (!book::reward_sequence_available()) {
                toast_stamp(kTwiliMidnaIndex, book::auto_open_enabled());
            }
        }
    }
    ui::notify_state_changed();
    return true;
}

uint64_t run_recovery() {
    if (!slot_ready()) {
        return 0;
    }
    const uint64_t candidates = compute_recovery_bits();
    uint64_t added = candidates & ~g_owned & kAllStampsMask;
    g_owned |= added;
    if (apply_twili_rule()) {
        added |= kTwiliMidnaBit;
    }
    if (added != 0) {
        write_blob();
        for (int i = 0; i < kStampCount; ++i) {
            if ((added & stamp_bit(i)) != 0) {
                items::on_stamp_owned(i);
            }
        }
        mods::log::info("recovered stamps (mask {:#018x}): {}", added, names_of(added, 64));
    }
    return added;
}

void on_new_save(uint32_t slot) {
    refresh_active_state();
    if (!mod_active()) {
        return;
    }
    g_owned = 0;
    g_slot = valid_slot(static_cast<int>(slot)) ? static_cast<int>(slot) : -1;
    if (valid_slot(g_slot)) {
        g_history[g_slot] = SlotHistory{};
    }
    g_slotReady = true;
    items::on_slot_changed();
    book::on_slot_changed();
    write_blob();
    mods::log::info("new save in slot {}: empty stamp collection", slot);
    ui::notify_state_changed();
}

void on_save_loaded(uint32_t slot) {
    refresh_active_state();
    if (!mod_active()) {
        g_slotReady = false;
        return;
    }
    const int index = valid_slot(static_cast<int>(slot)) ? static_cast<int>(slot) : -1;
    load_current_slot(index, fmt::format("save slot {} loaded", slot).c_str());
}

void on_save_written(uint32_t slot) {
    if (!slot_ready() || !valid_slot(static_cast<int>(slot))) {
        return;
    }
    // TP's save menu can write the game into a different file than the one that was loaded.
    // SaveService then makes that slot current (svc/save.cpp save_slot_written) but leaves its blob
    // as it was, and flushes right after this callback: store the collection there now so the
    // blob matches the .gci that was just written.
    g_slot = static_cast<int>(slot);
    write_blob();
    g_history[g_slot].unsaved = 0;
    mods::log::debug("save slot {} written with {} stamp(s)", slot, owned_count());
}

bool stored_stamp_mask(uint32_t slot, uint64_t& outMask) {
    outMask = 0;
    if (svc_save == nullptr) {
        return false;
    }
    StampBlobV1 blob{};
    size_t size = sizeof(blob);
    if (svc_save->peek_blob(mod_ctx, slot, kBlobName, &blob, &size) != MOD_OK || size < sizeof(blob) ||
        blob.version == 0)
    {
        return false;
    }
    uint64_t owned = blob.owned & kAllStampsMask;
    if (valid_slot(static_cast<int>(slot))) {
        const SlotHistory& history = g_history[slot];
        if (history.known && history.unsaved != 0 && owned == history.stored) {
            owned &= ~history.unsaved; // what load_current_slot() will take back
        }
    }
    outMask = owned;
    return true;
}

int stored_stamp_count(uint32_t slot) {
    uint64_t owned = 0;
    return stored_stamp_mask(slot, owned) ? std::popcount(owned) : -1;
}

void attach_to_current_slot() {
    refresh_active_state();
    if (!mod_active() || g_slotReady) {
        return;
    }
    size_t size = 0;
    ModResult result = svc_save->get_blob(mod_ctx, kBlobName, nullptr, &size);
    if (result != MOD_OK) {
        // Either no slot is active or this save has no stamp blob yet: probing with an empty
        // collection tells the two apart (it fails when no slot is active).
        const StampBlobV1 empty{kBlobVersion, 0, 0};
        result = svc_save->set_blob(mod_ctx, kBlobName, &empty, sizeof(empty));
        if (result != MOD_OK) {
            return;
        }
    }
    // The slot number is not known here (SaveService does not report it), so stamps granted
    // before the next load/save event are not tracked for the reload rule above.
    load_current_slot(-1, "attached to the loaded save");
}

void state_update() {
    // The save file name only changes in the pre-launch menu; checking twice a second is plenty and
    // keeps the memory-card mutex out of every frame. Save callbacks refresh it immediately.
    if (++g_activePollTimer >= 30) {
        g_activePollTimer = 0;
        refresh_active_state();
    }
    if (!mod_active()) {
        return;
    }
    // DESIGN: award the Malo Mart stamp when its HD "sold" switch becomes set. Nothing in GC sets
    // it, but a converted HD save (or another mod) may; poll the save data once a second.
    if (++g_maloPollTimer >= 60) {
        g_maloPollTimer = 0;
        if (g_slotReady && !is_owned(kMaloMartStampIndex) && malo_mart_switch_set()) {
            grant(kMaloMartStampIndex, GrantKind::Game);
        }
    }
    if (++g_slotProbeTimer >= 120) {
        g_slotProbeTimer = 0;
        if (g_slotReady) {
            size_t size = 0;
            if (svc_save->get_blob(mod_ctx, kBlobName, nullptr, &size) == MOD_UNAVAILABLE) {
                // Back at file select (SaveService has no active slot any more).
                g_slotReady = false;
                ui::notify_state_changed();
            }
        } else {
            attach_to_current_slot();
        }
    }
}

bool toasts_enabled() {
    bool value = false;
    if (g_toastVar != 0 && svc_config != nullptr) {
        svc_config->get_bool(mod_ctx, g_toastVar, &value);
    }
    return value;
}

void set_toasts_enabled(bool enabled) {
    if (g_toastVar != 0 && svc_config != nullptr) {
        svc_config->set_bool(mod_ctx, g_toastVar, enabled);
    }
}

ConfigVarHandle toast_config_var() {
    return g_toastVar;
}

void register_config() {
    if (svc_config == nullptr) {
        return;
    }
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = "stamps-toasts";
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = false; // HD has no stamp notice besides the item-get window
    if (svc_config->register_var(mod_ctx, &desc, &g_toastVar) != MOD_OK) {
        mods::log::warn("could not register the 'stamps-toasts' option; stamp notices stay off");
        g_toastVar = 0;
    }
}

void toast_stamp(int index, bool force) {
    if (index < 0 || index >= kStampCount || (!force && !toasts_enabled())) {
        return;
    }
    // HD's stamp name and found/total count (res/stamps/hdtext) when extracted, else the own wording.
    // Twili Midna also gets HD's 49-stamp reward notice (2806).
    const std::string name = hdtext::stamp_name(index);
    const std::string counter = hdtext::counter_text(owned_count());
    std::string text = counter.empty() ?
                           fmt::format("{} stamp ({}/{})", ui::escape_rml(name.c_str()),
                               owned_count(), kStampCount) :
                           fmt::format("{}<br/>{}", ui::escape_rml(name.c_str()),
                               ui::escape_rml(counter.c_str()));
    if (index == kTwiliMidnaIndex) {
        if (const std::string reward = hdtext::twili_reward_text(); !reward.empty()) {
            text = fmt::format("{}<br/>{}", ui::escape_rml(reward.c_str()), text);
        }
    }
    const std::string body = fmt::format(
        "<div style=\"display: flex; align-items: center;\"><div style=\"display: block; width: "
        "44dp; height: 44dp;\">{}</div><span style=\"margin-left: 12dp;\">{}</span></div>",
        ui::stamp_image_rml(index, 44, false), text);
    toast_text("New stamp!", body.c_str(), force);
}

void toast_text(const char* title, const char* body, bool force) {
    if (svc_ui == nullptr || (!force && !toasts_enabled())) {
        return;
    }
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.title_rml = title;
    toast.body_rml = body;
    toast.duration_ms = 6000;
    if (svc_ui->push_toast(mod_ctx, &toast) != MOD_OK) {
        mods::log::debug("toast failed: {}", title != nullptr ? title : "");
        return;
    }
    hdfeatures::note_toast(toast.duration_ms);  // same queue as the cave's toasts (its amiibo prompt waits)
}

} // namespace hdstamps
