// HD Features / Cave of Shadows: the cave's part of the Mods panel and its tab of the HD Features window.
//
// ../panel.cpp owns the one Mods panel, the one menu-bar tab ("HD Features") and its window; it calls
// build_panel / update_panel / build_troubleshooting here and adds the "Cave of Shadows" tab (build_tab /
// update_tab) while the cave runs.
//
// Entry is the virtual Wolf Link amiibo on the pause menu's Collection screen (cave.cpp / amiibo.cpp),
// then HD's "Cave of Shadows" screen with Save and begin / Cancel (confirm.cpp).
// The Enter button below is the hidden debug option DESIGN.md keeps from v1: it only shows when the
// "cave-debug_enter_button" option is on, or when the Collection-screen hook could not be installed.
// Buttons only queue the request; cave::tick() performs the stage change from mod_update.

#include "cave.hpp"

#include <mods/svc/config.h>
#include <mods/svc/log.hpp>
#include <mods/svc/ui.h>

#include <string>
#include <utility>

namespace cave::ui {
namespace {

struct Texts {
    UiElementHandle status = 0;
    UiElementHandle availability = 0;
    UiElementHandle record = 0;
    UiElementHandle problems = 0;
    UiElementHandle enter = 0;
    std::string lastStatus;
    std::string lastAvailability;
    std::string lastRecord;
    std::string lastProblems;
    int enterVisible = -1;  // -1 = unknown
};

Texts g_panelTexts;
Texts g_windowTexts;

constexpr const char* kButtonOptions[] = {"Z", "X", "Y"};

// Help texts: plain text without markup, because the same string is the help pane's RML in the HD
// Features window and the control's tooltip in the Mods window (which has no help pane).
constexpr const char* kEnterHelp =
    "Test shortcut: go straight to the cave, without the Cave of Shadows screen and without saving.";
constexpr const char* kWithdrawHelp =
    "Leave the cave, keeping what you found: back to your entry spot in a field, else to where your save would load.";
constexpr const char* kToastHelp =
    "Show the floor number each time you reach a new floor. Not in HD.";
constexpr const char* kSectionsHelp =
    "Like HD: floors 1-5, 7-19 and 21-40 open as the story goes on. Off: all 41 floors in one go.";
constexpr const char* kStartDeepestHelp =
    "Not in HD. Start each visit on the first floor of the deepest section you have unlocked (floor 7 or 21) "
    "instead of floor 1. Needs \"HD sections\".";
constexpr const char* kHintHelp =
    "Show a small Wolf Link amiibo reminder when you open the Collection screen.";
constexpr const char* kDebugHelp =
    "Show an \"Enter the cave now\" test button here.";
constexpr const char* kWarpHelp =
    "Play Midna's portal warp when you leave with Withdraw or \"Leave the cave\". Off: a simple fade.";
constexpr const char* kEntryWarpHelp =
    "Not in HD. Midna warps you in after \"Save and begin\", where her map warp works. Needs \"Midna's "
    "portal warp\".";
constexpr const char* kHdWithdrawHelp =
    "Like HD: Withdraw takes you back to your \"Save and begin\" save, and nothing from the visit is kept. "
    "Off: like \"Leave the cave\".";
constexpr const char* kButtonHelp =
    "The button that scans the Wolf Link amiibo on the Collection screen.";

bool enter_disabled(ModContext*, void*) {
    return !cave::can_enter(nullptr);
}

bool withdraw_disabled(ModContext*, void*) {
    return !cave::can_withdraw(nullptr);
}

void on_enter(ModContext*, void*) {
    if (cave::request_enter()) {
        hdfeatures::close_window();
    }
}

void on_withdraw(ModContext*, void*) {
    if (cave::request_withdraw()) {
        hdfeatures::close_window();
    }
}

void add_text(UiElementHandle pane, const std::string& text, UiElementHandle* out) {
    if (svc_ui->pane_add_text(mod_ctx, pane, text.c_str(), out) != MOD_OK && out != nullptr) {
        *out = 0;
    }
}

// help_rml only renders where a help pane exists (window tabs); the Mods window panel has none, so
// there the same text goes into the tooltip instead (shown on focus or hover).
void set_help(UiControlDesc& desc, const char* help, bool tooltip) {
    desc.help_rml = help;
    if (tooltip) {
        desc.tooltip = help;
    }
}

void add_toggle(UiElementHandle pane, const char* label, const char* help, std::uint64_t var, bool tooltip) {
    if (var == 0) {
        return;
    }
    UiControlDesc toggle = UI_CONTROL_DESC_INIT;
    toggle.kind = UI_CONTROL_TOGGLE;
    toggle.label = label;
    set_help(toggle, help, tooltip);
    toggle.binding = UI_BINDING_CONFIG_VAR;
    toggle.config_var = var;
    svc_ui->pane_add_control(mod_ctx, pane, &toggle, nullptr);
}

void add_button_choice(UiElementHandle pane, bool tooltip) {
    if (cave::amiibo_button_var() == 0) {
        return;
    }
    UiControlDesc choice = UI_CONTROL_DESC_INIT;
    choice.kind = UI_CONTROL_DROPDOWN;
    choice.label = "Amiibo button";
    set_help(choice, kButtonHelp, tooltip);
    choice.binding = UI_BINDING_CONFIG_VAR;
    choice.config_var = cave::amiibo_button_var();
    choice.options = kButtonOptions;
    choice.option_count = 3;
    svc_ui->pane_add_control(mod_ctx, pane, &choice, nullptr);
}

// modsPanel: built into the Mods window's detail pane, between the HD Features switches and the shared
// About / Troubleshooting sections (no help pane there, so help becomes tooltips, and the section titles
// say which feature they belong to). Otherwise the "Cave of Shadows" tab of the HD Features window.
void build_contents(UiElementHandle pane, Texts& texts, bool modsPanel) {
    texts = {};
    svc_ui->pane_add_section(mod_ctx, pane, modsPanel ? "Cave of Shadows" : "Your progress");

    texts.lastStatus = cave::status_text();
    add_text(pane, texts.lastStatus, &texts.status);
    texts.lastAvailability = cave::availability_text();
    add_text(pane, texts.lastAvailability, &texts.availability);

    UiControlDesc withdraw = UI_CONTROL_DESC_INIT;
    withdraw.kind = UI_CONTROL_BUTTON;
    withdraw.label = "Leave the cave";
    set_help(withdraw, kWithdrawHelp, modsPanel);
    withdraw.on_pressed = on_withdraw;
    withdraw.is_disabled = withdraw_disabled;
    svc_ui->pane_add_control(mod_ctx, pane, &withdraw, nullptr);

    UiControlDesc enter = UI_CONTROL_DESC_INIT;
    enter.kind = UI_CONTROL_BUTTON;
    enter.label = "Enter the cave now";
    set_help(enter, kEnterHelp, modsPanel);
    enter.on_pressed = on_enter;
    enter.is_disabled = enter_disabled;
    if (svc_ui->pane_add_control(mod_ctx, pane, &enter, &texts.enter) != MOD_OK) {
        texts.enter = 0;
    }
    if (texts.enter != 0) {
        texts.enterVisible = cave::enter_button_visible() ? 1 : 0;
        svc_ui->elem_set_visible(mod_ctx, texts.enter, texts.enterVisible != 0);
    }

    svc_ui->pane_add_section(mod_ctx, pane, modsPanel ? "Cave of Shadows: record" : "Record");
    texts.lastRecord = cave::record_rml();
    if (svc_ui->pane_add_rml(mod_ctx, pane, texts.lastRecord.c_str(), &texts.record) != MOD_OK) {
        texts.record = 0;
    }

    svc_ui->pane_add_section(mod_ctx, pane, modsPanel ? "Cave of Shadows: options" : "Options");
    add_button_choice(pane, modsPanel);
    add_toggle(pane, "HD sections", kSectionsHelp, cave::hd_sections_var(), modsPanel);
    add_toggle(pane, "Withdraw like HD (back to your save)", kHdWithdrawHelp, cave::hd_withdraw_var(), modsPanel);
    add_toggle(pane, "Start at deepest unlocked section (not in HD)", kStartDeepestHelp, cave::start_deepest_var(), modsPanel);
    add_toggle(pane, "Midna's portal warp", kWarpHelp, cave::warp_effect_var(), modsPanel);
    add_toggle(pane, "Midna's warp-out on entry (not in HD)", kEntryWarpHelp, cave::entry_warp_var(), modsPanel);
    add_toggle(pane, "Amiibo prompt", kHintHelp, cave::amiibo_hint_var(), modsPanel);
    add_toggle(pane, "Floor number notices", kToastHelp, cave::floor_toasts_var(), modsPanel);
    add_toggle(pane, "Show \"Enter the cave now\" (debug)", kDebugHelp, cave::debug_enter_var(), modsPanel);

    svc_ui->pane_add_section(mod_ctx, pane, modsPanel ? "Cave of Shadows: how to play" : "How to play");
    add_text(pane,
        std::string("Get in: on the Collection screen, press ") + cave::amiibo_button_label() +
            " and choose \"Save and begin\".",
        nullptr);
    add_text(pane, "Get out: step into the portal on floor 6, 20 or 41, or call Midna and choose Withdraw.",
        nullptr);

    if (modsPanel) {
        return;  // About and Troubleshooting are shared sections of the panel (../panel.cpp)
    }
    svc_ui->pane_add_section(mod_ctx, pane, "About");
    add_text(pane, hdfeatures::kAboutText, nullptr);

    svc_ui->pane_add_section(mod_ctx, pane, "Troubleshooting");
    texts.lastProblems = cave::hook_problems_text();
    add_text(pane, texts.lastProblems, &texts.problems);
}

template <class Make>
void refresh_text(UiElementHandle elem, std::string& last, Make make, bool rml) {
    if (elem == 0) {
        return;
    }
    std::string value = make();
    if (value == last) {
        return;
    }
    const ModResult result = rml ? svc_ui->elem_set_rml(mod_ctx, elem, value.c_str())
                                 : svc_ui->elem_set_text(mod_ctx, elem, value.c_str());
    if (result == MOD_OK) {
        last = std::move(value);
    }
}

void refresh(Texts& texts) {
    refresh_text(texts.status, texts.lastStatus, [] { return cave::status_text(); }, false);
    refresh_text(texts.availability, texts.lastAvailability, [] { return cave::availability_text(); }, false);
    refresh_text(texts.record, texts.lastRecord, [] { return cave::record_rml(); }, true);
    refresh_text(texts.problems, texts.lastProblems, [] { return cave::hook_problems_text(); }, false);
    if (texts.enter != 0) {
        const int visible = cave::enter_button_visible() ? 1 : 0;
        if (visible != texts.enterVisible &&
            svc_ui->elem_set_visible(mod_ctx, texts.enter, visible != 0) == MOD_OK)
        {
            texts.enterVisible = visible;
        }
    }
}

}  // namespace

// --- Mods-window panel (../panel.cpp) ---
// build_panel resets the panel's element handles, so ../panel.cpp calls it before build_troubleshooting.
void build_panel(std::uint64_t pane) {
    build_contents(pane, g_panelTexts, true);
}

void build_troubleshooting(std::uint64_t pane) {
    g_panelTexts.lastProblems = cave::hook_problems_text();
    add_text(pane, g_panelTexts.lastProblems, &g_panelTexts.problems);
}

void update_panel() {
    refresh(g_panelTexts);
}

// --- "Cave of Shadows" tab of the HD Features window (../panel.cpp) ---
ModResult build_tab(ModContext*, std::uint64_t, std::uint64_t left, std::uint64_t right, void*, ModError*) {
    build_contents(left, g_windowTexts, false);
    svc_ui->pane_add_text(mod_ctx, right,
        "The Cave of Shadows from Twilight Princess HD. Select a button or an option to see what it does.",
        nullptr);
    return MOD_OK;
}

ModResult update_tab(ModContext*, void*, ModError*) {
    refresh(g_windowTexts);
    return MOD_OK;
}

void tab_closed() {
    g_windowTexts = {};
}

void shutdown() {
    // The panel, the menu tab and the window are ../panel.cpp's (and go with the mod).
    g_panelTexts = {};
    g_windowTexts = {};
}

}  // namespace cave::ui
