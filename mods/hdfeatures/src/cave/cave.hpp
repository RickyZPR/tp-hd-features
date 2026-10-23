#pragma once

// HD Features / Cave of Shadows: shared state and game-thread actions.
//
// Everything here runs on the game thread (mod_update, hook callbacks, UI and flow callbacks).
// The game's simulation runs at 30 ticks per second (dusk/game_clock.h kSimPeriod); mod_update runs
// once per simulation tick, also while the pause menu is open.

#include <mods/api.h>

#include "hdfeatures.hpp"  // src/hdfeatures.hpp: the mod id, the shared window

#include <cstdint>
#include <string>

class daAlink_c;
class daObjBossWarp_c;
class daTag_Event_c;
class dMenu_Collect2D_c;
class fopAc_ac_c;

namespace cave {

inline constexpr const char* kStageName = "D_SB11";
inline constexpr const char* kModId = hdfeatures::kModId;  // mod.json.in "id" (local.ricky.hdfeatures)
inline constexpr int kTicksPerSecond = 30;

enum class Phase : std::uint8_t {
    Idle,      // not in the cave
    Entering,  // our setNextStage("D_SB11", ...) is pending
    InCave,    // start stage is D_SB11
    Exiting,   // our return-point setNextStage is pending
};

// Lifecycle (../mod.cpp).
// Registers the cave-* options. Once, at mod load, whether or not the "Cave of Shadows" switch is on.
void register_config();
// Starts the feature (save/give observers, hooks, HD text, amiibo screens) the first time the switch is
// on. Set the switch first (set_switch(true)). A failure leaves nothing installed and only this feature
// off; the caller keeps the rest of the mod running.
ModResult initialize(ModError* error);
void tick();
// mod_shutdown only.
void shutdown();

// The HD Features "Cave of Shadows" switch, applied by ../mod.cpp at a safe moment (no menu, event,
// message or scene change). Off: enabled() is false, so every hook is a no-op, the amiibo and the
// entry are gone, and the HD text, flows and talk tags are removed; on again reinstalls them. The save
// blob keeps being tracked either way, so no progress is lost.
void set_switch(bool on);
bool switch_on();
// True while turning the switch off has to wait: in D_SB11, entering or leaving it (warp, stage change,
// the amiibo trip after the menu closed).
bool busy();
// The switch wants to go off while busy(): no new trips; a player inside is sent back to the return point
// (like Midna's Withdraw) at the next safe moment. False again cancels it.
void request_stop(bool stop);

// Environment checks.
bool initialized();
// True while the vanilla game mode is selected (save name "gczelda2"). Every other game mode has its own
// save name (randomizer: "randomizer", Speedrun: "gczelda2-speedrun", other mods' modes) and the cave stays
// off there, like the stamps (DESIGN.md: "do nothing while a randomizer game mode/seed is active").
bool vanilla_mode();
// True when the feature may act at all (initialized, switch on and vanilla game mode).
bool enabled();
// True when the current start stage is D_SB11.
bool in_cave_stage();
// True in D_SB11 while the mod is enabled (the cave's rules apply).
bool cave_rules();
Phase phase();

// Entry / exit. can_* fill `reason` (static string, may be null) when they return false.
// fromPauseMenu: the request comes from the Collection screen (the pause menu is open right now).
// can_enter = the mod's own conditions, then HD's entry check (Zelda.rpx 0x029b0448, a deny-list; every HD refusal
// shows message 1530), plus the gameplay-state guards for the paths that do not start on the pause menu.
bool can_enter(const char** reason, bool fromPauseMenu = false);
bool can_withdraw(const char** reason);
// Queue the action for the next mod_update; false (with a toast) when not allowed right now.
bool request_enter();
bool request_withdraw();

// Send the player back to the stored return point (or a safe fallback). `lastMode` and `speed`/
// `angle` follow dComIfGp_setNextStage's lastMode/lastSpeed/lastAngle; `startEvent` is the
// player-parameter start event (0xCA = DEFAULT_START, 0xC9 = NORMAL_COMEBACK after a game over).
// `cause` decides how the run record ends (a portal on floor 6/20/41 stores the amiibo's hearts).
// ExitCause::Portal on a portal floor (rooms 25/34/14) also does HD's l8warp bookkeeping
// (set_section_cleared(portal_tier()) and clear_stage_chests(); both idempotent). Since the HD exit portal
// cutscene (on_portal_scene_chg*) this is only the portal's fallback (its hooks or the Record screen missing).
enum class ExitCause : std::uint8_t { Portal, GameOver, Withdraw, Shutdown };
void exit_to_return_point(
    std::uint32_t lastMode, int startEvent, float speed, std::int16_t angle, ExitCause cause);

// ---- HD sections (research/cos_sections_spec.md, VERIFIED in HD Zelda.rpx) ----------------------
// HD's cave controller ("los", created when D_SB11 loads after an amiibo entry or a death-continue)
// keeps one byte of section state, los+0x1E: the tier (1, 2 or 3) of the last tier ALLdie that fired
// in this run (HD 0x020d40a4/d8/0x020d4120). 0 = none yet. The mod keeps it in memory (reset on every
// entry, death-continue and when leaving the cave; kept across floors and void-outs, like los).
std::uint8_t section_tier();
// The tier the portal in the current room stands for: section_tier(), or (when the ALLdie hook is
// missing and no tier was recorded) the room's own: 25 -> 1, 34 -> 2, 14 -> 3. 0 elsewhere.
std::uint8_t portal_tier();
// HD l8warp SCENE_CHG (0x026d3360/7c/98): tier 3/2/1 sets 0x7004/0x7002/0x7001 = HD labels 828/826/824
// ("section 3/2/1 cleared"; blob flags). These labels are set nowhere else in HD. Other tiers: no-op.
void set_section_cleared(std::uint8_t tier);
// HD 0x02aa7c68(gameInfo+0xDF8): zero the current stage memory's chest bits (D_SB11 only), so the
// cave's chests refill for the next run. Called by the portal exit and the death-continue.
void clear_stage_chests();
// HD tier ALLdie (0x020d4044-0x020d4138): records the tier (prm & 0xFF, 1..3) and returns the switch to
// set: tier 1 -> swbit if 0x7001 && 0x5420 (labels 824 && 825) else alt; tier 2 -> swbit if 0x7002 &&
// 0x7101 (826 && 827) else alt; tier 3 -> swbit. swbit = (prm >> 8) & 0xFF, alt = (prm >> 16) & 0xFF.
// `room` is the ALLdie's room (the floor-5/19 talk-tag fallback, see cave.cpp).
int alldie_tier_switch(std::uint32_t prm, int room);
// An HD swc00 area of type 10 (start the cave timer) or 11 (stop it) was entered (its sw1 is already
// set by the GC type-15 switch rule the data uses; hooks.cpp reads the HD type from angle.x bits 12-15).
void on_hd_switch_area(int hdType);
// Game over in D_SB11, "Continue" chosen (dStage_restartRoom / dStage_changeScene with game-over status
// 2): HD 0x020b9f14-0x020b9ff0 restarts the run on floor 1 instead of the return point.
void continue_after_death();

// ---- HD exit portal (l8warp / Obj_BossWarp in D_SB11; research/cos_portal_spec.md, hooks.cpp A-D) ---------
// HD's cave branch of daObjBossWarp_c: hidden until its switch, WARP_CHECK (flow 3008) -> BOSS_WARPIN_M.
// WALK_TARGET1: HD Link+0x3968 = 1.0, so Link runs into the portal at full speed (setDemoData post-hook);
// SCENE_CHG sets it back to -1.0.
void set_portal_dash(bool on);
bool portal_dash_active();
// SCENE_CHG advance (HD 0x026d333c-0x026d33e0): section cleared by the cave controller's tier, the rupees
// collected committed (they survive leaving the cave), the stage's chest bits zeroed.
void on_portal_scene_chg(daObjBossWarp_c* portal);
// SCENE_CHG first frame (HD 0x026d3790): Midna hidden; then the "Cave of Shadows Record" screen (event
// removed, HD dMw+0x166), or without it the old fade to the stored return point.
void on_portal_scene_chg_first_frame(daObjBossWarp_c* portal);
// True from the portal's SCENE_CHG until the stage changes (no Withdraw, no second portal exit).
bool portal_exit_running();

// HD's cave controller keeps life and rupees from the entry (los+0x8 / +0xC, VERIFIED 0x0290ad50-5c), writes
// them into every save made inside the cave (memory_to_card 0x02aa8f98-0x02aa8fc4) and back into the game when
// the cave is left (0x0290acd8-e8); the exit portal commits the current rupees first (0x026d33a4). The mod also
// stores the entry form there (a human Link is a wolf in the cave; with the non-HD option "cave-entry_warp" the
// Midna warp-out on entry already turns him into one).
// dSv_info_c::memory_to_card pre/post (hooks.cpp):
void before_memory_to_card();
void after_memory_to_card();

// ---- "Cave of Shadows Record" (record.cpp) --------------------------------------------------------------
// One run's record as HD's Record screen writes it into the save (proc 0x42, save 0xA98..0xAA4).
struct RecordValues {
    std::uint32_t timeMs = 0;  // completion time (the cave timer), ms (HD 0xA98, dTimer 0x0290a698)
    int damage = 0;            // quarter hearts lost (HD 0xA9C = los+0x20)
    int floor = 0;             // floor of the room the run ended in (HD 0xA9E)
    int life = 0;              // life at the end, quarter hearts (HD 0xA9F; 0 = no record)
    int maxLife = 0;           // heart pieces, 5 per container (HD 0xAA4)
    int combo = 0;             // max combo (HD 0xAA0 = los+0x1D)
    int wolf = 0;              // Wolf Link amiibo row: the level of the figure used to heal, 0 = unused (HD 0xAA1)
    int ganon = 0;             // Ganondorf amiibo row (HD 0xAA2): no such figure here, always 0
    int zelda = 0;             // Zelda amiibo row (HD 0xAA3): always 0
};
// HD proc 0x42: the record shown becomes the save's "previous record" (cave blob).
void record_store(const RecordValues& values);
// Yes on 1261: the virtual NFC write popup (amiibo.cpp, ScanMode::Write). False = no popup could open (the
// figure was written at once and a notice shown instead).
bool open_amiibo_write();
// Every end of the Record screen once its save menu has closed (`saved`: the slot was written): HD
// dComIfGs_gameStart, back to the save's return place.
void record_finished(bool saved);

// ---- Virtual Wolf Link amiibo (DESIGN.md "Entry" 1-3) ------------------------------------------
enum class ScanMode : std::uint8_t { Enter, Heal, Write };
enum class ScanResult : std::uint8_t {
    EnterOk,       // the amiibo takes you to the cave: close the menu and warp
    EnterRefused,  // HD message 1530
    HealOk,        // HD message 1549 (hearts restored)
    HealUsed,      // HD message 1555 (once per visit)
    HealEmpty,     // HD message 1572 (no hearts stored yet)
    Unavailable,   // the mod cannot run the cave right now (hook/asset problem); 1530 + log
    Cancelled,     // the player closed the scan screen before the figure was read
    WriteOk,       // Record screen: the figure was written; HD message 1569
    WriteOkFirst,  // ... the first time for this save; HD message 1568 (save byte 0xABB)
};
bool amiibo_unlocked();                       // Faron's light spirit restored (HD label 823)
void collect_screen_opened();                 // Collection screen built
void collect_screen_closed();
// The Collection screen ran its idle (wait) process this frame with no sub-window opening (hooks.cpp):
// the prompt is visible and the menu-closing B retry is allowed only then.
void collect_screen_idle();
// The amiibo button was pressed in the Collection screen's idle state (hooks.cpp).
void amiibo_button(dMenu_Collect2D_c* screen);
// Called by the scan screen when the "reading" animation ends: evaluates and applies the scan.
ScanResult amiibo_scan_result(ScanMode mode);
// Called when the scan screen has closed. `byPlayer`: closed with Close/B (swallow that press so it
// does not also reach the Collection screen); false when the mod closed it after an EnterOk.
// After an EnterOk, HD's "Cave of Shadows" screen opens (confirm.cpp); Link only leaves once the game's
// own save has written the slot ("Save and begin").
void amiibo_scan_closed(ScanResult result, bool byPlayer);
// confirm.cpp -> here. The HD screen has closed: `save` = "Save and begin" (the game's pause-menu save
// is opening now), false = Cancel / B / the screen went away (back on the Collection screen).
void confirm_finished(bool save);
// The amiibo trip is in its save step (the pause-menu save screen it opened is up).
bool save_trip_active();
// ... and SaveService reported the slot written during it.
bool save_trip_saved();
// confirm.cpp's saveEnd post-hook, after the trip's "Saved." (HD save proc 0x24 type 6, 0x02a0a7fc-0x02a0a820:
// end status 2, status 3, then enterLos 0x02a02028 in the same frame): stores the return point and issues the
// stage change to D_SB11 while the save menu stays on screen (HD dMw state 0x2D). True = the stage change is
// issued (the caller holds the menu); false = the menu closes as before and the trip is decided once the game
// runs again (refused, or the non-HD "warp-out on entry" option can play Midna's warp here).
bool enter_from_menu();
int amiibo_button_choice();                   // 0 = Z, 1 = X, 2 = Y (config "cave-amiibo_button")
const char* amiibo_button_label();

// ---- HD flows (hdtext.cpp) -> game state ------------------------------------------------------
// HD-only event labels 822..829 (critic correction #8: saveBitLabels has 822 entries in v2.0.3, so
// these are emulated in the "cave" blob / derived from GC story flags). HD label table 0x1009F88C:
//   822 0x7008 reward given (blob)      823 0x0610 = M_019           824 0x7001 section 1 cleared (blob)
//   825 0x5420 = F_0685                 826 0x7002 section 2 cleared  827 0x7101 Sols' power in the sword
//   828 0x7004 section 3 cleared (blob) 829 0x7080 arrival intro pending (blob, one-shot)
// 827 has no GC flag (HD sets it in its own flow 811): the GC proxy is the Light Sword (cave.cpp).
bool hd_label(std::uint16_t label);
void set_hd_label(std::uint16_t label, bool on);
std::uint16_t intro_query();                  // HD query055 (index 54) of flow 1120, 0x02a64650
// HD event 44: Midna's "Withdraw" / the turn-back. HD (0x02a65c84): two warp sounds, then a soft reset to the title
// screen 15 frames later; the player reloads the save "Save and begin" wrote. Here (design decision; on PC
// the reset path ends the program): Midna's warp-out (POTAL_WARPIN), then that save is put back in memory and Link
// lands at its return place, as after the reload (option "cave-hd_withdraw", default on; off: back to the stored
// return point, keeping the visit). Starts once the talk / cutscene is over, and (HD's Withdraw) once a reward still
// queued in ItemService has been handed out, so the restore drops it as HD's reload does. A pause-menu save made in
// the cave replaces the "Save and begin" image (HD's reload would load that newer save).
// `afterMessage`: reached by the mod's own walk of flow 1120 (its message 5062 is not on screen yet).
void flow_withdraw(bool afterMessage = false);
// Flow 1120's turn-back condition (nodes 1640/1641: F_0250 clear and M_045 set): Midna is gravely ill. HD lets the
// amiibo take you in then; the arrival on floor 1 sends you straight back (message 5062, then event044).
bool midna_gravely_ill();
// dStage_Delete post-hook (hooks.cpp): the cave's stage memory has just been stored; the next stage has not
// loaded its own. A Withdraw that restores the "Save and begin" save does it here (without this hook: once the
// stage change's fade is at black, tick()).
void on_stage_deleted();
std::uint16_t reward_query();                 // reward talk on floor 41 (0 reward, 1 nothing, 2 done)
void flow_reward();                           // reward talk: hand out this clear's reward

// ---- Hook helpers -----------------------------------------------------------------------------
void before_wolf_change_check();       // applies a pending transform-status restore
bool colossal_wallet_active();         // blob flag + slot loaded + enabled
// HD next-wallet rule for vanilla wallet grants (pre-hooks on item_func_WALLET_LV2/LV3). true = the
// mod applied the grant itself, skip the original.
bool wallet_lv2_override();
bool wallet_lv3_override();
void notify_transform_blocked();       // error sound + rate-limited toast
// What a failed hook takes away (graceful degradation):
//   DemoGuard     cutscene message guard: entering the cave would crash at the arrival -> entry refused
//   AmiiboButton  Collection-screen button: the panel's Enter button is shown instead
//   FlowRemap     HD flows (Midna menu, talks): Midna's GC menu with the transform lock, no talk tags
//   PortalWarp    warp-out presentation: plain fades (an unhooked portal warp would hang on arrival)
//   SectionTiers  HD tier ALLdie: GC ALLdie always opens the way on (every section behaves as unlocked,
//                 one continuous run; the portals on floors 6/20 never appear)
//   CaveTimer     HD swc00 types 10/11: the record time runs for the whole visit instead of floor-1
//                 arena -> floor 5/19 (locked) or 40
//   PortalCutscene HD exit portal (Obj_BossWarp Create/execute/demoProc, Link setDemoData): logged only; the
//                 portal falls back step by step to GC's direct exit (hooks.cpp)
//   WithdrawRestore dStage_Delete post-hook: HD's Withdraw puts the "Save and begin" save back once its stage
//                 change's fade is at black instead (cave.cpp tick, Phase::Exiting)
enum class Feature : std::uint8_t {
    None, DemoGuard, AmiiboButton, FlowRemap, PortalWarp, SectionTiers, CaveTimer, PortalCutscene, WithdrawRestore
};
void record_hook_failure(const char* what, Feature feature = Feature::None);
bool feature_ok(Feature feature);
// dMsgObject_c::setDemoMessage(value) with HD's top-bit value (cutscene message of the D_SB11 r00_in
// demos): HD 0x02911f9c reads it as "start flow value & 0x7FFFFFFF": 1120 (R19) = the arrival flow,
// 0 (R26/R02) = no message, the demo resumes. Returns true when the original must be skipped (always
// for top-bit values: the original would index far past the message table).
bool on_demo_message(std::uint32_t value);
// dMsgFlow_c::init: HD flow ids used in D_SB11 -> the mod's FlowService graphs. Returns the flow id to
// use (unchanged when nothing applies).
int remap_flow(fopAc_ac_c* partner, int flowId);
// daAlink_c::skipPortalObjWarp (end of the Midna portal warp-out): true = handled (skip the original).
bool on_portal_warp_end(daAlink_c* link);
// daTag_Event_c::arrivalTerms post: false when floor 1's arrival intro must not play (the R19 TagEv
// whose HD valid label is 829 plays only while label 829 is set; every other TagEv keeps GC's rule).
bool intro_tag_allowed(daTag_Event_c* tag);
// cc_at_check: an enemy was hit; `killed` when that hit defeated it (run record "Max combo").
void on_enemy_hit(fopAc_ac_c* enemy, bool killed);

// ---- UI helpers -------------------------------------------------------------------------------
std::string status_text();
std::string availability_text();
std::string record_rml();              // "Cave of Shadows Record" block for the mod panel
std::string hook_problems_text();      // empty when every hook installed
bool enter_button_visible();           // debug option on, or the amiibo entry hooks failed
std::uint64_t floor_toasts_var();      // ConfigVarHandle
std::uint64_t hd_sections_var();
std::uint64_t amiibo_hint_var();
std::uint64_t debug_enter_var();
std::uint64_t warp_effect_var();
std::uint64_t amiibo_button_var();
std::uint64_t hd_withdraw_var();
std::uint64_t start_deepest_var();  // non-HD option, off by default
std::uint64_t entry_warp_var();     // non-HD option "cave-entry_warp", off by default

void toast(const char* title, const char* body, bool warning = false, std::uint32_t ms = 0);

}  // namespace cave

namespace cave::hooks {
// Installs every hook; failures are recorded (cave::record_hook_failure) and never fatal.
void install_all();
// mod_shutdown only. Leaves the targets the stamps hook too to the loader (see hooks.cpp).
void uninstall_all();
}  // namespace cave::hooks

// The cave's part of the HD Features UI (ui.cpp); ../panel.cpp owns the Mods panel, the menu-bar tab and
// the window and calls these.
namespace cave::ui {
// Mods panel: the sections "Cave of Shadows", "Cave of Shadows: record", "... options",
// "... how to play" and "... before you turn it off". Help texts become tooltips (no help pane there).
void build_panel(std::uint64_t pane);
void update_panel();
// One line for the panel's shared Troubleshooting section (updated by update_panel()).
void build_troubleshooting(std::uint64_t pane);
// The "Cave of Shadows" tab of the HD Features window (left: the contents, right: the help pane).
ModResult build_tab(ModContext* ctx, std::uint64_t window, std::uint64_t left, std::uint64_t right,
    void* userData, ModError* error);
ModResult update_tab(ModContext* ctx, void* userData, ModError* error);
// The window was closed or rebuilt: drop the tab's element handles.
void tab_closed();
void shutdown();
}  // namespace cave::ui
