#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "book_host.hpp"

#include "book.hpp"
#include "book_assets.hpp"
#include "stamp_book.hpp"
#include "stamp_data.hpp"
#include "state.hpp"
#include "ui.hpp"

#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JUtility/JUTFader.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_menu_collect.h"
#include "d/d_menu_window.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_scrn_item.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"

#include <algorithm>

// Hook targets: all exported by dusklight.exe v2.0.3 (sdk/windows-amd64.lib) and flagged 0x09 in
// its symbol manifest (code, several names for one address, no inline sites). Pre/post hooks
// only; the pre-hooks that skip the original only do so while this mod owns the state they run in.
//   dMw_c::collect_move_proc    pre, skip while the Collection-hosted book or its fades run
//   dMw_c::_draw                post: queue the book at window status 10
//   dMw_c::_delete              pre: destroy the book before the menu heap goes away; disarm
//   dMw_c::key_wait_proc        pre + post: request / reset the automatic opening, same frame
//   dMw_c::insect_open_init, insect_open_proc, insect_open2_proc, insect_move_proc,
//   insect_close_init, insect_close_proc   pre, skip while the automatic book owns them
//   dMsgScrnItem_c::~dMsgScrnItem_c        pre: arm (HD: the item-get window destructor 0x02a7fa48)
DEFINE_HOOK(&dMw_c::collect_move_proc, BkMwCollectMoveProc);
DEFINE_HOOK(&dMw_c::_draw, BkMwDraw);
DEFINE_HOOK(&dMw_c::_delete, BkMwDelete);
DEFINE_HOOK(&dMw_c::key_wait_proc, BkMwKeyWaitProc);
DEFINE_HOOK(&dMw_c::insect_open_init, BkMwInsectOpenInit);
DEFINE_HOOK(&dMw_c::insect_open_proc, BkMwInsectOpenProc);
DEFINE_HOOK(&dMw_c::insect_open2_proc, BkMwInsectOpen2Proc);
DEFINE_HOOK(&dMw_c::insect_move_proc, BkMwInsectMoveProc);
DEFINE_HOOK(&dMw_c::insect_close_init, BkMwInsectCloseInit);
DEFINE_HOOK(&dMw_c::insect_close_proc, BkMwInsectCloseProc);
// A destructor's address cannot be taken in C++, so it is named by symbol. MSVC's complete-object
// destructor (??1) takes only `this`; the deleting destructor in the vtable (??_G at another
// address) calls it, and the manifest lists no inline sites for ??1.
DEFINE_HOOK_SYMBOL("??1dMsgScrnItem_c@@UEAA@XZ", void(dMsgScrnItem_c*), BkMsgScrnItemDtor);

namespace hdstamps::book::host {
namespace {

// 2D heap the book may need. Estimate from the four layouts and the x64 object sizes (MSVC
// sizeof probes): 34 J2DPane x 296 B, 107 J2DPictureEx x 488 B, 19 J2DTextBoxEx x 384 B, 126
// J2DMaterial x 208 B plus their TEV blocks (120-224 B), 107 JUTTexture x 120 B, the select cursor
// with its own layout and animations, ExpHeap block headers: about 160 KB. An ExpHeap allocation
// failure aborts the game on PC (JKRExpHeap::alloc), so the book refuses to open below this
// instead of running the heap dry. After the first opening the measured use (logged, and shown
// in the mods panel) plus a margin is required instead.
constexpr s32 kFirstOpenFreeHeap = 0x40000;
constexpr s32 kHeapMargin = 0x10000;
constexpr s32 kMinHeapFloor = 0x30000;
// The largest single allocation is the menu layout's material array (99 x 208 B).
constexpr s32 kMinFreeBlock = 0x8000;
// Safety net for fader waits (dMw_fade_out is 13 frames).
constexpr int kFadeTimeoutFrames = 60;
// HD's arming rule (Zelda.rpx 0x02a7fa48: cmpwi r24,1 / count >= 49).
constexpr int kFirstStampCount = 1;
constexpr int kRewardStampCount = kStampCount - 1;
// dMenu_Collect2D_c::mSubWindowOpenCheck while the stamp book opens (HD uses 5 in its own
// numbering; GC uses 1-6, so 7 is free). collect_move_proc never sees it: its pre-hook skips the
// original from that frame on, and the Collection screen is deleted at black.
constexpr u8 kSubWindowStamps = 7;

enum class Phase : uint8_t {
    Idle,
    OpenRequested,  // A on the slot this frame (HD: sub-window 5 set in wait_proc)
    FadingOut,      // HD collect_close_init: SUB_IN + fade out
    Open,           // HD 0x21 proc / 0x22 STAMP_MOVE
    FadingOutClose, // HD 0x23 STAMP_CLOSE init: SUB_OUT + fade out (no vibration)
};

StampBook g_book;
Phase g_phase = Phase::Idle;
int g_phaseFrames = 0;
dMenu_Collect2D_c* g_requestScreen = nullptr; // the Collection screen the request came from

// Automatic opening (modes 2/3).
BookMode g_armedMode = BookMode::Collection; // Collection = not armed
bool g_requestThisFrame = false;
bool g_direct = false;
bool g_directFailed = false;
BookMode g_directMode = BookMode::FirstStamp;
int g_directFrames = 0;

int g_installed = 0;
int g_wanted = 0;
bool g_hooksOk = false;
bool g_autoHooksOk = false;

bool armed() {
    return g_armedMode == BookMode::FirstStamp || g_armedMode == BookMode::Reward;
}

void se(u32 id) {
    mDoAud_seStart(id, nullptr, 0, 0);
}

bool fader_black() {
    JUTFader* fader = mDoGph_gInf_c::getFader();
    return fader == nullptr || fader->getStatus() == JUTFader::None;
}

// The Twili Midna reward is presented by the book (mode 3); when that cannot happen after all, the
// player still gets HD's 2806 text once, whatever the toasts option says.
void twili_reward_fallback_notice() {
    mods::log::info("stamp book: the Twili Midna sequence could not run; showing its notice instead");
    toast_stamp(kTwiliMidnaIndex, true);
}

s32 required_free() {
    const s32 measured = StampBook::last_heap_use();
    return measured > 0 ? std::max<s32>(measured + kHeapMargin, kMinHeapFloor) : kFirstOpenFreeHeap;
}

bool heap_has_room(dMw_c* window, const char* why) {
    JKRExpHeap* heap = window != nullptr ? window->mpHeap : nullptr;
    const s32 freeSize = heap != nullptr ? heap->getTotalFreeSize() : 0;
    const s32 freeBlock = heap != nullptr ? heap->getFreeSize() : 0;
    if (freeSize >= required_free() && freeBlock >= kMinFreeBlock) {
        return true;
    }
    mods::log::error("stamp book ({}): 2D heap has {} bytes free (largest block {}), {} needed; not opening",
        why, freeSize, freeBlock, required_free());
    return false;
}

// --- Collection sub-window (mode 0) -------------------------------------------------------------

void restore_collection(dMw_c* window) {
    // HD collect_move_init(0x23) / GC collect_move_init(LETTER_CLOSE): fade in, delete the
    // sub-window, field_0x144 = 3, window status 3, re-create the Collection screen (its
    // screenSet restores the cursor from dMeter2Info, i.e. the stamp slot).
    dMw_c::dMw_fade_in();
    g_book.destroy();
    window->checkMemSize();
    window->field_0x144 = 3;
    dMeter2Info_setWindowStatus(3);
    if (window->mpMenuCollect == nullptr) {
        window->dMw_collect_create();
    }
}

void open_in_collection(dMw_c* window) {
    // HD 0x21 init/proc, GC collect_letter_open_init: at black, field_0x144 = 10, window status
    // 10, delete the Collection screen (saves its cursor), build the sub-window, fade in.
    window->field_0x144 = 10;
    dMeter2Info_setWindowStatus(10);
    window->dMw_collect_delete(true);
    g_requestScreen = nullptr;
    window->markMemSize();
    const bool ok = heap_has_room(window, "Collection") &&
                    g_book.create(BookMode::Collection, owned_mask(), window->mpHeap, window->mpStick);
    if (!ok) {
        restore_collection(window);
        se(Z2SE_SYS_ERROR);
        g_phase = Phase::Idle;
        ui::open_stamp_book(); // RmlUi fallback, with the reason in the log
        return;
    }
    dMw_c::dMw_fade_in();
    g_phase = Phase::Open;
}

void tick_collection(dMw_c* window) {
    switch (g_phase) {
    case Phase::Idle:
        break;
    case Phase::OpenRequested:
        se(Z2SE_SY_MENU_SUB_IN); // HD collect_close_init 0x02a177cc (sub-windows 5-8)
        dMw_c::dMw_fade_out();
        g_phase = Phase::FadingOut;
        g_phaseFrames = 0;
        break;
    case Phase::FadingOut:
        if (fader_black() || ++g_phaseFrames > kFadeTimeoutFrames) {
            open_in_collection(window);
        }
        break;
    case Phase::Open:
        if (!g_book.move()) {
            se(Z2SE_SY_MENU_SUB_OUT); // HD 0x23 init 0x02a18228: SUB_OUT + fade, no vibration
            dMw_c::dMw_fade_out();
            g_phase = Phase::FadingOutClose;
            g_phaseFrames = 0;
        }
        break;
    case Phase::FadingOutClose:
        if (fader_black() || ++g_phaseFrames > kFadeTimeoutFrames) {
            restore_collection(window);
            g_phase = Phase::Idle;
        }
        break;
    }
}

HookAction pre_collect_move_proc(ModContext*, void* args, void*, void*) {
    if (g_phase == Phase::Idle) {
        return HOOK_CONTINUE;
    }
    // While the book or its fades run, nothing of the Collection screen may run (it is deleted
    // while the book is open), and Start/B must not close the pause menu.
    tick_collection(mods::arg<dMw_c*>(args, 0));
    return HOOK_SKIP_ORIGINAL;
}

// --- draw / teardown ------------------------------------------------------------------------

void post_draw(ModContext*, void*, void*, void*) {
    // dMw_c::_draw draws sub-windows only at window status 10 while paused (d_menu_window.cpp:1672).
    if (g_book.is_open() && dComIfGp_isPauseFlag() && dMeter2Info_getWindowStatus() == 10) {
        dComIfGd_set2DOpa(&g_book);
    }
}

HookAction pre_delete(ModContext*, void*, void*, void*) {
    if (g_book.is_open()) {
        mods::log::warn("menu window deleted while the stamp book was open; closing it");
        g_book.destroy();
    }
    g_phase = Phase::Idle;
    g_requestScreen = nullptr;
    g_direct = false;
    g_directFailed = false;
    g_requestThisFrame = false;
    // HD keeps its request in dMw_c (+0x165): it does not survive the window.
    g_armedMode = BookMode::Collection;
    return HOOK_CONTINUE;
}

// --- automatic opening from the field (modes 2/3) ---------------------------------------------

// The menu window is back in key_wait_proc while this mod still thinks it owns a sub-window state:
// something else moved mMenuProc, and key_wait_init then ran mpHeap->freeAll() under the book.
// Nothing in the game does this (Start/B are blocked while the book runs); another mod could.
void recover_from_foreign_close() {
    mods::log::warn("stamp book: the pause menu left the book's state from outside (phase {}, direct {})",
        static_cast<int>(g_phase), g_direct);
    if (g_book.is_open()) {
        g_book.abandon(); // its J2D objects were freed without destructors: never delete them again
    }
    if (g_direct && g_directMode == BookMode::Reward) {
        twili_reward_fallback_notice();
    }
    g_phase = Phase::Idle;
    g_requestScreen = nullptr;
    g_direct = false;
    g_directFailed = false;
}

HookAction pre_key_wait_proc(ModContext*, void* args, void*, void*) {
    g_requestThisFrame = false;
    if (g_direct || g_phase != Phase::Idle || g_book.is_open()) {
        recover_from_foreign_close();
        return HOOK_CONTINUE;
    }
    if (!armed()) {
        return HOOK_CONTINUE;
    }
    if (!g_autoHooksOk || !mod_active() || !auto_open_enabled() || !book_available()) {
        if (g_armedMode == BookMode::Reward && mod_active()) {
            if (auto_open_enabled()) {
                twili_reward_fallback_notice();
            } else {
                toast_stamp(kTwiliMidnaIndex); // turned off in the options: the toasts option decides
            }
        }
        g_armedMode = BookMode::Collection;
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    // HD folds its request (+0x165) into key_wait_proc's Start branch (Zelda.rpx 0x02a19114-
    // 0x02a191f8): under the normal can-pause gates, and only with the map status 0 or 1, the
    // Collection screen accepted (window accept 3) and no stage change pending, exactly the gates
    // GC applies to Start (d_menu_window.cpp:618-621). Pause status 8 (GC's own field ->
    // sub-window path, Agitha's) skips those three and relaxes isEventCheck (a running non-talk
    // event is ignored while it is 8), so they are checked here, together with: no event, the
    // post-event cool-down over, no message up and nobody else using the pause status. The status
    // is only set for this one call (check.md): post_key_wait_proc resets it at once when
    // key_wait_proc does not take it. The 2D heap is not checked here: with an emphasis button
    // up (heap lock 5) two sub-heaps hold 4/5 of it until key_wait_proc's own branch frees them;
    // pre_insect_open_proc checks at black.
    if (window == nullptr || window->mMenuProc != dMw_c::NO_MENU || window->field_0x14B != 0 ||
        window->field_0x151 != 0 || window->field_0x148 > 0 || dComIfGp_event_runCheck() ||
        dComIfGp_getMesgStatus() != 0 || dMeter2Info_getPauseStatus() != 0 ||
        (dMeter2Info_getMapStatus() != 0 && dMeter2Info_getMapStatus() != 1) ||
        !dMeter2Info_isWindowAccept(3) || dComIfGp_isEnableNextStage())
    {
        return HOOK_CONTINUE;
    }
    dMeter2Info_setPauseStatus(8);
    g_requestThisFrame = true;
    return HOOK_CONTINUE;
}

void post_key_wait_proc(ModContext*, void* args, void*, void*) {
    if (!g_requestThisFrame) {
        return;
    }
    g_requestThisFrame = false;
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (window != nullptr && window->mMenuProc == dMw_c::INSECT_AGITHA_OPEN1) {
        // key_wait_proc took GC's field -> sub-window branch (kill message flag, heap lock 1,
        // pause status reset); the INSECT_AGITHA_* states are ours until insect_close_proc.
        g_direct = true;
        g_directFailed = false;
        g_directMode = g_armedMode;
        g_armedMode = BookMode::Collection;
        g_directFrames = 0;
        mods::log::info("stamp book: automatic opening (mode {})", static_cast<int>(g_directMode));
    } else if (dMeter2Info_getPauseStatus() == 8) {
        dMeter2Info_resetPauseStatus(); // not taken this frame: never leave status 8 behind
    }
}

HookAction pre_insect_open_init(ModContext*, void* args, void*, void*) {
    if (!g_direct) {
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    // HD 0x21 STAMP_OPEN init (0x02a181e8): no sound, fade out; window status 10 only at black.
    // The capture is GC's requirement for pausing the game behind a sub-window (HD's
    // dMw_stamp_create makes one too).
    window->field_0x144 = 10;
    dMw_c::dMw_fade_out();
    window->dMw_capture_create();
    g_directFrames = 0;
    return HOOK_SKIP_ORIGINAL;
}

HookAction pre_insect_open_proc(ModContext*, void* args, void*, void*) {
    if (!g_direct) {
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (!fader_black() && ++g_directFrames <= kFadeTimeoutFrames) {
        return HOOK_SKIP_ORIGINAL;
    }
    // GC insect_open_proc at black: field_0x152 = 0, heap lock 1, create (markMemSize).
    window->field_0x152 = 0;
    dComIfGp_setHeapLockFlag(1);
    window->markMemSize();
    const bool ok = heap_has_room(window, "automatic opening") &&
                    g_book.create(g_directMode, owned_mask(), window->mpHeap, window->mpStick);
    if (ok) {
        dMeter2Info_setWindowStatus(10);
        window->mMenuProc = dMw_c::INSECT_AGITHA_OPEN2;
    } else {
        // One-time event: not re-armed (a retry would fade out and in again every frame).
        g_directFailed = true;
        window->mMenuProc = dMw_c::INSECT_AGITHA_CLOSE;
        if (g_directMode == BookMode::Reward) {
            twili_reward_fallback_notice();
        }
    }
    return HOOK_SKIP_ORIGINAL;
}

HookAction pre_insect_open2_proc(ModContext*, void* args, void*, void*) {
    if (!g_direct) {
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    dMw_c::dMw_fade_in(); // HD 0x21 proc: status 10, fade in, -> 0x22
    window->mMenuProc = dMw_c::INSECT_AGITHA_MOVE;
    return HOOK_SKIP_ORIGINAL;
}

HookAction pre_insect_move_proc(ModContext*, void* args, void*, void*) {
    if (!g_direct) {
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (!g_book.move()) {
        window->mMenuProc = dMw_c::INSECT_AGITHA_CLOSE;
    }
    return HOOK_SKIP_ORIGINAL;
}

HookAction pre_insect_close_init(ModContext*, void*, void*, void*) {
    if (!g_direct) {
        return HOOK_CONTINUE;
    }
    // HD 0x23 STAMP_CLOSE init: SUB_OUT + fade out, no vibration (C15).
    if (!g_directFailed) {
        se(Z2SE_SY_MENU_SUB_OUT);
    }
    dMw_c::dMw_fade_out();
    g_directFrames = 0;
    return HOOK_SKIP_ORIGINAL;
}

HookAction pre_insect_close_proc(ModContext*, void* args, void*, void*) {
    if (!g_direct) {
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (!fader_black() && ++g_directFrames <= kFadeTimeoutFrames) {
        return HOOK_SKIP_ORIGINAL;
    }
    // HD 0x23 proc for a direct open: -> NO_MENU. The book is destroyed before
    // key_wait_init(INSECT_AGITHA_CLOSE) runs mpHeap->freeAll() (which would skip the J2D
    // destructors); key_wait_init then fades in, deletes the capture and sets window status 0.
    g_book.destroy();
    window->checkMemSize();
    g_direct = false;
    g_directFailed = false;
    window->mMenuProc = dMw_c::NO_MENU;
    return HOOK_SKIP_ORIGINAL;
}

// --- arming (HD: the item-get window's destructor) -----------------------------------------------

HookAction pre_item_window_dtor(ModContext*, void* args, void*, void*) {
    auto* window = mods::arg<dMsgScrnItem_c*>(args, 0);
    if (window == nullptr || !g_autoHooksOk || !mod_active() || !auto_open_enabled() || !book_available()) {
        return HOOK_CONTINUE;
    }
    // HD reads the window's item number (+0x1A7); GC's dMsgScrnItem_c keeps it in mItemIndex
    // (message id - 0x65 for item-get text, d_msg_scrn_item.cpp).
    if (window->mItemIndex < 0 || window->mItemIndex > 0xFF) {
        return HOOK_CONTINUE;
    }
    const int stamp = stamp_index_for_item(static_cast<uint8_t>(window->mItemIndex));
    if (stamp < 0) {
        return HOOK_CONTINUE;
    }
    // HD: count == 1 -> mode 2 (view-only); count >= 49 -> mode 3 (Twili Midna reward). Bit 49 is
    // granted together with the 49th stamp, so the reward case is reached at 50.
    const int count = owned_count();
    if (count == kFirstStampCount) {
        g_armedMode = BookMode::FirstStamp;
    } else if (count >= kRewardStampCount) {
        g_armedMode = BookMode::Reward;
    } else {
        return HOOK_CONTINUE;
    }
    mods::log::info("stamp book: armed by the item-get window of stamp {} ({} owned)", stamp, count);
    return HOOK_CONTINUE;
}

// --- installation --------------------------------------------------------------------------

template <class Entry>
bool add_pre(HookPreFn fn, const char* what, int priority = 0) {
    ++g_wanted;
    HookOptions options = HOOK_OPTIONS_INIT;
    options.priority = priority;
    const ModResult result = mods::hook::add_pre<Entry>(fn, &options);
    if (result != MOD_OK) {
        mods::log::error("stamp book: could not hook {} (result {})", what, static_cast<int>(result));
        return false;
    }
    ++g_installed;
    return true;
}

template <class Entry>
bool add_post(HookPostFn fn, const char* what) {
    ++g_wanted;
    const ModResult result = mods::hook::add_post<Entry>(fn);
    if (result != MOD_OK) {
        mods::log::error("stamp book: could not hook {} (result {})", what, static_cast<int>(result));
        return false;
    }
    ++g_installed;
    return true;
}

} // namespace

int install() {
    g_installed = 0;
    g_wanted = 0;
    // The skip pre-hooks run first (high priority) so that no lower pre-hook of another mod sees
    // a deleted Collection screen while the book is open.
    bool ok = add_pre<BkMwCollectMoveProc>(pre_collect_move_proc, "dMw_c::collect_move_proc", 1000);
    ok = add_post<BkMwDraw>(post_draw, "dMw_c::_draw") && ok;
    ok = add_pre<BkMwDelete>(pre_delete, "dMw_c::_delete", 1000) && ok;
    // High priority as well: the request (and the recovery check) must not depend on another
    // mod's pre-hook letting key_wait_proc run; post_key_wait_proc runs even if one skips it.
    bool direct = add_pre<BkMwKeyWaitProc>(pre_key_wait_proc, "dMw_c::key_wait_proc", 1000);
    direct = add_post<BkMwKeyWaitProc>(post_key_wait_proc, "dMw_c::key_wait_proc (post)") && direct;
    direct = add_pre<BkMwInsectOpenInit>(pre_insect_open_init, "dMw_c::insect_open_init", 1000) && direct;
    direct = add_pre<BkMwInsectOpenProc>(pre_insect_open_proc, "dMw_c::insect_open_proc", 1000) && direct;
    direct = add_pre<BkMwInsectOpen2Proc>(pre_insect_open2_proc, "dMw_c::insect_open2_proc", 1000) && direct;
    direct = add_pre<BkMwInsectMoveProc>(pre_insect_move_proc, "dMw_c::insect_move_proc", 1000) && direct;
    direct = add_pre<BkMwInsectCloseInit>(pre_insect_close_init, "dMw_c::insect_close_init", 1000) && direct;
    direct = add_pre<BkMwInsectCloseProc>(pre_insect_close_proc, "dMw_c::insect_close_proc", 1000) && direct;
    direct = add_pre<BkMsgScrnItemDtor>(pre_item_window_dtor, "dMsgScrnItem_c::~dMsgScrnItem_c") && direct;
    g_hooksOk = ok;
    // Without every one of these the automatic opening is never requested (the request is only
    // made when key_wait_proc's post-hook and all INSECT_AGITHA_* replacements exist).
    g_autoHooksOk = direct && ok;
    if (!g_autoHooksOk) {
        mods::log::warn("stamp book: automatic opening after the 1st/49th stamp is unavailable");
    }
    return g_wanted - g_installed;
}

bool hooks_ok() {
    return g_hooksOk;
}

bool auto_open_hooks_ok() {
    return g_autoHooksOk;
}

int hooks_installed() {
    return g_installed;
}

int hooks_wanted() {
    return g_wanted;
}

bool request_open_from_collection(dMenu_Collect2D_c* screen) {
    if (g_phase != Phase::Idle || g_direct || g_book.is_open() || screen == nullptr) {
        return false;
    }
    // HD wait_proc sets its sub-window request (5) here; the stamps' own value keeps other hooks
    // on the Collection screen (the cave mod's amiibo button) from treating it as idle.
    screen->mSubWindowOpenCheck = kSubWindowStamps;
    g_requestScreen = screen;
    g_phase = Phase::OpenRequested;
    return true;
}

bool busy() {
    return g_phase != Phase::Idle || g_direct || g_book.is_open();
}

void disarm() {
    g_armedMode = BookMode::Collection;
}

void shutdown() {
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    if (window != nullptr && g_phase != Phase::Idle) {
        // Leave a Collection screen the unhooked game can run.
        JKRHeap* previous = mDoExt_setCurrentHeap(window->mpHeap);
        if (g_phase == Phase::Open || g_phase == Phase::FadingOutClose || g_book.is_open() ||
            window->mpMenuCollect == nullptr)
        {
            restore_collection(window);
        } else {
            // Still on the Collection screen (request or fade-out): cancel the request.
            if (g_requestScreen != nullptr && window->mpMenuCollect != nullptr &&
                window->mpMenuCollect->mpCollect2D == g_requestScreen)
            {
                g_requestScreen->mSubWindowOpenCheck = 0;
            }
            dMw_c::dMw_fade_in();
        }
        mDoExt_setCurrentHeap(previous);
    }
    if (window != nullptr && g_direct) {
        // GC's own insect_close_proc returns to the game at black (key_wait_init cleans up).
        g_book.destroy();
        window->checkMemSize();
        dMw_c::dMw_fade_out();
        window->mMenuProc = dMw_c::INSECT_AGITHA_CLOSE;
    }
    if (g_book.is_open()) {
        g_book.destroy();
    }
    g_phase = Phase::Idle;
    g_requestScreen = nullptr;
    g_direct = false;
    g_directFailed = false;
    g_requestThisFrame = false;
    g_armedMode = BookMode::Collection;
    if (dMeter2Info_getPauseStatus() == 8 && window != nullptr && window->mMenuProc == dMw_c::NO_MENU) {
        dMeter2Info_resetPauseStatus();
    }
}

} // namespace hdstamps::book::host
