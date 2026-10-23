// HD Features / Cave of Shadows: game hooks.
//
// Hook aliases carry a "Cos" prefix (the stamps' use "Bk"/"Hds"), so every alias in the mod is unique.
// Two targets are hooked by both features: dMenu_Collect2D_c::_delete and dMenu_Collect2D_c::wait_proc
// (stamps/collect_slot.cpp, BkCollectDelete / BkCollectWaitProc). That works: HookService keeps one
// detour per target and any number of pre/post callbacks per mod (dusk/mods/svc/hook.cpp hook_install
// treats the second install of the same Hook<target> statics as known). But HookService::uninstall drops
// every callback this mod has on a target, so uninstall_all() leaves those two to the loader.
//
// Every target below is an exported symbol of Dusklight v2.0.3 (checked against the SDK import
// library's symbol list) and is reached from another translation unit, so its call sites are not
// inlined (the game is built without LTO; none of the targets carries the "inline sites" flag in the
// v2.0.3 symbol manifest). All callbacks are no-ops unless cave::enabled() (feature started, the HD
// Features "Cave of Shadows" switch on, vanilla game mode) and, for cave rules, the start stage is D_SB11.
// The hooks stay installed while the switch is off (that is what makes the switch immediate).
// Pre/post hooks only; nothing replaces a function, so other mods (dawnlight hooks daAlink) can
// keep their own pre/post/replace hooks on the same targets.

#include "stamps/prelude.hpp"  // library headers before any game header (see prelude.hpp)

#include "cave.hpp"
#include "confirm.hpp"  // HD's pre-entry screen: its own dMw_c / dMenu_save_c hooks
#include "record.hpp"   // HD's "Cave of Shadows Record" screen: its own dMw_c hooks
#include "stamps/feature.hpp"  // feature_busy: the stamp book on the Collection screen

#include <mods/svc/hook.hpp>
#include <mods/svc/log.hpp>

#include "JSystem/JParticle/JPAEmitter.h"
#include "SSystem/SComponent/c_lib.h"
#include "Z2AudioLib/Z2SceneMgr.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_alldie.h"
#include "d/actor/d_a_midna.h"
#include "d/actor/d_a_npc.h"
#include "d/actor/d_a_npc_fairy_seirei.h"
#include "d/actor/d_a_obj_bosswarp.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_swc00.h"
#include "d/actor/d_a_tag_event.h"
#include "d/d_cc_uty.h"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_menu_collect.h"
#include "d/d_menu_window.h"
#include "d/d_msg_flow.h"
#include "d/d_msg_object.h"
#include "d/d_save.h"
#include "d/d_stage.h"
#include "d/d_tresure.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

// --- BGM: tphd adds Z2SCENE_CAVE_OF_SHADOWS (Z2SceneMgr.cpp diff); we map the spot to D_SB01 ---
DEFINE_HOOK(&Z2SceneMgr::setSceneName, CosSetSceneName);
// --- Wolf form: tphd forces is_wolf in dComIfGs_Wolf_Change_Check for D_SB11 ---
DEFINE_HOOK(&dComIfGs_Wolf_Change_Check, CosWolfChangeCheck);
// --- No transforming back: tphd blocks daAlink_c::handleQuickTransform and Midna's menu ---
DEFINE_HOOK(&daAlink_c::handleQuickTransform, CosQuickTransform);
DEFINE_HOOK(&dMsgFlow_c::getEventId, CosFlowGetEventId);
// --- HD flows: Midna's cave menu (tphd: flow 0x457 = 1111), section-end and reward talks ---
DEFINE_HOOK(&dMsgFlow_c::init, CosFlowInit);
// --- HD start-room cutscenes: top-bit message values (crash guard + flow 1120 arrival message) ---
DEFINE_HOOK(&dMsgObject_c::setDemoMessage, CosSetDemoMessage);
// --- Midna's portal warp as the amiibo warp-out / Withdraw: replace its -4 stage change ---
DEFINE_HOOK(&daAlink_c::skipPortalObjWarp, CosSkipPortalObjWarp);
// --- Exits: l8warp (Obj_BossWarp) / leftover SCLS -> stored return point; game over Continue -> floor 1 ---
DEFINE_HOOK(&dStage_changeScene, CosChangeScene);
DEFINE_HOOK(&dStage_changeScene4Event, CosChangeScene4Event);
DEFINE_HOOK(&dStage_changeSceneExitId, CosChangeSceneExitId);
DEFINE_HOOK(&dStage_restartRoom, CosRestartRoom);
// --- Floor 40 end fairy (FSeirei type 4): keep it deleted (sTempBit[4] overrun, no GC flow 1122) ---
DEFINE_HOOK(&daNpc_FairySeirei_c::isDelete, CosFairySeireiIsDelete);
DEFINE_HOOK(&daNpcT_chkEvtBit, CosNpcTChkEvtBit);
// --- Colossal Wallet: 9999 at wallet size 2 when the blob flag is set ---
DEFINE_HOOK(&dSv_player_status_a_c::getRupeeMax, CosGetRupeeMax);
// --- HD next-wallet rule for the vanilla wallet grants (Agitha) ---
DEFINE_HOOK(&item_func_WALLET_LV2, CosWalletLv2);
DEFINE_HOOK(&item_func_WALLET_LV3, CosWalletLv3);
// --- Virtual Wolf Link amiibo on the pause menu's Collection screen ---
DEFINE_HOOK(&dMenu_Collect2D_c::_create, CosCollectCreate);
DEFINE_HOOK(&dMenu_Collect2D_c::_delete, CosCollectDelete);
DEFINE_HOOK(&dMenu_Collect2D_c::wait_proc, CosCollectWaitProc);
// --- HD sections: the tier ALLdie of floors 5/6, 19/20 and 40 (GC ALLdie ignores the tier byte) ---
DEFINE_HOOK(&daAlldie_c::actionTimer, CosAlldieActionTimer);
// --- HD swc00 types 10/11 (data: GC type 15 + HD type in angle.x bits 12-15): the cave timer ---
DEFINE_HOOK(&daSwc00_c::execute, CosSwc00Execute);
// --- Floor 1's arrival intro only after a new announcement (HD label 829) + label guard for unpatched data ---
DEFINE_HOOK(&daTag_Event_c::arrivalTerms, CosTagEventArrivalTerms);
// --- Defensive guard for HD data the offline patch should already have fixed ---
DEFINE_HOOK(&dTres_c::addData, CosTresAddData);
// --- Run record: enemies defeated without taking damage ("Max combo") ---
DEFINE_HOOK(&cc_at_check, CosCcAtCheck);
// --- HD exit portal (Obj_BossWarp cave branch) and Link's run into it ---
DEFINE_HOOK(&daObjBossWarp_c::Create, CosBossWarpCreate);
DEFINE_HOOK(&daObjBossWarp_c::execute, CosBossWarpExecute);
DEFINE_HOOK(&daObjBossWarp_c::demoProc, CosBossWarpDemoProc);
DEFINE_HOOK(&daAlink_c::setDemoData, CosAlinkSetDemoData);
// --- HD cave controller's life / rupees in saves made inside the cave ---
DEFINE_HOOK(&dSv_info_c::memory_to_card, CosMemoryToCard);
// --- HD Withdraw: the "Save and begin" save back in memory between the cave's stage delete and the next load ---
DEFINE_HOOK(&dStage_Delete, CosStageDelete);

namespace cave::hooks {
namespace {

constexpr std::uint16_t kSaveBitLabelCount = 822;  // dSv_event_flag_c::saveBitLabels[822]
constexpr std::uint32_t kFairySeireiClearLabel = 0x1F9;  // F_0505, used by FSeirei::isDelete
constexpr std::uint16_t kMidnaTransformEventA = 4;  // daMidna_c::execute: METAMORPHOSE (+Midna demo)
constexpr std::uint16_t kMidnaTransformEventB = 5;  // daMidna_c::execute: METAMORPHOSE
constexpr int kStartEventDefault = 0xCA;            // DEFAULT_START (dEvent_exception_c names)

bool player_is_wolf() {
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    return link != nullptr && link->checkWolf() != 0;
}

// ---------------------------------------------------------------------------------------------
// BGM (Z2SceneMgr::setSceneName, called from mDoAud_setSceneName in m_Do_audio.cpp).
// tphd: case Z2SCENE_CAVE_OF_SHADOWS { se_wave2 = room 29 ? 0x16 : room 40 ? 0x12 : 0x36;
//        bgm_wave1 = 0x45; se_wave1 = 0x51; bgm = Z2BGM_SUB_DUNGEON; }.
// We hand the GC Cave of Ordeals spot name to the original (same BGM, bgm wave 0x45, CoO env SE
// and reverb), then patch the two SE wave requests to tphd's values (twilight-enemy bank 0x36,
// Temple of Time bank 0x16 for room 29, Snowpeak bank 0x12 for room 40).
// ---------------------------------------------------------------------------------------------
char g_caveOfOrdealsSpot[] = "D_SB01";
bool g_sceneMapped = false;

HookAction pre_setSceneName(ModContext*, void* args, void*, void*) {
    g_sceneMapped = false;
    if (!cave::enabled()) {
        return HOOK_CONTINUE;
    }
    char*& spot = mods::arg_ref<char*>(args, 1);
    if (spot != nullptr && std::strcmp(spot, cave::kStageName) == 0) {
        spot = g_caveOfOrdealsSpot;
        g_sceneMapped = true;
    }
    return HOOK_CONTINUE;
}

void post_setSceneName(ModContext*, void* args, void*, void*) {
    if (!g_sceneMapped) {
        return;
    }
    g_sceneMapped = false;
    auto* mgr = mods::arg<Z2SceneMgr*>(args, 0);
    const s32 room = mods::arg<s32>(args, 2);
    if (mgr == nullptr) {
        return;
    }
    mgr->requestSeWave_1 = 0x51;
    mgr->requestSeWave_2 = room == 29 ? 0x16 : (room == 40 ? 0x12 : 0x36);
}

// ---------------------------------------------------------------------------------------------
// Wolf form. dComIfGs_Wolf_Change_Check is only called from daAlink_c::create
// (d_a_alink.cpp:4954), i.e. while the player of the new scene is being created.
// ---------------------------------------------------------------------------------------------
HookAction pre_wolfChangeCheck(ModContext*, void*, void*, void*) {
    cave::before_wolf_change_check();  // undoes the cave's forced wolf form after our exits
    return HOOK_CONTINUE;
}

void post_wolfChangeCheck(ModContext*, void*, void* retval, void*) {
    if (retval != nullptr && cave::cave_rules()) {
        *static_cast<BOOL*>(retval) = TRUE;
    }
}

// Dusklight quick-transform button (d_a_alink_dusk.cpp, called from game_combos.cpp).
HookAction pre_quickTransform(ModContext*, void*, void*, void*) {
    if (cave::cave_rules() && player_is_wolf()) {
        cave::notify_transform_blocked();
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

// Midna's call menu (flow 3001 / 0xBB9) ends with event 4 or 5 to transform
// (d_a_midna.cpp:3326-3346). Normally dMsgFlow_c::init below swaps in HD flow 1111 (whose "Transform
// into human" answer is Midna's refusal 5058, no event). This is the fallback when that did not happen
// (no graph, init hook missing): the transform result becomes "no event" for Midna's own flow.
void post_getEventId(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr) {
        return;
    }
    auto* flow = mods::arg<dMsgFlow_c*>(args, 0);
    daMidna_c* midna = daPy_py_c::getMidnaActor();
    if (midna == nullptr || flow != &midna->mMsgFlow || !cave::cave_rules() || !player_is_wolf()) {
        return;
    }
    u16& eventId = *static_cast<u16*>(retval);
    if (eventId == kMidnaTransformEventA || eventId == kMidnaTransformEventB) {
        eventId = 0;
        cave::notify_transform_blocked();
    }
}

// void dMsgFlow_c::init(fopAc_ac_c* partner, int flowID, int param_2, fopAc_ac_c** talkPartners).
// Only a fresh start (param_2 == 0) chooses a flow; getInitNodeIndex passes custom ids (>= 0x8000)
// straight through (d_msg_flow.cpp:451-455).
HookAction pre_flowInit(ModContext*, void* args, void*, void*) {
    if (mods::arg<int>(args, 3) != 0 || !cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    int& flowId = mods::arg_ref<int>(args, 2);
    const int mapped = cave::remap_flow(mods::arg<fopAc_ac_c*>(args, 1), flowId);
    if (mapped != flowId) {
        mods::log::debug("D_SB11 flow {} -> custom node 0x{:04X}", flowId, mapped);
        flowId = mapped;
    }
    return HOOK_CONTINUE;
}

// static void dMsgObject_c::setDemoMessage(u32): JStudio message paragraphs of cutscenes
// (d_demo.cpp jstudio_tAdaptor_message::adaptor_do_MESSAGE).
HookAction pre_setDemoMessage(ModContext*, void* args, void*, void*) {
    if (!cave::initialized()) {
        return HOOK_CONTINUE;
    }
    return cave::on_demo_message(mods::arg<u32>(args, 0)) ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

// virtual void daAlink_c::skipPortalObjWarp(): end of Midna's portal warp-out (procCoWarp).
HookAction pre_skipPortalObjWarp(ModContext*, void* args, void*, void*) {
    return cave::on_portal_warp_end(mods::arg<daAlink_c*>(args, 0)) ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// Exits. Inside D_SB11 every scene exit (rest-floor / final l8warp portals via daAlink_c exit processing,
// leftover GC SCLS entries) goes to the stored return point (cave.cpp exit_to_return_point) instead of a room SCLS target.
// Game over -> "Continue" (game-over status 2, set by the game-over save menu's end status 1) restarts
// the run on floor 1 instead, as in HD (daAlink_c::procDead cave branch 0x020b9f14, VERIFIED): GC
// procCoDead reaches dStage_restartRoom (startRestartRoom) or, for a death exit, dStage_changeScene.
// ---------------------------------------------------------------------------------------------
bool game_over_continue() {
    return dComIfGp_getGameoverStatus() == 2;
}

// FIX (review API #3): while the mod's own exit is under way (HD's Withdraw, "Leave the cave", the portal's fallback, the
// Record screen's return) another exit must not change the pending stage change: it would land the player at the
// stored entry point while HD's Withdraw still puts the "Save and begin" save back, or (a void-out restart) send the
// stage change back to D_SB11 and lose the Withdraw. Only reachable when Link can still move during the fade (the
// Withdraw's no-warp fallback).
bool exit_under_way(const char* what, int id) {
    if (cave::phase() != cave::Phase::Exiting) {
        return false;
    }
    mods::log::info("D_SB11 {} {} ignored: the cave is already being left", what, id);
    return true;
}

// int dStage_changeScene(int exitId, f32 speed, u32 mode, s8 roomNo, s16 angle, int layer)
HookAction pre_changeScene(ModContext*, void* args, void* retval, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    if (game_over_continue()) {
        mods::log::info("D_SB11 game-over exit {} -> Continue on floor 1", mods::arg<int>(args, 0));
        cave::continue_after_death();
    } else if (exit_under_way("exit", mods::arg<int>(args, 0))) {
        // (the pending stage change stays)
    } else {
        const f32 speed = mods::arg<f32>(args, 1);
        const u32 mode = mods::arg<u32>(args, 2);
        const s16 angle = mods::arg<s16>(args, 4);
        mods::log::info("D_SB11 exit {} (room {}) redirected", mods::arg<int>(args, 0),
            static_cast<int>(mods::arg<s8>(args, 3)));
        cave::exit_to_return_point(mode, kStartEventDefault, speed, angle, cave::ExitCause::Portal);
    }
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 1;
    }
    return HOOK_SKIP_ORIGINAL;
}

// int dStage_changeScene4Event(int exitId, s8 roomNo, int wipe, bool, f32 speed, u32 mode,
//                              s16 angle, int)
HookAction pre_changeScene4Event(ModContext*, void* args, void* retval, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    if (game_over_continue()) {
        mods::log::info("D_SB11 game-over event exit {} -> Continue on floor 1", mods::arg<int>(args, 0));
        cave::continue_after_death();
    } else if (exit_under_way("event exit", mods::arg<int>(args, 0))) {
        // (the pending stage change stays)
    } else {
        const f32 speed = mods::arg<f32>(args, 4);
        const u32 mode = mods::arg<u32>(args, 5);
        const s16 angle = mods::arg<s16>(args, 6);
        mods::log::info("D_SB11 event exit {} redirected", mods::arg<int>(args, 0));
        cave::exit_to_return_point(mode, kStartEventDefault, speed, angle, cave::ExitCause::Portal);
    }
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 1;
    }
    return HOOK_SKIP_ORIGINAL;
}

// int dStage_changeSceneExitId(cBgS_PolyInfo&, f32 speed, u32 mode, s8 roomNo, s16 angle):
// only redirect when the polygon really had an exit (the original returned non-zero).
// FIX (API review): dStage_changeSceneExitId calls dStage_changeScene, whose pre-hook already redirected
// (phase Exiting); this post-hook is only the fallback for when that hook could not be installed.
void post_changeSceneExitId(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr || *static_cast<int*>(retval) == 0 || !cave::cave_rules() ||
        cave::phase() == cave::Phase::Exiting)
    {
        return;
    }
    if (game_over_continue()) {
        mods::log::info("D_SB11 game-over polygon exit -> Continue on floor 1");
        cave::continue_after_death();
        return;
    }
    mods::log::info("D_SB11 polygon exit redirected");
    cave::exit_to_return_point(mods::arg<u32>(args, 2), kStartEventDefault, mods::arg<f32>(args, 1),
        mods::arg<s16>(args, 4), cave::ExitCause::Portal);
}

// void dStage_restartRoom(u32 roomParam, u32 mode, int): game over -> "Continue" restarts the
// current stage at the restart room (daAlink_c::procCoDead -> startRestartRoom). In GC that would put
// the player back on the floor they died on with 3 hearts; HD restarts the run on floor 1 with full life
// (cave.cpp continue_after_death). Void-outs (no game over) keep the vanilla floor restart, as in HD
// (its void-out restart keeps the cave controller).
HookAction pre_restartRoom(ModContext*, void* args, void*, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    if (!game_over_continue()) {
        // A void-out restart while the mod's exit fades out would replace its stage change with D_SB11 (review API #3).
        return exit_under_way("void-out restart, room param", static_cast<int>(mods::arg<u32>(args, 0)))
                   ? HOOK_SKIP_ORIGINAL
                   : HOOK_CONTINUE;
    }
    mods::log::info("D_SB11 game-over continue -> floor 1");
    cave::continue_after_death();
    return HOOK_SKIP_ORIGINAL;
}

// ---------------------------------------------------------------------------------------------
// Floor 40 fairy. GC isDelete keeps FSeirei only once F_0505 (Cave of Ordeals B50 cleared) is
// set; type 4 then overruns sTempBit[4] in _to_FairyCave and its HD flow 1122 is missing from GC
// text. tphd special-cases type 4; we keep it deleted in D_SB11 (the floor-40 gate is opened by
// ALLdie in the patched data; the reward talk with her lines runs on floor 41, hdtext.cpp).
// ---------------------------------------------------------------------------------------------
void post_fairySeireiIsDelete(ModContext*, void*, void* retval, void*) {
    if (retval != nullptr && cave::cave_rules()) {
        *static_cast<int*>(retval) = 1;
    }
}

// Backup in case isDelete is inlined into its own create(): answer F_0505 "not set" in D_SB11.
HookAction pre_npcTChkEvtBit(ModContext*, void* args, void* retval, void*) {
    if (retval != nullptr && mods::arg<u32>(args, 0) == kFairySeireiClearLabel && cave::cave_rules()) {
        *static_cast<BOOL*>(retval) = FALSE;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// Colossal Wallet. Dusklight keeps wallet sizes 0..2 (getRupeeMax returns 0 for >= 3,
// d_save.cpp:119-142, with biggerWallets 500/1000/2000 instead of 300/600/1000). The save keeps
// size 2 (Giant); the colossal state lives only in the blob, so the .gci stays vanilla.
// ---------------------------------------------------------------------------------------------
void post_getRupeeMax(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr || !cave::colossal_wallet_active()) {
        return;
    }
    const auto* status = mods::arg<const dSv_player_status_a_c*>(args, 0);
    if (status != nullptr && status->getWalletSize() == 2) {
        *static_cast<u16*>(retval) = 9999;
    }
}

// item_func_WALLET_LV2/LV3 (d_item.cpp, reached through item_func_ptr[] from execItemGet).
HookAction pre_walletLv2(ModContext*, void*, void*, void*) {
    return cave::wallet_lv2_override() ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

HookAction pre_walletLv3(ModContext*, void*, void*, void*) {
    return cave::wallet_lv3_override() ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// Virtual Wolf Link amiibo. The Collection screen (dMenu_Collect2D_c) never reads Z, X or Y in its wait
// state (d_menu_collect.cpp wait_proc / cursorMove read only A and the stick; the menu window's
// collect_move_proc only reacts to Start/B), so any of the three is free there ("cave-amiibo_button" option).
// The scan screen (amiibo.cpp) then decides; a successful scan opens HD's "Cave of Shadows" screen
// (confirm.cpp), whose "Save and begin" runs the game's own save before the trip (cave.cpp tick_entry).
// ---------------------------------------------------------------------------------------------
void post_collectCreate(ModContext*, void*, void*, void*) {
    if (cave::enabled()) {
        cave::collect_screen_opened();
    }
}

void post_collectDelete(ModContext*, void*, void*, void*) {
    cave::collect_screen_closed();
}

void post_collectWaitProc(ModContext*, void* args, void*, void*) {
    auto* collect = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (collect == nullptr || !cave::enabled()) {
        return;
    }
    if (hdstamps::feature_busy()) {
        // The stamps' pre-hook opened the stamp book this frame (A on the stamp slot) and skipped the
        // original; the book now owns the screen, so no amiibo scan may open over it (review "api" #3).
        return;
    }
    // Same conditions the screen itself requires before reacting to A / closing on B: idle (no sub-window
    // opening, no key lock), no fade running, no equipment-change animation.
    if (collect->mProcess != 0 || collect->field_0x25f != 0 || collect->getSubWindowOpenCheck() != 0) {
        return;
    }
    cave::collect_screen_idle();  // amiibo prompt visibility, menu-closing B retry (cave.cpp)
    static constexpr u32 kButtons[] = {PAD_TRIGGER_Z, PAD_BUTTON_X, PAD_BUTTON_Y};
    if ((mDoCPd_c::getTrig(PAD_1) & kButtons[cave::amiibo_button_choice()]) == 0) {
        return;
    }
    if (mDoGph_gInf_c::getFader() == nullptr || mDoGph_gInf_c::getFader()->getStatus() != 1) {
        return;
    }
    daPy_py_c* link = daPy_getLinkPlayerActorClass();
    if (link == nullptr || link->getSwordChangeWaitTimer() != 0 || link->getShieldChangeWaitTimer() != 0 ||
        link->getClothesChangeWaitTimer() != 0)
    {
        return;
    }
    cave::amiibo_button(collect);
}

// ---------------------------------------------------------------------------------------------
// HD sections (research/cos_sections_spec.md §3). Every visit starts on floor 1; what the story has
// unlocked only changes which switch the "tier" ALLdie of floors 5/6 (tier 1), 19/20 (tier 2) and 40
// (tier 3) sets. The ALLdie parameter is ev<<24 | alt<<16 | swbit<<8 | tier (R10 ffc28a01, R25 ffc1c301,
// R48 ffc2b002, R34 ffc1c302, R49 ffffb103); GC's d_a_alldie.cpp ignores the tier byte and always sets
// swbit, which is HD's "unlocked" branch. int daAlldie_c::actionTimer(): a copy of GC's actionTimer
// (d_a_alldie.cpp) whose switch comes from cave::alldie_tier_switch, i.e. HD's 0x020d3fd4 (VERIFIED: same
// enemy check, 65-frame timer and event order as GC; only the switch choice and the tier record differ).
// Skips the original only for tier bytes 1..3 in D_SB11; everything else runs GC's own code.
// Exported, manifest flags 0x09 (no inline sites): execute() calls it through the hooked entry point.
// ---------------------------------------------------------------------------------------------
HookAction pre_alldieActionTimer(ModContext*, void* args, void* retval, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    auto* alldie = mods::arg<daAlldie_c*>(args, 0);
    if (alldie == nullptr) {
        return HOOK_CONTINUE;
    }
    const u32 prm = fopAcM_GetParam(alldie);
    const u32 tier = prm & 0xFF;
    if (tier < 1 || tier > 3) {
        return HOOK_CONTINUE;
    }
    const s8 room = fopAcM_GetRoomNo(alldie);
    if (fopAcM_myRoomSearchEnemy(room) != nullptr) {
        alldie->mAction = daAlldie_c::ACT_CHECK;
    } else if (alldie->mTimer > 0) {
        alldie->mTimer--;
    } else {
        alldie->mAction = alldie->mEventIdx == -1 ? daAlldie_c::ACT_WAIT : daAlldie_c::ACT_ORDER;
        dComIfGs_onSwitch(cave::alldie_tier_switch(prm, room), room);
    }
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 1;
    }
    return HOOK_SKIP_ORIGINAL;
}

// HD swc00 types 10 and 11 (HD handlers 0x02891d98 / 0x02891df4, VERIFIED): "sw2 on (or FF) -> set sw1
// on entering the area, then delete", type 10 also starting HD's hidden cave timer, type 11 stopping it.
// GC has no types 10/11 (its execute asserts on them), so cave_patch.py --swc stores them as GC type 15
// (the same switch rule and self-delete) with the HD type in angle.x bits 12-15, which GC's d_a_swc00.cpp
// does not read (type = bits 8-11, condition = bits 0-7). int daSwc00_c::execute() post: type 15 sets field_0x584
// on the frame the player is inside; the timer state change is idempotent while the area waits to be
// deleted (actionDead, next frame).
void post_swc00Execute(ModContext*, void* args, void*, void*) {
    if (!cave::cave_rules()) {
        return;
    }
    auto* area = mods::arg<daSwc00_c*>(args, 0);
    if (area == nullptr || area->field_0x584 == 0) {
        return;
    }
    const int hdType = (static_cast<u16>(area->shape_angle.x) >> 12) & 0xF;
    if (hdType == 10 || hdType == 11) {
        cave::on_hd_switch_area(hdType);
    }
}

// ---------------------------------------------------------------------------------------------
// daTag_Event_c::arrivalTerms.
// Pre (defensive): HD event labels >= 822 (R19 TagEv valid label 829) index past saveBitLabels[822].
// cave_patch.py rewrites that label; if an unpatched archive is used, neutralise it the same way
// (invalid label -> none, valid label -> none) before the original reads it.
// Post: HD label 829 ("arrival intro pending", set by the entry bookkeeping only when the entry announces
// something new, cleared by flow 1120): floor 1's arrival cutscene plays then, and never again after a
// void-out reloads the room. Floors 7/21's camera-only intros keep GC's result (cave.cpp).
// ---------------------------------------------------------------------------------------------
HookAction pre_tagEventArrivalTerms(ModContext*, void* args, void*, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    auto* tag = mods::arg<daTag_Event_c*>(args, 0);
    if (tag == nullptr) {
        return HOOK_CONTINUE;
    }
    const std::uint16_t invalidLabel = static_cast<std::uint16_t>(tag->home.angle.x) & 0x7FFF;
    if (invalidLabel != 0x7FFF && invalidLabel >= kSaveBitLabelCount) {
        tag->home.angle.x = static_cast<s16>((static_cast<std::uint16_t>(tag->home.angle.x) & 0x8000) | 0x7FFF);
    }
    const std::uint16_t validLabel = static_cast<std::uint16_t>(tag->home.angle.z);
    if (validLabel != 0xFFFF && validLabel >= kSaveBitLabelCount) {
        tag->home.angle.z = static_cast<s16>(-1);
    }
    return HOOK_CONTINUE;
}

void post_tagEventArrivalTerms(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr || *static_cast<BOOL*>(retval) == FALSE) {
        return;
    }
    if (!cave::intro_tag_allowed(mods::arg<daTag_Event_c*>(args, 0))) {
        *static_cast<BOOL*>(retval) = FALSE;
    }
}

// Defensive: dTres_c::addData writes mTypeGroupListAll[getTypeToTypeGroupNo(type)] (17 entries)
// and mTypeGroupData[mNum] (0x40 entries) without bounds checks; tphd adds both guards. Skip a
// room's minimap data in D_SB11 if it would overrun either (cosmetic loss only).
HookAction pre_tresAddData(ModContext*, void* args, void*, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    auto* list = mods::arg<dTres_c::list_class*>(args, 0);
    if (list == nullptr) {
        return HOOK_CONTINUE;
    }
    const int count = list->field_0x0;
    if (count <= 0) {
        return HOOK_CONTINUE;
    }
    if (count > 0x40 || static_cast<int>(dTres_c::mNum) + count > 0x40) {
        mods::log::warn("D_SB11 minimap data skipped (would exceed 64 entries)");
        return HOOK_SKIP_ORIGINAL;
    }
    const dTres_c::data_s* data = list->field_0x4->getDataPointer();
    if (data == nullptr) {
        return HOOK_CONTINUE;
    }
    for (int i = 0; i < count; ++i) {
        if (dTres_c::getTypeToTypeGroupNo(data[i].mType) >= dTres_c::TYPE_GROUP_ENUM_NUMBER) {
            mods::log::warn("D_SB11 minimap data skipped (unknown type 0x{:02X})", data[i].mType);
            return HOOK_SKIP_ORIGINAL;
        }
    }
    return HOOK_CONTINUE;
}

// fopAc_ac_c* cc_at_check(fopAc_ac_c* enemy, dCcU_AtInfo* info): an enemy took a hit; mHitStatus 2 =
// that hit brought its health to 0 (d_cc_uty.cpp, the same place Dusklight's "enemy_killed"
// achievement signal comes from).
void post_ccAtCheck(ModContext*, void* args, void*, void*) {
    if (!cave::cave_rules()) {
        return;
    }
    auto* info = mods::arg<dCcU_AtInfo*>(args, 1);
    cave::on_enemy_hit(mods::arg<fopAc_ac_c*>(args, 0), info != nullptr && info->mHitStatus == 2);
}

// ---------------------------------------------------------------------------------------------
// HD exit portal (l8warp = Obj_BossWarp on floors 6/20/41; research/cos_portal_spec.md §3, HD Zelda.rpx
// 0x026d1e08-0x026d3f74). HD's daObjBossWarp_c has a cave branch in front of GC's code, exclusive in execute:
//  - Create (0x026d2de0): BOSS_WARPIN_M, action 0, no set_appear() (hidden until its switch is on);
//  - execute (0x026d284c): switch on -> ACT_WAIT_WARP, set_appear() every frame (the 0x8548 burst kept, so it
//    plays once), Midna waits at the portal (onTagWaitPos), event_proc_call (WARP_CHECK, flow 3008, Yes ->
//    BOSS_WARPIN_M, No -> WARP_CANCEL); switch off -> disappear, Midna released; never onSceneChangeArea;
//  - demoProc: WALK_TARGET1 makes Link run (Link+0x3968 = 1.0, 0x026d3684); SCENE_CHG (0x026d333c) does the
//    cave bookkeeping instead of a stage change and, on its first frame (0x026d3790), hides Midna and hands
//    over to the "Cave of Shadows Record" screen (cave.cpp, record.cpp);
//  - daAlink_c::setDemoData (0x02062a40): while Link+0x3968 >= 0 it replaces the event's "stick" (0.1).
// Hooks A-D are pre (skip) on Create / execute, pre on demoProc (skips only SCENE_CHG frames), post on
// setDemoData. All exported (SDK import library), no inline sites (execute is a separate function from
// daObjBossWarp_Execute). Fallbacks when one is missing (spec 3.1): no C -> GC SCENE_CHG -> onSceneChangeArea
// -> the dStage_changeScene redirect above (the old fade); no B -> today's fade; no A -> BOSS_WARPIN without
// the lead-in, still hidden until the switch by hook B. Failures are recorded as Feature::PortalCutscene.
// ---------------------------------------------------------------------------------------------

// The event manager keeps the archive name pointer (d_event.cpp:420/511, d_event_manager.cpp:936/1304): it must
// outlive the actor (GC uses its TU-static l_arcName). FIX (API review #3): a portal actor can outlive this DLL
// (the mod disabled while its room is loaded: the DLL is freed a loader tick later, the actor at the next stage
// change), so the name is not a DLL global but a copy in the process heap (the UCRT's malloc heap is
// GetProcessHeap(); unloading the DLL does not free it). 10 bytes per load of the mod, never freed on purpose.
char g_portalArcFallback[] = "ef_Portal";

char* portal_arc_name() {
    static char* const name = [] {
        auto* copy = static_cast<char*>(std::malloc(sizeof(g_portalArcFallback)));
        if (copy != nullptr) {
            std::memcpy(copy, g_portalArcFallback, sizeof(g_portalArcFallback));
        }
        return copy;
    }();
    return name != nullptr ? name : g_portalArcFallback;
}

// The Create hook installed (hook A). Without it GC's Create shows the portal at room load (set_appear), and the
// switch-off branch of hook B also closes the model again so the fallback still matches HD's "hidden until its
// switch" (API review INFO "hook A failure fallback"; GC's disappear() only removes the particles).
bool g_createHookOk = false;

// int daObjBossWarp_c::Create(), cave branch (HD 0x026d2de0).
HookAction pre_bossWarpCreate(ModContext*, void* args, void* retval, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    auto* w = mods::arg<daObjBossWarp_c*>(args, 0);
    if (w == nullptr) {
        return HOOK_CONTINUE;
    }
    dEvent_manager_c& evm = dComIfGp_getEventManager();
    w->initBaseMtx();
    w->mBossClearEventId = -1;  // getNowLevel() is -1 in D_SB11
    w->mBossClearMapToolId = 0xFF;
    w->mBossWarpInEventId = evm.getEventIdx(w, "BOSS_WARPIN_M", 0xFF);  // HD 0x026d2e40
    w->mBossWarpInMapToolId = 0xFF;
    w->mAction = daObjBossWarp_c::ACT_WAIT;  // HD: no set_appear(); hidden until its switch is on
    w->field_0x595 = false;
    w->field_0x599 = 0;  // reused as "burst 0x8548 played" (only GC's ystone code uses it; no stone here)
    w->eventInfo.mArchiveName = portal_arc_name();  // after BOSS_WARPIN_M (global Event.arc), before ef_Portal's
    w->mWarpCheckEventId = evm.getEventIdx(w, "WARP_CHECK", 0xFF);
    w->mWarpCancelEventId = evm.getEventIdx(w, "WARP_CANCEL", 0xFF);
    w->mScaleF = 1.0f;
    w->execute();  // through hook B
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 1;
    }
    return HOOK_SKIP_ORIGINAL;
}

// HD set_appear (0x026d2554) + appear (0x026d2358): the portal fully open at once; the burst 0x8548 once per
// appearance (HD keeps its handle at 0x026d2410; GC's appear() clears it, so calling it every frame would spawn
// the burst every frame).
void hd_set_appear(daObjBossWarp_c* w) {
    static constexpr u16 kEff[4] = {0x8548, 0x8549, 0x854A, 0x854B};
    w->mpBrkAnm->setFrame(w->mpBrkAnm->getEndFrame());
    w->mpBtkAnm[1]->setFrame(w->mpBtkAnm[1]->getEndFrame());
    w->mParticlePos = w->current.pos;
    w->mParticlePos.y += 2.0f;
    if (w->field_0x599 == 0) {
        // Not keeping the pointer: a finished one-shot emitter is recycled by JPA.
        dComIfGp_particle_set(kEff[0], &w->mParticlePos, nullptr, nullptr);
        w->field_0x599 = 1;
    }
    for (int i = 1; i < 4; ++i) {
        if (w->mpParticle[i] == nullptr) {
            w->mpParticle[i] = dComIfGp_particle_set(kEff[i], &w->mParticlePos, nullptr, i == 3 ? &w->scale : nullptr);
        }
    }
    w->mpBrkAnm->setPlaySpeed(1.0f);
    w->mpBtkAnm[0]->setPlaySpeed(1.0f);
    w->mpBtkAnm[1]->setPlaySpeed(1.0f);
}

// int daObjBossWarp_c::execute(), HD cave branch + GC's common tail (HD 0x026d284c / 0x026d2c6c-0x026d2d44).
HookAction pre_bossWarpExecute(ModContext*, void* args, void* retval, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    auto* w = mods::arg<daObjBossWarp_c*>(args, 0);
    if (w == nullptr || w->mpBrkAnm == nullptr || w->mpBtkAnm[0] == nullptr || w->mpBtkAnm[1] == nullptr) {
        return HOOK_CONTINUE;
    }
    daMidna_c* midna = daPy_py_c::getMidnaActor();
    const u8 sw = w->getSwNo();
    if (sw == 0xFF || fopAcM_isSwitch(w, sw)) {
        if (w->mAction == daObjBossWarp_c::ACT_WAIT) {
            w->setAction(daObjBossWarp_c::ACT_WAIT_WARP);
        }
        hd_set_appear(w);
        if (!dComIfGp_evmng_checkStartDemo()) {
            w->field_0x595 = true;
            w->field_0x620 = w->current.pos;
        }
        // DEVIATION (defensive only): actionOrderChkEvent dereferences Midna without a check.
        if (midna != nullptr || w->mAction != daObjBossWarp_c::ACT_WAIT_WARP) {
            w->event_proc_call();
        }
        if (w->field_0x595 && midna != nullptr && w->mAction != daObjBossWarp_c::ACT_DEAD) {
            midna->onTagWaitPos(&w->field_0x620);  // HD: Midna +0x898 |= 0x400, +0x89C &= ~0x2000, +0x9B4 = pos
        }
    } else if (w->mAction == daObjBossWarp_c::ACT_WAIT_WARP) {
        w->disappear(0);
        w->field_0x599 = 0;
        if (!g_createHookOk) {
            // Hook A missing: GC's Create opened the model at room load (set_appear). HD's cave Create never does;
            // close it again (CreateHeap's state: brk/btk at frame 0, stopped) so it stays hidden until its switch.
            w->mpBrkAnm->setFrame(0.0f);
            w->mpBrkAnm->setPlaySpeed(0.0f);
            w->mpBtkAnm[0]->setPlaySpeed(0.0f);
            w->mpBtkAnm[1]->setFrame(0.0f);
            w->mpBtkAnm[1]->setPlaySpeed(0.0f);
        }
        w->setAction(daObjBossWarp_c::ACT_WAIT);
        if (w->field_0x595 && midna != nullptr) {
            midna->offTagWaitPos();  // HD 0x0247c50c (INFERRED to be offTagWaitPos)
        }
    }
    // Common tail = GC execute() / HD 0x026d2c6c-0x026d2d44.
    w->mpBrkAnm->play();
    w->mpBtkAnm[0]->play();
    w->mpBtkAnm[1]->play();
    if (w->mScalingUp) {
        cLib_chaseF(&w->scale.y, 1.0f, 0.016f);
    }
    if (w->mpParticle[3] != nullptr) {
        JGeometry::TVec3<f32> s;
        JGeometry::setTVec3f(&w->scale.x, &s.x);
        w->mpParticle[3]->setGlobalScale(s);
    }
    if (w->mpBrkAnm->getFrame() != 0.0f) {
        mDoAud_seStartLevel(Z2SE_OBJ_MDN_ESCAPE_HOLE, &w->current.pos, 0, 0);
    }
    w->setBaseMtx();
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 1;
    }
    return HOOK_SKIP_ORIGINAL;
}

// int daObjBossWarp_c::demoProc() during BOSS_WARPIN_M (HD WALK_TARGET1 0x026d366c, SCENE_CHG 0x026d333c /
// 0x026d3790). Must be the same 15 names in the same order as GC's table: getMyActIdx caches the index per staff.
HookAction pre_bossWarpDemoProc(ModContext*, void* args, void* retval, void*) {
    if (!cave::cave_rules()) {
        return HOOK_CONTINUE;
    }
    auto* w = mods::arg<daObjBossWarp_c*>(args, 0);
    if (w == nullptr) {
        return HOOK_CONTINUE;
    }
    if (w->mAction == daObjBossWarp_c::ACT_DEAD) {
        // SCENE_CHG ran and the event was removed: nothing of the warp event may run again.
        if (retval != nullptr) {
            *static_cast<int*>(retval) = 1;
        }
        return HOOK_SKIP_ORIGINAL;
    }
    if (w->mAction != daObjBossWarp_c::ACT_WARP_EVENT) {
        return HOOK_CONTINUE;  // WARP_CHECK / WARP_CANCEL: GC's own cuts (WCHECK flow 3008, SETPOS)
    }
    static DUSK_CONST char* kActs[15] = {"WAIT", "APPEAR", "DISAPPEAR", "SCENE_CHG", "STONE_FALL", "STONE_MIDNA",
        "WALK_TARGET1", "APPEAR_END", "STONE_DELETE", "STONE_PUTAWAY", "WCHECK", "SETPOS", "SCALING",
        "STONE_SCALE", "HEART_MOVE"};
    const int act = dComIfGp_evmng_getMyActIdx(w->mStaffId, kActs, 15, 0, 0);
    const bool advance = dComIfGp_evmng_getIsAddvance(w->mStaffId) != 0;
    if (act == 6 && advance) {
        cave::set_portal_dash(true);  // HD Link+0x3968 = 1.0; the original still sets the goal
        mods::log::info("portal BOSS_WARPIN_M: Link runs into the portal");
    }
    if (act != 3) {
        return HOOK_CONTINUE;
    }
    if (advance) {
        cave::on_portal_scene_chg(w);  // HD bookkeeping (also Link+0x3968 = -1.0)
        w->mCounter = 0;
    }
    if (w->mCounter++ == 0) {
        w->setAction(daObjBossWarp_c::ACT_DEAD);  // HD's later SCENE_CHG frames do nothing either
        cave::on_portal_scene_chg_first_frame(w);
    }
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 1;
    }
    return HOOK_SKIP_ORIGINAL;  // GC's SCENE_CHG would call onSceneChangeArea
}

// void daAlink_c::setDemoData(): the event's "stick" is set every frame (d_a_alink_demo.inc:738-743); HD uses
// 1.0 instead while Link+0x3968 >= 0 (WALK_TARGET1 .. SCENE_CHG), so wolf Link runs into the portal.
void post_alinkSetDemoData(ModContext*, void* args, void*, void*) {
    if (!cave::portal_dash_active()) {
        return;
    }
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (link != nullptr && link->mDemo.getDemoType() == daPy_demo_c::DEMO_TYPE_SYSTEM_e) {
        link->mDemo.setStick(1.0f);
    }
}

// int dSv_info_c::memory_to_card(char*, int): HD's cave controller puts its life / rupees into a save made in
// the cave (0x02aa8f98-0x02aa8fc4) and restores the real ones afterwards; the mod also stores the entry form
// there (cave.cpp before_memory_to_card).
HookAction pre_memoryToCard(ModContext*, void*, void*, void*) {
    cave::before_memory_to_card();
    return HOOK_CONTINUE;
}

void post_memoryToCard(ModContext*, void*, void*, void*) {
    cave::after_memory_to_card();
}

// void dStage_Delete() (d_stage.cpp:2898, called from dScnPly_Delete): it stores the leaving stage's memory into the
// save (putSave) and frees the stage; the next play scene's dStage_stagInfoInit then loads its own stage memory from
// the save (d_stage.cpp:2014-2026). Between the two, HD's Withdraw (cave.cpp) puts the "Save and begin" save back, as
// the title screen's reload would. Exported, manifest flags 0x09 (no inline sites).
void post_stageDelete(ModContext*, void*, void*, void*) {
    cave::on_stage_deleted();
}

// ---------------------------------------------------------------------------------------------
// Installation.
// ---------------------------------------------------------------------------------------------
HookOptions make_options(int priority) {
    HookOptions o = HOOK_OPTIONS_INIT;
    o.priority = priority;
    return o;
}

bool install_ok(ModResult result, const char* what, cave::Feature feature = cave::Feature::None) {
    if (result == MOD_OK) {
        return true;
    }
    mods::log::warn("hook '{}' not installed (result {})", what, static_cast<int>(result));
    cave::record_hook_failure(what, feature);
    return false;
}

template <class Entry>
bool install_pre(const char* what, HookPreFn fn, int priority = 0, cave::Feature feature = cave::Feature::None) {
    const HookOptions o = make_options(priority);
    return install_ok(mods::hook::add_pre<Entry>(fn, &o), what, feature);
}

template <class Entry>
void install_post(const char* what, HookPostFn fn, int priority = 0, cave::Feature feature = cave::Feature::None) {
    const HookOptions o = make_options(priority);
    install_ok(mods::hook::add_post<Entry>(fn, &o), what, feature);
}

template <class Entry>
void install_pre_post(const char* what, HookPreFn pre, HookPostFn post) {
    const HookOptions o = make_options(0);
    if (install_ok(mods::hook::add_pre<Entry>(pre, &o), what)) {
        install_ok(mods::hook::add_post<Entry>(post, &o), what);
    }
}

}  // namespace

void install_all() {
    install_pre_post<CosSetSceneName>("cave music", pre_setSceneName, post_setSceneName);
    install_pre_post<CosWolfChangeCheck>("wolf form on entry", pre_wolfChangeCheck, post_wolfChangeCheck);
    // High priority so the block runs before other mods' pre/replace hooks on the same target.
    install_pre<CosQuickTransform>("quick transform lock", pre_quickTransform, 1000);
    install_post<CosFlowGetEventId>("Midna transform lock", post_getEventId);
    install_pre<CosFlowInit>("HD flows (Midna menu, talks)", pre_flowInit, 1000, cave::Feature::FlowRemap);
    // Without this guard the start rooms' cutscene would read far past the message table: entering the
    // cave is refused when it is missing.
    install_pre<CosSetDemoMessage>(
        "cutscene message guard", pre_setDemoMessage, 1000, cave::Feature::DemoGuard);
    // Without it the portal warp-out is not used (direct fade instead): see cave.cpp warp_effect_enabled.
    install_pre<CosSkipPortalObjWarp>(
        "portal warp-out", pre_skipPortalObjWarp, 1000, cave::Feature::PortalWarp);
    install_pre<CosChangeScene>("portal exits", pre_changeScene, 1000);
    install_pre<CosChangeScene4Event>("event exits", pre_changeScene4Event, 1000);
    install_post<CosChangeSceneExitId>("polygon exits", post_changeSceneExitId);
    install_pre<CosRestartRoom>("game-over Continue (floor 1)", pre_restartRoom, 1000);
    install_post<CosFairySeireiIsDelete>("floor 40 fairy", post_fairySeireiIsDelete);
    install_pre<CosNpcTChkEvtBit>("floor 40 fairy (flag)", pre_npcTChkEvtBit);
    install_post<CosGetRupeeMax>("Colossal Wallet", post_getRupeeMax);
    install_pre<CosWalletLv2>("wallet upgrades (Big)", pre_walletLv2);
    install_pre<CosWalletLv3>("wallet upgrades (Giant)", pre_walletLv3);
    // If the amiibo button cannot be hooked, the Enter button is shown instead.
    install_post<CosCollectWaitProc>(
        "amiibo button (Collection screen)", post_collectWaitProc, 0, cave::Feature::AmiiboButton);
    install_post<CosCollectCreate>("amiibo prompt", post_collectCreate);
    install_post<CosCollectDelete>("amiibo prompt (close)", post_collectDelete);
    // Without it GC's ALLdie opens every section-end floor (one continuous run, no portals on 6/20).
    install_pre<CosAlldieActionTimer>(
        "HD sections (tier ALLdie)", pre_alldieActionTimer, 1000, cave::Feature::SectionTiers);
    // Without it the record time runs for the whole visit (cave.cpp start_run).
    install_post<CosSwc00Execute>("cave timer (swc00 types 10/11)", post_swc00Execute, 0, cave::Feature::CaveTimer);
    install_pre_post<CosTagEventArrivalTerms>("arrival intro / label guard", pre_tagEventArrivalTerms,
        post_tagEventArrivalTerms);
    install_pre<CosTresAddData>("minimap guard", pre_tresAddData);
    install_post<CosCcAtCheck>("run record (combo)", post_ccAtCheck);
    // HD's exit portal (cos_portal_spec.md hooks A-D): each missing one falls back to GC's direct exit.
    g_createHookOk = install_pre<CosBossWarpCreate>("portal: hidden until its switch (Create)", pre_bossWarpCreate,
        1000, cave::Feature::PortalCutscene);
    install_pre<CosBossWarpExecute>("portal: Midna's question and cutscene (execute)", pre_bossWarpExecute, 1000,
        cave::Feature::PortalCutscene);
    install_pre<CosBossWarpDemoProc>("portal: Record screen after the warp (demoProc)", pre_bossWarpDemoProc, 1000,
        cave::Feature::PortalCutscene);
    install_post<CosAlinkSetDemoData>("portal: Link runs in (setDemoData)", post_alinkSetDemoData, 0,
        cave::Feature::PortalCutscene);
    // FIX (HD review L2): the post-hook (which puts the real life / rupees / form back) first, and the swap only
    // when it is in: a swap without its restore would leave the entry values in the running game.
    {
        const HookOptions o = make_options(0);
        if (install_ok(mods::hook::add_post<CosMemoryToCard>(post_memoryToCard, &o),
                "cave life/rupees in saves (memory_to_card, restore)"))
        {
            install_ok(mods::hook::add_pre<CosMemoryToCard>(pre_memoryToCard, &o),
                "cave life/rupees in saves (memory_to_card)");
        }
    }
    // Without it HD's Withdraw restores the "Save and begin" save once its stage change's fade is at black (cave.cpp).
    install_post<CosStageDelete>("HD Withdraw (the save put back at the stage change)", post_stageDelete, 0,
        cave::Feature::WithdrawRestore);
    // HD's "Cave of Shadows" screen after the scan and the save before the trip (confirm.cpp).
    confirm::install_hooks();
    // HD's "Cave of Shadows Record" screen after an exit portal (record.cpp).
    record::install_hooks();
}

void uninstall_all() {
    // mod_shutdown only. The loader removes a mod's hooks on teardown anyway; this keeps reloads tidy.
    record::uninstall_hooks();   // leaves its shared dMw_c targets to the loader
    confirm::uninstall_hooks();  // leaves its dMw_c targets (shared with the stamp book) to the loader
    mods::hook::uninstall<CosStageDelete>();
    mods::hook::uninstall<CosMemoryToCard>();
    mods::hook::uninstall<CosAlinkSetDemoData>();
    mods::hook::uninstall<CosBossWarpDemoProc>();
    mods::hook::uninstall<CosBossWarpExecute>();
    mods::hook::uninstall<CosBossWarpCreate>();
    mods::hook::uninstall<CosCcAtCheck>();
    mods::hook::uninstall<CosTresAddData>();
    mods::hook::uninstall<CosTagEventArrivalTerms>();
    mods::hook::uninstall<CosSwc00Execute>();
    mods::hook::uninstall<CosAlldieActionTimer>();
    // Not CosCollectDelete / CosCollectWaitProc: the stamps hook the same two functions, and uninstall
    // would remove their callbacks too (see the top of this file). The loader removes them on teardown.
    mods::hook::uninstall<CosCollectCreate>();
    mods::hook::uninstall<CosWalletLv3>();
    mods::hook::uninstall<CosWalletLv2>();
    mods::hook::uninstall<CosGetRupeeMax>();
    mods::hook::uninstall<CosNpcTChkEvtBit>();
    mods::hook::uninstall<CosFairySeireiIsDelete>();
    mods::hook::uninstall<CosRestartRoom>();
    mods::hook::uninstall<CosChangeSceneExitId>();
    mods::hook::uninstall<CosChangeScene4Event>();
    mods::hook::uninstall<CosChangeScene>();
    mods::hook::uninstall<CosSkipPortalObjWarp>();
    mods::hook::uninstall<CosSetDemoMessage>();
    mods::hook::uninstall<CosFlowInit>();
    mods::hook::uninstall<CosFlowGetEventId>();
    mods::hook::uninstall<CosQuickTransform>();
    mods::hook::uninstall<CosWolfChangeCheck>();
    mods::hook::uninstall<CosSetSceneName>();
}

}  // namespace cave::hooks
