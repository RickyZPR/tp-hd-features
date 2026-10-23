// HD Features / Cave of Shadows: state, save blob, gating, entry/exit, the virtual amiibo and rewards.
//
// This was the Cave of Shadows mod's cave.cpp (its mod.cpp entry points now live in ../mod.cpp, which
// registers the options at load, starts the feature the first time the "Cave of Shadows" switch is on
// and applies the switch at safe moments: set_switch / busy / request_stop below).

#include "stamps/prelude.hpp"  // library headers before any game header (see prelude.hpp)

#include "cave.hpp"

#include "amiibo.hpp"
#include "confirm.hpp"
#include "floors.hpp"
#include "hdfiles/files.hpp"
#include "hdtext.hpp"
#include "record.hpp"

#include <mods/svc/config.h>
#include <mods/svc/hook.h>
#include <mods/svc/item.h>
#include <mods/svc/log.hpp>
#include <mods/svc/save.h>
#include <mods/svc/ui.h>

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_midna.h"
#include "d/actor/d_a_obj_bosswarp.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_tag_event.h"
#include "d/d_com_inf_game.h"
#include "d/d_demo.h"
#include "d/d_item_data.h"
#include "d/d_kankyo.h"
#include "d/d_menu_collect.h"
#include "d/d_menu_save.h"
#include "d/d_menu_window.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_object.h"
#include "d/d_save.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_msg_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_MemCard.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cave {
namespace {

// ---------------------------------------------------------------------------------------------
// Save blob ("cave"), stored per slot through SaveService (never in the .gci).
//
// The first 24 bytes are exactly the DESIGN.md layout:
//   {u32 version=1; u32 flags; u16 best_floor; u16 reserved; char stage[8]; s16 point; s8 room;
//    s8 layer;}
// DEVIATIONS (documented): `reserved` holds the number of full clears; flags bit 2 = has return point,
// bits 3/4 = HD sections 1/2 completed (reached the rest floor 6 / 20); 20 bytes follow for the exact
// return position (restart-room pos/angle) and the transform status at entry (44 bytes, previous pass).
// This pass appends the HD extras (40 bytes): emulated HD labels >= 822 (critic #8), the virtual Wolf
// Link amiibo's stored hearts and the "Cave of Shadows Record". The version stays 1: older builds read
// the first 44 bytes and ignore the rest; older/shorter blobs load with the new fields zeroed.
// The pre-entry screen (confirm.cpp) appends 4 bytes: the amiibo data as HD keeps it on the figure
// (level, life in quarter hearts, max life in heart pieces; amiibo area +0x11..+0x13), written together
// with stored_hearts. Older builds read the first 84 bytes and ignore them; an older blob loads them as 0
// (amiibo_data() then derives them from stored_hearts).
// HD sections (research/cos_sections_spec.md): HD keeps its cave progress in event bits 0x7001..0x7101 and
// the save bytes gameInfo+0xAB8..0xABA, none of which exist in GC; they live here, in bytes that were
// already in the blob (no size or version change): flags bits 3/4/5 = 0x7001/0x7002/0x7004 (section
// 1/2/3 cleared = left through the floor-6/20/41 portal; until this pass bits 3/4 meant "reached floor
// 6/20", which old saves keep), intro_flags bits 0/1/2 = 0x7020/0x7040/0x7010 ("announced"), the former
// pad byte = the Collection-screen notice guards. Older builds keep these bytes as they are.
// The "Cave of Shadows Record" screen (record.cpp) appends 16 bytes: HD's record in the save (0xA98..0xAA4), the
// previous record its screen counts up from and writes after each portal exit. The version stays 1 again: older
// builds read their shorter prefix, an older blob loads it zeroed (no previous record; migrate_record seeds it).
//
// The blob is written ONLY from the on-save-written callback, i.e. into the slot the game is being saved
// to, right before SaveService flushes the sidecar (review "logic" #1). In-memory state lives in g_blob.
// ---------------------------------------------------------------------------------------------

constexpr char kBlobName[] = "cave";
constexpr std::uint32_t kBlobVersion = 1;
constexpr std::size_t kDesignPrefixSize = 24;
constexpr std::size_t kBaseSize = 44;
constexpr char kVanillaSaveName[] = "gczelda2";  // dusk/game_mode.hpp kDefaultGameModeSaveName

enum : std::uint32_t {
    FLAG_CLEARED_ONCE = 1u << 0,    // first full clear reward handed out (set when the wallet is granted)
    FLAG_COLOSSAL = 1u << 1,        // Colossal Wallet (9999 rupees at wallet size 2)
    FLAG_HAS_RETURN = 1u << 2,      // ret_* fields describe the entry spot (the exact spot is used only after a field entry)
    FLAG_SECTION1_DONE = 1u << 3,   // HD 0x7001 / label 824: left through the floor-6 portal (tier 1)
    FLAG_SECTION2_DONE = 1u << 4,   // HD 0x7002 / label 826: left through the floor-20 portal (tier 2)
    FLAG_SECTION3_DONE = 1u << 5,   // HD 0x7004 / label 828: left through the floor-41 portal (tier 3)
};

// Blob::intro_flags: HD's "announced" bits, written only by the entry bookkeeping (entry_bookkeeping).
enum : std::uint8_t {
    INTRO_SECTION2 = 1u << 0,  // HD 0x7020 (kept at the old "section-2 new-path line shown" position)
    INTRO_SECTION3 = 1u << 1,  // HD 0x7040 (old "section-3 new-path line shown")
    INTRO_FIRST = 1u << 2,     // HD 0x7010 (first entry announced)
};

// Blob::notice_flags: HD's Collection-screen notice guards (save bytes gameInfo+0xAB8/0xAB9/0xABA).
enum : std::uint8_t {
    NOTICE_1545 = 1u << 0,  // zel_00 1545 (M_019)
    NOTICE_1546 = 1u << 1,  // zel_00 1546 (0x7001 && F_0685)
    NOTICE_1547 = 1u << 2,  // zel_00 1547, same text as 1546 (0x7002 && 0x7101)
    NOTICE_1568 = 1u << 3,  // Record screen: the long "hearts stored" note was shown (HD save byte 0xABB)
};

enum class RunEnd : std::uint8_t { None = 0, SectionEnd = 1, FullClear = 2, Withdraw = 3, GameOver = 4 };

struct RunRecord {
    std::uint32_t ticks;        // game ticks (30 per second) in the cave, pause menu excluded
    std::uint16_t damage;       // quarter hearts lost
    std::uint16_t combo;        // longest run of defeated enemies without taking damage
    std::uint8_t hearts;        // whole hearts left when the run ended
    std::uint8_t floorFrom;
    std::uint8_t floorTo;
    std::uint8_t end;           // RunEnd
    std::uint8_t amiiboUsed;    // healing scans used in the run
    std::uint8_t pad[3];
};
static_assert(sizeof(RunRecord) == 16);

// HD's "Cave of Shadows Record" in the save (written by its screen, proc 0x42 0x02a0cc40; VERIFIED offsets).
struct HdRecord {
    std::uint32_t time_ms;   // 0xA98 completion time (cave timer), ms
    std::int16_t damage;     // 0xA9C quarter hearts
    std::uint8_t floor;      // 0xA9E floor of the room the run ended in
    std::uint8_t life;       // 0xA9F life at the end (0 = no record)
    std::uint8_t combo;      // 0xAA0
    std::uint8_t wolf;       // 0xAA1 Wolf Link amiibo row (the figure's level when it healed)
    std::uint8_t ganon;      // 0xAA2 Ganondorf amiibo row
    std::uint8_t zelda;      // 0xAA3 Zelda amiibo row
    std::uint8_t max_life;   // 0xAA4 heart pieces
    std::uint8_t pad[3];
};
static_assert(sizeof(HdRecord) == 16);

struct Blob {
    std::uint32_t version;
    std::uint32_t flags;
    std::uint16_t best_floor;
    std::uint16_t clear_count;  // DESIGN.md "reserved"
    char ret_stage[8];
    std::int16_t ret_point;     // start point of the stage the player came from (informational)
    std::int8_t ret_room;       // restart room used for the return (dComIfGs_getRestartRoomNo)
    std::int8_t ret_layer;      // dComIfGp_getStartStageLayer() at entry (-1 = default)
    float ret_pos[3];           // dComIfGs_getRestartRoomPos() at entry
    std::int16_t ret_angle;     // dComIfGs_getRestartRoomAngleY() at entry
    std::uint8_t ret_form;      // dComIfGs_getTransformStatus() at entry (0 human, 1 wolf)
    std::uint8_t pad0;
    std::uint32_t reserved2;
    // --- extension (this pass) ---
    std::uint8_t hd_labels;     // bit n = emulated HD label 822 + n (only 829 is blob-backed state)
    std::uint8_t stored_hearts; // virtual Wolf Link amiibo: hearts stored at the end of a run (0-20)
    std::uint8_t intro_flags;   // INTRO_*: HD 0x7020 / 0x7040 / 0x7010 ("section 2/3/first entry announced")
    std::uint8_t notice_flags;  // NOTICE_*: HD Collection-screen notices 1545/1546/1547 shown (was padding)
    std::uint16_t runs;         // runs that ended at a portal
    std::uint16_t best_combo;
    RunRecord last;             // the last run that ended (any way)
    RunRecord best;             // fastest run that reached floor 41
    // --- extension (pre-entry screen) ---
    std::uint8_t amiibo_level;  // floor the stored run ended on (HD "Level B<n>"); 0 = none
    std::uint8_t amiibo_life;   // life when it ended, quarter hearts (0 = nothing stored)
    std::uint8_t amiibo_max;    // max life then, heart pieces (5 per container)
    std::uint8_t pad2;
    // --- extension (Record screen) ---
    HdRecord hdrec;             // HD save 0xA98..0xAA4: the last record its screen wrote
};
static_assert(sizeof(Blob) == 104);
static_assert(offsetof(Blob, ret_layer) + 1 == kDesignPrefixSize);
static_assert(offsetof(Blob, ret_pos) == 24);
static_assert(offsetof(Blob, hd_labels) == kBaseSize);

Blob default_blob() {
    Blob blob{};
    blob.version = kBlobVersion;
    blob.ret_room = -1;
    blob.ret_layer = -1;
    blob.ret_point = -1;
    return blob;
}

bool is_default(const Blob& b) {
    const Blob d = default_blob();
    return std::memcmp(&b, &d, sizeof(Blob)) == 0;
}

// ---------------------------------------------------------------------------------------------
// Runtime state (memory only).
// ---------------------------------------------------------------------------------------------

enum class EntryStep : std::uint8_t {
    None,
    Scanning,   // scan screen open on the Collection screen (Enter mode)
    CloseMenu,  // close the pause menu through its own B path (no longer used by the amiibo entry)
    Confirm,    // scan succeeded: HD's "Cave of Shadows" screen (confirm.cpp) is up
    Saving,     // "Save and begin": the game's own pause-menu save is up (no trip without a written slot)
    WaitMenu,   // saved: waiting for the menu to close and the game to run again
    Leaving,    // saved: HD's stage change from the save menu, "Saved." held on screen (confirm.cpp)
};

enum class WarpKind : std::uint8_t { None, Enter, Withdraw };

struct Warp {
    WarpKind kind = WarpKind::None;
    int ticks = 0;
    int consumedAt = -1;      // tick at which checkWarpStart consumed the warp status
    bool fallback = false;    // the portal event did not happen; the stage change was done directly
    bool finished = false;    // the stage change for this warp has been issued
};

// HD's cave timer (research/cos_sections/code.md §1, VERIFIED): a hidden mode-11 dTimer (HD
// 0x02ac8a64 creates it with show type 0, which hides its panes in GC's dDlst_TimerScrnDraw_c::
// setShowType too) that the floor-1 swc00 type-10 area starts (only from its fresh state) and a type-11
// area stops (floor 5/19 while the next section is locked, floor 40 always). It is the record's
// "Completion time" (INFERRED: a dTimer method copies it into los+0x0/+0x4). The mod's run timer plays
// that role: Run::ticks counts while `timer` is Running (pause menu excluded).
enum class CaveTimer : std::uint8_t { Fresh, Running, Stopped };

struct Run {
    bool active = false;
    CaveTimer timer = CaveTimer::Fresh;
    std::uint32_t ticks = 0;
    std::uint32_t damage = 0;
    std::uint16_t combo = 0;
    std::uint16_t maxCombo = 0;
    std::uint8_t floorFrom = 0;
    std::uint8_t floorTo = 0;
    std::uint8_t heals = 0;
    std::uint8_t healLevel = 0;     // the figure's level when it healed (HD los+0x11, the record's Wolf Link row)
    std::uint16_t lastLife = 0;
    bool lifeKnown = false;
};

bool g_initialized = false;
bool g_switchOn = false;       // HD Features "Cave of Shadows" switch as applied by ../mod.cpp
bool g_stopRequested = false;  // the switch wants to go off; leave the cave first
bool g_stopNoticeShown = false;
bool g_stopWithdrawQueued = false;  // g_pendingWithdraw was set by tick_stop (cancelled with the stop)
Blob g_blob = default_blob();
bool g_blobExists = false;     // the active slot has a stored cave blob
bool g_slotActive = false;
bool g_vanilla = true;         // vanilla game mode selected (save name "gczelda2")
int g_modeTimer = 0;
SaveObserverHandle g_saveObserver = 0;
ItemGiveHandle g_giveObserver = 0;
ConfigVarHandle g_cvarFloorToasts = 0;
ConfigVarHandle g_cvarHdSections = 0;
ConfigVarHandle g_cvarAmiiboHint = 0;
ConfigVarHandle g_cvarDebugEnter = 0;
ConfigVarHandle g_cvarWarpEffect = 0;
ConfigVarHandle g_cvarAmiiboButton = 0;
ConfigVarHandle g_cvarHdWithdraw = 0;
// NOT IN HD (an extra option, off by default): start a visit on the first floor of the deepest section the
// player has unlocked (floor 7 or 21) instead of HD's floor 1. Option "cave-start_deepest".
ConfigVarHandle g_cvarStartDeepest = 0;
// NOT IN HD (off by default): Midna's portal warp-out after "Save and begin", where GC's map warp is allowed. HD
// changes the stage straight from the "Saved." menu with a black fade and nothing on Link (VERIFIED, research
// cospres: save proc 0x24 -> enterLos 0x02a02028, dMw state 0x2D). Option "cave-entry_warp".
ConfigVarHandle g_cvarEntryWarp = 0;

Phase g_phase = Phase::Idle;
int g_phaseTimer = 0;
std::uint64_t g_frame = 0;

bool g_pendingEnter = false;        // debug button
bool g_pendingWithdraw = false;     // the mod's own exit: warp back to the stored return point (do_withdraw)
int g_pendingWithdrawTimer = 0;
// The pending Withdraw is flow 1120's turn-back (event044 after message 5062): it never times out while that
// cutscene, its message or the pause menu is up (gc_safety.md F3). Set wherever g_pendingWithdraw is set.
bool g_pendingWithdrawCutscene = false;

// HD event044 = Midna's Withdraw (Zelda.rpx 0x02a65c84-0x02a65f40, VERIFIED; flows 1111 node 1607 and 1120 node
// 1645, both with parameter 0): on its first call it plays Z2SE_MDN_WARP_IN_TATE (0x600A0) and
// Z2SE_WOLF_WARP_OUT (0x2009B) and sets a counter to 16; every later call counts it down, and the call that finds
// 2 writes game-over status 3 (gameInfo+0x634E) and mDoRst::mResetData->mReset = 1, exactly what the game-over
// "Quit" writes (HD 0x0294fce8-f4 = GC d_gameover.cpp:299-303). So HD's Withdraw is a soft reset to the title
// screen: the player reloads the save written by "Save and begin" and lands at its return place with nothing of
// the visit kept. On PC Dusklight's reset path ends in mDoRst_reset -> OSRestart, which closes the program (seen in
// a test log), so the mod never resets (design decision). Instead (option
// "cave-hd_withdraw", on by default):
//   1. "Save and begin" keeps a copy of the save image it wrote (on_save_written: dMenu_save_c::mSaveBuffer after
//      dataWrite's memory_to_card, d_menu_save.cpp:1432-1445) and the cave blob stored with it;
//   2. Withdraw waits for the talk / cutscene to end, then plays Midna's warp-out (POTAL_WARPIN, as "Leave the
//      cave"), or only HD's two event044 sounds where it cannot play;
//   3. at its end the stage change to that save's return place is issued exactly as dComIfGs_gameStart does (the
//      title screen's load, d_com_inf_game.cpp:2327-2336; savedata +0x58 name, +0x60 point, +0x61 room);
//   4. between the cave's dStage_Delete and the next stage's load (hooks.cpp), the save image is put back as the
//      file select's load does (dComIfGs_init + card_to_memory, d_file_select.cpp:283, d_save.cpp:1868-1912;
//      play-time bookkeeping as d_s_name.cpp:373-374) together with the cave blob of that save.
// Off = the mod's earlier warp back to the stored return point, keeping the visit (also "Leave the cave", the
// switch-off and a visit that began without a save).
bool g_restoreWithdraw = false;    // the pending / running Withdraw puts the "Save and begin" save back (HD)
bool g_restorePending = false;     // ... its stage change is issued; on_stage_deleted() restores the save
bool g_restoreHookFailed = false;  // dStage_Delete post-hook missing: restore once the stage change's fade is at black
char g_restoreTarget[9] = {};      // the stage the pending restore's stage change goes to (the image's return place)
alignas(16) unsigned char g_tripCard[QUEST_LOG_SIZE] = {};  // the quest log "Save and begin" wrote (dSv_save_c image)
bool g_tripValid = false;
Blob g_tripBlob = default_blob();  // the cave blob stored with that save
// This visit began with a written save (the amiibo trip's "Save and begin"), as every HD visit does. Only then can
// HD's Withdraw put that save back: after the debug "Enter the cave now" button (no save) it would lose everything
// since the last save, so Withdraw takes the mod's warp back instead.
bool g_visitSaved = false;

// Virtual amiibo (Collection screen).
EntryStep g_entry = EntryStep::None;
int g_entryTimer = 0;
int g_entrySettle = 0;
int g_entryRetry = 0;               // ticks until the menu-closing B is injected again
int g_entryBusyTicks = 0;           // WaitMenu: ticks waited for a cutscene / message after the menu closed
bool g_menuClosedSeen = false;
bool g_promptShownThisPause = false;
bool g_noticeCheckedThisPause = false;  // HD's Collection-screen notices were checked in this pause
bool g_collectOpen = false;
std::uint64_t g_collectIdleFrame = 0;  // g_frame of the Collection screen's last idle wait_proc
bool g_tripSaved = false;           // EntryStep::Saving: SaveService reported the slot written
int g_savingTicks = 0;              // ticks spent in EntryStep::Saving
std::uint32_t g_swallowMask = 0;    // pad triggers swallowed after the scan screen closed
int g_swallowTicks = 0;
int g_swallowMin = 0;

Warp g_warp;
Run g_run;
int g_arrivalRoom = -1;             // room the current visit started in (always floor 1's room 19)
std::uint8_t g_tier = 0;            // HD los+0x1E: tier of the last tier ALLdie that fired in this run
bool g_restartRun = false;          // death-continue: start a new run record once floor 1 has loaded
// HD cave controller los+0x8 / +0xC: life and rupees from the moment the controller was created (amiibo entry,
// death-continue), written into saves made in the cave and back when the cave is left (cos_portal code.md,
// re-checked: 0x0290ad50-5c, 0x0290acd8-e8, 0x02aa8f98-0x02aa8fc4; the meaning of +6 as rupees is INFERRED from
// GC's dSv_player_status_a_c layout, life+2 = rupees). The portal commits the rupees collected (0x026d33a4).
struct Snapshot {
    bool valid = false;
    std::uint16_t life = 0;
    std::uint16_t rupee = 0;
};
Snapshot g_snap;
bool g_cardSwap = false;            // memory_to_card: the real life/rupees (and form) are parked here
std::uint16_t g_cardLife = 0;
std::uint16_t g_cardRupee = 0;
bool g_cardFormSwapped = false;     // ... and the transform status was swapped to the entry form
std::uint8_t g_cardForm = 0;

// HD exit portal (cos_portal_spec.md): Link's run-in, the SCENE_CHG bookkeeping, the Record screen.
bool g_portalDash = false;          // HD Link+0x3968 >= 0 (WALK_TARGET1 .. SCENE_CHG)
bool g_portalExit = false;          // SCENE_CHG ran: the Record screen / the exit is under way
bool g_writeScan = false;           // the amiibo scan screen is the Record screen's write popup
RecordValues g_recordValues;        // the run the Record screen shows (its amiibo write uses it)

// The entry bookkeeping's previous state, restored if the stage change to D_SB11 never happens.
std::uint8_t g_undoIntroFlags = 0;
std::uint8_t g_undoHdLabels = 0;
bool g_undoLifeValid = false;       // ... and the life before HD's entry set it to full
std::uint16_t g_undoLife = 0;

int g_lastRoom = -1;
bool g_roomSynced = false;          // room control has reported the arrival room since arriving
std::uint64_t g_toastedFloors = 0;
bool g_rewardedThisVisit = false;
int g_rewardRoomTicks = 0;          // ticks spent in the reward room before its talk happened
std::uint8_t g_rewardQueued = 0;    // wallet item queued for the first-clear reward (0 = none)
std::uint64_t g_rewardQueuedFrame = 0;
// FIX (review HD #4 / API #4): reward items handed to ItemService and not granted yet. ItemService cannot take a
// queued give back, so HD's Withdraw (which puts the "Save and begin" save back) waits until they are granted: the
// restore then drops them, as HD's reload drops the reward.
std::uint8_t g_rewardGives[4] = {};
int g_rewardGiveCount = 0;
int g_rewardWaitTicks = 0;          // ticks the ready Withdraw has waited for them (the host drops a give after 5
                                    // failed dispatches without telling anyone, so the wait is limited)

bool g_pendingFormRestore = false;
std::uint8_t g_formToRestore = 0;

int g_assetsCheckTimer = 0;
bool g_assetsOk = false;

int g_transformToastCooldown = 0;

std::vector<std::string> g_hookFailures;
bool g_entryHookFailed = false;     // a hook the entry needs (amiibo button) failed: show Enter button
bool g_criticalHookFailed = false;  // a hook the cave needs to be safe failed: refuse entry
bool g_flowHookFailed = false;      // dMsgFlow_c::init hook missing: no HD flows in the game
bool g_portalHookFailed = false;    // skipPortalObjWarp hook missing: never start a portal warp
bool g_tierHookFailed = false;      // daAlldie_c::actionTimer hook missing: GC ALLdie (every tier unlocked)
bool g_timerHookFailed = false;     // daSwc00_c::execute hook missing: the run timer runs the whole visit

// Tunables (game ticks, 30 per second).
constexpr int kPhaseTimeoutTicks = kTicksPerSecond * 30;
constexpr int kWithdrawWaitTicks = kTicksPerSecond * 20;    // wait up to 20 s for a safe moment
constexpr int kMenuEnterWaitTicks = kTicksPerSecond * 15;   // pause menu must close within 15 s
constexpr int kEntryBusyWaitTicks = kTicksPerSecond * 20;   // ... then a cutscene / message has 20 s to end
constexpr int kMenuSettleTicks = 10;                        // let the menu's closing finish
constexpr int kMenuCloseRetryTicks = 15;                    // B retry interval while the screen stays idle
constexpr int kAssetsRecheckTicks = 60;
constexpr std::uint64_t kRewardRetryFrames = 30ull * 60 * 5;  // re-queue a lost wallet after 5 min
constexpr int kWarpStartTicks = 12;       // checkWarpStart must consume the warp status by then
constexpr int kWarpEventTicks = 12;       // ... and the portal event must be running by then
constexpr int kWarpTimeoutTicks = kTicksPerSecond * 20;
constexpr int kRewardTalkWaitTicks = kTicksPerSecond * 25;  // reward talk fallback
constexpr std::uint8_t kMaxStoredHearts = 20;
constexpr int kWithdrawCutsceneWaitTicks = kTicksPerSecond * 60;  // flow 1120's Withdraw waits for its cutscene
constexpr int kSaveHoldTimeoutTicks = kTicksPerSecond * 3;  // held "Saved." menu: the scene change must start by then
constexpr int kRewardGiveWaitTicks = kTicksPerSecond * 10;  // a ready HD Withdraw waits this long for a reward give

// Event flags (raw dSv_event_c bit values, VERIFIED in src/dusk/imgui/ImGuiEventFlags.hpp).
constexpr u16 kFlagFaronSpiritRevived = 0x0610;  // M_019 (HD label 823: amiibo use unlocked)
constexpr u16 kFlagLakebedCleared = 0x0904;      // M_045 (saveBitLabels[78])
constexpr u16 kFlagMidnaRevived = 0x1E08;        // F_0250 (Zelda reunion, Midna revived; saveBitLabels[250])
constexpr u16 kFlagZantAppears = 0x0C01;         // M_071 (Zant appears, Midna's desperate hour; saveBitLabels[104])
constexpr u16 kFlagShadCannonSeen = 0x5F10;      // F_0786 (Shad sees the sky cannon; saveBitLabels[786])
constexpr u16 kFlagShadCannonLeft = 0x5F20;      // F_0785 (Shad leaves after trying to warp it; saveBitLabels[785])
// HD section unlocks (VERIFIED, HD tier ALLdie 0x020d4098 / 0x020d40cc). F_0685 = "(Cutscene 32) Sage
// appears, get first Mirror of Twilight shard", the post-Arbiter's Grounds Mirror Chamber scene with
// Ganondorf's execution (HD label 825 = the same bit 0x5420): with section 1 cleared it opens section 2.
// Section 3 needs section 2 cleared and HD 0x7101 (label 827: the Sols' power is in the sword; set only
// by HD's own flow 811 node 1173); NOT F_0570 (Palace cleared), which no HD code
// reads. GC's flow 811 has no such node, so sols_in_sword() stands in for it.
constexpr u16 kFlagMirrorShard = 0x5420;         // F_0685 (HD label 825)

// HD labels >= 822 (HD label table 0x1009F88C, VERIFIED: research/cos_sections_spec.md §1).
constexpr std::uint16_t kLabelReward = 822;      // 0x7008 Great Fairy reward given
constexpr std::uint16_t kLabelAmiibo = 823;      // 0x0610 = M_019 (Faron spirit): amiibo usable
constexpr std::uint16_t kLabelTier1 = 824;       // 0x7001 section 1 cleared (floor-6 portal)
constexpr std::uint16_t kLabelMirror = 825;      // 0x5420 = F_0685
constexpr std::uint16_t kLabelTier2 = 826;       // 0x7002 section 2 cleared (floor-20 portal)
constexpr std::uint16_t kLabelSols = 827;        // 0x7101 the Sols' power is in the Master Sword
constexpr std::uint16_t kLabelTier3 = 828;       // 0x7004 section 3 cleared (floor-41 portal)
constexpr std::uint16_t kLabelIntro = 829;       // 0x7080 arrival intro pending (R19 TagEv valid label)

// GC stand-in for HD 0x7101 (label 827). VERIFIED in the decomp: the Sol cradle cutscene in the Palace of
// Twilight (d_a_obj_swBallC.cpp:239-240, cut 10) runs execItemGet(dItemNo_LIGHT_SWORD_e); getItemFunc
// sets the item's first-get bit (d_item.cpp:276) and item_func_LIGHT_SWORD sets the collect bit
// COLLECT_LIGHT_SWORD (d_item.cpp:831-834). Outside debug code nothing clears either (the only
// offItemFirstBit(LIGHT_SWORD) is the debug all-items routine, the only offCollectSword the HIO menu).
// INFERRED: it is the same scene as HD flow 811 (Midna's line HD 9828, HD msgs 9828-9831 = GC flow 811
// msgs 1165-1168), which plays right after the sword is
// powered; the cave cannot be entered in between (both are one cutscene, and the pause menu cannot open
// during a cutscene: dMw_c::isEventCheck, d_menu_window.cpp:1511-1522).
bool sols_in_sword() {
    return dComIfGs_isItemFirstBit(dItemNo_LIGHT_SWORD_e) != 0 ||
           dComIfGs_isCollectSword(COLLECT_LIGHT_SWORD) != 0;
}

// ---------------------------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------------------------

void refresh_mode() {
    const char* name = g_mDoMemCd_control.getFileName();
    const bool vanilla = name != nullptr && std::strcmp(name, kVanillaSaveName) == 0;
    if (vanilla != g_vanilla) {
        mods::log::info("save name '{}': Cave of Shadows {}", name != nullptr ? name : "",
            vanilla ? "enabled" : "disabled (not the vanilla game mode)");
    }
    g_vanilla = vanilla;
}

bool cfg_bool(ConfigVarHandle var, bool fallback) {
    bool value = fallback;
    if (var != 0) {
        svc_config->get_bool(mod_ctx, var, &value);
    }
    return value;
}

std::int64_t cfg_int(ConfigVarHandle var, std::int64_t fallback) {
    std::int64_t value = fallback;
    if (var != 0) {
        svc_config->get_int(mod_ctx, var, &value);
    }
    return value;
}

bool hd_sections() {
    return cfg_bool(g_cvarHdSections, true);
}

bool hd_withdraw() {
    return cfg_bool(g_cvarHdWithdraw, true);
}

bool start_deepest() {
    return cfg_bool(g_cvarStartDeepest, false);
}

bool entry_warp_option() {
    return cfg_bool(g_cvarEntryWarp, false);
}

// Blobs written before the HD section rules (cos_sections_spec.md M2/M3, idempotent):
//  - INTRO_FIRST (HD 0x7010) is new: any earlier visit means the first-entry line was already said.
//  - FLAG_SECTION3_DONE (HD 0x7004) is new: a full clear used to be counted only in clear_count.
//  - The notice guards are new: HD shows 1545 at the first Collection screen after M_019 and 1546/1547 at
//    the first one after their condition, i.e. always before the entry whose bookkeeping announces that
//    section (an entry starts on the Collection screen). An announced bit therefore means "shown".
void migrate_sections(Blob& b) {
    const bool visited = b.best_floor > 0 || b.clear_count > 0 || b.runs > 0 ||
                         (b.flags & (FLAG_SECTION1_DONE | FLAG_SECTION2_DONE | FLAG_SECTION3_DONE)) != 0 ||
                         (b.intro_flags & (INTRO_SECTION2 | INTRO_SECTION3)) != 0;
    if (visited) {
        b.intro_flags |= INTRO_FIRST;
    }
    if (b.clear_count > 0) {
        b.flags |= FLAG_SECTION3_DONE;
    }
    if ((b.intro_flags & INTRO_FIRST) != 0) {
        b.notice_flags |= NOTICE_1545;
    }
    if ((b.intro_flags & INTRO_SECTION2) != 0) {
        b.notice_flags |= NOTICE_1546;
    }
    if ((b.intro_flags & INTRO_SECTION3) != 0) {
        b.notice_flags |= NOTICE_1547;
    }
}

// Blobs written before the Record screen have no HD record (0xA98..0xAA4): the last run that ended at a portal
// (the one the old record notice showed) becomes the previous record, as HD's screen would have written it.
void migrate_record(Blob& b) {
    const auto end = static_cast<RunEnd>(b.last.end);
    if (b.hdrec.life != 0 || (end != RunEnd::SectionEnd && end != RunEnd::FullClear) || b.last.hearts == 0) {
        return;
    }
    b.hdrec.time_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(b.last.ticks) * 1000 / kTicksPerSecond, 0xFFFFFFFFu));
    b.hdrec.damage = static_cast<std::int16_t>(std::min<std::uint16_t>(b.last.damage, 0x7FFF));
    b.hdrec.floor = b.last.floorTo;
    b.hdrec.life = static_cast<std::uint8_t>(std::min(b.last.hearts * 4, 0xFF));
    b.hdrec.combo = static_cast<std::uint8_t>(std::min<std::uint16_t>(b.last.combo, 0xFF));
}

bool load_blob() {
    g_blob = default_blob();
    std::size_t size = 0;
    if (svc_save->get_blob(mod_ctx, kBlobName, nullptr, &size) != MOD_OK || size == 0) {
        return false;
    }
    std::vector<unsigned char> raw(size);
    std::size_t got = raw.size();
    if (svc_save->get_blob(mod_ctx, kBlobName, raw.data(), &got) != MOD_OK) {
        return false;
    }
    if (got < kDesignPrefixSize) {
        mods::log::warn("cave blob too small ({} bytes); using defaults", got);
        return true;
    }
    Blob blob = default_blob();
    std::memcpy(&blob, raw.data(), std::min(got, sizeof(Blob)));
    if (blob.version == 0) {
        mods::log::warn("cave blob has version 0; using defaults");
        return true;
    }
    if (got < kBaseSize) {
        // Only the DESIGN.md prefix: no exact return position.
        blob.flags &= ~FLAG_HAS_RETURN;
    }
    blob.version = kBlobVersion;
    blob.ret_stage[sizeof(blob.ret_stage) - 1] = '\0';
    if (blob.best_floor > kLastFloor) {
        blob.best_floor = kLastFloor;
    }
    if (blob.ret_form > 1) {
        blob.ret_form = 0;
    }
    if (blob.stored_hearts > kMaxStoredHearts) {
        blob.stored_hearts = kMaxStoredHearts;
    }
    migrate_sections(blob);
    migrate_record(blob);
    g_blob = blob;
    return true;
}

daPy_py_c* link_player() {
    return dComIfGp_getLinkPlayer();
}

daAlink_c* alink() {
    return daAlink_getAlinkActorClass();
}

void copy_stage_name(char (&dst)[8], const char* src) {
    std::memset(dst, 0, sizeof(dst));
    if (src == nullptr) {
        return;
    }
    for (std::size_t i = 0; i + 1 < sizeof(dst) && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
}

// A field stage the mod's own exits ("Leave the cave", the switch-off, ...) may return to at the exact entry spot
// (exit_to_return_point). It is no longer an entry rule: HD's entry check is a deny-list (hd_entry_rule).
bool is_overworld_stage(const char* name) {
    if (name == nullptr || std::strncmp(name, "F_SP", 4) != 0) {
        return false;
    }
    // F_SP102 = title-screen stage, F_SP200 = Hidden Skill training (randomizer stages.cpp),
    // F_SP127 = the fishing hole (no field play there).
    return std::strcmp(name, "F_SP102") != 0 && std::strcmp(name, "F_SP200") != 0 &&
           std::strcmp(name, "F_SP127") != 0;
}

bool assets_present() {
    if (g_assetsCheckTimer > 0) {
        return g_assetsOk;
    }
    g_assetsCheckTimer = kAssetsRecheckTicks;
    // The stored game files are valid and their 56 overlays are live (hdfiles/files.hpp, plan 2.7). Overlay-added
    // files get stable EntryNums in Aurora's FST (extern/aurora/include/aurora/dvd.h), so the stage archives are
    // visible to DVDConvertPathToEntrynum like disc files.
    g_assetsOk = hdfiles::ready() && DVDConvertPathToEntrynum("/res/Stage/D_SB11/STG_00.arc") >= 0 &&
                 DVDConvertPathToEntrynum("/res/Stage/D_SB11/R19_00.arc") >= 0 &&
                 DVDConvertPathToEntrynum("/res/Stage/D_SB11/R14_00.arc") >= 0 &&
                 DVDConvertPathToEntrynum("/res/Particle/Pscene206.jpc") >= 0;
    return g_assetsOk;
}

bool scene_busy() {
    return dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() != 0;
}

void play_se(u32 id) {
    mDoAud_seStart(id, nullptr, 0, 0);
}

// FIX (review "cave entry" #4): dComIfGp_getMesgStatus() reads dComIfG_play_c::mMesgStatus, which nothing
// in v2.0.3 ever writes (always 0). The message object's own status is what the game's actors test
// (dMsgObject_isTalkNowCheck: status 1 = idle). The stage / boss title cards and the staff credits are
// passive (they show over normal play and do not stop the pause menu from opening), so they do not count.
bool message_box_up() {
    if (!dMsgObject_isTalkNowCheck()) {
        return false;
    }
    dMsgObject_c* object = dComIfGp_getMsgObjectClass();
    return object != nullptr && !object->isPlaceMessage() && !object->isBossMessage() &&
           !object->isStaffMessage();
}

// The Collection screen is up and ran its idle process in the previous or this tick (mod_update runs
// before the menu processes in a frame, so the hook of the previous frame left g_frame - 1).
bool collect_idle() {
    return g_collectOpen && g_frame - g_collectIdleFrame <= 2;
}

void give(std::uint8_t item) {
    const ModResult result = svc_item->give_item(mod_ctx, nullptr, item, 0);
    if (result != MOD_OK) {
        mods::log::error("give_item(0x{:02X}) failed (result {})", item, static_cast<int>(result));
    } else {
        mods::log::info("queued reward item 0x{:02X}", item);
        if (g_rewardGiveCount < static_cast<int>(sizeof(g_rewardGives))) {
            g_rewardGives[g_rewardGiveCount++] = item;
        }
    }
}

std::string hd(std::uint8_t file, std::uint16_t id, const char* fallback) {
    return hdtext::text_or(file, id, fallback);
}

// One line of HD text (message-box line breaks become spaces). hd_plain for plain-text toast titles and
// bodies (the host shows them as text unless they start with '<'), hd_rml inside RML.
std::string hd_plain(std::uint8_t file, std::uint16_t id, const char* fallback) {
    std::string text = hd(file, id, fallback);
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
        } else {
            out.push_back(text[i]);
        }
    }
    while (!out.empty() && out.front() == '<') {
        out.erase(out.begin());  // never let plain text switch the toast to RML
    }
    return out;
}

std::string hd_rml(std::uint8_t file, std::uint16_t id, const char* fallback) {
    return amiibo::escape(hd_plain(file, id, fallback));
}

void swallow_buttons(std::uint32_t mask) {
    g_swallowMask = mask;
    g_swallowTicks = kTicksPerSecond * 3;
    g_swallowMin = 8;
}

std::string format_time(std::uint32_t ticks) {
    const std::uint32_t seconds = ticks / kTicksPerSecond;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    return buf;
}

std::string format_damage(std::uint32_t quarters) {
    char buf[32];
    if (quarters % 4 == 0) {
        std::snprintf(buf, sizeof(buf), "%u", quarters / 4);
    } else {
        std::snprintf(buf, sizeof(buf), "%u %u/4", quarters / 4, quarters % 4);
    }
    return buf;
}

// ---------------------------------------------------------------------------------------------
// HD sections (research/cos_sections_spec.md). Every visit starts on floor 1 (room 19); the story state
// only decides what the section-end floors 5/6 and 19/20 do (alldie_tier_switch).
// ---------------------------------------------------------------------------------------------

bool label_set(std::uint16_t label) {
    return cave::hd_label(label);
}

// HD's unlock conditions (tier ALLdie 0x020d4098 / 0x020d40cc, query055, the bookkeeping, the notices).
bool section2_unlocked() {
    return label_set(kLabelTier1) && label_set(kLabelMirror);  // 0x7001 && 0x5420
}

bool section3_unlocked() {
    return label_set(kLabelTier2) && label_set(kLabelSols);    // 0x7002 && 0x7101
}

// Deepest floor the next run can reach (status texts): the rest floor of the first locked section.
int reachable_floor() {
    if (!hd_sections() || g_tierHookFailed || section3_unlocked()) {
        return kLastFloor;  // (without the tier ALLdie hook GC's ALLdie opens every floor)
    }
    return section2_unlocked() ? floor_of_room(kRestRoom2) : floor_of_room(kRestRoom1);
}

// FIX (HD review L3): per room. The floor-5 and floor-19 talk tags are two separate stage edits; if only one of
// them could be added, only that floor may wait for its talk (the other one opens with the notice).
bool talk_tag_active(int room) {
    return hdtext::talk_tag_in_room(hdtext::kFlowSectionEnd, room);
}

// HD flow 1121's choice (VERIFIED re-dump, spec §4.2): 827 set -> 5067; else F_0685 clear -> 5066; else
// 824 set -> 5066, otherwise 5067. Used for the notice when the talk tag could not be added.
std::uint16_t section_end_message() {
    if (label_set(kLabelSols)) {
        return 5067;
    }
    if (!label_set(kLabelMirror)) {
        return 5066;
    }
    return label_set(kLabelTier1) ? 5066 : 5067;
}

void section_end_notice() {
    const std::uint16_t id = section_end_message();
    const std::string body = id == 5067 ? hd_plain(hdtext::kZel05, 5067,
                                              "Come back when you want a harder test.")
                                        : hd_plain(hdtext::kZel05, 5066,
                                              "Come back when you are stronger.");
    toast(hd_plain(hdtext::kZel00, 413, "Cave of Shadows").c_str(), body.c_str(), false, 8000);
}

// HD's entry bookkeeping (0x0290ad78-0x0290aef4, VERIFIED), run when the cave controller is created: on
// every amiibo entry and on every death-continue (spec-check correction A; after a death the announced
// bits are already set, so it ends in the last branch and clears 829). Sets 829 only on an entry that
// newly announces something, which is what lets floor 1's arrival intro (flow 1120) play.
void entry_bookkeeping() {
    std::uint8_t& intro = g_blob.intro_flags;
    const char* what = "nothing new";
    bool pending = true;
    if (section3_unlocked() && (intro & INTRO_SECTION3) == 0) {
        intro |= INTRO_SECTION3 | INTRO_SECTION2 | INTRO_FIRST;  // on 0x7040, 0x7020, 0x7010, 0x7080
        what = "section 3 announced";
    } else if (section2_unlocked() && (intro & INTRO_SECTION2) == 0) {
        intro |= INTRO_SECTION2 | INTRO_FIRST;  // on 0x7020, 0x7010, 0x7080
        what = "section 2 announced";
    } else if (label_set(kLabelAmiibo) && (intro & INTRO_FIRST) == 0) {
        intro |= INTRO_FIRST;  // on 0x7010, 0x7080
        what = "first entry";
    } else {
        pending = false;  // off 0x7080
    }
    set_hd_label(kLabelIntro, pending);
    mods::log::info("cave entry bookkeeping: {} (label 829 {}, announced 0x{:X})", what,
        pending ? "set" : "clear", static_cast<unsigned>(intro));
}

// HD Collection-screen notices (0x029bc3ec, VERIFIED): at most one per call, in this order, each once per
// save (guard bytes gameInfo+0xAB8/9/A): 1545 when M_019; once that one was shown, 1546 when 0x7001 &&
// F_0685; once that one was shown, 1547 when 0x7002 && 0x7101. HD shows them on the GamePad; here they are
// a notice while the Collection screen is up. Returns the message id shown (0 = none).
std::uint16_t collection_notice() {
    std::uint8_t& shown = g_blob.notice_flags;
    std::uint16_t id = 0;
    if ((shown & NOTICE_1545) == 0) {
        if (label_set(kLabelAmiibo)) {
            shown |= NOTICE_1545;
            id = 1545;
        }
    } else if ((shown & NOTICE_1546) == 0) {
        if (section2_unlocked()) {
            shown |= NOTICE_1546;
            id = 1546;
        }
    } else if ((shown & NOTICE_1547) == 0) {
        if (section3_unlocked()) {
            shown |= NOTICE_1547;
            id = 1547;
        }
    }
    if (id == 0) {
        return 0;
    }
    const char* fallback = id == 1545 ? "Wolf Link amiibo: ready on the Collection screen."
                                      : "Cave of Shadows: new floors are open.";
    toast("Wolf Link amiibo", hd_plain(hdtext::kZel00, id, fallback).c_str(), false, 5000);
    mods::log::info("Collection-screen notice {}", id);
    return id;
}

// ---------------------------------------------------------------------------------------------
// Entry: port of tphd LosTable.cpp set_los_next_stage(), split in two so the Midna portal warp-out can
// play in between (capture the return point first, change the stage when the warp-out ends).
// ---------------------------------------------------------------------------------------------

void capture_return_point() {
    // The restart-room data is what the game itself uses to respawn the player in this stage (set on
    // spawn, d_a_alink.cpp:5044, and by restart tags), and the same mechanism Ooccoo's dungeon return
    // warp uses (daAlink_c::dungeonReturnWarp).
    Blob& b = g_blob;
    copy_stage_name(b.ret_stage, dComIfGp_getStartStageName());
    b.ret_point = dComIfGp_getStartStagePoint();
    b.ret_room = dComIfGs_getRestartRoomNo();
    b.ret_layer = dComIfGp_getStartStageLayer();
    const cXyz& pos = dComIfGs_getRestartRoomPos();
    b.ret_pos[0] = pos.x;
    b.ret_pos[1] = pos.y;
    b.ret_pos[2] = pos.z;
    b.ret_angle = dComIfGs_getRestartRoomAngleY();
    b.ret_form = dComIfGs_getTransformStatus() != 0 ? 1 : 0;
    b.flags |= FLAG_HAS_RETURN;
}

// HD: every visit starts on floor 1 (room 19). With the non-HD option "cave-start_deepest" on (and the HD
// sections on), a visit starts on the first floor of the deepest section that is unlocked by HD's own rule
// (the tier-ALLdie conditions: section 2 = 824 + 825, section 3 = 826 + 827).
int entry_room() {
    // While Midna is gravely ill, floor 1's arrival (flow 1120) must send the player back, as in HD: the non-HD
    // option never skips it.
    if (!start_deepest() || !hd_sections() || midna_gravely_ill()) {
        return kEntryRoom;
    }
    if (section3_unlocked()) {
        return kSection3Room;
    }
    if (section2_unlocked()) {
        return kSection2Room;
    }
    return kEntryRoom;
}

void do_enter_stage() {
    const int startRoom = entry_room();
    mods::log::info("entering D_SB11 room {} (floor {}) from {} (restart room {}, layer {}, form {})", startRoom,
        floor_of_room(startRoom),
        g_blob.ret_stage, static_cast<int>(g_blob.ret_room), static_cast<int>(g_blob.ret_layer),
        static_cast<int>(g_blob.ret_form));
    // HD's amiibo entry (Zelda.rpx 0x02a020a8-0x02a02130, VERIFIED): the room of los floor 1 (room 19),
    // point 0, default layer, start event 0xCA (DEFAULT_START), life = max. HD has no section start
    // points: every visit starts on floor 1 (cos_sections_spec.md §0.1, M1).
    dComIfGp_setNextStage(kStageName, 0, static_cast<s8>(startRoom), -1, 0.0f, 0, 1, 0, 0, 1, 0);
    g_dComIfG_gameInfo.info.getRestart().setLastSceneInfo(20.0f, 0, 0);
    dComIfGs_setTurnRestart(dComIfGs_getTurnRestartPos(), 0, 0, daPy_py_c::setParamData(0, 1, 0xCA, 0));
    // DEVIATION: tphd writes dComIfGs_getMaxLife() (heart pieces, 5 per heart) into the life
    // counter (quarter hearts). Use the gauge value like d_meter2_draw.cpp:878 does.
    g_undoLife = dComIfGs_getLife();  // undo_entry(): the life before, if this stage change never happens
    g_undoLifeValid = true;
    dComIfGs_setLife(dComIfGs_getMaxLifeGauge());
    // HD's cave controller is created when D_SB11 loads and snapshots life (now full) and rupees (0x0290ad50).
    g_snap.valid = true;
    g_snap.life = dComIfGs_getLife();
    g_snap.rupee = dComIfGs_getRupee();
    g_arrivalRoom = startRoom;
    g_tier = 0;           // a new cave controller (HD los+0x1E = 0 at creation)
    g_restartRun = false;
    // HD runs the bookkeeping when D_SB11 loads and its controller is created; here it runs with the stage
    // change it belongs to, and is undone if that stage change never happens (tick, Phase::Entering).
    g_undoIntroFlags = g_blob.intro_flags;
    g_undoHdLabels = g_blob.hd_labels;
    entry_bookkeeping();
    g_phase = Phase::Entering;
    g_phaseTimer = 0;
}

// The stage change do_enter_stage issued will not happen (timeout, or the mod shutting down during its fade): put
// back what the entry changed. The bookkeeping belongs to an arrival (HD runs it when D_SB11 loads), and HD sets the
// life to full only together with the stage change. FIX (review HD #6): the shutdown path did not do this.
void undo_entry() {
    g_blob.intro_flags = g_undoIntroFlags;
    g_blob.hd_labels = g_undoHdLabels;
    set_hd_label(kLabelIntro, false);
    if (g_undoLifeValid) {
        dComIfGs_setLife(std::min<std::uint16_t>(g_undoLife, dComIfGs_getMaxLifeGauge()));
        g_undoLifeValid = false;
    }
    g_snap = Snapshot{};
    g_arrivalRoom = -1;
}

// Reload the spot the player stands in (the stage change to D_SB11 must not happen any more: the mod is shutting
// down). FIX (review API #2a): start point -1 takes the room from the restart-room parameter (d_stage.cpp:1709-1712,
// 1797-1801), so it is set as GC's own callers of point -1 set it (daAlink_c::dungeonReturnWarp,
// d_a_alink_demo.inc:4390-4400; exit_to_return_point below).
void reload_current_spot() {
    const s8 room = static_cast<s8>(dComIfGs_getRestartRoomNo());
    const cXyz pos = dComIfGs_getRestartRoomPos();
    const s16 angle = dComIfGs_getRestartRoomAngleY();
    char stage[8];
    copy_stage_name(stage, dComIfGp_getStartStageName());
    dComIfGp_offEnableNextStage();
    dComIfGs_setRestartRoom(pos, angle, room);
    // Layer -1, as GC's own point -1 restarts (dStage_restartRoom, d_stage.cpp:3070-3074): an explicit cutscene layer
    // of the last stage change must not come back (research/cos_entryrules/gc_safety.md F1).
    dComIfGp_setNextStage(stage, -1, room, -1, 0.0f, 0, 0, 0, 0, 1, 0);
    dComIfGs_setRestartRoomParam(daPy_py_c::setParamData(room, 0, 0xCA, 0));
    mods::log::info("reloading {} room {} instead of entering the cave", stage, static_cast<int>(room));
}

// ---------------------------------------------------------------------------------------------
// Midna's portal warp (GC map warp), used for Midna's "Withdraw", "Leave the cave" and (non-HD option
// "cave-entry_warp") the amiibo entry. Path, all exported game code:
//   dMeter2Info_setWarpInfo + dMeter2Info_setWarpStatus(WARP_STATUS_DECIDED_e) (what the map screen
//   does) -> daAlink_c::checkWarpStart (player execute) orders the common Event.arc event
//   "POTAL_WARPIN" (VERIFIED in the GC Event.arc: Link 057wchange + 045warp prm1=1, Midna 09pwout,
//   camera) -> daAlink_c::procCoWarp (twilight warp-out effect, Z2SE_WOLF_WARP_OUT as wolf) ->
//   daAlink_c::skipPortalObjWarp, whose stage change (start point -4, which needs a portal actor at the
//   destination) is replaced by the mod's own (hooks.cpp pre-hook, see on_portal_warp_end()).
// When anything in that chain does not happen in time, the stage change is done directly (fade).
// ---------------------------------------------------------------------------------------------

bool warp_effect_enabled() {
    return !g_portalHookFailed && cfg_bool(g_cvarWarpEffect, true);
}

void begin_warp(WarpKind kind, const char* stage, const cXyz& pos, s16 angle, int room) {
    g_warp = Warp{};
    g_warp.kind = kind;
    // TargetWarpPt selects the special bridge/cannon portal events; 0xFF = the plain portal warp
    // (what checkWarpStart itself sets when Midna rides).
    dComIfGp_TargetWarpPt_set(0xFF);
    dMeter2Info_setWarpInfo(stage, pos, angle, static_cast<u8>(room), 0, 0);
    dMeter2Info_setWarpStatus(WARP_STATUS_DECIDED_e);
    mods::log::info("portal warp-out requested ({})", kind == WarpKind::Enter ? "entry" : "withdraw");
}

// GC's map refuses every portal warp while Midna is gravely ill (M_071 set, F_0250 clear: checkWarpAcceptRegion4,
// d_menu_fmap2D.cpp:3213-3219, message 2141). checkAcceptWarp has no such test
// (d_a_alink_demo.inc:4344-4386), so the mod tests it itself (research/cos_entryrules/gc_safety.md F2).
bool gc_ill_warp_refused() {
    return dComIfGs_isEventBit(kFlagZantAppears) && !dComIfGs_isEventBit(kFlagMidnaRevived);
}

// GC's own "can Midna's portal warp take the player now" test (the map screen uses it), plus the map's own refusals
// that checkAcceptWarp lacks: the ill window and Shad's sky-cannon scene (checkWarpAcceptCannon, 3224-3233). Only the
// non-HD "warp-out on entry" option uses it; when it is false the entry is HD's plain one.
bool portal_entry_allowed() {
    daAlink_c* link = alink();
    return link != nullptr && daPy_py_c::getMidnaActor() != nullptr && link->checkAcceptWarp() &&
           link->mGrabItemAcKeep.getActor() == nullptr && !gc_ill_warp_refused() &&
           !(dComIfGs_isEventBit(kFlagShadCannonSeen) && !dComIfGs_isEventBit(kFlagShadCannonLeft));
}

// checkAcceptWarp() also requires a field stage, so inside D_SB11 the same physical conditions are
// checked directly: wolf with Midna riding, standing on the ground, not swimming/climbing/jumping, and not while
// Midna is gravely ill (GC never plays a warp-out then; HD's event044 plays only its two sounds anyway).
bool portal_withdraw_allowed() {
    daAlink_c* link = alink();
    return link != nullptr && daPy_py_c::getMidnaActor() != nullptr && link->checkWolf() &&
           link->checkMidnaRide() && link->mLinkAcch.ChkGroundHit() && !link->checkPlayerFly() &&
           !link->checkNoResetFlg0(daPy_py_c::FLG0_WATER_IN_MOVE) && !link->checkRide() &&
           link->mGrabItemAcKeep.getActor() == nullptr && !gc_ill_warp_refused();
}

// Why Midna's warp-out cannot play right now (portal_withdraw_allowed() false), for the log.
const char* withdraw_warp_refusal() {
    daAlink_c* link = alink();
    if (gc_ill_warp_refused()) {
        return "Midna is gravely ill";
    }
    if (link == nullptr) {
        return "no player";
    }
    if (daPy_py_c::getMidnaActor() == nullptr) {
        return "no Midna actor";
    }
    if (!link->checkWolf() || !link->checkMidnaRide()) {
        return "Midna is not on the wolf's back";
    }
    if (!link->mLinkAcch.ChkGroundHit() || link->checkPlayerFly()) {
        return "not standing on the ground";
    }
    if (link->checkNoResetFlg0(daPy_py_c::FLG0_WATER_IN_MOVE)) {
        return "in water";
    }
    if (link->checkRide()) {
        return "riding";
    }
    return link->mGrabItemAcKeep.getActor() != nullptr ? "carrying something" : "unknown";
}

void finish_withdraw_stage(std::int16_t angle) {
    exit_to_return_point(0, 0xCA, 0.0f, angle, ExitCause::Withdraw);
}

void finish_hd_withdraw();  // HD's Withdraw: the "Save and begin" save's return place (below)

void warp_fallback() {
    if (g_warp.fallback || g_warp.finished) {
        return;
    }
    g_warp.fallback = true;
    if (dMeter2Info_getWarpStatus() == WARP_STATUS_DECIDED_e) {
        dMeter2Info_resetWarpStatus();
    }
    mods::log::warn("portal warp-out did not happen; changing the stage directly");
    if (g_warp.kind == WarpKind::Enter) {
        g_warp.finished = true;
        do_enter_stage();
    } else if (g_warp.kind == WarpKind::Withdraw) {
        g_warp.finished = true;
        if (daAlink_c* link = alink()) {
            link->seStartOnlyReverb(Z2SE_WOLF_WARP_OUT);
        }
        if (g_restoreWithdraw) {
            finish_hd_withdraw();
        } else {
            finish_withdraw_stage(0);
        }
    }
}

void tick_warp() {
    if (g_warp.kind == WarpKind::None || g_warp.finished) {
        return;
    }
    ++g_warp.ticks;
    const u8 status = dMeter2Info_getWarpStatus();
    if (g_warp.consumedAt < 0 && status != WARP_STATUS_DECIDED_e) {
        g_warp.consumedAt = g_warp.ticks;  // checkWarpStart ordered the event (and reset the status)
    }
    if (g_warp.consumedAt < 0 && g_warp.ticks > kWarpStartTicks) {
        warp_fallback();  // the player did not run checkWarpStart (no player, frozen, ...)
    } else if (g_warp.consumedAt >= 0 && !dComIfGp_event_runCheck() &&
               g_warp.ticks - g_warp.consumedAt > kWarpEventTicks)
    {
        warp_fallback();  // the event order was refused
    } else if (g_warp.ticks > kWarpTimeoutTicks) {
        warp_fallback();
    }
}

// ---------------------------------------------------------------------------------------------
// Run record (HD "Cave of Shadows Record": hearts, time, damage, max combo; amiibo heart storage).
// ---------------------------------------------------------------------------------------------

void start_run() {
    g_run = Run{};
    g_run.active = true;
    // HD's timer starts at the floor-1 type-10 area; without the swc00 hook the whole visit counts.
    g_run.timer = g_timerHookFailed ? CaveTimer::Running : CaveTimer::Fresh;
    if (g_arrivalRoom >= 0 && g_arrivalRoom != kEntryRoom) {
        g_run.timer = CaveTimer::Running;  // non-HD deeper start: no floor-1 type-10 area to start it
    }
    // Right after the stage change the room control can still report the previous stage's room
    // (seen in testing: overworld F_SP125 room 4 read as cave floor 29), so prefer the known arrival room.
    const int room = g_arrivalRoom >= 0 ? g_arrivalRoom : dComIfGp_roomControl_getStayNo();
    const int floor = floor_of_room(room);
    g_run.floorFrom = static_cast<std::uint8_t>(floor > 0 ? floor : 1);
    g_run.floorTo = g_run.floorFrom;
}

void track_run(bool paused) {
    if (!g_run.active || paused) {
        return;
    }
    if (g_run.timer == CaveTimer::Running) {
        ++g_run.ticks;  // HD's cave timer ("Completion time")
    }
    const std::uint16_t life = dComIfGs_getLife();
    if (g_run.lifeKnown && life < g_run.lastLife) {
        g_run.damage += g_run.lastLife - life;
        g_run.combo = 0;
    }
    g_run.lastLife = life;
    g_run.lifeKnown = true;
}

std::string record_toast_body(const RunRecord& rec, bool newRecord) {
    std::string body;
    const auto row = [&](std::uint16_t labelId, const char* fallback, const std::string& value) {
        body += "<div class=\"cos-rec-row\"><span class=\"cos-rec-label\">" +
                hd_rml(hdtext::kZel00, labelId, fallback) + "</span><span class=\"cos-rec-value\">" + value +
                "</span></div>";
    };
    row(1255, "Time", format_time(rec.ticks));
    row(1256, "Best combo", std::to_string(rec.combo));
    row(1257, "Damage taken", amiibo::image("los_heart.png", "cos-rec-heart") + " x" + format_damage(rec.damage));
    row(1254, "With amiibo", amiibo::image("amiibo_wolflink.png", "cos-rec-figure") + " x" +
                                 std::to_string(rec.amiiboUsed));
    if (newRecord) {
        body += "<div class=\"cos-rec-new\">" + hd_rml(hdtext::kZel00, 1259, "New best!") + "</div>";
    }
    return body;
}

// `recordScreen`: HD's Record screen shows this run (record.cpp): no notice, and the virtual amiibo is written
// only when the player says Yes to 1261 there (HD proc 0x47), not here.
void end_run(RunEnd end, bool recordScreen = false) {
    if (!g_run.active) {
        return;
    }
    g_run.active = false;
    if (end == RunEnd::None) {
        return;  // left without an ending (reset to the title, file load): keep the last record
    }
    const std::uint16_t life = dComIfGs_getLife();
    RunRecord rec{};
    rec.ticks = g_run.ticks;
    rec.damage = static_cast<std::uint16_t>(std::min<std::uint32_t>(g_run.damage, 0xFFFF));
    rec.combo = g_run.maxCombo;
    rec.hearts = static_cast<std::uint8_t>(std::min<std::uint16_t>(life / 4, kMaxStoredHearts));
    rec.floorFrom = g_run.floorFrom;
    rec.floorTo = g_run.floorTo;
    rec.end = static_cast<std::uint8_t>(end);
    rec.amiiboUsed = g_run.heals;
    g_blob.last = rec;
    if (rec.combo > g_blob.best_combo) {
        g_blob.best_combo = rec.combo;
    }
    mods::log::info("run ended ({}): floors {}-{}, {}, damage {} quarters, combo {}, hearts {}",
        static_cast<int>(end), rec.floorFrom, rec.floorTo, format_time(rec.ticks), rec.damage, rec.combo,
        rec.hearts);
    if (end != RunEnd::SectionEnd && end != RunEnd::FullClear) {
        return;  // withdraw / game over: nothing is written to the amiibo
    }
    if (g_blob.runs < 0xFFFF) {
        ++g_blob.runs;
    }
    bool newRecord = false;
    if (end == RunEnd::FullClear &&
        (g_blob.best.end != static_cast<std::uint8_t>(RunEnd::FullClear) || rec.ticks < g_blob.best.ticks))
    {
        newRecord = g_blob.best.end == static_cast<std::uint8_t>(RunEnd::FullClear);
        g_blob.best = rec;
    }
    if (recordScreen) {
        return;  // HD: the Record screen shows it and asks before the figure is written
    }
    // HD: the Wolf Link amiibo keeps the hearts left at the end of a run (research/final_web.md).
    g_blob.stored_hearts = rec.hearts;
    // ... as HD's Record screen writes them to the figure (proc 0x42/0x47: the floor of the room the run
    // ended in, life and max life): the pre-entry screen's plate shows them (confirm.cpp).
    {
        const int exitFloor = floor_of_room(dComIfGp_roomControl_getStayNo());
        g_blob.amiibo_level = static_cast<std::uint8_t>(exitFloor > 0 && exitFloor <= kLastFloor ? exitFloor : rec.floorTo);
        g_blob.amiibo_life = static_cast<std::uint8_t>(std::min<std::uint16_t>(life, 0xFF));
        g_blob.amiibo_max = static_cast<std::uint8_t>(std::min<std::uint16_t>(dComIfGs_getMaxLife(), 0xFF));
    }
    const std::string title = hd_plain(hdtext::kZel00, 1258, "Cave of Shadows: your record");
    std::string body = record_toast_body(rec, newRecord);
    if (g_blob.amiibo_life > 0) {  // quarter hearts (review L8): a part heart is stored and healed too
        body += "<div>" + hd_rml(hdtext::kZel00, 1568,
                              "Hearts stored in your Wolf Link amiibo. To get them back, scan it on the "
                              "cave's Collection screen.") +
                "</div>";
    }
    UiToastDesc desc = UI_TOAST_DESC_INIT;
    desc.type = "cos-record";
    desc.title_rml = title.c_str();
    desc.body_rml = body.c_str();
    desc.duration_ms = 9000;
    amiibo::push_toast(desc);
}

// The virtual amiibo's data for HD's pre-entry screen (dFile_amiibo_c: level +0x11, life +0x12, max life
// +0x13). A fresh figure holds 0xFF there (life -1): HD then shows the screen without the plate; here
// that is "nothing stored" (life 0). Blobs written before the screen existed only have stored_hearts
// (whole hearts): life = those hearts, max life = at least that many containers, and the level from the
// last run if that run is the one that stored them.
// The life the virtual amiibo holds, in quarter hearts (HD's figure byte +0x12): what the pre-entry
// screen's plate shows and what the in-cave heal gives back. Older blobs only have whole stored hearts.
std::uint32_t stored_life_quarters() {
    if (g_blob.amiibo_life > 0) {
        return g_blob.amiibo_life;
    }
    return static_cast<std::uint32_t>(g_blob.stored_hearts) * 4;
}

confirm::AmiiboData amiibo_data() {
    confirm::AmiiboData data;
    if (g_blob.amiibo_life > 0) {
        data.level = g_blob.amiibo_level;
        data.life = g_blob.amiibo_life;
        data.maxLife = g_blob.amiibo_max;
    } else if (g_blob.stored_hearts > 0) {
        data.life = g_blob.stored_hearts * 4;
        data.maxLife = std::max<int>(g_blob.stored_hearts * 5, (dComIfGs_getMaxLife() / 5) * 5);
        const auto end = static_cast<RunEnd>(g_blob.last.end);
        data.level = (end == RunEnd::SectionEnd || end == RunEnd::FullClear) ? g_blob.last.floorTo : 0;
    }
    if (data.maxLife < ((data.life + 3) / 4) * 5) {
        data.maxLife = ((data.life + 3) / 4) * 5;  // never fewer containers than filled hearts
    }
    return data;
}

// ---------------------------------------------------------------------------------------------
// Arrival / leaving.
// ---------------------------------------------------------------------------------------------

void on_arrive() {
    g_phase = Phase::InCave;
    g_phaseTimer = 0;
    g_lastRoom = -1;
    g_roomSynced = false;
    g_toastedFloors = 0;
    g_rewardedThisVisit = false;
    g_rewardRoomTicks = 0;
    if (g_arrivalRoom < 0) {
        g_arrivalRoom = dComIfGp_roomControl_getStayNo();
    }
    if (g_warp.kind == WarpKind::Enter) {
        g_warp = Warp{};
    }
    g_undoLifeValid = false;  // the entry happened
    g_rewardGiveCount = 0;    // (a reward give from an earlier visit was handed out in the world, or dropped by the host)
    g_rewardWaitTicks = 0;
    if (confirm::save_held()) {
        confirm::forget_save_hold("arrived in the cave");  // (the dMw_c::_delete pre-hook normally let it go already)
    }
    // The pre-entry screen's music (HD lets the stage change end it): stop it if it is still playing.
    confirm::bgm_stop();
    start_run();
    mods::log::info("arrived in D_SB11 (return point {})",
        (g_blob.flags & FLAG_HAS_RETURN) ? g_blob.ret_stage : "none, will use the save point");
}

void on_leave(bool ours) {
    mods::log::info("left D_SB11 ({})", ours ? "return point" : "other route");
    if (ours && g_snap.valid) {
        // HD 0x0290acd8-e8: deleting the cave controller writes the entry life and the (portal-committed) rupees
        // back, whichever way the cave was left (portal, Withdraw); a reset or a file load replaces both anyway.
        if (g_snap.life > 0) {
            dComIfGs_setLife(std::min<std::uint16_t>(g_snap.life, dComIfGs_getMaxLifeGauge()));
        }
        dComIfGs_setRupee(std::min<std::uint16_t>(g_snap.rupee, dComIfGs_getRupeeMax()));
        mods::log::info("cave left: life {} and rupees {} from the cave's entry (HD)", g_snap.life, g_snap.rupee);
    }
    g_snap = Snapshot{};
    g_portalDash = false;
    g_portalExit = false;
    g_writeScan = false;
    g_restoreWithdraw = false;
    g_restorePending = false;
    g_tripValid = false;  // the "Save and begin" image belongs to this visit only
    g_visitSaved = false;
    if (g_run.active) {
        end_run(RunEnd::None);  // reset to the title / load / unknown route
    }
    g_phase = Phase::Idle;
    g_phaseTimer = 0;
    g_lastRoom = -1;
    g_roomSynced = false;
    g_pendingWithdraw = false;
    g_arrivalRoom = -1;
    g_tier = 0;  // HD deletes its cave controller when the play scene leaves D_SB11
    g_restartRun = false;
    g_warp = Warp{};
    set_hd_label(kLabelIntro, false);
    if (ours) {
        g_blob.flags &= ~FLAG_HAS_RETURN;  // stored at the next game save (the autosave on arrival)
    } else {
        // Left by another route (reset to the title, loading a file). Keep the return point for now:
        // if a save written inside the cave gets loaded, on_save_loaded uses it to restore the entry
        // form; tick() drops it once play resumes outside the cave.
        g_pendingFormRestore = false;
    }
}

// ---------------------------------------------------------------------------------------------
// Rewards (HD flow 1122 behaviour; the end fairy itself is not ported, her lines are the reward talk).
// ---------------------------------------------------------------------------------------------

void grant_colossal(const char* how) {
    if ((g_blob.flags & FLAG_COLOSSAL) != 0) {
        return;
    }
    g_blob.flags |= FLAG_COLOSSAL;
    // HD zel_00 412 (name) and 177 (get text); GC has no get-item scene for HD's item 0xDA.
    toast(hd_plain(hdtext::kZel00, 412, "Colossal Wallet").c_str(),
        hd_plain(hdtext::kZel00, 177, "Colossal Wallet: holds 9,999 rupees.").c_str(), false, 9000);
    mods::log::info("Colossal Wallet granted ({})", how);
}

// What the reward talk will do (reward_query): 0 = hand out a reward, 1 = cleared again without an
// empty bottle (nothing to give), 2 = nothing (already rewarded in this visit).
int reward_kind() {
    if (g_rewardedThisVisit) {
        return 2;
    }
    if ((g_blob.flags & FLAG_CLEARED_ONCE) == 0) {
        return 0;
    }
    return dComIfGs_checkEmptyBottle() > 0 ? 0 : 1;
}

void grant_reward() {
    if (g_rewardedThisVisit) {
        return;
    }
    g_rewardedThisVisit = true;
    Blob& b = g_blob;
    if (b.clear_count < 0xFFFF) {
        ++b.clear_count;
    }

    if ((b.flags & FLAG_CLEARED_ONCE) == 0) {
        // FIX (review "logic" #4): FLAG_CLEARED_ONCE is set when the queued wallet is really granted
        // (on_item_given). give_item only queues, and the host queue drops an entry after 5 failed
        // dispatch attempts or on a slot change; a lost reward is queued again on a later visit.
        if (g_rewardQueued != 0 && g_frame - g_rewardQueuedFrame < kRewardRetryFrames) {
            mods::log::info("wallet reward 0x{:02X} still queued", g_rewardQueued);
            return;
        }
        const u8 wallet = dComIfGs_getWalletSize();
        if (wallet == 0) {
            g_rewardQueued = dItemNo_WALLET_LV2_e;  // 0x35 Big Wallet
        } else if (wallet == 1) {
            g_rewardQueued = dItemNo_WALLET_LV3_e;  // 0x36 Giant Wallet
        } else {
            g_rewardQueued = 0;
            b.flags |= FLAG_CLEARED_ONCE;
            if ((b.flags & FLAG_COLOSSAL) == 0) {
                grant_colossal("first full clear");
            }
            return;
        }
        g_rewardQueuedFrame = g_frame;
        give(g_rewardQueued);
    } else if (dComIfGs_checkEmptyBottle() > 0) {
        give(dItemNo_FAIRY_DROP_e);  // 0x73 Great Fairy's Tears
    }
}

void on_item_given(ModContext*, const ItemGiveInfo* info, void*) {
    if (info == nullptr || info->check_name != nullptr ||
        (info->origin != ITEM_GIVE_ORIGIN_QUEUE && info->origin != ITEM_GIVE_ORIGIN_QUEUE_SILENT))
    {
        return;
    }
    for (int i = 0; i < g_rewardGiveCount; ++i) {
        if (g_rewardGives[i] == info->item) {
            g_rewardGives[i] = g_rewardGives[--g_rewardGiveCount];
            break;
        }
    }
    if (g_rewardQueued == 0 || info->item != g_rewardQueued || info->origin != ITEM_GIVE_ORIGIN_QUEUE) {
        return;
    }
    g_blob.flags |= FLAG_CLEARED_ONCE;
    mods::log::info("first-clear wallet 0x{:02X} granted", g_rewardQueued);
    g_rewardQueued = 0;
}

// Without the reward talk (no HD text / StageService), or when its tag never fired, the reward is
// handed out directly with notices (previous behaviour).
void reward_without_talk() {
    if (g_rewardedThisVisit) {
        return;
    }
    const int kind = reward_kind();
    grant_reward();
    if (kind == 1) {
        toast(hd_plain(hdtext::kZel00, 413, "Cave of Shadows").c_str(),
            hd_plain(hdtext::kZel05, 7546, "Well fought. Here is your reward.").c_str(), false, 6000);
    }
}

void track_floor(bool paused) {
    if (link_player() == nullptr || scene_busy()) {
        return;
    }
    const int room = dComIfGp_roomControl_getStayNo();
    if (room == kFinalRoom && !g_rewardedThisVisit && !paused) {
        // The reward talk tag (hdtext.cpp) normally fires right after the entrance; fall back to the
        // direct reward if it has not after a while (tag missing, player standing still, ...).
        // FIX (review "cave entry" #3): only game time counts (mod_update also runs while the pause menu
        // is open), so a long look at the pause menu no longer replaces the talk with the notice.
        const bool talk = hdtext::talk_tag(hdtext::kFlowReward);
        if (!talk || ++g_rewardRoomTicks > kRewardTalkWaitTicks) {
            if (!dComIfGp_event_runCheck()) {
                reward_without_talk();
            }
        }
    }
    if (!g_roomSynced) {
        // Right after the stage change the room control can still hold the previous stage's room
        // number; ignore everything until the visit's arrival room shows up (every visit starts there).
        if (g_arrivalRoom >= 0 && room != g_arrivalRoom) {
            return;
        }
        g_roomSynced = true;
    }
    if (g_restartRun) {
        // Death-continue: floor 1 has loaded again; HD's new cave controller means a new run.
        g_restartRun = false;
        start_run();
    }
    if (room == g_lastRoom || room < 0 || room >= kRoomCount) {
        return;
    }
    g_lastRoom = room;
    const int floor = floor_of_room(room);
    if (floor <= 0) {
        return;
    }

    if (floor <= kLastFloor && floor > g_blob.best_floor) {
        g_blob.best_floor = static_cast<std::uint16_t>(floor);
    }
    if (g_run.active && floor > g_run.floorTo && floor <= kLastFloor) {
        g_run.floorTo = static_cast<std::uint8_t>(floor);
    }

    if (floor < 64 && (g_toastedFloors & (1ull << floor)) == 0 && cfg_bool(g_cvarFloorToasts, false)) {
        g_toastedFloors |= 1ull << floor;
        char body[96];
        std::snprintf(body, sizeof(body), "Floor %d of %d", floor, kLastFloor);
        toast(hd_plain(hdtext::kZel00, 413, "Cave of Shadows").c_str(), body, false, 2500);
    }
    // (Reaching a rest floor no longer counts as clearing the section: HD sets 0x7001/0x7002 only when
    // its portal is used, see set_section_cleared. HD says nothing on floors 6/20.)
}

// The "Save and begin" save image as the game's own save struct (card_to_memory copies exactly this much).
dSv_save_c* trip_save() {
    static_assert(sizeof(dSv_save_c) <= QUEST_LOG_SIZE);
    return reinterpret_cast<dSv_save_c*>(g_tripCard);
}

// The image's return place (savedata +0x58), or false when it has none the game could start at.
bool trip_return_place(char (&stage)[9], int& point, int& room) {
    dSv_player_return_place_c& place = trip_save()->getPlayer().getPlayerReturnPlace();
    std::memset(stage, 0, sizeof(stage));
    std::memcpy(stage, place.getName(), 8);
    point = place.getPlayerStatus();
    room = place.getRoomNo();
    return stage[0] != '\0' && std::strcmp(stage, kStageName) != 0;
}

// When the "Save and begin" save is put back (the end state of HD's Withdraw, a reload of that save, without the
// reset):
//  - StageDeleted: the dStage_Delete post-hook (the normal case). The cave's stage is deleted and the next stage has
//    not loaded its memory, so the whole dSv_info_c is reset as the file select does.
//  - SceneGoing: that hook is missing (restore once the scene change's fade is at black), or the mod is shutting down
//    mid-fade. Only the saved data is replaced, and the cave's own stage table is taken from the image too, so the
//    cave's putSave at its stage delete writes the image's table back.
//  - NextLoaded: the scene change was not seen at black (no hook, a missed frame): the next stage has loaded already.
//    Only the saved data is replaced.
enum class RestoreAt : std::uint8_t { StageDeleted, SceneGoing, NextLoaded };

void restore_trip_save(RestoreAt when) {
    const bool full = when == RestoreAt::StageDeleted;
    if (full) {
        dComIfGs_init();  // dSv_info_c::init: savedata, stage memory, dungeon bits, zones, temp flags (d_save.cpp:1501)
    }
    // The file select's load (d_save.cpp:1868-1912): the whole dSv_save_c, life raised to 3 hearts, keys, hookshots,
    // item line-up, vibration, the saved stage name.
    g_dComIfG_gameInfo.info.card_to_memory(reinterpret_cast<char*>(g_tripCard), 0);
    if (when == RestoreAt::SceneGoing) {
        // (null once dStage_Delete has run: dComIfGp_getStage()->init(), d_stage.cpp:2932)
        if (stage_stag_info_class* stag = dComIfGp_getStageStagInfo(); stag != nullptr) {
            dComIfGs_getSave(dStage_stagInfo_GetSaveTbl(stag));
        }
        g_dComIfG_gameInfo.info.getTmp().init();
    }
    // The name scene's start of play (d_s_name.cpp:373-374): play time counts from the save's total again.
    dComIfGs_setSaveTotalTime(dComIfGs_getTotalTime());
    dComIfGs_setSaveStartTime(OSGetTime());
    // FIX (review HD #7 / API #5): what the name scene also does before the play scene (d_s_name.cpp:419-421), and the
    // last-scene info dComIfGs_gameStart leaves when it runs from the title (no player: speed, mode and angle 0,
    // d_com_inf_game.cpp:1462-1490). Without this the cave Link's last-scene bits (wolf 0x800000) reached the first
    // spawn after landing.
    if (full) {
        dKy_clear_game_init();  // weather and a few environment fields; the cave scene is gone
    }
    dComIfGs_setRestartRoomParam(0);
    g_dComIfG_gameInfo.info.getRestart().setLastSceneInfo(0.0f, 0, 0);
    // Pending HUD counts of the cave (hearts, rupees, ammunition still being added) must not land on the save.
    dComIfGp_clearItemLifeCount();
    dComIfGp_clearItemRupeeCount();
    dComIfGp_clearItemMaxLifeCount();
    dComIfGp_clearItemKeyNumCount();
    dComIfGp_clearItemOilCount();
    dComIfGp_clearItemMaxOilCount();
    dComIfGp_clearItemArrowNumCount();
    dComIfGp_clearItemPachinkoNumCount();
    for (u8 i = 0; i < 3; ++i) {
        dComIfGp_clearItemBombNumCount(i);
    }
    // The cave blob stored with that save. The virtual Wolf Link amiibo is a figure, not part of the save: it keeps
    // what it holds (only the Record screen writes it, and no visit that ends with Withdraw reaches that screen).
    Blob restored = g_tripBlob;
    restored.stored_hearts = g_blob.stored_hearts;
    restored.amiibo_level = g_blob.amiibo_level;
    restored.amiibo_life = g_blob.amiibo_life;
    restored.amiibo_max = g_blob.amiibo_max;
    restored.hd_labels &= static_cast<std::uint8_t>(~(1u << (kLabelIntro - kLabelReward)));
    g_blob = restored;
    g_tripValid = false;
    g_snap = Snapshot{};  // on_leave must not write the cave's entry life / rupees over the restored save
    g_pendingFormRestore = false;  // the restored save has its own form
    // FIX (review HD #4 / API #4): a reward of this visit is not the restored save's (HD's reload drops it). The
    // Withdraw waited for queued reward gives (tick, the pending Withdraw), so normally nothing is left here.
    if (g_rewardGiveCount > 0) {
        mods::log::warn("Withdraw (HD): {} reward item(s) of this visit are still queued and may still be handed out",
            g_rewardGiveCount);
    }
    g_rewardQueued = 0;
    g_rewardGiveCount = 0;
    mods::log::info("Withdraw (HD): the save from \"Save and begin\" is back ({}): life {}/4, rupees {}, {}; nothing "
                    "from the visit is kept",
        when == RestoreAt::StageDeleted ? "at the stage change" :
        when == RestoreAt::SceneGoing   ? "while the cave scene is deleted" :
                                          "after the next stage loaded",
        dComIfGs_getLife(), dComIfGs_getRupee(), dComIfGs_getTransformStatus() != 0 ? "wolf" : "human");
}

// The pending restore still belongs to the stage change finish_hd_withdraw issued: out of D_SB11 into its target, no
// reset to the title.
bool restore_target_pending() {
    const char* next = dComIfGp_isEnableNextStage() ? dComIfGp_getNextStageName() : nullptr;
    return !mDoRst::isReset() && next != nullptr && std::strcmp(next, g_restoreTarget) == 0 && in_cave_stage();
}

// HD's Withdraw after the warp-out (or right after event044's sounds): dComIfGs_gameStart with the image's return
// place (HD 0x029076e4, what the title screen's load and the Record screen's exit do), the save put back at the
// stage change (on_stage_deleted; without its hook once the fade is at black, tick).
void finish_hd_withdraw() {
    g_restoreWithdraw = false;
    char stage[9];
    int point = 0;
    int room = 0;
    if (!g_tripValid || !trip_return_place(stage, point, room)) {
        mods::log::warn("Withdraw (HD): the \"Save and begin\" save has no return place; the mod's own exit "
                        "instead (the visit is kept)");
        finish_withdraw_stage(0);
        return;
    }
    // Nothing of the run is recorded: HD reloads the save, which never saw it.
    if (g_run.active) {
        end_run(RunEnd::None);
    }
    dComIfGp_offEnableNextStage();
    dComIfGp_setNextStage(stage, static_cast<s16>(point), static_cast<s8>(room), -1, 0.0f, 0, 1, 0, 0, 0, 0);
    std::memcpy(g_restoreTarget, stage, sizeof(g_restoreTarget));
    g_snap = Snapshot{};
    g_pendingFormRestore = false;
    g_pendingWithdraw = false;
    g_phase = Phase::Exiting;
    g_phaseTimer = 0;
    // FIX (review HD #5): without the dStage_Delete hook the save used to be put back right here, while the cave scene
    // still ran for the whole fade (its actors' switch bits then went into the image's D_SB11 table, and a stage
    // change that never happened left the restored save in a live cave). Now tick() does it once the fade is at black.
    g_restorePending = true;
    mods::log::info("Withdraw (HD): exit -> {} point {} room {} (the return place of the \"Save and begin\" save){}",
        stage, point, room, g_restoreHookFailed ? "; the save is put back at black (stage-delete hook missing)" : "");
}

void do_withdraw() {
    if (g_restoreWithdraw) {
        // HD's Withdraw (design decision): Midna's warp-out, then the "Save and begin" save's return place.
        char stage[9];
        int point = 0;
        int room = 0;
        const bool place = g_tripValid && trip_return_place(stage, point, room);
        if (place && warp_effect_enabled() && portal_withdraw_allowed()) {
            mods::log::info("Withdraw (HD): Midna's warp-out, then the \"Save and begin\" save's return place");
            begin_warp(WarpKind::Withdraw, stage, cXyz(0.0f, 0.0f, 0.0f), 0, room);
            return;
        }
        if (place) {
            mods::log::info("Withdraw (HD): Midna's warp-out cannot play ({}); HD's warp sounds and a fade instead",
                !warp_effect_enabled() ? (g_portalHookFailed ? "its hook is missing" : "\"Midna's portal warp\" is off")
                                       : withdraw_warp_refusal());
            // HD event044's first call (0x02a65e60-0x02a65eec, VERIFIED): these two sounds.
            play_se(Z2SE_MDN_WARP_IN_TATE);  // 0x600A0
            play_se(Z2SE_WOLF_WARP_OUT);     // 0x2009B
        }
        finish_hd_withdraw();  // (without a return place it logs and takes the stored return point)
        return;
    }
    mods::log::info("withdraw: returning to the stored point");
    if (warp_effect_enabled() && (g_blob.flags & FLAG_HAS_RETURN) != 0 && g_blob.ret_stage[0] != '\0' &&
        portal_withdraw_allowed())
    {
        char stage[8];
        std::memcpy(stage, g_blob.ret_stage, sizeof(stage));
        stage[7] = '\0';
        const cXyz pos(g_blob.ret_pos[0], g_blob.ret_pos[1], g_blob.ret_pos[2]);
        begin_warp(WarpKind::Withdraw, stage, pos, g_blob.ret_angle, g_blob.ret_room);
        return;
    }
    // tphd event044: the wolf's warp-out sound, then back to the return point (fade).
    if (daAlink_c* link = alink()) {
        link->seStartOnlyReverb(Z2SE_WOLF_WARP_OUT);
    }
    finish_withdraw_stage(0);
}

// ---------------------------------------------------------------------------------------------
// Save observers.
// ---------------------------------------------------------------------------------------------

void reset_session_state() {
    g_phase = Phase::Idle;
    g_phaseTimer = 0;
    g_pendingEnter = false;
    g_pendingWithdraw = false;
    g_entry = EntryStep::None;
    g_tripSaved = false;
    g_rewardQueued = 0;  // the host clears its give queue on every slot change (item_gives_clear)
    g_rewardGiveCount = 0;
    g_rewardWaitTicks = 0;
    g_undoLifeValid = false;
    g_warp = Warp{};
    g_run = Run{};
    g_arrivalRoom = -1;
    g_tier = 0;
    g_restartRun = false;
    g_swallowMask = 0;
    g_snap = Snapshot{};
    g_portalDash = false;
    g_portalExit = false;
    g_writeScan = false;
    g_restoreWithdraw = false;
    g_restorePending = false;
    g_tripValid = false;
    g_visitSaved = false;
    amiibo::cancel();
}

void on_new_save(ModContext*, std::uint32_t, void*) {
    refresh_mode();
    g_slotActive = true;
    g_blob = default_blob();
    g_blobExists = false;  // the host already cleared the slot's blobs
    g_pendingFormRestore = false;
    reset_session_state();
}

void on_save_loaded(ModContext*, std::uint32_t slot, void*) {
    refresh_mode();
    g_slotActive = true;
    g_blobExists = load_blob();
    reset_session_state();
    // A save written while a cave session was active (autosave on scene entry) still carries
    // the wolf form from the cave. Restore the entry form on the next player spawn outside it.
    g_pendingFormRestore = (g_blob.flags & FLAG_HAS_RETURN) != 0;
    g_formToRestore = g_blob.ret_form;
    g_blob.hd_labels &= static_cast<std::uint8_t>(~(1u << (kLabelIntro - kLabelReward)));
    mods::log::info("slot {} loaded: best floor {}, clears {}, flags 0x{:X}, stored hearts {}", slot,
        g_blob.best_floor, g_blob.clear_count, g_blob.flags, g_blob.stored_hearts);
}

// The pause menu's save screen is writing `slot` right now (dMenu_save_c::memCardDataSaveWait reports the
// slot from inside that step, before it moves on to memCardDataSaveWait2).
bool menu_save_writing(std::uint32_t slot) {
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    const dMenu_save_c* menu = window != nullptr ? window->mpMenuSave : nullptr;
    return dComIfGp_isPauseFlag() != 0 && menu != nullptr &&
           menu->mMenuProc == dMenu_save_c::PROC_MEMCARD_DATA_SAVE_WAIT && menu->mSelectedFile == slot;
}

void on_save_written(ModContext*, std::uint32_t slot, void*) {
    // FIX (review L1): only the pause-menu save screen's own write counts as the trip's save (an autosave,
    // which reports through the same callback, must never start a trip the player then cancelled).
    if (g_entry == EntryStep::Saving && menu_save_writing(slot)) {
        // The amiibo trip's save (dMenu_save_c::memCardDataSaveWait, SaveSync done): the slot is written
        // by the game's own save path, so the trip may start.
        g_tripSaved = true;
        // HD's Withdraw reloads this save (see g_restoreWithdraw): keep the image dataWrite wrote for the slot
        // (memory_to_card into mSaveBuffer, d_menu_save.cpp:1432-1445) and the blob stored with it below.
        const dMw_c* window = dMeter2Info_getMenuWindowClass();
        const dMenu_save_c* menu = window != nullptr ? window->mpMenuSave : nullptr;
        if (menu != nullptr && slot < 3) {
            std::memcpy(g_tripCard, menu->mSaveBuffer + static_cast<std::size_t>(slot) * QUEST_LOG_SIZE,
                sizeof(g_tripCard));
            g_tripBlob = g_blob;
            g_tripValid = true;
        }
        char stage[9];
        int point = 0;
        int room = 0;
        const bool place = g_tripValid && trip_return_place(stage, point, room);
        mods::log::info("cave trip: slot {} written (return place {} point {} room {})", slot,
            place ? stage : "none", point, room);
    } else if (g_entry == EntryStep::Saving) {
        mods::log::info("slot {} written outside the trip's save screen; it does not start the trip", slot);
    } else if (cave_rules() && g_visitSaved && g_phase == Phase::InCave && !record::busy() && menu_save_writing(slot)) {
        // FIX (review HD #3): HD's Withdraw is a reset, after which the player loads the newest save, i.e. this one
        // (a pause-menu save made in the cave: life, rupees and form of the entry, before_memory_to_card). Keep its
        // image instead of the "Save and begin" one, so Withdraw lands as that reload would and the autosave after
        // landing does not overwrite this newer save with the older one. A save whose return place the game could not
        // start at (none, or D_SB11) keeps the older image.
        const dMw_c* window = dMeter2Info_getMenuWindowClass();
        const dMenu_save_c* menu = window != nullptr ? window->mpMenuSave : nullptr;
        if (menu != nullptr && slot < 3) {
            const unsigned char* image = menu->mSaveBuffer + static_cast<std::size_t>(slot) * QUEST_LOG_SIZE;
            // (read only; the accessors are not const)
            dSv_player_return_place_c& place =
                reinterpret_cast<dSv_save_c*>(const_cast<unsigned char*>(image))->getPlayer().getPlayerReturnPlace();
            char stage[9] = {};
            std::memcpy(stage, place.getName(), 8);
            if (stage[0] != '\0' && std::strcmp(stage, kStageName) != 0) {
                std::memcpy(g_tripCard, image, sizeof(g_tripCard));
                g_tripBlob = g_blob;  // the blob stored with it below
                g_tripValid = true;
                mods::log::info("cave: slot {} written in the cave; Withdraw now goes back to this save ({} point {} "
                                "room {})",
                    slot, stage, static_cast<int>(place.getPlayerStatus()), static_cast<int>(place.getRoomNo()));
            } else {
                mods::log::info("cave: slot {} written in the cave without a return place; Withdraw keeps the "
                                "\"Save and begin\" save",
                    slot);
            }
        }
    }
    if (record::busy() && menu_save_writing(slot)) {
        record::on_slot_written();  // the "Cave of Shadows Record" screen's save
        mods::log::info("Cave of Shadows Record: slot {} written", slot);
    }
    refresh_mode();
    if (!g_initialized || !g_vanilla) {
        return;  // never write cave data into another game mode's saves
    }
    g_slotActive = true;
    if (!g_blobExists && is_default(g_blob)) {
        // This game never used the cave: store nothing, but drop a cave blob an overwritten file may have
        // left in this slot (saving into another file must not inherit that file's cave progress).
        svc_save->delete_blob(mod_ctx, kBlobName);
        return;
    }
    const ModResult result = svc_save->set_blob(mod_ctx, kBlobName, &g_blob, sizeof(g_blob));
    if (result != MOD_OK) {
        mods::log::warn("could not store the cave blob in slot {} (result {})", slot, static_cast<int>(result));
        return;
    }
    g_blobExists = true;
}

}  // namespace

// =============================================================================================
// Public API.
// =============================================================================================

bool initialized() {
    return g_initialized;
}

bool vanilla_mode() {
    return g_vanilla;
}

bool enabled() {
    return g_initialized && g_switchOn && g_vanilla;
}

bool in_cave_stage() {
    const char* name = dComIfGp_getStartStageName();
    return name != nullptr && std::strcmp(name, kStageName) == 0;
}

bool cave_rules() {
    return enabled() && in_cave_stage();
}

Phase phase() {
    return g_phase;
}

bool amiibo_unlocked() {
    return enabled() && g_slotActive && dComIfGs_isEventBit(kFlagFaronSpiritRevived);
}

bool midna_gravely_ill() {
    return dComIfGs_isEventBit(kFlagLakebedCleared) && !dComIfGs_isEventBit(kFlagMidnaRevived);
}

// ---------------------------------------------------------------------------------------------
// Entry check. HD's own check is Zelda.rpx 0x029b0448-0x029b0fc4 (VERIFIED, research/cos_entryrules/decode_hd.md):
// a deny-list of 12 tests, called from one place only (0x029bc0dc), when Wolf Link is scanned on the Collection
// screen outside the cave. Every refusal shows message 1530 and nothing else. It has no field-only, twilight,
// Midna-ill, swimming, climbing, riding or carrying rule: the amiibo works in dungeons (Hyrule Castle D_MN09 too),
// houses, caves and grottos, and in the twilight. HD checks nothing again after "Save and begin".
// ---------------------------------------------------------------------------------------------

namespace {

// HD 0x02906858 = dComIfGs_isOneZoneSwitch (it tail-calls 0x02aa8100, which tests dSv_zoneBit_c::mRoomSwitch,
// VERIFIED). A room without a zone (-1, e.g. F_SP115 room 3, a room that stage does not have) makes HD read
// mZone[-1].mBit.mRoomSwitch, which is mDan.unk28[3] (mDan lies right before mZone in dSv_info_c; HD mDan =
// gameInfo+0xE18). dSv_danBit_c::init fills unk28 with 0xFFFF and nothing else writes it (d_save.cpp:1307-1309).
// This gives HD's answer without indexing mZone[-1].
bool hd_one_zone_switch(int sw, int room) {
    const int zone = dComIfGp_roomControl_getZoneNo(room);
    if (zone >= 0) {
        return dComIfGs_isSvOneZoneSwitch(zone, sw) != FALSE;
    }
    return ((static_cast<u16>(g_dComIfG_gameInfo.info.getDan().unk28[3]) >> sw) & 1) != 0;
}

// HD rule 7 (0x029b0b98-0x029b0c80): the first Malo found by name (fopAcM_SearchByName(fpcNm_NPC_MARO_e); HD proc
// 0x261 = GC 0x260) is type 8 (daNpc_Maro_c::getType() = param & 0xFF, d_a_npc_maro.cpp:491; TYPE_8 = the arrow
// tutorial, :1163) and temp bit T_0060 "Kakariko Village - during bow and arrow game" is set (daNpcT_chkTmpBit(0x3C)).
bool hd_bow_game() {
    fopAc_ac_c* malo = fopAcM_SearchByName(fpcNm_NPC_MARO_e);
    return malo != nullptr && (fopAcM_GetParam(malo) & 0xFF) == 8 &&
           dComIfGs_isTmpBit(dSv_event_tmp_flag_c::T_0060) != FALSE;
}

// TP HD's entry check, test by test, in HD's order (Zelda.rpx 0x029b0448, all 12 tests VERIFIED; the meanings are
// INFERRED unless the comment says otherwise). Returns the first HD rule that refuses (1-12), 0 when HD allows the
// entry. The tests have no side effects, so the order only decides which rule is logged.
int hd_entry_rule() {
    const char* stage = dComIfGp_getStartStageName();  // HD gameInfo+0x52A0
    if (stage == nullptr) {
        return 0;
    }
    const int room = dComIfGp_getStartStageRoomNo();   // HD (s8) gameInfo+0x52AA: the start room, not the stay room
    daPy_py_c* player = link_player();                 // HD gameInfo+0x6270
    daAlink_c* link = alink();
    const auto is = [stage](const char* name) { return std::strcmp(stage, name) == 0; };

    // 1 (0x0474-0x0774): Ganondorf's stages and F_SP200.
    if (is("D_MN09A") || is("D_MN09B") || is("D_MN09C") || is("F_SP200")) {
        return 1;
    }
    // 2 (0x0778-0x079c): any D_MN* stage entered in room 50-52 (boss and mini-boss rooms).
    if (std::strncmp(stage, "D_MN", 4) == 0 && room >= 50 && room <= 52) {
        return 2;
    }
    // 3 (0x07a0-0x090c): Zant's throne room (D_MN08A room 10) and the Zant fight (D_MN08D).
    if ((is("D_MN08A") && room == 10) || is("D_MN08D")) {
        return 3;
    }
    if (player != nullptr) {
        // 4 (0x0914-0x09e4): Zora's River (F_SP112, GC d_kankyo_data.cpp:126) entered in room 1, Iza's room.
        if (room == 1 && is("F_SP112")) {
            return 4;
        }
        // 5 (0x09e8-0x0b7c): in a canoe (vt+0x324 = checkCanoeRide) at the fishing pond or Lake Hylia.
        if (player->checkCanoeRide() && (is("F_SP127") || is("F_SP115"))) {
            return 5;
        }
    }
    // 6 (0x0b80-0x0b94): taming a horse (rodeo mode, horse+0x1748 bit 31). Normal riding is allowed.
    if (daHorse_c* horse = dComIfGp_getHorseActor(); horse != nullptr && horse->checkRodeoMode()) {
        return 6;
    }
    // 7 (0x0b98-0x0c80): the Kakariko bow and arrow game (meaning VERIFIED from the GC temp-bit label).
    if (is("F_SP109") && hd_bow_game()) {
        return 7;
    }
    // HD reads the player without a NULL test from here on (it always has one when the scan runs); the mod tests it.
    if (link != nullptr) {
        const bool layer0 = dComIfG_play_c::getLayerNo(0) == 0;  // HD 0x02908afc; the twilight is layer 14
        // 8 (0x0c84-0x0d70): gliding with a Cucco over Lake Hylia (mProcID == PROC_AUTO_JUMP, mProcVar2 != 0).
        if (layer0 && room == 0 && is("F_SP115") && hd_one_zone_switch(0, 3) && link->checkCokkoGlide()) {
            return 8;
        }
        // 9 (0x0d74-0x0d84): sumo (daPy_py_c::getSumouMode, mMode 1..0x25).
        if (link->getSumouMode()) {
            return 9;
        }
        // 10 (0x0d88-0x0e84): the Snowpeak ice-leaf slide (on the board, or about to get back on it).
        if (layer0 && room == 1 && is("F_SP114") && (link->checkBoardRide() || link->checkBoardRestart())) {
            return 10;
        }
        // 11 (0x0e88-0x0f64): Sacred Grove room 1 while its one-zone switch 1 is on (the Skull Kid chase).
        if (layer0 && room == 1 && is("F_SP117") && hd_one_zone_switch(1, 1)) {
            return 11;
        }
    }
    // 12 (0x0f68-0x0f7c): a timer of mode 2-6 runs (dComIfG_getTimerMode; HD compares it unsigned).
    if (static_cast<unsigned>(dComIfG_getTimerMode()) - 2u <= 4u) {
        return 12;
    }
    return 0;
}

// The HD rule behind the last can_enter refusal (0 = none). Logged by the scan only: the mod's panel calls can_enter
// every frame (fix L4).
int g_lastHdRule = 0;

// For the log and the mod's panel only: in the game HD shows message 1530 for every rule.
const char* hd_rule_text(int rule) {
    switch (rule) {
    case 2:
        return "Not available in a boss room.";
    case 5:
        return "Not available in a canoe here.";
    case 6:
        return "Not available while taming a horse.";
    case 7:
        return "Not available during the bow and arrow game.";
    case 8:
        return "Not available while gliding with a Cucco.";
    case 9:
        return "Not available during sumo.";
    case 10:
        return "Not available while snowboarding.";
    case 12:
        return "Not available while a timer is running.";
    default:
        return "Not available here right now.";
    }
}

// The mod's own conditions, not HD rules: the cave must be able to run at all (switch, game mode, a loaded slot,
// its hooks and stage files), and no trip may already be under way. HD has the same "in the cave" branch: with its
// cave controller up the scan heals instead (0x029bbea8); the mod picks ScanMode::Heal before this (amiibo_button).
const char* mod_refusal() {
    if (!g_switchOn || g_stopRequested) {
        return "The Cave of Shadows is turned off in the HD Features options.";
    }
    if (!g_initialized) {
        return "The mod is still starting.";
    }
    if (!g_vanilla) {
        return "Not available in this game mode (randomizer, speedrun or another mod's mode).";
    }
    if (!g_slotActive) {
        return "Load a save file first.";
    }
    if (g_criticalHookFailed) {
        return "A game hook the cave needs could not be installed (see the mod panel).";
    }
    if (g_phase != Phase::Idle || in_cave_stage() || g_warp.kind != WarpKind::None) {
        return "You are already in the Cave of Shadows.";
    }
    if (!assets_present()) {
        return "HD Features' game files are missing.";
    }
    if (link_player() == nullptr) {
        // (HD's menu needs the player too; the mod's stage change and return point need a play scene.)
        return "Only available during gameplay.";
    }
    if (g_pendingEnter || (g_entry != EntryStep::None && g_entry != EntryStep::Scanning)) {
        return "Entering...";
    }
    return nullptr;
}

// Gameplay states in which the mod's own stage change (or Midna's warp-out) cannot start safely: a second
// setNextStage while one is pending, or an event ordered while another runs, would be lost or race (the trip would
// then time out in Phase::Entering). Not HD rules: HD's pause menu cannot open in these states (dMw_c key_wait_proc,
// isPauseReady: life 0, isEventCheck, fopOvlpM_IsDoingReq, isEnableNextStage; d_menu_window.cpp:586-621), so from
// the Collection screen they never refuse what HD allows. The one GC event that opens the menu by force (Iza's
// bomb-bag menu, d_a_npc_hoz.cpp:629, onMenuInForce(2)) is at F_SP112 room 1 (:1055), which HD rule 4 refuses anyway.
const char* busy_refusal(bool fromPauseMenu) {
    if (dComIfGp_event_runCheck() || scene_busy() ||
        (!fromPauseMenu && (dComIfGp_isPauseFlag() != 0 || message_box_up())))
    {
        // (From the Collection screen the pause and message tests are skipped: the menu itself is up, and it shows
        // no message box.)
        return "Not available during a cutscene, a message or a scene change.";
    }
    if (dComIfGs_getLife() == 0 || dComIfGp_getGameoverStatus() != 0) {
        return "Not available right now.";
    }
    return nullptr;
}

}  // namespace

bool can_enter(const char** reason, bool fromPauseMenu) {
    const char* why = mod_refusal();
    if (why == nullptr && !dComIfGs_isEventBit(kFlagFaronSpiritRevived)) {
        // HD: before Faron's light spirit (M_019, HD label 823) the amiibo reader is off (0x029bc5ac); no message.
        // The Collection-screen scan never gets here then (amiibo_button); this is for the mod's panel.
        why = "Restore the light spirit of Faron first.";
    }
    if (why == nullptr) {
        why = busy_refusal(fromPauseMenu);
    }
    g_lastHdRule = 0;
    if (why == nullptr) {
        if (const int rule = hd_entry_rule(); rule != 0) {
            why = hd_rule_text(rule);
            g_lastHdRule = rule;
        }
    }
    if (reason != nullptr) {
        *reason = why;
    }
    return why == nullptr;
}

bool can_withdraw(const char** reason) {
    const char* why = nullptr;
    if (!enabled()) {
        why = !g_vanilla ? "Not available in this game mode." : "Not ready.";
    } else if (!in_cave_stage()) {
        why = "You are not in the Cave of Shadows.";
    } else if (g_phase == Phase::Exiting || g_pendingWithdraw || g_warp.kind != WarpKind::None || g_portalExit ||
               g_restorePending)
    {
        why = "Already leaving.";
    } else if (link_player() == nullptr || scene_busy()) {
        why = "Please wait for the scene to finish loading.";
    } else if (dComIfGs_getLife() == 0 || dComIfGp_getGameoverStatus() != 0) {
        why = "Not available right now.";
    }
    if (reason != nullptr) {
        *reason = why;
    }
    return why == nullptr;
}

bool request_enter() {
    const char* reason = nullptr;
    if (!can_enter(&reason)) {
        toast("Cave of Shadows", reason != nullptr ? reason : "Not available right now.", true);
        return false;
    }
    // Executed from mod_update (fapGm_Execute -> duskExecute, before process management), the same
    // safe point the host uses for its own per-frame game cheats (f_ap_game.cpp:780-807).
    g_pendingEnter = true;
    return true;
}

bool request_withdraw() {
    const char* reason = nullptr;
    if (!can_withdraw(&reason)) {
        toast("Cave of Shadows", reason != nullptr ? reason : "Not available right now.", true);
        return false;
    }
    g_stopWithdrawQueued = false;  // the player's own Withdraw: turning the switch back on keeps it
    g_restoreWithdraw = false;     // "Leave the cave": the stored return point, keeping the visit
    g_pendingWithdraw = true;
    g_pendingWithdrawTimer = kWithdrawWaitTicks;
    g_pendingWithdrawCutscene = false;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Virtual Wolf Link amiibo (design notes "Entry": entry exactly like HD).
// ---------------------------------------------------------------------------------------------

int amiibo_button_choice() {
    const std::int64_t value = cfg_int(g_cvarAmiiboButton, 0);
    return value >= 0 && value <= 2 ? static_cast<int>(value) : 0;
}

const char* amiibo_button_label() {
    static const char* const kLabels[] = {"Z", "X", "Y"};
    return kLabels[amiibo_button_choice()];
}

void collect_screen_opened() {
    // The prompt itself is pushed by tick() once the screen is idle (update_prompt), so it never shows
    // over a sub-window, the scan screen or, after the menu has closed, gameplay.
    g_collectOpen = true;
}

void collect_screen_idle() {
    g_collectOpen = true;  // also covers a missing _create hook (wait_proc only runs while the screen exists)
    g_collectIdleFrame = g_frame;
}

void collect_screen_closed() {
    g_collectOpen = false;
    amiibo::set_prompt_visible(false);  // fade out a prompt that is still showing
    if (amiibo::scan_open()) {
        amiibo::cancel();  // the screen went away under the scan screen (should not happen: input is blocked)
        if (g_entry == EntryStep::Scanning) {
            g_entry = EntryStep::None;
        }
    }
}

void amiibo_button(dMenu_Collect2D_c*) {
    if (!amiibo_unlocked() || amiibo::scan_open() || g_entry != EntryStep::None ||
        g_warp.kind != WarpKind::None || g_phase == Phase::Entering || g_phase == Phase::Exiting)
    {
        return;  // locked before Faron (like HD) or a scan / trip is already under way
    }
    const ScanMode mode = in_cave_stage() ? ScanMode::Heal : ScanMode::Enter;
    // FIX (review L2): HD's pre-entry screen archive (about 11 MB) is no longer read here, on the Z press:
    // confirm.cpp reads it once, at black, when the Collection screen has faded out for the screen.
    if (amiibo::open_scan(mode)) {
        if (mode == ScanMode::Enter) {
            g_entry = EntryStep::Scanning;
        }
        return;
    }
    // No scan screen (RmlUi unavailable): decide at once.
    const ScanResult result = amiibo_scan_result(mode);
    if (result != ScanResult::EnterOk && result != ScanResult::HealOk) {
        play_se(Z2SE_SYS_ERROR);
        toast("Wolf Link amiibo", hd_plain(hdtext::kZel00, result == ScanResult::HealUsed ? 1555 :
                                                           result == ScanResult::HealEmpty ? 1572 : 1530,
                                      "The Wolf Link amiibo has no effect here.").c_str(),
            true, 5000);
    } else {
        play_se(result == ScanResult::EnterOk ? Z2SE_WARP_MAP_DECIDE : Z2SE_SY_LIGHT_DROP_GET);
        if (mode == ScanMode::Enter) {
            g_entry = EntryStep::Scanning;
        }
    }
    amiibo_scan_closed(result, false);
}

ScanResult amiibo_scan_result(ScanMode mode) {
    if (mode == ScanMode::Write) {
        // HD proc 0x47 (0x02a0d53c) + callback 0x02a0d144: the figure gets the record's floor, life and max life;
        // the long note 1568 the first time (save byte 0xABB), 1569 after that.
        const RecordValues& v = g_recordValues;
        g_blob.amiibo_level = static_cast<std::uint8_t>(std::clamp(v.floor, 0, 0xFF));
        g_blob.amiibo_life = static_cast<std::uint8_t>(std::clamp(v.life, 0, 0xFF));
        g_blob.amiibo_max = static_cast<std::uint8_t>(std::clamp(v.maxLife, 0, 0xFF));
        g_blob.stored_hearts = static_cast<std::uint8_t>(std::min(v.life / 4, static_cast<int>(kMaxStoredHearts)));
        const bool first = (g_blob.notice_flags & NOTICE_1568) == 0;
        g_blob.notice_flags |= NOTICE_1568;
        mods::log::info("virtual Wolf Link amiibo written: level {}, life {}/4, max life {}", v.floor, v.life,
            v.maxLife);
        return first ? ScanResult::WriteOkFirst : ScanResult::WriteOk;
    }
    if (mode == ScanMode::Heal) {
        if (!cave_rules() || !g_run.active) {
            return ScanResult::EnterRefused;
        }
        // FIX (review L8): the same stored life the pre-entry screen's plate shows (quarter hearts). HD
        // (Zelda.rpx 0x029bbf60-0x029bc02c, VERIFIED): life byte <= 0 -> 1572; otherwise it heals
        // min(stored life, missing life) in quarter hearts.
        const std::uint32_t stored = stored_life_quarters();
        if (stored == 0) {
            return ScanResult::HealEmpty;
        }
        if (g_run.heals > 0) {
            return ScanResult::HealUsed;  // HD: once per visit (zel_00 1555)
        }
        // HD zel_00 1568: refill as many hearts as are stored (life is in quarter hearts).
        const std::uint16_t max = dComIfGs_getMaxLifeGauge();
        const std::uint32_t life = dComIfGs_getLife() + stored;
        dComIfGs_setLife(static_cast<u16>(std::min<std::uint32_t>(life, max)));
        g_run.heals = 1;
        // HD 0x029bc030: the cave controller's +0x11 = the figure's level (the record's Wolf Link row).
        g_run.healLevel = static_cast<std::uint8_t>(std::clamp(amiibo_data().level, 0, 0xFF));
        g_run.lastLife = dComIfGs_getLife();  // a heal is not "damage"
        mods::log::info("amiibo heal: +{} quarter hearts stored", stored);
        return ScanResult::HealOk;
    }
    const char* reason = nullptr;
    if (can_enter(&reason, true)) {
        return ScanResult::EnterOk;
    }
    if (const char* stage = dComIfGp_getStartStageName(); g_lastHdRule != 0 && stage != nullptr) {
        mods::log::info("amiibo scan refused by HD rule {} ({} room {}): {}", g_lastHdRule, stage,
            static_cast<int>(dComIfGp_getStartStageRoomNo()), reason != nullptr ? reason : "?");
    } else {
        mods::log::info("amiibo scan refused: {}", reason != nullptr ? reason : "?");
    }
    return g_criticalHookFailed || !assets_present() ? ScanResult::Unavailable : ScanResult::EnterRefused;
}

void amiibo_scan_closed(ScanResult result, bool byPlayer) {
    if (g_writeScan) {
        // The Record screen's write popup: OK after the write -> its save; closed before -> the 1262 question.
        g_writeScan = false;
        record::amiibo_closed(result == ScanResult::WriteOk || result == ScanResult::WriteOkFirst);
        if (byPlayer) {
            swallow_buttons(PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_START);
        }
        return;
    }
    if (byPlayer) {
        // The A/B press that closed the scan screen must not also reach the Collection screen when the
        // host unblocks game input (it would close the pause menu).
        swallow_buttons(PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_START | PAD_TRIGGER_Z | PAD_BUTTON_X |
                        PAD_BUTTON_Y);
    }
    if (g_entry != EntryStep::Scanning) {
        return;
    }
    if (result != ScanResult::EnterOk) {
        g_entry = EntryStep::None;
        return;
    }
    // HD (Zelda.rpx 0x029bc184 -> dMw state 0x2C): the scan closes, the Collection screen fades out and
    // the save menu opens as the "Cave of Shadows" screen: Save and begin / Cancel. The trip starts only
    // after the game's own save has written the slot (tick_entry, EntryStep::Saving).
    g_tripSaved = false;
    g_tripValid = false;  // the image of this trip's save is taken when it is written
    if (confirm::open(amiibo_data())) {
        g_entry = EntryStep::Confirm;
        mods::log::info("virtual Wolf Link amiibo scanned; showing the Cave of Shadows screen");
    } else if (confirm::open_save()) {
        // Without HD's screen (archive or hooks missing): straight to the stock save menu.
        g_entry = EntryStep::Saving;
        g_savingTicks = 0;
        mods::log::warn("virtual Wolf Link amiibo scanned; HD's screen is unavailable ({}), opening the save menu",
            confirm::problem());
    } else {
        g_entry = EntryStep::None;
        play_se(Z2SE_SYS_ERROR);
        mods::log::warn("virtual Wolf Link amiibo scanned, but the pause menu could not continue to the save");
    }
}

void confirm_finished(bool save) {
    if (g_entry != EntryStep::Confirm) {
        return;
    }
    if (save) {
        g_entry = EntryStep::Saving;
        g_savingTicks = 0;
        g_tripSaved = false;
        g_tripValid = false;
        mods::log::info("Save and begin: the pause-menu save opens");
    } else {
        g_entry = EntryStep::None;
        mods::log::info("Cave of Shadows screen cancelled");
    }
}

bool save_trip_active() {
    return g_entry == EntryStep::Saving;
}

bool save_trip_saved() {
    return g_entry == EntryStep::Saving && g_tripSaved;
}

bool enter_from_menu() {
    if (g_entry != EntryStep::Saving || !g_tripSaved) {
        return false;
    }
    // HD checks only at the scan (0x029b0448, VERIFIED: its only caller is 0x029bc0dc) and never after the save, so
    // only the mod's own conditions are checked here (the switch-off, a lost slot, a missing hook or stage file):
    // a written save never ends in a non-HD "saved, but refused". mod_refusal refuses while a trip step is pending,
    // so the step is cleared for the check.
    g_entry = EntryStep::None;
    if (const char* reason = mod_refusal(); reason != nullptr) {
        g_entry = EntryStep::Saving;  // the menu closes; tick_entry checks again and shows message 1530
        mods::log::info("amiibo trip after the save: not from the menu ({})", reason);
        return false;
    }
    if (entry_warp_option() && warp_effect_enabled()) {
        if (portal_entry_allowed()) {
            // NOT IN HD (option "cave-entry_warp"): the menu closes and Midna's warp-out plays (tick_entry WaitMenu).
            g_entry = EntryStep::Saving;
            mods::log::info("amiibo trip after the save: Midna's warp-out (option, not in HD) after the menu closes");
            return false;
        }
        mods::log::info("amiibo trip after the save: Midna's warp-out (option) is not possible here (GC map warps "
                        "are not allowed); HD's stage change from the menu instead");
    }
    // HD enterLos (0x02a02028-0x02a02150): the return point, then setNextStage(D_SB11 ...) right away.
    capture_return_point();
    g_visitSaved = true;  // "Save and begin" wrote the slot
    do_enter_stage();
    g_entry = EntryStep::Leaving;
    g_entryTimer = 0;
    return true;
}

void exit_to_return_point(
    std::uint32_t lastMode, int startEvent, float speed, std::int16_t angle, ExitCause cause) {
    // FIX (review "cave entry" #3): reaching floor 41 is the full clear (DESIGN.md "Reward"). If the player
    // left it before the reward talk happened (walked past the tag into the portal, or withdrew), hand the
    // reward out now; ItemService keeps the queued wallet until the player can receive it in the world.
    if (g_phase == Phase::InCave && in_cave_stage() && !g_rewardedThisVisit &&
        (cause == ExitCause::Portal || cause == ExitCause::Withdraw) &&
        dComIfGp_roomControl_getStayNo() == kFinalRoom)
    {
        mods::log::info("left floor {} before the reward talk; rewarding on the way out", kLastFloor);
        reward_without_talk();
    }

    // HD l8warp SCENE_CHG bookkeeping (0x026d333c-0x026d33e0, VERIFIED): the section the portal ends is
    // cleared (by the cave controller's tier: 0x7001/0x7002/0x7004) and the stage's chest bits are zeroed.
    // Only for the three portal rooms, so a stray leftover exit can never clear a section. The portal
    // cutscene (HD SCENE_CHG) may already have done both; they are idempotent.
    if (cause == ExitCause::Portal && in_cave_stage()) {
        const std::uint8_t tier = portal_tier();
        if (tier != 0) {
            set_section_cleared(tier);
            clear_stage_chests();
            // FIX (HD review L4): HD 0x026d33a4, the rupees collected stay. The portal cutscene's SCENE_CHG already
            // did this; on the fallback paths (its hooks missing, the Record screen unavailable) it happens here,
            // before on_leave writes the cave controller's values back.
            if (g_snap.valid) {
                g_snap.rupee = dComIfGs_getRupee();
            }
        }
    }

    // Run bookkeeping first (life and floor are still the cave's).
    if (g_run.active) {
        RunEnd end = RunEnd::Withdraw;
        if (cause == ExitCause::GameOver) {
            end = RunEnd::GameOver;
        } else if (cause == ExitCause::Portal) {
            const FloorKind kind = floor_kind(floor_of_room(dComIfGp_roomControl_getStayNo()));
            end = kind == FloorKind::Reward ? RunEnd::FullClear :
                  kind == FloorKind::Rest   ? RunEnd::SectionEnd :
                                              RunEnd::Withdraw;
        }
        end_run(end);
    }

    Blob& b = g_blob;
    const bool haveReturn =
        (b.flags & FLAG_HAS_RETURN) != 0 && b.ret_stage[0] != '\0' && b.ret_room >= 0;
    char retStage[8];
    std::memcpy(retStage, b.ret_stage, sizeof(retStage));
    retStage[7] = '\0';
    // Only a field entry goes back to the exact spot. After any other entry (dungeon, boss-type stage, house, cave)
    // the mod's exits go to the save's return place, as HD's reload does: no Ooccoo-like return into the middle of a
    // dungeon, and never into a boss-type stage with D_SB11's old multi kept (d_stage.cpp:2009-2012, 2914-2918;
    // research/cos_entryrules/gc_safety.md F1).
    const bool exactSpot = haveReturn && is_overworld_stage(retStage);

    if (exactSpot) {
        // Same sequence as daAlink_c::dungeonReturnWarp (d_a_alink_demo.inc:4390-4400): spawn at
        // the stored restart-room position via start point -1, layer -1 as GC's own point -1 restarts
        // (dStage_restartRoom, d_stage.cpp:3070-3074), so a cutscene layer of the entry never comes back.
        // A stage change already set (the unhooked dStage_changeScene before post_changeSceneExitId) must not win:
        // dStage_nextStage_c::set ignores a second one (d_stage.cpp:34-50; fix L5).
        const cXyz pos(b.ret_pos[0], b.ret_pos[1], b.ret_pos[2]);
        dComIfGp_offEnableNextStage();
        dComIfGs_setRestartRoom(pos, b.ret_angle, b.ret_room);
        dComIfGp_setNextStage(retStage, -1, b.ret_room, -1, speed, lastMode, 0, 0, angle, 1, 0);
        dComIfGs_setRestartRoomParam(daPy_py_c::setParamData(b.ret_room, 0, startEvent, 0));
        mods::log::info("exit -> {} room {} (mode {}, event 0x{:X})", retStage,
            static_cast<int>(b.ret_room), lastMode, startEvent);
    } else {
        // Not a field entry, or no stored return point (e.g. started with --stage D_SB11): go to the save's return
        // place, exactly like loading the file (dComIfGs_gameStart, d_com_inf_game.cpp:2327).
        dSv_player_return_place_c& place = g_dComIfG_gameInfo.info.getPlayer().getPlayerReturnPlace();
        char stage[9] = {};
        std::memcpy(stage, place.getName(), 8);
        if (stage[0] != '\0' && std::strcmp(stage, kStageName) != 0) {
            // GC's own call (offEnableNextStage, then setNextStage with the return place; fix L5): the same stage
            // change as HD's reload and the Record screen's exit (record_finished).
            dComIfGs_gameStart();
            mods::log::info("exit -> save point {} ({})", stage,
                haveReturn ? "entered from a non-field stage" : "no stored return point");
        } else {
            // Last resort: Ordon Village, the randomizer's "return to spawn" target.
            dComIfGp_offEnableNextStage();
            dComIfGp_setNextStage("F_SP103", 1, 1, -1);
            mods::log::info("exit -> F_SP103 (no return point and no save point)");
        }
    }
    // The entry's form comes back in both cases (the restore only turns the cave's forced wolf back to human).
    g_pendingFormRestore = haveReturn;
    g_formToRestore = b.ret_form;

    // FLAG_HAS_RETURN is cleared by on_leave() once the stage really changed, so a cancelled
    // stage change keeps the return point for the next attempt.
    g_pendingWithdraw = false;
    g_phase = Phase::Exiting;
    g_phaseTimer = 0;
}

void before_wolf_change_check() {
    // Not gated by the switch (only by the vanilla game mode): the restore belongs to a trip the cave
    // made, and the switch may go off between the exit's stage change and the creation of the next
    // scene's Link (review "api" #1). The hook stays installed while the switch is off.
    if (!g_pendingFormRestore || !g_initialized || !g_vanilla || in_cave_stage()) {
        return;
    }
    g_pendingFormRestore = false;
    // FIX (review "logic" #6): only undo the cave's forced wolf form. A stale return point (save made in
    // the cave, then played with the mod disabled) must never turn a human Link into a wolf.
    if (g_formToRestore == 0 && dComIfGs_getTransformStatus() != 0) {
        dComIfGs_setTransformStatus(0);
        mods::log::debug("restored human form after the cave");
    }
}

bool colossal_wallet_active() {
    return g_slotActive && (g_blob.flags & FLAG_COLOSSAL) != 0 && enabled();
}

// FIX (review "logic" #2): HD's wallet grants are "next wallet" grants. HD zel_04 node 1727 (Agitha's
// first reward) gives Giant if you have Big, node 1743 (all 24 bugs) gives Colossal if you have Giant, and
// the cave's flow 1122 does the same. GC gives fixed wallets, so after the cave's Big/Giant reward
// Agitha's grant would be a no-op (or a downgrade) and the Colossal Wallet unreachable.
bool wallet_lv2_override() {
    if (!enabled() || !g_slotActive) {
        return false;
    }
    const u8 size = dComIfGs_getWalletSize();
    if (size == 0) {
        return false;  // vanilla: Big Wallet
    }
    if (size == 1) {
        dComIfGs_setWalletSize(2);
        toast("Wallet upgrade", "You already had the Big Wallet, so it became the Giant Wallet.", false, 6000);
        mods::log::info("Big Wallet grant with a Big Wallet -> Giant Wallet");
    }
    return true;  // never shrink a bigger wallet back to the Big Wallet
}

bool wallet_lv3_override() {
    if (!enabled() || !g_slotActive || dComIfGs_getWalletSize() < 2) {
        return false;  // vanilla: Giant Wallet
    }
    grant_colossal("Giant Wallet grant with a Giant Wallet");
    return true;  // the save keeps wallet size 2; the Colossal state lives in the blob
}

// ---------------------------------------------------------------------------------------------
// HD sections: tier ALLdie, cave timer, portal bookkeeping, death-continue.
// ---------------------------------------------------------------------------------------------

std::uint8_t section_tier() {
    return g_tier;
}

std::uint8_t portal_tier() {
    if (!in_cave_stage()) {
        return 0;
    }
    const int room = dComIfGp_roomControl_getStayNo();
    std::uint8_t roomTier = 0;
    if (room == kRestRoom1) {
        roomTier = 1;
    } else if (room == kRestRoom2) {
        roomTier = 2;
    } else if (room == kFinalRoom) {
        roomTier = 3;
    }
    if (roomTier == 0) {
        return 0;  // not a portal floor
    }
    // HD uses los+0x1E as it stands (each rest floor's own tier ALLdie fires on load; floor 40's writes
    // 3 before floor 41). The room's tier covers a missing ALLdie hook or a record that never fired.
    return g_tier != 0 ? g_tier : roomTier;
}

void set_section_cleared(std::uint8_t tier) {
    std::uint32_t flag = 0;
    std::uint16_t label = 0;
    switch (tier) {
    case 1:
        flag = FLAG_SECTION1_DONE;
        label = kLabelTier1;
        break;
    case 2:
        flag = FLAG_SECTION2_DONE;
        label = kLabelTier2;
        break;
    case 3:
        flag = FLAG_SECTION3_DONE;
        label = kLabelTier3;
        break;
    default:
        return;
    }
    if ((g_blob.flags & flag) == 0) {
        g_blob.flags |= flag;
        mods::log::info("section {} cleared (HD label {})", static_cast<int>(tier), label);
    }
}

void clear_stage_chests() {
    if (!in_cave_stage()) {
        return;  // only ever D_SB11's own stage memory
    }
    // HD 0x02aa7c68 zeroes memBit mTbox[0..1] (all 64 chest bits) of the current stage memory, which
    // dStage_Delete then stores back into the stage's save table (putSave). The mod's data gives the three
    // cave chests the unique numbers 60/61/62; clearing all 64 is what HD does.
    for (int i = 0; i < 64; ++i) {
        dComIfGs_offTbox(i);
    }
}

int alldie_tier_switch(std::uint32_t prm, int room) {
    const int swbit = static_cast<int>((prm >> 8) & 0xFF);
    const int alt = static_cast<int>((prm >> 16) & 0xFF);
    const std::uint8_t tier = static_cast<std::uint8_t>(prm & 0xFF);
    if (tier < 1 || tier > 3) {
        return swbit;  // HD: tier 0 or >= 4 behaves like GC
    }
    g_tier = tier;  // HD writes los+0x1E before testing the flags, in both branches
    bool unlocked = true;
    if (tier == 1) {
        unlocked = section2_unlocked();
    } else if (tier == 2) {
        unlocked = section3_unlocked();
    }
    const char* why = unlocked ? "unlocked" : "locked";
    if (!unlocked && !hd_sections()) {
        unlocked = true;  // "HD sections" option off: every section behaves as unlocked (one run)
        why = "locked, but the HD sections option is off";
    }
    if (!unlocked && (room == kSectionEndRoom1 || room == kSectionEndRoom2) && !talk_tag_active(room)) {
        // Floor 5/19 end the run with the voice's talk (Tag_ms, flow 1121), whose end opens the gate. Without
        // that tag (no HD text / flow hook) the gate would never open: open it now and say the talk's line
        // as a notice (cos_sections_spec.md M8 fallback). The rest floor still shows its portal.
        unlocked = true;
        why = "locked, no section-end talk: notice instead";
        section_end_notice();
    }
    const int sw = unlocked ? swbit : alt;
    mods::log::info("tier ALLdie (room {}, prm {:08X}): tier {} {} -> switch 0x{:02X}", room, prm,
        static_cast<int>(tier), why, sw);
    return sw;
}

void on_hd_switch_area(int hdType) {
    if (!cave_rules() || !g_run.active) {
        return;
    }
    if (hdType == 10 && g_run.timer == CaveTimer::Fresh) {
        // HD 0x02891d98: dComIfG_TimerStart(11, 0), which only starts a timer from its fresh state.
        g_run.timer = CaveTimer::Running;
        mods::log::info("cave timer started (floor {})", floor_of_room(dComIfGp_roomControl_getStayNo()));
    } else if (hdType == 11 && g_run.timer == CaveTimer::Running) {
        // HD 0x02891df4: dComIfG_TimerStop(11) while it runs.
        g_run.timer = CaveTimer::Stopped;
        mods::log::info("cave timer stopped (floor {}, {})", floor_of_room(dComIfGp_roomControl_getStayNo()),
            format_time(g_run.ticks));
    }
}

void continue_after_death() {
    // HD daAlink_c::procDead, cave branch (Zelda.rpx 0x020b9f14-0x020b9ff0, VERIFIED): with the cave
    // controller present, "Continue" restarts the run on floor 1 instead of the restart room:
    //  1. zero the current stage memory's chest bits (0x02aa7c68) and store it (0x02aa856c = putSave);
    //  2. reset the dungeon (dan) switches (mDan.mStageNo = -1, then init(save table)): every floor's gate
    //     and ALLdie switch 0x80-0xBF is off again, as on a fresh entry;
    //  3. setNextStage("D_SB11", point 0, floor 1's room, layer -1), restart info (0, 0), turn-restart param
    //     setParamData(0, 1, 0xCA, 0);
    //  4. life = max (HD copies the max-life field into the life field), wolf.
    // HD then deletes and recreates its cave controller (spec-check correction A): tier 0, and the entry
    // bookkeeping runs again (it only clears 829 here, so there is no arrival intro).
    if (g_restartRun && scene_busy()) {
        // API review INFO: a second scene-change path of the same death (both hooked) while the first restart is
        // still pending. GC's death code restarts once (d_a_alink.cpp:13583-13611); this only keeps it so.
        mods::log::debug("game over in D_SB11: Continue already under way");
        return;
    }
    mods::log::info("game over in D_SB11: Continue restarts the run on floor 1 (HD)");
    if (g_run.active) {
        end_run(RunEnd::GameOver);
    }
    clear_stage_chests();
    if (stage_stag_info_class* stag = dComIfGp_getStageStagInfo(); stag != nullptr) {
        dComIfGs_putSave(dStage_stagInfo_GetSaveTbl(stag));
    }
    dComIfGs_resetDan();  // the floor-1 load's dStage_stagInfoInit -> initDan(table) then clears them
    const int startRoom = entry_room();  // HD: floor 1; the non-HD start_deepest option: same as the entry
    dComIfGp_setNextStage(kStageName, 0, static_cast<s8>(startRoom), -1, 0.0f, 0, 1, 0, 0, 1, 0);
    g_dComIfG_gameInfo.info.getRestart().setLastSceneInfo(0.0f, 0, 0);
    dComIfGs_setTurnRestart(dComIfGs_getTurnRestartPos(), 0, 0, daPy_py_c::setParamData(0, 1, 0xCA, 0));
    dComIfGs_setLife(dComIfGs_getMaxLifeGauge());
    // HD: the old controller's delete (0x0290acd8) puts the entry rupees back (the death path does not keep it,
    // spec-check correction A); the new one snapshots full life and those rupees again.
    if (g_snap.valid) {
        dComIfGs_setRupee(std::min<std::uint16_t>(g_snap.rupee, dComIfGs_getRupeeMax()));
    } else {
        g_snap.valid = true;
        g_snap.rupee = dComIfGs_getRupee();
    }
    g_snap.life = dComIfGs_getLife();
    g_portalDash = false;
    g_tier = 0;
    entry_bookkeeping();
    // The visit goes on (FLAG_HAS_RETURN and the phase stay); a new run record starts once floor 1 is up.
    g_arrivalRoom = startRoom;
    g_lastRoom = -1;
    g_roomSynced = false;
    g_toastedFloors = 0;
    g_rewardRoomTicks = 0;
    g_pendingWithdraw = false;
    g_restartRun = true;
}

// ---------------------------------------------------------------------------------------------
// HD exit portal (research/cos_portal_spec.md) and the "Cave of Shadows Record" screen (record.cpp).
// ---------------------------------------------------------------------------------------------

void set_portal_dash(bool on) {
    g_portalDash = on && cave_rules();
}

bool portal_dash_active() {
    return g_portalDash && cave_rules();
}

bool portal_exit_running() {
    return g_portalExit;
}

void on_portal_scene_chg(daObjBossWarp_c*) {
    if (!cave_rules() || g_portalExit) {
        return;
    }
    g_portalDash = false;  // HD Link+0x3968 = -1.0
    // The floor-41 reward when the talk never happened (exit_to_return_point did this for the old fade).
    if (g_phase == Phase::InCave && !g_rewardedThisVisit && dComIfGp_roomControl_getStayNo() == kFinalRoom) {
        mods::log::info("left floor {} before the reward talk; rewarding on the way out", kLastFloor);
        reward_without_talk();
    }
    // HD 0x026d3360/7c/98: the section by the controller's tier; 0x026d33a4: the rupees collected stay (the
    // controller's entry value becomes the current one); 0x02aa7c68: the stage's chest bits are zeroed.
    const std::uint8_t tier = portal_tier();
    set_section_cleared(tier);
    if (g_snap.valid) {
        g_snap.rupee = dComIfGs_getRupee();
    }
    clear_stage_chests();
    mods::log::info("portal SCENE_CHG (room {}, tier {}): section cleared, rupees {} kept, chests cleared",
        dComIfGp_roomControl_getStayNo(), static_cast<int>(tier), dComIfGs_getRupee());
}

void on_portal_scene_chg_first_frame(daObjBossWarp_c*) {
    if (!cave_rules() || g_portalExit) {
        return;
    }
    g_portalExit = true;
    g_pendingWithdraw = false;
    // HD 0x026d37b0: Midna is hidden (m_midnaActor), not the portal.
    if (daMidna_c* midna = daPy_py_c::getMidnaActor(); midna != nullptr) {
        fopAcM_offDraw(midna);
    }
    // The run as HD's proc 0x42 records it.
    RecordValues v;
    const int room = dComIfGp_roomControl_getStayNo();
    const int floor = floor_of_room(room);
    v.timeMs = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(g_run.ticks) * 1000 / kTicksPerSecond, 0xFFFFFFFFu));
    v.damage = static_cast<int>(std::min<std::uint32_t>(g_run.damage, 0x7FFF));
    v.floor = floor > 0 && floor <= kLastFloor ? floor : g_run.floorTo;
    v.life = std::min<int>(dComIfGs_getLife(), 0xFF);
    v.maxLife = std::min<int>(dComIfGs_getMaxLife(), 0xFF);
    v.combo = std::min<int>(g_run.maxCombo, 0xFF);
    v.wolf = g_run.heals > 0 ? g_run.healLevel : 0;
    g_recordValues = v;
    record::Input input;
    input.cur = v;
    input.hasPrev = g_blob.hdrec.life > 0;
    input.prev.timeMs = g_blob.hdrec.time_ms;
    input.prev.damage = g_blob.hdrec.damage;
    input.prev.floor = g_blob.hdrec.floor;
    input.prev.life = g_blob.hdrec.life;
    input.prev.maxLife = g_blob.hdrec.max_life;
    input.prev.combo = g_blob.hdrec.combo;
    input.prev.wolf = g_blob.hdrec.wolf;
    input.prev.ganon = g_blob.hdrec.ganon;
    input.prev.zelda = g_blob.hdrec.zelda;
    input.figure = amiibo_data();
    const FloorKind kind = floor_kind(floor);
    const RunEnd end = kind == FloorKind::Reward ? RunEnd::FullClear : RunEnd::SectionEnd;
    if (record::available() && record::request(input)) {
        // HD 0x026d37c4: the event ends here (no cut end) and the pause-menu window takes over (dMw+0x166).
        end_run(end, true);
        dComIfGp_event_remove();
        return;
    }
    // Without the Record screen: the old exit (fade to the stored return point, record notice, figure written).
    // The event keeps running so Link stays frozen and dissolved until the fade ends (spec 3.5).
    mods::log::info("D_SB11 portal warp finished (room {}); the Record screen is unavailable", room);
    daAlink_c* link = alink();
    exit_to_return_point(0, 0xCA, 0.0f, link != nullptr ? link->shape_angle.y : 0, ExitCause::Portal);
}

void record_store(const RecordValues& v) {
    // HD proc 0x42 (0x02a0cc8c-0x02a0cd88): the record shown becomes the save's previous record.
    HdRecord& r = g_blob.hdrec;
    r.time_ms = v.timeMs;
    r.damage = static_cast<std::int16_t>(std::clamp(v.damage, 0, 0x7FFF));
    r.floor = static_cast<std::uint8_t>(std::clamp(v.floor, 0, 0xFF));
    r.life = static_cast<std::uint8_t>(std::clamp(v.life, 0, 0xFF));
    r.combo = static_cast<std::uint8_t>(std::clamp(v.combo, 0, 0xFF));
    r.wolf = static_cast<std::uint8_t>(std::clamp(v.wolf, 0, 0xFF));
    r.ganon = static_cast<std::uint8_t>(std::clamp(v.ganon, 0, 0xFF));
    r.zelda = static_cast<std::uint8_t>(std::clamp(v.zelda, 0, 0xFF));
    r.max_life = static_cast<std::uint8_t>(std::clamp(v.maxLife, 0, 0xFF));
    mods::log::info("Cave of Shadows Record stored: {} ms, combo {}, damage {}, floor {}, life {}", r.time_ms,
        r.combo, r.damage, r.floor, r.life);
}

bool open_amiibo_write() {
    g_writeScan = true;
    if (amiibo::open_scan(ScanMode::Write)) {
        return true;  // amiibo_scan_result(Write) does the write at the "touch"; amiibo_scan_closed reports back
    }
    g_writeScan = false;
    const ScanResult result = amiibo_scan_result(ScanMode::Write);
    toast("Wolf Link amiibo", hd_plain(hdtext::kZel00, result == ScanResult::WriteOkFirst ? 1568 : 1569,
                                  "Your hearts were stored in the Wolf Link amiibo.").c_str(),
        false, 6000);
    return false;
}

void record_finished(bool saved) {
    if (!g_portalExit) {
        return;
    }
    // HD 0x029076e4 = dComIfGs_gameStart(): back to the save's return place (the last Savmem point; D_SB11 has
    // none and the entry does not write it, VERIFIED), black wipe 0, after the Record screen's save.
    dSv_player_return_place_c& place = g_dComIfG_gameInfo.info.getPlayer().getPlayerReturnPlace();
    char stage[9] = {};
    std::memcpy(stage, place.getName(), 8);
    if (stage[0] == '\0' || std::strcmp(stage, kStageName) == 0) {
        mods::log::warn("Cave of Shadows Record done ({}): the save has no return place; using the stored point",
            saved ? "saved" : "not saved");
        exit_to_return_point(0, 0xCA, 0.0f, 0, ExitCause::Portal);
        return;
    }
    dComIfGs_gameStart();
    // The cave's forced wolf form goes back to the entry form (as every exit of the mod does).
    g_pendingFormRestore = (g_blob.flags & FLAG_HAS_RETURN) != 0;
    g_formToRestore = g_blob.ret_form;
    g_pendingWithdraw = false;
    g_phase = Phase::Exiting;
    g_phaseTimer = 0;
    mods::log::info("Cave of Shadows Record done ({}): exit -> {} point {} room {} (the save's return place)",
        saved ? "saved" : "not saved", stage, static_cast<int>(place.getPlayerStatus()),
        static_cast<int>(place.getRoomNo()));
}

void before_memory_to_card() {
    if (g_cardSwap || !g_snap.valid || !cave_rules()) {
        return;
    }
    // HD 0x02aa8f98-0x02aa8fc4: a save made inside the cave stores the controller's life (if > 0) and rupees.
    g_cardSwap = true;
    g_cardLife = dComIfGs_getLife();
    g_cardRupee = dComIfGs_getRupee();
    if (g_snap.life > 0) {
        dComIfGs_setLife(g_snap.life);
    }
    dComIfGs_setRupee(g_snap.rupee);
    // FIX (HD review L5): a save made inside the cave always loads outside it (D_SB11 has no Savmem point), so it
    // stores the form the player entered with. The mod's warp-in turned a human Link into a wolf (POTAL_WARPIN
    // 057wchange); HD enters directly and never does. Without this, such a save (every Record-screen save) loaded
    // with the cave switch off or without the mod spawns a wolf once the Shadow Crystal makes the transform
    // status count (dComIfGs_Wolf_Change_Check, d_com_inf_game.cpp:2887-2888). With the mod on, the form
    // restore after the cave (before_wolf_change_check) gives the same result either way.
    g_cardFormSwapped = false;
    if ((g_blob.flags & FLAG_HAS_RETURN) != 0 && g_blob.ret_form == 0 && dComIfGs_getTransformStatus() != 0) {
        g_cardFormSwapped = true;
        g_cardForm = dComIfGs_getTransformStatus();
        dComIfGs_setTransformStatus(0);
    }
}

void after_memory_to_card() {
    if (!g_cardSwap) {
        return;
    }
    g_cardSwap = false;
    dComIfGs_setLife(g_cardLife);
    dComIfGs_setRupee(g_cardRupee);
    if (g_cardFormSwapped) {
        g_cardFormSwapped = false;
        dComIfGs_setTransformStatus(g_cardForm);
    }
}

void notify_transform_blocked() {
    play_se(Z2SE_SYS_ERROR);  // like tphd's handleQuickTransform block
    if (g_transformToastCooldown > 0) {
        return;
    }
    g_transformToastCooldown = kTicksPerSecond * 3;
    // HD's own refusal is Midna's line 5058 in flow 1111; this covers the quick-transform button.
    toast(hd_plain(hdtext::kZel00, 413, "Cave of Shadows").c_str(),
        "The power of this place keeps you in wolf form.", true, 3000);
}

void record_hook_failure(const char* what, Feature feature) {
    g_hookFailures.emplace_back(what);
    switch (feature) {
    case Feature::DemoGuard:
        g_criticalHookFailed = true;
        break;
    case Feature::AmiiboButton:
        g_entryHookFailed = true;
        break;
    case Feature::FlowRemap:
        g_flowHookFailed = true;
        break;
    case Feature::PortalWarp:
        g_portalHookFailed = true;
        break;
    case Feature::SectionTiers:
        g_tierHookFailed = true;
        break;
    case Feature::CaveTimer:
        g_timerHookFailed = true;
        break;
    case Feature::WithdrawRestore:
        g_restoreHookFailed = true;
        break;
    case Feature::PortalCutscene:  // logged only: the portal falls back by itself (hooks.cpp)
    case Feature::None:
        break;
    }
}

bool feature_ok(Feature feature) {
    switch (feature) {
    case Feature::DemoGuard:
        return !g_criticalHookFailed;
    case Feature::AmiiboButton:
        return !g_entryHookFailed;
    case Feature::FlowRemap:
        return !g_flowHookFailed;
    case Feature::PortalWarp:
        return !g_portalHookFailed;
    case Feature::SectionTiers:
        return !g_tierHookFailed;
    case Feature::CaveTimer:
        return !g_timerHookFailed;
    case Feature::WithdrawRestore:
        return !g_restoreHookFailed;
    case Feature::PortalCutscene:
    case Feature::None:
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// HD labels >= 822 and the HD-only flow queries/events.
// ---------------------------------------------------------------------------------------------

bool hd_label(std::uint16_t label) {
    switch (label) {
    case kLabelReward:
        return (g_blob.flags & FLAG_CLEARED_ONCE) != 0;
    case kLabelAmiibo:
        return dComIfGs_isEventBit(kFlagFaronSpiritRevived) != 0;
    case kLabelTier1:
        return (g_blob.flags & FLAG_SECTION1_DONE) != 0;
    case kLabelMirror:
        return dComIfGs_isEventBit(kFlagMirrorShard) != 0;
    case kLabelTier2:
        return (g_blob.flags & FLAG_SECTION2_DONE) != 0;
    case kLabelSols:
        return sols_in_sword();
    case kLabelTier3:
        return (g_blob.flags & FLAG_SECTION3_DONE) != 0;
    case kLabelIntro:
        return (g_blob.hd_labels & (1u << (kLabelIntro - kLabelReward))) != 0;
    default:
        return false;
    }
}

void set_hd_label(std::uint16_t label, bool on) {
    if (label < kLabelReward || label > kLabelIntro) {
        return;
    }
    if (label == kLabelIntro) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << (kLabelIntro - kLabelReward));
        g_blob.hd_labels = on ? (g_blob.hd_labels | bit) : (g_blob.hd_labels & ~bit);
        return;
    }
    // The other labels are derived from the mod's own state / GC story flags; HD flows only write 822
    // (flow 1122, replaced by the mod's reward talk), 827 (flow 811, GC has its own Sols scene, see
    // sols_in_sword) and 829. 824/826/828 are written by the portal exit (set_section_cleared).
    mods::log::debug("HD label {} write ignored (derived)", label);
}

std::uint16_t intro_query() {
    // HD query055 (Zelda.rpx 0x02a64650, VERIFIED; its node parameter is ignored), checked in this order:
    //   1 if 0x7001 is clear                       -> message 5060
    //   2 if 0x7001 && 0x5420 && !0x7002           -> message 5061
    //   3 if 0x7002 && 0x7101 && !0x7004           -> 5061
    //   0 otherwise                                -> event045 (no message, the demo resumes)
    if (!label_set(kLabelTier1)) {
        return 1;
    }
    if (section2_unlocked() && !label_set(kLabelTier2)) {
        return 2;
    }
    if (section3_unlocked() && !label_set(kLabelTier3)) {
        return 3;
    }
    return 0;
}

void flow_withdraw(bool afterMessage) {
    if (!cave_rules() || g_portalExit || g_phase == Phase::Exiting || g_warp.kind != WarpKind::None) {
        return;  // FIX (HD review L6): never during a portal exit (its Record screen / return is under way)
    }
    g_stopWithdrawQueued = false;  // the player's own Withdraw: turning the switch back on keeps it
    // HD (event044 + the reload, see g_restoreWithdraw): the "Save and begin" save comes back. Only for a visit that
    // began with that save; the mod's own exits (switch-off, the panel button) keep the stored-return-point warp.
    const bool restore = hd_withdraw() && g_visitSaved && g_tripValid;
    if (hd_withdraw() && !restore) {
        mods::log::info("Withdraw: {}; the mod's own exit instead (the visit is kept)",
            !g_visitSaved ? "this visit started without a save (debug Enter button)"
                          : "the \"Save and begin\" save image is not available");
    }
    g_restoreWithdraw = restore;
    // Midna's warp-out needs the talk (flow 1111) or, for flow 1120's turn-back after message 5062, the arrival
    // cutscene to be over (the walk of that flow runs before 5062 is on screen): the pending Withdraw waits for no
    // event, message or pause, then plays it (tick -> do_withdraw).
    g_pendingWithdraw = true;
    g_pendingWithdrawTimer = afterMessage ? kWithdrawCutsceneWaitTicks : kWithdrawWaitTicks;
    g_pendingWithdrawCutscene = afterMessage;
    mods::log::info("Withdraw chosen in a cave flow ({}); it starts when the {} is over",
        restore ? "HD: Midna's warp-out, then the \"Save and begin\" save" : "the mod's own exit",
        afterMessage ? "cutscene" : "talk");
}

void on_stage_deleted() {
    if (!g_restorePending) {
        return;
    }
    g_restorePending = false;
    // Only for the stage change finish_hd_withdraw issued: out of D_SB11 into its target, no reset to the title.
    if (!restore_target_pending()) {
        const char* next = dComIfGp_isEnableNextStage() ? dComIfGp_getNextStageName() : nullptr;
        mods::log::warn("Withdraw (HD): the cave was left another way ({}); the \"Save and begin\" save is not put back",
            mDoRst::isReset() ? "reset" : next == nullptr ? "no stage change" : next);
        return;
    }
    restore_trip_save(RestoreAt::StageDeleted);
}

std::uint16_t reward_query() {
    return static_cast<std::uint16_t>(reward_kind());
}

void flow_reward() {
    if (!cave_rules()) {
        return;
    }
    grant_reward();
}

// ---------------------------------------------------------------------------------------------
// Hook helpers.
// ---------------------------------------------------------------------------------------------

namespace {

void unsuspend_demo() {
    if (dDemo_c::getControl() != nullptr) {
        dDemo_c::getControl()->unsuspend(1);
    }
}

// dMsgObject_c::setDemoMessageLocal without the INF1 index lookup: set the demo-message flag, then show
// the message by id (custom ids resolve through MessageService, setMessageIndex TARGET_PC path).
bool show_demo_message(std::uint16_t messageId) {
    dMsgObject_c* object = dComIfGp_getMsgObjectClass();
    if (object == nullptr) {
        return false;
    }
    object->field_0x4cc = 1;
    dMsgObject_c::onCameraCancelFlag();
    const fpc_ProcID id = fopMsgM_messageSet(messageId, 1000);
    if (id == 0 || id == fpcM_ERROR_PROCESS_ID_e) {
        object->field_0x4cc = 0;
        return false;
    }
    return true;
}

}  // namespace

bool on_demo_message(std::uint32_t value) {
    if ((value & 0x80000000u) == 0) {
        return false;  // GC data: an INF1 index, let the game show it
    }
    // HD cutscenes (D_SB11 R19/R26/R02 r00_in.stb, VERIFIED: 0x80000460 and 0x80000000) pass top-bit
    // values; v2.0.3 would index entries[0x80000460] of the message table on a 64-bit build (the tphd
    // branch had to special-case this). HD's dispatcher (adaptor_do_MESSAGE 0x02911f9c, VERIFIED) starts
    // flow `value & 0x7FFFFFFF` while its cave controller exists, and nothing for flow 0: R19's floor-1
    // intro carries flow 1120 (the arrival flow, which clears label 829 itself), R26/R02's camera-only
    // intros on floors 7/21 carry 0.
    const std::uint32_t flow = value & 0x7FFFFFFFu;
    std::uint16_t message = 0;
    if (cave_rules()) {
        if (flow == hdtext::kFlowArrival) {
            message = hdtext::arrival_message();
        } else if (flow != 0) {
            mods::log::warn("cutscene message: unknown HD flow {}", flow);
        }
    }
    if (message == 0 || !show_demo_message(message)) {
        // No message: undo the cutscene's coming suspend(1) (its "Wait" block waits for a message); HD
        // flow 1120 does the same with event045 (unsuspend(1)) when it shows nothing.
        unsuspend_demo();
    }
    mods::log::info("cutscene message 0x{:08X} (flow {}) -> {}", value, flow, message);
    return true;
}

int remap_flow(fopAc_ac_c* partner, int flowId) {
    if (!cave_rules()) {
        return flowId;
    }
    std::uint16_t start = 0;
    if (flowId == 0xBB9 && partner != nullptr &&
        partner == static_cast<fopAc_ac_c*>(daPy_py_c::getMidnaActor()))
    {
        start = hdtext::flow_start(hdtext::kFlowMidna);  // tphd: daMidna_c uses 0x457 in the cave
    } else if (flowId == hdtext::kFlowMidna || flowId == hdtext::kFlowArrival ||
               flowId == hdtext::kFlowSectionEnd || flowId == hdtext::kFlowReward)
    {
        start = hdtext::flow_start(static_cast<std::uint16_t>(flowId));
    }
    return start != 0 ? start : flowId;
}

bool on_portal_warp_end(daAlink_c* link) {
    if (g_warp.kind == WarpKind::None || link == nullptr) {
        return false;  // a vanilla map warp
    }
    if (link->mProcID == daAlink_c::PROC_WARP) {
        if (link->mProcVar3.field_0x300e <= 0) {
            return true;  // already handled (the original returns here too)
        }
        link->mProcVar3.field_0x300e = -1;
    }
    if (!g_warp.finished) {
        g_warp.finished = true;
        if (g_warp.kind == WarpKind::Enter) {
            do_enter_stage();
        } else if (g_restoreWithdraw) {
            finish_hd_withdraw();  // HD's Withdraw: the "Save and begin" save's return place
        } else {
            finish_withdraw_stage(link->shape_angle.y);
        }
    }
    return true;
}

bool intro_tag_allowed(daTag_Event_c* tag) {
    if (tag == nullptr || !cave_rules()) {
        return true;
    }
    // Floor 1 (R19) has two arrival TagEvs on the start ledge (VERIFIED, research/cave_D_SB11_rooms.txt):
    //   #1 valid label 78 (M_045), invalid label 250 (F_0250): GC labels, the original tested them;
    //   #2 valid label 829 (HD-only): cave_patch.py --tagev (or the pre-hook) turns its labels into "none"
    //      (angle.x 0x7FFF, angle.z 0xFFFF), so the HD label is tested here: it plays only while 829 is set,
    //      i.e. after an entry whose bookkeeping announced something new.
    // Floors 7 (R26) and 21 (R02) have a TagEv without conditions: HD plays their camera-only intro every
    // time Link walks in, so every other TagEv keeps GC's own result.
    if (fopAcM_GetRoomNo(tag) != kEntryRoom) {
        return true;
    }
    const std::uint16_t invalidLabel = static_cast<std::uint16_t>(tag->home.angle.x) & 0x7FFF;
    const std::uint16_t validLabel = static_cast<std::uint16_t>(tag->home.angle.z);
    if (invalidLabel != 0x7FFF || validLabel != 0xFFFF) {
        return true;  // TagEv #1
    }
    return hd_label(kLabelIntro);
}

void on_enemy_hit(fopAc_ac_c* enemy, bool killed) {
    if (!killed || enemy == nullptr || !g_run.active || fopAcM_GetGroup(enemy) != fopAc_ENEMY_e) {
        return;
    }
    ++g_run.combo;
    g_run.maxCombo = std::max(g_run.maxCombo, g_run.combo);
}

bool enter_button_visible() {
    return cfg_bool(g_cvarDebugEnter, false) || g_entryHookFailed;
}

std::string status_text() {
    if (!g_initialized) {
        // Nothing of the save is known before the feature first starts (no save observer yet).
        return "Progress shows here once the Cave of Shadows is on.";
    }
    char line[360];
    const char* where = "";
    char floorBuf[48] = {};
    if (in_cave_stage() && g_lastRoom >= 0) {
        const int floor = floor_of_room(g_lastRoom);
        if (floor > 0) {
            std::snprintf(floorBuf, sizeof(floorBuf), "On floor %d. ", floor);
            where = floorBuf;
        }
    }
    // HD's three sections are cleared by leaving through the portals on floors 6, 20 and 41.
    const int partsDone = ((g_blob.flags & FLAG_SECTION1_DONE) != 0 ? 1 : 0) +
                          ((g_blob.flags & FLAG_SECTION2_DONE) != 0 ? 1 : 0) +
                          ((g_blob.flags & FLAG_SECTION3_DONE) != 0 ? 1 : 0);
    std::snprintf(line, sizeof(line),
        "%sDeepest floor: %u/%d. Sections cleared: %d/3. Full clears: %u. Colossal Wallet: %s.",
        where, static_cast<unsigned>(g_blob.best_floor), kLastFloor, partsDone,
        static_cast<unsigned>(g_blob.clear_count), (g_blob.flags & FLAG_COLOSSAL) ? "yes" : "no");
    return line;
}

std::string availability_text() {
    if (hdfeatures::feature_failed(hdfeatures::Feature::Cave)) {
        return "Couldn't start (see Troubleshooting).";
    }
    if (hdfeatures::feature_waits_for_files(hdfeatures::Feature::Cave)) {
        return "Waiting for the game files.";  // wave3 F5 (as for the stamps): the switch is on, it is not off
    }
    if (!g_switchOn) {
        return "Turned off. Your progress is kept.";
    }
    if (!g_initialized) {
        return "Starting...";
    }
    if (g_stopRequested) {
        return in_cave_stage() || g_phase != Phase::Idle ? "Turning off: leaving the cave first." : "Turning off...";
    }
    if (in_cave_stage()) {
        const char* reason = nullptr;
        if (can_withdraw(&reason)) {
            return !feature_ok(Feature::FlowRemap) || hdtext::flow_start(hdtext::kFlowMidna) == 0
                       ? "In the cave. To leave, use \"Leave the cave\"."
                       : "In the cave. To leave, call Midna and choose Withdraw.";
        }
        return std::string("In the cave. ") + (reason != nullptr ? reason : "");
    }
    if (!g_vanilla) {
        return "Off: works in the normal game mode only.";
    }
    if (g_entryHookFailed) {
        return "The amiibo couldn't be set up; use \"Enter the cave now\".";
    }
    if (!g_slotActive || link_player() == nullptr) {
        return "Load a save to play.";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "This visit can reach floor %d.", reachable_floor());
    std::string text = buf;
    const char* reason = nullptr;
    if (!can_enter(&reason, true)) {
        text += " Can't enter right now: ";
        text += reason != nullptr ? reason : "";
    }
    return text;
}

std::string record_rml() {
    std::string rml = "<div class=\"cos-record\"><div class=\"cos-record-title\">" +
                      hd_rml(hdtext::kZel00, 1258, "Cave of Shadows: your record") + "</div>";
    const auto section = [&](const char* heading, const RunRecord& rec) {
        if (rec.end == static_cast<std::uint8_t>(RunEnd::None)) {
            return;
        }
        char floors[64];
        std::snprintf(floors, sizeof(floors), " (floors %u-%u)", rec.floorFrom, rec.floorTo);
        rml += "<div class=\"cos-rec-row\"><span class=\"cos-rec-label\">" + std::string(heading) + floors +
               "</span></div>";
        rml += record_toast_body(rec, false);
    };
    section("Last run", g_blob.last);
    section("Best full clear", g_blob.best);
    if (g_blob.last.end == 0 && g_blob.best.end == 0) {
        rml += "<div>No finished runs yet.</div>";
    }
    rml += "<div class=\"cos-rec-row\"><span class=\"cos-rec-label\">Best combo ever</span><span>" +
           std::to_string(g_blob.best_combo) + "</span></div>";
    rml += "<div class=\"cos-rec-row\"><span class=\"cos-rec-label\">" +
           hd_rml(hdtext::kZel00, 1260, "Level") + " (hearts stored in the amiibo)</span><span>" +
           amiibo::image("amiibo_wolflink.png", "cos-rec-figure") + " " +
           std::to_string(g_blob.stored_hearts) + "</span></div></div>";
    return rml;
}

std::string hook_problems_text() {
    if (hdfeatures::feature_failed(hdfeatures::Feature::Cave)) {
        return "Cave of Shadows: couldn't start (see the log). Turn HD Features off and on to retry.";
    }
    if (!g_initialized) {
        return hdfeatures::feature_waits_for_files(hdfeatures::Feature::Cave) ?
                   "Cave of Shadows: not started (waiting for the game files)." :
                   "Cave of Shadows: not started (switched off since the game started).";
    }
    if (!g_switchOn) {
        return "Cave of Shadows: off.";
    }
    // Only problems are listed (the details are in the log).
    const char* screen = confirm::problem();
    const bool screenOk = screen == nullptr || screen[0] == '\0';
    const bool textOk = hdtext::text_loaded() && hdtext::flow_start(hdtext::kFlowMidna) != 0;
    if (screenOk && textOk && g_hookFailures.empty()) {
        return "Cave of Shadows: OK.";
    }
    std::string text = "Cave of Shadows:";
    if (!textOk) {
        text += " " + hdtext::summary() + ".";
    }
    if (!screenOk) {
        text += std::string(" HD's \"Cave of Shadows\" screen is off (") + screen + ").";
    }
    if (g_hookFailures.empty()) {
        return text;
    }
    text += " Missing game hooks: ";
    for (std::size_t i = 0; i < g_hookFailures.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += g_hookFailures[i];
    }
    return text + ".";
}

std::uint64_t floor_toasts_var() {
    return g_cvarFloorToasts;
}

std::uint64_t hd_sections_var() {
    return g_cvarHdSections;
}

std::uint64_t amiibo_hint_var() {
    return g_cvarAmiiboHint;
}

std::uint64_t debug_enter_var() {
    return g_cvarDebugEnter;
}

std::uint64_t warp_effect_var() {
    return g_cvarWarpEffect;
}

std::uint64_t amiibo_button_var() {
    return g_cvarAmiiboButton;
}

std::uint64_t hd_withdraw_var() {
    return g_cvarHdWithdraw;
}

std::uint64_t start_deepest_var() {
    return g_cvarStartDeepest;
}

std::uint64_t entry_warp_var() {
    return g_cvarEntryWarp;
}

void toast(const char* title, const char* body, bool warning, std::uint32_t ms) {
    UiToastDesc desc = UI_TOAST_DESC_INIT;
    desc.type = warning ? "warning" : nullptr;
    desc.title_rml = title;
    desc.body_rml = body;
    desc.duration_ms = ms;
    amiibo::push_toast(desc);
}

// =============================================================================================
// Lifecycle.
// =============================================================================================

namespace {

ConfigVarHandle register_var(const char* name, ConfigVarType type, bool defBool, std::int64_t defInt) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = type;
    desc.default_bool = defBool;
    desc.default_int = defInt;
    ConfigVarHandle handle = 0;
    if (svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK) {
        mods::log::warn("could not register the {} config var", name);
        return 0;
    }
    return handle;
}

void apply_swallow() {
    if (g_swallowMask == 0) {
        return;
    }
    interface_of_controller_pad& pad = mDoCPd_c::getCpadInfo(PAD_1);
    pad.mPressedButtonFlags &= ~g_swallowMask;
    if (g_swallowMin > 0) {
        --g_swallowMin;
    }
    --g_swallowTicks;
    if ((g_swallowMin == 0 && (pad.mButtonFlags & g_swallowMask) == 0) || g_swallowTicks <= 0) {
        g_swallowMask = 0;
    }
}

// The pause menu's save screen (the trip's, or one the player opened) exists right now.
bool save_menu_up() {
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    return window != nullptr && window->mpMenuSave != nullptr;
}

void tick_entry(bool paused) {
    switch (g_entry) {
    case EntryStep::None:
    case EntryStep::Scanning:
        break;
    case EntryStep::Confirm:
        // confirm.cpp drives the screen and reports through confirm_finished(); this only covers a
        // screen that went away without it (the menu closed under it).
        if (!paused || !confirm::busy()) {
            g_entry = EntryStep::None;
            confirm::bgm_restore();
            mods::log::info("Cave of Shadows screen gone; amiibo trip dropped");
        } else if (g_stopRequested) {
            // FIX (review API #3): the switch is being turned off. A trip could no longer start, so "Save and
            // begin" would only save and then refuse: the screen leaves as Cancel (once its buttons take
            // input), and the switch-off goes ahead when it is back on the Collection screen.
            confirm::request_cancel();
        }
        break;
    case EntryStep::Saving: {
        // "Save and begin": the game's own pause-menu save (dMenu_save_c, opened by confirm.cpp or the
        // Collection screen's Save). Only a written slot (on_save_written) lets the trip go on; the save
        // menu then closes the pause menu by itself (confirm.cpp saveEnd hook, or GC's "Continue playing?
        // Yes" without that hook).
        ++g_savingTicks;
        if (g_tripSaved) {
            // FIX (review H1): stay in Saving until the save screen is gone. The slot is reported written
            // (memCardDataSaveWait) a frame or more before memCardDataSaveWait2 / saveEnd run, and this tick
            // runs before the game's process management: moving on at once left confirm.cpp's two save
            // hooks (they test save_trip_active / save_trip_saved) with nothing to act on, so the stock
            // "Saved. Continue playing?" showed instead of HD's "Saved." and the menu closing.
            if (paused && save_menu_up()) {
                break;
            }
            g_entry = EntryStep::WaitMenu;
            g_entryTimer = kMenuEnterWaitTicks;
            g_entrySettle = 0;
            g_entryRetry = kMenuCloseRetryTicks;
            g_entryBusyTicks = 0;
            g_menuClosedSeen = false;
            break;
        }
        // B in the log list, a memory card error or "No": the save screen went back to the Collection
        // screen (HD: end status 1 -> state 0x2E), or the menu closed without a save. No trip.
        dMw_c* window = dMeter2Info_getMenuWindowClass();
        const bool backOnCollection = paused && window != nullptr && window->mpMenuSave == nullptr &&
                                      window->mMenuProc == dMw_c::COLLECT_MOVE;
        if (g_savingTicks > 2 && (!paused || backOnCollection)) {
            g_entry = EntryStep::None;
            confirm::bgm_restore();
            mods::log::info("amiibo trip cancelled in the save menu (nothing saved)");
        }
        break;
    }
    case EntryStep::CloseMenu:
        // One tick after the scan screen closed (the host has dropped it and its input block), one B
        // trigger is injected after this frame's pad read and before the menu process runs: the
        // Collection screen closes through its normal B path (dMw_c::dMw_isPush_S_Button), with the
        // normal close sound and fade. Only while the pause menu is up, so B can never reach Link.
        if (++g_entrySettle < 2) {
            break;
        }
        g_entrySettle = 0;
        if (paused && g_collectOpen) {
            mDoCPd_c::getCpadInfo(PAD_1).mPressedButtonFlags |= PAD_BUTTON_B;
        }
        g_entry = EntryStep::WaitMenu;
        g_entryRetry = kMenuCloseRetryTicks;
        g_entryBusyTicks = 0;
        break;
    case EntryStep::WaitMenu:
        if (paused || scene_busy()) {
            g_entrySettle = 0;
            // FIX (review "cave entry" #5): the retry used `g_entryTimer % 15 == 0` with the timer starting
            // at 450, so it fired on the very next tick. Now it waits kMenuCloseRetryTicks and only fires
            // while the Collection screen is still in its idle process: once the menu window has started
            // closing (dMw_c::collect_close_proc no longer runs the screen's _move) no extra B is sent.
            if (paused && !g_menuClosedSeen && collect_idle() && --g_entryRetry <= 0) {
                g_entryRetry = kMenuCloseRetryTicks;
                mods::log::debug("pause menu still open; pressing B again");
                mDoCPd_c::getCpadInfo(PAD_1).mPressedButtonFlags |= PAD_BUTTON_B;
            }
            if (g_menuClosedSeen) {
                // After the menu had closed (also while waiting for a cutscene below), a menu opened or a scene change
                // started: drop the trip. The save is already written, so the player is told (fix L1).
                g_entry = EntryStep::None;
                confirm::bgm_restore();  // review L3: an area's own stream music comes back
                toast("Wolf Link amiibo", "The trip to the Cave of Shadows was cancelled.", true);
                mods::log::info("amiibo trip cancelled after the menu closed: {}",
                    paused ? "a menu opened" : "a scene change started");
            } else if (!save_menu_up() && --g_entryTimer <= 0) {
                // (Not while the save screen is still up: without the saveEnd hook it asks "Continue
                // playing?" and waits for the player.)
                g_entry = EntryStep::None;
                confirm::bgm_restore();
                toast("Wolf Link amiibo", "The trip to the Cave of Shadows was cancelled.", true);
            }
        } else {
            g_menuClosedSeen = true;
            if (++g_entrySettle >= kMenuSettleTicks) {
                g_entry = EntryStep::None;  // cleared first: mod_refusal() refuses while an entry is pending
                if (link_player() == nullptr) {
                    // Not back in the field (GC's "Continue playing? No" quits to the title screen).
                    confirm::bgm_stop();
                    mods::log::info("amiibo trip dropped: no player after the menu closed");
                    break;
                }
                // HD's rules gave their answer at the scan and are not applied again (HD never checks after "Save
                // and begin"). Only the mod's own conditions can still refuse here.
                if (const char* reason = mod_refusal(); reason != nullptr) {
                    confirm::bgm_restore();
                    play_se(Z2SE_SYS_ERROR);
                    toast("Wolf Link amiibo", hd_plain(hdtext::kZel00, 1530,
                                                  "The cave can't be entered from here.")
                                                  .c_str(),
                        true, 5000);
                    mods::log::info("amiibo trip refused after the menu closed: {}", reason);
                    break;
                }
                if (const char* busy = busy_refusal(false); busy != nullptr) {
                    // A cutscene, message or game over started as the menu closed: the mod's stage change waits for
                    // it to end (not HD: HD changes the stage from the held menu; this path runs only without it).
                    g_entry = EntryStep::WaitMenu;
                    g_entrySettle = kMenuSettleTicks;
                    if (++g_entryBusyTicks == 1) {
                        // The cutscene or message gets the area's music back, not the pre-entry screen's (fix L1).
                        confirm::bgm_restore();
                        mods::log::info("amiibo trip waits after the menu closed: {}", busy);
                    }
                    if (g_entryBusyTicks > kEntryBusyWaitTicks) {
                        g_entry = EntryStep::None;
                        confirm::bgm_restore();
                        toast("Wolf Link amiibo", "The trip to the Cave of Shadows was cancelled.", true);
                        mods::log::info("amiibo trip cancelled after the menu closed: {}", busy);
                    }
                    break;
                }
                capture_return_point();
                g_visitSaved = true;  // "Save and begin" wrote the slot (EntryStep::Saving -> WaitMenu)
                // Here only when HD's stage change from the held "Saved." menu did not run (its hook is missing,
                // the stock "Continue playing? Yes" closed the menu) or the non-HD warp-out option chose this way.
                if (entry_warp_option() && warp_effect_enabled() && portal_entry_allowed()) {
                    begin_warp(WarpKind::Enter, kStageName, dComIfGs_getTurnRestartPos(),
                        dComIfGs_getTurnRestartAngleY(), kEntryRoom);
                } else {
                    mods::log::info("amiibo trip: plain stage change after the menu closed ({})",
                        entry_warp_option() ? "Midna's warp-out is not possible here any more"
                                            : "HD's stage change from the save menu was not available");
                    do_enter_stage();
                }
            }
        }
        break;
    case EntryStep::Leaving:
        // HD dMw state 0x2D: "Saved." stays up while the stage change from the menu fades to black; the menu goes with
        // the play scene (confirm.cpp releases the hold in the dMw_c::_delete pre-hook).
        if (!paused || !confirm::save_held()) {
            // FIX (review API/HD #2): the pause menu is gone; a hold the dMw_c::_delete pre-hook did not end must never
            // outlive it (a later save menu at the same heap address would be held forever).
            confirm::forget_save_hold("the pause menu is gone");
            g_entry = EntryStep::None;
            break;
        }
        if (fopOvlpM_IsDoingReq() != 0 || fopOvlpM_IsPeek() != 0) {
            g_entryTimer = 0;  // the scene change's fade is running
            break;
        }
        if (++g_entryTimer > kSaveHoldTimeoutTicks) {
            // Never seen in the GC/HD code paths (dScnPly_Draw issues a pending stage change without a pause check,
            // d_s_play.cpp:559-591); the menu closes so the pending stage change can run once the game runs again.
            confirm::release_save_hold("the stage change from the menu did not start within 3 seconds");
            g_entry = EntryStep::None;
        }
        break;
    }
}

// Collection-screen amiibo prompt (DESIGN.md "Entry" 1). FIX (review "cave entry" #2): it used to be
// queued once per pause from the _create hook, so it waited behind other notices, played on after the
// menu had closed (over gameplay and the warp-out) and piled up when the menu was opened several times.
// Now it is visible only while the screen is idle, and pushed only when the mod's own toast queue is
// empty (amiibo::prompt_ready), at most once per pause.
void update_prompt(bool paused) {
    // Not while the switch is going off (request_stop): the scan would only be refused.
    const bool visible = paused && collect_idle() && !amiibo::scan_open() && g_entry == EntryStep::None &&
                         !g_stopRequested;
    amiibo::set_prompt_visible(visible);
    if (visible && !g_noticeCheckedThisPause && g_slotActive && g_vanilla) {
        // HD's one-shot notices 1545/1546/1547 (at most one per visit to the screen), ahead of the prompt.
        g_noticeCheckedThisPause = true;
        collection_notice();
    }
    if (!visible || g_promptShownThisPause) {
        return;
    }
    if (!amiibo_unlocked() || !cfg_bool(g_cvarAmiiboHint, true)) {
        return;
    }
    if (amiibo::prompt_pending()) {
        // A prompt from a moment ago is still on its way or showing (menu reopened quickly): it is
        // visible again now and counts as this visit's prompt.
        g_promptShownThisPause = true;
        return;
    }
    if (amiibo::show_prompt(amiibo_button_label())) {
        g_promptShownThisPause = true;
    }
}

// Leaving because the switch wants to go off (request_stop): the same path as Midna's Withdraw, at the
// next safe moment; retried by ../mod.cpp while the switch stays pending.
void tick_stop(bool paused) {
    if (!g_stopRequested || g_phase != Phase::InCave || !in_cave_stage() || g_pendingWithdraw ||
        g_warp.kind != WarpKind::None || paused || g_portalExit || g_restorePending)
    {
        return;  // (a Withdraw or the portal exit is already taking the player out)
    }
    if (!can_withdraw(nullptr)) {
        return;
    }
    if (!g_stopNoticeShown) {
        g_stopNoticeShown = true;
        toast(hd_plain(hdtext::kZel00, 413, "Cave of Shadows").c_str(),
            "The Cave of Shadows is being turned off, so you leave the cave.", false, 6000);
    }
    mods::log::info("Cave of Shadows switch turned off inside the cave: withdrawing first");
    g_stopWithdrawQueued = true;
    g_restoreWithdraw = false;  // the switch-off keeps the visit: back to the stored return point
    g_pendingWithdraw = true;
    g_pendingWithdrawTimer = kWithdrawWaitTicks;
    g_pendingWithdrawCutscene = false;
}

}  // namespace

void register_config() {
    g_cvarFloorToasts = register_var("cave-floor_toasts", CONFIG_VAR_BOOL, false, 0);
    g_cvarHdSections = register_var("cave-hd_sections", CONFIG_VAR_BOOL, true, 0);
    g_cvarAmiiboHint = register_var("cave-amiibo_hint", CONFIG_VAR_BOOL, true, 0);
    g_cvarDebugEnter = register_var("cave-debug_enter_button", CONFIG_VAR_BOOL, false, 0);
    g_cvarWarpEffect = register_var("cave-warp_effect", CONFIG_VAR_BOOL, true, 0);
    g_cvarAmiiboButton = register_var("cave-amiibo_button", CONFIG_VAR_INT, false, 0);
    g_cvarHdWithdraw = register_var("cave-hd_withdraw", CONFIG_VAR_BOOL, true, 0);
    g_cvarStartDeepest = register_var("cave-start_deepest", CONFIG_VAR_BOOL, false, 0);
    g_cvarEntryWarp = register_var("cave-entry_warp", CONFIG_VAR_BOOL, false, 0);
}

void set_switch(bool on) {
    if (on == g_switchOn) {
        return;
    }
    g_switchOn = on;
    g_stopRequested = false;
    g_stopNoticeShown = false;
    g_stopWithdrawQueued = false;
    if (!g_initialized) {
        return;  // initialize() installs everything with the switch already on
    }
    if (on) {
        refresh_mode();
        hdtext::initialize();  // messages, flows and talk tags back in
        mods::log::info("Cave of Shadows switch on");
    } else {
        // ../mod.cpp only gets here while busy() is false: nobody is in the cave or on the way.
        g_pendingEnter = false;
        g_pendingWithdraw = false;
        g_entry = EntryStep::None;
        g_swallowMask = 0;
        amiibo::cancel();
        amiibo::set_prompt_visible(false);
        hdtext::shutdown();  // no HD flows, message overrides or D_SB11 talk tags while off
        mods::log::info("Cave of Shadows switch off");
    }
}

bool switch_on() {
    return g_switchOn;
}

bool busy() {
    if (!g_initialized) {
        return false;
    }
    return in_cave_stage() || g_phase != Phase::Idle || g_warp.kind != WarpKind::None || g_pendingEnter ||
           (g_entry != EntryStep::None && g_entry != EntryStep::Scanning) || record::busy();
}

void request_stop(bool stop) {
    if (stop == g_stopRequested) {
        return;
    }
    g_stopRequested = stop;
    if (stop) {
        g_pendingEnter = false;  // no new trips (can_enter refuses as well)
    } else {
        g_stopNoticeShown = false;
        if (g_stopWithdrawQueued && g_pendingWithdraw) {
            // Switched back on before the warp started: stay in the cave (parity review L3). Once the
            // Withdraw has started (do_withdraw cleared g_pendingWithdraw), the trip out goes on.
            g_pendingWithdraw = false;
            mods::log::info("Cave of Shadows switch back on: the Withdraw it queued is cancelled");
        }
        g_stopWithdrawQueued = false;
    }
}

ModResult initialize(ModError* error) {
    if (g_initialized) {
        return MOD_OK;
    }
    refresh_mode();
    ModResult result = svc_save->observe_saves(
        mod_ctx, on_new_save, on_save_loaded, on_save_written, nullptr, &g_saveObserver);
    if (result != MOD_OK) {
        g_saveObserver = 0;
        return mods::set_error(error, result, "Cave of Shadows: could not observe saves");
    }
    if (svc_item->observe_gives(mod_ctx, on_item_given, nullptr, &g_giveObserver) != MOD_OK) {
        g_giveObserver = 0;
        mods::log::warn("could not observe item grants; the first-clear flag is set when the wallet is queued");
    }

    // Enabled or reloaded mid-game: pick up the current slot if there is one.
    std::size_t size = 0;
    if (svc_save->get_blob(mod_ctx, kBlobName, nullptr, &size) == MOD_OK) {
        g_slotActive = true;
        g_blobExists = load_blob();
    } else if (link_player() != nullptr) {
        g_slotActive = true;  // a file is being played; it simply has no cave blob yet
    }

    g_initialized = true;
    hooks::install_all();
    if (g_switchOn) {
        hdtext::initialize();  // after the hooks: the talk tags are only added when flows can be remapped
    }
    amiibo::initialize();
    // The Mods panel, the menu-bar tab and the window belong to ../panel.cpp (HD Features).

    if (g_switchOn && in_cave_stage()) {
        on_arrive();
    }
    mods::log::info("Cave of Shadows ready ({} hook problem(s))", g_hookFailures.size());
    return MOD_OK;
}

void tick() {
    if (!g_initialized) {
        return;
    }
    ++g_frame;
    if (--g_modeTimer <= 0) {
        g_modeTimer = 30;
        refresh_mode();
    }
    if (g_assetsCheckTimer > 0) {
        --g_assetsCheckTimer;
    }
    if (g_transformToastCooldown > 0) {
        --g_transformToastCooldown;
    }
    const bool paused = dComIfGp_isPauseFlag() != 0;
    if (!paused) {
        g_promptShownThisPause = false;  // the next pause-menu visit may show the amiibo prompt again
        g_noticeCheckedThisPause = false;
        g_collectOpen = false;
    }
    if (!g_switchOn) {
        // Switched off (HD Features): nothing of the cave runs; the save observers keep the blob in step.
        apply_swallow();
        return;
    }
    tick_stop(paused);
    apply_swallow();
    if (amiibo::scan_open() && !paused) {
        // The pause menu went away under the scan screen (it cannot normally: game input is blocked
        // while the screen is up). Never leave the scan screen over live gameplay.
        amiibo::cancel();
        amiibo_scan_closed(ScanResult::Cancelled, false);
    }
    amiibo::tick();
    confirm::tick();
    record::tick();
    tick_entry(paused);
    update_prompt(paused);

    const bool inCave = in_cave_stage();
    switch (g_phase) {
    case Phase::Idle:
        if (inCave) {
            on_arrive();
        } else if ((g_blob.flags & FLAG_HAS_RETURN) != 0 && link_player() != nullptr &&
                   g_warp.kind == WarpKind::None && g_entry == EntryStep::None &&
                   std::strcmp(dComIfGp_getStartStageName(), "F_SP102") != 0)
        {
            // Playing outside the cave with a leftover return point (loaded a save written
            // during a session): it is stale now.
            g_blob.flags &= ~FLAG_HAS_RETURN;
        }
        break;
    case Phase::Entering:
        if (inCave) {
            on_arrive();
        } else if (++g_phaseTimer > kPhaseTimeoutTicks) {
            mods::log::warn("stage change to D_SB11 did not happen; giving up");
            g_phase = Phase::Idle;
            g_warp = Warp{};
            undo_entry();  // the entry bookkeeping belongs to an arrival; the life before it comes back
            confirm::bgm_restore();  // FIX (review L3): the pre-entry screen's music must not stay on
        }
        break;
    case Phase::InCave:
        if (!inCave) {
            on_leave(false);
        }
        break;
    case Phase::Exiting:
        if (g_restorePending && g_restoreHookFailed && inCave && fopOvlpM_IsPeek() != 0 && restore_target_pending()) {
            // HD's Withdraw without the dStage_Delete hook: the fade is at black and the cave scene is being deleted.
            g_restorePending = false;
            restore_trip_save(RestoreAt::SceneGoing);
        }
        if (!inCave) {
            if (g_restorePending) {
                // Not seen at black (or the dStage_Delete hook did not run): put the save back now, if this is the
                // stage the Withdraw went to.
                g_restorePending = false;
                const char* start = dComIfGp_getStartStageName();
                if (start != nullptr && std::strcmp(start, g_restoreTarget) == 0 && !mDoRst::isReset()) {
                    mods::log::warn("Withdraw (HD): the save is put back late (the next stage has loaded)");
                    restore_trip_save(RestoreAt::NextLoaded);
                } else {
                    mods::log::warn("Withdraw (HD): the cave was left another way ({}); the save is not put back",
                        start != nullptr ? start : "?");
                }
            }
            on_leave(true);
        } else if (++g_phaseTimer > kPhaseTimeoutTicks) {
            mods::log::warn("exit from D_SB11 did not happen; back to in-cave state");
            g_phase = Phase::InCave;
            g_warp = Warp{};
            g_restoreWithdraw = false;
            g_restorePending = false;  // (the "Save and begin" image stays: Withdraw can be chosen again)
            if (g_portalExit) {
                // The Record screen's return never happened: Link is still dissolved and Midna hidden; leave by
                // the stored return point instead (a fade), so the player is never stuck.
                exit_to_return_point(0, 0xCA, 0.0f, 0, ExitCause::Portal);
            }
        }
        break;
    }
    tick_warp();

    if (!g_vanilla) {
        g_pendingEnter = false;
        g_pendingWithdraw = false;
        if (g_entry != EntryStep::None) {
            g_entry = EntryStep::None;
            amiibo::cancel();
        }
        return;
    }

    if (g_pendingEnter) {
        g_pendingEnter = false;
        const char* reason = nullptr;
        if (can_enter(&reason)) {
            capture_return_point();
            g_visitSaved = false;  // no save before this trip: Withdraw has no "Save and begin" save to go back to
            do_enter_stage();  // debug shortcut: no warp-out
        } else {
            toast("Cave of Shadows", reason != nullptr ? reason : "Not available right now.", true);
        }
    }

    if (g_pendingWithdraw) {
        // FIX (HD review L6): also never while a portal exit (Record screen, return) runs.
        if (!in_cave_stage() || g_phase == Phase::Exiting || g_warp.kind != WarpKind::None || g_portalExit) {
            g_pendingWithdraw = false;
            g_restoreWithdraw = false;
        } else if (link_player() != nullptr && !scene_busy() && !dComIfGp_event_runCheck() &&
                   dComIfGp_getGameoverStatus() == 0 && dComIfGs_getLife() != 0 && !paused &&
                   !message_box_up() &&
                   !(g_restoreWithdraw && g_rewardGiveCount > 0 && g_rewardWaitTicks++ < kRewardGiveWaitTicks))
        {
            // (HD's Withdraw first lets a reward still in ItemService's queue be handed out, which needs Link idle and
            // no event: its get-item plays, and the restore then drops it with the rest of the visit; review HD #4 /
            // API #4. ItemService has no way to take a queued give back.)
            if (g_rewardWaitTicks > kRewardGiveWaitTicks) {
                mods::log::warn("Withdraw (HD): a queued reward was not handed out within {} s; withdrawing anyway",
                    kRewardGiveWaitTicks / kTicksPerSecond);
            }
            g_rewardWaitTicks = 0;
            g_pendingWithdraw = false;
            do_withdraw();
        } else if (g_pendingWithdrawCutscene && (dComIfGp_event_runCheck() || message_box_up() || paused)) {
            // HD's event044 follows message 5062 directly (flow 1120): the turn-back is never dropped while that
            // cutscene, the message or the pause menu is still up.
        } else if (--g_pendingWithdrawTimer <= 0) {
            g_pendingWithdraw = false;
            g_restoreWithdraw = false;
            g_rewardWaitTicks = 0;
            if (!g_stopRequested) {  // the switch-off retries by itself (tick_stop)
                toast("Cave of Shadows", "Could not withdraw right now. Try again in a moment.", true);
            }
        }
    }

    if (g_phase == Phase::InCave) {
        track_run(paused);
        track_floor(paused);
    }
}

void shutdown() {
    // Disabling the mod mid-run: its overlay (the D_SB11 files) goes away with it, so never leave the
    // player in the cave or in the middle of the mod's portal warp.
    if (g_initialized && g_vanilla) {
        if (g_restorePending) {
            // HD's Withdraw has issued its stage change, and the dStage_Delete hook goes away with the mod: put the
            // "Save and begin" save back now (the cave's putSave then writes the image's own table back).
            g_restorePending = false;
            const char* start = dComIfGp_getStartStageName();
            if (restore_target_pending()) {
                restore_trip_save(RestoreAt::SceneGoing);
            } else if (!in_cave_stage() && start != nullptr && std::strcmp(start, g_restoreTarget) == 0 &&
                       !mDoRst::isReset())
            {
                restore_trip_save(RestoreAt::NextLoaded);
            }
        }
        if (g_warp.kind != WarpKind::None && !g_warp.finished) {
            if (dMeter2Info_getWarpStatus() == WARP_STATUS_DECIDED_e) {
                dMeter2Info_resetWarpStatus();
            }
            if (daAlink_c* link = alink(); link != nullptr && link->mProcID == daAlink_c::PROC_WARP) {
                link->mProcVar3.field_0x300e = -1;  // make the unhooked skipPortalObjWarp a no-op
            }
            if (g_warp.kind == WarpKind::Withdraw || in_cave_stage()) {
                exit_to_return_point(0, 0xCA, 0.0f, 0, ExitCause::Shutdown);
            } else {
                // Entry warp under way: reload the entry spot instead of an unhooked -4 arrival.
                reload_current_spot();
                // The warp event turned a human Link into a wolf (POTAL_WARPIN 057wchange); the form
                // restore hook goes away with the mod, so undo it here (review "cave entry", edge case).
                if ((g_blob.flags & FLAG_HAS_RETURN) != 0 && g_blob.ret_form == 0 &&
                    dComIfGs_getTransformStatus() != 0)
                {
                    dComIfGs_setTransformStatus(0);
                }
                g_blob.flags &= ~FLAG_HAS_RETURN;
            }
        } else if (g_phase == Phase::Entering && !in_cave_stage() && dComIfGp_isEnableNextStage()) {
            // The stage change to D_SB11 is issued (from the held "Saved." menu, or after it closed) and the overlay
            // goes away with the mod: reload the entry spot instead (the next play scene reads the next stage
            // only when it is created, d_s_play.cpp:1164), and undo what the entry changed (review HD #6).
            reload_current_spot();
            undo_entry();
            g_phase = Phase::Idle;
            g_blob.flags &= ~FLAG_HAS_RETURN;
        } else if (in_cave_stage() && g_phase != Phase::Exiting && !scene_busy()) {
            exit_to_return_point(0, 0xCA, 0.0f, 0, ExitCause::Shutdown);
        }
        if (in_cave_stage() && (g_blob.flags & FLAG_HAS_RETURN) != 0 && g_blob.ret_form == 0) {
            // The form restore hook is about to go: put the transform status back now; the player of
            // the next scene is created from it (dComIfGs_Wolf_Change_Check).
            dComIfGs_setTransformStatus(0);
        }
    }
    record::shutdown();   // the Record screen, before its hooks go
    confirm::shutdown();  // the pre-entry screen, before its hooks go
    amiibo::shutdown();
    ui::shutdown();
    hooks::uninstall_all();
    hdtext::shutdown();
    if (g_giveObserver != 0) {
        svc_item->unobserve_gives(mod_ctx, g_giveObserver);
        g_giveObserver = 0;
    }
    if (g_saveObserver != 0) {
        svc_save->unobserve_saves(mod_ctx, g_saveObserver);
        g_saveObserver = 0;
    }
    g_initialized = false;
}

}  // namespace cave
