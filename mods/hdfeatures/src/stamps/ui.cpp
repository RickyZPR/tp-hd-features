#include <mods/svc/log.hpp>
#include "hdfiles/files.hpp"
#include <mods/svc/ui.h>

#include "ui.hpp"

#include "book.hpp"
#include "feature.hpp"
#include "hdtext.hpp"
#include "items.hpp"
#include "state.hpp"

#include "hdfeatures.hpp" // src/hdfeatures.hpp: mod id, the shared window

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string>

namespace hdstamps::ui {
namespace {

// RCSS of the HD Features window (UiWindowDesc::rcss applies to that document only; src/panel.cpp
// passes window_rcss()). Divs default to inline in RmlUi, so every block gets an explicit display.
// Tiles use inline styles (see tile_rml). The window is shared with the Cave of Shadows tab, so the
// wide left pane is only for this feature's tabs: their build marks their left pane "hds-wide".
constexpr const char* kWindowRcss = R"RCSS(
window content pane.hds-wide { flex: 3 1 0; }
.hds-head { display: block; font-size: 26dp; font-weight: bold; }
.hds-sub { display: block; font-size: 16dp; opacity: 0.8; }
.hds-grid { display: flex; flex-flow: row wrap; width: 100%; margin-top: 8dp; }
.hds-cell { display: block; width: 10%; height: 66dp; padding: 2dp; overflow: hidden; }
.hds-big { display: block; margin: 0dp auto 12dp auto; }
.hds-title { display: block; font-size: 24dp; font-weight: bold; }
.hds-line { display: block; font-size: 18dp; }
.hds-note { display: block; font-size: 16dp; opacity: 0.75; }
.hds-ok { color: #8fe08f; }
.hds-no { color: #f0c070; }
)RCSS";

uint64_t g_version = 1; // bumped by notify_state_changed()
std::array<bool, kStampCount> g_png{};
int g_pngCount = 0;
bool g_framePng = false; // res/stamps/stamps/stamp_frame.png (HD stamp-book tile)
bool g_emptyPng = false; // res/stamps/stamps/stamp_empty.png (HD "unknown stamp" tile)

// Collection tab elements (valid until the tab is rebuilt).
UiElementHandle g_headElem = 0;
UiElementHandle g_progressElem = 0;
UiElementHandle g_gridElem = 0;
UiElementHandle g_summaryElem = 0;
uint64_t g_collectionVersion = 0;

// Mods panel elements (../panel.cpp builds the panel; this feature's part is build_panel and
// build_troubleshooting).
UiElementHandle g_panelStatus = 0;
UiElementHandle g_panelProgress = 0;
UiElementHandle g_panelHowTo = 0;
UiElementHandle g_panelListButton = 0;
UiElementHandle g_panelDiag = 0;
UiElementHandle g_panelBookDiag = 0;
uint64_t g_panelVersion = 0;
std::string g_lastStatus;
std::string g_lastDiag;
std::string g_lastHowTo;
std::string g_lastBookDiag;
int g_listButtonVisible = -1; // -1 = unknown

std::array<std::string, kStampCount> g_groupLabels;
std::string g_collectionTabTitle; // kept alive for the window's tab descriptor

float progress_value() {
    return slot_ready() ? static_cast<float>(owned_count()) / static_cast<float>(kStampCount) : 0.0f;
}

const char* source_text(const StampDef& def) {
    switch (def.source) {
    case StampSource::Chest:
        return "A chest (on GameCube it held something else)";
    case StampSource::HdChest:
        return "A chest that only exists in HD (added by this mod)";
    case StampSource::Gengle:
        return "A gift from Gengle, Jovani's cat";
    case StampSource::Agitha:
        return "A gift from Agitha";
    case StampSource::MaloMart:
        return "Your first purchase at the Castle Town Malo Mart";
    case StampSource::Automatic:
        return "Given to you once you have the other 49";
    }
    return "";
}

std::string header_rml() {
    if (!feature_enabled()) {
        return "<div class=\"hds-head\">Stamps are turned off</div>"
               "<div class=\"hds-sub\">Turn \"Stamps\" back on in the HD Features options to see them.</div>";
    }
    if (!mod_active()) {
        return "<div class=\"hds-head\">Stamps are off</div>"
               "<div class=\"hds-sub\">The stamps only work in the normal game mode, not in the "
               "randomizer, the speedrun mode or other mods' game modes.</div>";
    }
    if (!slot_ready()) {
        return "<div class=\"hds-head\">No save loaded</div>"
               "<div class=\"hds-sub\">Load a save file to see its stamps.</div>";
    }
    // HD's Collection screen: title 2800 and the found/total line of 2801.
    const std::string counter = hdtext::counter_text(owned_count());
    if (!counter.empty()) {
        return fmt::format("<div class=\"hds-head\">{}</div><div class=\"hds-sub\">{}</div>",
            escape_rml(hdtext::plain_or(hdtext::kGroupZel00, hdtext::kMsgStampCollection, "Stamps")
                           .c_str()),
            escape_rml(counter.c_str()));
    }
    return fmt::format("<div class=\"hds-head\">{} / {} stamps collected</div>", owned_count(),
        kStampCount);
}

std::string image_src(const std::string& file) {
    // An absolute path into the stored game files (hdfiles/files.hpp, RML-escaped): RmlUi opens it as it is
    // (S1 spike 1). The callers only ask for images whose existence flag is set (scan_pngs).
    return hdfiles::image_src("stamps/stamps/" + file);
}

std::string abs_img(const std::string& file, int left, int top, int size, float opacity) {
    return fmt::format("<img src=\"{}\" style=\"position: absolute; left: {}dp; top: {}dp; width: "
                       "{}dp; height: {}dp; opacity: {:.2f};\"/>",
        image_src(file), left, top, size, size, opacity);
}

// The text on a tile when the stamp PNGs are not bundled: the letter of stamps 0-25 (the alphabet
// stamps, A-Z in index order), else the stamp's name (HD's, or "Stamp N").
std::string grid_label(int index) {
    if (index >= 0 && index < 26) {
        return std::string(1, static_cast<char>('A' + index));
    }
    return hdtext::stamp_name(index);
}

// One stamp-book tile with inline styles only, so it renders the same in the window and in
// toasts (overlay scope, where the window RCSS does not apply): HD's frame, then the stamp art
// (owned), HD's "?" tile (missing) or a text label when the PNGs are not bundled.
std::string tile_rml(int index, int size, bool owned, bool showNumber) {
    const int inset = size * 16 / 100;
    const int art = size - 2 * inset;
    std::string out = fmt::format(
        "<div style=\"display: block; position: relative; width: {0}dp; height: {0}dp; margin: 0dp "
        "auto;\">",
        size);
    if (g_framePng) {
        out += abs_img("stamp_frame.png", 0, 0, size, owned ? 1.0f : 0.45f);
    } else {
        out += fmt::format("<div style=\"display: block; position: absolute; left: 0dp; top: 0dp; "
                           "width: {0}dp; height: {0}dp; border-width: 2dp; border-radius: 6dp; "
                           "border-color: #7a5a30; background-color: {1};\"></div>",
            size, owned ? "#f4ecd6" : "#f4ecd630");
    }
    if (owned && g_png[index]) {
        out += abs_img(fmt::format("stamp{:02}.png", index), inset, inset, art, 1.0f);
    } else if (!owned && g_emptyPng) {
        out += abs_img("stamp_empty.png", inset, inset, art, 0.8f);
    } else {
        out += fmt::format("<span style=\"position: absolute; left: 0dp; top: {}dp; width: {}dp; "
                           "text-align: center; font-size: {}dp; color: {};\">{}</span>",
            size / 2 - size / 8, size, std::max(10, size / 5), owned ? "#3a2a10" : "#ffffffa0",
            owned ? escape_rml(grid_label(index).c_str()) : std::string{"?"});
    }
    if (showNumber) {
        out += fmt::format("<span style=\"position: absolute; right: {0}dp; bottom: {0}dp; "
                           "font-size: 11dp; color: {1};\">{2:02}</span>",
            std::max(2, size / 12), owned ? "#5a3a10" : "#ffffffa0", index + 1);
    }
    out += "</div>";
    return out;
}

std::string grid_rml() {
    std::string out = "<div class=\"hds-grid\">";
    const bool ready = slot_ready();
    for (int i = 0; i < kStampCount; ++i) {
        out += "<div class=\"hds-cell\">";
        out += tile_rml(i, 58, ready && is_owned(i), true);
        out += "</div>";
    }
    out += "</div>";
    return out;
}

const char* yes_no(bool value) {
    return value ? "<span class=\"hds-ok\">yes</span>" : "<span class=\"hds-no\">not yet</span>";
}

std::string summary_rml() {
    if (!slot_ready()) {
        return "<div class=\"hds-note\">Load a save file to see its stamps.</div>";
    }
    int chests = 0;
    int chestsOwned = 0;
    int hdChests = 0;
    int hdOwned = 0;
    for (const StampDef& def : kStamps) {
        if (def.source == StampSource::Chest || def.source == StampSource::HdChest) {
            ++chests;
            chestsOwned += is_owned(def.index) ? 1 : 0;
        }
        if (def.source == StampSource::HdChest) {
            ++hdChests;
            hdOwned += is_owned(def.index) ? 1 : 0;
        }
    }
    std::string out;
    out += fmt::format("<div class=\"hds-line\">From chests: {} / {}</div>", chestsOwned, chests);
    out += fmt::format("<div class=\"hds-note\">({} / {} from chests that only exist in HD)</div>",
        hdOwned, hdChests);
    out += fmt::format("<div class=\"hds-line\">Gengle: {}</div>", yes_no(is_owned(kGengleStampIndex)));
    out += fmt::format("<div class=\"hds-line\">Agitha: {}</div>", yes_no(is_owned(kAgithaStampIndex)));
    out += fmt::format("<div class=\"hds-line\">Malo Mart: {}</div>", yes_no(is_owned(kMaloMartStampIndex)));
    out += fmt::format("<div class=\"hds-line\">Twili Midna: {}</div>", yes_no(is_owned(kTwiliMidnaIndex)));
    if (const std::string reward = hdtext::twili_reward_text();
        !reward.empty() && is_owned(kTwiliMidnaIndex))
    {
        out += fmt::format("<div class=\"hds-note hds-ok\">{}</div>", escape_rml(reward.c_str()));
    }
    out += "<br/><div class=\"hds-note\">Faded squares are stamps you don't have yet. The Stamp "
           "checklist tab has a hint for each one.</div>";
    if (!items::hd_text_loaded()) {
        out += "<br/><div class=\"hds-note\">HD's own text (res/stamps/hdtext) could not be loaded from HD "
               "Features' game files, so stamps use this mod's wording.</div>";
    }
    if (items::model_archive_invalid()) {
        out += "<br/><div class=\"hds-note\">The stamp model (overlay/res/Object/Obj_Stamp.arc) is "
               "damaged, so stamp chests show a stand-in model.</div>";
    } else if (!items::model_archive_found()) {
        out += "<br/><div class=\"hds-note\">The stamp model (overlay/res/Object/Obj_Stamp.arc) is "
               "missing, so stamp chests show a stand-in model.</div>";
    }
    if (g_pngCount < kStampCount) {
        out += fmt::format("<br/><div class=\"hds-note\">Only {} of {} stamp pictures are in the mod "
                           "(res/stamps/stamps/stampNN.png); the rest show their names.</div>",
            g_pngCount, kStampCount);
    }
    return out;
}

std::string detail_rml(int index) {
    const StampDef& def = kStamps[index];
    const bool owned = slot_ready() && is_owned(index);
    std::string out;
    out += fmt::format("<div class=\"hds-big\">{}</div>", tile_rml(index, 168, owned, false));
    out += fmt::format("<div class=\"hds-title\">#{:02} {}</div>", index + 1,
        escape_rml(hdtext::stamp_name(index).c_str()));
    out += fmt::format("<div class=\"hds-line\">{}</div>",
        owned ? "<span class=\"hds-ok\">Collected</span>" : "<span class=\"hds-no\">Not collected yet</span>");
    out += fmt::format("<br/><div class=\"hds-line\">{}</div>", escape_rml(def.hint));
    out += fmt::format("<br/><div class=\"hds-note\">How to get it: {}</div>", escape_rml(source_text(def)));
    if (def.stage != nullptr) {
        out += fmt::format("<div class=\"hds-note\">Map data: stage {}, room {}, chest {}</div>",
            escape_rml(def.stage), def.room, def.tboxNo);
    }
    return out;
}

std::string panel_status_text() {
    if (hdfeatures::feature_waits_for_files(hdfeatures::Feature::Stamps)) {
        return "Waiting for the game files."; // wave3 F5: the switch is on; "Game files" above says why
    }
    if (!feature_enabled()) {
        return "Stamps are off. Your stamps are kept.";
    }
    if (!feature_started()) {
        return "Starting...";
    }
    if (!mod_active()) {
        return "Off: works in the normal game mode only.";
    }
    if (!slot_ready()) {
        return "Load a save to see its stamps.";
    }
    if (const std::string counter = hdtext::counter_text(owned_count()); !counter.empty()) {
        return counter; // HD's found/total line (2801)
    }
    return fmt::format("{} of {} stamps found in this save.", owned_count(), kStampCount);
}

// How to look at the stamps: HD's book on the Collection screen, or the stamp list when that is not in
// use. Updated live (the switch, the book's state and the options can change while the panel is shown).
std::string howto_text() {
    if (!feature_enabled() || !feature_started() || book::integrated()) {
        return "Stamp book: the stamp slot on the Collection screen, right of the Letters.";
    }
    return "HD's stamp book isn't available (see Troubleshooting), so use the stamp list below.";
}

std::string book_diag_text() {
    return feature_started() ? book::diagnostics() : std::string{};
}

std::string panel_diag_text() {
    if (!feature_started()) {
        return hdfeatures::feature_waits_for_files(hdfeatures::Feature::Stamps) ?
                   "Stamps: not started (waiting for the game files)." :
                   "Stamps: not started (switched off since the game started).";
    }
    std::string problems;
    const auto add = [&](const std::string& what) {
        problems += problems.empty() ? " " : ", ";
        problems += what;
    };
    if (!items::model_archive_found()) {
        add(items::model_archive_invalid() ? "stamp model damaged" : "stamp model missing");
    }
    if (items::custom_icon_count() == 0) {
        add("item-get icons missing");
    }
    if (g_pngCount < kStampCount) {
        add(fmt::format("{} of {} stamp pictures", g_pngCount, kStampCount));
    }
    if (items::hooks_installed() < items::hooks_wanted()) {
        add(fmt::format("{} of {} game hooks", items::hooks_installed(), items::hooks_wanted()));
    }
    if (!items::hd_text_loaded()) {
        add("HD text missing");
    } else if (items::agitha_dialogue_unavailable()) {
        add("Agitha's HD lines unavailable");
    }
    return problems.empty() ? std::string("Stamps: OK.") : "Stamps: problems (see the log):" + problems + ".";
}

void set_text_if_changed(UiElementHandle elem, std::string& last, std::string value) {
    if (elem == 0 || value == last) {
        return;
    }
    if (svc_ui->elem_set_text(mod_ctx, elem, value.c_str()) == MOD_OK) {
        last = std::move(value);
    }
}

ModResult build_detail(ModContext* ctx, UiElementHandle pane, void* userData, ModError*) {
    const auto index = static_cast<int>(reinterpret_cast<intptr_t>(userData));
    if (index >= 0 && index < kStampCount) {
        svc_ui->pane_add_rml(ctx, pane, detail_rml(index).c_str(), nullptr);
    }
    return MOD_OK;
}

void scan_pngs() {
    g_pngCount = 0;
    for (int i = 0; i < kStampCount; ++i) {
        const std::string path = fmt::format("stamps/stamps/stamp{:02}.png", i);
        g_png[i] = hdfiles::exists(path);
        g_pngCount += g_png[i] ? 1 : 0;
    }
    g_framePng = hdfiles::exists("stamps/stamps/stamp_frame.png");
    g_emptyPng = hdfiles::exists("stamps/stamps/stamp_empty.png");
    if (g_pngCount < kStampCount) {
        mods::log::warn("{} of {} stamp images found under res/stamps/stamps; the book shows labels for the "
                        "rest",
            g_pngCount, kStampCount);
    }
}

} // namespace

// --- HD Features window tabs (../panel.cpp builds the window) ---------------------------------

const char* window_rcss() {
    return kWindowRcss;
}

const char* collection_tab_title() {
    g_collectionTabTitle =
        hdtext::plain_or(hdtext::kGroupZel00, hdtext::kMsgStampCollection, "Stamps");
    return g_collectionTabTitle.c_str();
}

ModResult build_collection(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    g_headElem = g_progressElem = g_gridElem = g_summaryElem = 0;
    svc_ui->elem_set_class(ctx, left, "hds-wide", true); // the 10x5 grid needs the wide left pane
    svc_ui->pane_add_rml(ctx, left, header_rml().c_str(), &g_headElem);
    svc_ui->pane_add_progress(ctx, left, progress_value(), &g_progressElem);
    svc_ui->pane_add_rml(ctx, left, grid_rml().c_str(), &g_gridElem);
    svc_ui->pane_add_section(ctx, right, "Summary");
    svc_ui->pane_add_rml(ctx, right, summary_rml().c_str(), &g_summaryElem);
    g_collectionVersion = g_version;
    return MOD_OK;
}

ModResult update_collection(ModContext* ctx, void*, ModError*) {
    if (g_collectionVersion == g_version) {
        return MOD_OK;
    }
    g_collectionVersion = g_version;
    if (g_headElem != 0) {
        svc_ui->elem_set_rml(ctx, g_headElem, header_rml().c_str());
    }
    if (g_progressElem != 0) {
        svc_ui->elem_set_progress(ctx, g_progressElem, progress_value());
    }
    if (g_gridElem != 0) {
        svc_ui->elem_set_rml(ctx, g_gridElem, grid_rml().c_str());
    }
    if (g_summaryElem != 0) {
        svc_ui->elem_set_rml(ctx, g_summaryElem, summary_rml().c_str());
    }
    return MOD_OK;
}

ModResult build_checklist(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    svc_ui->elem_set_class(ctx, left, "hds-wide", true);
    const bool ready = slot_ready();
    for (int i = 0; i < kStampCount; ++i) {
        g_groupLabels[i] = fmt::format("{:02}  {}{}", i + 1, hdtext::stamp_name(i),
            ready && is_owned(i) ? "   (collected)" : "");
        UiGroupDesc group = UI_GROUP_DESC_INIT;
        group.label = g_groupLabels[i].c_str();
        group.build = build_detail;
        group.user_data = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        svc_ui->pane_add_group(ctx, left, right, &group, nullptr);
    }
    svc_ui->pane_add_rml(ctx, right,
        "<div class=\"hds-note\">Pick a stamp on the left to see where it is.</div>", nullptr);
    return MOD_OK;
}

void window_closed() {
    g_headElem = g_progressElem = g_gridElem = g_summaryElem = 0;
}

// --- Mods panel part (../panel.cpp builds the panel) -------------------------------------------

void build_panel(UiElementHandle panel) {
    ModContext* ctx = mod_ctx;
    g_panelStatus = g_panelProgress = g_panelHowTo = g_panelListButton = 0;
    g_panelDiag = g_panelBookDiag = 0;
    g_lastStatus.clear();
    g_lastDiag.clear();
    g_lastHowTo.clear();
    g_lastBookDiag.clear();
    g_listButtonVisible = -1;

    // The section is "Stamps" (the HD Features switch of the same name sits above it); HD's own name
    // for the collection (2800) is the window tab's title.
    svc_ui->pane_add_section(ctx, panel, "Stamps");
    g_lastStatus = panel_status_text();
    svc_ui->pane_add_text(ctx, panel, g_lastStatus.c_str(), &g_panelStatus);
    svc_ui->pane_add_progress(ctx, panel, progress_value(), &g_panelProgress);

    // HD has no separate stamp menu: the book is the stamp slot on the Collection screen. The old
    // RmlUi list is only offered when that is unavailable, or as the debug option.
    g_lastHowTo = howto_text();
    svc_ui->pane_add_text(ctx, panel, g_lastHowTo.c_str(), &g_panelHowTo);
    UiControlDesc open = UI_CONTROL_DESC_INIT;
    open.kind = UI_CONTROL_BUTTON;
    open.label = book::integrated() ? "Open the stamp list (debug)" : "Open the stamp list";
    open.tooltip = "A list of all 50 stamps with a hint for each one. Not part of HD.";
    open.on_pressed = [](ModContext*, void*) { open_stamp_book(); };
    if (svc_ui->pane_add_control(ctx, panel, &open, &g_panelListButton) != MOD_OK) {
        g_panelListButton = 0;
    }
    if (g_panelListButton != 0) {
        g_listButtonVisible = stamp_list_offered() ? 1 : 0;
        svc_ui->elem_set_visible(ctx, g_panelListButton, g_listButtonVisible != 0);
    }

    svc_ui->pane_add_text(ctx, panel,
        "Found in chests, and from Agitha, Jovani's cat Gengle and the Castle Town Malo Mart.", nullptr);

    // The Mods window has no help pane, so help_rml would not render here: each option's help is
    // its tooltip (shown on focus or hover). SELECT controls need a help pane, hence the DROPDOWN.
    const auto add_toggle = [&](ConfigVarHandle var, const char* label, const char* help) {
        if (var == 0) {
            return;
        }
        UiControlDesc toggle = UI_CONTROL_DESC_INIT;
        toggle.kind = UI_CONTROL_TOGGLE;
        toggle.label = label;
        toggle.tooltip = help;
        toggle.binding = UI_BINDING_CONFIG_VAR;
        toggle.config_var = var;
        svc_ui->pane_add_control(ctx, panel, &toggle, nullptr);
    };
    svc_ui->pane_add_section(ctx, panel, "Stamps: options");
    add_toggle(book::auto_open_config_var(), "Open the stamp book by itself",
        "Like HD: the stamp book opens by itself after your 1st and 49th stamp.");
    if (book::post_button_config_var() != 0) {
        // Same order as book::PostButton (Hd, Greyed, Hidden).
        static constexpr const char* kPostOptions[] = {"Shown (HD)", "Greyed out", "Hidden"};
        UiControlDesc post = UI_CONTROL_DESC_INIT;
        post.kind = UI_CONTROL_DROPDOWN;
        post.label = "Post button";
        post.tooltip = "HD's Miiverse Post button in the stamp book. Miiverse is gone, so it does nothing.";
        post.binding = UI_BINDING_CONFIG_VAR;
        post.config_var = book::post_button_config_var();
        post.options = kPostOptions;
        post.option_count = 3;
        svc_ui->pane_add_control(ctx, panel, &post, nullptr);
    }
    add_toggle(book::filesel_config_var(), "Stamp count on the file select",
        "Like HD: show each file's stamp count on the file select.");
    add_toggle(book::viewer_config_var(), "Stamp book on the file select",
        "Like HD: press + (Start) on a file in the file select to see its stamp book.");
    add_toggle(toast_config_var(), "New stamp notices",
        "Show a small notice for each new stamp. Not in HD.");
    add_toggle(book::slot_config_var(), "Collection-screen stamp slot",
        "HD's stamp slot on the Collection screen. Off: use the stamp list in the menu bar instead.");
    add_toggle(book::debug_list_config_var(), "Stamp list in the menu bar (debug)",
        "A list of all 50 stamps with hints, in the menu bar. Not in HD.");

    g_panelVersion = g_version;
}

// build_panel resets the panel's element handles, so ../panel.cpp calls it before this.
void build_troubleshooting(UiElementHandle panel) {
    g_lastDiag = panel_diag_text();
    svc_ui->pane_add_text(mod_ctx, panel, g_lastDiag.c_str(), &g_panelDiag);
    g_lastBookDiag = book_diag_text();
    svc_ui->pane_add_text(mod_ctx, panel, g_lastBookDiag.c_str(), &g_panelBookDiag);
}

void update_panel() {
    ModContext* ctx = mod_ctx;
    // Live parts (the switch, the book's state and the options can change while the panel is shown). The status
    // and the diagnostics too: the switch can be turned off while the stamps wait for the game files, which
    // changes their text without a state change of the stamps (wave3 F5).
    set_text_if_changed(g_panelStatus, g_lastStatus, panel_status_text());
    set_text_if_changed(g_panelDiag, g_lastDiag, panel_diag_text());
    set_text_if_changed(g_panelHowTo, g_lastHowTo, howto_text());
    set_text_if_changed(g_panelBookDiag, g_lastBookDiag, book_diag_text());
    if (g_panelListButton != 0) {
        const int visible = stamp_list_offered() ? 1 : 0;
        if (visible != g_listButtonVisible &&
            svc_ui->elem_set_visible(ctx, g_panelListButton, visible != 0) == MOD_OK)
        {
            g_listButtonVisible = visible;
        }
    }
    if (g_panelVersion == g_version) {
        return;
    }
    g_panelVersion = g_version;
    if (g_panelProgress != 0) {
        svc_ui->elem_set_progress(ctx, g_panelProgress, progress_value());
    }
}

ModResult initialize() {
    // The Mods panel, the menu-bar tab and the window are ../panel.cpp's (one for the whole mod).
    scan_pngs();
    notify_state_changed();
    return MOD_OK;
}

void shutdown() {
    // The panel, the menu tab and the window go with the mod (../panel.cpp).
    window_closed();
    g_panelStatus = g_panelProgress = g_panelHowTo = g_panelListButton = 0;
    g_panelDiag = g_panelBookDiag = 0;
}

void update() {}

void notify_state_changed() {
    ++g_version;
}

bool stamp_list_available() {
    return feature_started() && feature_enabled();
}

bool stamp_list_offered() {
    // The menu-bar list is not HD: only when the Collection-screen book is unavailable (fallback) or
    // with the debug option, and only while the feature runs.
    return stamp_list_available() && (!book::integrated() || book::debug_stamp_list());
}

void open_stamp_book() {
    // Forced: this is also the fallback when HD's book could not open (book_host.cpp, not enough menu
    // memory) while the book counts as usable, so the list is shown even though it is not "offered".
    hdfeatures::open_window(hdfeatures::Feature::Stamps, true);
}

std::string stamp_image_rml(int index, int sizeDp, bool dim) {
    if (index < 0 || index >= kStampCount) {
        return {};
    }
    return tile_rml(index, sizeDp > 0 ? sizeDp : 48, !dim, false);
}

std::string escape_rml(const char* text) {
    std::string out;
    if (text == nullptr) {
        return out;
    }
    for (const char* p = text; *p != '\0'; ++p) {
        switch (*p) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += *p;
            break;
        }
    }
    return out;
}

bool stamp_png_available(int index) {
    return index >= 0 && index < kStampCount && g_png[index];
}

} // namespace hdstamps::ui
