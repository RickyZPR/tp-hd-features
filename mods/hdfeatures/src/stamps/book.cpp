#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "book.hpp"

#include "book_assets.hpp"
#include "book_host.hpp"
#include "collect_slot.hpp"
#include "filesel.hpp"
#include "hdtext.hpp"
#include "stamp_book.hpp"
#include "state.hpp"

#include <mods/svc/config.h>
#include <mods/svc/message.h>

#include <fmt/format.h>

#include <array>
#include <string>
#include <vector>

namespace hdstamps::book {
namespace {

ConfigVarHandle g_slotVar = 0;
ConfigVarHandle g_autoOpenVar = 0;
ConfigVarHandle g_postVar = 0;
ConfigVarHandle g_debugListVar = 0;
ConfigVarHandle g_fileSelectVar = 0;
ConfigVarHandle g_viewerVar = 0;
// Not shown in the mods panel: the menu scaling mode last seen on a game screen, so that an
// automatic opening before the Collection screen was opened this session uses the right one.
ConfigVarHandle g_scalingVar = 0;

MenuScaling g_scaling = MenuScaling::Wii; // Dusklight's default menu scaling
std::vector<MessageOverrideHandle> g_overrides;
bool g_initialized = false;

constexpr std::array<uint8_t, 6> kLanguages = {MESSAGE_LANGUAGE_ENGLISH, MESSAGE_LANGUAGE_GERMAN,
    MESSAGE_LANGUAGE_FRENCH, MESSAGE_LANGUAGE_SPANISH, MESSAGE_LANGUAGE_ITALIAN, MESSAGE_LANGUAGE_JAPANESE};

// Own wording for 2801 when res/stamps/hdtext is missing (GC's zel_00 2801 is empty).
constexpr char kFallbackDescription[] = "Your stamp collection.\n\n%d/%d stamps found";

bool get_bool(ConfigVarHandle var, bool fallback) {
    bool value = fallback;
    if (var != 0 && svc_config != nullptr) {
        svc_config->get_bool(mod_ctx, var, &value);
    }
    return value;
}

ConfigVarHandle register_bool(const char* name, bool value) {
    if (svc_config == nullptr) {
        return 0;
    }
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = value;
    ConfigVarHandle handle = 0;
    if (svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK) {
        mods::log::warn("could not register the '{}' option; it keeps its default", name);
        return 0;
    }
    return handle;
}

// zel_00 2801, the slot description: HD's text with "%d/%d" = (owned, 50) at fetch time. GC renders
// it through dMsgString_c::getString (JMessage::TControl), which resolves MessageService overrides
// (libs/JSystem/src/JMessage/control.cpp:113), including the [A] glyph tag.
bool description_override(ModContext*, const MessageOverrideContext*, MessageTextData* outText, void*) {
    if (outText == nullptr || !mod_active()) {
        return false;
    }
    thread_local std::vector<uint8_t> buffer;
    if (const hdtext::Message* hd = hdtext::find(hdtext::kGroupZel00, hdtext::kMsgStampCollectionHelp); hd != nullptr) {
        buffer = hdtext::format_counter_encoded(hd->text, owned_count(), kStampCount);
    } else {
        const std::vector<uint8_t> own(kFallbackDescription, kFallbackDescription + sizeof(kFallbackDescription));
        buffer = hdtext::format_counter_encoded(own, owned_count(), kStampCount);
    }
    if (!hdtext::valid_encoded_text(buffer.data(), buffer.size())) {
        return false;
    }
    outText->text = buffer.data();
    outText->text_size = buffer.size();
    return true;
}

// zel_00 2800 for any game code that renders it through JMessage (the slot name itself is written
// by collect_slot, since dMeter2Info_getStringKanji bypasses overrides).
bool name_override(ModContext*, const MessageOverrideContext*, MessageTextData* outText, void*) {
    if (outText == nullptr || !mod_active()) {
        return false;
    }
    thread_local std::vector<uint8_t> buffer;
    if (const hdtext::Message* hd = hdtext::find(hdtext::kGroupZel00, hdtext::kMsgStampCollection); hd != nullptr) {
        buffer = hd->text;
    } else {
        static constexpr char kName[] = "Stamps";
        buffer.assign(kName, kName + sizeof(kName));
    }
    outText->text = buffer.data();
    outText->text_size = buffer.size();
    return true;
}

void register_overrides() {
    if (svc_message == nullptr) {
        return;
    }
    int failures = 0;
    for (uint8_t language : kLanguages) {
        MessageOverrideHandle handle = 0;
        if (svc_message->override_message_fn(mod_ctx, hdtext::kGroupZel00, hdtext::kMsgStampCollectionHelp,
                language, description_override, nullptr, &handle) == MOD_OK)
        {
            g_overrides.push_back(handle);
        } else {
            ++failures;
        }
        if (svc_message->override_message_fn(mod_ctx, hdtext::kGroupZel00, hdtext::kMsgStampCollection,
                language, name_override, nullptr, &handle) == MOD_OK)
        {
            g_overrides.push_back(handle);
        } else {
            ++failures;
        }
    }
    if (failures != 0) {
        mods::log::warn("{} stamp-slot message override(s) failed", failures);
    }
}

} // namespace

void register_config() {
    g_slotVar = register_bool("stamps-collection_slot", true);
    g_autoOpenVar = register_bool("stamps-book_auto_open", true);
    g_debugListVar = register_bool("stamps-debug_stamp_list", false);
    g_fileSelectVar = register_bool("stamps-file_select_counter", true);
    g_viewerVar = register_bool("stamps-file_select_viewer", true);
    if (svc_config != nullptr) {
        ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
        desc.name = "stamps-book_post_button";
        desc.type = CONFIG_VAR_INT;
        desc.default_int = static_cast<int64_t>(PostButton::Hd);
        if (svc_config->register_var(mod_ctx, &desc, &g_postVar) != MOD_OK) {
            mods::log::warn("could not register the 'stamps-book_post_button' option; Post keeps HD's behaviour");
            g_postVar = 0;
        }
        ConfigVarDesc scaling = CONFIG_VAR_DESC_INIT;
        scaling.name = "stamps-last_menu_scaling";
        scaling.type = CONFIG_VAR_INT;
        scaling.default_int = static_cast<int64_t>(MenuScaling::Wii);
        if (svc_config->register_var(mod_ctx, &scaling, &g_scalingVar) == MOD_OK) {
            int64_t value = static_cast<int64_t>(MenuScaling::Wii);
            svc_config->get_int(mod_ctx, g_scalingVar, &value);
            if (value >= 0 && value <= static_cast<int64_t>(MenuScaling::Dusklight)) {
                g_scaling = static_cast<MenuScaling>(value);
            }
        } else {
            g_scalingVar = 0;
        }
    }
}

ConfigVarHandle slot_config_var() {
    return g_slotVar;
}

ConfigVarHandle auto_open_config_var() {
    return g_autoOpenVar;
}

ConfigVarHandle post_button_config_var() {
    return g_postVar;
}

ConfigVarHandle debug_list_config_var() {
    return g_debugListVar;
}

ConfigVarHandle filesel_config_var() {
    return g_fileSelectVar;
}

ConfigVarHandle viewer_config_var() {
    return g_viewerVar;
}

bool collection_slot_enabled() {
    return get_bool(g_slotVar, true);
}

bool auto_open_enabled() {
    return get_bool(g_autoOpenVar, true);
}

PostButton post_button() {
    int64_t value = static_cast<int64_t>(PostButton::Hd);
    if (g_postVar != 0 && svc_config != nullptr) {
        svc_config->get_int(mod_ctx, g_postVar, &value);
    }
    if (value < 0 || value > static_cast<int64_t>(PostButton::Hidden)) {
        value = static_cast<int64_t>(PostButton::Hd);
    }
    return static_cast<PostButton>(value);
}

bool debug_stamp_list() {
    return get_bool(g_debugListVar, false);
}

bool filesel_counter_enabled() {
    return get_bool(g_fileSelectVar, true);
}

bool filesel_viewer_enabled() {
    return get_bool(g_viewerVar, true);
}

MenuScaling menu_scaling() {
    return g_scaling;
}

void set_menu_scaling(MenuScaling mode) {
    if (mode == g_scaling) {
        return;
    }
    g_scaling = mode;
    if (g_scalingVar != 0 && svc_config != nullptr) {
        svc_config->set_int(mod_ctx, g_scalingVar, static_cast<int64_t>(mode));
    }
}

void note_wide_menus(bool wide) {
    // The file select only tells GameCube from the stretched modes (fileSelectWide); Wii and
    // Dusklight scale the book the same way.
    if (!wide) {
        set_menu_scaling(MenuScaling::GameCube);
    } else if (g_scaling == MenuScaling::GameCube) {
        set_menu_scaling(MenuScaling::Wii);
    }
}

ModResult initialize() {
    assets::load();
    const int hostFailures = host::install();
    const int slotFailures = slot::install();
    const int fileFailures = filesel::install();
    register_overrides();
    g_initialized = true;
    mods::log::info("stamp book: {} (archive {}, host hooks {}/{}, slot hooks {}/{}, file select {}); "
                    "slot {}, automatic opening {}",
        integrated() ? "HD book on the Collection screen" : "RmlUi fallback",
        assets::archive_ready() ? "ready" : assets::archive_problem(), host::hooks_installed(),
        host::hooks_wanted(), slot::hooks_installed(), slot::hooks_wanted(),
        fileFailures == 0 ? "ok" : "partial", slot_available() ? "on" : "off",
        reward_sequence_available() ? "on" : "off");
    (void)hostFailures;
    (void)slotFailures;
    return MOD_OK;
}

void shutdown() {
    if (!g_initialized) {
        return;
    }
    // The slot first: a Collection screen the host puts back while closing the book must not get
    // the slot injected again by hooks that are about to go away.
    slot::shutdown();
    host::shutdown();
    filesel::shutdown();
    for (MessageOverrideHandle handle : g_overrides) {
        svc_message->remove_override(mod_ctx, handle);
    }
    g_overrides.clear();
    assets::unload();
    g_initialized = false;
}

void update() {
    filesel::update();
}

bool book_available() {
    return assets::archive_ready() && host::hooks_ok();
}

bool slot_available() {
    return slot::hooks_ok() && collection_slot_enabled() && assets::slot_icon() != nullptr;
}

bool integrated() {
    return slot_available() && book_available();
}

bool reward_sequence_available() {
    // The reward is the automatic opening (mode 3); it does not need the Collection slot.
    return book_available() && host::auto_open_hooks_ok() && auto_open_enabled();
}

std::string diagnostics() {
    std::string out;
    if (integrated()) {
        out = "Stamp book: OK.";
    } else if (!assets::archive_ready()) {
        out = fmt::format("Stamp book: using the stamp list, because HD's book files are not usable ({}).",
            assets::archive_problem());
    } else if (!host::hooks_ok() || !slot::hooks_ok()) {
        out = fmt::format("Stamp book: using the stamp list, because some game hooks are missing (book {}/{}, "
                          "slot {}/{}; see the log).",
            host::hooks_installed(), host::hooks_wanted(), slot::hooks_installed(), slot::hooks_wanted());
    } else if (assets::slot_icon() == nullptr) {
        out = "Stamp book: using the stamp list, because the slot icon (res/stamps/stampbook/stampicon.bti) is "
              "missing.";
    } else {
        out = "Stamp book: using the stamp list, because the Collection-screen stamp slot is turned off.";
    }
    // Only what is broken (the options themselves are shown above).
    if (!host::auto_open_hooks_ok()) {
        out += " Opening by itself: unavailable.";
    }
    if (filesel_viewer_enabled() && !filesel::viewer_ready()) {
        out += " File-select stamp book: unavailable.";
    }
    return out;
}

void on_slot_changed() {
    host::disarm();
    filesel::invalidate();
}

} // namespace hdstamps::book
