// HD Features: the "Game files" panel section (see panel_files.hpp). Module R2.

#include "hdfiles/panel_files.hpp"

#include "hdfiles/setup.hpp"

#include <string>

#include <mods/api.h>
#include <mods/svc/ui.h>

namespace hdfiles::panel {
namespace {

constexpr const char* kPickHelp = "The folder with your game (code, content, meta) or with its .wua file.";
constexpr const char* kTryAgainHelp = "Look for your Twilight Princess HD files again.";
constexpr const char* kRemakeHelp = "Makes them again from your Twilight Princess HD at the next start.";
constexpr const char* kDeleteHelp =
    "Removes them (about 75 MB) when Dusklight closes, for example before you remove HD Features.";

struct Shown {
    UiElementHandle line = 0, bar = 0, pick = 0, retry = 0, remake = 0, del = 0;
    std::string lastLine;
    float lastBar = -2.0f;
    int barVisible = -1, pickVisible = -1, retryVisible = -1, remakeVisible = -1, delVisible = -1;  // -1 = unknown
};
Shown g;

void set_visible(UiElementHandle elem, int& shown, bool visible) {
    if (elem != 0 && shown != (visible ? 1 : 0) && svc_ui->elem_set_visible(mod_ctx, elem, visible) == MOD_OK) {
        shown = visible ? 1 : 0;
    }
}

UiElementHandle add_button(UiElementHandle pane, const char* label, const char* tooltip, UiPressedFn pressed,
                           UiPredicateFn disabled = nullptr) {
    UiControlDesc desc = UI_CONTROL_DESC_INIT;
    desc.kind = UI_CONTROL_BUTTON;
    desc.label = label;
    desc.tooltip = tooltip;
    desc.on_pressed = pressed;
    desc.is_disabled = disabled;
    UiElementHandle handle = 0;
    if (svc_ui->pane_add_control(mod_ctx, pane, &desc, &handle) != MOD_OK) {
        return 0;
    }
    return handle;
}

}  // namespace

void build(std::uint64_t pane) {
    // A new build: every handle of the last one is gone (the Troubleshooting buttons come later in this build).
    g = Shown{};
    svc_ui->pane_add_section(mod_ctx, pane, "Game files");
    g.lastLine = setup::line();
    if (svc_ui->pane_add_text(mod_ctx, pane, g.lastLine.empty() ? " " : g.lastLine.c_str(), &g.line) != MOD_OK) {
        g.line = 0;
    }
    const float value = setup::progress();
    if (svc_ui->pane_add_progress(mod_ctx, pane, value < 0 ? 0.0f : value, &g.bar) != MOD_OK) {
        g.bar = 0;
    }
    g.pick = add_button(pane, "Choose your Twilight Princess HD folder", kPickHelp,
        [](ModContext*, void*) { setup::choose_folder(); },
        [](ModContext*, void*) { return !setup::can_choose_folder(); });
    g.retry = add_button(pane, "Try again", kTryAgainHelp, [](ModContext*, void*) { setup::try_again(); },
        [](ModContext*, void*) { return !setup::can_try_again(); });
    update();
}

void build_troubleshooting(std::uint64_t pane) {
    g.remakeVisible = g.delVisible = -1;
    // Disabled once one of them was pressed, never hidden while the files are ready: hiding the pressed button
    // made the Mods window jump to the top (wave3_R2_verify F2).
    g.remake = add_button(pane, "Make the game files again", kRemakeHelp,
        [](ModContext*, void*) { setup::request_remake(); }, [](ModContext*, void*) { return !setup::can_manage(); });
    g.del = add_button(pane, "Delete the game files", kDeleteHelp, [](ModContext*, void*) { setup::request_delete(); },
        [](ModContext*, void*) { return !setup::can_manage(); });
    update();
}

void update() {
    if (g.line != 0) {
        std::string text = setup::line();
        if (text != g.lastLine && svc_ui->elem_set_text(mod_ctx, g.line, text.empty() ? " " : text.c_str()) == MOD_OK) {
            g.lastLine = std::move(text);
        }
    }
    const float value = setup::progress();
    set_visible(g.bar, g.barVisible, value >= 0.0f);
    if (g.bar != 0 && value >= 0.0f && value != g.lastBar &&
        svc_ui->elem_set_progress(mod_ctx, g.bar, value) == MOD_OK)
    {
        g.lastBar = value;
    }
    // Shown or not; enabled or not is up to their is_disabled predicates.
    set_visible(g.pick, g.pickVisible, setup::show_folder_button());
    set_visible(g.retry, g.retryVisible, setup::show_setup_buttons());
    const bool manage = setup::show_manage_buttons();
    set_visible(g.remake, g.remakeVisible, manage);
    set_visible(g.del, g.delVisible, manage);
}

}  // namespace hdfiles::panel
