#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "filesel.hpp"

#include "book.hpp"
#include "book_assets.hpp"
#include "stamp_book.hpp"
#include "stamp_data.hpp"
#include "state.hpp"

#include "JSystem/J2DGraph/J2DPane.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JUtility/JUTFader.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_file_select.h"
#include "d/d_lib.h"
#include "d/d_menu_window.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

// Hook targets: exported by dusklight.exe v2.0.3, flag 0x09 (code, no inline sites).
//   dFile_select_c::screenSetDetail  post (build the plate under the details' menu_n, HD's moves)
//   dFile_select_c::makeRecInfo      post (fill it for the file whose details are shown)
//   dFile_select_c::dataSelectStart  pre  (hide it; makeRecInfo shows it again for a used file)
//   dFile_select_c::menuSelect       pre  (viewer: + opens it; skipped while it runs)
//   dFile_select_c::_draw            pre  (viewer: the book instead of the file select while open)
//   dFile_select_c::~dFile_select_c  pre  (viewer: torn down before the file select goes)
DEFINE_HOOK(&dFile_select_c::screenSetDetail, BkFsScreenSetDetail);
DEFINE_HOOK(&dFile_select_c::makeRecInfo, BkFsMakeRecInfo);
DEFINE_HOOK(&dFile_select_c::dataSelectStart, BkFsDataSelectStart);
DEFINE_HOOK(&dFile_select_c::menuSelect, BkFsMenuSelect);
DEFINE_HOOK(&dFile_select_c::_draw, BkFsDraw);
DEFINE_HOOK_SYMBOL("??1dFile_select_c@@UEAA@XZ", void(dFile_select_c*), BkFsDtor);

namespace hdstamps::book::filesel {
namespace {

constexpr uint64_t pane_tag(std::string_view name) {
    uint64_t value = 0;
    for (char c : name) {
        value = (value << 8) | static_cast<uint8_t>(c);
    }
    return value;
}

constexpr uint64_t kPlateTag = pane_tag("stamps_n");
constexpr uint64_t kTextTag = pane_tag("stamps_t");
constexpr uint64_t kPlusTag = pane_tag("stamps_p");

// Viewer heap: a child of the game heap (the file select's own 3D model takes 0x80000 from it the
// same way). The book needs about 160 KB of J2D objects (book_host.cpp); the rest is headroom,
// since an ExpHeap allocation failure aborts the game on PC.
constexpr u32 kViewerHeapSize = 0x80000;
constexpr s32 kViewerHeapMargin = 0x10000;
constexpr int kFadeTimeoutFrames = 60;

int g_installed = 0;
int g_wanted = 0;
bool g_viewerHooksOk = false;

// --- viewer (HD book mode 1) --------------------------------------------------------------------

enum class Phase : uint8_t {
    Idle,
    FadingOut,      // + pressed: 0xAA and fade to black
    Open,           // the book runs
    FadingOutClose, // B: SUB_OUT and fade to black
};

StampBook g_view;
Phase g_phase = Phase::Idle;
int g_phaseFrames = 0;
dFile_select_c* g_select = nullptr;
JKRExpHeap* g_heap = nullptr;
uint64_t g_mask = 0;

bool counter_enabled() {
    return mod_active() && filesel_counter_enabled() && assets::filesel_plate() != nullptr &&
           assets::filesel_plus() != nullptr && assets::filesel_icon() != nullptr;
}

bool viewer_available() {
    return g_viewerHooksOk && counter_enabled() && filesel_viewer_enabled() && book_available();
}

void se(u32 id) {
    mDoAud_seStart(id, nullptr, 0, 0);
}

JUTFader::EStatus fader_status() {
    JUTFader* fader = mDoGph_gInf_c::getFader();
    return fader != nullptr ? static_cast<JUTFader::EStatus>(fader->getStatus()) : JUTFader::None;
}

void destroy_heap() {
    if (g_heap != nullptr) {
        mDoExt_destroyExpHeap(g_heap);
        g_heap = nullptr;
    }
}

// Back to the file select: the book and its heap go, the screen fades in (not when the file select
// itself is being deleted: the scene change owns the fader then).
void close_viewer(bool fadeIn = true) {
    g_view.destroy();
    destroy_heap();
    if (fadeIn) {
        dMw_c::dMw_fade_in();
    }
    g_phase = Phase::Idle;
    g_select = nullptr;
    g_mask = 0;
}

bool open_viewer(dFile_select_c* select) {
    JKRExpHeap* parent = mDoExt_getGameHeap();
    if (parent == nullptr || parent->getFreeSize() < static_cast<s32>(kViewerHeapSize) + kViewerHeapMargin) {
        mods::log::error("stamp book (file select): game heap has {} bytes in its largest block, {} needed",
            parent != nullptr ? parent->getFreeSize() : 0, kViewerHeapSize + kViewerHeapMargin);
        return false;
    }
    g_heap = JKRExpHeap::create(kViewerHeapSize, parent, false);
    if (g_heap == nullptr) {
        mods::log::error("stamp book (file select): could not create its heap");
        return false;
    }
    if (!g_view.create(BookMode::FileSelect, g_mask, g_heap, select->stick)) {
        destroy_heap();
        return false;
    }
    return true;
}

void tick_viewer(dFile_select_c* select) {
    switch (g_phase) {
    case Phase::Idle:
        break;
    case Phase::FadingOut:
        if (fader_status() == JUTFader::None || ++g_phaseFrames > kFadeTimeoutFrames) {
            if (open_viewer(select)) {
                dMw_c::dMw_fade_in();
                g_phase = Phase::Open;
            } else {
                se(Z2SE_SYS_ERROR);
                close_viewer();
            }
        }
        break;
    case Phase::Open:
        // menuSelect (skipped) reads the stick; the book's moves need its triggers.
        if (select->stick != nullptr) {
            select->stick->checkTrigger();
        }
        if (!g_view.move()) {
            se(Z2SE_SY_MENU_SUB_OUT); // HD's close sound here is unknown (INFERRED: the book's own)
            dMw_c::dMw_fade_out();
            g_phase = Phase::FadingOutClose;
            g_phaseFrames = 0;
        }
        break;
    case Phase::FadingOutClose:
        if (fader_status() == JUTFader::None || ++g_phaseFrames > kFadeTimeoutFrames) {
            close_viewer();
        }
        break;
    }
}

// --- the Quest Log plate ------------------------------------------------------------------------

// Same convention as a blo2 pane: anchor `basePosition` (0-8 grid), `x, y` relative to the
// parent's anchor.
void place(J2DPane* pane, J2DBasePosition basePosition, float x, float y, float w, float h) {
    const int position = static_cast<int>(basePosition);
    const float xOffset = position % 3 == 1 ? w / 2.0f : position % 3 == 2 ? w : 0.0f;
    const float yOffset = position / 3 == 1 ? h / 2.0f : position / 3 == 2 ? h : 0.0f;
    pane->mBounds.set(-xOffset, -yOffset, w - xOffset, h - yOffset);
    pane->mBasePosition = static_cast<u8>(position);
    pane->mRotateOffsetX = xOffset;
    pane->mRotateOffsetY = yOffset;
    pane->translate(x, y);
}

J2DPicture* picture(J2DPane* parent, std::string_view name, const ResTIMG* timg, float x, float y, float w, float h) {
    auto* pic = JKR_NEW J2DPicture(pane_tag(name), JGeometry::TBox2<f32>(0.0f, 0.0f, w, h), timg, nullptr);
    if (pic != nullptr) {
        parent->appendChild(pic);
        place(pic, J2DBasePosition_4, x, y, w, h);
    }
    return pic;
}

// Owned stamps of save slot `slot` from its own blob (as the slot will load); 0 without one.
int stamp_count(int slot) {
    return std::max(0, stored_stamp_count(static_cast<uint32_t>(slot)));
}

bool file_has_data(const dFile_select_c* select, int slot) {
    return slot >= 0 && slot < 3 && !select->mIsNoData[slot] && !select->mIsDataNew[slot];
}

void post_screen_set_detail(ModContext*, void* args, void*, void*) {
    auto* select = mods::arg<dFile_select_c*>(args, 0);
    if (select == nullptr || !counter_enabled()) {
        return;
    }
    J2DScreen* screen = select->mSelDt.ScrDt;
    J2DPane* menu = screen != nullptr ? screen->search(pane_tag("menu_n")) : nullptr;
    if (menu == nullptr || screen->search(kPlateTag) != nullptr) {
        return;
    }
    // HD's stamps_n group (fileSel.arc zelda_file_select_details.blo, research/stampbook_spec.md
    // section 11.1): 80x25 at menu_n + (229, 12).
    auto* plate = JKR_NEW J2DPane(kPlateTag, JGeometry::TBox2<f32>(0.0f, 0.0f, 80.0f, 25.0f));
    if (plate == nullptr) {
        return;
    }
    menu->appendChild(plate);
    // HD's spot (229, 12) works in HD's re-laid-out Quest Log, where the 3D model sits higher. In
    // GC's layout it puts the plate's lower half behind the Copy/Start/Erase bar (separate screen
    // drawn on top), and the rotating Fused Shadow / Mirror model sweeps over its left end.
    // Measured in-game (16:9): up 18 units clears the bar with a small margin, right 16 clears
    // the model's sweep, and 88% keeps the plate and its "+" inside the panel's right edge.
    place(plate, J2DBasePosition_4, 245.0f, -6.0f, 80.0f, 25.0f);
    plate->scale(0.88f, 0.88f);
    picture(plate, "stamps_i", assets::filesel_plate(), 0.0f, 2.0f, 102.0f, 35.0f);
    picture(plate, "stamps_1", assets::filesel_icon(), -27.0f, 2.0f, 26.0f, 20.0f);
    auto* text = JKR_NEW J2DTextBox(kTextTag, JGeometry::TBox2<f32>(0.0f, 0.0f, 70.0f, 25.0f), nullptr, "", -1,
        HBIND_LEFT, VBIND_CENTER);
    if (text != nullptr) {
        plate->appendChild(text);
        place(text, J2DBasePosition_3, -10.0f, 3.0f, 70.0f, 25.0f); // centre-left anchor
        text->setFont(mDoExt_getMesgFont());
        text->setFontSize(17.0f, 14.0f);
        text->setCharSpace(0.0f);
        text->setLineSpace(24.0f);
        text->setCharColor(JUtility::TColor(255, 255, 255, 255));
        text->setGradColor(JUtility::TColor(255, 255, 255, 255));
        text->setString(16, "");
    }
    picture(plate, "stamps_p", assets::filesel_plus(), 45.0f, -10.0f, 20.0f, 17.0f);
    plate->hide();

    // HD's BLO also moves the 3D model base (mset_p_n) and the mask group (kamen_n) up. That does NOT
    // carry over to GC's quest-log screen: tested in-game, the Fused Shadow / Mirror model ended up on
    // top of the hearts and save time. GC's positions are kept; the plate fits in the corner as is.
}

void hide_plate(dFile_select_c* select) {
    J2DScreen* screen = select != nullptr ? select->mSelDt.ScrDt : nullptr;
    if (J2DPane* plate = screen != nullptr ? screen->search(kPlateTag) : nullptr; plate != nullptr) {
        plate->hide();
    }
}

void post_make_rec_info(ModContext*, void* args, void*, void*) {
    auto* select = mods::arg<dFile_select_c*>(args, 0);
    if (select == nullptr) {
        return;
    }
    // fileSelectWide (every presented frame) leaves the root stretched in Wii/Dusklight scaling:
    // the book follows it (book.hpp menu_scaling) when the first automatic opening comes before
    // any Collection screen.
    if (mDoGph_gInf_c::hudAspectScaleUp > 1.0001f && select->fileSel.Scr != nullptr) {
        note_wide_menus(std::fabs(select->fileSel.Scr->getScaleX() - 1.0f) > 0.001f);
    }
    const int slot = mods::arg<u8>(args, 1);
    J2DScreen* screen = select->mSelDt.ScrDt;
    J2DPane* plate = screen != nullptr ? screen->search(kPlateTag) : nullptr;
    if (plate == nullptr) {
        return;
    }
    if (!counter_enabled() || !file_has_data(select, slot)) {
        plate->hide();
        return;
    }
    const int count = stamp_count(slot);
    plate->show();
    // HD 0x0293cfbc toggles the counter and the "+" together (INFERRED: "has at least 1 stamp").
    // The "+" is the viewer's button glyph: only shown while the viewer can open.
    auto* text = static_cast<J2DTextBox*>(screen->search(kTextTag));
    J2DPane* plus = screen->search(kPlusTag);
    if (text != nullptr && text->getTypeID() == 19) {
        const std::string value = fmt::format("{}/{}", count, kStampCount);
        TextSpan span = text->getStringPtr();
        if (span.buffer != nullptr && span.size > value.size()) {
            std::memcpy(span.buffer, value.c_str(), value.size() + 1);
        }
        count >= 1 ? text->show() : text->hide();
    }
    if (plus != nullptr) {
        count >= 1 && viewer_available() ? plus->show() : plus->hide();
    }
}

HookAction pre_data_select_start(ModContext*, void* args, void*, void*) {
    hide_plate(mods::arg<dFile_select_c*>(args, 0));
    return HOOK_CONTINUE;
}

// --- viewer hooks -------------------------------------------------------------------------------

HookAction pre_menu_select(ModContext*, void* args, void*, void*) {
    auto* select = mods::arg<dFile_select_c*>(args, 0);
    if (select == nullptr) {
        return HOOK_CONTINUE;
    }
    if (g_phase != Phase::Idle) {
        // menuSelect is the file select's state while the viewer runs (nothing else changes
        // mDataSelProc meanwhile): its A/B/stick/pointer handling must not run under the book.
        tick_viewer(select);
        return HOOK_SKIP_ORIGINAL;
    }
    // HD: file-select state 0x2C, entered with 0xAA (Zelda.rpx 0x029471f8); the "+" glyph on the
    // plate names the button (Plus = Start, INFERRED).
    if (mDoCPd_c::getTrigStart(PAD_1) == 0 || !viewer_available() || fader_status() != JUTFader::Wait) {
        return HOOK_CONTINUE;
    }
    const int slot = select->mSelectNum;
    if (!file_has_data(select, slot) || !stored_stamp_mask(static_cast<uint32_t>(slot), g_mask) || g_mask == 0) {
        g_mask = 0;
        return HOOK_CONTINUE;
    }
    se(Z2SE_SY_FILE_COPY_OK); // 0xAA
    dMw_c::dMw_fade_out();
    g_select = select;
    g_phase = Phase::FadingOut;
    g_phaseFrames = 0;
    mods::log::info("stamp book (file select): viewing slot {} ({} stamps)", slot, std::popcount(g_mask));
    return HOOK_SKIP_ORIGINAL;
}

HookAction pre_draw(ModContext*, void* args, void*, void*) {
    auto* select = mods::arg<dFile_select_c*>(args, 0);
    if (g_phase == Phase::Idle || select != g_select) {
        return HOOK_CONTINUE;
    }
    if (select->mDataSelProc != dFile_select_c::DATASELPROC_MENU_SELECT) {
        // Something else took the file select over (memory-card check): give it back.
        mods::log::warn("stamp book (file select): the file select left its menu; closing the viewer");
        close_viewer();
        return HOOK_CONTINUE;
    }
    if (!g_view.is_open()) {
        return HOOK_CONTINUE; // fading out over the file select
    }
    // The book covers the whole screen (black quad first); skipping the file select also keeps
    // its 3D model (item 3D list) from drawing over the book.
    dComIfGd_set2DOpa(&g_view);
    return HOOK_SKIP_ORIGINAL;
}

HookAction pre_dtor(ModContext*, void* args, void*, void*) {
    if (mods::arg<dFile_select_c*>(args, 0) == g_select && g_phase != Phase::Idle) {
        close_viewer(false);
    }
    return HOOK_CONTINUE;
}

template <class Entry>
bool add_pre(HookPreFn fn, const char* what, int32_t priority = 0) {
    ++g_wanted;
    HookOptions options = HOOK_OPTIONS_INIT;
    options.priority = priority;
    if (mods::hook::add_pre<Entry>(fn, &options) == MOD_OK) {
        ++g_installed;
        return true;
    }
    mods::log::warn("file-select stamp plate: could not hook {}", what);
    return false;
}

template <class Entry>
bool add_post(HookPostFn fn, const char* what) {
    ++g_wanted;
    if (mods::hook::add_post<Entry>(fn) == MOD_OK) {
        ++g_installed;
        return true;
    }
    mods::log::warn("file-select stamp plate: could not hook {}", what);
    return false;
}

} // namespace

int install() {
    g_installed = 0;
    g_wanted = 0;
    add_post<BkFsScreenSetDetail>(post_screen_set_detail, "dFile_select_c::screenSetDetail");
    add_post<BkFsMakeRecInfo>(post_make_rec_info, "dFile_select_c::makeRecInfo");
    add_pre<BkFsDataSelectStart>(pre_data_select_start, "dFile_select_c::dataSelectStart");
    // The viewer only exists with all three (the "+" is hidden otherwise). High priority: while it
    // runs nothing else may act on the file select's menu under it.
    bool viewer = add_pre<BkFsMenuSelect>(pre_menu_select, "dFile_select_c::menuSelect", 1000);
    viewer = add_pre<BkFsDraw>(pre_draw, "dFile_select_c::_draw", 1000) && viewer;
    viewer = add_pre<BkFsDtor>(pre_dtor, "dFile_select_c::~dFile_select_c", 1000) && viewer;
    g_viewerHooksOk = viewer;
    return g_wanted - g_installed;
}

bool viewer_ready() {
    return viewer_available();
}

bool viewer_open() {
    return g_phase != Phase::Idle || g_view.is_open();
}

void shutdown() {
    if (g_phase != Phase::Idle || g_view.is_open()) {
        close_viewer(); // back to the file select's menu, faded in
    }
    g_viewerHooksOk = false;
}

void update() {}

void invalidate() {}

} // namespace hdstamps::book::filesel
