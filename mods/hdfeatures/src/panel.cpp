// HD Features: the one Mods panel, the one menu-bar tab ("HD Features") and its window.
//
// Panel (Mods window detail pane, no help pane: help goes into tooltips):
//   HD Features             intro, the "Stamps" and "Cave of Shadows" switches, a line while one waits
//   Game files              hdfiles/panel_files.cpp: one line about the stored game files, a progress bar
//                           while they are made, the folder picker and "Try again" when they apply
//   Stamps ...              stamps/ui.cpp build_panel ("Stamps", "Stamps: where to find them", "...: options")
//   Cave of Shadows ...     cave/ui.cpp build_panel ("Cave of Shadows", "...: record", "...: options",
//                           "...: how to play", "...: before you turn it off")
//   About, Troubleshooting  shared by both features (Troubleshooting also: "Make the game files again",
//                           "Delete the game files", hdfiles/panel_files.cpp)
// Window (menu-bar tab "HD Features", only while there is something to show): a "Cave of Shadows" tab
// while the cave runs, and the stamp list's two tabs while the list is offered (HD's book unavailable,
// or the stamps-debug_stamp_list option) or was asked for (the book could not open, or the panel's
// button: open_window forceStamps). Neither old mod's own tab or panel exists any more.

#include <mods/svc/config.h>
#include <mods/svc/log.hpp>
#include <mods/svc/ui.h>

#include "hdfeatures.hpp"
#include "hdfiles/files.hpp"
#include "hdfiles/panel_files.hpp"

#include "cave/cave.hpp"
#include "stamps/ui.hpp"

#include <string>
#include <utility>
#include <vector>

namespace hdfeatures {
namespace {

UiMenuTabHandle g_menuTab = 0;
UiWindowHandle g_window = 0;
bool g_windowCave = false;    // tabs of the open window
bool g_windowStamps = false;
bool g_stampsForced = false;  // the stamp list was asked for while HD's book is in use (open_window)
bool g_panelRegistered = false;

UiElementHandle g_switchNote = 0;
std::string g_lastSwitchNote;
int g_switchNoteVisible = -1;  // -1 = unknown

constexpr const char* kStampsSwitchHelp =
    "HD's 50 stamps and the stamp book. Your stamps are kept while it is off.";
constexpr const char* kCaveSwitchHelp =
    "HD's Cave of Shadows. Your progress is kept while it is off, but the Colossal Wallet becomes a Giant "
    "Wallet again and rupees over its limit are lost.";

// A switched-on feature that only waits for the game files: the "Game files" line says what is going on, so the
// switch note stays quiet (one line per state, plan 1.2).
bool waits_for_files(Feature feature) {
    return feature_waits_for_files(feature);
}

bool cave_tab_wanted() {
    return feature_running(Feature::Cave);
}

bool stamps_tabs_wanted() {
    return hdstamps::ui::stamp_list_offered() || (g_stampsForced && hdstamps::ui::stamp_list_available());
}

std::string switch_note() {
    std::string note;
    const auto add = [&](const char* line) {
        if (!note.empty()) {
            note += ' ';
        }
        note += line;
    };
    const bool stampsOn = feature_pending(Feature::Stamps) && !waits_for_files(Feature::Stamps) &&
                          feature_wanted(Feature::Stamps);
    const bool caveOn = !feature_failed(Feature::Cave) && feature_pending(Feature::Cave) &&
                        !waits_for_files(Feature::Cave) && feature_wanted(Feature::Cave);
    if (stampsOn && caveOn) {
        // Both start together (for example once the game files are ready): one sentence.
        return "Stamps and the Cave of Shadows turn on as soon as no menu, cutscene or loading is in progress.";
    }
    if (feature_pending(Feature::Stamps) && !waits_for_files(Feature::Stamps)) {
        if (feature_wanted(Feature::Stamps)) {
            add("Stamps turn on as soon as no menu, cutscene or loading is in progress.");
        } else if (const char* reason = feature_wait_reason(Feature::Stamps); reason != nullptr) {
            const std::string line = std::string("Stamps turn off once ") + reason + ".";
            add(line.c_str());
        } else {
            add("Stamps turn off as soon as no menu, cutscene, loading or stamp book is in progress.");
        }
    }
    if (feature_failed(Feature::Cave)) {
        add("The Cave of Shadows could not start (see the log). The stamps are not affected.");
    } else if (feature_pending(Feature::Cave) && !waits_for_files(Feature::Cave)) {
        add(feature_wanted(Feature::Cave)
                ? "The Cave of Shadows turns on as soon as no menu, cutscene or loading is in progress."
                : cave::busy() ? "The Cave of Shadows turns off once you have left the cave."
                               : "The Cave of Shadows turns off as soon as no menu, cutscene or loading is in "
                                 "progress.");
    }
    return note;
}

void add_switch(UiElementHandle pane, Feature feature, const char* label, const char* help) {
    const ConfigVarHandle var = feature_config_var(feature);
    if (var == 0) {
        return;
    }
    UiControlDesc toggle = UI_CONTROL_DESC_INIT;
    toggle.kind = UI_CONTROL_TOGGLE;
    toggle.label = label;
    toggle.tooltip = help;
    toggle.binding = UI_BINDING_CONFIG_VAR;
    toggle.config_var = var;
    svc_ui->pane_add_control(mod_ctx, pane, &toggle, nullptr);
}

void refresh_switch_note() {
    if (g_switchNote == 0) {
        return;
    }
    std::string note = switch_note();
    if (note != g_lastSwitchNote &&
        svc_ui->elem_set_text(mod_ctx, g_switchNote, note.empty() ? " " : note.c_str()) == MOD_OK)
    {
        g_lastSwitchNote = std::move(note);
    }
    const int visible = g_lastSwitchNote.empty() ? 0 : 1;
    if (visible != g_switchNoteVisible && svc_ui->elem_set_visible(mod_ctx, g_switchNote, visible != 0) == MOD_OK) {
        g_switchNoteVisible = visible;
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    g_switchNote = 0;
    g_lastSwitchNote.clear();
    g_switchNoteVisible = -1;

    svc_ui->pane_add_section(mod_ctx, panel, "HD Features");
    add_switch(panel, Feature::Stamps, "Stamps", kStampsSwitchHelp);
    add_switch(panel, Feature::Cave, "Cave of Shadows", kCaveSwitchHelp);
    g_lastSwitchNote = switch_note();
    if (svc_ui->pane_add_text(mod_ctx, panel, g_lastSwitchNote.empty() ? " " : g_lastSwitchNote.c_str(),
            &g_switchNote) != MOD_OK)
    {
        g_switchNote = 0;
    }
    refresh_switch_note();
    hdfiles::panel::build(panel);

    hdstamps::ui::build_panel(panel);
    cave::ui::build_panel(panel);

    svc_ui->pane_add_section(mod_ctx, panel, "About");
    svc_ui->pane_add_text(mod_ctx, panel, kAboutText, nullptr);

    svc_ui->pane_add_section(mod_ctx, panel, "Troubleshooting");
    hdstamps::ui::build_troubleshooting(panel);
    cave::ui::build_troubleshooting(panel);
    hdfiles::panel::build_troubleshooting(panel);
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    refresh_switch_note();
    hdfiles::panel::update();
    hdstamps::ui::update_panel();
    cave::ui::update_panel();
    return MOD_OK;
}

void on_window_closed(ModContext*, UiWindowHandle window, void*) {
    if (window != g_window) {
        return;
    }
    g_window = 0;
    g_windowCave = g_windowStamps = false;
    g_stampsForced = false;
    hdstamps::ui::window_closed();
    cave::ui::tab_closed();
}

void on_menu_tab(ModContext*, void*) {
    // The cave's tab first: the stamp list is only a fallback (or the debug option) next to HD's book.
    open_window(feature_running(Feature::Cave) ? Feature::Cave : Feature::Stamps);
}

}  // namespace

bool open_window(Feature first, bool forceStamps) {
    if (g_window != 0) {
        const bool stampsWanted = stamps_tabs_wanted() || (forceStamps && hdstamps::ui::stamp_list_available());
        if (g_windowCave == cave_tab_wanted() && g_windowStamps == stampsWanted) {
            return true;  // already open with these tabs
        }
        close_window();  // rebuilt below with the tabs asked for (e.g. the stamp list after a failed book)
    }
    if (forceStamps && hdstamps::ui::stamp_list_available()) {
        g_stampsForced = true;  // kept by refresh_menu until the window closes
    }
    const bool cave = cave_tab_wanted();
    const bool stamps = stamps_tabs_wanted();
    std::vector<UiTabDesc> tabs;
    const auto add_cave = [&] {
        if (!cave) {
            return;
        }
        UiTabDesc tab = UI_TAB_DESC_INIT;
        tab.title = "Cave of Shadows";
        tab.build = cave::ui::build_tab;
        tab.update = cave::ui::update_tab;
        tabs.push_back(tab);
    };
    const auto add_stamps = [&] {
        if (!stamps) {
            return;
        }
        UiTabDesc collection = UI_TAB_DESC_INIT;
        collection.title = hdstamps::ui::collection_tab_title();
        collection.build = hdstamps::ui::build_collection;
        collection.update = hdstamps::ui::update_collection;
        tabs.push_back(collection);
        UiTabDesc checklist = UI_TAB_DESC_INIT;
        checklist.title = "Stamp checklist";
        checklist.build = hdstamps::ui::build_checklist;
        tabs.push_back(checklist);
    };
    if (first == Feature::Stamps) {
        add_stamps();
        add_cave();
    } else {
        add_cave();
        add_stamps();
    }
    if (tabs.empty()) {
        g_stampsForced = false;
        mods::log::info("HD Features window: nothing to show (both parts off, or HD's stamp book in use)");
        return false;
    }
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs.data();
    desc.tab_count = tabs.size();
    desc.rcss = hdstamps::ui::window_rcss();  // hds-* classes only (the cave's styles are UI_SCOPE_WINDOW)
    desc.on_closed = on_window_closed;
    UiWindowHandle window = 0;
    const ModResult result = svc_ui->window_push(mod_ctx, &desc, &window);
    if (result != MOD_OK) {
        g_stampsForced = false;
        mods::log::warn("could not open the HD Features window (result {})", static_cast<int>(result));
        return false;
    }
    g_window = window;
    g_windowCave = cave;
    g_windowStamps = stamps;
    return true;
}

void close_window() {
    if (g_window == 0) {
        return;
    }
    const UiWindowHandle window = g_window;
    g_window = 0;
    g_windowCave = g_windowStamps = false;
    g_stampsForced = false;
    hdstamps::ui::window_closed();
    cave::ui::tab_closed();
    svc_ui->window_close(mod_ctx, window);
}

void refresh_menu() {
    const bool cave = cave_tab_wanted();
    const bool stamps = stamps_tabs_wanted();
    if (g_window != 0 && (g_windowCave != cave || g_windowStamps != stamps)) {
        close_window();  // its tabs no longer match what runs
    }
    const bool want = cave || stamps;
    if (want && g_menuTab == 0) {
        UiMenuTabDesc tab = UI_MENU_TAB_DESC_INIT;
        tab.label = "HD Features";
        tab.on_selected = on_menu_tab;
        if (svc_ui->register_menu_tab(mod_ctx, &tab, &g_menuTab) != MOD_OK) {
            // The Mods panel still works; the menu tab is a convenience.
            g_menuTab = 0;
            mods::log::warn("could not add the HD Features menu tab");
        }
    } else if (!want && g_menuTab != 0) {
        svc_ui->unregister_menu_tab(mod_ctx, g_menuTab);
        g_menuTab = 0;
    }
}

void panel_initialize() {
    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    g_panelRegistered = svc_ui->register_mods_panel(mod_ctx, &panel) == MOD_OK;
    if (!g_panelRegistered) {
        mods::log::warn("could not register the HD Features panel in the Mods window");
    }
}

void panel_update() {
    refresh_menu();
}

void panel_shutdown() {
    close_window();
    if (g_menuTab != 0) {
        svc_ui->unregister_menu_tab(mod_ctx, g_menuTab);
        g_menuTab = 0;
    }
    g_switchNote = 0;
    // The features' own UI parts are reset by their shutdown (stamps/feature.cpp, cave/cave.cpp).
}

}  // namespace hdfeatures
