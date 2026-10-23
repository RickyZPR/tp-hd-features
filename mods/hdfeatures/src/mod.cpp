// HD Features (local.ricky.hdfeatures): Twilight Princess HD's 50 stamps and its Cave of Shadows for
// Dusklight v2.0.3, as one native mod. Every game asset is made on the player's PC from their own games and never
// shipped: since 2.1.0 the mod makes them itself on the first start (src/hdfiles: setup finds the player's TP HD
// and runs the converter library on a worker thread; the files are stored in the mod's data folder). See
// DESIGN.md and research/dropin/plan.md.
//
// This file is the mod's only entry point: DEFINE_MOD, the one set of service imports and
// mod_initialize / mod_update / mod_shutdown. They drive the two features (src/stamps, namespace
// hdstamps: stamps/feature.hpp; src/cave, namespace cave: cave/cave.hpp) and their switches, the config
// bools "stamps-enabled" and "cave-enabled" (Mods panel "Stamps" / "Cave of Shadows", both on by
// default). src/panel.cpp owns the one Mods panel, menu-bar tab and window.
//
// Switches. Each feature starts the first time its switch is on (at load, or later) and is never torn
// down before mod_shutdown; switching off gates it instead (stamps: mod_active(), cave: enabled()), the
// same paths the old mods used for other game modes, so a change applies without a Reload. It applies
// at the next safe moment of mod_update, held for kSettleTicks in a row: no scene change or wipe, and
// either the title screen / file select (their own scenes are running, no play scene is being loaded)
// or free play (Link exists; no pause menu, event, cutscene or message box). The stamps also wait for
// their book (pause menu or file-select viewer) to close, and switching them off waits until no stamp
// is being handed over and Link is not in a stage with one of HD's extra chests (stamps/feature.hpp
// feature_off_blocked). The cave refuses new trips at once and waits until nobody is in it or on the
// way (a player inside is sent back to the return point first, like Midna's Withdraw).
//
// Game files (src/hdfiles). mod_initialize checks the stored files first (hdfiles/setup.hpp: the Dusklight
// version, the disc id, then R1's cache check and its 56 overlays). Valid: the features start at load as
// before. Not valid: a feature that is switched on waits (update_*_switch, feature_wait_reason "the game files
// are ready") until setup has made the files and their overlays are live (hdfiles::ready()), then starts at
// the next safe moment like a switch turned on later. A running feature is never stopped by them.

#include "stamps/prelude.hpp"  // library headers before any game header (see prelude.hpp)

#include <mods/service.hpp>
#include <mods/svc/config.h>
#include <mods/svc/file.h>
#include <mods/svc/flow.h>
#include <mods/svc/hook.h>
#include <mods/svc/item.h>
#include <mods/svc/log.h>
#include <mods/svc/log.hpp>
#include <mods/svc/message.h>
#include <mods/svc/save.h>
#include <mods/svc/stage.h>
#include <mods/svc/ui.h>

#include "hdfeatures.hpp"
#include "hdfiles/files.hpp"
#include "hdfiles/setup.hpp"

#include "cave/amiibo.hpp"
#include "cave/cave.hpp"
#include "stamps/feature.hpp"
#include "stamps/state.hpp"
#include "stamps/ui.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_msg_object.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

DEFINE_MOD();

// The union of both old mods' imports (HD Stamps: flow optional; Cave of Shadows: stage optional), plus
// FileService for the folder picker (hdfiles/setup.cpp). All ten are built into Dusklight v2.0.3, so they are
// required here; both features still null-check the ones they treated as optional. HostService and
// OverlayService are imported in hdfiles/files.cpp (R1). ResourceService is no longer imported: since 2.1.0 the
// package holds no game files, and every reader goes through hdfiles (files.hpp).
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(SaveService, svc_save);
IMPORT_SERVICE(ItemService, svc_item);
IMPORT_SERVICE(MessageService, svc_message);
IMPORT_SERVICE(FlowService, svc_flow);
IMPORT_SERVICE(StageService, svc_stage);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(FileService, svc_file);

namespace hdfeatures {
namespace {

struct Switch {
    const char* name;            // config var (mod.local_ricky_hdfeatures.<name>)
    ConfigVarHandle var = 0;
    bool running = false;        // the switch as applied
    bool failed = false;         // could not start (stays off until the mod is reloaded)
};

Switch g_stamps{"stamps-enabled"};
Switch g_cave{"cave-enabled"};
int g_menuTimer = 0;
int g_settledTicks = 0;  // consecutive mod_update ticks with game_settled_now() (update_settled)

// A switch change is applied only after this many settled ticks in a row (a third of a second), so a
// single quiet tick between two steps of a scene load or an event never counts.
constexpr int kSettleTicks = 10;

Switch& sw(Feature feature) {
    return feature == Feature::Stamps ? g_stamps : g_cave;
}

void register_switch(Switch& s) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = s.name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = true;
    if (svc_config->register_var(mod_ctx, &desc, &s.var) != MOD_OK) {
        mods::log::warn("could not register the '{}' option; the feature stays on", s.name);
        s.var = 0;
    }
}

bool wanted(const Switch& s) {
    bool value = true;
    if (s.var != 0) {
        svc_config->get_bool(mod_ctx, s.var, &value);
    }
    return value && !s.failed;
}

// A message box is up (dMsgObject_isTalkNowCheck; the passive stage / boss / staff title cards do not
// count, like cave.cpp message_box_up).
bool message_box_up() {
    if (!dMsgObject_isTalkNowCheck()) {
        return false;
    }
    dMsgObject_c* object = dComIfGp_getMsgObjectClass();
    return object != nullptr && !object->isPlaceMessage() && !object->isBossMessage() &&
           !object->isStaffMessage();
}

// The title screen (OPENING_SCENE, d_s_play.cpp), the file select (NAME_SCENE, d_s_name.cpp) or its name
// entry is the running scene. fpcM_SearchByName only finds processes that finished creating (the layer
// queues, f_pc_executor.cpp fpcEx_ToExecuteQ), so a play scene that is still loading is not mistaken
// for one of these: "no Link" alone also matches every scene load (Link is created late in it,
// d_s_play.cpp / d_a_alink.cpp create), when a switch must not apply.
bool menu_scene_running() {
    return fpcM_SearchByName(fpcNm_NAME_SCENE_e) != nullptr || fpcM_SearchByName(fpcNm_NAMEEX_SCENE_e) != nullptr ||
           fpcM_SearchByName(fpcNm_OPENING_SCENE_e) != nullptr;
}

// The safe moment both switches wait for, this tick (see the top of this file).
bool game_settled_now() {
    if (dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() != 0) {
        return false;  // a scene change or wipe is under way
    }
    if (dComIfGp_getLinkPlayer() == nullptr) {
        return menu_scene_running() && fpcM_SearchByName(fpcNm_PLAY_SCENE_e) == nullptr;
    }
    return dComIfGp_isPauseFlag() == 0 && !dComIfGp_event_runCheck() && !message_box_up();
}

// Once per mod_update, before the switches.
void update_settled() {
    if (!game_settled_now()) {
        g_settledTicks = 0;
    } else if (g_settledTicks < kSettleTicks) {
        ++g_settledTicks;
    }
}

bool game_settled() {
    return g_settledTicks >= kSettleTicks;
}

// A feature that is not running yet waits until the stored game files are live (hdfiles::ready(), Correction
// #13); a running feature is never stopped by them (Correction #16: gated here, not in wanted()).
bool waits_for_files(bool want, bool running) {
    return want && !running && !hdfiles::ready();
}

// --- Stamps -------------------------------------------------------------------------------------

void start_stamps() {
    if (!hdstamps::feature_started()) {
        // With the switch state as it is: on at load, so the item side comes first and the first
        // active-state evaluation applies the item table (stamps/feature.cpp); off before a later start,
        // so everything is installed while inactive and activated by set_feature_enabled below.
        hdstamps::feature_initialize();
    }
    hdstamps::set_feature_enabled(true);
    hdstamps::attach_to_current_slot();  // picks the loaded save's stamps up again
    hdstamps::ui::notify_state_changed();
    g_stamps.running = true;
    mods::log::info("Stamps on");
}

void stop_stamps() {
    // As in another game mode: the 50 item ids go back to the game, the HD-only chests and Agitha's
    // flow patch are removed, the Collection slot and file-select plate are not added to the next
    // screens. Hooks stay (gated), so turning it on again needs no reload.
    hdstamps::set_feature_enabled(false);
    hdstamps::ui::notify_state_changed();
    g_stamps.running = false;
    mods::log::info("Stamps off");
}

void update_stamps_switch() {
    const bool want = wanted(g_stamps);
    if (want == g_stamps.running || !game_settled() || waits_for_files(want, g_stamps.running) ||
        hdstamps::feature_busy())
    {
        return;
    }
    if (!want && hdstamps::feature_off_blocked() != nullptr) {
        // A stamp is on its way to the player, or one of HD's extra chests may already be in the
        // loaded rooms (it would give the game's empty item once the item ids are handed back).
        return;
    }
    want ? start_stamps() : stop_stamps();
    refresh_menu();
}

// --- Cave of Shadows ------------------------------------------------------------------------------

bool start_cave() {
    cave::set_switch(true);
    if (!cave::initialized()) {
        ModError error = MOD_ERROR_INIT;
        if (cave::initialize(&error) != MOD_OK) {
            // Only this feature: the stamps keep running (the old mod failed as a whole here).
            cave::set_switch(false);
            g_cave.failed = true;
            g_cave.running = false;
            mods::log::error("the Cave of Shadows could not start: {}",
                error.message[0] != '\0' ? error.message : "unknown error");
            return false;
        }
    }
    g_cave.running = true;
    mods::log::info("Cave of Shadows on");
    return true;
}

void stop_cave() {
    cave::set_switch(false);
    g_cave.running = false;
    mods::log::info("Cave of Shadows off");
}

void update_cave_switch() {
    const bool want = wanted(g_cave);
    if (want == g_cave.running) {
        cave::request_stop(false);  // switched back before it applied (also cancels a queued Withdraw)
        return;
    }
    if (!want) {
        // At once, before the busy and settled checks: no new trips (can_enter refuses, so a scan on the
        // still-open Collection screen cannot take you in), and a player inside is sent back to the
        // return point at the next safe moment (cave.cpp tick_stop).
        cave::request_stop(true);
        if (cave::busy()) {
            return;
        }
    }
    if (!game_settled() || waits_for_files(want, g_cave.running)) {
        return;
    }
    want ? (void)start_cave() : stop_cave();
    refresh_menu();
}

}  // namespace

bool feature_wanted(Feature feature) {
    return wanted(sw(feature));
}

bool feature_running(Feature feature) {
    return sw(feature).running;
}

bool feature_pending(Feature feature) {
    const Switch& s = sw(feature);
    return !s.failed && wanted(s) != s.running;
}

bool feature_failed(Feature feature) {
    return sw(feature).failed;
}

bool feature_waits_for_files(Feature feature) {
    const Switch& s = sw(feature);
    return waits_for_files(wanted(s), s.running);
}

ConfigVarHandle feature_config_var(Feature feature) {
    return sw(feature).var;
}

const char* feature_wait_reason(Feature feature) {
    if (!feature_pending(feature)) {
        return nullptr;
    }
    if (feature_wanted(feature) && !hdfiles::ready()) {
        return "the game files are ready";
    }
    if (feature == Feature::Stamps && !feature_wanted(feature)) {
        if (hdstamps::feature_busy()) {
            return "the stamp book is closed";
        }
        if (const char* reason = hdstamps::feature_off_blocked(); reason != nullptr) {
            return reason;
        }
    }
    return nullptr;
}

void note_toast(std::uint32_t durationMs) {
    cave::amiibo::note_other_toast(durationMs);
}

}  // namespace hdfeatures

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    (void)error;  // nothing here fails the whole mod: a feature that cannot start is only that feature
    using namespace hdfeatures;

    // The stored game files first (hdfiles/setup.hpp): the Dusklight version, the disc id, the cache check and
    // the 56 overlays, before anything reads them. Without valid files, setup reads the 13 GC inputs now and
    // starts its worker at the first mod_update.
    hdfiles::setup::register_config();
    const bool files = hdfiles::setup::initialize();

    // Options first, whether or not their feature runs, so the panel always shows them.
    register_switch(g_stamps);
    register_switch(g_cave);
    hdstamps::feature_register_config();
    cave::register_config();
    panel_initialize();

    // At load the switches apply at once (a lifecycle safe point, as for the old mods).
    if (wanted(g_stamps) && files) {
        start_stamps();
    } else if (wanted(g_stamps)) {
        // The same state as "switched off at load" (plan 2.9: no observer, no save blob is touched); the stamps
        // start once the game files are ready.
        mods::log::info("Stamps wait for the game files");
        hdstamps::set_feature_enabled(false);
    } else {
        hdstamps::set_feature_enabled(false);  // the stamps stay off until the switch is turned on
        mods::log::info("Stamps are switched off");
    }
    if (wanted(g_cave) && files) {
        start_cave();
    } else if (wanted(g_cave)) {
        mods::log::info("the Cave of Shadows waits for the game files");
    } else {
        mods::log::info("the Cave of Shadows is switched off");
    }
    refresh_menu();
    mods::log::info("HD Features ready (stamps {}, cave {})",
        g_stamps.running ? "on" : wanted(g_stamps) ? "waiting" : "off",
        g_cave.running ? "on" : g_cave.failed ? "failed" : wanted(g_cave) ? "waiting" : "off");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    (void)error;
    using namespace hdfeatures;
    hdfiles::update();         // Registered -> Ready a mod_update after the overlays were added (Correction #13)
    hdfiles::setup::tick();    // the worker: start, progress, hand-over (hdfiles::adopt)
    update_settled();
    update_stamps_switch();
    update_cave_switch();
    hdstamps::feature_update();  // runs while off too: everything there is gated by mod_active()
    cave::tick();                // likewise: only housekeeping while the switch is off
    if (++g_menuTimer >= 15) {
        g_menuTimer = 0;
        panel_update();  // the stamp list's offering follows the book's state and the debug option
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    (void)error;
    using namespace hdfeatures;
    hdfiles::setup::stop_worker();  // stop and join the worker before anything else (Corrections #4, #5)
    panel_shutdown();
    // Reverse start order. The cave first: disabling the mod inside the cave sends the player out
    // before its overlay goes away (cave.cpp shutdown).
    if (cave::initialized()) {
        cave::shutdown();
    }
    hdstamps::feature_shutdown();
    hdfiles::shutdown();
    hdfiles::setup::shutdown();  // a "Delete the game files" request is carried out here (Correction #14)
    mods::log::info("HD Features unloaded");
    return MOD_OK;
}

}  // extern "C"
