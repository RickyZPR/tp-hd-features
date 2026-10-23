// HD Features / Cave of Shadows: TP HD's "Cave of Shadows Record" screen (see record.hpp).
//
// HD reference (Zelda.rpx; disassembled for this pass in scratch cosimpl/impl-portals/re, VERIFIED unless noted):
//   dMw state 0x2F init 0x02a17bd8 (window status 10, SE 0xA8, dMw_fade_out, capture, dMenu_save_c(7)); proc
//     0x02a1a2d8 (_open, then fade in; status 2 -> 0x30), 0x02a1a34c (_move; status 3/4 -> 0x31), 0x31 init =
//     fade out, proc 0x02a1a3ac -> state 0 (NO_MENU).
//   _open case 7 0x02a06288: +0x244 = 1, header 1258, A hint 1061, B hint none, amiibo_n + losrpt_n 4020->4030,
//     dFile_amiibo_c::setData(figure, mode 1), dLos_record_c values 0x029a275c, proc 0x41.
//   proc 0x41 0x02a0cbcc: header / plate / panel animations done -> 0x42.
//   proc 0x42 0x02a0cc40: dLos_record_c move until done; then A hint 1036; the record into the save
//     (0xA98 time, 0xA9C damage, 0xA9E floor, 0xA9F life, 0xAA4 max life, 0xAA0 combo, 0xAA1-0xAA3 amiibo rows);
//     life > figure life (0x029a202c) -> Yes/No in (0x02a05a70 2089 -> 2099, cursor reset to No) + prompt 1261
//     -> 0x44; else the "Close" button (1263) -> 0x43.
//   proc 0x43 0x02a0cdf4: cursor on cl_bt_tx (setParam 0.53/0.7/0.03/0.7/0.7); A or B: save, SE 0x9F, gameStart.
//   proc 0x44/0x49 wait for the buttons -> cursor; 0x45 0x02a0cfc4 YesNoSelect: No -> SE 0x6E, buttons out
//     (2099 -> 2089), prompt hidden -> 0x46 (prompt 1262, buttons in, B hint 1017) -> 0x4a; Yes -> SE 0x6D,
//     buttons out, prompt hidden -> 0x47 (figure +0x11/+0x12/+0x13 = 0xA9E/0xA9F/0xAA4, NFC write, popup 1567,
//     callback 0x02a0d144: 1568 once per save (0xABB) else 1569, OK -> save + gameStart).
//   proc 0x4a 0x02a0d660: B or No -> SE 0x6E, buttons out, prompt hidden, no B hint -> 0x4b (buttons in, 1261 ->
//     0x44); Yes -> SE 0x6D, save, gameStart.
//   dLos_record_c 0x029a17d8 screen setup (labels 1254-1257/1260/1263, rows wolf/ganon/zelda dimmed to alpha
//     0x40), 0x029a275c values, 0x029a29d8 reveal (8 states, A skips), 0x029a2224 rows, 0x029a2370 time
//     "%d:%02d:%05.2f", 0x029a2488 combo "%d", 0x029a24a0 damage, 0x029a16ec draw (Nm_02 = losrpt_n global x
//     Scale(losrpt_n / submenu)); tables .data 0x10122f54-0x10122f90; gold FFE061FF (sinit 0x029a32f0).
//   dFile_amiibo_c record mode: 0x0294abfc hearts step, 0x0294b028 heart difference, 0x0294b3ec NEW Record.
// INFERRED here: the cursor parameters on the Yes/No buttons (GC's yesnoCursorShow, as confirm.cpp), the
// selection-swap frames (confirm.cpp's), B on the first 1261 question (ignored), the virtual NFC popup's pacing.

#include "stamps/prelude.hpp"  // library headers before any game header (see prelude.hpp)

#include "record.hpp"

#include "cave.hpp"
#include "confirm.hpp"
#include "hdtext.hpp"
#include "savescreen.hpp"

#include <mods/svc/hook.hpp>
#include <mods/svc/log.hpp>

#include "JSystem/J2DGraph/J2DAnmLoader.h"
#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "d/d_lib.h"
#include "d/d_menu_save.h"
#include "d/d_menu_window.h"
#include "d/d_meter2.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_object.h"
#include "d/d_pane_class.h"
#include "d/d_select_cursor.h"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_controller_pad.h"
#include <dolphin/mtx.h>

// Hook targets: exported by dusklight.exe v2.0.3 (SDK import library), reached through dMw_c's process
// tables (no inline sites). key_wait_proc, _draw and _delete are hooked by the stamp book too (and _draw /
// _delete by confirm.cpp), under other aliases: HookService keeps one detour per target with every callback.
//   dMw_c::key_wait_proc   pre (1000): opens the screen when requested; skips the idle process while it runs
//   dMw_c::_draw           post: queue the screen at window status 10
//   dMw_c::_delete         pre (1000): destroy the screen before the menu heap goes
//   dMw_c::collect_save_close_proc  post: after the record's save the menu closes instead of opening the
//                                   Collection screen (GC's SAVE_CLOSE goes back to it on save status 3)
DEFINE_HOOK(&dMw_c::key_wait_proc, CosRecKeyWaitProc);
DEFINE_HOOK(&dMw_c::collect_save_close_proc, CosRecSaveCloseProc);
DEFINE_HOOK(&dMw_c::_draw, CosRecMwDraw);
DEFINE_HOOK(&dMw_c::_delete, CosRecMwDelete);

namespace cave::record {
namespace {

using namespace savescreen;

constexpr const char* kLayoutSave = "zelda_file_select2.blo";
constexpr const char* kLayoutInfo = "zelda_file_select_info_amiibo.blo";
constexpr const char* kLayoutHints = "zelda_file_select_icon_parts_revo.blo";
constexpr const char* kLayoutRecord = "zelda_file_select_los_record.blo";
constexpr const char* kAnimation = "zelda_file_select2.bck";
// HD saveres tt_heart_00..03, renamed in the archive (see confirm.cpp); HD's own record code looks them up by
// name (0x1011e7fc table) through the volume list, where saveres comes first.
constexpr const char* kHeartTextures[4] = {
    "cos_heart_00.bti", "cos_heart_01.bti", "cos_heart_02.bti", "cos_heart_03.bti"};

// zelda_file_select2.bck frames (keys evaluated for this pass: at 2099 / 2137 / 2139 "No" is the enlarged
// button, at 2119 / 2149 "Yes"; at 4030 amiibo_n sits at y 27.7 and losrpt_n has scale 1).
constexpr int kFrameSetup = 1;
constexpr int kFramePanelFrom = 4020;      // HD _open case 7: 0x02a05d20 / 0x02a05dd4 (4020, 4030)
constexpr int kFramePanelTo = 4030;
constexpr int kFrameButtonsInFrom = 2089;  // HD 0x02a05a70(2089, 2099)
constexpr int kFrameButtonsInTo = 2099;
constexpr int kFrameButtonsOutFrom = 2099;
constexpr int kFrameButtonsOutTo = 2089;
constexpr int kFrameToNoFrom = 2119;       // selection swaps: confirm.cpp's (INFERRED from GC's table)
constexpr int kFrameToNoTo = 2137;
constexpr int kFrameToYesFrom = 2139;
constexpr int kFrameToYesTo = 2149;

// dLos_record_c (HD .data 0x10122f54-0x10122f90, VERIFIED).
constexpr int kHeartStepWait = 3;     // 0x10122f54: ticks between two heart steps
constexpr int kStateWait = 10;        // 0x10122f58: ticks before each reveal step
constexpr int kTimeFrames = 45;       // 0x10122f5c: time count-up length (and the damage count-up's cap)
constexpr int kComboStep = 4;         // 0x10122f60: ticks per combo step (and its tick sound)
constexpr int kDamageSeDefault = 8;   // 0x10122f64: damage tick-sound interval (recomputed per run)
constexpr u8 kRowDimAlpha = 0x40;     // 0x10122f68: unused amiibo row icon
constexpr int kRowType[3] = {1, 1, 0};  // 0x10122f88: 1 = msg 855 "B%d" (level), 0 = msg 854 "x%d" (count)
constexpr u32 kTimeMax = 360000000;   // 0x15752A00 ms: "99:59:59.99" from 100 hours on
constexpr int kHearts = 20;
constexpr int kHighlightFadeTicks = 5;  // GC g_msHIO.mSelectFrames (HD uses the same save menu)

const JUtility::TColor kGold(0xFF, 0xE0, 0x61, 0xFF);  // HD sinit 0x029a32f0 / 0x0294b4f4
const JUtility::TColor kTextSelected(255, 255, 255, 255);
const JUtility::TColor kTextOther(150, 150, 150, 255);  // HD 0x02a05a70: 0x969696FF
const JUtility::TColor kIconOn(255, 255, 255, 255);     // HD 0x02a0556c: hint icon white / 0x808080 without text
const JUtility::TColor kIconOff(128, 128, 128, 255);
const JUtility::TColor kBlack(0, 0, 0, 0);

// Rows of the "amiibo used" line (dLos_record_c +0x48.. order: wolf, ganon, zelda; HD 0x029a1cb4 loop).
constexpr std::array<const char*, 3> kRowPane = {"amwolf_n", "amganf_n", "amzeld_n"};
constexpr std::array<const char*, 3> kRowIcon = {"amwolf_i", "amganf_i", "amzeld_i"};
constexpr std::array<const char*, 3> kRowLabel = {"amwolf_l", "amganf_l", nullptr};
constexpr std::array<const char*, 3> kRowValue = {"amwolf_v", "amganf_v", "amzeld_v"};

enum class Choice : std::uint8_t { None, Finish };
enum Sel : int { kNo = 0, kYes = 1 };  // HD +0x24A: 0 = No (right button), 1 = Yes (left button)

int digits_offset(int value) {
    // HD 0x029a262c / 0x0294b2a8: the partial heart follows the number: < 10 -> -12, > 99 -> +12.
    if (value < 10) {
        return -12;
    }
    return value > 99 ? 12 : 0;
}

std::string format_time(std::uint32_t ms) {
    // HD 0x029a2370: "%d:%02d:%05.2f" (hours, minutes, seconds with hundredths), "99:59:59.99" from 100 h on.
    if (ms >= kTimeMax) {
        return "99:59:59.99";
    }
    // API review INFO: "%05.2f" rounds the seconds, so a count-up value such as 59,995 ms printed "0:00:60.00".
    // The same rounding (to the nearest hundredth, as "%05.2f" does) is done first and carried into the minutes;
    // every value HD's format shows correctly comes out the same.
    const std::uint32_t hundredths = (ms + 5) / 10;
    const std::uint32_t seconds = hundredths / 100;
    if (seconds >= 100u * 3600u) {
        return "99:59:59.99";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u:%02u:%02u.%02u", seconds / 3600, (seconds / 60) % 60, seconds % 60,
        hundredths % 100);
    return buf;
}

// The record of this request went into the cave blob (HD proc 0x42). FIX (API review #4): every way the request ends
// stores it exactly once, also when the screen is dropped from outside before its reveal finished.
bool g_recordStored = false;

void store_record(const cave::RecordValues& values) {
    cave::record_store(values);
    g_recordStored = true;
}

// =================================================================================================
// The screen.
// =================================================================================================

class Screen final : public dDlst_base_c {
public:
    bool create(JKRHeap* heap, STControl* stick, const Input& input);
    void destroy();
    void abandon();
    bool is_open() const { return mOpen; }
    Choice move();
    void draw() override;
    void amiibo_closed(bool written);
    static s32 last_heap_use() { return sLastHeapUse; }

private:
    // HD procs: 0x41 Open, 0x42 Reveal, 0x43 Close, 0x44/0x49 ButtonsIn, 0x45/0x4a Select (+ swap), 0x46/0x47/
    // 0x4b ButtonsOut, 0x48 Write.
    enum class Step : std::uint8_t { Open, Reveal, Close, ButtonsIn, Select, Swap, ButtonsOut, Write, Done };
    enum class Question : std::uint8_t { Store, Leave };   // 1261 / 1262
    enum class After : std::uint8_t { None, Leave, Write, Store };

    bool build();
    bool lookup();
    void setup();
    // dLos_record_c
    void set_row(int idx, int value);
    void set_time(std::uint32_t ms);
    void set_combo(int value);
    void set_damage(int quarters, bool counting);
    void reveal();
    bool reveal_state(bool skip, u32& sound);
    bool count(int& out);
    // dFile_amiibo_c (record mode)
    void draw_hearts(int life, int maxLife);
    bool heart_step(int targetLife, int targetMax, bool skip);
    void set_diff(int diff);
    // save menu helpers
    void set_hint(bool a, u32 messageId, const char* fallback);
    void buttons_in(Question question);
    void buttons_out(After after);
    void cursor_show();
    void cursor_close();
    void set_selection_look(int selected);
    void select();
    void start_swap(int to);
    void swap();
    void finish();
    J2DPane* button(int sel) const { return sel == kYes ? mYes : mNo; }
    J2DPane* highlight(int sel) const { return sel == kYes ? mYesHi : mNoHi; }
    J2DTextBox* label(int sel) const { return sel == kYes ? mYesText : mNoText; }
    void release_font();
    void forget();

    static inline s32 sLastHeapUse = 0;

    bool mOpen = false;
    bool mFontRef = false;
    JKRHeap* mHeap = nullptr;
    STControl* mStick = nullptr;
    JKRArchive* mArchive = nullptr;
    Input mIn;

    J2DScreen* mSave = nullptr;    // zelda_file_select2.blo
    J2DScreen* mInfo = nullptr;    // zelda_file_select_info_amiibo.blo (on amiibo_n)
    J2DScreen* mHints = nullptr;   // zelda_file_select_icon_parts_revo.blo
    J2DScreen* mRec = nullptr;     // zelda_file_select_los_record.blo (on losrpt_n)
    dSelect_cursor_c* mCursor = nullptr;
    J2DAnmTransform* mAnmButtons = nullptr;
    J2DAnmTransform* mAnmPanel = nullptr;

    // save screen
    J2DPane* mNm02 = nullptr;
    J2DPane* mPlate = nullptr;     // amiibo_n
    J2DPane* mPanel = nullptr;     // losrpt_n
    J2DPane* mYes = nullptr;
    J2DPane* mNo = nullptr;
    J2DPane* mYesHi = nullptr;
    J2DPane* mNoHi = nullptr;
    J2DTextBox* mYesText = nullptr;
    J2DTextBox* mNoText = nullptr;
    J2DTextBox* mHeaderText = nullptr;
    J2DTextBox* mHeaderText1 = nullptr;
    // info screen
    J2DPane* mInfoNm02 = nullptr;
    J2DPane* mInfoDat = nullptr;
    std::array<J2DPicture*, kHearts> mHeart{};
    std::array<J2DPicture*, kHearts> mHeartBase{};
    J2DTextBox* mLevelLabel = nullptr;
    J2DTextBox* mLevelValue = nullptr;
    J2DPicture* mDiffHeart = nullptr;      // hear_01
    J2DPicture* mDiffPart = nullptr;       // hear_02
    J2DPicture* mDiffPartBase = nullptr;   // heab_02
    J2DTextBox* mDiffText = nullptr;       // w_htdiff
    J2DTextBox* mDiffSign = nullptr;       // w_htdif1
    J2DTextBox* mNewRecord = nullptr;      // t_newrec
    J2DPane* mNewRecordBadge = nullptr;    // t_w_04
    f32 mDiffPartX = 0.0f;
    // hints
    J2DPane* mABtn = nullptr;
    J2DPane* mBBtn = nullptr;
    J2DPicture* mAIcon = nullptr;
    J2DPicture* mBIcon = nullptr;
    std::array<J2DTextBox*, 4> mHintText{};  // cont_at, cont_at1, cont_bt, cont_bt1
    // record panel
    J2DPane* mRecNm02 = nullptr;
    J2DPane* mSubmenu = nullptr;
    std::array<J2DPane*, 3> mRowIcon{};
    std::array<J2DPane*, 3> mRowLabel{};
    std::array<J2DTextBox*, 3> mRowValue{};
    J2DTextBox* mTime = nullptr;           // cmtime_v
    J2DTextBox* mCombo = nullptr;          // mxcomb_v
    J2DTextBox* mDamage = nullptr;         // dmgrec_v
    J2DPane* mDamageHeart = nullptr;       // dmghea_1
    J2DPicture* mDamagePart = nullptr;     // dmghea_2
    J2DPane* mDamagePartBase = nullptr;    // dmghea_3
    f32 mDamagePartX = 0.0f;
    std::array<J2DPane*, 2> mPrompt{};     // promp_i1, promp_i2
    J2DTextBox* mPromptText = nullptr;     // prompt_t
    J2DPane* mCloseImage = nullptr;        // cl_bt_im
    J2DPane* mCloseText = nullptr;         // cl_bt_tx
    std::array<const ResTIMG*, 4> mHeartTex{};
    std::vector<J2DPane*> mN43;
    std::vector<std::pair<f32, f32>> mN43Saved;

    Anim mButtons;
    Anim mPlateAnim;  // amiibo_n + losrpt_n together (one J2DAnmTransform)
    Step mStep = Step::Open;
    Question mQuestion = Question::Store;
    After mAfter = After::None;
    int mSel = kNo;
    int mPrev = kYes;
    int mFadeTicks = 0;
    bool mBHint = false;
    bool mWriteDone = false;
    bool mWriteResult = false;

    // dLos_record_c reveal (+0x20 counter, +0x23 state, +0x24 row, +0x94..+0xA0 count-up)
    int mCounter = 0;
    int mState = 0;
    int mRow = 0;
    int mFrom = 0;
    int mTo = 0;
    int mTick = 0;
    int mLen = 1;
    int mDamageSe = kDamageSeDefault;
    bool mRevealDone = false;
    // dFile_amiibo_c's displayed hearts (+0x44 life, +0x48 max)
    int mShownLife = 0;
    int mShownMax = 0;
};

bool Screen::create(JKRHeap* heap, STControl* stick, const Input& input) {
    if (mOpen) {
        destroy();
    }
    if (heap == nullptr) {
        return false;
    }
    mHeap = heap;
    mStick = stick;
    mIn = input;
    JKRHeap* previous = mDoExt_setCurrentHeap(heap);
    const s32 freeBefore = heap->getTotalFreeSize();
    const bool ok = build() && lookup();
    if (ok) {
        setup();
        mOpen = true;
    }
    const s32 freeAfter = heap->getTotalFreeSize();
    mDoExt_setCurrentHeap(previous);
    if (!ok) {
        destroy();
        return false;
    }
    sLastHeapUse = freeBefore - freeAfter;
    mods::log::info("Cave of Shadows Record opened (floor {}, life {}/4, figure life {}/4, previous {}): 2D heap "
                    "{} -> {} ({} used)",
        mIn.cur.floor, mIn.cur.life, mIn.figure.life, mIn.hasPrev ? "yes" : "none", freeBefore, freeAfter,
        sLastHeapUse);
    return true;
}

bool Screen::build() {
    // GC-side pieces before the archive is mounted (a mounted JKRMemArchive is searched first).
    mCursor = JKR_NEW dSelect_cursor_c(0, 1.0f, nullptr);
    if (mCursor == nullptr) {
        return false;
    }
    mCursor->setAlphaRate(0.0f);
    mArchive = confirm::mount_screen_archive();
    if (mArchive == nullptr) {
        return false;
    }
    const std::array<std::pair<J2DScreen**, const char*>, 4> layouts = {{
        {&mSave, kLayoutSave}, {&mInfo, kLayoutInfo}, {&mHints, kLayoutHints}, {&mRec, kLayoutRecord}}};
    for (const auto& [slot, name] : layouts) {
        auto* screen = JKR_NEW J2DScreen();
        if (screen == nullptr) {
            mods::log::error("Cave of Shadows Record: out of 2D heap building {}", name);
            return false;
        }
        *slot = screen;
        if (!screen->setPriority(name, kSetPriorityFlags, mArchive)) {
            mods::log::error("Cave of Shadows Record: {} could not be built", name);
            return false;
        }
        dPaneClass_showNullPane(screen);
    }
    void* bck = mArchive->getResource(0, kAnimation);
    if (bck == nullptr) {
        mods::log::error("Cave of Shadows Record: {} missing", kAnimation);
        return false;
    }
    J2DAnmBase* a = J2DAnmLoaderDataBase::load(bck);
    J2DAnmBase* b = J2DAnmLoaderDataBase::load(bck);
    mAnmButtons = static_cast<J2DAnmTransform*>(a);
    mAnmPanel = static_cast<J2DAnmTransform*>(b);
    if (a == nullptr || b == nullptr || a->getKind() != KIND_TRANSFORM || b->getKind() != KIND_TRANSFORM) {
        mods::log::error("Cave of Shadows Record: {} is not a transform animation", kAnimation);
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        mHeartTex[i] = static_cast<const ResTIMG*>(mArchive->getResource('TIMG', kHeartTextures[i]));
        if (mHeartTex[i] == nullptr) {
            mods::log::error("Cave of Shadows Record: {} missing", kHeartTextures[i]);
            return false;
        }
    }
    return true;
}

bool Screen::lookup() {
    mNm02 = find(mSave, "Nm_02");
    mPlate = find(mSave, "amiibo_n");
    mPanel = find(mSave, "losrpt_n");
    mYes = find(mSave, "w_yes_n");
    mNo = find(mSave, "w_no_n");
    mYesHi = find_as<J2DPicture>(mSave, "w_yes_m");
    mNoHi = find_as<J2DPicture>(mSave, "w_no_m");
    mYesText = find_as<J2DTextBox>(mSave, "f_yes_t");
    mNoText = find_as<J2DTextBox>(mSave, "f_no_t");
    mHeaderText = find_as<J2DTextBox>(mSave, "t_for");
    mHeaderText1 = find_as<J2DTextBox>(mSave, "t_for1", false);
    bool ok = mNm02 != nullptr && mPlate != nullptr && mPanel != nullptr && mYes != nullptr && mNo != nullptr &&
              mYesHi != nullptr && mNoHi != nullptr && mYesText != nullptr && mNoText != nullptr &&
              mHeaderText != nullptr;

    mInfoNm02 = find(mInfo, "Nm_02");
    mInfoDat = find(mInfo, "w_dat_i1");
    for (int i = 0; i < kHearts; ++i) {
        mHeart[i] = find_as<J2DPicture>(mInfo, fmt::format("hear_{}", 20 + i));
        mHeartBase[i] = find_as<J2DPicture>(mInfo, fmt::format("heab_{}", 20 + i));
        ok = ok && mHeart[i] != nullptr && mHeartBase[i] != nullptr;
    }
    mLevelLabel = find_as<J2DTextBox>(mInfo, "level_l");
    mLevelValue = find_as<J2DTextBox>(mInfo, "level_v");
    mDiffHeart = find_as<J2DPicture>(mInfo, "hear_01");
    mDiffPart = find_as<J2DPicture>(mInfo, "hear_02");
    mDiffPartBase = find_as<J2DPicture>(mInfo, "heab_02");
    mDiffText = find_as<J2DTextBox>(mInfo, "w_htdiff");
    mDiffSign = find_as<J2DTextBox>(mInfo, "w_htdif1");
    mNewRecord = find_as<J2DTextBox>(mInfo, "t_newrec");
    mNewRecordBadge = find(mInfo, "t_w_04");
    ok = ok && mInfoNm02 != nullptr && mInfoDat != nullptr && mLevelLabel != nullptr && mLevelValue != nullptr &&
         mDiffHeart != nullptr && mDiffPart != nullptr && mDiffPartBase != nullptr && mDiffText != nullptr &&
         mDiffSign != nullptr && mNewRecord != nullptr && mNewRecordBadge != nullptr;

    mABtn = find(mHints, "abtn_n");
    mBBtn = find(mHints, "bbtn_n");
    mAIcon = find_as<J2DPicture>(mHints, "a_btn_cp", false);
    mBIcon = find_as<J2DPicture>(mHints, "b_btn_cp", false);
    mHintText = {find_as<J2DTextBox>(mHints, "cont_at"), find_as<J2DTextBox>(mHints, "cont_at1", false),
        find_as<J2DTextBox>(mHints, "cont_bt"), find_as<J2DTextBox>(mHints, "cont_bt1", false)};
    ok = ok && mABtn != nullptr && mBBtn != nullptr && mHintText[0] != nullptr && mHintText[2] != nullptr;

    mRecNm02 = find(mRec, "Nm_02");
    mSubmenu = find(mRec, "submenu");
    for (int i = 0; i < 3; ++i) {
        mRowIcon[i] = find(mRec, kRowIcon[i]);
        mRowLabel[i] = kRowLabel[i] != nullptr ? find(mRec, kRowLabel[i]) : nullptr;
        mRowValue[i] = find_as<J2DTextBox>(mRec, kRowValue[i]);
        ok = ok && find(mRec, kRowPane[i]) != nullptr && mRowIcon[i] != nullptr && mRowValue[i] != nullptr &&
             (kRowLabel[i] == nullptr || mRowLabel[i] != nullptr);
    }
    mTime = find_as<J2DTextBox>(mRec, "cmtime_v");
    mCombo = find_as<J2DTextBox>(mRec, "mxcomb_v");
    mDamage = find_as<J2DTextBox>(mRec, "dmgrec_v");
    mDamageHeart = find(mRec, "dmghea_1");
    mDamagePart = find_as<J2DPicture>(mRec, "dmghea_2");
    mDamagePartBase = find(mRec, "dmghea_3");
    mPrompt = {find(mRec, "promp_i1"), find(mRec, "promp_i2")};
    mPromptText = find_as<J2DTextBox>(mRec, "prompt_t");
    mCloseImage = find(mRec, "cl_bt_im");
    mCloseText = find(mRec, "cl_bt_tx");
    ok = ok && mRecNm02 != nullptr && mSubmenu != nullptr && mTime != nullptr && mCombo != nullptr &&
         mDamage != nullptr && mDamageHeart != nullptr && mDamagePart != nullptr && mDamagePartBase != nullptr &&
         mPrompt[0] != nullptr && mPrompt[1] != nullptr && mPromptText != nullptr && mCloseImage != nullptr &&
         mCloseText != nullptr;
    if (ok && (mInfoDat->getWidth() <= 0.0f || mInfoDat->getHeight() <= 0.0f || mSubmenu->getWidth() <= 0.0f ||
                  mSubmenu->getHeight() <= 0.0f))
    {
        mods::log::error("Cave of Shadows Record: w_dat_i1 / submenu has no size");
        ok = false;
    }
    return ok;
}

void Screen::setup() {
    mButtons.anm = mAnmButtons;
    mPlateAnim.anm = mAnmPanel;  // the plate and the panel play the same range together (one instance)
    // Frame 1 on the Nm_02 subtree (HD/GC screenSet): background and title bar in place, everything else out.
    mNm02->setAnimation(mAnmPanel);
    mAnmPanel->setFrame(static_cast<f32>(kFrameSetup));
    mNm02->animationTransform();
    mNm02->setAnimation(static_cast<J2DAnmTransform*>(nullptr));

    // Save screen: what HD's screenSet / _open hide for both Cave of Shadows uses (confirm.cpp).
    for (const char* name : {"w_tabi_s", "w_tabi_x", "w_tabi_1", "w_yes_t", "w_no_t", "Wi_btn_n", "a_title"}) {
        hide(mSave, name);
    }
    for (int i = 0; i < 10; ++i) {
        hide(mSave, fmt::format("tmoyou{:02}", i));
    }
    for (const char* name : {"w_yes_g", "w_no_g", "w_yes_gr", "w_no_gr"}) {
        if (J2DPane* pane = find(mSave, name, false); pane != nullptr) {
            pane->setAlpha(0);
        }
    }
    set_text(find_as<J2DTextBox>(mSave, "t_gmov_1", false), "", mHeap);

    // Fonts: every text box names rodan_24_22.bfn, which no archive has; the code sets the message font on
    // the ones it shows (HD screenSet, dFile_amiibo_c and dLos_record_c 0x029a17d8 setFont calls).
    JUTFont* font = mDoExt_getMesgFont();
    mFontRef = true;
    if (font != nullptr) {
        std::vector<J2DTextBox*> boxes = {mHeaderText, mHeaderText1, mYesText, mNoText, mLevelLabel, mLevelValue,
            mDiffText, mDiffSign, mNewRecord, mHintText[0], mHintText[1], mHintText[2], mHintText[3], mTime, mCombo,
            mDamage, mPromptText, mRowValue[0], mRowValue[1], mRowValue[2]};
        for (const char* name : {"amused_t", "cmtime_t", "mxcomb_t", "dmgrec_t", "amwolf_l", "amganf_l", "cl_bt_tx"}) {
            boxes.push_back(find_as<J2DTextBox>(mRec, name, false));
        }
        for (J2DTextBox* box : boxes) {
            if (box != nullptr) {
                box->setFont(font);
            }
        }
    }
    for (J2DTextBox* box : {mHeaderText, mHeaderText1}) {
        if (box != nullptr) {
            box->setFontSize(27.0f, 27.0f);
            box->setLineSpace(20.0f);
            box->setCharSpace(box == mHeaderText ? 0.0f : 2.0f);
        }
    }
    // _open case 7: header 1258.
    set_text(mHeaderText, hd_text(1258, "Cave of Shadows: your record"), mHeap);
    set_text(mHeaderText1, "", mHeap);
    if (mHeaderText1 != nullptr) {
        mHeaderText1->setAlpha(0);
    }
    mHeaderText->setAlpha(255);
    // Yes/No keep the save menu's own 7 / 8 (HD never changes them in use type 7).
    set_message(mYesText, 7, "Yes", mHeap);
    set_message(mNoText, 8, "No", mHeap);
    mYesHi->setAlpha(0);
    mNoHi->setAlpha(0);
    // Hints: A 1061 "Skip" while the record is revealed, no B (HD 0x02a0556c / 0x02a0575c).
    mABtn->show();
    mBBtn->show();
    set_hint(true, 1061, "Skip");
    set_hint(false, 0, "");

    // Info screen in record mode (setup 0x0294a1f8 + setData(figure, 1)).
    for (const char* name : {"t_w_02", "t_w_01", "w_hm", "hear_01", "hear_02", "heab_02", "t_w_04"}) {
        hide(mInfo, name);
    }
    for (J2DTextBox* box : {mDiffText, mNewRecord, mDiffSign}) {
        set_text(box, "", mHeap);
    }
    mDiffPartX = mDiffPart->getTranslateX();
    // Plate: the figure's hearts (0x0294abfc with skip), level = the floor the run ended on (0x0294aa8c).
    mShownLife = std::max(mIn.figure.life, 0);
    mShownMax = std::max(mIn.figure.maxLife, 0);
    draw_hearts(mShownLife, mShownMax);
    if (mIn.cur.floor <= 0) {
        mLevelLabel->hide();
        set_text(mLevelValue, "", mHeap);
    } else {
        mLevelLabel->show();
        set_text(mLevelLabel, hd_text(1260, "Level"), mHeap);
        set_text(mLevelValue, format_number(hd_text(855, "B%d"), mIn.cur.floor), mHeap);
    }

    // Record panel (0x029a17d8): labels, rows dimmed, prompt and Close hidden, damage hearts hidden.
    const std::array<std::pair<const char*, std::uint16_t>, 7> labels = {{{"amused_t", 1254}, {"cmtime_t", 1255},
        {"mxcomb_t", 1256}, {"dmgrec_t", 1257}, {"amwolf_l", 1260}, {"amganf_l", 1260}, {"cl_bt_tx", 1263}}};
    const std::array<const char*, 7> fallback = {
        "With amiibo", "Time", "Best combo", "Damage taken", "Level", "Level", "Close"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        set_text(find_as<J2DTextBox>(mRec, labels[i].first, false), hd_text(labels[i].second, fallback[i]), mHeap);
    }
    set_text(mPromptText, "", mHeap);
    for (J2DPane* pane : {mPrompt[0], mPrompt[1], mCloseImage, mCloseText, mDamageHeart,
             static_cast<J2DPane*>(mDamagePart), mDamagePartBase})
    {
        pane->hide();
    }
    mDamagePartX = mDamagePart->getTranslateX();
    for (J2DTextBox* box : {mTime, mCombo, mDamage}) {
        set_text(box, "", mHeap);
    }
    for (int i = 0; i < 3; ++i) {
        set_row(i, 0);
    }
    // The previous record first (0x029a275c): rows, time, combo, damage; none without one.
    if (mIn.hasPrev) {
        set_row(0, mIn.prev.wolf);
        set_row(1, mIn.prev.ganon);
        set_row(2, mIn.prev.zelda);
        set_time(mIn.prev.timeMs);
        set_combo(mIn.prev.combo);
        set_damage(mIn.prev.damage, false);
    }

    // _open case 7: the plate rises and the panel scales in together (4020 -> 4030).
    mPlate->show();
    mPanel->show();
    mPlateAnim.start(kFramePanelFrom, kFramePanelTo, mPlate, mPanel);

    collect_n43(mSave, mN43);
    collect_n43(mHints, mN43);
    mN43Saved.assign(mN43.size(), {1.0f, 1.0f});

    mStep = Step::Open;
    mSel = kNo;
    mPrev = kYes;
    mState = 0;
    mCounter = 0;
    mRow = 0;
    mRevealDone = false;
}

// ---- dLos_record_c ------------------------------------------------------------------------------

void Screen::set_row(int idx, int value) {
    // HD 0x029a2224: used rows full alpha + "Level" label + the value; unused rows dimmed, no label, no value.
    if (value > 0) {
        mRowIcon[idx]->setAlpha(0xFF);
        if (mRowLabel[idx] != nullptr) {
            mRowLabel[idx]->show();
        }
        const std::string format = kRowType[idx] == 1 ? hd_text(855, "B%d") : hd_text(854, "x%d");
        set_text(mRowValue[idx], format_number(format, value), mHeap);
    } else {
        mRowIcon[idx]->setAlpha(kRowDimAlpha);
        if (mRowLabel[idx] != nullptr) {
            mRowLabel[idx]->hide();
        }
        set_text(mRowValue[idx], "", mHeap);
    }
}

void Screen::set_time(std::uint32_t ms) {
    set_text(mTime, format_time(ms), mHeap);
}

void Screen::set_combo(int value) {
    set_text(mCombo, std::to_string(value), mHeap);
}

void Screen::set_damage(int quarters, bool counting) {
    // HD 0x029a24a0: heart icon, "x<whole hearts>", and while not counting a part heart (1/4..3/4) + " +".
    mDamageHeart->show();
    const int hearts = std::abs(quarters / 4);
    std::string text = format_number(hd_text(854, "x%d"), hearts);
    const int part = std::abs(quarters % 4);
    if (part != 0 && !counting) {
        text += " +";
        mDamagePart->changeTexture(mHeartTex[part], 0);
        const f32 x = mDamagePartX + static_cast<f32>(digits_offset(hearts));
        mDamagePart->translate(x, mDamagePart->getTranslateY());
        mDamagePartBase->translate(x, mDamagePartBase->getTranslateY());
        mDamagePart->show();
        mDamagePartBase->show();
    } else {
        mDamagePart->hide();
        mDamagePartBase->hide();
    }
    set_text(mDamage, text, mHeap);  // hd_text is already in the game encoding
}

// HD 0x029a28e0: one step of a count-up; true (and the target) on its last step.
bool Screen::count(int& out) {
    ++mTick;
    if (mTick >= mLen) {
        out = mTo;
        return true;
    }
    const double t = static_cast<double>(mTick) / static_cast<double>(mLen);
    out = mFrom + static_cast<int>(static_cast<double>(mTo - mFrom) * t);
    return false;
}

// HD 0x029a29d8. Every state waits mCounter ticks first; A runs the rest at once (every state in one call).
void Screen::reveal() {
    const bool skip = mDoCPd_c::getTrigA(PAD_1) != 0;
    if (!skip && mCounter > 0) {
        --mCounter;
        return;
    }
    u32 sound = 0;
    while (true) {
        const bool more = reveal_state(skip, sound);
        if (!skip || !more || mRevealDone) {
            break;
        }
    }
    if (sound != 0) {
        se(sound);
    }
}

// One pass of the state switch; returns false once the record is complete.
bool Screen::reveal_state(bool skip, u32& sound) {
    const bool prev = mIn.hasPrev;
    switch (mState) {
    case 0:
        mState = 1;
        mCounter = kStateWait;
        return true;
    case 1: {
        // Hearts from the figure's to the current life (0x0294abfc), the difference counting along (0x0294b028).
        const bool done = heart_step(mIn.cur.life, mIn.cur.maxLife, skip);
        set_diff(mShownLife - std::max(mIn.figure.life, 0));
        if (!done) {
            mCounter = kHeartStepWait;
            return true;
        }
        mRow = 0;
        mState = 2;
        for (int i = 0; i < 3; ++i) {
            set_row(i, 0);
        }
        mCounter = kStateWait;
        if (std::max(mIn.figure.life, 0) < mIn.cur.life) {
            // 0x0294b3ec: "NEW Record!" (1259) and the record badge.
            set_text(mNewRecord, hd_text(1259, "New best!"), mHeap);
            mNewRecordBadge->show();
        }
        return true;
    }
    case 2: {
        const int values[3] = {mIn.cur.wolf, mIn.cur.ganon, mIn.cur.zelda};
        while (mRow < 3) {
            const int idx = mRow++;
            if (values[idx] > 0) {
                set_row(idx, values[idx]);
                sound = Z2SE_ITEM_RING_IN;  // 0x3C
                mCounter = kStateWait;
                return true;
            }
        }
        // Time: from the previous record (or 0) up to this run's, 45 frames.
        mFrom = prev ? static_cast<int>(std::min<std::uint32_t>(mIn.prev.timeMs, 0x7FFFFFFF)) : 0;
        mTo = static_cast<int>(std::min<std::uint32_t>(mIn.cur.timeMs, 0x7FFFFFFF));
        mTick = 0;
        mLen = kTimeFrames;
        mState = 3;
        mCounter = kStateWait;
        return true;
    }
    case 3: {
        int value = 0;
        const bool done = count(value);
        set_time(static_cast<std::uint32_t>(value));
        if (!done) {
            if (mTick > 0 && (mTick & 1) == 0) {
                se(Z2SE_CONSUM_INC_CNT_1);  // 0x1B
            }
            return true;
        }
        if (!prev || mIn.cur.timeMs < mIn.prev.timeMs) {
            mTime->setWhite(kGold);
        }
        sound = Z2SE_CONSUM_INC_CNT_2;  // 0x1C
        mFrom = prev ? mIn.prev.combo : 0;
        mTo = mIn.cur.combo;
        mTick = 0;
        mLen = (kComboStep <= 0 || mFrom == mTo) ? 1 : kComboStep * std::abs(mTo - mFrom);
        mState = 4;
        mCounter = kStateWait;
        return true;
    }
    case 4: {
        int value = 0;
        const bool done = count(value);
        set_combo(value);
        if (!done) {
            if (kComboStep > 0 && mTick > 0 && mTick % kComboStep == 0) {
                se(Z2SE_SY_TALK_CURSOR);  // 0x63
            }
            return true;
        }
        if (!prev || mIn.cur.combo > mIn.prev.combo) {
            mCombo->setWhite(kGold);
        }
        sound = Z2SE_SY_TALK_CURSOR_OK;  // 0x64
        mFrom = prev ? mIn.prev.damage : 0;
        mTo = mIn.cur.damage;
        int len = mFrom == mTo ? 1 : 2 * std::abs(mTo - mFrom);
        int interval = kDamageSeDefault;
        if (len > kTimeFrames) {
            interval = std::max(kTimeFrames * 8 / len, 2);
            len = kTimeFrames;
        }
        mDamageSe = interval;
        mTick = 0;
        mLen = len;
        mState = 5;
        mCounter = kStateWait;
        return true;
    }
    case 5: {
        int value = 0;
        const bool done = count(value);
        set_damage(value, !done);
        if (!done) {
            if (mDamageSe > 0 && mTick > 0 && mTick % mDamageSe == 0) {
                se(Z2SE_LUPY_DEC_CNT_1);  // 0x75
            }
            return true;
        }
        if (!prev || mIn.cur.damage < mIn.prev.damage) {
            mDamage->setWhite(kGold);
        }
        sound = Z2SE_LUPY_DEC_CNT_2;  // 0x76
        mState = 6;
        mCounter = kStateWait;
        return true;
    }
    case 6:
        // HD: with no previous record and Hero Mode the "Hard Mode" mark appears here; GC has no Hero Mode.
        mState = 7;
        mRevealDone = true;
        return false;
    default:
        mRevealDone = true;
        return false;
    }
}

// ---- dFile_amiibo_c (record mode) ----------------------------------------------------------------

void Screen::draw_hearts(int life, int maxLife) {
    // HD 0x0294a718 (as confirm.cpp's plate): hear_(20+i) for i < ceil(life/4), the last one 1/4..3/4;
    // heab_(20+i) for i < maxLife/5.
    const int quarters = std::max(life, 0);
    const int filled = std::min((quarters + 3) / 4, kHearts);
    const int part = quarters % 4;
    const int containers = std::clamp(maxLife / 5, 0, kHearts);
    for (int i = 0; i < kHearts; ++i) {
        if (i < filled) {
            const int tex = (i == filled - 1 && part != 0) ? part : 0;
            mHeart[i]->changeTexture(mHeartTex[tex], 0);
            mHeart[i]->show();
        } else {
            mHeart[i]->hide();
        }
        if (i < containers) {
            mHeartBase[i]->show();
        } else {
            mHeartBase[i]->hide();
        }
    }
}

// HD 0x0294abfc: one heart per call (sound 0x17), containers following the hearts, the full maximum last.
bool Screen::heart_step(int targetLife, int targetMax, bool skip) {
    targetLife = std::max(targetLife, 0);
    targetMax = std::max(targetMax, 0);
    if (skip) {
        mShownLife = targetLife;
        mShownMax = targetMax;
        draw_hearts(mShownLife, mShownMax);
    } else {
        const auto need = [](int life) { return static_cast<int>(std::ceil(static_cast<double>(life) * 0.25)); };
        if (need(mShownLife) != mShownMax / 5) {
            mShownMax = need(mShownLife) * 5;
            draw_hearts(mShownLife, mShownMax);
            se(Z2SE_HEART_PIECE_GET);
        } else if (targetLife > mShownLife) {
            mShownLife += (mShownLife % 4 == 0) ? 4 : 4 - mShownLife % 4;
            mShownLife = std::min(mShownLife, targetLife);
            if (need(mShownLife) != mShownMax / 5) {
                mShownMax = need(mShownLife) * 5;
            }
            draw_hearts(mShownLife, mShownMax);
            se(Z2SE_HEART_PIECE_GET);
        } else if (targetLife < mShownLife) {
            mShownLife -= (mShownLife % 4 == 0) ? 4 : mShownLife % 4;
            mShownLife = std::max(mShownLife, targetLife);
            if (need(mShownLife) != mShownMax / 5) {
                mShownMax = need(mShownLife) * 5;
            }
            draw_hearts(mShownLife, mShownMax);
            se(Z2SE_HEART_PIECE_GET);
        } else if (mShownMax / 5 != targetMax / 5) {
            mShownMax = targetMax;
            draw_hearts(mShownLife, mShownMax);
            se(Z2SE_HEART_PIECE_GET);
        }
    }
    return mShownLife == targetLife && mShownMax / 5 == targetMax / 5;
}

// HD 0x0294b028: the heart difference next to the plate's hearts: "+"/"-", a heart, "x<whole hearts>" and a part
// heart for the remainder; gold when hearts were gained.
void Screen::set_diff(int diff) {
    set_text(mDiffSign, diff >= 0 ? "+" : "-", mHeap);
    mDiffHeart->show();
    const int hearts = std::abs(diff / 4);
    std::string text = format_number(hd_text(854, "x%d"), hearts);
    const int part = std::abs(diff % 4);
    if (part != 0) {
        text += " +";
        mDiffPart->changeTexture(mHeartTex[part], 0);
        const f32 x = mDiffPartX + static_cast<f32>(digits_offset(hearts));
        mDiffPart->translate(x, mDiffPart->getTranslateY());
        mDiffPartBase->translate(x, mDiffPartBase->getTranslateY());
        mDiffPart->show();
        mDiffPartBase->show();
    } else {
        mDiffPart->hide();
        mDiffPartBase->hide();
    }
    set_text(mDiffText, text, mHeap);  // hd_text is already in the game encoding
    if (diff > 0) {
        mDiffText->setWhite(kGold);
        mDiffSign->setWhite(kGold);
    }
}

// ---- save menu helpers ---------------------------------------------------------------------------

void Screen::set_hint(bool a, u32 messageId, const char* fallback) {
    // HD 0x02a0556c (A) / 0x02a0575c (B): message 0 empties the text and greys the button icon.
    J2DTextBox* text = mHintText[a ? 0 : 2];
    J2DTextBox* shadow = mHintText[a ? 1 : 3];
    J2DPicture* icon = a ? mAIcon : mBIcon;
    if (messageId == 0) {
        set_text(text, "", mHeap);
        set_text(shadow, "", mHeap);
        if (icon != nullptr) {
            icon->setBlackWhite(kBlack, kIconOff);
        }
    } else {
        set_message(text, messageId, fallback, mHeap);
        set_message(shadow, messageId, fallback, mHeap);
        if (icon != nullptr) {
            icon->setBlackWhite(kBlack, kIconOn);
        }
    }
    if (!a) {
        mBHint = messageId != 0;
    }
}

void Screen::set_selection_look(int selected) {
    label(selected)->setWhite(kTextSelected);
    label(selected ^ 1)->setWhite(kTextOther);
}

void Screen::buttons_in(Question question) {
    // HD 0x02a05a70(2089, 2099, 0): the cursor goes back to No; selected text white, the other grey; then the
    // prompt (0x029a2048: promp_i1/i2 shown, prompt_t = the question).
    mQuestion = question;
    mSel = kNo;
    mPrev = kYes;
    mCursor->setAlphaRate(0.0f);
    mYesHi->setAlpha(0);
    mNoHi->setAlpha(0);
    set_selection_look(mSel);
    mButtons.start(kFrameButtonsInFrom, kFrameButtonsInTo, mYes, mNo);
    if (question == Question::Store) {
        set_text(mPromptText, hd_text(1261, "Store your hearts in the\nWolf Link amiibo?"), mHeap);
    } else {
        set_text(mPromptText, hd_text(1262, "Leave without saving\nyour record?"),
            mHeap);
    }
    mPrompt[0]->show();
    mPrompt[1]->show();
    mStep = Step::ButtonsIn;
}

void Screen::buttons_out(After after) {
    // HD 0x02a05a70(2099, 2089) + 0x029a20e0 (prompt text emptied, promp_i1/i2 hidden).
    mAfter = after;
    mCursor->setAlphaRate(0.0f);
    mButtons.start(kFrameButtonsOutFrom, kFrameButtonsOutTo, mYes, mNo);
    set_text(mPromptText, "", mHeap);
    mPrompt[0]->hide();
    mPrompt[1]->hide();
    mStep = Step::ButtonsOut;
}

void Screen::cursor_show() {
    // GC dMenu_save_c::yesnoCursorShow (HD 0x02a07950): white text, highlight, cursor on the button.
    label(mSel)->setWhite(kTextSelected);
    highlight(mSel)->setAlpha(255);
    highlight(mSel ^ 1)->setAlpha(0);
    mCursor->setPos(0.0f, 0.0f, button(mSel), true);
    mCursor->setAlphaRate(1.0f);
    mCursor->setParam(0.96f, 0.84f, 0.06f, 0.5f, 0.5f);
}

void Screen::cursor_close() {
    // HD proc 0x43 (0x02a0ce68-0x02a0ced4): on cl_bt_tx, alpha 1, setParam(0.53, 0.7, 0.03, 0.7, 0.7).
    mCursor->setPos(0.0f, 0.0f, mCloseText, true);
    mCursor->setAlphaRate(1.0f);
    mCursor->setParam(0.53f, 0.7f, 0.03f, 0.7f, 0.7f);
}

void Screen::select() {
    // HD YesNoSelect (0x02a0678c) in procs 0x45 / 0x4a. Input only while the fader is idle.
    if (!fader_idle()) {
        return;
    }
    const bool b = mDoCPd_c::getTrigB(PAD_1) != 0;
    if (mDoCPd_c::getTrigA(PAD_1) != 0) {
        if (mSel == kYes) {
            se(Z2SE_SY_CURSOR_OK);  // 0x6D
            if (mQuestion == Question::Store) {
                buttons_out(After::Write);  // proc 0x47
            } else {
                mCursor->setAlphaRate(0.0f);
                finish();  // proc 0x4a Yes: save, gameStart
            }
        } else {
            se(Z2SE_SY_CURSOR_CANCEL);  // 0x6E
            buttons_out(mQuestion == Question::Store ? After::Leave : After::Store);  // 0x46 / 0x4b
        }
        return;
    }
    if (b && mQuestion == Question::Leave && mBHint) {
        se(Z2SE_SY_CURSOR_CANCEL);  // proc 0x4a: B as No
        buttons_out(After::Store);
        return;
    }
    if (mStick == nullptr) {
        return;
    }
    if (mStick->checkRightTrigger()) {
        if (mSel == kYes) {
            start_swap(kNo);
        }
    } else if (mStick->checkLeftTrigger()) {
        if (mSel == kNo) {
            start_swap(kYes);
        }
    }
}

void Screen::start_swap(int to) {
    se(Z2SE_SY_MENU_CURSOR_COMMON);
    dMeter2Info_set2DVibration();
    mPrev = mSel;
    mSel = to;
    mFadeTicks = 0;
    mCursor->setAlphaRate(0.0f);
    if (to == kNo) {
        mButtons.start(kFrameToNoFrom, kFrameToNoTo, mYes, mNo);
    } else {
        mButtons.start(kFrameToYesFrom, kFrameToYesTo, mYes, mNo);
    }
    mStep = Step::Swap;
}

void Screen::swap() {
    const bool buttons = mButtons.step();
    if (mFadeTicks < kHighlightFadeTicks) {
        ++mFadeTicks;
        const float t = static_cast<float>(mFadeTicks) / kHighlightFadeTicks;
        highlight(mPrev)->setAlpha(static_cast<u8>(255.0f * (1.0f - t)));
        const auto mix = [t](u8 a, u8 b) { return static_cast<u8>(a + (b - a) * t); };
        label(mPrev)->setWhite(JUtility::TColor(mix(255, 150), mix(255, 150), mix(255, 150), 255));
    }
    if (buttons && mFadeTicks >= kHighlightFadeTicks) {
        cursor_show();
        mStep = Step::Select;
    }
}

void Screen::finish() {
    mCursor->setAlphaRate(0.0f);
    mStep = Step::Done;
}

void Screen::amiibo_closed(bool written) {
    if (mStep != Step::Write) {
        return;
    }
    mWriteDone = true;
    mWriteResult = written;
}

Choice Screen::move() {
    if (!mOpen) {
        return Choice::Finish;
    }
    switch (mStep) {
    case Step::Open:
        // proc 0x41: the plate and the panel are in place.
        if (mPlateAnim.step()) {
            mStep = Step::Reveal;
        }
        break;
    case Step::Reveal:
        // proc 0x42
        reveal();
        if (mRevealDone) {
            set_hint(true, 1036, "Confirm");
            store_record(mIn.cur);  // HD: 0xA98..0xAA4 = this run
            if (std::max(mIn.figure.life, 0) < mIn.cur.life) {
                buttons_in(Question::Store);  // -> 0x44
            } else {
                mCloseImage->show();          // 0x029a217c -> 0x43
                mCloseText->show();
                cursor_close();
                mStep = Step::Close;
            }
        }
        break;
    case Step::Close:
        // proc 0x43: A or B.
        if (fader_idle() && (mDoCPd_c::getTrigA(PAD_1) != 0 || mDoCPd_c::getTrigB(PAD_1) != 0)) {
            se(Z2SE_SY_CONTINUE_OK);  // 0x9F
            finish();
        }
        break;
    case Step::ButtonsIn:
        // procs 0x44 / 0x49
        if (mButtons.step()) {
            cursor_show();
            mStep = Step::Select;
        }
        break;
    case Step::Select:
        select();
        break;
    case Step::Swap:
        swap();
        break;
    case Step::ButtonsOut:
        // procs 0x46 / 0x47 / 0x4b
        if (!mButtons.step()) {
            break;
        }
        switch (mAfter) {
        case After::Leave:  // 0x46: "return without saving?" with B as "Back"
            buttons_in(Question::Leave);
            set_hint(false, 1017, "Back");
            break;
        case After::Store:  // 0x4b: back to the amiibo question
            set_hint(false, 0, "");
            buttons_in(Question::Store);
            break;
        case After::Write:  // 0x47: the figure and the NFC popup
            mWriteDone = false;
            mWriteResult = false;
            mStep = Step::Write;
            if (!cave::open_amiibo_write()) {
                mWriteDone = true;  // written at once (no popup)
                mWriteResult = true;
            }
            break;
        case After::None:
            finish();
            break;
        }
        break;
    case Step::Write:
        // proc 0x48: waits for the popup (0x02a0d144); OK -> save + gameStart, cancelled -> the 1262 question
        // (HD's NFC error path 0x02a0d400: prompt 1262, buttons in, proc 0x49).
        if (mWriteDone) {
            mWriteDone = false;
            if (mWriteResult) {
                finish();
            } else {
                buttons_in(Question::Leave);
                set_hint(false, 1017, "Back");
            }
        }
        break;
    case Step::Done:
        break;
    }
    return mStep == Step::Done ? Choice::Finish : Choice::None;
}

void Screen::draw() {
    if (!mOpen) {
        return;
    }
    const bool wide = mDoGph_gInf_c::hudAspectScaleUp > 1.0001f;
    const f32 up = wide ? mDoGph_gInf_c::hudAspectScaleUp : 1.0f;
    const f32 down = wide ? mDoGph_gInf_c::hudAspectScaleDown : 1.0f;
    const f32 left = wide ? mDoGph_gInf_c::getSafeMinXF() : 0.0f;
    for (J2DScreen* screen : {mSave, mHints}) {
        screen->scale(up, 1.0f);
        screen->translate(left, 0.0f);
    }
    for (std::size_t i = 0; i < mN43.size(); ++i) {
        J2DPane* pane = mN43[i];
        mN43Saved[i] = {pane->getScaleX(), pane->getScaleY()};
        pane->scale(mN43Saved[i].first * down, mN43Saved[i].second);
    }

    J2DGrafContext* port = dComIfGp_getCurrentGrafPort();
    port->setup2D();
    mSave->draw(0.0f, 0.0f, port);
    if (mPlate->isVisible()) {
        // HD 0x0294a108: info Nm_02 = amiibo_n global x Scale(amiibo_n / w_dat_i1).
        Mtx scale;
        Mtx placed;
        MTXScale(scale, mPlate->getWidth() / mInfoDat->getWidth(), mPlate->getHeight() / mInfoDat->getHeight(), 1.0f);
        MTXConcat(mPlate->getGlbMtx(), scale, placed);
        mInfoNm02->setMtx(placed);
        port->setup2D();
        mInfo->draw(0.0f, 0.0f, port);
    }
    if (mPanel->isVisible()) {
        // HD 0x029a16ec: record Nm_02 = losrpt_n global x Scale(losrpt_n / submenu).
        Mtx scale;
        Mtx placed;
        MTXScale(scale, mPanel->getWidth() / mSubmenu->getWidth(), mPanel->getHeight() / mSubmenu->getHeight(), 1.0f);
        MTXConcat(mPanel->getGlbMtx(), scale, placed);
        mRecNm02->setMtx(placed);
        port->setup2D();
        mRec->draw(0.0f, 0.0f, port);
    }
    port->setup2D();
    mHints->draw(0.0f, 0.0f, port);
    mCursor->refreshAspectScale(up);
    mCursor->draw();

    for (std::size_t i = 0; i < mN43.size(); ++i) {
        mN43[i]->scale(mN43Saved[i].first, mN43Saved[i].second);
    }
}

void Screen::destroy() {
    for (J2DPane* pane : {mYes, mNo, mPlate, mPanel, mNm02}) {
        if (pane != nullptr) {
            pane->setAnimation(static_cast<J2DAnmTransform*>(nullptr));
        }
    }
    if (mCursor != nullptr) {
        JKR_DELETE(mCursor);
        mCursor = nullptr;
    }
    for (J2DScreen** screen : {&mSave, &mInfo, &mHints, &mRec}) {
        if (*screen != nullptr) {
            JKR_DELETE(*screen);
            *screen = nullptr;
        }
    }
    if (mAnmButtons != nullptr) {
        JKR_DELETE(mAnmButtons);
        mAnmButtons = nullptr;
    }
    if (mAnmPanel != nullptr) {
        JKR_DELETE(mAnmPanel);
        mAnmPanel = nullptr;
    }
    confirm::unmount_screen_archive(mArchive);
    release_font();
    forget();
}

void Screen::abandon() {
    // The 2D heap was wiped under the screen: forget every pointer into it; the archive object lives on the
    // archive heap and still has to be unmounted.
    if (mOpen || mCursor != nullptr || mSave != nullptr) {
        mods::log::warn("Cave of Shadows Record: the pause menu was taken away under it; dropping it without destructors");
    }
    mCursor = nullptr;
    mSave = mInfo = mHints = mRec = nullptr;
    mAnmButtons = mAnmPanel = nullptr;
    confirm::unmount_screen_archive(mArchive);
    release_font();
    forget();
}

void Screen::release_font() {
    if (mFontRef) {
        mDoExt_removeMesgFont();
        mFontRef = false;
    }
}

void Screen::forget() {
    mNm02 = mPlate = mPanel = mYes = mNo = mYesHi = mNoHi = nullptr;
    mYesText = mNoText = mHeaderText = mHeaderText1 = nullptr;
    mInfoNm02 = mInfoDat = nullptr;
    mHeart.fill(nullptr);
    mHeartBase.fill(nullptr);
    mLevelLabel = mLevelValue = nullptr;
    mDiffHeart = mDiffPart = mDiffPartBase = nullptr;
    mDiffText = mDiffSign = mNewRecord = nullptr;
    mNewRecordBadge = nullptr;
    mABtn = mBBtn = nullptr;
    mAIcon = mBIcon = nullptr;
    mHintText.fill(nullptr);
    mRecNm02 = mSubmenu = nullptr;
    mRowIcon.fill(nullptr);
    mRowLabel.fill(nullptr);
    mRowValue.fill(nullptr);
    mTime = mCombo = mDamage = nullptr;
    mDamageHeart = mDamagePartBase = nullptr;
    mDamagePart = nullptr;
    mPrompt.fill(nullptr);
    mPromptText = nullptr;
    mCloseImage = mCloseText = nullptr;
    mHeartTex.fill(nullptr);
    mN43.clear();
    mN43Saved.clear();
    mButtons.reset();
    mPlateAnim.reset();
    mOpen = false;
    mHeap = nullptr;
    mStick = nullptr;
}

// =================================================================================================
// Hosting in the pause-menu window (HD dMw states 0x2F-0x31, from the window's idle process).
// =================================================================================================

enum class Phase : std::uint8_t {
    Idle,
    Requested,   // HD dMw+0x166: waiting for the window's idle process
    FadingOut,   // HD 0x2F init: SE 0xA8, capture (the game pauses), fade out
    Open,        // HD 0x2F/0x30 with dMenu_save_c(7)
    FadingSave,  // the screen ended: fade out, then the game's own pause-menu save (SAVE_OPEN)
    Saving,      // the stock save menu runs (dMw SAVE_OPEN .. SAVE_CLOSE)
    Returning,   // the save menu closed: next idle frame, the HD return (cave::record_finished)
};

// After one opening: the measured use + a margin (never below the floor); confirm.cpp's numbers (this screen
// adds zelda_file_select_los_record.blo, 32 panes / 23 materials, to the same three layouts).
constexpr s32 kFirstOpenFreeHeap = 0x80000;
constexpr s32 kHeapMargin = 0x20000;
constexpr s32 kMinHeapFloor = 0x60000;
constexpr s32 kMinFreeBlock = 0x20000;
constexpr int kFadeTimeoutFrames = 60;
constexpr int kRequestTimeoutTicks = 30 * 8;   // the window never idled: return without the screen
constexpr int kSaveTimeoutTicks = 30 * 60 * 30;  // the save menu never closed (player AFK is fine: 30 min)

Screen g_screen;
Phase g_phase = Phase::Idle;
int g_phaseFrames = 0;
int g_requestTicks = 0;
Input g_input;
bool g_hostHooksOk = false;
bool g_saved = false;
dMw_c* g_window = nullptr;

s32 required_free() {
    const s32 measured = Screen::last_heap_use();
    return measured > 0 ? std::max<s32>(measured + kHeapMargin, kMinHeapFloor) : kFirstOpenFreeHeap;
}

bool heap_has_room(dMw_c* window) {
    JKRExpHeap* heap = window != nullptr ? window->mpHeap : nullptr;
    const s32 freeSize = heap != nullptr ? heap->getTotalFreeSize() : 0;
    const s32 freeBlock = heap != nullptr ? heap->getFreeSize() : 0;
    if (freeSize >= required_free() && freeBlock >= kMinFreeBlock) {
        return true;
    }
    mods::log::error("Cave of Shadows Record: 2D heap has {} bytes free (largest block {}), {} needed", freeSize,
        freeBlock, required_free());
    return false;
}

// The idle window may take the screen now (GC dMw_c::key_wait_proc's own conditions for opening a menu, the
// ones that do not read input; HD checks dMw+0x166 inside the same chain, 0x02a18fcc-0x02a19214).
bool window_ready() {
    JUTFader* fader = mDoGph_gInf_c::getFader();
    const u8 lock = dComIfGp_isHeapLockFlag();
    return fader != nullptr && fader->getStatus() == JUTFader::Wait && (lock == 0 || lock == 5) &&
           !dComIfGp_isPauseFlag() && !fopOvlpM_IsDoingReq() && !mDoGph_gInf_c::isFade() &&
           !dComIfGp_event_runCheck() && dComIfGs_getLife() != 0;
}

void open_window(dMw_c* window) {
    // HD 0x2F init 0x02a17bd8 (+ what GC's own menu openings do in key_wait_proc).
    dMsgObject_setKillMessageFlag();
    if (dComIfGp_isHeapLockFlag() == 5) {
        dMeter2Info_getMeterClass()->emphasisButtonDelete();
    }
    dComIfGp_setHeapLockFlag(1);
    window->field_0x144 = 10;
    dMeter2Info_setWindowStatus(10);
    se(Z2SE_SY_FILE_MENU_SLIDE_IN);  // 0xA8
    dMw_c::dMw_fade_out();
    window->dMw_capture_create();
    g_phase = Phase::FadingOut;
    g_phaseFrames = 0;
    mods::log::info("Cave of Shadows Record: opening");
}

void hand_over_to_save(dMw_c* window) {
    // At black: the game's own pause-menu save (Collection -> Save path from SAVE_OPEN on: window status 10,
    // dMenu_save_c use type 1; collect_save_open_proc fades in once it is built). confirm.cpp's save hooks give
    // it HD's "Saved." ending and close the menu (status 4) afterwards.
    g_screen.destroy();
    window->checkMemSize();
    window->mMenuProc = dMw_c::SAVE_OPEN;  // dMw_c::_execute runs its init after this (skipped) idle process
    g_phase = Phase::Saving;
    g_phaseFrames = 0;
    g_saved = false;
    mods::log::info("Cave of Shadows Record: the game's save opens");
}

void open_screen(dMw_c* window) {
    window->markMemSize();
    if (!confirm::preload() || !confirm::archive_has_record() || !heap_has_room(window) ||
        !g_screen.create(window->mpHeap, window->mpStick, g_input))
    {
        // Never a dead end: the record still counts, the save still follows (no amiibo question).
        mods::log::error("Cave of Shadows Record could not be built ({}); the record is kept and the save opens",
            confirm::problem());
        g_screen.destroy();
        store_record(g_input.cur);
        hand_over_to_save(window);
        return;
    }
    dMw_c::dMw_fade_in();
    g_phase = Phase::Open;
}

void finish_return() {
    g_phase = Phase::Idle;
    g_window = nullptr;
    const bool saved = g_saved;
    g_saved = false;
    cave::record_finished(saved);
}

// The record request could not be served at all (no idle window for seconds, or the screen dropped before its
// save): HD would have saved; return without the game save (the cave blob keeps the record in memory and is
// written with the next save).
void give_up(const char* why) {
    mods::log::warn("Cave of Shadows Record: {}; returning without the record screen's save", why);
    if (!g_recordStored) {
        store_record(g_input.cur);
    }
    g_phase = Phase::Idle;
    g_window = nullptr;
    g_saved = false;
    cave::record_finished(false);
}

void tick_host(dMw_c* window) {
    switch (g_phase) {
    case Phase::Idle:
    case Phase::Saving:
        break;
    case Phase::Requested:
        if (window_ready()) {
            g_window = window;
            open_window(window);
        }
        break;
    case Phase::FadingOut:
        if (fader_black() || ++g_phaseFrames > kFadeTimeoutFrames) {
            open_screen(window);
        }
        break;
    case Phase::Open:
        if (g_screen.move() == Choice::Finish) {
            dMw_c::dMw_fade_out();
            g_phase = Phase::FadingSave;
            g_phaseFrames = 0;
        }
        break;
    case Phase::FadingSave:
        if (fader_black() || ++g_phaseFrames > kFadeTimeoutFrames) {
            hand_over_to_save(window);
        }
        break;
    case Phase::Returning:
        finish_return();
        break;
    }
}

void drop_screen(dMw_c* window, const char* why) {
    if (g_screen.is_open()) {
        if (window != nullptr && window->mMenuProc == dMw_c::NO_MENU) {
            g_screen.destroy();
        } else {
            g_screen.abandon();
        }
    }
    if (g_phase != Phase::Idle) {
        mods::log::warn("Cave of Shadows Record closed: {}", why);
        g_phase = Phase::Idle;
        g_window = nullptr;
    }
}

// --- hooks ---------------------------------------------------------------------------------------

HookAction pre_key_wait_proc(ModContext*, void* args, void*, void*) {
    if (g_phase == Phase::Idle || g_phase == Phase::Saving) {
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (window == nullptr) {
        return HOOK_CONTINUE;
    }
    // From the request on, the idle process must not open another menu (Start, the ring): HD's idle state
    // takes the record state first (0x02a19214). The screen opens once window_ready() holds.
    JKRHeap* previous = mDoExt_setCurrentHeap(window->mpHeap);
    tick_host(window);
    mDoExt_setCurrentHeap(previous);
    return HOOK_SKIP_ORIGINAL;
}

void post_draw(ModContext*, void*, void*, void*) {
    if (g_screen.is_open() && dComIfGp_isPauseFlag() && dMeter2Info_getWindowStatus() == 10) {
        dComIfGd_set2DOpa(&g_screen);
    }
}

// GC collect_save_close_proc: at black, save status 3 (saved, or B in the log list, or a card error) goes back
// to the Collection screen, 4 closes the menu. The Record screen's save always closes it (HD: the record
// screen's every end saves and leaves; there is no Collection screen in this flow).
// FIX (HD review L7 / API review #1): the return (dComIfGs_gameStart) is issued right here, while the screen is
// still black, as HD does: its record exits call the save and gameStart (0x02a0cf38/0x02a0d1a4/0x02a0d778 ->
// 0x029076e4) before the menu's state 0x31 -> 0 (key_wait_init), which fades the cave back in (VERIFIED: HD
// key_wait_init 0x02a17118 takes prev state 0x31 to dMw_fade_in at 0x02a172b8, like GC's SAVE_CLOSE). The
// scene change's fade-out can only start once that fade-in has ended (JUTFader::startFadeOut needs status Wait,
// d_ovlp_fade.cpp:85), in HD as here; issuing gameStart a frame later only made it start a frame later.
void post_save_close_proc(ModContext*, void* args, void*, void*) {
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (g_phase != Phase::Saving || window == nullptr) {
        return;
    }
    if (window->mMenuProc != dMw_c::COLLECT_MOVE && window->mMenuProc != dMw_c::NO_MENU) {
        return;  // not at black yet (the original only moves on at fader status 0)
    }
    window->mMenuProc = dMw_c::NO_MENU;  // dMw_c::_execute runs key_wait_init(SAVE_CLOSE) next
    mods::log::info("Cave of Shadows Record: save menu closed ({})", g_saved ? "saved" : "not saved");
    finish_return();
}

HookAction pre_delete(ModContext*, void* args, void*, void*) {
    if (g_phase != Phase::Idle || g_screen.is_open()) {
        drop_screen(mods::arg<dMw_c*>(args, 0), "the pause menu was deleted");
    }
    return HOOK_CONTINUE;
}

HookOptions options(int priority) {
    HookOptions o = HOOK_OPTIONS_INIT;
    o.priority = priority;
    return o;
}

bool ok_or_record(ModResult result, const char* what) {
    if (result == MOD_OK) {
        return true;
    }
    mods::log::warn("hook '{}' not installed (result {})", what, static_cast<int>(result));
    cave::record_hook_failure(what);
    return false;
}

}  // namespace

// =================================================================================================
// Public API.
// =================================================================================================

void install_hooks() {
    const HookOptions high = options(1000);
    const HookOptions normal = options(0);
    bool ok = ok_or_record(mods::hook::add_pre<CosRecKeyWaitProc>(pre_key_wait_proc, &high),
        "Cave of Shadows Record (menu host)");
    ok = ok_or_record(mods::hook::add_post<CosRecMwDraw>(post_draw, &normal), "Cave of Shadows Record (draw)") && ok;
    ok = ok_or_record(mods::hook::add_pre<CosRecMwDelete>(pre_delete, &high), "Cave of Shadows Record (teardown)") &&
         ok;
    ok = ok_or_record(mods::hook::add_post<CosRecSaveCloseProc>(post_save_close_proc, &normal),
             "Cave of Shadows Record (save closes the menu)") &&
         ok;
    g_hostHooksOk = ok;
}

void uninstall_hooks() {
    // FIX (HD review L1 / API review #2): key_wait_proc, _draw and _delete are hooked by the stamp book too
    // (stamps/book_host.cpp BkMwKeyWaitProc), and HookService::uninstall drops every callback this mod has on a
    // target (dusk/mods/svc/hook.cpp hook_uninstall -> erase_callbacks): the loader removes those on teardown.
    // collect_save_close_proc is hooked only here.
    mods::hook::uninstall<CosRecSaveCloseProc>();
}

void shutdown() {
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    const bool hosting = g_phase == Phase::FadingOut || g_phase == Phase::Open || g_phase == Phase::FadingSave;
    if (g_screen.is_open()) {
        if (window != nullptr && window->mMenuProc == dMw_c::NO_MENU) {
            JKRHeap* previous = mDoExt_setCurrentHeap(window->mpHeap);
            g_screen.destroy();
            window->checkMemSize();
            mDoExt_setCurrentHeap(previous);
        } else {
            g_screen.abandon();
        }
    }
    if (hosting && window != nullptr && window->mMenuProc == dMw_c::NO_MENU) {
        // Give the window back like GC's key_wait_init (no menu was created).
        dMw_c::dMw_fade_in();
        window->dMw_capture_delete();
        dMeter2Info_setWindowStatus(0);
        dComIfGp_offHeapLockFlag(0);
    }
    g_phase = Phase::Idle;
    g_window = nullptr;
}

bool available() {
    return g_hostHooksOk && !confirm::archive_known_bad();
}

bool request(const Input& input) {
    if (!available() || g_phase != Phase::Idle) {
        return false;
    }
    g_input = input;
    g_phase = Phase::Requested;
    g_requestTicks = 0;
    g_saved = false;
    g_recordStored = false;
    mods::log::info("Cave of Shadows Record requested (floor {}, life {}/4, time {} ms, combo {}, damage {})",
        input.cur.floor, input.cur.life, input.cur.timeMs, input.cur.combo, input.cur.damage);
    return true;
}

bool busy() {
    return g_phase != Phase::Idle || g_screen.is_open();
}

void tick() {
    switch (g_phase) {
    case Phase::Idle:
        return;
    case Phase::Requested:
        if (++g_requestTicks > kRequestTimeoutTicks) {
            give_up("the pause-menu window never became idle");
        }
        return;
    case Phase::Saving: {
        dMw_c* window = dMeter2Info_getMenuWindowClass();
        if (window == nullptr) {
            give_up("the pause-menu window went away during the save");
            return;
        }
        if (window->mMenuProc == dMw_c::NO_MENU && window->mpMenuSave == nullptr) {
            // The save menu closed without the close hook seeing it (normally post_save_close_proc already sent
            // Link back): the next idle frame does it.
            g_phase = Phase::Returning;
            g_phaseFrames = 0;  // API review INFO: the 3-frame grace below counts from here
        } else if (++g_phaseFrames > kSaveTimeoutTicks) {
            give_up("the save menu did not close");
        }
        return;
    }
    case Phase::Returning:
        // Normally done in key_wait_proc; without it (another mod skipping the idle process) do it here.
        if (++g_phaseFrames > 3) {
            finish_return();
        }
        return;
    default: {
        dMw_c* window = dMeter2Info_getMenuWindowClass();
        if (window == nullptr || window->mMenuProc != dMw_c::NO_MENU) {
            // Something else moved the window on (another mod): never draw or delete freed objects.
            drop_screen(window, "the pause menu left its state from outside");
            give_up("the record screen was dropped");
        }
        return;
    }
    }
}

bool save_menu(const dMenu_save_c* menu) {
    if (g_phase != Phase::Saving || menu == nullptr) {
        return false;
    }
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    return window != nullptr && window->mpMenuSave == menu && menu->mUseType == dMenu_save_c::TYPE_DEFAULT;
}

void on_slot_written() {
    if (g_phase == Phase::Saving) {
        g_saved = true;
    }
}

void amiibo_closed(bool written) {
    g_screen.amiibo_closed(written);
}

}  // namespace cave::record
