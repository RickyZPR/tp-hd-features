// HD Features / Stamps: Twilight Princess HD's 50 stamps for Dusklight v2.0.3.
//
// Personal-use feature. All new state lives in the SaveService blob "stamps"; the .gci only sees
// what the game itself writes when a chest is opened. See DESIGN.md ("Feature 1: Stamps").
//
// This was HD Stamps' mod.cpp. The mod entry points (DEFINE_MOD, the service imports and
// mod_initialize/update/shutdown) are in ../mod.cpp now; it registers this feature's options at
// load (register_config) and starts the feature the first time the "Stamps" switch is on. The
// switch itself only gates the feature (mod_active(), state.cpp): the feature is never torn down
// before mod_shutdown. The Mods panel, the menu-bar tab and the window are ../panel.cpp's.

#include <mods/svc/log.hpp>

#include "feature.hpp"

#include "book.hpp"
#include "book_host.hpp"
#include "filesel.hpp"
#include "items.hpp"
#include "state.hpp"
#include "ui.hpp"

#include <mods/svc/save.h>

namespace hdstamps {
namespace {

SaveObserverHandle g_saveObserver = 0;
bool g_started = false;

void on_new_save_cb(ModContext*, uint32_t slot, void*) {
    on_new_save(slot);
}

void on_save_loaded_cb(ModContext*, uint32_t slot, void*) {
    on_save_loaded(slot);
}

void on_save_written_cb(ModContext*, uint32_t slot, void*) {
    on_save_written(slot);
}

} // namespace

void feature_register_config() {
    register_config();
    book::register_config();
}

void feature_initialize() {
    if (g_started) {
        return;
    }
    g_started = true;

    // Item side first: the first active-state evaluation below applies the item table.
    items::initialize();

    if (svc_save->observe_saves(mod_ctx, on_new_save_cb, on_save_loaded_cb, on_save_written_cb, nullptr,
            &g_saveObserver) != MOD_OK)
    {
        mods::log::error("could not observe saves; the collection is only picked up by polling");
        g_saveObserver = 0;
    }

    // HD stamp book: converted assets, Collection-screen slot, pause-menu hosting, file-select
    // counter. Before the UI, which only offers the RmlUi list when the book is unavailable.
    book::initialize();
    ui::initialize();

    refresh_active_state();
    attach_to_current_slot();

    mods::log::info("Stamps started ({} stamps, {} stamp chest resolvers + HD's Hyrule Castle seed move, "
                    "{} HD-only chests)",
        kStampCount, 39, 7);
}

bool feature_started() {
    return g_started;
}

void feature_update() {
    if (!g_started) {
        return;
    }
    state_update();
    items::update();
    book::update();
    ui::update();
}

bool feature_busy() {
    return g_started && (book::host::busy() || book::filesel::viewer_open());
}

const char* feature_off_blocked() {
    if (!g_started || !mod_active()) {
        return nullptr;
    }
    if (items::gives_in_flight()) {
        return "the stamp on its way to you has been handed over";
    }
    if (items::hd_chest_stage_loaded()) {
        return "you leave this area (it has one of HD's extra stamp chests)";
    }
    return nullptr;
}

void feature_shutdown() {
    if (!g_started) {
        return;
    }
    // The book first: it may have to put a Collection screen back before the hooks go away.
    book::shutdown();
    items::shutdown();
    ui::shutdown();
    if (g_saveObserver != 0) {
        svc_save->unobserve_saves(mod_ctx, g_saveObserver);
        g_saveObserver = 0;
    }
    g_started = false;
    mods::log::info("Stamps unloaded");
}

} // namespace hdstamps
