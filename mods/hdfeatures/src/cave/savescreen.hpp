#pragma once

// Cave of Shadows: helpers shared by the two native J2D screens built from TP HD's save menu layouts
// (Layout/saveres.arc, converted by tools/pipeline/cosconfirm.py into res/cave/confirm/cosconfirm_gc.arc):
//   confirm.cpp  HD's pre-entry "Cave of Shadows" screen (dMenu_save_c use type 6)
//   record.cpp   HD's "Cave of Shadows Record" screen (dMenu_save_c use type 7, dLos_record_c)
// Header-only (inline): pane lookup, texts in the game's encoding, GC messages, frame-range animations.
// Include after "stamps/prelude.hpp" and the J2D / game headers below.

#include "hdtext.hpp"

#include <mods/svc/log.hpp>

#include "JSystem/J2DGraph/J2DAnimation.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JSupport/JSUList.h"
#include "JSystem/JUtility/JUTFader.h"
#include "d/d_meter2_info.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cave::savescreen {

constexpr std::uint64_t pane_tag(std::string_view name) {
    std::uint64_t value = 0;
    for (char c : name) {
        value = (value << 8) | static_cast<std::uint8_t>(c);
    }
    return value;
}

inline constexpr std::uint64_t kTagN43 = pane_tag("n_43");
inline constexpr u16 kTypePicture = 18;  // J2DPicture::getTypeID (J2DPictureEx inherits it)
inline constexpr u16 kTypeTextBox = 19;  // J2DTextBox::getTypeID (J2DTextBoxEx inherits it)
inline constexpr u32 kSetPriorityFlags = 0x20000;  // keep the resources, as the stamp book (HD blo2 in v2.0.3)

inline J2DPane* find(J2DScreen* screen, std::string_view name, bool required = true) {
    J2DPane* pane = screen != nullptr ? screen->search(pane_tag(name)) : nullptr;
    if (pane == nullptr && required) {
        mods::log::error("Cave of Shadows screen: pane '{}' not found in the HD layout", name);
    }
    return pane;
}

template <class T>
T* find_as(J2DScreen* screen, std::string_view name, bool required = true) {
    static_assert(std::is_same_v<T, J2DPicture> || std::is_same_v<T, J2DTextBox>);
    constexpr u16 kType = std::is_same_v<T, J2DPicture> ? kTypePicture : kTypeTextBox;
    J2DPane* pane = find(screen, name, required);
    if (pane != nullptr && pane->getTypeID() != kType) {
        // Review API #5: an optional pane of another type is skipped quietly (only required ones are errors).
        if (required) {
            mods::log::error("Cave of Shadows screen: pane '{}' has type {}, {} expected", name, pane->getTypeID(),
                kType);
        } else {
            mods::log::debug("Cave of Shadows screen: optional pane '{}' has type {}, skipped", name,
                pane->getTypeID());
        }
        return nullptr;
    }
    return static_cast<T*>(pane);
}

inline void hide(J2DScreen* screen, std::string_view name) {
    if (J2DPane* pane = find(screen, name, false); pane != nullptr) {
        pane->hide();
    }
}

// The outermost "n_43" panes of a screen (HD: their X scale is divided by the canvas stretch so they
// keep their 4:3 shape; Dusklight compiles J2D's own handling out, see stamps/stamp_book.cpp).
inline void collect_n43(J2DPane* pane, std::vector<J2DPane*>& out) {
    if (pane == nullptr) {
        return;
    }
    if (pane->getUserInfo() == kTagN43) {
        out.push_back(pane);
        return;
    }
    for (JSUTreeIterator<J2DPane> it = pane->getFirstChild(); it != pane->getEndChild(); ++it) {
        collect_n43(it.getObject(), out);
    }
}

// HD text is plain UTF-8 (cave/hdtext); the game's message font takes the 8-bit game encoding (Windows
// 1252 for the western versions). The strings used here are ASCII in English (and U+00D7 "x" in 854).
inline std::string to_game(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<std::uint8_t>(utf8[i]);
        if (c < 0x80) {
            if (c >= 0x20 || c == '\n') {
                out.push_back(static_cast<char>(c));
            }
            ++i;
            continue;
        }
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            ++i;
            continue;
        }
        if (i + len > utf8.size()) {
            break;
        }
        for (std::size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<std::uint8_t>(utf8[i + k]) & 0x3F);
        }
        i += len;
        switch (cp) {
        case 0x2018: case 0x2019: out.push_back('\''); break;
        case 0x201C: case 0x201D: out.push_back('"'); break;
        case 0x2013: case 0x2014: out.push_back('-'); break;
        case 0x2026: out += "..."; break;
        default: out.push_back(cp >= 0xA0 && cp <= 0xFF ? static_cast<char>(cp) : '?'); break;
        }
    }
    return out;
}

inline std::string hd_text(std::uint16_t id, const char* fallback) {
    return to_game(hdtext::text_or(hdtext::kZel00, id, fallback));
}

inline void set_text(J2DTextBox* box, const std::string& text, JKRHeap* heap) {
    if (box == nullptr) {
        return;
    }
    TextSpan span = box->getStringPtr();
    if (span.buffer == nullptr || span.size <= text.size()) {
        JKRHeap* previous = mDoExt_setCurrentHeap(heap);
        box->setString(static_cast<s16>(std::max<std::size_t>(text.size() + 1, 64)), "");
        mDoExt_setCurrentHeap(previous);
        span = box->getStringPtr();
        if (span.buffer == nullptr || span.size == 0) {
            return;
        }
    }
    const std::size_t length = std::min(text.size(), span.size - 1);
    std::memcpy(span.buffer, text.data(), length);
    span.buffer[length] = '\0';
}

// A GC message (in the game's own language) into a text box; `fallback` when it comes back empty.
inline void set_message(J2DTextBox* box, u32 messageId, const char* fallback, JKRHeap* heap) {
    if (box == nullptr) {
        return;
    }
    set_text(box, std::string(63, ' '), heap);
    TextSpan span = box->getStringPtr();
    if (span.buffer != nullptr && span.size > 0) {
        span.buffer[0] = '\0';
        dMeter2Info_getStringKanji(messageId, span, nullptr);
        if (span.buffer[0] != '\0') {
            return;
        }
    }
    set_text(box, fallback, heap);
}

// An HD "%d" message (855 "B%d", 854 "x%d") with a number (manual: the text comes from a data file, never a
// printf format).
inline std::string format_number(const std::string& format, int value) {
    std::string out;
    bool done = false;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] == '%') {
            if (!done && i + 1 < format.size() && format[i + 1] == 'd') {
                out += std::to_string(value);
                done = true;
                ++i;
            }
            continue;
        }
        out.push_back(format[i]);
    }
    if (!done) {
        out += std::to_string(value);
    }
    return out;
}

inline void se(u32 id) {
    mDoAud_seStart(id, nullptr, 0, 0);
}

inline bool fader_black() {
    JUTFader* fader = mDoGph_gInf_c::getFader();
    return fader == nullptr || fader->getStatus() == JUTFader::None;
}

inline bool fader_idle() {
    JUTFader* fader = mDoGph_gInf_c::getFader();
    return fader == nullptr || fader->getStatus() == JUTFader::Wait;
}

// One J2DAnmTransform instance playing a frame range on up to two pane subtrees, 2 frames per tick like
// GC's yesnoMenuMoveAnm (HD's save-menu helpers 0x02a07384 / 0x02a07b50 step the same way): done (and
// detached) on the call after the target frame was applied.
struct Anim {
    J2DAnmTransform* anm = nullptr;
    std::array<J2DPane*, 2> panes{};
    int frame = 0;
    int target = 0;
    bool active = false;

    void start(int from, int to, J2DPane* a, J2DPane* b = nullptr) {
        panes = {a, b};
        frame = from;
        target = to;
        active = anm != nullptr;
        apply();
    }
    void apply() {
        if (anm == nullptr) {
            return;
        }
        anm->setFrame(static_cast<f32>(frame));
        for (J2DPane* pane : panes) {
            if (pane != nullptr) {
                pane->setAnimation(anm);
                pane->animationTransform();
            }
        }
    }
    bool step() {
        if (!active) {
            return true;
        }
        if (frame == target) {
            for (J2DPane* pane : panes) {
                if (pane != nullptr) {
                    pane->setAnimation(static_cast<J2DAnmTransform*>(nullptr));
                }
            }
            active = false;
            return true;
        }
        frame = frame < target ? std::min(frame + 2, target) : std::max(frame - 2, target);
        apply();
        return false;
    }
    void reset() {
        anm = nullptr;
        panes = {};
        active = false;
    }
};

}  // namespace cave::savescreen
