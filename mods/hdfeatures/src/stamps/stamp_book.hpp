#pragma once

// The stamp book screen: a port of TP HD's dMenu_Stamp_c (Zelda.rpx 0x02a11898..0x02a15040,
// research/stampbook/code.md) on top of HD's own four layouts, converted to GC textures
// (res/stamps/stampbook/stampResR_gc.arc, tools/pipeline/stampbook.py).
//
// Screens (draw order, HD draw 0x02a14ef0): black quad, zelda_stamps_background,
// zelda_stamps_menu, zelda_stamps_buttons, select cursor, zelda_stamps_message.
//
// Modes (HD +0x44C):
//   0 Collection  opened from the Collection screen slot; B closes back to it
//   1 FileSelect  read-only view of a save file's own stamps from the file select (filesel.hpp)
//   2 FirstStamp  automatic after the 1st stamp: view-only, any input closes
//   3 Reward      automatic after the 49th stamp: Twili Midna intro + 2806 message
//
// Everything J2D is allocated on the JKR heap passed to create() (dMw_c::mpHeap, the 2D scene
// heap, or the file-select viewer's own heap) and released by destroy(), which the hosts always
// call before that heap is freed. The archive object lives on the archive heap.

#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "book.hpp"

#include "d/d_drawlist.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class CPaneMgr;
class J2DPane;
class J2DPicture;
class J2DScreen;
class J2DTextBox;
class JKRArchive;
class JKRHeap;
class STControl;
class dSelect_cursor_c;

namespace hdstamps::book {

enum class BookMode : uint8_t {
    Collection = 0,
    FileSelect = 1,
    FirstStamp = 2,
    Reward = 3,
};

class StampBook final : public dDlst_base_c {
public:
    // Builds the screens on `heap` (made current while building). False (and nothing left
    // allocated) on failure; the reason is logged.
    bool create(BookMode mode, uint64_t ownedMask, JKRHeap* heap, STControl* stick);
    void destroy();
    // The heap the book was built on was wiped (freeAll) without the J2D destructors running:
    // forgets every pointer into it without freeing anything, then releases what lives elsewhere
    // (the archive object on the archive heap, the font references).
    void abandon();
    bool is_open() const { return mOpen; }
    BookMode mode() const { return mMode; }

    // One game frame (called from the dMw_c state the host owns). Returns false once the book
    // asks to close; the host then plays SUB_OUT and fades.
    bool move();

    // dDlst_base_c: queued with dComIfGd_set2DOpa by the host's dMw_c::_draw post-hook.
    void draw() override;

    // Bytes of 2D heap the last successful create() used (0 before the first one).
    static int32_t last_heap_use() { return sLastHeapUse; }

private:
    struct ScaledPane {
        J2DPane* pane;
        float scaleX;
        float scaleY;
    };

    enum class Step : uint8_t {
        Browse = 0,        // HD step 0
        RewardIntro = 1,   // HD step 1 (mode 3)
        RewardMessage = 2, // HD step 2 (mode 3)
    };

    bool build();
    bool lookup_panes();
    void init_texts();
    void init_cursor();
    void refresh();
    void browse();
    void reward_intro();
    void reward_message();
    void apply_scaling();
    void set_text(J2DTextBox* box, const std::string& text);
    void set_a_button(bool active);
    bool owned(int stamp) const;
    bool post_selectable() const;
    void close_with(uint32_t sound);
    void forget_panes();
    void release_fonts();

    static inline int32_t sLastHeapUse = 0;

    bool mOpen = false;
    bool mClose = false;
    BookMode mMode = BookMode::Collection;
    Step mStep = Step::Browse;
    uint64_t mMask = 0;
    JKRHeap* mHeap = nullptr;
    STControl* mStick = nullptr;
    JKRArchive* mArchive = nullptr;
    PostButton mPost = PostButton::Hd;

    int mPage = 0;
    int mRow = 0; // 0-1 stamps, 2 = Post
    int mCol = 0;
    int mFrame = 0;    // frames in the current reward step
    int mAButton = -1; // last A-icon state written (-1 = none yet)
    // Mode 2: a stick/D-pad direction already held when the book became ready (the player was
    // walking) is not an input until it has been released once.
    bool mIgnoreHeldDirection = false;
    // One mDoExt_getMesgFont / mDoExt_getSubFont reference each, given back in destroy().
    bool mMesgFontRef = false;
    bool mSubFontRef = false;

    J2DScreen* mBg = nullptr;
    J2DScreen* mMenu = nullptr;
    J2DScreen* mButtons = nullptr;
    J2DScreen* mMsg = nullptr;
    J2DPicture* mBlack = nullptr;
    dSelect_cursor_c* mCursor = nullptr;
    CPaneMgr* mMsgAll = nullptr;

    std::array<J2DPane*, 10> mSlot{};
    std::array<J2DPicture*, 10> mFrameNormal{};
    std::array<J2DPicture*, 10> mFrameSelected{};
    std::array<J2DPicture*, 10> mEmpty{};
    std::array<J2DPicture*, 50> mArt{};
    J2DPane* mPostPane = nullptr;
    J2DPicture* mPostImg = nullptr;
    J2DPicture* mPostIm1 = nullptr;
    J2DPicture* mPostIcon = nullptr;
    J2DTextBox* mPostText = nullptr;
    J2DTextBox* mTitle = nullptr;
    J2DTextBox* mInfo = nullptr;
    J2DTextBox* mPageText = nullptr;
    J2DPicture* mAIcon = nullptr;
    J2DTextBox* mAText = nullptr;
    J2DTextBox* mATextShadow = nullptr;
    J2DTextBox* mBText = nullptr;
    J2DTextBox* mBTextShadow = nullptr;
    J2DPane* mMsgRoot = nullptr;
    J2DTextBox* mMsgText = nullptr;

    std::vector<ScaledPane> mN43; // user tag "n_43": kept 4:3 when the canvas is stretched
    std::array<J2DScreen*, 4> mScreens{};
};

} // namespace hdstamps::book
