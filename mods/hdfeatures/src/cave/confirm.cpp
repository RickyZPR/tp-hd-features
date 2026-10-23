// HD Features / Cave of Shadows: TP HD's pre-entry "Cave of Shadows" screen (see confirm.hpp).
//
// HD reference (Zelda.rpx, scratch cosconfirm/hdcode + hdlayout reports; VERIFIED unless noted):
//   dMw state 0x2C (0x02a17b04 / 0x02a1a07c): window status 10, Collection deleted, dMenu_save_c(6), fade
//     in and stream BGM 0x0200002F once it is built.
//   _open type 6 (0x02a0614c): header 413, Yes 414, No 415, cursor on Yes, A/B hints hidden, amiibo_n
//     hidden, intro counter 50; life > 0: a_title hidden; else w_mg_n hidden and a_title scales in
//     (bck 4000->4010).
//   proc 0x3D (0x02a0c7d0): no input; header y0 + min(counter-1, 20) * 190/20; at 0: A = 1036, B = 1017,
//     Yes/No in (bck 2109->2119), selected text white / other 969696, plate set + 4000->4010; both
//     animations done -> cursor, proc 0x3E. Animations advance 2 frames per tick.
//   proc 0x3E (0x02a0c9a0): Right Yes->No / Left No->Yes (0xA0 + 2D vibration, no wrap); A on Yes 0x6D,
//     A on No 0x6E (+ vibration), B 0x6E -> Cancel. Out: Yes/No 2099->2089, plate/title 4010->4000.
//   Save and begin -> proc 9: the normal save; Cancel -> proc 0x40: end 1, status 3 -> dMw 0x2E: fade
//     out, back to the Collection screen; the stream stops and the menu music comes back.
// INFERRED here: the selection-swap frames (2119->2137 to Cancel, 2139->2149 to Save: HD's keys, GC's
// table), the cursor parameters (GC's save menu yesnoCursorShow), the draw order, the fade between this
// screen and the stock GC save menu (HD stays on one screen), the music volume handling.

#include "stamps/prelude.hpp"  // library headers before any game header (see prelude.hpp)

#include "confirm.hpp"

#include "cave.hpp"
#include "hdtext.hpp"
#include "record.hpp"
#include "savescreen.hpp"
#include "hdfiles/files.hpp"

#include <mods/svc/hook.hpp>
#include <mods/svc/log.hpp>

#include "JSystem/J2DGraph/J2DAnmLoader.h"
#include "JSystem/J2DGraph/J2DAnimation.h"
#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JSupport/JSUList.h"
#include "JSystem/JUtility/JUTFader.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "d/d_lib.h"
#include "d/d_menu_collect.h"
#include "d/d_menu_save.h"
#include "d/d_menu_window.h"
#include "d/d_meter2_info.h"
#include "d/d_pane_class.h"
#include "d/d_select_cursor.h"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"
#include <dolphin/mtx.h>

// Hook targets: exported by dusklight.exe v2.0.3 and flagged 0x09 in its symbol manifest (code, no
// inline sites). dMw_c::collect_move_proc / _draw / _delete are hooked by the stamp book too
// (stamps/book_host.cpp, Bk*): the two hosts are never active at the same time, and their aliases
// differ, so HookService keeps one detour with both callbacks (see hooks.cpp).
//   dMw_c::collect_move_proc            pre (1000): skip while this screen or its fades own the window
//   dMw_c::_draw                        post: queue the screen at window status 10
//   dMw_c::_delete                      pre (1000): destroy the screen before the menu heap goes
//   dMenu_save_c::memCardDataSaveWait2  pre + post: HD's "Saved." ending for the cave trip's save
//   dMenu_save_c::saveEnd               post: after that save, HD's stage change from the menu (end status 2,
//                                       the menu held) or, without the hold, close the pause menu (status 4)
//   dMw_c::collect_save_move_proc       pre (1000) + post: HD dMw state 0x2D, the held "Saved." menu ignores
//                                       input and never closes (manifest flags 0x09; hooked only here)
DEFINE_HOOK(&dMw_c::collect_move_proc, CosMwCollectMoveProc);
DEFINE_HOOK(&dMw_c::_draw, CosMwDraw);
DEFINE_HOOK(&dMw_c::_delete, CosMwDelete);
DEFINE_HOOK(&dMenu_save_c::memCardDataSaveWait2, CosSaveDataSaveWait2);
DEFINE_HOOK(&dMenu_save_c::saveEnd, CosSaveEnd);
DEFINE_HOOK(&dMw_c::collect_save_move_proc, CosMwSaveMoveProc);

namespace cave::confirm {
namespace {

// =================================================================================================
// Assets: res/cave/confirm/cosconfirm_gc.arc (tools/pipeline/cosconfirm.py; manifest confirm.json).
// =================================================================================================

constexpr char kArchivePath[] = "cave/confirm/cosconfirm_gc.arc";
constexpr size_t kMaxArchiveSize = 16u * 1024u * 1024u;  // the pipeline keeps it below 16 MiB
constexpr size_t kAlignment = 32;

constexpr const char* kLayoutSave = "zelda_file_select2.blo";
constexpr const char* kLayoutInfo = "zelda_file_select_info_amiibo.blo";
constexpr const char* kLayoutHints = "zelda_file_select_icon_parts_revo.blo";
constexpr const char* kAnimation = "zelda_file_select2.bck";
// The "Cave of Shadows Record" screen's panel (record.cpp): optional, archives from before it lack it.
constexpr const char* kLayoutRecord = "zelda_file_select_los_record.blo";
// HD's tt_heart_00..03 under names no GC archive uses (the mounted archive is searched first by global
// name lookups, and GC's quest-log code looks tt_heart_00.bti up by name).
constexpr const char* kHeartTextures[4] = {
    "cos_heart_00.bti", "cos_heart_01.bti", "cos_heart_02.bti", "cos_heart_03.bti"};

struct Buffer {
    uint8_t* data = nullptr;
    size_t size = 0;
};

Buffer g_archive;
bool g_loadTried = false;
bool g_archiveReady = false;
const char* g_archiveProblem = "not loaded yet";
std::string g_archiveProblemText;  // owns g_archiveProblem when it comes from hdfiles::problem
int g_mounted = 0;
bool g_archiveHasRecord = false;  // validate_archive saw zelda_file_select_los_record.blo

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

// RARC as the pipeline writes it (and stamps/book_assets.cpp checks it): uncompressed, members 32-byte
// aligned, the three layouts, the animation and the four heart textures present and plausible.
bool validate_archive(const Buffer& blob, const char*& why) {
    const uint8_t* a = blob.data;
    const size_t size = blob.size;
    if (a == nullptr || size < 0x40 || std::memcmp(a, "RARC", 4) != 0) {
        why = "not an uncompressed RARC archive";
        return false;
    }
    if (be32(a + 0x08) != 0x20) {
        why = "unexpected RARC header size";
        return false;
    }
    const uint64_t dataBase = 0x20ull + be32(a + 0x0C);
    const uint32_t numNodes = be32(a + 0x20);
    const uint64_t nodes = 0x20ull + be32(a + 0x24);
    const uint32_t numEntries = be32(a + 0x28);
    const uint64_t entries = 0x20ull + be32(a + 0x2C);
    const uint32_t strSize = be32(a + 0x30);
    const uint64_t strings = 0x20ull + be32(a + 0x34);
    if (numNodes == 0 || numNodes > 0x100 || numEntries == 0 || numEntries > 0x1000 ||
        nodes + numNodes * 0x10ull > size || entries + numEntries * 0x14ull > size ||
        strings + strSize > size || dataBase > size)
    {
        why = "RARC tables out of range";
        return false;
    }
    constexpr size_t kRequired = 8;
    const std::array<std::string_view, kRequired> required = {kLayoutSave, kLayoutInfo, kLayoutHints, kAnimation,
        kHeartTextures[0], kHeartTextures[1], kHeartTextures[2], kHeartTextures[3]};
    std::array<bool, kRequired> found{};
    for (uint32_t i = 0; i < numEntries; ++i) {
        const uint8_t* e = a + entries + i * 0x14ull;
        const uint32_t typeAndName = be32(e + 4);
        if (((typeAndName >> 24) & 1u) == 0) {
            continue;  // directory entry
        }
        const uint32_t nameOffset = typeAndName & 0xFFFFFFu;
        const uint64_t fileStart = dataBase + be32(e + 8);
        const uint32_t fileSize = be32(e + 12);
        if (nameOffset >= strSize || fileStart + fileSize > size) {
            why = "RARC file entry out of range";
            return false;
        }
        if ((fileStart % kAlignment) != 0) {
            why = "RARC member data is not 32-byte aligned";
            return false;
        }
        const char* name = reinterpret_cast<const char*>(a + strings + nameOffset);
        const std::string_view entryName{name, strnlen(name, strSize - nameOffset)};
        if (entryName == kLayoutRecord) {
            // Optional (archives built before the Record screen lack it): only the Record screen needs it.
            g_archiveHasRecord = fileSize >= 0x40 && std::memcmp(a + fileStart, "SCRNblo", 7) == 0;
        }
        for (size_t k = 0; k < kRequired; ++k) {
            if (entryName != required[k]) {
                continue;
            }
            const uint8_t* member = a + fileStart;
            bool ok = fileSize >= 0x40;
            if (ok && k < 3) {
                ok = std::memcmp(member, "SCRNblo", 7) == 0;
            } else if (ok && k == 3) {
                ok = std::memcmp(member, "J3D1bck1", 8) == 0;
            } else if (ok) {
                ok = member[0] <= 0x0E && be32(member + 0x1C) >= 0x20 && be32(member + 0x1C) < fileSize;
            }
            if (!ok) {
                why = "a member of the archive is not what the screen expects";
                return false;
            }
            found[k] = true;
        }
    }
    for (bool f : found) {
        if (!f) {
            why = "a layout, the animation or a heart texture is missing from the archive";
            return false;
        }
    }
    return true;
}

bool load_archive() {
    std::vector<std::uint8_t> bytes;
    if (!hdfiles::load(kArchivePath, bytes)) {
        g_archiveProblemText = hdfiles::problem(kArchivePath);  // missing, didn't pass the check, can't be read
        g_archiveProblem = g_archiveProblemText.c_str();
        mods::log::warn("{}; the amiibo opens the save menu without HD's screen", g_archiveProblemText);
        return false;
    }
    bool ok = false;
    if (bytes.size() < 0x40 || bytes.size() > kMaxArchiveSize) {
        g_archiveProblem = "res/cave/confirm/cosconfirm_gc.arc has an unexpected size";
        mods::log::warn("res/{} has an unexpected size ({} bytes)", kArchivePath, bytes.size());
    } else {
        // JKRMemArchive and the GX textures need 32-byte alignment and a stable address for the session.
        void* copy = ::operator new(bytes.size(), std::align_val_t{kAlignment}, std::nothrow);
        if (copy == nullptr) {
            g_archiveProblem = "out of memory loading the screen's archive";
            mods::log::error("out of memory copying res/{} ({} bytes)", kArchivePath, bytes.size());
        } else {
            std::memcpy(copy, bytes.data(), bytes.size());
            Buffer candidate{static_cast<uint8_t*>(copy), bytes.size()};
            const char* why = "";
            if (validate_archive(candidate, why)) {
                g_archive = candidate;
                g_archiveProblem = "";
                ok = true;
            } else {
                g_archiveProblem = why;
                mods::log::error("res/{} is not usable ({})", kArchivePath, why);
                ::operator delete(copy, std::align_val_t{kAlignment});
            }
        }
    }
    return ok;
}

JKRArchive* mount_archive() {
    if (!g_archiveReady) {
        return nullptr;
    }
    // On the archive heap like the stamp book's (never freeAll'd), so it can always be unmounted.
    JKRHeap* heap = mDoExt_getArchiveHeap();
    if (heap == nullptr) {
        heap = mDoExt_getCurrentHeap();
    }
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4291)  // the JKR placement new has no matching placement delete
#endif
    auto* archive = JKR_NEW_ARGS(heap, 0)
        JKRMemArchive(g_archive.data, static_cast<u32>(g_archive.size), JKRMEMBREAK_FLAG_UNKNOWN0);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (archive == nullptr) {
        mods::log::error("could not allocate the Cave of Shadows screen's archive object");
        return nullptr;
    }
    if (!archive->isMounted()) {
        mods::log::error("mounting the Cave of Shadows screen's archive failed");
        JKR_DELETE(archive);
        return nullptr;
    }
    ++g_mounted;
    return archive;
}

void unmount_archive(JKRArchive*& archive) {
    if (archive == nullptr) {
        return;
    }
    // Mount count 1 after construction: unmount() deletes the object and takes it off the volume list.
    // The buffer is never freed (JKRMEMBREAK_FLAG_UNKNOWN0).
    archive->unmount();
    archive = nullptr;
    if (g_mounted > 0) {
        --g_mounted;
    }
}

// Shared J2D helpers (pane lookup, texts, GC messages, Anim): savescreen.hpp.
using namespace cave::savescreen;

// =================================================================================================
// Music (HD state 0x2C / 0x2D).
// =================================================================================================

constexpr u32 kStream = 0x0200002F;  // AudioRes/Stream/demo19_02.ast (same index in GC's Z2Sound.baa)
constexpr u32 kNoStream = 0xFFFFFFFFu;
constexpr u32 kStreamFadeFrames = 30;
constexpr u32 kSceneBgmFadeFrames = 13;  // the scene music leaves with the screen's 13-frame fade-in
constexpr int kVolumeDelayTicks = 15;
u32 g_savedStream = kNoStream;
bool g_streamOurs = false;
int g_volumePending = 0;  // ticks until the stream gets the normal (not the pause menu's) volume

void bgm_open() {
    Z2AudioMgr* audio = Z2AudioMgr::getInterface();
    if (audio == nullptr) {
        return;
    }
    // HD saves the playing stream (if any) and plays its own (VERIFIED calls); INFERRED: it plays at
    // the normal volume, not the pause menu's (HD's 0x02c209d8(1), volume restored on Cancel). The
    // scene music (main and sub BGM) is taken out with the fade-in (Z2SeqMgr's stream master, which
    // bgmStreamPrepare would fade over 60 frames) before the volume goes back up, so it never gets
    // louder on the way out.
    g_savedStream = audio->getStreamBgmID();
    audio->bgmStreamPrepare(kStream);
    audio->bgmStreamPlay();
    audio->mStreamBgmMaster.fadeOut(kSceneBgmFadeFrames);
    g_volumePending = kVolumeDelayTicks;
    g_streamOurs = true;
}

void bgm_tick() {
    if (g_volumePending > 0 && --g_volumePending == 0 && g_streamOurs) {
        if (Z2AudioMgr* audio = Z2AudioMgr::getInterface(); audio != nullptr) {
            audio->menuOutBgm();
        }
    }
}

void bgm_cancel(bool backOnCollection) {
    if (!g_streamOurs) {
        return;
    }
    g_streamOurs = false;
    g_volumePending = 0;
    Z2AudioMgr* audio = Z2AudioMgr::getInterface();
    if (audio == nullptr) {
        return;
    }
    if (audio->getStreamBgmID() == kStream) {
        audio->bgmStreamStop(kStreamFadeFrames);
    }
    if (g_savedStream != kNoStream && g_savedStream != kStream) {
        audio->bgmStreamPrepare(g_savedStream);
        audio->bgmStreamPlay();
    }
    g_savedStream = kNoStream;
    if (backOnCollection) {
        audio->menuInBgm();  // the pause menu's own volume again
    }
}

// =================================================================================================
// The screen.
// =================================================================================================

enum class Choice : uint8_t { None, Save, Cancel };
enum Sel : int { kSave = 0, kCancel = 1 };

// HD frame numbers in zelda_file_select2.bck (cos_entry_anim_keys.json).
constexpr int kFrameSetup = 1;
constexpr int kFrameButtonsInFrom = 2109;
constexpr int kFrameButtonsInTo = 2119;
constexpr int kFrameButtonsOutFrom = 2099;
constexpr int kFrameButtonsOutTo = 2089;
constexpr int kFrameToCancelFrom = 2119;
constexpr int kFrameToCancelTo = 2137;
constexpr int kFrameToSaveFrom = 2139;
constexpr int kFrameToSaveTo = 2149;
constexpr int kFramePlateHidden = 4000;
constexpr int kFramePlateShown = 4010;

constexpr int kIntroTicks = 50;       // HD +0x251
constexpr float kHeaderDrop = 190.0f; // HD: the header starts 190 units low
constexpr int kHeaderSlideTicks = 20; // ... and slides up over the last 20 ticks
constexpr int kHighlightFadeTicks = 5;  // GC g_msHIO.mSelectFrames (HD uses the same save menu)
constexpr int kHearts = 20;

const JUtility::TColor kTextSelected(255, 255, 255, 255);
const JUtility::TColor kTextOther(150, 150, 150, 255);

class Screen final : public dDlst_base_c {
public:
    bool create(JKRHeap* heap, STControl* stick, const AmiiboData& data);
    void destroy();
    void abandon();
    bool is_open() const { return mOpen; }
    bool cancel_now();
    Choice move();
    void draw() override;
    static s32 last_heap_use() { return sLastHeapUse; }

private:
    enum class Step : uint8_t { Intro, Enter, Select, Swap, Out, Done };

    bool build();
    bool lookup();
    void setup(const AmiiboData& data);
    void set_plate(const AmiiboData& data);
    void set_header_offset(float offset);
    void intro();
    void intro_end();
    void enter();
    void select();
    void start_swap(int to);
    void swap();
    void start_out(Choice choice);
    void out();
    void cursor_show();
    void set_selection_look(int selected);
    J2DPane* button(int sel) const { return sel == kSave ? mYes : mNo; }
    J2DPane* highlight(int sel) const { return sel == kSave ? mYesHi : mNoHi; }
    J2DTextBox* label(int sel) const { return sel == kSave ? mYesText : mNoText; }
    void forget();
    void release_font();

    static inline s32 sLastHeapUse = 0;

    bool mOpen = false;
    bool mFontRef = false;
    JKRHeap* mHeap = nullptr;
    STControl* mStick = nullptr;
    JKRArchive* mArchive = nullptr;

    J2DScreen* mSave = nullptr;   // zelda_file_select2.blo
    J2DScreen* mInfo = nullptr;   // zelda_file_select_info_amiibo.blo
    J2DScreen* mHints = nullptr;  // zelda_file_select_icon_parts_revo.blo
    dSelect_cursor_c* mCursor = nullptr;
    J2DAnmTransform* mAnmButtons = nullptr;
    J2DAnmTransform* mAnmPlate = nullptr;

    J2DPane* mNm02 = nullptr;
    J2DPane* mHeader = nullptr;   // w_mg_n (title bar with t_for), data case
    J2DPane* mTitle = nullptr;    // a_title (centred bar with t_for2), no-data case
    J2DPane* mPlate = nullptr;    // amiibo_n
    J2DPane* mYes = nullptr;      // w_yes_n
    J2DPane* mNo = nullptr;       // w_no_n
    J2DPane* mYesHi = nullptr;    // w_yes_m (button2)
    J2DPane* mNoHi = nullptr;     // w_no_m
    J2DTextBox* mYesText = nullptr;
    J2DTextBox* mNoText = nullptr;
    J2DTextBox* mHeaderText = nullptr;   // t_for
    J2DTextBox* mHeaderText1 = nullptr;  // t_for1 (HD's cross-fade partner, alpha 0)
    J2DTextBox* mTitleText = nullptr;    // t_for2
    J2DPane* mInfoNm02 = nullptr;
    J2DPane* mInfoDat = nullptr;  // w_dat_i1 (474 x 86): mapped onto amiibo_n (420 x 85)
    std::array<J2DPicture*, kHearts> mHeart{};
    std::array<J2DPicture*, kHearts> mHeartBase{};
    J2DTextBox* mLevelLabel = nullptr;
    J2DTextBox* mLevelValue = nullptr;
    J2DPane* mABtn = nullptr;
    J2DPane* mBBtn = nullptr;
    std::array<J2DTextBox*, 4> mHintText{};  // cont_at, cont_at1, cont_bt, cont_bt1
    std::array<const ResTIMG*, 4> mHeartTex{};
    std::vector<J2DPane*> mN43;
    std::vector<std::pair<f32, f32>> mN43Saved;

    Anim mButtons;
    Anim mPlateAnim;
    Step mStep = Step::Intro;
    Choice mChoice = Choice::None;
    bool mHasData = false;
    bool mPlateDrawn = false;
    int mCounter = 0;
    int mSel = kSave;
    int mPrev = kCancel;
    int mFadeTicks = 0;
    f32 mHeaderX0 = 0.0f;
    f32 mHeaderY0 = 0.0f;
};

bool Screen::create(JKRHeap* heap, STControl* stick, const AmiiboData& data) {
    if (mOpen) {
        destroy();
    }
    if (heap == nullptr) {
        return false;
    }
    mHeap = heap;
    mStick = stick;
    JKRHeap* previous = mDoExt_setCurrentHeap(heap);
    const s32 freeBefore = heap->getTotalFreeSize();
    const bool ok = build() && lookup();
    if (ok) {
        setup(data);
        mOpen = true;
    }
    const s32 freeAfter = heap->getTotalFreeSize();
    mDoExt_setCurrentHeap(previous);
    if (!ok) {
        destroy();
        return false;
    }
    sLastHeapUse = freeBefore - freeAfter;
    mods::log::info("Cave of Shadows screen opened (level {}, life {}/4, max life {}/5): 2D heap {} -> {} ({} used)",
        data.level, data.life, data.maxLife, freeBefore, freeAfter, sLastHeapUse);
    return true;
}

bool Screen::build() {
    // GC-side pieces before the archive is mounted (a mounted JKRMemArchive is searched first).
    // The save menu's own cursor: GC dMenu_save_c::screenSet makes dSelect_cursor_c(0, 1.0f, main2D).
    mCursor = JKR_NEW dSelect_cursor_c(0, 1.0f, nullptr);
    if (mCursor == nullptr) {
        return false;
    }
    mCursor->setAlphaRate(0.0f);

    mArchive = mount_archive();
    if (mArchive == nullptr) {
        return false;
    }
    const std::array<std::pair<J2DScreen**, const char*>, 3> layouts = {{
        {&mSave, kLayoutSave}, {&mInfo, kLayoutInfo}, {&mHints, kLayoutHints}}};
    for (const auto& [slot, name] : layouts) {
        auto* screen = JKR_NEW J2DScreen();
        if (screen == nullptr) {
            mods::log::error("Cave of Shadows screen: out of 2D heap building {}", name);
            return false;
        }
        *slot = screen;
        if (!screen->setPriority(name, kSetPriorityFlags, mArchive)) {
            mods::log::error("Cave of Shadows screen: {} could not be built", name);
            return false;
        }
        dPaneClass_showNullPane(screen);  // as GC's/HD's save menu screenSet
    }
    void* bck = mArchive->getResource(0, kAnimation);
    if (bck == nullptr) {
        mods::log::error("Cave of Shadows screen: {} missing", kAnimation);
        return false;
    }
    J2DAnmBase* a = J2DAnmLoaderDataBase::load(bck);
    J2DAnmBase* b = J2DAnmLoaderDataBase::load(bck);
    mAnmButtons = static_cast<J2DAnmTransform*>(a);
    mAnmPlate = static_cast<J2DAnmTransform*>(b);
    if (a == nullptr || b == nullptr || a->getKind() != KIND_TRANSFORM || b->getKind() != KIND_TRANSFORM) {
        mods::log::error("Cave of Shadows screen: {} is not a transform animation", kAnimation);
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        mHeartTex[i] = static_cast<const ResTIMG*>(mArchive->getResource('TIMG', kHeartTextures[i]));
        if (mHeartTex[i] == nullptr) {
            mods::log::error("Cave of Shadows screen: {} missing", kHeartTextures[i]);
            return false;
        }
    }
    return true;
}

bool Screen::lookup() {
    mNm02 = find(mSave, "Nm_02");
    mHeader = find(mSave, "w_mg_n");
    mTitle = find(mSave, "a_title");
    mPlate = find(mSave, "amiibo_n");
    mYes = find(mSave, "w_yes_n");
    mNo = find(mSave, "w_no_n");
    mYesHi = find_as<J2DPicture>(mSave, "w_yes_m");
    mNoHi = find_as<J2DPicture>(mSave, "w_no_m");
    mYesText = find_as<J2DTextBox>(mSave, "f_yes_t");
    mNoText = find_as<J2DTextBox>(mSave, "f_no_t");
    mHeaderText = find_as<J2DTextBox>(mSave, "t_for");
    mHeaderText1 = find_as<J2DTextBox>(mSave, "t_for1", false);
    mTitleText = find_as<J2DTextBox>(mSave, "t_for2", false);
    mInfoNm02 = find(mInfo, "Nm_02");
    mInfoDat = find(mInfo, "w_dat_i1");
    bool ok = mNm02 != nullptr && mHeader != nullptr && mTitle != nullptr && mPlate != nullptr && mYes != nullptr &&
              mNo != nullptr && mYesHi != nullptr && mNoHi != nullptr && mYesText != nullptr && mNoText != nullptr &&
              mHeaderText != nullptr && mInfoNm02 != nullptr && mInfoDat != nullptr;
    for (int i = 0; i < kHearts; ++i) {
        mHeart[i] = find_as<J2DPicture>(mInfo, fmt::format("hear_{}", 20 + i));
        mHeartBase[i] = find_as<J2DPicture>(mInfo, fmt::format("heab_{}", 20 + i));
        ok = ok && mHeart[i] != nullptr && mHeartBase[i] != nullptr;
    }
    mLevelLabel = find_as<J2DTextBox>(mInfo, "level_l");
    mLevelValue = find_as<J2DTextBox>(mInfo, "level_v");
    mABtn = find(mHints, "abtn_n");
    mBBtn = find(mHints, "bbtn_n");
    mHintText = {find_as<J2DTextBox>(mHints, "cont_at"), find_as<J2DTextBox>(mHints, "cont_at1", false),
        find_as<J2DTextBox>(mHints, "cont_bt"), find_as<J2DTextBox>(mHints, "cont_bt1", false)};
    ok = ok && mLevelLabel != nullptr && mLevelValue != nullptr && mABtn != nullptr && mBBtn != nullptr &&
         mHintText[0] != nullptr && mHintText[2] != nullptr;
    if (ok && (mInfoDat->getWidth() <= 0.0f || mInfoDat->getHeight() <= 0.0f)) {
        mods::log::error("Cave of Shadows screen: w_dat_i1 has no size");
        ok = false;
    }
    return ok;
}

void Screen::setup(const AmiiboData& data) {
    // Frame 1 on the Nm_02 subtree (GC/HD screenSet): Nm_02 at 0.95, background and title bar in place,
    // the save slots, the GC hints, the popups and the buttons off-screen or at scale 0.
    mButtons.anm = mAnmButtons;
    mPlateAnim.anm = mAnmPlate;
    mNm02->setAnimation(mAnmPlate);
    mAnmPlate->setFrame(static_cast<f32>(kFrameSetup));
    mNm02->animationTransform();
    mNm02->setAnimation(static_cast<J2DAnmTransform*>(nullptr));

    // Panes HD's code hides or empties for this screen (JP-only texts and decorations, the Wii hints).
    for (const char* name : {"w_tabi_s", "w_tabi_x", "w_tabi_1", "w_yes_t", "w_no_t", "Wi_btn_n"}) {
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

    // Fonts: every text box names rodan_24_22.bfn, which no archive has; HD sets the message font on
    // the ones it shows (t_for, t_for1, t_for2, f_yes_t, f_no_t, level_l, level_v). The hints name
    // rodan_b_24_22.bfn, the same message font.
    JUTFont* font = mDoExt_getMesgFont();
    mFontRef = true;
    if (font != nullptr) {
        for (J2DTextBox* box : {mHeaderText, mHeaderText1, mTitleText, mYesText, mNoText, mLevelLabel, mLevelValue,
                 mHintText[0], mHintText[1], mHintText[2], mHintText[3]})
        {
            if (box != nullptr) {
                box->setFont(font);
            }
        }
    }
    // HD header: 27x27, line space 20, character space 0 (t_for) / 2 (t_for1, t_for2).
    for (J2DTextBox* box : {mHeaderText, mHeaderText1, mTitleText}) {
        if (box != nullptr) {
            box->setFontSize(27.0f, 27.0f);
            box->setLineSpace(20.0f);
            box->setCharSpace(box == mHeaderText ? 0.0f : 2.0f);
        }
    }

    // Texts (_open type 6): header 413, Yes 414, No 415.
    const std::string title = hd_text(413, "Cave of Shadows");
    set_text(mHeaderText, title, mHeap);
    set_text(mHeaderText1, "", mHeap);
    if (mHeaderText1 != nullptr) {
        mHeaderText1->setAlpha(0);
    }
    mHeaderText->setAlpha(255);
    set_text(mTitleText, title, mHeap);
    set_text(mYesText, hd_text(414, "Save and enter"), mHeap);
    set_text(mNoText, hd_text(415, "Cancel"), mHeap);
    mYesHi->setAlpha(0);
    mNoHi->setAlpha(0);

    // Hints hidden until the intro ends.
    mABtn->hide();
    mBBtn->hide();

    // Info screen (dFile_amiibo_c screen setup 0x0294a1f8).
    for (const char* name : {"t_w_02", "t_w_01", "w_hm", "hear_01", "hear_02", "heab_02", "t_w_04"}) {
        hide(mInfo, name);
    }
    for (const char* name : {"w_htdiff", "t_newrec", "w_htdif1"}) {
        set_text(find_as<J2DTextBox>(mInfo, name, false), "", mHeap);
    }
    set_plate(data);

    mHasData = data.life > 0;
    mPlate->hide();  // shown with its animation when the intro ends (data case)
    mPlateDrawn = false;
    mHeaderX0 = mHeader->getTranslateX();
    mHeaderY0 = mHeader->getTranslateY();
    if (mHasData) {
        mTitle->hide();
        mHeader->show();
        set_header_offset(kHeaderDrop);
    } else {
        // No data: the centred bar scales in (bck 4000 -> 4010). FIX (review L5): t_for2 stays hidden, as in
        // HD. It is HIDDEN in HD's layout (VERIFIED, blo dump), and HD only writes its text (headerTxtSet
        // 0x02a05bec) and calls setAlpha(0xFF) on its pane manager (screenSet 0x02a04a34), which never
        // shows a pane; no HD code shows it (VERIFIED: the only +0x194 uses). So HD draws the bar without
        // a title (INFERRED from the code; not checked against footage of a first scan).
        mHeader->hide();
        mTitle->show();
        mPlateAnim.start(kFramePlateHidden, kFramePlateShown, mTitle);
    }

    collect_n43(mSave, mN43);
    collect_n43(mHints, mN43);
    mN43Saved.assign(mN43.size(), {1.0f, 1.0f});

    mStep = Step::Intro;
    mChoice = Choice::None;
    mCounter = kIntroTicks;
    mSel = kSave;  // HD: the cursor starts on Yes ("Save and begin")
    mPrev = kCancel;
}

void Screen::set_plate(const AmiiboData& data) {
    // dFile_amiibo_c::setData hearts (0x0294a718): hear_(20+i) for i < ceil(life/4), the last one 1/4,
    // 2/4 or 3/4 when life % 4 is 1, 2 or 3; heab_(20+i) for i < maxLife/5.
    const int life = std::max(data.life, 0);
    const int filled = std::min((life + 3) / 4, kHearts);
    const int part = life % 4;
    const int containers = std::clamp(data.maxLife / 5, 0, kHearts);
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
    // Level (0x0294aa8c): <= 0 hides the label and empties the value.
    if (data.level <= 0) {
        mLevelLabel->hide();
        set_text(mLevelValue, "", mHeap);
    } else {
        mLevelLabel->show();
        set_text(mLevelLabel, hd_text(1260, "Level"), mHeap);
        set_text(mLevelValue, format_number(hd_text(855, "B%d"), data.level), mHeap);
    }
}

void Screen::set_header_offset(float offset) {
    mHeader->translate(mHeaderX0, mHeaderY0 + offset);
}

void Screen::set_selection_look(int selected) {
    label(selected)->setWhite(kTextSelected);
    label(selected ^ 1)->setWhite(kTextOther);
}

void Screen::cursor_show() {
    // GC dMenu_save_c::yesnoCursorShow: white text, highlight, cursor on the button.
    label(mSel)->setWhite(kTextSelected);
    highlight(mSel)->setAlpha(255);
    mCursor->setPos(0.0f, 0.0f, button(mSel), true);
    mCursor->setAlphaRate(1.0f);
    mCursor->setParam(0.96f, 0.84f, 0.06f, 0.5f, 0.5f);
}

void Screen::intro() {
    // HD proc 0x3D: 50 ticks without input; the data case's header slides up over the last 20.
    if (!mHasData) {
        mPlateAnim.step();
    }
    if (mCounter > 0) {
        // FIX (review L6): HD computes the offset from the counter before decrementing it
        // (0x02a0c848-0x02a0c8c8): min(counter - 1, 20) * 190 / 20, then counter - 1.
        if (mHasData) {
            const int steps = std::clamp(mCounter - 1, 0, kHeaderSlideTicks);
            set_header_offset(static_cast<float>(steps) * kHeaderDrop / kHeaderSlideTicks);
        }
        --mCounter;
    }
    if (mCounter == 0) {
        intro_end();
    }
}

void Screen::intro_end() {
    // A = 1036 "Confirm", B = 1017 "Back" (the game's own labels in its language).
    set_message(mHintText[0], 1036, "Confirm", mHeap);
    set_message(mHintText[1], 1036, "Confirm", mHeap);
    set_message(mHintText[2], 1017, "Back", mHeap);
    set_message(mHintText[3], 1017, "Back", mHeap);
    mABtn->show();
    mBBtn->show();
    if (mHasData) {
        set_header_offset(0.0f);
    }
    mButtons.start(kFrameButtonsInFrom, kFrameButtonsInTo, mYes, mNo);
    set_selection_look(mSel);
    if (mHasData) {
        mPlate->show();
        mPlateDrawn = true;
        mPlateAnim.start(kFramePlateHidden, kFramePlateShown, mPlate);
    }
    mStep = Step::Enter;
}

void Screen::enter() {
    const bool buttons = mButtons.step();
    const bool plate = mPlateAnim.step();
    if (buttons && plate) {
        cursor_show();
        mStep = Step::Select;
    }
}

void Screen::select() {
    // HD proc 0x3E / YesNoSelect. Input only while the fader is idle (it is: the screen faded in).
    if (!fader_idle()) {
        return;
    }
    if (mDoCPd_c::getTrigA(PAD_1) != 0) {
        if (mSel == kSave) {
            se(Z2SE_SY_CURSOR_OK);
            dMeter2Info_set2DVibration();
            start_out(Choice::Save);
        } else {
            se(Z2SE_SY_CURSOR_CANCEL);
            dMeter2Info_set2DVibration();
            start_out(Choice::Cancel);
        }
        return;
    }
    if (mDoCPd_c::getTrigB(PAD_1) != 0) {
        se(Z2SE_SY_CURSOR_CANCEL);
        start_out(Choice::Cancel);
        return;
    }
    if (mStick == nullptr) {
        return;
    }
    if (mStick->checkRightTrigger()) {
        if (mSel == kSave) {
            start_swap(kCancel);
        }
    } else if (mStick->checkLeftTrigger()) {
        if (mSel == kCancel) {
            start_swap(kSave);
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
    if (to == kCancel) {
        mButtons.start(kFrameToCancelFrom, kFrameToCancelTo, mYes, mNo);
    } else {
        mButtons.start(kFrameToSaveFrom, kFrameToSaveTo, mYes, mNo);
    }
    mStep = Step::Swap;
}

void Screen::swap() {
    // GC yesNoCursorMoveAnm: the buttons' scale swap plus the previous button's highlight and text
    // fading to the unselected look over 5 ticks; then the cursor on the new button.
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

void Screen::start_out(Choice choice) {
    // HD: the buttons slide out (2099 -> 2089) and the plate / centred title shrink (4010 -> 4000).
    mChoice = choice;
    mCursor->setAlphaRate(0.0f);
    mButtons.start(kFrameButtonsOutFrom, kFrameButtonsOutTo, mYes, mNo);
    if (mHasData) {
        mPlateAnim.start(kFramePlateShown, kFramePlateHidden, mPlate);
    } else {
        mPlateAnim.start(kFramePlateShown, kFramePlateHidden, mTitle);
    }
    mStep = Step::Out;
}

void Screen::out() {
    const bool buttons = mButtons.step();
    const bool plate = mPlateAnim.step();
    if (buttons && plate) {
        if (!mHasData) {
            mTitle->hide();  // HD proc 0x3F hides a_title if it was used
        }
        mStep = Step::Done;
    }
}

// Leave as Cancel without the player (the switch is being turned off). Only once the buttons take input:
// the intro, the slide-in and a selection swap finish first. True when the out-animation started.
bool Screen::cancel_now() {
    if (!mOpen || mStep != Step::Select) {
        return false;
    }
    se(Z2SE_SY_CURSOR_CANCEL);
    start_out(Choice::Cancel);
    return true;
}

Choice Screen::move() {
    if (!mOpen) {
        return Choice::Cancel;
    }
    switch (mStep) {
    case Step::Intro:
        intro();
        break;
    case Step::Enter:
        enter();
        break;
    case Step::Select:
        select();
        break;
    case Step::Swap:
        swap();
        break;
    case Step::Out:
        out();
        break;
    case Step::Done:
        break;
    }
    return mStep == Step::Done ? mChoice : Choice::None;
}

void Screen::draw() {
    if (!mOpen) {
        return;
    }
    // HD stretches the save screen with the canvas and keeps its "n_43" panes 4:3, like GC's own save
    // menu does (dMenu_save_c::menuSaveWide); the info screen follows amiibo_n's matrix.
    const bool wide = mDoGph_gInf_c::hudAspectScaleUp > 1.0001f;
    const f32 up = wide ? mDoGph_gInf_c::hudAspectScaleUp : 1.0f;
    const f32 down = wide ? mDoGph_gInf_c::hudAspectScaleDown : 1.0f;
    const f32 left = wide ? mDoGph_gInf_c::getSafeMinXF() : 0.0f;
    for (J2DScreen* screen : {mSave, mHints}) {
        screen->scale(up, 1.0f);
        screen->translate(left, 0.0f);
    }
    for (size_t i = 0; i < mN43.size(); ++i) {
        J2DPane* pane = mN43[i];
        mN43Saved[i] = {pane->getScaleX(), pane->getScaleY()};
        pane->scale(mN43Saved[i].first * down, mN43Saved[i].second);
    }

    J2DGrafContext* port = dComIfGp_getCurrentGrafPort();
    port->setup2D();
    mSave->draw(0.0f, 0.0f, port);
    if (mPlateDrawn && mPlate->isVisible()) {
        // dDlst_FileInfo_c::draw / HD 0x0294a108: the info screen's Nm_02 = amiibo_n's global matrix x
        // Scale(amiibo_n size / w_dat_i1 size).
        Mtx scale;
        Mtx placed;
        MTXScale(scale, mPlate->getWidth() / mInfoDat->getWidth(), mPlate->getHeight() / mInfoDat->getHeight(), 1.0f);
        MTXConcat(mPlate->getGlbMtx(), scale, placed);
        mInfoNm02->setMtx(placed);
        port->setup2D();
        mInfo->draw(0.0f, 0.0f, port);
    }
    port->setup2D();
    mHints->draw(0.0f, 0.0f, port);
    mCursor->refreshAspectScale(up);
    mCursor->draw();

    for (size_t i = 0; i < mN43.size(); ++i) {
        mN43[i]->scale(mN43Saved[i].first, mN43Saved[i].second);
    }
}

void Screen::destroy() {
    for (J2DPane* pane : {mYes, mNo, mPlate, mTitle, mNm02}) {
        if (pane != nullptr) {
            pane->setAnimation(static_cast<J2DAnmTransform*>(nullptr));
        }
    }
    if (mCursor != nullptr) {
        JKR_DELETE(mCursor);
        mCursor = nullptr;
    }
    for (J2DScreen** screen : {&mSave, &mInfo, &mHints}) {
        if (*screen != nullptr) {
            JKR_DELETE(*screen);
            *screen = nullptr;
        }
    }
    if (mAnmButtons != nullptr) {
        JKR_DELETE(mAnmButtons);
        mAnmButtons = nullptr;
    }
    if (mAnmPlate != nullptr) {
        JKR_DELETE(mAnmPlate);
        mAnmPlate = nullptr;
    }
    unmount_archive(mArchive);
    release_font();  // after the text boxes that used it are gone
    forget();
}

void Screen::abandon() {
    // The 2D heap was wiped under the screen (the pause menu left its state from outside): forget every
    // pointer into it; the archive object lives on the archive heap and still has to be unmounted.
    if (mOpen || mCursor != nullptr || mSave != nullptr) {
        mods::log::warn("Cave of Shadows screen: the pause menu was taken away under it; dropping it without destructors");
    }
    mCursor = nullptr;
    mSave = mInfo = mHints = nullptr;
    mAnmButtons = mAnmPlate = nullptr;
    unmount_archive(mArchive);
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
    mNm02 = mHeader = mTitle = mPlate = mYes = mNo = mYesHi = mNoHi = nullptr;
    mYesText = mNoText = mHeaderText = mHeaderText1 = mTitleText = nullptr;
    mInfoNm02 = mInfoDat = nullptr;
    mHeart.fill(nullptr);
    mHeartBase.fill(nullptr);
    mLevelLabel = mLevelValue = nullptr;
    mABtn = mBBtn = nullptr;
    mHintText.fill(nullptr);
    mHeartTex.fill(nullptr);
    mN43.clear();
    mN43Saved.clear();
    mButtons.reset();
    mPlateAnim.reset();
    mOpen = false;
    mPlateDrawn = false;
    mHeap = nullptr;
    mStick = nullptr;
}

// =================================================================================================
// Hosting in the pause menu window (stamps/book_host.cpp's Collection sub-window pattern).
// =================================================================================================

enum class Phase : uint8_t {
    Idle,
    Requested,      // the scan succeeded on the Collection screen (HD: sub-window 9)
    FadingOut,      // HD collect_close_init(9): no sound, fade out
    Open,           // HD state 0x2C/0x2D with dMenu_save_c type 6
    FadingCancel,   // HD state 0x2E: fade out, then the Collection screen again
    FadingSave,     // fade out, then the game's own pause-menu save (dMw SAVE_OPEN)
};

// The Collection screen's sub-window value while this screen is requested (HD's own number; GC uses
// 1-6, the stamp book 7): other hooks on that screen see it as busy.
constexpr u8 kSubWindowCave = 9;
constexpr int kFadeTimeoutFrames = 60;
// 2D heap: about GC's own save menu (378 panes / 310 materials against its 397 / 335, assets report).
// A failed allocation stops the game on PC, so this check is the only guard. FIX (review API #2): the
// stamp book's floors were too low for this screen: its largest single block is zelda_file_select2.blo's
// material array (248 x ~208 bytes, about 52 KB) and the whole screen is estimated at 350-450 KB. With
// the Collection screen deleted the 2D heap has about 1.5 MB free (VERIFIED in test logs: the
// Collection-hosted stamp book opens at 1535544 bytes free), so these floors cost nothing in practice.
// After one opening: the measured use + a margin (never below the floor).
constexpr s32 kFirstOpenFreeHeap = 0x80000;
constexpr s32 kHeapMargin = 0x20000;
constexpr s32 kMinHeapFloor = 0x60000;
constexpr s32 kMinFreeBlock = 0x20000;

Screen g_screen;
Phase g_phase = Phase::Idle;
int g_phaseFrames = 0;
bool g_cancelRequested = false;  // request_cancel(): leave as Cancel as soon as the buttons take input
AmiiboData g_data;
bool g_hostHooksOk = false;
bool g_saveHooksOk = false;
bool g_holdHookOk = false;              // dMw_c::collect_save_move_proc pre/post + dMw_c::_delete pre (HD's held menu)
dMenu_save_c* g_retypedMenu = nullptr;  // the save menu whose use type is swapped for one call
dMenu_save_c* g_heldMenu = nullptr;     // HD dMw state 0x2D: the trip's save menu, held while the stage changes

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
    mods::log::error("Cave of Shadows screen: 2D heap has {} bytes free (largest block {}), {} needed",
        freeSize, freeBlock, required_free());
    return false;
}

void restore_collection(dMw_c* window) {
    // HD collect_move_init(0x2E) / GC collect_move_init(SAVE_CLOSE): fade in, delete the sub-window,
    // window status 3, the Collection screen again (its cursor comes back from dMeter2Info).
    dMw_c::dMw_fade_in();
    g_screen.destroy();
    window->checkMemSize();
    window->field_0x144 = 3;
    dMeter2Info_setWindowStatus(3);
    if (window->mpMenuCollect == nullptr) {
        window->dMw_collect_create();
    }
}

void hand_over_to_save(dMw_c* window) {
    // At black: the game's own Collection -> Save path from SAVE_OPEN on (collect_save_open_init:
    // window status 10, dMenu_save_c type 1 on saveres.arc; collect_save_open_proc fades in once it is
    // built). The rest of the trip is cave.cpp's (EntryStep::Saving).
    g_screen.destroy();
    window->checkMemSize();
    window->mMenuProc = dMw_c::SAVE_OPEN;  // dMw_c::_execute runs its init after this move proc
    g_phase = Phase::Idle;
    g_cancelRequested = false;
    cave::confirm_finished(true);
}

void open_screen(dMw_c* window) {
    // HD state 0x2C at black: window status 10, the Collection screen deleted, the screen built.
    window->field_0x144 = 10;
    dMeter2Info_setWindowStatus(10);
    window->dMw_collect_delete(true);
    window->markMemSize();
    // FIX (review L2): the archive (about 11 MB) is read here the first time, while the screen is black,
    // instead of on the Z press: the one-time hitch is hidden by the fade.
    if (!preload() || !heap_has_room(window) || !g_screen.create(window->mpHeap, window->mpStick, g_data)) {
        // Never a dead end: the stock save menu still saves before the trip.
        mods::log::error("Cave of Shadows screen could not be built; opening the save menu directly");
        hand_over_to_save(window);
        return;
    }
    bgm_open();
    dMw_c::dMw_fade_in();
    g_phase = Phase::Open;
}

void tick_host(dMw_c* window) {
    switch (g_phase) {
    case Phase::Idle:
        break;
    case Phase::Requested:
        dMw_c::dMw_fade_out();  // HD collect_close_init for sub-window 9: no sound
        g_phase = Phase::FadingOut;
        g_phaseFrames = 0;
        break;
    case Phase::FadingOut:
        if (fader_black() || ++g_phaseFrames > kFadeTimeoutFrames) {
            open_screen(window);
        }
        break;
    case Phase::Open: {
        bgm_tick();
        if (g_cancelRequested && g_screen.cancel_now()) {
            g_cancelRequested = false;
            mods::log::info("Cave of Shadows screen: leaving as Cancel (the switch is being turned off)");
        }
        const Choice choice = g_screen.move();
        if (choice == Choice::Cancel) {
            bgm_cancel(true);  // HD 0x02a1a224 (state 0x2D -> 0x2E)
            dMw_c::dMw_fade_out();
            g_phase = Phase::FadingCancel;
            g_phaseFrames = 0;
        } else if (choice == Choice::Save) {
            dMw_c::dMw_fade_out();
            g_phase = Phase::FadingSave;
            g_phaseFrames = 0;
        }
        break;
    }
    case Phase::FadingCancel:
        if (fader_black() || ++g_phaseFrames > kFadeTimeoutFrames) {
            restore_collection(window);
            g_phase = Phase::Idle;
            g_cancelRequested = false;
            cave::confirm_finished(false);
        }
        break;
    case Phase::FadingSave:
        if (fader_black() || ++g_phaseFrames > kFadeTimeoutFrames) {
            hand_over_to_save(window);
        }
        break;
    }
}

void drop_screen(dMw_c* window, const char* why) {
    if (g_screen.is_open()) {
        if (window != nullptr && window->mMenuProc == dMw_c::COLLECT_MOVE) {
            g_screen.destroy();
        } else {
            g_screen.abandon();
        }
    }
    g_cancelRequested = false;
    if (g_phase != Phase::Idle) {
        mods::log::warn("Cave of Shadows screen closed: {}", why);
        g_phase = Phase::Idle;
        bgm_cancel(false);
        cave::confirm_finished(false);
    }
}

// --- hooks ---------------------------------------------------------------------------------------

HookAction pre_collect_move_proc(ModContext*, void* args, void*, void*) {
    if (g_phase == Phase::Idle) {
        return HOOK_CONTINUE;
    }
    // While the screen or its fades run, nothing of the Collection screen may run (it is deleted while
    // the screen is up) and Start/B must not close the pause menu.
    tick_host(mods::arg<dMw_c*>(args, 0));
    return HOOK_SKIP_ORIGINAL;
}

void post_draw(ModContext*, void*, void*, void*) {
    // dMw_c::_draw draws sub-windows only at window status 10 while paused (d_menu_window.cpp).
    if (g_screen.is_open() && dComIfGp_isPauseFlag() && dMeter2Info_getWindowStatus() == 10) {
        dComIfGd_set2DOpa(&g_screen);
    }
}

HookAction pre_delete(ModContext*, void* args, void*, void*) {
    drop_screen(mods::arg<dMw_c*>(args, 0), "the pause menu was deleted");
    if (g_heldMenu != nullptr) {
        // The play scene is going (the stage change from the held menu): HD's state 0x2D ends the same way.
        // dMw_c::_delete deletes the save menu and the pause flag with it (d_menu_window.cpp:1725, 1084-1092).
        g_heldMenu = nullptr;
        mods::log::info("cave trip: the held save menu went with the scene change");
    }
    return HOOK_CONTINUE;
}

// HD dMw state 0x2D (0x02a1a1dc-0x02a1a224): the save menu's _move runs only while its status is 2, and the state
// is left only when its end status is 1. With end status 2 the "Saved." menu just stays drawn, without input, until
// the stage change deletes the scene. GC's collect_save_move_proc would go to its close step on status 3
// (d_menu_window.cpp:860-868), so it is skipped while the trip's menu is held.
HookAction pre_save_move_proc(ModContext*, void* args, void*, void*) {
    if (g_heldMenu == nullptr) {
        return HOOK_CONTINUE;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (window == nullptr || window->mpMenuSave != g_heldMenu) {
        g_heldMenu = nullptr;  // another save menu (cannot normally happen: the held one is never closed)
        return HOOK_CONTINUE;
    }
    return HOOK_SKIP_ORIGINAL;
}

// The frame the hold starts: saveEnd (post_save_end below) runs inside this proc's _move, and the original then sees
// status 3 and switches to SAVE_CLOSE (fade out, back to the Collection screen). dMw_c::_execute runs the new state's
// init only after this proc returns (d_menu_window.cpp:1636-1639), so putting SAVE_MOVE back here cancels the switch.
void post_save_move_proc(ModContext*, void* args, void*, void*) {
    if (g_heldMenu == nullptr) {
        return;
    }
    auto* window = mods::arg<dMw_c*>(args, 0);
    if (window != nullptr && window->mpMenuSave == g_heldMenu && window->mMenuProc == dMw_c::SAVE_CLOSE) {
        window->mMenuProc = dMw_c::SAVE_MOVE;
    }
}

// The pause menu's save screen is the one cave.cpp's trip opened (Collection -> SAVE_OPEN).
bool trip_menu(const dMenu_save_c* menu) {
    if (menu == nullptr || !cave::save_trip_active()) {
        return false;
    }
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    return window != nullptr && window->mpMenuSave == menu && menu->mUseType == dMenu_save_c::TYPE_DEFAULT;
}

// ... or the one the "Cave of Shadows Record" screen handed over to (record.cpp): HD saves there and ends
// with "Saved." too (use types 4-7 share that ending).
bool hd_save_menu(const dMenu_save_c* menu) {
    return trip_menu(menu) || cave::record::save_menu(menu);
}

// GC's memCardDataSaveWait2 ends a successful pause-menu save (type 1) with "Saved. Continue playing?"
// (Yes closes the menu, No quits to the title). HD's type 6 shows "Saved." (1328 = 0x530) and closes the
// saving indicator instead; GC's event types 3/4 do exactly that, so the success branch runs as type 3
// for this one call (the error branch is the same for every type).
HookAction pre_save_wait2(ModContext*, void* args, void*, void*) {
    auto* menu = mods::arg<dMenu_save_c*>(args, 0);
    g_retypedMenu = nullptr;
    // FIX (review API #4): only with all three save hooks in place (the post hook restores the use type).
    if (g_saveHooksOk && hd_save_menu(menu) && menu->mWaitTimer == 0 && menu->mCmdState == 1) {
        menu->mUseType = dMenu_save_c::TYPE_WHITE_EVENT;
        g_retypedMenu = menu;
    }
    return HOOK_CONTINUE;
}

void post_save_wait2(ModContext*, void* args, void*, void*) {
    auto* menu = mods::arg<dMenu_save_c*>(args, 0);
    if (menu != nullptr && menu == g_retypedMenu) {
        menu->mUseType = dMenu_save_c::TYPE_DEFAULT;
        mods::log::info("cave: the save is written (\"Saved.\")");
    }
    g_retypedMenu = nullptr;
}

void close_save_menu(dMenu_save_c* menu) {
    menu->mSaveStatus = 4;
    // What GC's Z2SE_SY_CONTINUE_OK does (Z2SeMgr: menuOut), without a sound: HD plays none here.
    if (Z2AudioMgr* audio = Z2AudioMgr::getInterface(); audio != nullptr) {
        audio->menuOut();
    }
}

// saveEnd (type 1): end status 1, restartInit() (HD calls it too before enterLos), status 3 = back to the
// Collection screen, proc saveWait (empty = HD proc 0x28). After the trip's save, HD's type 6 (save proc 0x24,
// 0x02a0a7fc-0x02a0a820, VERIFIED) sets end status 2 instead, keeps status 3 and calls enterLos (0x02a02028:
// setNextStage D_SB11) in the same frame: no sound, no effect on Link; the "Saved." menu stays up while the scene
// change fades to black (dMw state 0x2D). Here: the same, with the menu held by pre_save_move_proc. Without that
// hook (or when the trip does not start from the menu) the pause menu closes (status 4, GC's "Continue playing?
// Yes") and the trip is decided once the game runs again (cave.cpp tick_entry).
void post_save_end(ModContext*, void* args, void*, void*) {
    auto* menu = mods::arg<dMenu_save_c*>(args, 0);
    if (!g_saveHooksOk || !trip_menu(menu) || !cave::save_trip_saved() || menu->mSaveStatus != 3 ||
        menu->mEndStatus != 1)
    {
        return;
    }
    if (g_holdHookOk && cave::enter_from_menu()) {
        menu->mEndStatus = 2;  // HD +0x24E = 2 (0x02a0a804); the status stays 3 (0x02a0a818)
        g_heldMenu = menu;
        mods::log::info("cave trip: saved; the stage changes from the save menu (HD: \"Saved.\" stays up)");
        return;
    }
    if (!g_holdHookOk) {
        mods::log::warn("cave trip: saved; the save menu cannot be held (hook missing), closing it first");
    }
    close_save_menu(menu);
    mods::log::info("cave trip: saved; closing the pause menu");
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
    bool host = ok_or_record(mods::hook::add_pre<CosMwCollectMoveProc>(pre_collect_move_proc, &high),
        "Cave of Shadows screen (menu host)");
    host = ok_or_record(mods::hook::add_post<CosMwDraw>(post_draw, &normal), "Cave of Shadows screen (draw)") && host;
    // FIX (review API/HD #2): the held "Saved." menu is let go only by this pre-hook (the scene change deletes the
    // menu); it is part of g_holdHookOk below, so a hold can never outlive its menu.
    const bool deleteHook =
        ok_or_record(mods::hook::add_pre<CosMwDelete>(pre_delete, &high), "Cave of Shadows screen (teardown)");
    host = deleteHook && host;
    g_hostHooksOk = host;
    bool save = ok_or_record(mods::hook::add_pre<CosSaveDataSaveWait2>(pre_save_wait2, &normal),
        "\"Saved.\" ending of the cave trip's save");
    save = save && ok_or_record(mods::hook::add_post<CosSaveDataSaveWait2>(post_save_wait2, &normal),
                       "\"Saved.\" ending of the cave trip's save (post)");
    save = ok_or_record(mods::hook::add_post<CosSaveEnd>(post_save_end, &normal),
               "menu closing after the cave trip's save") &&
           save;
    g_saveHooksOk = save;
    // Without them the menu closes after "Saved." and the stage change follows in the field (a plain fade). Both or
    // neither: the post-hook keeps the menu from leaving in the first frame, the pre-hook in every later one; and only
    // with the dMw_c::_delete pre-hook, which ends the hold when the scene change deletes the menu.
    g_holdHookOk = deleteHook && ok_or_record(mods::hook::add_post<CosMwSaveMoveProc>(post_save_move_proc, &normal),
                       "HD's \"Saved.\" screen held during the stage change to the cave") &&
                   ok_or_record(mods::hook::add_pre<CosMwSaveMoveProc>(pre_save_move_proc, &high),
                       "HD's \"Saved.\" screen held during the stage change to the cave (input)");
}

void uninstall_hooks() {
    // Not the three dMw_c targets: the stamp book hooks them too, and uninstall would drop its callbacks
    // as well (see hooks.cpp). The loader removes them on teardown. collect_save_move_proc is hooked only here.
    mods::hook::uninstall<CosMwSaveMoveProc>();
    mods::hook::uninstall<CosSaveEnd>();
    mods::hook::uninstall<CosSaveDataSaveWait2>();
}

void shutdown() {
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    if (window != nullptr && g_phase != Phase::Idle && window->mMenuProc == dMw_c::COLLECT_MOVE) {
        JKRHeap* previous = mDoExt_setCurrentHeap(window->mpHeap);
        if (g_screen.is_open() || window->mpMenuCollect == nullptr) {
            restore_collection(window);
        } else {
            // Still on the Collection screen (request or fade-out): cancel the request.
            if (window->mpMenuCollect->mpCollect2D != nullptr &&
                window->mpMenuCollect->mpCollect2D->mSubWindowOpenCheck == kSubWindowCave)
            {
                window->mpMenuCollect->mpCollect2D->mSubWindowOpenCheck = 0;
            }
            dMw_c::dMw_fade_in();
        }
        mDoExt_setCurrentHeap(previous);
    } else if (g_screen.is_open()) {
        g_screen.abandon();
    }
    g_phase = Phase::Idle;
    g_cancelRequested = false;
    bgm_cancel(dComIfGp_isPauseFlag() != 0);
    if (g_retypedMenu != nullptr) {
        g_retypedMenu->mUseType = dMenu_save_c::TYPE_DEFAULT;
        g_retypedMenu = nullptr;
    }
    release_save_hold("the mod is shutting down");  // the unhooked GC proc must be able to close it
}

bool preload() {
    if (!g_loadTried) {
        g_loadTried = true;
        g_archiveReady = load_archive();
        mods::log::info("Cave of Shadows screen archive: {} ({} bytes)",
            g_archiveReady ? "ready" : g_archiveProblem, g_archive.size);
    }
    return g_archiveReady;
}

bool screen_available() {
    return g_hostHooksOk && g_archiveReady;
}

JKRArchive* mount_screen_archive() {
    return mount_archive();
}

void unmount_screen_archive(JKRArchive*& archive) {
    unmount_archive(archive);
}

bool archive_has_record() {
    return g_archiveReady && g_archiveHasRecord;
}

bool archive_known_bad() {
    return g_loadTried && (!g_archiveReady || !g_archiveHasRecord);
}

bool save_hooks_ok() {
    return g_saveHooksOk;
}

bool save_held() {
    return g_heldMenu != nullptr;
}

void release_save_hold(const char* why) {
    if (g_heldMenu == nullptr) {
        return;
    }
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    if (window != nullptr && window->mpMenuSave == g_heldMenu) {
        if (fopOvlpM_IsDoingReq() != 0 && g_heldMenu->mSaveStatus == 3) {
            // FIX (review API #2b): the scene change's fade is already running. Closing now (status 4) would run GC's
            // SAVE_CLOSE, whose key_wait_init fades the screen back in (dMw_fade_in, d_menu_window.cpp:331-333) while
            // the scene change is still at black. Park the menu instead: in SAVE_OPEN with save status 3 GC's
            // collect_save_open_proc does nothing (d_menu_window.cpp:849-857), and set from outside no init runs, so
            // the menu stays inert without the mod's hooks until it is deleted with the scene.
            window->mMenuProc = dMw_c::SAVE_OPEN;
            mods::log::warn("cave trip: the held save menu stays up until the scene change deletes it ({})", why);
        } else {
            close_save_menu(g_heldMenu);
            mods::log::warn("cave trip: the held save menu closes ({})", why);
        }
    }
    g_heldMenu = nullptr;
}

void forget_save_hold(const char* why) {
    if (g_heldMenu == nullptr) {
        return;
    }
    // FIX (review API/HD #2): never touch the object: it may already be freed (its heap goes with the play scene).
    g_heldMenu = nullptr;
    mods::log::info("cave trip: the held save menu is forgotten ({})", why);
}

const char* problem() {
    if (!g_hostHooksOk) {
        return "the pause-menu hooks for HD's Cave of Shadows screen are missing";
    }
    if (g_loadTried && !g_archiveReady) {
        return g_archiveProblem;
    }
    return "";
}

bool open(const AmiiboData& data) {
    // The archive is read at black (open_screen, review L2); a known-bad one sends the scan straight to
    // the stock save menu (open_save) as before.
    if (g_phase != Phase::Idle || !g_hostHooksOk || (g_loadTried && !g_archiveReady)) {
        return false;
    }
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    if (window == nullptr || window->mMenuProc != dMw_c::COLLECT_MOVE || window->mpMenuCollect == nullptr ||
        window->mpMenuCollect->mpCollect2D == nullptr || !dComIfGp_isPauseFlag())
    {
        return false;
    }
    dMenu_Collect2D_c* collect = window->mpMenuCollect->mpCollect2D;
    if (collect->mSubWindowOpenCheck != 0) {
        return false;  // a sub-window (or the stamp book) is already opening
    }
    collect->mSubWindowOpenCheck = kSubWindowCave;
    g_data = data;
    g_phase = Phase::Requested;
    g_phaseFrames = 0;
    g_cancelRequested = false;
    return true;
}

void request_cancel() {
    if (g_phase != Phase::Idle) {
        g_cancelRequested = true;
    }
}

bool open_save() {
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    if (window == nullptr || window->mMenuProc != dMw_c::COLLECT_MOVE || window->mpMenuCollect == nullptr ||
        window->mpMenuCollect->mpCollect2D == nullptr || !dComIfGp_isPauseFlag())
    {
        return false;
    }
    dMenu_Collect2D_c* collect = window->mpMenuCollect->mpCollect2D;
    if (collect->mSubWindowOpenCheck != 0) {
        return false;
    }
    // The Collection screen's own Save (GC: collect_close_init without a sound for sub-window 1, fade,
    // SAVE_OPEN at black).
    collect->mSubWindowOpenCheck = 1;
    return true;
}

bool busy() {
    return g_phase != Phase::Idle || g_screen.is_open();
}

void tick() {
    if (g_phase == Phase::Idle) {
        return;
    }
    dMw_c* window = dMeter2Info_getMenuWindowClass();
    if (window == nullptr || window->mMenuProc != dMw_c::COLLECT_MOVE || !dComIfGp_isPauseFlag()) {
        // Something else moved the pause menu on (another mod; key_wait_init would have freed the 2D
        // heap under the screen). Never draw or delete freed objects.
        drop_screen(window, "the pause menu left its state from outside");
    }
}

void bgm_restore() {
    bgm_cancel(dComIfGp_isPauseFlag() != 0);
}

void bgm_stop() {
    if (!g_streamOurs) {
        return;
    }
    g_streamOurs = false;
    g_volumePending = 0;
    g_savedStream = kNoStream;
    Z2AudioMgr* audio = Z2AudioMgr::getInterface();
    if (audio != nullptr && audio->getStreamBgmID() == kStream) {
        audio->bgmStreamStop(kStreamFadeFrames);
    }
}

}  // namespace cave::confirm
