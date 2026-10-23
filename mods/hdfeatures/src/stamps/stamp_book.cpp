#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "stamp_book.hpp"

#include "book_assets.hpp"
#include "hdtext.hpp"
#include "stamp_data.hpp"

#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JSupport/JSUList.h"
#include "JSystem/JUtility/JUTFader.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "Z2AudioLib/Z2SeqMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_lib.h"
#include "d/d_meter2_info.h"
#include "d/d_pane_class.h"
#include "d/d_select_cursor.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <string_view>

namespace hdstamps::book {
namespace {

// J2D pane names are up to 8 characters packed big-endian into a u64 (MULTI_CHAR).
constexpr uint64_t pane_tag(std::string_view name) {
    uint64_t value = 0;
    for (char c : name) {
        value = (value << 8) | static_cast<uint8_t>(c);
    }
    return value;
}

constexpr uint64_t kTagN43 = pane_tag("n_43");
constexpr u16 kTypePicture = 18;           // J2DPicture::getTypeID (J2DPictureEx inherits it)
constexpr u16 kTypeTextBox = 19;           // J2DTextBox::getTypeID (J2DTextBoxEx inherits it)
constexpr u32 kSetPriorityFlags = 0x20000; // material panes kept, as GC's dMenu_Fishing_c

// HD values (research/stampbook/code.md, check.md).
constexpr int kPages = 5;              // ceil(5.0f) at 0x10098800
constexpr float kSelectedScale = 1.1f; // refreshStampPanes 0x02a1251c
constexpr int kRewardBgmFrame = 30;    // reward intro 0x02a14b94
constexpr int kRewardFadeStart = 30;
constexpr int kRewardFadeFrames = 30;
constexpr int kRewardMessageFrame = 90;
constexpr int kMessageFadeFrames = 30; // message step 0x02a14d24
constexpr u16 kMsgBack = 0x3F9;        // zel_00 1017 "Back" (GC; HD uses the same id)

// Own wording, used only when res/stamps/hdtext (HD's text from the user's dump) is missing.
constexpr char kFallbackTitle[] = "Stamps";
constexpr char kFallbackPage[] = "%d / %d";
constexpr char kFallbackPost[] = "Post";
constexpr char kFallbackReward[] = "All 49 stamps collected!\nThe Twili Midna stamp is yours.";

J2DPane* find(J2DScreen* screen, std::string_view name, bool required = true) {
    J2DPane* pane = screen != nullptr ? screen->search(pane_tag(name)) : nullptr;
    if (pane == nullptr && required) {
        mods::log::error("stamp book: pane '{}' not found in the HD layout", name);
    }
    return pane;
}

// Only returns a pane of the type the caller writes through (a PAN2 used as a picture or a text box
// would be written past its end by setBlackWhite/setCornerColor/setString).
template <class T>
T* find_as(J2DScreen* screen, std::string_view name, bool required = true) {
    static_assert(std::is_same_v<T, J2DPicture> || std::is_same_v<T, J2DTextBox>);
    constexpr u16 kType = std::is_same_v<T, J2DPicture> ? kTypePicture : kTypeTextBox;
    J2DPane* pane = find(screen, name, required);
    if (pane != nullptr && pane->getTypeID() != kType) {
        mods::log::error("stamp book: pane '{}' has type {} in the HD layout, {} expected", name,
            pane->getTypeID(), kType);
        return nullptr;
    }
    return static_cast<T*>(pane);
}

template <class Fn>
void for_each_pane(J2DPane* pane, Fn&& fn) {
    if (pane == nullptr) {
        return;
    }
    fn(pane);
    for (JSUTreeIterator<J2DPane> it = pane->getFirstChild(); it != pane->getEndChild(); ++it) {
        for_each_pane(it.getObject(), fn);
    }
}

constexpr uint64_t kTagArrowL = pane_tag("N_arrowl");
constexpr uint64_t kTagArrowR = pane_tag("N_arrowr");
// The reward popup's full-screen dim (850x550 at (-143,-59), alpha 0xBE). It is tagged n_43, but
// counter-scaled it would stop about 150 units short of the right edge of a 16:9 screen; it is
// meant to cover the whole screen, so it stretches with the canvas.
constexpr uint64_t kTagMsgDim = pane_tag("msg_bac1");

// Panes kept at their 4:3 shape when the canvas is stretched: HD's "n_43" panes, plus (INFERRED)
// the text boxes and L/R page glyphs outside them, which GC's own widescreen code
// (menuCollectWide) never lets stretch either. Nothing inside a kept pane is added again.
template <class Fn>
void for_each_kept_43(J2DPane* pane, bool inside, Fn&& fn) {
    if (pane == nullptr) {
        return;
    }
    bool keep = false;
    if (!inside && pane->mInfoTag != kTagMsgDim) {
        keep = pane->getUserInfo() == kTagN43 || pane->getTypeID() == kTypeTextBox ||
               pane->mInfoTag == kTagArrowL || pane->mInfoTag == kTagArrowR;
        if (keep) {
            fn(pane);
        }
    }
    for (JSUTreeIterator<J2DPane> it = pane->getFirstChild(); it != pane->getEndChild(); ++it) {
        for_each_kept_43(it.getObject(), inside || keep, fn);
    }
}

void se(u32 id) {
    mDoAud_seStart(id, nullptr, 0, 0);
}

bool fader_idle() {
    JUTFader* fader = mDoGph_gInf_c::getFader();
    return fader == nullptr || fader->getStatus() == JUTFader::Wait;
}

// HD 0x02a12188: 2890 + index (zel_00, HD book order).
std::string stamp_name_game(int index) {
    std::string text =
        hdtext::game_text(hdtext::kGroupZel00, static_cast<uint16_t>(hdtext::kMsgFirstStampName + index));
    return text.empty() ? hdtext::fallback_stamp_name(index) : text;
}

} // namespace

bool StampBook::owned(int stamp) const {
    return stamp >= 0 && stamp < kStampCount && (mMask & stamp_bit(stamp)) != 0;
}

bool StampBook::post_selectable() const {
    return mPost == PostButton::Hd && mPostPane != nullptr;
}

bool StampBook::create(BookMode mode, uint64_t ownedMask, JKRHeap* heap, STControl* stick) {
    if (mOpen) {
        destroy();
    }
    if (heap == nullptr) {
        mods::log::error("stamp book: no heap to build on");
        return false;
    }
    mMode = mode;
    mMask = ownedMask & kAllStampsMask;
    mHeap = heap;
    mStick = stick;
    mPost = post_button();
    mClose = false;
    mAButton = -1;
    mFrame = 0;
    mIgnoreHeldDirection = mode == BookMode::FirstStamp;

    JKRHeap* previous = mDoExt_setCurrentHeap(heap);
    const s32 freeBefore = heap->getTotalFreeSize();
    const bool ok = build() && lookup_panes();
    if (ok) {
        init_texts();
        init_cursor();
        mStep = mMode == BookMode::Reward ? Step::RewardIntro : Step::Browse;
        mOpen = true;
        refresh();
        apply_scaling();
    }
    const s32 freeAfter = heap->getTotalFreeSize();
    mDoExt_setCurrentHeap(previous);
    if (!ok) {
        destroy();
        return false;
    }
    sLastHeapUse = freeBefore - freeAfter;
    mods::log::info("stamp book opened (mode {}, {} stamps): 2D heap {} -> {} bytes free ({} used)",
        static_cast<int>(mode), std::popcount(mMask), freeBefore, freeAfter, sLastHeapUse);
    return true;
}

bool StampBook::build() {
    // GC-side pieces first, before the archive is mounted: a mounted JKRMemArchive sits at the
    // front of the global volume list, so nothing else should be looked up while it is.
    // Black full-screen quad drawn first (HD 0x02a13024; the dMenu_Fishing_c idiom).
    JKRArchive* main2D = dComIfGp_getMain2DArchive();
    auto* block = main2D != nullptr ? static_cast<ResTIMG*>(main2D->getResource('TIMG', "tt_block8x8.bti")) : nullptr;
    if (block == nullptr) {
        mods::log::error("stamp book: tt_block8x8.bti missing from main2D");
        return false;
    }
    mBlack = JKR_NEW J2DPicture(block);
    if (mBlack == nullptr) {
        return false;
    }
    mBlack->setBlackWhite(JUtility::TColor(0, 0, 0, 0), JUtility::TColor(0, 0, 0, 255));

    // HD 0x02a12438: the Collection/Letter select cursor (zelda_select_cursor_4parts.blo from
    // main2D) with the book's own parameters.
    mCursor = JKR_NEW dSelect_cursor_c(2, 1.0f, nullptr);
    if (mCursor == nullptr) {
        return false;
    }
    mCursor->setParam(0.88f, 0.93f, 0.1f, 0.7f, 0.875f);
    mCursor->setAlphaRate(1.0f);

    mArchive = assets::mount();
    if (mArchive == nullptr) {
        return false;
    }
    static constexpr const char* kLayouts[4] = {
        "zelda_stamps_background.blo",
        "zelda_stamps_menu.blo",
        "zelda_stamps_buttons.blo",
        "zelda_stamps_message.blo",
    };
    for (int i = 0; i < 4; ++i) {
        auto* screen = JKR_NEW J2DScreen();
        if (screen == nullptr) {
            mods::log::error("stamp book: out of 2D heap building {}", kLayouts[i]);
            return false;
        }
        mScreens[i] = screen;
        // The layouts' own visibility flags are kept (no dPaneClass_showNullPane): every pane they
        // hide is one refresh() drives.
        if (!screen->setPriority(kLayouts[i], kSetPriorityFlags, mArchive)) {
            mods::log::error("stamp book: {} could not be built", kLayouts[i]);
            return false;
        }
    }
    mBg = mScreens[0];
    mMenu = mScreens[1];
    mButtons = mScreens[2];
    mMsg = mScreens[3];

    // CPaneMgr with non-zero flags walks the pane's children in its constructor
    // (d_pane_class.cpp CPaneMgr::initiate), so the pane must exist first.
    if (find(mMsg, "n_all") == nullptr) {
        return false;
    }
    mMsgAll = JKR_NEW CPaneMgr(mMsg, pane_tag("n_all"), 2, nullptr);
    return mMsgAll != nullptr;
}

bool StampBook::lookup_panes() {
    bool ok = true;
    for (int k = 0; k < 10; ++k) {
        mSlot[k] = find(mMenu, fmt::format("N_it{:04}", k));
        mFrameNormal[k] = find_as<J2DPicture>(mMenu, fmt::format("it_i{:04}", k));
        // Row 1's selected frames are numbered backwards (layout and code agree, HD 0x02a13220).
        const int selected = k < 5 ? 10 + k : 24 - k;
        mFrameSelected[k] = find_as<J2DPicture>(mMenu, fmt::format("it_i{:04}", selected));
        mEmpty[k] = find_as<J2DPicture>(mMenu, fmt::format("it_e{:02}", k + 1));
        if (J2DPane* label = find(mMenu, fmt::format("it_t{:04}", k), false); label != nullptr) {
            label->hide(); // always hidden in HD (check C14)
        }
        ok = ok && mSlot[k] != nullptr && mFrameNormal[k] != nullptr && mFrameSelected[k] != nullptr &&
             mEmpty[k] != nullptr;
    }
    for (int s = 0; s < kStampCount; ++s) {
        mArt[s] = find_as<J2DPicture>(mMenu, fmt::format("it_s{:04}", s));
        ok = ok && mArt[s] != nullptr;
    }
    mInfo = find_as<J2DTextBox>(mMenu, "info_txt");
    mPageText = find_as<J2DTextBox>(mMenu, "page_txt");
    mPostPane = find(mMenu, "N_postbt");
    mPostImg = find_as<J2DPicture>(mMenu, "post_img");
    mPostIm1 = find_as<J2DPicture>(mMenu, "post_im1");
    mPostIcon = find_as<J2DPicture>(mMenu, "post_im2", false);
    mPostText = find_as<J2DTextBox>(mMenu, "post_txt", false);
    mTitle = find_as<J2DTextBox>(mBg, "title_tx");
    mAIcon = find_as<J2DPicture>(mButtons, "a_btn_cp");
    mAText = find_as<J2DTextBox>(mButtons, "cont_at");
    mATextShadow = find_as<J2DTextBox>(mButtons, "cont_at1", false);
    mBText = find_as<J2DTextBox>(mButtons, "cont_bt");
    mBTextShadow = find_as<J2DTextBox>(mButtons, "cont_bt1", false);
    mMsgRoot = find(mMsg, "n_all");
    mMsgText = find_as<J2DTextBox>(mMsg, "msg_text");
    ok = ok && mInfo != nullptr && mPageText != nullptr && mPostPane != nullptr && mPostImg != nullptr &&
         mPostIm1 != nullptr && mTitle != nullptr && mAIcon != nullptr && mAText != nullptr &&
         mBText != nullptr && mMsgRoot != nullptr && mMsgText != nullptr;
    if (!ok) {
        return false;
    }

    // Fonts: the layouts name rodan_24_22 / rodan_b_24_22 / reishotai_24_22, which no archive of
    // the book ships; HD sets them in code. The title uses the sub (reishotai) font like GC's
    // sub-window titles, every other box GC's message font (rodan_b_24_22).
    // Each get counts a reference (m_Do_ext.cpp); destroy() gives both back, as dMenu_Save does.
    JUTFont* mesgFont = mDoExt_getMesgFont();
    mMesgFontRef = true;
    mN43.clear();
    for (J2DScreen* screen : mScreens) {
        for_each_pane(screen, [&](J2DPane* pane) {
            if (pane->getTypeID() == kTypeTextBox && mesgFont != nullptr) {
                static_cast<J2DTextBox*>(pane)->setFont(mesgFont);
            }
        });
        for_each_kept_43(screen, false, [&](J2DPane* pane) {
            mN43.push_back({pane, pane->getScaleX(), pane->getScaleY()});
        });
    }
    JUTFont* subFont = mDoExt_getSubFont();
    mSubFontRef = true;
    if (subFont != nullptr) {
        mTitle->setFont(subFont);
    }

    // Post plate (check.md amendment 3): HD shows it and lets the cursor reach it; greyed and
    // hidden are config options only.
    if (mPost == PostButton::Hidden) {
        mPostPane->hide();
    } else if (mPost == PostButton::Greyed) {
        const JUtility::TColor black(0, 0, 0, 0);
        const JUtility::TColor grey(0x80, 0x80, 0x80, 0xFF);
        mPostImg->setBlackWhite(black, grey);
        if (mPostIcon != nullptr) {
            mPostIcon->setBlackWhite(black, grey);
        }
        if (mPostText != nullptr) {
            mPostText->setBlackWhite(black, grey);
        }
    }
    mMsgRoot->hide(); // only shown in the reward message step
    mMsgAll->setAlphaRate(0.0f);
    return true;
}

void StampBook::set_text(J2DTextBox* box, const std::string& text) {
    if (box == nullptr) {
        return;
    }
    TextSpan span = box->getStringPtr();
    if (span.buffer == nullptr || span.size <= text.size()) {
        JKRHeap* previous = mDoExt_setCurrentHeap(mHeap);
        box->setString(static_cast<s16>(std::max<size_t>(text.size() + 1, 64)), "");
        mDoExt_setCurrentHeap(previous);
        span = box->getStringPtr();
        if (span.buffer == nullptr || span.size == 0) {
            return;
        }
    }
    const size_t length = std::min(text.size(), span.size - 1);
    std::memcpy(span.buffer, text.data(), length);
    span.buffer[length] = '\0';
}

void StampBook::init_texts() {
    using namespace hdtext;
    // HD: title_tx = 2800, post_txt = 2803, msg_text = 2806 (overwriting the BLO defaults).
    set_text(mTitle, game_text_or(kGroupZel00, kMsgStampCollection, kFallbackTitle));
    set_text(mPostText, game_text_or(kGroupZel00, kMsgStampPost, kFallbackPost));
    set_text(mInfo, "");
    set_text(mPageText, "");
    set_text(mAText, "");
    set_text(mATextShadow, "");
    // B is always 1017 "Back", the game's own label in the game's language.
    for (J2DTextBox* box : {mBText, mBTextShadow}) {
        if (box != nullptr) {
            set_text(box, std::string(63, ' '));
            dMeter2Info_getStringKanji(kMsgBack, box->getStringPtr(), nullptr);
        }
    }
    // 2806 with HD's own two lines (the extracted copy is re-wrapped for GC's message window).
    const std::string reward = game_text(kGroupZel00, kMsgTwiliReward);
    set_text(mMsgText, reward.empty() ? std::string{kFallbackReward} : rejoin_soft_breaks(reward));
}

void StampBook::init_cursor() {
    // HD initCursor (0x02a12ba0): mode 3 starts on Twili Midna (page 5, row 1, col 4), the others
    // on the first owned stamp in index order (page 0, row 0, col 1 when none is owned). Never
    // remembered between openings.
    if (mMode == BookMode::Reward) {
        mPage = kTwiliMidnaIndex / 10;
        mRow = (kTwiliMidnaIndex % 10) / 5;
        mCol = kTwiliMidnaIndex % 5;
        return;
    }
    for (int s = 0; s < kStampCount; ++s) {
        if (owned(s)) {
            mPage = s / 10;
            mRow = (s % 10) / 5;
            mCol = s % 5;
            return;
        }
    }
    mPage = 0;
    mRow = 0;
    mCol = 1;
}

void StampBook::set_a_button(bool active) {
    const int state = active ? 1 : 0;
    if (mAButton == state) {
        return;
    }
    mAButton = state;
    // HD 0x02a12aec: label 0 empties the A text and greys the icon (white = 0x808080); on Post
    // the label is 2803 "Post" and the icon is drawn normally.
    mAIcon->setBlackWhite(JUtility::TColor(0, 0, 0, 0),
        active ? JUtility::TColor(255, 255, 255, 255) : JUtility::TColor(0x80, 0x80, 0x80, 0xFF));
    const std::string label =
        active ? hdtext::game_text_or(hdtext::kGroupZel00, hdtext::kMsgStampPost, kFallbackPost) : std::string{};
    set_text(mAText, label);
    set_text(mATextShadow, label);
}

void StampBook::refresh() {
    if (!mOpen) {
        return;
    }
    // HD refreshStampPanes (0x02a1251c).
    const bool postSelected = mRow == 2;
    const int selected = postSelected ? -1 : mRow * 5 + mCol;
    for (J2DPicture* art : mArt) {
        art->hide();
    }
    for (int k = 0; k < 10; ++k) {
        const int s = mPage * 10 + k;
        const bool isSelected = k == selected;
        bool showArt = owned(s);
        u8 alpha = 255;
        if (mMode == BookMode::Reward && s == kTwiliMidnaIndex && mStep == Step::RewardIntro) {
            // Hidden until the cross-fade (frames 31-60), the "?" shown meanwhile.
            if (mFrame <= kRewardFadeStart) {
                showArt = false;
            } else {
                alpha = static_cast<u8>(std::min(255, (mFrame - kRewardFadeStart) * 255 / kRewardFadeFrames));
            }
        }
        J2DPicture* art = mArt[s];
        if (showArt) {
            art->show();
            mEmpty[k]->hide();
        } else {
            mEmpty[k]->show();
        }
        const float scale = isSelected ? kSelectedScale : 1.0f;
        art->scale(scale, scale);
        // The selected stamp is drawn in black ink (corner colours 0,0,0,255), the others in the
        // texture's own brown (255,255,255,255).
        art->setCornerColor(isSelected ? JUtility::TColor(0, 0, 0, 255) : JUtility::TColor(255, 255, 255, 255));
        art->setAlpha(alpha);
        if (isSelected) {
            mFrameNormal[k]->hide();
            mFrameSelected[k]->show();
        } else {
            mFrameNormal[k]->show();
            mFrameSelected[k]->hide();
        }
        mFrameSelected[k]->scale(scale, scale);
    }
    if (mPost != PostButton::Hidden) {
        if (postSelected) {
            mPostIm1->show();
            mPostImg->hide();
        } else {
            mPostImg->show();
            mPostIm1->hide();
        }
    }
    // Name: 2890 + index for an owned stamp; empty for a missing one or on Post (HD 0x02a12188).
    const int stamp = postSelected ? -1 : mPage * 10 + selected;
    set_text(mInfo, stamp >= 0 && owned(stamp) ? stamp_name_game(stamp) : std::string{});
    // "Page %d/%d" with (page + 1, 5) (HD 0x02a1229c).
    const std::string pageFormat = hdtext::game_text_or(hdtext::kGroupZel00, hdtext::kMsgStampPage, kFallbackPage);
    set_text(mPageText, hdtext::format_counter(pageFormat, mPage + 1, kPages));
    set_a_button(postSelected);

    // HD 0x02a12438 places the cursor on the slot (or Post) pane. Dusklight's cursor re-centres
    // on that pane every draw (dSelect_cursor_c::update, TARGET_PC), so HD's (-1.75, -1.5) offset
    // from the pane centre is not reproduced.
    J2DPane* target = postSelected ? mPostPane : mSlot[selected];
    mCursor->setPos(0.0f, 0.0f, target, false);
}

void StampBook::close_with(uint32_t sound) {
    se(sound);
    mClose = true;
}

void StampBook::browse() {
    // HD proc0_browse (0x02a144c0): input only while the fader is idle.
    if (!fader_idle()) {
        return;
    }
    const bool a = mDoCPd_c::getTrigA(PAD_1) != 0;
    const bool b = mDoCPd_c::getTrigB(PAD_1) != 0;
    const bool l = mDoCPd_c::getTrigL(PAD_1) != 0;
    const bool r = mDoCPd_c::getTrigR(PAD_1) != 0;
    const bool start = mDoCPd_c::getTrigStart(PAD_1) != 0;

    if (mMode == BookMode::FirstStamp) {
        // View-only: any of the nine menu actions closes it with CURSOR_CANCEL (0x6E); the host's
        // close then plays SUB_OUT. dMw_c::_execute already ran mpStick->checkTrigger().
        bool any = a || b || l || r || start;
        if (mIgnoreHeldDirection && (mStick == nullptr || mStick->mDirectionTrig == 0)) {
            mIgnoreHeldDirection = false; // released: the next push counts
        }
        if (!any && mStick != nullptr && !mIgnoreHeldDirection) {
            any = mStick->checkLeftTrigger() || mStick->checkRightTrigger() || mStick->checkUpTrigger() ||
                  mStick->checkDownTrigger();
        }
        if (any) {
            close_with(Z2SE_SY_CURSOR_CANCEL);
        }
        return;
    }
    if (a) {
        // A acts only on Post. HD's Miiverse call (0x02b029d4) cannot succeed on PC (no nn_olv);
        // its failure path does nothing and plays nothing. On a stamp A does nothing either.
        return;
    }
    if (b) {
        mClose = true; // the host's close plays SUB_OUT (0xA5)
        return;
    }
    // HD proc0 (0x02a14880-0x02a14998): at most one action per frame, tested in the order Left,
    // Right, Up, Down, PageLeft (L), PageRight (R). Stick and D-pad (STControl reads both on PC).
    // No wrap; Left/Right never change page and are ignored on Post.
    bool moved = false;
    bool paged = false;
    if (mStick != nullptr && mStick->checkLeftTrigger()) {
        if (mRow < 2 && mCol > 0) {
            --mCol;
            moved = true;
        }
    } else if (mStick != nullptr && mStick->checkRightTrigger()) {
        if (mRow < 2 && mCol < 4) {
            ++mCol;
            moved = true;
        }
    } else if (mStick != nullptr && mStick->checkUpTrigger()) {
        if (mRow > 0) {
            --mRow; // from Post (col 4) back to row 1, col 4
            moved = true;
        }
    } else if (mStick != nullptr && mStick->checkDownTrigger()) {
        if (mRow == 0) {
            mRow = 1;
            moved = true;
        } else if (mRow == 1 && post_selectable()) {
            mRow = 2; // entering row 2 forces column 4 (the only Post cell)
            mCol = 4;
            moved = true;
        }
    } else if (l) {
        // L/R: page -/+ 1 wrapping over the 5 pages, the slot (or Post) is kept.
        mPage = (mPage + kPages - 1) % kPages;
        paged = true;
    } else if (r) {
        mPage = (mPage + 1) % kPages;
        paged = true;
    }
    if (paged) {
        se(Z2SE_SY_MENU_SUB_NEXT);
    } else if (moved) {
        se(Z2SE_SY_CURSOR_ITEM);
    }
    if (moved || paged) {
        refresh();
    }
}

void StampBook::reward_intro() {
    // HD 0x02a14b94, 30 Hz frames, counted only while the fader is idle (0x02a14bac: the fade-in
    // finishes first): sub BGM at 30, Twili Midna fades in over 31-60, message from 91. HD also
    // sets bit 49 there (redundant: the stamp was granted with the 49th) and gameInfo+0x637d = 1
    // at 30 (purpose unknown, not ported).
    if (!fader_idle()) {
        return;
    }
    ++mFrame;
    if (mFrame == kRewardBgmFrame) {
        mDoAud_subBgmStart(Z2BGM_ITEM_GET_POU);
    }
    if (mFrame > kRewardFadeStart && mFrame <= kRewardFadeStart + kRewardFadeFrames + 1) {
        refresh();
    }
    if (mFrame > kRewardMessageFrame) { // HD: cmpwi 0x5a / bgt (0x02a14cbc)
        mStep = Step::RewardMessage;
        mFrame = 0;
        refresh();
        mMsgRoot->show();
        mMsgAll->setAlphaRate(0.0f);
    }
}

void StampBook::reward_message() {
    // HD 0x02a14d24: the popup fades in over 30 frames; then A or B closes with CURSOR_CANCEL.
    if (mFrame < kMessageFadeFrames) {
        ++mFrame;
        mMsgAll->setAlphaRate(static_cast<f32>(mFrame) / kMessageFadeFrames);
        return;
    }
    if (!fader_idle()) {
        return;
    }
    if (mDoCPd_c::getTrigA(PAD_1) != 0 || mDoCPd_c::getTrigB(PAD_1) != 0) {
        close_with(Z2SE_SY_CURSOR_CANCEL);
    }
}

bool StampBook::move() {
    if (!mOpen) {
        return false;
    }
    if (!mClose) {
        switch (mStep) {
        case Step::Browse:
            browse();
            break;
        case Step::RewardIntro:
            reward_intro();
            break;
        case Step::RewardMessage:
            reward_message();
            break;
        }
    }
    return !mClose;
}

void StampBook::apply_scaling() {
    // GameCube: the 608x448 canvas as is (the black quad fills the side bars). Wii/Dusklight: the
    // canvas stretched like the Collection screen's, every "n_43" pane counter-scaled to keep its
    // shape (Dusklight compiles J2D's own n_43 handling out, VERSION=0).
    const bool wide = menu_scaling() != MenuScaling::GameCube && mDoGph_gInf_c::hudAspectScaleUp > 1.0001f;
    const f32 up = wide ? mDoGph_gInf_c::hudAspectScaleUp : 1.0f;
    const f32 down = wide ? mDoGph_gInf_c::hudAspectScaleDown : 1.0f;
    for (J2DScreen* screen : mScreens) {
        screen->scale(up, 1.0f);
        screen->translate(wide ? mDoGph_gInf_c::getSafeMinXF() : 0.0f, 0.0f);
    }
    for (const ScaledPane& entry : mN43) {
        entry.pane->scale(entry.scaleX * down, entry.scaleY);
    }
    mCursor->refreshAspectScale(up);
}

void StampBook::draw() {
    if (!mOpen) {
        return;
    }
    apply_scaling(); // follows a resolution change while the book is open
    J2DGrafContext* port = dComIfGp_getCurrentGrafPort();
    mBlack->setAlpha(255);
    mBlack->draw(mDoGph_gInf_c::getMinXF(), mDoGph_gInf_c::getMinYF(), mDoGph_gInf_c::getWidthF(),
        mDoGph_gInf_c::getHeightF(), false, false, false);
    port->setup2D();
    mBg->draw(0.0f, 0.0f, port);
    mMenu->draw(0.0f, 0.0f, port);
    mButtons->draw(0.0f, 0.0f, port);
    mCursor->draw();
    port->setup2D();
    mMsg->draw(0.0f, 0.0f, port);
}

void StampBook::destroy() {
    // HD teardown order: cursor and pane managers, the screens (each deletes its panes and
    // materials), the black quad, then the archive.
    if (mCursor != nullptr) {
        JKR_DELETE(mCursor);
        mCursor = nullptr;
    }
    if (mMsgAll != nullptr) {
        JKR_DELETE(mMsgAll);
        mMsgAll = nullptr;
    }
    for (J2DScreen*& screen : mScreens) {
        if (screen != nullptr) {
            JKR_DELETE(screen);
            screen = nullptr;
        }
    }
    if (mBlack != nullptr) {
        JKR_DELETE(mBlack);
        mBlack = nullptr;
    }
    assets::unmount(mArchive);
    release_fonts(); // after the text boxes that used them are gone
    forget_panes();
}

void StampBook::abandon() {
    if (mOpen || mCursor != nullptr || mBlack != nullptr || mArchive != nullptr) {
        mods::log::warn("stamp book: its heap was freed under it; dropping the book without destructors");
    }
    mCursor = nullptr;
    mMsgAll = nullptr;
    mScreens.fill(nullptr);
    mBlack = nullptr;
    // The archive object lives on the archive heap (book_assets mount), so it is still valid and
    // must leave the global volume list.
    assets::unmount(mArchive);
    release_fonts();
    forget_panes();
}

void StampBook::release_fonts() {
    if (mSubFontRef) {
        mDoExt_removeSubFont();
        mSubFontRef = false;
    }
    if (mMesgFontRef) {
        mDoExt_removeMesgFont();
        mMesgFontRef = false;
    }
}

void StampBook::forget_panes() {
    mBg = mMenu = mButtons = mMsg = nullptr;
    mSlot.fill(nullptr);
    mFrameNormal.fill(nullptr);
    mFrameSelected.fill(nullptr);
    mEmpty.fill(nullptr);
    mArt.fill(nullptr);
    mPostPane = nullptr;
    mPostImg = mPostIm1 = mPostIcon = nullptr;
    mPostText = mTitle = mInfo = mPageText = nullptr;
    mAIcon = nullptr;
    mAText = mATextShadow = mBText = mBTextShadow = nullptr;
    mMsgRoot = nullptr;
    mMsgText = nullptr;
    mN43.clear();
    mOpen = false;
    mClose = false;
    mHeap = nullptr;
    mStick = nullptr;
}

} // namespace hdstamps::book
