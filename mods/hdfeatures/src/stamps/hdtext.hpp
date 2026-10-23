#pragma once

// Twilight Princess HD's own stamp text, read at init from res/stamps/hdtext/messages.bin.
//
// The mod makes these records from the player's own HD copy (C++ port of tools/pipeline/hdtext.py) and stores
// them in its data folder (hdfiles/files.hpp); the text is never part of the source tree or the package. When
// the file is missing or invalid, every caller falls back to this mod's own wording.
//
// Header-only on purpose: mods/stamps/CMakeLists.txt lists the translation units, so the store
// lives in inline variables shared by items.cpp, ui.cpp and state.cpp.
//
// messages.bin (little-endian, schema version 1, see the docstring of tools/pipeline/hdtext.py):
//   'HDTX' u32 version u32 count, then per record, each starting 4-byte aligned:
//   u8 file u8 group u16 id u8 action u8 language u16 text_size u8 entry[20] u8 text[text_size]
// entry = the HD INF1 entry with bytes 0-5 (DAT1 offset + id) zeroed; text = GC-encoded DAT1 text
// (0x1A tags), terminated by exactly one NUL.

#include "stamp_data.hpp"

#include <mods/svc/log.hpp>
#include <mods/svc/message.h>
#include "hdfiles/files.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdstamps::hdtext {

inline constexpr char kPath[] = "stamps/hdtext/messages.bin"; // res/stamps/hdtext (HD Features layout)
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kMaxRecords = 4096;

// MessageService groups (dMsgObject_c::getMessageGroup in v2.0.3: ids <= 5000 are zel_00; larger
// ids live in the current stage's group, 4 = zel_04 for R_SP160).
inline constexpr uint16_t kGroupZel00 = 0;
inline constexpr uint16_t kGroupZel04 = 4;

// HD message ids. zel_00: the stamp UI strings and stamp names (HD's Collection screen).
inline constexpr uint16_t kMsgStampCollection = 2800; // book title
inline constexpr uint16_t kMsgStampCollectionHelp = 2801; // [A] help, blank line, "%d/%d" counter
inline constexpr uint16_t kMsgStampPage = 2802;   // book page_txt "Page %d/%d"
inline constexpr uint16_t kMsgStampPost = 2803;   // book Post plate / A label on Post
inline constexpr uint16_t kMsgTwiliReward = 2806; // shown when the 49th stamp unlocks Twili Midna
inline constexpr uint16_t kMsgFirstStampName = 2890; // + stamp index (HD book order)
// zel_04 (R_SP160): what the givers say.
inline constexpr uint16_t kMsgMaloMartThanks = 5465; // HD flow 21, before the stamp is handed over
inline constexpr uint16_t kMsgGengleGive = 5466;     // HD flow 1145, before the Rupee stamp
inline constexpr std::array<uint16_t, 3> kMsgAgithaLines = {5468, 5469, 6138}; // HD flow 1900 head

enum class Action : uint8_t {
    Native = 0,   // GC already shows this exact text
    Override = 1, // GC has an entry with this id (empty or other text)
    Register = 2, // GC has no usable entry: register a custom message
};

struct Message {
    uint8_t file = 0;
    uint8_t group = 0;
    uint16_t id = 0;
    Action action = Action::Native;
    uint8_t language = 0;
    MessageEntryData entry{};
    std::vector<uint8_t> text; // GC-encoded, ends with exactly one NUL
};

inline std::vector<Message> g_messages;
inline bool g_loaded = false;

// Same rules as the host's svc/flow.cpp valid_encoded_text(): override_message, an accepted
// override callback and register_message all reject text that fails them (an accepted callback
// with bad text fails the whole mod).
inline bool valid_encoded_text(const uint8_t* text, size_t size) {
    if (text == nullptr || size == 0) {
        return false;
    }
    size_t offset = 0;
    while (offset < size) {
        if (text[offset] == 0) {
            return offset + 1 == size;
        }
        if (text[offset] == 0x1A) {
            if (offset + 2 > size || text[offset + 1] < 5 || text[offset + 1] > size - offset) {
                return false;
            }
            const size_t tagSize = text[offset + 1];
            const bool colorTag =
                text[offset + 2] == 0xFF && text[offset + 3] == 0 && text[offset + 4] == 0;
            const size_t argumentSize = tagSize - 5;
            if (colorTag && argumentSize != 1 && argumentSize != 4 && argumentSize != 8) {
                return false;
            }
            offset += tagSize;
        } else {
            ++offset;
        }
    }
    return false;
}

namespace detail {

inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t le32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

inline bool parse(const uint8_t* data, size_t size, std::vector<Message>& out, std::string& why) {
    out.clear();
    if (data == nullptr || size < 12 || std::memcmp(data, "HDTX", 4) != 0) {
        why = "not an HDTX file";
        return false;
    }
    if (const uint32_t version = le32(data + 4); version != kVersion) {
        why = "schema version " + std::to_string(version) + " (expected 1)";
        return false;
    }
    const uint32_t count = le32(data + 8);
    if (count == 0 || count > kMaxRecords) {
        why = "bad record count " + std::to_string(count);
        return false;
    }
    out.reserve(count);
    size_t offset = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (size - offset < 28) {
            why = "record " + std::to_string(i) + " is truncated";
            return false;
        }
        const uint8_t* record = data + offset;
        const size_t textSize = le16(record + 6);
        if (textSize == 0 || size - offset - 28 < textSize) {
            why = "record " + std::to_string(i) + " text is out of range";
            return false;
        }
        Message message;
        message.file = record[0];
        message.group = record[1];
        message.id = le16(record + 2);
        message.language = record[5];
        if (record[4] > static_cast<uint8_t>(Action::Register) || message.group > 8 ||
            message.id >= 0x8000)
        {
            why = "record " + std::to_string(i) + " has a bad group, id or action";
            return false;
        }
        message.action = static_cast<Action>(record[4]);
        std::memcpy(message.entry.bytes, record + 8, sizeof(message.entry.bytes));
        // register_message() rejects entries whose DAT1 offset or id is set.
        if (std::any_of(message.entry.bytes, message.entry.bytes + 6, [](uint8_t b) { return b != 0; })) {
            why = "record " + std::to_string(i) + " has a non-zero entry offset/id";
            return false;
        }
        message.text.assign(record + 28, record + 28 + textSize);
        if (!valid_encoded_text(message.text.data(), message.text.size())) {
            why = "record " + std::to_string(i) + " (id " + std::to_string(message.id) +
                  ") has malformed text";
            return false;
        }
        out.push_back(std::move(message));
        offset += 28 + textSize;
        offset = std::min(size, (offset + 3) & ~size_t{3});
    }
    return true;
}

} // namespace detail

inline bool loaded() {
    return g_loaded;
}

// Loads res/stamps/hdtext/messages.bin once. Returns false (and logs why) when the file is missing or
// invalid; the mod then keeps its own wording everywhere.
inline bool load() {
    if (g_loaded) {
        return true;
    }
    g_messages.clear();
    std::vector<uint8_t> bytes;
    if (!hdfiles::load(kPath, bytes)) {
        mods::log::info("{}: using this mod's own wording", hdfiles::problem(kPath));
        return false;
    }
    std::vector<Message> parsed;
    std::string why;
    const bool ok = detail::parse(bytes.data(), bytes.size(), parsed, why);
    if (!ok) {
        mods::log::warn("res/{} is invalid ({}); using this mod's own wording", kPath, why);
        return false;
    }
    g_messages = std::move(parsed);
    g_loaded = true;
    mods::log::info("{} HD message(s) loaded from res/{}", g_messages.size(), kPath);
    return true;
}

inline void unload() {
    g_messages.clear();
    g_loaded = false;
}

// The record for (group, id), English preferred. nullptr when not loaded or not extracted.
inline const Message* find(uint16_t group, uint16_t id) {
    const Message* found = nullptr;
    for (const Message& message : g_messages) {
        if (message.group == group && message.id == id) {
            if (message.language == MESSAGE_LANGUAGE_ENGLISH) {
                return &message;
            }
            if (found == nullptr) {
                found = &message;
            }
        }
    }
    return found;
}

namespace detail {

// CP1252 0x80-0x9F (the GC/HD BMG files use encoding 1, a single-byte Windows-style charset).
inline constexpr std::array<char16_t, 32> kCp1252High = {
    0x20AC, 0x003F, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x003F, 0x017D, 0x003F,
    0x003F, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x003F, 0x017E, 0x0178,
};

inline void put_utf8(std::string& out, char16_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Button glyph tags rendered as text, as in the build tool's "preview" (d_msg_class.h MSGTAG_*).
inline const char* button_name(uint8_t group, uint16_t type) {
    if (group == 0) {
        switch (type) {
        case 10: return "A";
        case 11: return "B";
        case 12: return "C Stick";
        case 13: return "L";
        case 14: return "R";
        case 15: return "X";
        case 16: return "Y";
        case 17: return "Z";
        case 18: return "+Control Pad";
        case 19: return "Control Stick";
        default: return nullptr;
        }
    }
    return nullptr;
}

} // namespace detail

// Plain UTF-8 rendering of GC-encoded text for RmlUi: control tags dropped (button glyphs become
// "[A]" etc.), the message's own line breaks kept as '\n'.
inline std::string plain(const std::vector<uint8_t>& text) {
    std::string out;
    size_t offset = 0;
    while (offset < text.size() && text[offset] != 0) {
        const uint8_t c = text[offset];
        if (c == 0x1A) {
            if (offset + 5 > text.size() || text[offset + 1] < 5 ||
                text[offset + 1] > text.size() - offset)
            {
                break;
            }
            const uint8_t group = text[offset + 2];
            const auto type = static_cast<uint16_t>((text[offset + 3] << 8) | text[offset + 4]);
            if (const char* button = detail::button_name(group, type); button != nullptr) {
                out += '[';
                out += button;
                out += ']';
            }
            offset += text[offset + 1];
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0xA0) {
            detail::put_utf8(out, detail::kCp1252High[c - 0x80]);
        } else {
            detail::put_utf8(out, static_cast<char16_t>(c));
        }
        ++offset;
    }
    return out;
}

// The HD text for (group, id) as plain UTF-8, or `fallback` when it was not extracted.
inline std::string plain_or(uint16_t group, uint16_t id, std::string_view fallback) {
    const Message* message = find(group, id);
    return message != nullptr ? plain(message->text) : std::string{fallback};
}

// Splits plain text into paragraphs: lines are joined with a space (GC re-wrapped HD's lines to
// the message window, which means nothing in RmlUi) and blank lines separate paragraphs.
inline std::vector<std::string> paragraphs(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        while (!line.empty() && line.back() == ' ') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            if (!current.empty()) {
                current += ' ';
            }
            current += line;
        }
        start = end + 1;
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

// Replaces the first two "%d" of an HD format string (never passed to printf).
inline std::string format_counter(std::string_view format, int first, int second) {
    std::string out;
    int used = 0;
    for (size_t i = 0; i < format.size(); ++i) {
        if (used < 2 && format[i] == '%' && i + 1 < format.size() && format[i + 1] == 'd') {
            out += std::to_string(used == 0 ? first : second);
            ++used;
            ++i;
        } else {
            out += format[i];
        }
    }
    return out;
}

// This mod's neutral name for a stamp ("Stamp 12"), used when HD's text is not available.
inline std::string fallback_stamp_name(int index) {
    return "Stamp " + std::to_string(index + 1);
}

// HD's name of a stamp (2890 + index), or "Stamp N" without the HD text.
inline std::string stamp_name(int index) {
    if (index < 0 || index >= kStampCount) {
        return {};
    }
    if (const Message* message =
            find(kGroupZel00, static_cast<uint16_t>(kMsgFirstStampName + index));
        message != nullptr)
    {
        std::string name = plain(message->text);
        if (!name.empty()) {
            return name;
        }
    }
    return fallback_stamp_name(index);
}

// HD's book counter (the "%d/%d" paragraph of 2801) filled in, or "" without it.
inline std::string counter_text(int owned) {
    const Message* message = find(kGroupZel00, kMsgStampCollectionHelp);
    if (message == nullptr) {
        return {};
    }
    const std::vector<std::string> parts = paragraphs(plain(message->text));
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (it->find("%d") != std::string::npos) {
            return format_counter(*it, owned, kStampCount);
        }
    }
    return {};
}

// HD's 49-stamp reward notice (2806) as one paragraph, or "" without it.
inline std::string twili_reward_text() {
    const Message* message = find(kGroupZel00, kMsgTwiliReward);
    if (message == nullptr) {
        return {};
    }
    std::string out;
    for (const std::string& part : paragraphs(plain(message->text))) {
        if (!out.empty()) {
            out += ' ';
        }
        out += part;
    }
    return out;
}

// --- text for J2D text boxes (stamp book, Collection slot) --------------------------------------

// The message's own bytes in the game's single-byte encoding (what J2DTextBox prints), with the
// 0x1A control tags removed and its line breaks kept. "" when not extracted.
inline std::string game_text(uint16_t group, uint16_t id) {
    const Message* message = find(group, id);
    if (message == nullptr) {
        return {};
    }
    std::string out;
    const std::vector<uint8_t>& text = message->text;
    size_t offset = 0;
    while (offset < text.size() && text[offset] != 0) {
        if (text[offset] == 0x1A) {
            if (offset + 1 >= text.size() || text[offset + 1] < 5) {
                break;
            }
            offset += text[offset + 1];
            continue;
        }
        out += static_cast<char>(text[offset]);
        ++offset;
    }
    return out;
}

inline std::string game_text_or(uint16_t group, uint16_t id, std::string_view fallback) {
    std::string text = game_text(group, id);
    return text.empty() ? std::string{fallback} : text;
}

// tools/pipeline/hdtext.py re-wraps some HD messages to GC's message-window width (2806 becomes 3
// lines). A J2D text box shows HD's own layout: a line break is HD's only where the line ends a
// sentence, so the other (soft) breaks are joined back with a space.
inline std::string rejoin_soft_breaks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '\n') {
            out += c;
            continue;
        }
        const char last = out.empty() ? '\n' : out.back();
        const bool hard = last == '.' || last == '!' || last == '?' || last == ':' || last == '\n' ||
                          (i + 1 < text.size() && text[i + 1] == '\n');
        out += hard ? '\n' : ' ';
    }
    return out;
}

// Encoded text (0x1A tags kept) with the first two "%d" outside tags replaced, NUL-terminated.
inline std::vector<uint8_t> format_counter_encoded(const std::vector<uint8_t>& text, int first, int second) {
    std::vector<uint8_t> out;
    out.reserve(text.size() + 8);
    int used = 0;
    size_t offset = 0;
    while (offset < text.size() && text[offset] != 0) {
        const uint8_t c = text[offset];
        if (c == 0x1A && offset + 1 < text.size() && text[offset + 1] >= 5 &&
            offset + text[offset + 1] <= text.size())
        {
            out.insert(out.end(), text.begin() + static_cast<std::ptrdiff_t>(offset),
                text.begin() + static_cast<std::ptrdiff_t>(offset + text[offset + 1]));
            offset += text[offset + 1];
            continue;
        }
        if (used < 2 && c == '%' && offset + 1 < text.size() && text[offset + 1] == 'd') {
            const std::string number = std::to_string(used == 0 ? first : second);
            out.insert(out.end(), number.begin(), number.end());
            ++used;
            offset += 2;
            continue;
        }
        out.push_back(c);
        ++offset;
    }
    out.push_back(0);
    return out;
}

// HD's help sentence of the Collection screen (first paragraph of 2801), or "" without it.
inline std::string help_text() {
    const Message* message = find(kGroupZel00, kMsgStampCollectionHelp);
    if (message == nullptr) {
        return {};
    }
    const std::vector<std::string> parts = paragraphs(plain(message->text));
    return !parts.empty() && parts.front().find("%d") == std::string::npos ? parts.front() :
                                                                           std::string{};
}

// --- appending a page to a game message -------------------------------------------------------

// Lines of a GC-encoded text ('\n' outside tags; tag arguments may contain 0x0A bytes), or -1 when
// it is malformed or ends in a choice prompt (nothing can follow a selection).
inline int line_count(const uint8_t* text, size_t size) {
    if (!valid_encoded_text(text, size)) {
        return -1;
    }
    int lines = 1;
    size_t offset = 0;
    while (offset < size && text[offset] != 0) {
        if (text[offset] == 0x1A) {
            const uint8_t group = text[offset + 2];
            const auto type = static_cast<uint16_t>((text[offset + 3] << 8) | text[offset + 4]);
            // MSGTAG_SELECT_2WAY / _3WAY / AWAIT_CHOICE
            if (group == 0 && (type == 8 || type == 9 || type == 32)) {
                return -1;
            }
            offset += text[offset + 1];
            continue;
        }
        if (text[offset] == '\n') {
            ++lines;
        }
        ++offset;
    }
    return lines;
}

// out = original + blank lines up to the end of its last 4-line page + `page` (its own page, as
// in HD where it is a separate message). jmessage_tMeasureProcessor breaks pages every
// getLineMax() = 4 lines for talk / animal boxes outside Japan, and trailing blank lines do not
// count for centering. False if either text is unusable.
inline bool append_page(const uint8_t* original, size_t originalSize, const std::vector<uint8_t>& page,
    std::vector<uint8_t>& out) {
    constexpr int kLinesPerPage = 4;
    const int lines = line_count(original, originalSize);
    if (lines <= 0 || !valid_encoded_text(page.data(), page.size()) || line_count(page.data(), page.size()) <= 0)
    {
        return false;
    }
    const int pad = ((lines + kLinesPerPage - 1) / kLinesPerPage) * kLinesPerPage - lines + 1;
    out.assign(original, original + originalSize - 1); // without the NUL
    out.insert(out.end(), static_cast<size_t>(pad), static_cast<uint8_t>('\n'));
    out.insert(out.end(), page.begin(), page.end()); // ends with the NUL
    return valid_encoded_text(out.data(), out.size());
}

} // namespace hdstamps::hdtext
