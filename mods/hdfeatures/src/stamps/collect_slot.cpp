#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "collect_slot.hpp"

#include <cstring>

#include "book.hpp"
#include "book_assets.hpp"
#include "book_host.hpp"
#include "hdtext.hpp"
#include "state.hpp"
#include "ui.hpp"

#include "JSystem/J2DGraph/J2DPane.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JUtility/JUTFader.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "d/d_com_inf_game.h"
#include "d/d_menu_collect.h"
#include "d/d_menu_window.h"
#include "d/d_meter2_info.h"
#include "m_Do/m_Do_graphic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

// Hook targets: all exported by dusklight.exe v2.0.3, flag 0x09 (code, no inline sites). Only pre
// and post hooks, so the cave mod's post-hooks on _create/_delete/wait_proc keep working.
//   screenSet               pre (inject the panes) + post (flags, text ids, counter, description)
//   getItemTag              post: (4,4) -> 'stam_7_n' for the screen the panes were injected in
//   wait_proc               pre (A on the slot: open, skip GC's generic A branch) + post (A label)
//   cursorMove              pre + post: HD's moves from the slot (check.md amendment 1)
//   pointerActivateCurrent  pre: mouse click on the slot (HD: tap on the selected cell)
//   menuCollectWide         post: runs every frame from _draw (TARGET_PC); description shift,
//                           Letters' scale on the slot, menu scaling mode for the book
//   setItemNameString       post: HD's name (dMeter2Info_getStringKanji reads zel_00 directly)
//   _delete                 post: forget the screen
DEFINE_HOOK(&dMenu_Collect2D_c::screenSet, BkCollectScreenSet);
DEFINE_HOOK(&dMenu_Collect2D_c::getItemTag, BkCollectGetItemTag);
DEFINE_HOOK(&dMenu_Collect2D_c::wait_proc, BkCollectWaitProc);
DEFINE_HOOK(&dMenu_Collect2D_c::cursorMove, BkCollectCursorMove);
DEFINE_HOOK(&dMenu_Collect2D_c::pointerActivateCurrent, BkCollectPointerActivate);
DEFINE_HOOK(&dMenu_Collect2D_c::menuCollectWide, BkCollectWide);
DEFINE_HOOK(&dMenu_Collect2D_c::setItemNameString, BkCollectItemName);
DEFINE_HOOK(&dMenu_Collect2D_c::_delete, BkCollectDelete);

namespace hdstamps::book::slot {
namespace {

constexpr uint64_t pane_tag(std::string_view name) {
    uint64_t value = 0;
    for (char c : name) {
        value = (value << 8) | static_cast<uint8_t>(c);
    }
    return value;
}

constexpr uint64_t kSlotTag = pane_tag("stam_7_n");
constexpr int kSlotX = 4; // GC grid cell right of Letters (3,4); HD's (4,3)
constexpr int kSlotY = 4;
constexpr float kSlotPitch = 47.0f; // GC Fishing -> Letters pitch (lett_4_n -90 vs fish_3_n -137)
constexpr u16 kMsgName = hdtext::kMsgStampCollection;            // 2800
constexpr u16 kMsgDescription = hdtext::kMsgStampCollectionHelp; // 2801
constexpr u16 kMsgOpen = 0x3EE;                                  // 1006 "Open" (GC; HD uses the same id)
// HD's counter digits (revo stamp10/stamp01): material C1 gold tint, 12x12, top-left anchored at
// x = -3 / +8 from the slot centre. HD puts them at the height of its Poe-soul digits (+13 in
// its 36x48 cells); GC's Poe digits sit at +8 in its 42x42 cells, so +8 here.
const JUtility::TColor kDigitTint(190, 190, 130, 255);
constexpr float kDigitTensX = -3.0f;
constexpr float kDigitOnesX = 8.0f;
constexpr float kDigitY = 8.0f;
constexpr float kDigitSize = 12.0f;
// The description block moves one slot pitch right and shrinks to fit (HD moved and shrank its
// own: 328 px at font 16 -> 250 px at 11x11). GC descriptions are pre-wrapped for 328 px.
constexpr float kDescriptionWidth = 328.0f - kSlotPitch;
constexpr float kDescriptionScale = kDescriptionWidth / 328.0f;

struct SavedPos {
    J2DPane* pane = nullptr;
    float x = 0.0f;
    float y = 0.0f;
};

// The Collection screen the panes were injected into (nullptr: none / not active).
dMenu_Collect2D_c* g_screenOwner = nullptr;
J2DPane* g_slot = nullptr;
J2DPicture* g_digitTens = nullptr;
J2DPicture* g_digitOnes = nullptr;
J2DPane* g_letters = nullptr;
J2DPane* g_menu = nullptr;
J2DPane* g_info = nullptr;
SavedPos g_moved[4]; // infotxtn, iiw00, iiw02, iib00 (layout positions)

u8 g_moveFromX = 0;
u8 g_moveFromY = 0;
bool g_moveTracked = false;

int g_installed = 0;
int g_wanted = 0;
bool g_hooksOk = false;

// --- getItemTag table fallback ------------------------------------------------------------------
// In-game (v2.0.3, MSVC build) funchook refuses getItemTag: its prologue holds a RIP-relative
// "lea reg, [itemTag]" it cannot relocate ("Could not fix ip-relative address"). getItemTag is just
// "return itemTag[y][x]" (d_menu_collect.cpp:1842), so instead of hooking it the slot's tag is
// written into that static table at [kSlotY][kSlotX] while the slot panes exist on a Collection
// screen, and cleared again when they go. Every caller (incl. inlined ones) then sees the slot.
extern "C" __declspec(dllimport) int __stdcall VirtualProtect(void* address, size_t size,
                                                             unsigned long newProtect,
                                                             unsigned long* oldProtect);
constexpr unsigned long kPageReadWrite = 0x04;

uint64_t* g_tagCell = nullptr;  // &itemTag[kSlotY][kSlotX] inside dusklight.exe, once validated
bool g_tagHookInstalled = false;
bool g_tagCellSet = false;

void tag_cell_write(uint64_t value) {
    if (g_tagCell == nullptr) {
        return;
    }
    unsigned long old = 0;
    if (VirtualProtect(g_tagCell, sizeof(uint64_t), kPageReadWrite, &old) == 0) {
        mods::log::error("stamp slot: could not unprotect the Collection tag table");
        return;
    }
    *g_tagCell = value;
    unsigned long ignored = 0;
    VirtualProtect(g_tagCell, sizeof(uint64_t), old, &ignored);
}

void tag_cell_set(bool on) {
    if (g_tagCell == nullptr || g_tagCellSet == on) {
        return;
    }
    tag_cell_write(on ? pane_tag("stam_7_n") : 0);
    g_tagCellSet = on;
}

// Finds itemTag[6][7] from getItemTag's code (the RIP-relative lea that made funchook give up) and
// validates it against GC's known contents before trusting it.
uint64_t* find_tag_cell(const void* function) {
    if (function == nullptr) {
        return nullptr;
    }
    const auto* code = static_cast<const uint8_t*>(function);
    for (int i = 0; i + 7 <= 64; ++i) {
        const uint8_t rex = code[i];
        if ((rex != 0x48 && rex != 0x4C) || code[i + 1] != 0x8D || (code[i + 2] & 0xC7) != 0x05) {
            continue;  // not "lea r64, [rip + disp32]"
        }
        int32_t disp = 0;
        std::memcpy(&disp, code + i + 3, sizeof(disp));
        auto* table = reinterpret_cast<uint64_t*>(const_cast<uint8_t*>(code + i + 7) + disp);
        auto at = [table](int y, int x) { return table[y * 7 + x]; };
        if (at(3, 0) == pane_tag("item_1_n") && at(3, 3) == pane_tag("maki_5_n") &&
            at(4, 0) == pane_tag("wolf_n") && at(4, 3) == pane_tag("lett_4_n") &&
            at(5, 0) == pane_tag("save_n") && at(5, 1) == pane_tag("option_n") &&
            (at(kSlotY, kSlotX) == 0 || at(kSlotY, kSlotX) == pane_tag("stam_7_n")))
        {
            return &table[kSlotY * 7 + kSlotX];
        }
    }
    return nullptr;
}

void forget_screen() {
    tag_cell_set(false);
    g_screenOwner = nullptr;
    g_slot = nullptr;
    g_digitTens = nullptr;
    g_digitOnes = nullptr;
    g_letters = nullptr;
    g_menu = nullptr;
    g_info = nullptr;
    for (SavedPos& saved : g_moved) {
        saved = SavedPos{};
    }
}

bool active_for(const dMenu_Collect2D_c* screen) {
    return screen != nullptr && screen == g_screenOwner;
}

// Gives a code-built pane the geometry of a layout pane (bounds, anchor, rotation offsets), moved
// by (dx, dy) in the parent's space.
void copy_geometry(J2DPane* pane, const J2DPane* model, float dx, float dy) {
    pane->mBounds = model->mBounds;
    pane->mBasePosition = model->mBasePosition;
    pane->mRotateOffsetX = model->mRotateOffsetX;
    pane->mRotateOffsetY = model->mRotateOffsetY;
    pane->translate(model->getTranslateX() + dx, model->getTranslateY() + dy);
}

// Top-left anchored pane (blo2 basePosition 0) at (x, y) from the parent's anchor.
void place_top_left(J2DPane* pane, float x, float y, float w, float h) {
    pane->mBounds.set(0.0f, 0.0f, w, h);
    pane->mBasePosition = J2DBasePosition_0;
    pane->mRotateOffsetX = 0.0f;
    pane->mRotateOffsetY = 0.0f;
    pane->translate(x, y);
}

// Centre-anchored pane (blo2 basePosition 4) at (x, y) from the parent's anchor.
void place_center(J2DPane* pane, float x, float y, float w, float h) {
    pane->mBounds.set(-w / 2.0f, -h / 2.0f, w / 2.0f, h / 2.0f);
    pane->mBasePosition = J2DBasePosition_4;
    pane->mRotateOffsetX = w / 2.0f;
    pane->mRotateOffsetY = h / 2.0f;
    pane->translate(x, y);
}

J2DPane* make_pane(J2DPane* parent, std::string_view name) {
    auto* pane = JKR_NEW J2DPane(pane_tag(name), JGeometry::TBox2<f32>(0.0f, 0.0f, 1.0f, 1.0f));
    if (pane != nullptr) {
        parent->appendChild(pane);
    }
    return pane;
}

J2DPicture* make_picture(J2DPane* parent, std::string_view name, const ResTIMG* timg) {
    auto* picture =
        JKR_NEW J2DPicture(pane_tag(name), JGeometry::TBox2<f32>(0.0f, 0.0f, 1.0f, 1.0f), timg, nullptr);
    if (picture != nullptr) {
        parent->appendChild(picture);
    }
    return picture;
}

const ResTIMG* digit_texture(int digit) {
    // GC setPohMaxNum's idiom: main2D 'TIMG' by dMeter2Info_getNumberTextureName.
    JKRArchive* archive = dComIfGp_getMain2DArchive();
    return archive != nullptr ?
               static_cast<const ResTIMG*>(archive->getResource('TIMG', dMeter2Info_getNumberTextureName(digit))) :
               nullptr;
}

bool slot_wanted() {
    return g_hooksOk && mod_active() && slot_available() && assets::slot_icon() != nullptr;
}

bool inject(dMenu_Collect2D_c* screen) {
    J2DScreen* layout = screen->mpScreen;
    if (layout == nullptr) {
        return false;
    }
    J2DPane* menu = layout->search(pane_tag("menu_n"));
    J2DPane* gray = layout->search(pane_tag("gray_n"));
    J2DPane* letters = layout->search(pane_tag("lett_4_n"));
    J2DPane* lettersIcon = layout->search(pane_tag("letter"));
    J2DPane* lettersTarget = layout->search(pane_tag("N_letter"));
    J2DPane* lettersFrame = layout->search(pane_tag("gray_05"));
    J2DPane* info = layout->search(pane_tag("info_n"));
    if (menu == nullptr || gray == nullptr || letters == nullptr || lettersFrame == nullptr || info == nullptr ||
        lettersFrame->getTypeID() != 18)
    {
        mods::log::warn("Collection layout differs from GC's; the stamp slot is not added");
        return false;
    }
    if (layout->search(kSlotTag) != nullptr) {
        mods::log::warn("Collection screen already has a 'stam_7_n' pane; the stamp slot is not added");
        return false;
    }
    const ResTIMG* zero = digit_texture(0);
    if (zero == nullptr) {
        mods::log::warn("main2D digit textures missing; the stamp slot is not added");
        return false;
    }

    // Empty-slot frame (HD gray_21, a sibling in the gray-frame group): Letters' frame gray_05
    // one pitch to the right, with its texture (tt_spot_square3) and corner colours.
    auto* frameModel = static_cast<J2DPicture*>(lettersFrame);
    const ResTIMG* frameTexture = nullptr;
    if (JUTTexture* texture = frameModel->getTexture(0); texture != nullptr) {
        frameTexture = texture->getTexInfo();
    }
    if (frameTexture == nullptr) {
        if (JKRArchive* collect = dComIfGp_getCollectResArchive(); collect != nullptr) {
            frameTexture = static_cast<const ResTIMG*>(collect->getResource('TIMG', "tt_spot_square3.bti"));
        }
    }
    if (frameTexture != nullptr) {
        if (J2DPicture* frame = make_picture(gray, "gray_21", frameTexture); frame != nullptr) {
            copy_geometry(frame, frameModel, kSlotPitch, 0.0f);
            frame->setCornerColor(frameModel->corner(0), frameModel->corner(1), frameModel->corner(2),
                frameModel->corner(3));
            // The material's black/white tint too (J2DPictureEx reads them from its TEV colours);
            // the rest of gray_05's material is not copied (INFERRED to be a plain blend).
            frame->setBlackWhite(frameModel->getBlack(), frameModel->getWhite());
            frame->setAlpha(frameModel->getAlpha());
        }
    }

    // stam_7_n: lett_4_n's metrics one pitch to the right (HD: Letters + 41 in its own grid),
    // user tag n_43 like every cell; children as HD's revo tree.
    J2DPane* slot = make_pane(menu, "stam_7_n");
    if (slot == nullptr) {
        return false;
    }
    copy_geometry(slot, letters, kSlotPitch, 0.0f);
    slot->setUserInfo(pane_tag("n_43"));
    if (J2DPicture* icon = make_picture(slot, "stamp", assets::slot_icon()); icon != nullptr) {
        // Untinted (HD material: one TEV stage, no tint), where the Letters icon sits.
        if (lettersIcon != nullptr) {
            copy_geometry(icon, lettersIcon, 0.0f, 0.0f);
        } else {
            place_center(icon, -1.0f, -1.0f, 38.0f, 38.0f);
        }
    }
    J2DPicture* tens = make_picture(slot, "stamp10", zero);
    J2DPicture* ones = make_picture(slot, "stamp01", zero);
    for (J2DPicture* digit : {tens, ones}) {
        if (digit != nullptr) {
            place_top_left(digit, digit == tens ? kDigitTensX : kDigitOnesX, kDigitY, kDigitSize, kDigitSize);
            digit->setBlackWhite(JUtility::TColor(0, 0, 0, 0), kDigitTint);
        }
    }
    if (J2DPane* target = make_pane(slot, "N_stamp"); target != nullptr) {
        if (lettersTarget != nullptr) {
            copy_geometry(target, lettersTarget, 0.0f, 0.0f);
        } else {
            place_center(target, 0.0f, 0.0f, letters->getWidth(), letters->getHeight());
        }
    }
    slot->hide();

    g_screenOwner = screen;
    g_slot = slot;
    g_digitTens = tens;
    g_digitOnes = ones;
    g_letters = letters;
    g_menu = menu;
    g_info = info;
    static constexpr std::string_view kMoved[4] = {"infotxtn", "iiw00", "iiw02", "iib00"};
    for (int i = 0; i < 4; ++i) {
        J2DPane* pane = layout->search(pane_tag(kMoved[i]));
        g_moved[i] = pane != nullptr ? SavedPos{pane, pane->getTranslateX(), pane->getTranslateY()} : SavedPos{};
    }
    return true;
}

void set_counter(int count) {
    if (g_digitTens == nullptr || g_digitOnes == nullptr) {
        return;
    }
    // HD setStampNum (0x029b3850): two digits, the tens only from 10.
    count = std::clamp(count, 0, 99);
    if (const ResTIMG* ones = digit_texture(count % 10); ones != nullptr) {
        g_digitOnes->changeTexture(ones, 0);
    }
    if (count >= 10) {
        if (const ResTIMG* tens = digit_texture(count / 10); tens != nullptr) {
            g_digitTens->changeTexture(tens, 0);
        }
        g_digitTens->show();
    } else {
        g_digitTens->hide();
    }
    g_digitOnes->show();
}

void shrink_description(J2DScreen* layout) {
    static constexpr std::string_view kBoxes[4] = {"f_text0", "f_text1", "i_text0", "i_text1"};
    for (std::string_view name : kBoxes) {
        J2DPane* pane = layout->search(pane_tag(name));
        if (pane == nullptr || pane->getTypeID() != 19) {
            continue;
        }
        auto* box = static_cast<J2DTextBox*>(pane);
        J2DTextBox::TFontSize size;
        box->getFontSize(size);
        box->setFontSize(size.mSizeX * kDescriptionScale, size.mSizeY * kDescriptionScale);
        box->setLineSpace(box->getLineSpace() * kDescriptionScale);
        box->resize(box->getWidth() * kDescriptionScale, box->getHeight());
    }
}

void write_name(dMenu_Collect2D_c* screen) {
    // dMeter2Info_getStringKanji (setItemNameString) reads zel_00 directly, so MessageService
    // overrides never reach it: HD's 2800 is written into the name boxes here.
    const std::string name = hdtext::game_text_or(hdtext::kGroupZel00, kMsgName, "Stamps");
    for (int i = 0; i < 8; ++i) {
        char tag[9] = "item_n00";
        tag[7] = static_cast<char>('0' + i);
        J2DPane* pane = screen->mpScreen->search(pane_tag(tag));
        if (pane == nullptr || pane->getTypeID() != 19) {
            continue;
        }
        TextSpan span = static_cast<J2DTextBox*>(pane)->getStringPtr();
        if (span.buffer == nullptr || span.size == 0) {
            continue;
        }
        const size_t length = std::min(name.size(), span.size - 1);
        std::memcpy(span.buffer, name.data(), length);
        span.buffer[length] = '\0';
    }
}

bool open_from_slot(dMenu_Collect2D_c* screen) {
    if (screen->field_0x22d[kSlotX][kSlotY] == 0) {
        return false; // HD: only with at least 1 stamp (flag[4][3])
    }
    JUTFader* fader = mDoGph_gInf_c::getFader();
    if (fader != nullptr && fader->getStatus() != JUTFader::Wait) {
        return false; // HD: fader idle
    }
    if (book_available()) {
        if (!host::request_open_from_collection(screen)) {
            return false;
        }
        dMeter2Info_set2DVibration(); // HD wait_proc: vibration, no sound (SUB_IN comes with the fade)
        return true;
    }
    // Fallback: the RmlUi list (archive or host hooks missing; the mods panel says which).
    dMeter2Info_set2DVibration();
    ui::open_stamp_book();
    return true;
}

// --- hooks -----------------------------------------------------------------------------------

HookAction pre_screen_set(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    forget_screen();
    if (screen == nullptr || !slot_wanted()) {
        return HOOK_CONTINUE;
    }
    if (!inject(screen)) {
        forget_screen();
    } else if (!g_tagHookInstalled) {
        tag_cell_set(true);  // the panes exist now: let getItemTag name the slot (table fallback)
    }
    return HOOK_CONTINUE;
}

void post_screen_set(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (!active_for(screen)) {
        return;
    }
    // screenSet zeroes and recomputes the flags before its show/hide loop: set ours afterwards.
    // HD isStampIconVisible (0x029b1990): the icon only from the first stamp; the empty frame
    // gray_21 always shows.
    const int count = owned_count();
    screen->field_0x22d[kSlotX][kSlotY] = count >= 1 ? 1 : 0;
    screen->field_0x184[kSlotX][kSlotY] = kMsgName;
    screen->field_0x1d8[kSlotX][kSlotY] = kMsgDescription;
    if (count >= 1) {
        g_slot->show();
    } else {
        g_slot->hide();
    }
    set_counter(count);
    shrink_description(screen->mpScreen);
    if (screen->mCursorX == kSlotX && screen->mCursorY == kSlotY) {
        screen->setItemNameString(kSlotX, kSlotY);
    }
}

void post_get_item_tag(ModContext*, void* args, void* retval, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (retval == nullptr || !active_for(screen)) {
        return;
    }
    // getItemTag(x, y, bool): y == 5 with false is the only case GC answers 0 for a real cell;
    // the slot is in row 4, so both lookups name it.
    if (mods::arg<int>(args, 1) == kSlotX && mods::arg<int>(args, 2) == kSlotY) {
        *static_cast<u64*>(retval) = kSlotTag;
    }
}

HookAction pre_wait_proc(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (!active_for(screen) || host::busy()) {
        return HOOK_CONTINUE;
    }
    if (screen->mCursorX == kSlotX && screen->mCursorY == kSlotY && dMw_A_TRIGGER()) {
        // Skipping the original stops GC's generic A branch for this cell (it does nothing at
        // y = 4, but it is equipment code).
        if (open_from_slot(screen)) {
            return HOOK_SKIP_ORIGINAL;
        }
    }
    return HOOK_CONTINUE;
}

void post_wait_proc(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (!active_for(screen) || host::busy()) {
        return;
    }
    if (screen->mCursorX == kSlotX && screen->mCursorY == kSlotY) {
        // HD 0x029b867c: A = 1006 "Open" with at least 1 stamp; B stays GC's 1017 "Back".
        screen->setAButtonString(screen->field_0x22d[kSlotX][kSlotY] != 0 ? kMsgOpen : 0);
    }
}

HookAction pre_cursor_move(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    g_moveTracked = active_for(screen);
    if (g_moveTracked) {
        g_moveFromX = screen->mCursorX;
        g_moveFromY = screen->mCursorY;
    }
    return HOOK_CONTINUE;
}

void post_cursor_move(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (!g_moveTracked || !active_for(screen)) {
        return;
    }
    g_moveTracked = false;
    if (g_moveFromY == 5 && screen->mCursorX == kSlotX && screen->mCursorY == kSlotY) {
        // GC's "Up from Save/Options goes back to the previous cell" rule (d_menu_collect.cpp
        // cursorMove, field_0x259/25a) re-enters the slot after Down from it. HD has no such rule
        // (Up loop 0x029b78fc: from Save it takes the cell above, Golden Bugs in HD's grid) and
        // only ever enters the slot with Right from Letters. Redirect to what GC's own code picks
        // when there is no previous cell: the first cell of the row above from the left for Save
        // (GC's cell above Save, wolf_n (0,4)), from x = 3 for Options.
        const int first = g_moveFromX == 0 ? 0 : 3;
        const int last = g_moveFromX == 0 ? 3 : 6;
        for (int x = first; x <= last; ++x) {
            if (x != kSlotX && screen->getItemTag(x, kSlotY, true) != 0) {
                screen->mCursorX = static_cast<u8>(x);
                screen->mCursorY = static_cast<u8>(kSlotY);
                screen->cursorPosSet();
                screen->setItemNameString(static_cast<u8>(x), static_cast<u8>(kSlotY));
                break;
            }
        }
        return;
    }
    if (g_moveFromX != kSlotX || g_moveFromY != kSlotY) {
        return; // moves into the slot are GC's own: Right from Letters reaches it (as in HD)
    }
    // HD cursorMove (Zelda.rpx 0x029b76f4, [5][9] grid, stamp slot (4,3)):
    //   Up    y 3 -> 2, then the rule "y == 2 && x == 4 -> (3,2)" HD added for this column: Scent,
    //         the cell of the row above nearest to the slot
    //   Down  nothing below in the column -> Save (y = 4, x < 8 -> 0)
    //   Left  Letters (generic scan); Right nothing (x 5-7 empty)
    // GC's own algorithm already does Left/Right the same way. GC's Up would give the Hero's
    // Clothes (3,2) (its y == 2 list) and its Down Options (1,5) (y == 4 and x > 3); they are
    // mapped onto GC's grid as HD's results: the nearest cell of the row above, Hidden Skills
    // (3,3), and Save (0,5).
    int targetX;
    int targetY;
    if (screen->mCursorY < kSlotY) {
        targetX = 3;
        targetY = 3;
    } else if (screen->mCursorY == 5) {
        targetX = 0;
        targetY = 5;
    } else {
        return;
    }
    if ((screen->mCursorX == targetX && screen->mCursorY == targetY) ||
        screen->getItemTag(targetX, targetY, true) == 0)
    {
        return;
    }
    // The original already played the move sound (CURSOR_ITEM, or CURSOR_OPTION into row 5 as
    // HD's 0x4C for Save) and stored the previous cell (4,4) for GC's "back up" rules.
    screen->mCursorX = static_cast<u8>(targetX);
    screen->mCursorY = static_cast<u8>(targetY);
    screen->cursorPosSet();
    screen->setItemNameString(static_cast<u8>(targetX), static_cast<u8>(targetY));
}

HookAction pre_pointer_activate(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (!active_for(screen) || host::busy()) {
        return HOOK_CONTINUE;
    }
    if (screen->mCursorX == kSlotX && screen->mCursorY == kSlotY && open_from_slot(screen)) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void post_menu_collect_wide(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (screen == nullptr || screen->mpScreen == nullptr) {
        return;
    }
    // menuCollectWide resets the cached panes and re-applies the menu scaling mode every frame
    // (dMenu_Collect2D_c::_draw on TARGET_PC); read the mode it applied (the setting itself is not
    // exported): GameCube leaves the canvas at 1, Wii stretches it, Dusklight also counter-scales
    // menu_n.
    J2DScreen* root = screen->mpScreen;
    J2DPane* menu = active_for(screen) ? g_menu : root->search(pane_tag("menu_n"));
    // At 4:3 (hudAspectScaleUp 1) every mode leaves the canvas at 1: nothing to learn.
    if (mDoGph_gInf_c::hudAspectScaleUp > 1.0001f) {
        MenuScaling mode = MenuScaling::GameCube;
        if (std::fabs(root->getScaleX() - 1.0f) > 0.001f) {
            mode = menu != nullptr && std::fabs(menu->getScaleX() - 1.0f) > 0.001f ? MenuScaling::Dusklight :
                                                                                      MenuScaling::Wii;
        }
        set_menu_scaling(mode);
    }
    if (!active_for(screen)) {
        return;
    }
    // The slot takes Letters' scale (menuCollectWide resets lett_4_n every frame, counter-scaled
    // in Wii mode), so it behaves exactly like the cell next to it.
    g_slot->scale(g_letters->getScaleX(), g_letters->getScaleY());
    // Shift the description one slot pitch right in screen space (menu_n and info_n are scaled
    // differently in Dusklight mode); from the layout positions, so it never accumulates.
    const float infoScale = g_info->getScaleX() != 0.0f ? g_info->getScaleX() : 1.0f;
    const float shift = kSlotPitch * g_menu->getScaleX() / infoScale;
    for (const SavedPos& saved : g_moved) {
        if (saved.pane != nullptr) {
            saved.pane->translate(saved.x + shift, saved.y);
        }
    }
}

void post_set_item_name(ModContext*, void* args, void*, void*) {
    auto* screen = mods::arg<dMenu_Collect2D_c*>(args, 0);
    if (!active_for(screen)) {
        return;
    }
    if (mods::arg<u8>(args, 1) == kSlotX && mods::arg<u8>(args, 2) == kSlotY &&
        screen->field_0x22d[kSlotX][kSlotY] != 0)
    {
        write_name(screen);
    }
}

void post_delete(ModContext*, void* args, void*, void*) {
    if (mods::arg<dMenu_Collect2D_c*>(args, 0) == g_screenOwner) {
        forget_screen();
    }
}

// Pre-hooks run in priority order and a HOOK_SKIP_ORIGINAL cancels the lower ones. Other mods hook
// the same Collection functions (Dawnlight 4.1.0: screenSet, cursorMove, wait_proc,
// pointerActivateCurrent, ...), so the slot's pre-hooks run ahead of default-priority ones: a
// skip elsewhere must not swallow the A press on the slot or the cursor-move bookkeeping. They only
// skip the original themselves for the slot's own A press / click.
constexpr int32_t kPrePriority = 100;

template <class Entry>
bool add_pre(HookPreFn fn, const char* what) {
    ++g_wanted;
    HookOptions options = HOOK_OPTIONS_INIT;
    options.priority = kPrePriority;
    const ModResult result = mods::hook::add_pre<Entry>(fn, &options);
    if (result != MOD_OK) {
        mods::log::error("stamp slot: could not hook {} (result {})", what, static_cast<int>(result));
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
        mods::log::error("stamp slot: could not hook {} (result {})", what, static_cast<int>(result));
        return false;
    }
    ++g_installed;
    return true;
}

} // namespace

int install() {
    g_installed = 0;
    g_wanted = 0;
    bool ok = add_pre<BkCollectScreenSet>(pre_screen_set, "dMenu_Collect2D_c::screenSet");
    ok = add_post<BkCollectScreenSet>(post_screen_set, "dMenu_Collect2D_c::screenSet (post)") && ok;
    // getItemTag: a post-hook where funchook can patch it, otherwise the itemTag table fallback.
    g_tagHookInstalled = false;
    g_tagCell = nullptr;
    g_tagCellSet = false;
    if (mods::hook::add_post<BkCollectGetItemTag>(post_get_item_tag) == MOD_OK) {
        g_tagHookInstalled = true;
        ++g_wanted;
        ++g_installed;
    } else {
        ++g_wanted;
        g_tagCell = find_tag_cell(BkCollectGetItemTag::resolved_target());
        if (g_tagCell != nullptr) {
            ++g_installed;
            mods::log::info("stamp slot: getItemTag can't be hooked here; using its tag table instead");
        } else {
            mods::log::error("stamp slot: getItemTag can't be hooked and its tag table wasn't found");
            ok = false;
        }
    }
    ok = add_pre<BkCollectWaitProc>(pre_wait_proc, "dMenu_Collect2D_c::wait_proc") && ok;
    ok = add_post<BkCollectWaitProc>(post_wait_proc, "dMenu_Collect2D_c::wait_proc (post)") && ok;
    ok = add_post<BkCollectItemName>(post_set_item_name, "dMenu_Collect2D_c::setItemNameString") && ok;
    ok = add_post<BkCollectDelete>(post_delete, "dMenu_Collect2D_c::_delete") && ok;
    // HD's cursor moves and the description shift are part of the HD layout: without them the
    // slot is not added either (the description would overlap it).
    ok = add_pre<BkCollectCursorMove>(pre_cursor_move, "dMenu_Collect2D_c::cursorMove") && ok;
    ok = add_post<BkCollectCursorMove>(post_cursor_move, "dMenu_Collect2D_c::cursorMove (post)") && ok;
    ok = add_post<BkCollectWide>(post_menu_collect_wide, "dMenu_Collect2D_c::menuCollectWide") && ok;
    // Optional: the mouse (the pad covers everything).
    add_pre<BkCollectPointerActivate>(pre_pointer_activate, "dMenu_Collect2D_c::pointerActivateCurrent");
    g_hooksOk = ok;
    return g_wanted - g_installed;
}

bool hooks_ok() {
    return g_hooksOk;
}

int hooks_installed() {
    return g_installed;
}

int hooks_wanted() {
    return g_wanted;
}

void shutdown() {
    // The panes belong to the Collection screen (deleted with it); without the getItemTag hook a
    // still-open screen simply stops treating (4,4) as a cell.
    forget_screen();
    g_hooksOk = false;
}

} // namespace hdstamps::book::slot
