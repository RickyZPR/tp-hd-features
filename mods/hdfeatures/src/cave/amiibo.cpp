// Cave of Shadows: the virtual Wolf Link amiibo's screens (see amiibo.hpp).

#include "stamps/prelude.hpp"  // library headers before any game header (see prelude.hpp)

#include "amiibo.hpp"

#include "hdparse.hpp"
#include "hdtext.hpp"
#include "hdfiles/files.hpp"

#include <mods/svc/log.hpp>
#include <mods/svc/ui.h>

#include "m_Do/m_Do_audio.h"

#include <dolphin/os.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace cave::amiibo {
namespace {

// Toast queue estimate, in milliseconds of the native (wall, steady) clock: the host times toasts with
// std::chrono::steady_clock (dusk/ui/ui.hpp), not with the game clock, which can be sped up (aurora
// time::set_scale). OSGetNativeTime is aurora's native_clock (exported; <chrono> itself cannot be used in
// a mod TU because the forced-include dolphin/types.h defines `nullptr` as 0 under MSVC's __cplusplus).
// The host's overlay shows one toast at a time, in arrival order, each for its duration plus the 0.2 s
// open and 0.2 s close transitions of overlay.rcss (dusk/ui/overlay.cpp, "done" on transitionend). Host
// toasts (autosave, achievements) and other mods' toasts are not visible to a mod: this covers its own.
using Millis = std::int64_t;
constexpr Millis kToastOverhead = 450;
constexpr std::uint32_t kDefaultToastMs = 5000;  // UiToastDesc duration_ms 0
constexpr std::uint32_t kPromptMs = 3000;        // toasts cannot be closed early: keep the prompt short
// How long after its push a prompt might still be waiting behind other toasts (the hide sheet is only
// applied when a prompt can still exist, so a closed menu costs no restyle otherwise).
constexpr Millis kPromptLinger = 20000;

Millis now_ms() {
    return static_cast<Millis>(OSTicksToMilliseconds(OSGetNativeTime()));
}

Millis g_toastBusyUntil = 0;   // the mod's queued toasts are done by then
Millis g_promptEnd = 0;        // the last prompt's estimated end
Millis g_promptLingerEnd = 0;  // the last prompt cannot exist any more after this
bool g_promptVisible = false;
UiStyleHandle g_promptHideStyle = 0;
bool g_promptHideWarned = false;

// Applied while the prompt must not be seen. Only `filter` is changed (it is one of the toast's own
// transition properties, so a showing prompt fades out over 0.2 s); `transform` still transitions on
// open/close, so the host's transitionend-driven toast lifecycle runs unchanged.
constexpr const char* kPromptHideRcss = R"RCSS(
toast[mod-id="local.ricky.hdfeatures"].cos-prompt,
toast[mod-id="local.ricky.hdfeatures"].cos-prompt[open] {
    filter: opacity(0);
    backdrop-filter: none;
}
)RCSS";

Millis note_toast(std::uint32_t durationMs) {
    const Millis start = std::max(now_ms(), g_toastBusyUntil);
    g_toastBusyUntil = start + static_cast<Millis>(durationMs != 0 ? durationMs : kDefaultToastMs) + kToastOverhead;
    return start;
}

// Scan screen timing, in game ticks (30 per second). HD drives its scan screen from code
// (amiibo.arc has no J2D animations, extractor report), so this pacing is the mod's own.
constexpr int kTouchTicks = 14;     // "touch your amiibo" before the figure is shown as read
constexpr int kReadTicks = 34;      // figure read -> result
constexpr int kSuccessTicks = 14;   // success flash before the screen closes by itself

enum class Stage : std::uint8_t { Closed, Reading, Success, Result };

struct Scan {
    UiDialogHandle dialog = 0;
    Stage stage = Stage::Closed;
    ScanMode mode = ScanMode::Enter;
    ScanResult result = ScanResult::Cancelled;
    int ticks = 0;
};

Scan g_scan;
bool g_art = false;
bool g_initialized = false;
UiStyleHandle g_overlayStyle = 0;
UiStyleHandle g_windowStyle = 0;

// Toasts are scoped to this mod (toast[mod-id], spelled out: hdfeatures.hpp static_asserts the id) and
// to the cave's toast types (cos-prompt, cos-record: the stamps' toasts have no type); the dialog/panel
// classes are cos-* only (the stamps use hds-*).
constexpr const char* kOverlayRcss = R"RCSS(
toast[mod-id="local.ricky.hdfeatures"].cos-prompt {
    top: auto;
    right: 44dp;
    bottom: 52dp;
    padding: 8dp 14dp;
    border-width: 0dp;
    box-shadow: rgba(0, 0, 0, 0%) 0dp 0dp 0dp 0dp;
    background-color: rgba(16, 12, 6, 72%);
}
toast[mod-id="local.ricky.hdfeatures"].cos-prompt heading { display: none; }
toast[mod-id="local.ricky.hdfeatures"].cos-prompt progress { display: none; }
.cos-prompt-row { display: flex; flex-flow: row; align-items: center; gap: 10dp; }
.cos-prompt-figure { width: 38dp; height: 39dp; }
.cos-prompt-logo { width: 92dp; height: 25dp; }
.cos-prompt-key {
    display: block;
    width: 30dp;
    height: 30dp;
    line-height: 30dp;
    text-align: center;
    font-weight: bold;
    color: #ffffff;
    background-color: #5d4fb3;
    border-radius: 15dp;
}
.cos-prompt-text { color: #f4ead2; font-weight: bold; }
toast[mod-id="local.ricky.hdfeatures"].cos-record { width: 380dp; }
.cos-rec-row { display: flex; flex-flow: row; gap: 8dp; }
.cos-rec-label { flex: 1 1 auto; }
.cos-rec-value { color: #f4ead2; }
.cos-rec-new { color: #ffd84a; font-weight: bold; }
.cos-rec-heart { width: 16dp; height: 15dp; }
.cos-rec-figure { width: 26dp; height: 27dp; }
)RCSS";

// Geometry-free part of the scan screen / panel styles; the geometry comes from layout.json.
constexpr const char* kWindowRcss = R"RCSS(
.cos-scan { display: block; position: relative; margin: 4dp auto; }
.cos-scan img { position: absolute; }
.cos-scan-glow { opacity: 0; }
.cos-scan-nfc { opacity: 0.9; }
.cos-scan-figure { opacity: 0; }
.cos-scan.reading .cos-scan-glow { animation: 0.45s cubic-in-out infinite alternate cos-glow-pulse; }
.cos-scan.touched .cos-scan-figure { opacity: 1; }
.cos-scan.touched .cos-scan-nfc { opacity: 0; }
.cos-scan.done .cos-scan-figure { opacity: 1; }
.cos-scan.done .cos-scan-nfc { opacity: 0; }
.cos-scan.done .cos-scan-glow { opacity: 1; }
.cos-scan.error .cos-scan-figure { opacity: 0; }
.cos-scan.error .cos-scan-glow { opacity: 0; }
.cos-scan-text {
    position: absolute;
    display: flex;
    flex-flow: column;
    justify-content: center;
    align-items: center;
    text-align: center;
    line-height: 1.3;
}
.cos-scan-text p { margin: 0dp 0dp 4dp 0dp; }
.cos-scan-plain { display: block; padding: 8dp 4dp; }
.cos-scan-plain p { margin: 0dp 0dp 6dp 0dp; }
@keyframes cos-glow-pulse {
    from { opacity: 0.1; }
    to { opacity: 0.95; }
}
.cos-record { display: block; }
.cos-record-title { font-weight: bold; margin-bottom: 4dp; }
.cos-rec-row { display: flex; flex-flow: row; gap: 8dp; }
.cos-rec-label { flex: 1 1 auto; }
.cos-rec-new { color: #d8a400; font-weight: bold; }
.cos-rec-heart { width: 16dp; height: 15dp; }
.cos-rec-figure { width: 26dp; height: 27dp; }
)RCSS";

// HD's zelda_amiibo_progress.blo (res/cave/amiibo/layout.json, 608x448 J2D units): the parchment popup is
// four mirrored quarters of `paper` (popup, popup4, popup5, popup6), the message box msg_text (dark brown,
// centred), the NFC prompt PICT_001 and its glow PICT_002 below the text. The numbers come from layout.json;
// the defaults below are this mod's own round approximations, used only when layout.json is missing.
struct Rect {
    float x0, y0, x1, y1;
};

struct Layout {
    Rect popup{64.0f, 112.0f, 544.0f, 336.0f};
    Rect text{112.0f, 160.0f, 496.0f, 296.0f};
    Rect glow{240.0f, 264.0f, 368.0f, 392.0f};
    Rect prompt{292.0f, 316.0f, 316.0f, 340.0f};
    float fontSize = 19.0f;
    std::string textColor = "#3c1e00";
};

constexpr float kScanWidthDp = 480.0f;  // width of the parchment in the dialog

bool rect_of(const hd::json::Value& pane, Rect& out) {
    const hd::json::Value* rect = pane.find("rect");
    if (rect == nullptr || !rect->is_array() || rect->items.size() != 4) {
        return false;
    }
    float v[4];
    for (int i = 0; i < 4; ++i) {
        if (!rect->items[i].is_number()) {
            return false;
        }
        v[i] = static_cast<float>(rect->items[i].number);
    }
    out = {v[0], v[1], v[2], v[3]};
    return out.x1 > out.x0 && out.y1 > out.y0;
}

Layout load_layout() {
    Layout layout;
    std::vector<std::uint8_t> bytes;
    if (!hdfiles::load("cave/amiibo/layout.json", bytes)) {
        return layout;
    }
    std::string why;
    hd::json::Value root;
    bool parsed = false;
    if (!bytes.empty()) {
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        parsed = hd::json::parse(text, root, &why);
    }
    if (!parsed) {
        mods::log::warn("res/cave/amiibo/layout.json unreadable ({}); using built-in scan screen geometry", why);
        return layout;
    }
    const hd::json::Value* screens = root.find("screens");
    const hd::json::Value* screen = screens != nullptr ? screens->find("zelda_amiibo_progress") : nullptr;
    const hd::json::Value* panes = screen != nullptr ? screen->find("panes") : nullptr;
    if (panes == nullptr || !panes->is_array()) {
        return layout;
    }
    Layout found = layout;
    bool havePopup = false;
    for (const hd::json::Value& pane : panes->items) {
        const hd::json::Value* name = pane.find("name");
        if (name == nullptr || !name->is_string()) {
            continue;
        }
        Rect r{};
        if (!rect_of(pane, r)) {
            continue;
        }
        const std::string& n = name->string;
        if (n == "popup" || n == "popup4" || n == "popup5" || n == "popup6") {
            if (!havePopup) {
                found.popup = r;
                havePopup = true;
            } else {
                found.popup = {std::min(found.popup.x0, r.x0), std::min(found.popup.y0, r.y0),
                    std::max(found.popup.x1, r.x1), std::max(found.popup.y1, r.y1)};
            }
        } else if (n == "msg_text") {
            found.text = r;
            const hd::json::Value* size = pane.find("font_size");
            if (size != nullptr && size->is_array() && !size->items.empty() && size->items[0].is_number()) {
                found.fontSize = static_cast<float>(size->items[0].number);
            }
            const hd::json::Value* color = pane.find("char_color");
            if (color != nullptr && color->is_string() && color->string.size() >= 7 && color->string[0] == '#') {
                found.textColor = color->string.substr(0, 7);
            }
        } else if (n == "PICT_002") {
            found.glow = r;
        } else if (n == "PICT_001") {
            found.prompt = r;
        }
    }
    if (!havePopup || found.popup.x1 - found.popup.x0 < 100.0f) {
        return layout;
    }
    return found;
}

// Absolute RCSS geometry for the scan screen from HD's layout, scaled so the parchment is kScanWidthDp
// wide, with the popup's top-left corner as the origin.
std::string scan_geometry_rcss(const Layout& l) {
    const float scale = kScanWidthDp / (l.popup.x1 - l.popup.x0);
    const auto box = [&](const char* selector, const Rect& r, float grow) {
        const float w = (r.x1 - r.x0) * scale * grow;
        const float h = (r.y1 - r.y0) * scale * grow;
        const float cx = ((r.x0 + r.x1) * 0.5f - l.popup.x0) * scale;
        const float cy = ((r.y0 + r.y1) * 0.5f - l.popup.y0) * scale;
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s { left: %.1fdp; top: %.1fdp; width: %.1fdp; height: %.1fdp; }\n",
            selector, cx - w * 0.5f, cy - h * 0.5f, w, h);
        return std::string(buf);
    };
    const float height = (std::max(l.popup.y1, l.glow.y1) - l.popup.y0) * scale;
    char head[160];
    std::snprintf(head, sizeof(head), ".cos-scan { width: %.1fdp; height: %.1fdp; }\n", kScanWidthDp, height);
    std::string rcss = head;
    rcss += box(".cos-scan-paper", l.popup, 1.0f);
    rcss += box(".cos-scan-glow", l.glow, 1.0f);
    rcss += box(".cos-scan-nfc", l.prompt, 1.0f);
    // The read figure appears inside the glow (half its size), where HD shows the NFC prompt.
    rcss += box(".cos-scan-figure", l.glow, 0.5f);
    rcss += box(".cos-scan-text", l.text, 1.0f);
    char font[120];
    std::snprintf(font, sizeof(font), ".cos-scan-text { color: %s; font-size: %.1fdp; }\n", l.textColor.c_str(),
        l.fontSize * scale);
    rcss += font;
    return rcss;
}

// res/cave/amiibo (HD Features layout: each feature has its own folder under res/).
const char* const kArtFiles[] = {"cave/amiibo/popup.png", "cave/amiibo/tt_input_glow.png",
    "cave/amiibo/tt_input_prompt.png", "cave/amiibo/amiibo_wolflink.png", "cave/amiibo/font_amiibo.png",
    "cave/amiibo/los_heart.png"};

void play(u32 sound) {
    mDoAud_seStart(sound, nullptr, 0, 0);
}

std::string scan_text(std::uint16_t id, const char* fallback) {
    return paragraphs(hdtext::text_or(hdtext::kZel00, id, fallback));
}

std::string scan_body(const char* state, const std::string& textRml) {
    if (!g_art) {
        return std::string("<div class=\"cos-scan-plain ") + state + "\">" + textRml + "</div>";
    }
    std::string body = std::string("<div class=\"cos-scan ") + state + "\">";
    body += image("popup.png", "cos-scan-paper");
    body += image("tt_input_glow.png", "cos-scan-glow");
    body += image("tt_input_prompt.png", "cos-scan-nfc");
    body += image("amiibo_wolflink.png", "cos-scan-figure");
    body += "<div class=\"cos-scan-text\">" + textRml + "</div></div>";
    return body;
}

std::string reading_body(bool touched) {
    const std::string text = scan_text(1567, "Scanning the Wolf Link amiibo...");
    return scan_body(touched ? "reading touched" : "reading", text);
}

std::string result_body(ScanResult result) {
    switch (result) {
    case ScanResult::EnterOk:
        return scan_body("done", "<p>" + escape(hdtext::text_or(hdtext::kZel00, 413, "Cave of Shadows")) + "</p>");
    case ScanResult::HealOk:
        return scan_body("done", scan_text(1549, "Hearts refilled."));  // the mod's own fallback wording
    case ScanResult::HealUsed:
        return scan_body("error",
            scan_text(1555, "The Wolf Link amiibo can be used here only once per visit to the cave."));
    case ScanResult::HealEmpty:
        return scan_body("error", scan_text(1572, "The Wolf Link amiibo has no hearts stored yet."));
    case ScanResult::WriteOkFirst:
        // The Record screen's write (HD callback 0x02a0d144): the long note once per save (save byte 0xABB).
        return scan_body("done", scan_text(1568, "Hearts stored in your Wolf Link amiibo. To get them back, scan "
                                                 "it on the cave's Collection screen."));
    case ScanResult::WriteOk:
        return scan_body("done", scan_text(1569, "Hearts stored in the Wolf Link amiibo."));
    case ScanResult::EnterRefused:
    case ScanResult::Unavailable:
    case ScanResult::Cancelled:
    default:
        return scan_body("error",
            scan_text(1530, "The cave can't be entered from here."));
    }
}

bool set_body(const std::string& body) {
    if (g_scan.dialog == 0) {
        return false;
    }
    return svc_ui->dialog_set_body(mod_ctx, g_scan.dialog, body.c_str()) == MOD_OK;
}

void finish(bool byPlayer) {
    const ScanResult result = g_scan.stage == Stage::Reading ? ScanResult::Cancelled : g_scan.result;
    g_scan = Scan{};
    cave::amiibo_scan_closed(result, byPlayer);
}

void on_close_pressed(ModContext*, UiDialogHandle dialog, void*) {
    if (dialog != g_scan.dialog || g_scan.stage == Stage::Closed) {
        return;
    }
    finish(true);  // the host closes the dialog after this callback (keep_open = false)
}

void on_dismiss(ModContext*, UiDialogHandle dialog, void*) {
    if (dialog != g_scan.dialog || g_scan.stage == Stage::Closed) {
        return;
    }
    finish(true);
}

}  // namespace

// =================================================================================================

ModResult initialize() {
    g_art = true;
    std::string artProblem;
    for (const char* file : kArtFiles) {
        if (g_art && !hdfiles::exists(file)) {
            g_art = false;
            artProblem = hdfiles::problem(file);  // missing, didn't pass the check, can't be read
        }
    }
    if (!g_art) {
        mods::log::warn("the amiibo art is not available ({}); the amiibo screens use plain text", artProblem);
    }
    if (svc_ui->register_styles(mod_ctx, UI_SCOPE_OVERLAY, kOverlayRcss, &g_overlayStyle) != MOD_OK) {
        g_overlayStyle = 0;
        mods::log::warn("could not register the amiibo prompt style");
    }
    const std::string windowRcss = std::string(kWindowRcss) + scan_geometry_rcss(load_layout());
    if (svc_ui->register_styles(mod_ctx, UI_SCOPE_WINDOW, windowRcss.c_str(), &g_windowStyle) != MOD_OK) {
        g_windowStyle = 0;
        mods::log::warn("could not register the amiibo screen style");
    }
    if (g_art && (g_overlayStyle == 0 || g_windowStyle == 0)) {
        // Unstyled <img> elements would show at their full texture size: use the plain screens instead.
        g_art = false;
        mods::log::warn("amiibo screens use plain text (style registration failed)");
    }
    g_initialized = true;
    return MOD_OK;
}

void shutdown() {
    cancel();
    if (g_promptHideStyle != 0) {
        svc_ui->unregister_styles(mod_ctx, g_promptHideStyle);
        g_promptHideStyle = 0;
    }
    g_promptVisible = false;
    if (g_overlayStyle != 0) {
        svc_ui->unregister_styles(mod_ctx, g_overlayStyle);
        g_overlayStyle = 0;
    }
    if (g_windowStyle != 0) {
        svc_ui->unregister_styles(mod_ctx, g_windowStyle);
        g_windowStyle = 0;
    }
    g_initialized = false;
}

bool art_available() {
    return g_art;
}

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string paragraphs(std::string_view plain) {
    std::string out;
    std::string current;
    std::size_t i = 0;
    const auto flush = [&] {
        if (!current.empty()) {
            out += "<p>" + escape(current) + "</p>";
            current.clear();
        }
    };
    while (i < plain.size()) {
        if (plain[i] == '\n') {
            std::size_t run = 0;
            while (i < plain.size() && plain[i] == '\n') {
                ++run;
                ++i;
            }
            if (run >= 2) {
                flush();
            } else if (!current.empty() && current.back() != ' ') {
                current.push_back(' ');
            }
            continue;
        }
        current.push_back(plain[i]);
        ++i;
    }
    flush();
    return out;
}

std::string image(const char* file, const char* cssClass) {
    if (!g_art) {
        return {};
    }
    // An absolute path into the stored files (hdfiles/files.hpp): RmlUi opens it as it is (S1 spike 1).
    const std::string src = hdfiles::image_src(std::string("cave/amiibo/") + file);
    if (src.empty()) {
        return {};
    }
    return std::string("<img class=\"") + cssClass + "\" src=\"" + src + "\"/>";
}

bool push_toast(const UiToastDesc& desc) {
    if (svc_ui->push_toast(mod_ctx, &desc) != MOD_OK) {
        return false;
    }
    note_toast(desc.duration_ms);
    return true;
}

void note_other_toast(std::uint32_t durationMs) {
    note_toast(durationMs);
}

bool prompt_ready() {
    return now_ms() >= g_toastBusyUntil;
}

bool prompt_pending() {
    return now_ms() < g_promptEnd;
}

void set_prompt_visible(bool visible) {
    if (visible == g_promptVisible) {
        return;
    }
    g_promptVisible = visible;
    if (visible) {
        if (g_promptHideStyle != 0) {
            svc_ui->unregister_styles(mod_ctx, g_promptHideStyle);
            g_promptHideStyle = 0;
        }
        return;
    }
    // Prompts are only pushed while visible, so after the linger window none can exist any more and
    // nothing needs hiding (no overlay restyle on menus opened without a prompt).
    if (g_promptHideStyle != 0 || now_ms() >= g_promptLingerEnd) {
        return;
    }
    if (svc_ui->register_styles(mod_ctx, UI_SCOPE_OVERLAY, kPromptHideRcss, &g_promptHideStyle) != MOD_OK) {
        g_promptHideStyle = 0;
        if (!g_promptHideWarned) {
            g_promptHideWarned = true;
            mods::log::warn("could not register the amiibo prompt's hide style; the prompt may outlast the menu");
        }
    }
}

bool show_prompt(const char* buttonLabel) {
    // Only while the Collection screen is up (the caller sets the visibility first) and the mod's own
    // toast queue is empty, so the prompt shows at once instead of after other notices.
    if (!g_initialized || !g_promptVisible || !prompt_ready()) {
        return false;
    }
    std::string body;
    if (g_art) {
        body = "<div class=\"cos-prompt-row\">" + image("amiibo_wolflink.png", "cos-prompt-figure") +
               image("font_amiibo.png", "cos-prompt-logo") + "<span class=\"cos-prompt-key\">" +
               escape(buttonLabel) + "</span></div>";
    } else {
        body = "<div class=\"cos-prompt-row\"><span class=\"cos-prompt-key\">" + escape(buttonLabel) +
               "</span><span class=\"cos-prompt-text\">amiibo</span></div>";
    }
    UiToastDesc desc = UI_TOAST_DESC_INIT;
    desc.type = "cos-prompt";
    desc.title_rml = nullptr;  // the heading is hidden by the cos-prompt style
    desc.body_rml = body.c_str();
    desc.duration_ms = kPromptMs;
    if (svc_ui->push_toast(mod_ctx, &desc) != MOD_OK) {
        return false;
    }
    const Millis start = note_toast(kPromptMs);
    g_promptEnd = start + static_cast<Millis>(kPromptMs) + kToastOverhead;
    g_promptLingerEnd = g_promptEnd + kPromptLinger;
    return true;
}

bool open_scan(ScanMode mode) {
    if (!g_initialized || g_scan.stage != Stage::Closed) {
        return false;
    }
    static std::string s_closeLabel;
    s_closeLabel = hdtext::text_or(hdtext::kZel00, 1263, "Close");
    const std::string body = reading_body(false);

    UiDialogAction close = UI_DIALOG_ACTION_INIT;
    close.label = s_closeLabel.c_str();
    close.on_pressed = on_close_pressed;
    UiDialogDesc desc = UI_DIALOG_DESC_INIT;
    desc.title = "amiibo";
    desc.body_rml = body.c_str();
    desc.actions = &close;
    desc.action_count = 1;
    desc.on_dismiss = on_dismiss;
    UiDialogHandle handle = 0;
    if (svc_ui->dialog_push(mod_ctx, &desc, &handle) != MOD_OK || handle == 0) {
        mods::log::warn("could not open the amiibo scan screen");
        return false;
    }
    g_scan = Scan{};
    g_scan.dialog = handle;
    g_scan.stage = Stage::Reading;
    g_scan.mode = mode;
    return true;
}

bool scan_open() {
    return g_scan.stage != Stage::Closed;
}

void tick() {
    if (g_scan.stage == Stage::Closed) {
        return;
    }
    ++g_scan.ticks;
    bool ok = true;
    switch (g_scan.stage) {
    case Stage::Reading:
        if (g_scan.ticks == kTouchTicks) {
            ok = set_body(reading_body(true));
            play(Z2SE_SY_LIGHT_POT_EQUIP);
        } else if (g_scan.ticks >= kReadTicks) {
            g_scan.result = cave::amiibo_scan_result(g_scan.mode);
            g_scan.ticks = 0;
            ok = set_body(result_body(g_scan.result));
            switch (g_scan.result) {
            case ScanResult::EnterOk:
                g_scan.stage = Stage::Success;
                play(Z2SE_WARP_MAP_DECIDE);
                break;
            case ScanResult::HealOk:
            case ScanResult::WriteOk:
            case ScanResult::WriteOkFirst:
                g_scan.stage = Stage::Result;
                play(Z2SE_SY_LIGHT_DROP_GET);
                break;
            default:
                g_scan.stage = Stage::Result;
                play(Z2SE_SYS_ERROR);
                break;
            }
        }
        break;
    case Stage::Success:
        if (g_scan.ticks >= kSuccessTicks) {
            const UiDialogHandle dialog = g_scan.dialog;
            g_scan = Scan{};  // cleared first: dialog_close fires no callback, but stay safe
            if (dialog != 0) {
                svc_ui->dialog_close(mod_ctx, dialog);
            }
            cave::amiibo_scan_closed(ScanResult::EnterOk, false);
            return;
        }
        break;
    case Stage::Result:
    case Stage::Closed:
        break;
    }
    if (!ok) {
        // The dialog went away without telling us (host closed it): treat as cancelled.
        mods::log::warn("amiibo scan screen lost; scan cancelled");
        g_scan = Scan{};
        cave::amiibo_scan_closed(ScanResult::Cancelled, false);
    }
}

void cancel() {
    if (g_scan.stage == Stage::Closed) {
        return;
    }
    const UiDialogHandle dialog = g_scan.dialog;
    g_scan = Scan{};
    if (dialog != 0) {
        svc_ui->dialog_close(mod_ctx, dialog);
    }
}

}  // namespace cave::amiibo
