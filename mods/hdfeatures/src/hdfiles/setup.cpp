// HD Features: setting up the game files (see setup.hpp). Module R2.
//
// No game headers here (the DVD calls are in dvd_source.cpp, the overlay calls in R1's files). Library headers
// first, then the SDK's.

#include "hdfiles/setup.hpp"

#include "hdfiles/cache.hpp"
#include "hdfiles/dvd_source.hpp"
#include "hdfiles/files.hpp"
#include "hdfiles/lines.hpp"  // the player's lines and toasts, and line_for()
#include "hdfiles/overlays.hpp"
#include "hdfiles/worker.hpp"

#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <mods/api.h>
#include <mods/service.hpp>
#include <mods/svc/config.h>
#include <mods/svc/file.h>
#include <mods/svc/host.h>
#include <mods/svc/log.hpp>
#include <mods/svc/ui.h>

#include "hdfeatures.hpp"

namespace hdfiles::setup {
namespace {

namespace fs = std::filesystem;

constexpr const char* kRemakeRequest = "remake.request";  // Correction #14: acted on at the next start
constexpr const char* kDeleteRequest = "delete.request";  // at mod_shutdown, else at the next start

enum class Stage : std::uint8_t {
    Off,          // before initialize()
    Blocked,      // nothing can be set up this session (g_line says why)
    Ready,        // the stored files are valid (R1: Registered / Ready)
    Pending,      // the worker starts at the next mod_update (Correction #4)
    Interrupted,  // setup.running was found at start: waits for "Try again" (Correction #1c)
    Waiting,      // another Dusklight holds the lock (Correction #15): polled
    Running,      // the worker runs
    NeedFolder,   // nothing found: the folder picker
    Failed,       // the worker reported a problem (g_line)
};

struct TestSteps {  // files-test (hidden)
    bool noAutodetect = false;
    std::string dusklight;  // "dusklight=<version>": check as if HostService said this (tests Correction #8)
    std::int64_t pickAt = -1, tryAgainAt = -1, remakeAt = -1, deleteAt = -1;
};

Stage g_stage = Stage::Off;
std::string g_line;              // Blocked / Failed
bool g_update = false;           // a mod update changed the files (cache.set): "Updating the game files"
bool g_toastSetup = false;       // shown this session
bool g_toastNeed = false;
bool g_toastProblem = false;
bool g_remakeRequested = false;  // pressed in this session (the request file is acted on at the next start)
bool g_deleteRequested = false;
bool g_remakeRun = false;        // this session acts on a remake request: the stored set is kept until a usable
                                 // TP HD is found (wave3_R2_verify F6; worker::Input::keep_stored)
bool g_remakeKept = false;       // ...and none was found: the kept files are in use (kLineRemakeNeedFolder)
fs::path g_dataDir;
std::string g_dusklight;         // HostService version
std::shared_ptr<const hdf::MemorySource> g_gc;
std::unique_ptr<worker::Worker> g_worker;
std::optional<fs::path> g_picked;  // chosen in this session
bool g_pickWaiting = false;        // chosen while a run was going: used if that run doesn't make the files
bool g_autoRun = true;             // the current / last run was started by the mod itself (toasts)
std::uint64_t g_ticks = 0;
int g_waitTimer = 0;
worker::Phase g_lastPhase = worker::Phase::Starting;
ConfigVarHandle g_varFolder = 0;
ConfigVarHandle g_varIgnoreVersion = 0;
ConfigVarHandle g_varTest = 0;
TestSteps g_test;
std::string g_loggedLine;          // the panel line last written to the log (digits left out)

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string get_string(ConfigVarHandle var) {
    if (var == 0 || svc_config == nullptr) {
        return {};
    }
    std::size_t length = 0;
    if (svc_config->get_string(mod_ctx, var, nullptr, 0, &length) != MOD_OK || length == 0) {
        return {};
    }
    std::string value(length + 1, '\0');
    if (svc_config->get_string(mod_ctx, var, value.data(), value.size(), &length) != MOD_OK) {
        return {};
    }
    value.resize(length);
    return value;
}

bool get_bool(ConfigVarHandle var) {
    bool value = false;
    if (var != 0 && svc_config != nullptr) {
        svc_config->get_bool(mod_ctx, var, &value);
    }
    return value;
}

ConfigVarHandle register_var(const char* name, ConfigVarType type) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = type;
    ConfigVarHandle handle = 0;
    if (svc_config == nullptr || svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK) {
        mods::log::warn("could not register the '{}' option", name);
        return 0;
    }
    return handle;
}

void parse_test(std::string_view text) {
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('+', at), text.size());
        const std::string_view token = text.substr(at, end - at);
        at = end + 1;
        if (token.empty()) {
            continue;
        }
        if (token == "no_autodetect") {
            g_test.noAutodetect = true;
            continue;
        }
        if (token.starts_with("dusklight=")) {
            g_test.dusklight = std::string(token.substr(10));
            continue;
        }
        const std::size_t sep = token.find('@');
        std::int64_t tick = -1;
        if (sep != std::string_view::npos) {
            const std::string_view number = token.substr(sep + 1);
            std::from_chars(number.data(), number.data() + number.size(), tick);
        }
        const std::string_view name = token.substr(0, sep);
        if (name == "pick") {
            g_test.pickAt = tick;
        } else if (name == "try_again") {
            g_test.tryAgainAt = tick;
        } else if (name == "remake") {
            g_test.remakeAt = tick;
        } else if (name == "delete") {
            g_test.deleteAt = tick;
        } else {
            mods::log::warn("files-test: unknown step '{}'", token);
        }
    }
}

std::string hex(const std::uint8_t* data, std::uint32_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::uint32_t i = 0; data != nullptr && i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 15]);
    }
    return out;
}

std::string mod_version() {
    const char* v = svc_host != nullptr && svc_host->mod_version != nullptr ? svc_host->mod_version(mod_ctx) : nullptr;
    return v != nullptr ? v : "";
}

bool get_data_dir() {
    const char* path = nullptr;
    if (svc_host == nullptr || svc_host->header.minor_version < 2 || svc_host->data_dir == nullptr ||
        svc_host->data_dir(mod_ctx, &path) != MOD_OK || path == nullptr || path[0] == '\0')
    {
        return false;
    }
    g_dataDir = cache::u8_path(path);
    return !g_dataDir.empty();
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool write_request(const char* name) {
    std::ofstream f(g_dataDir / name, std::ios::binary | std::ios::trunc);
    f << "asked for in the Mods panel\n";
    f.flush();
    return static_cast<bool>(f);
}

void remove_file(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

void toast(const char* body) {
    if (svc_ui == nullptr) {
        return;
    }
    UiToastDesc desc = UI_TOAST_DESC_INIT;
    desc.body_rml = body;  // plain text without RML specials
    desc.duration_ms = 6000;
    if (svc_ui->push_toast(mod_ctx, &desc) == MOD_OK) {
        hdfeatures::note_toast(desc.duration_ms);
        mods::log::info("game files: toast \"{}\"", body);
    }
}

void block(const char* line) {
    g_stage = Stage::Blocked;
    g_line = line;
}

// At most one problem toast per start (plan 1.2): a failed run the mod started itself, a start that can't set
// up at all, or one that waits for "Try again" (wave 4 review M1). The panel line says why.
void toast_problem() {
    if (!g_toastProblem) {
        g_toastProblem = true;
        toast(kToastProblem);
    }
}

// The stored files and setup's own leftovers, for "Delete the game files" (Correction #14). cache.json first,
// so nothing is valid any more even if a folder can't be removed now.
bool delete_files() {
    cache::drop(g_dataDir);
    hdf::cleanup(g_dataDir, nullptr);  // every files-* and tmp-* folder, and cache.json.tmp
    remove_file(g_dataDir / worker::kMarkerFile);
    remove_file(g_dataDir / kRemakeRequest);
    bool left = file_exists(g_dataDir / std::string(hdf::kStampFile));
    std::error_code ec;
    for (fs::directory_iterator it(g_dataDir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = cache::path_utf8(it->path().filename());
        if (name.starts_with("files-") || name.starts_with("tmp-")) {
            left = true;
        }
    }
    return !left;
}

// Correction #14: the two Troubleshooting buttons only leave a request file; it is acted on here, before the
// cache check (and a delete also at mod_shutdown).
void act_on_requests() {
    if (cache::another_instance_is_making(g_dataDir)) {
        return;  // Correction #15: touch nothing while another Dusklight makes files here (next start)
    }
    if (file_exists(g_dataDir / kDeleteRequest)) {
        const bool done = delete_files();
        if (done) {
            remove_file(g_dataDir / kDeleteRequest);
        }
        mods::log::info("game files: {}", done ? "deleted (asked for in Troubleshooting); they are made again now" :
                                                 "could not all be deleted now; trying again at the next start");
    }
    if (file_exists(g_dataDir / kRemakeRequest)) {
        if (cache::Cache::open(g_dataDir)) {
            // wave3_R2_verify F6: the stored set stays valid until a usable TP HD is found (the worker's
            // prepare_files drops it then). The request file stays until that run ends.
            g_remakeRun = true;
            mods::log::info("game files: making them again (asked for in Troubleshooting); the stored ones are kept "
                            "until your Twilight Princess HD is found");
        } else {
            cache::drop(g_dataDir);
            remove_file(g_dataDir / kRemakeRequest);
            mods::log::info("game files: making them again (asked for in Troubleshooting)");
        }
    }
}

// The folder searched first when nothing was chosen in this session: files-hd_folder, else the location the
// last stored files were made from (cache.json hd.location, Correction #7: tried first after a mod update).
std::optional<fs::path> remembered() {
    const std::string chosen = get_string(g_varFolder);
    if (!chosen.empty()) {
        return cache::u8_path(chosen);
    }
    if (auto stamp = hdf::read_stamp(g_dataDir); stamp && !stamp->hd.location.empty()) {
        return cache::u8_path(stamp->hd.location);
    }
    return std::nullopt;
}

void start_worker(bool automatic) {
    if (g_worker != nullptr) {
        return;  // finish_worker() first
    }
    worker::Input in;
    in.data_dir = g_dataDir;
    in.picked = g_picked;
    if (!g_picked) {
        in.remembered = remembered();
    }
    in.autodetect = !g_test.noAutodetect;
    in.keep_stored = g_remakeRun;
    in.gc = g_gc;
    in.mod_version = mod_version();
    in.dusklight = g_dusklight;
    mods::log::info("game files: setup started ({}{})",
        in.picked ? "the chosen folder " + worker::shorten(cache::short_path(*in.picked)) :
        in.remembered ? std::string("the remembered ") + worker::location_word(*in.remembered) + " " +
                            worker::shorten(cache::short_path(*in.remembered)) :
                        std::string("no folder chosen"),
        in.picked ? "" : in.autodetect ? ", then Cemu's lists" : ", automatic search off (files-test)");
    g_pickWaiting = false;
    g_autoRun = automatic;
    g_lastPhase = worker::Phase::Starting;
    g_worker = std::make_unique<worker::Worker>(std::move(in));
    g_worker->start();
    g_stage = Stage::Running;
}

void drain_log() {
    if (g_worker == nullptr) {
        return;
    }
    for (const std::string& line : g_worker->drain_log()) {
        mods::log::info("game files: {}", line);
    }
}

// The hand-over (wave3 F4): the worker opened and checked the stamp and hashed the 56 buffers on its thread, so
// this frame only compares the hashes and calls add_buffer.
State adopt_result(worker::Result& r) {
    if (!r.stored) {
        // Not expected: Done, FromDisk and Kept always carry their set (worker.cpp).
        mods::log::error("game files: the worker handed over no stored set");
        return State::Failed;
    }
    return hdfiles::adopt(std::move(*r.stored), std::move(r.overlays));
}

void finish_worker() {
    drain_log();
    const double joinSeconds = g_worker->join();
    worker::Result r = g_worker->take_result();
    g_worker.reset();
    if (joinSeconds > 1.0) {
        mods::log::warn("game files: the worker took {:.1f} s to end", joinSeconds);
    }
    if (g_pickWaiting && r.outcome != worker::Outcome::Done && r.outcome != worker::Outcome::FromDisk) {
        start_worker(false);  // a folder was chosen meanwhile (Correction #7: a pick replaces a search)
        return;
    }
    if (g_remakeRun && r.outcome != worker::Outcome::OtherInstance && r.outcome != worker::Outcome::Stopped) {
        g_remakeRun = false;  // the remake request was acted on, whatever the result
        remove_file(g_dataDir / kRemakeRequest);
    }
    switch (r.outcome) {
    case worker::Outcome::Kept: {
        // wave3_R2_verify F6: "Make the game files again" found no usable TP HD; the stored files are used.
        mods::log::info("game files: not made again ({}); the stored ones are used. Choose your Twilight Princess "
                        "HD folder in Mods > HD Features to make them again at the next start",
            worker::shorten(r.detail));
        const State state = adopt_result(r);
        if (state == State::Registered || state == State::Ready) {
            g_stage = Stage::Ready;
            g_remakeKept = true;
            g_gc.reset();
            if (!g_toastNeed) {
                g_toastNeed = true;
                toast(kToastNeedFolder);
            }
        } else {
            g_stage = Stage::Failed;
            g_line = kLineLoad;
            toast_problem();
        }
        return;
    }
    case worker::Outcome::Done:
    case worker::Outcome::FromDisk: {
        if (r.outcome == worker::Outcome::Done) {
            mods::log::info("game files: made in {:.1f} s (making {:.1f} s) from the {} {}", r.seconds, r.make_seconds,
                r.hd_kind, r.hd_location);
        } else {
            mods::log::info("game files: another Dusklight made them; read in {:.1f} s", r.seconds);
        }
        const State state = adopt_result(r);
        if (state == State::Registered || state == State::Ready) {
            g_stage = Stage::Ready;
            g_gc.reset();  // not needed any more this session (1 MB)
            if (g_toastSetup) {
                toast(kToastReady);
            }
        } else {
            g_stage = Stage::Failed;
            g_line = kLineLoad;
            if (g_autoRun) {
                toast_problem();
            }
        }
        return;
    }
    case worker::Outcome::Stopped:
        mods::log::info("game files: setup stopped");
        g_stage = g_picked ? Stage::Pending : Stage::NeedFolder;
        return;
    case worker::Outcome::OtherInstance:
        g_stage = Stage::Waiting;
        g_waitTimer = 0;
        return;
    case worker::Outcome::NotFound:
        if (r.picked) {
            mods::log::info("game files: no Twilight Princess HD in the chosen folder");
            g_stage = Stage::Failed;
            g_line = kLineEmptyFolder;
            return;
        }
        mods::log::info("game files: no Twilight Princess HD found; the folder picker is in Mods > HD Features");
        g_stage = Stage::NeedFolder;
        if (!g_toastNeed) {
            g_toastNeed = true;
            toast(g_update ? kToastUpdateNeedFolder : kToastNeedFolder);
        }
        return;
    case worker::Outcome::Problem:
    case worker::Outcome::None:
        g_stage = Stage::Failed;
        g_line = line_for(r);
        mods::log::warn("game files: setup failed ({}: {}) after {:.1f} s", r.code.empty() ? "none" : r.code,
            worker::shorten(r.detail), r.seconds);
        if (g_autoRun) {
            toast_problem();
        }
        return;
    }
}

void folder_chosen(const fs::path& folder) {
    g_picked = folder;
    switch (g_stage) {
    case Stage::Running: {
        const worker::Phase phase = g_worker != nullptr ? g_worker->phase() : worker::Phase::Finished;
        g_pickWaiting = true;
        if (phase == worker::Phase::Starting || phase == worker::Phase::Searching) {
            g_worker->request_stop();  // Correction #7: a pick during a search cancels it
            mods::log::info("game files: the search stops; the chosen folder is searched instead");
        } else {
            mods::log::info("game files: the files are being made from the copy found; the chosen folder is used "
                            "only if that fails");
        }
        return;
    }
    case Stage::Pending:
    case Stage::Interrupted:
    case Stage::NeedFolder:
    case Stage::Failed:
        start_worker(false);
        return;
    case Stage::Waiting:
        return;  // searched when the other Dusklight is done
    case Stage::Ready:
        if (g_remakeKept && !g_remakeRequested && !g_deleteRequested) {
            // The kept files stay live this session (Correction #14: no swap of live overlays); the remake runs
            // at the next start and searches this folder first (files-hd_folder).
            g_remakeRequested = write_request(kRemakeRequest);
            mods::log::info("game files: {}", g_remakeRequested ? "they are made again from that folder at the next start" :
                                                                  "could not write the request");
            return;
        }
        mods::log::info("game files: folder remembered for later");
        return;
    default:
        mods::log::info("game files: folder remembered for later");
        return;
    }
}

void on_pick(ModContext*, ModResult status, const char* const* locations, std::uint32_t count, const char* error, void*) {
    if (status != MOD_OK || count == 0 || locations == nullptr || locations[0] == nullptr || locations[0][0] == '\0') {
        // Correction #7: a cancelled picker changes nothing and says nothing.
        if (status != MOD_UNAVAILABLE) {
            mods::log::info("game files: folder picker: result {} {}", static_cast<int>(status),
                error != nullptr ? error : "");
        }
        return;
    }
    const std::string location = locations[0];
    if (g_varFolder != 0) {
        svc_config->set_string(mod_ctx, g_varFolder, location.c_str());
    }
    const fs::path folder = cache::u8_path(location);
    mods::log::info("game files: folder chosen: {}", worker::shorten(cache::short_path(folder)));
    folder_chosen(folder);
}

void run_test_steps() {
    const auto due = [](std::int64_t at) { return at >= 0 && g_ticks == static_cast<std::uint64_t>(at); };
    if (due(g_test.pickAt)) {
        mods::log::info("files-test: pick at mod_update {}", g_ticks);
        choose_folder();
    }
    if (due(g_test.tryAgainAt)) {
        mods::log::info("files-test: try again at mod_update {}", g_ticks);
        try_again();
    }
    if (due(g_test.remakeAt)) {
        mods::log::info("files-test: make again at mod_update {}", g_ticks);
        request_remake();
    }
    if (due(g_test.deleteAt)) {
        mods::log::info("files-test: delete at mod_update {}", g_ticks);
        request_delete();
    }
}

}  // namespace

void register_config() {
    g_test = TestSteps{};
    g_varFolder = register_var("files-hd_folder", CONFIG_VAR_STRING);
    g_varIgnoreVersion = register_var("files-ignore_dusklight_version", CONFIG_VAR_BOOL);
    g_varTest = register_var("files-test", CONFIG_VAR_STRING);
    const std::string test = get_string(g_varTest);
    if (!test.empty()) {
        parse_test(test);
        mods::log::info("files-test: {}", test);
    }
}

bool initialize() {
    const auto t0 = std::chrono::steady_clock::now();
    g_stage = Stage::Off;
    g_ticks = 0;
    g_line.clear();
    g_loggedLine.clear();
    g_update = g_toastSetup = g_toastNeed = g_toastProblem = false;
    g_remakeRequested = g_deleteRequested = false;
    g_remakeRun = g_remakeKept = false;
    g_picked.reset();
    g_pickWaiting = false;
    g_autoRun = true;
    g_gc.reset();
    g_dataDir.clear();

    // Correction #8: the Dusklight build this DLL is made for. Another build: nothing runs, the files are kept.
    g_dusklight = svc_host != nullptr && svc_host->version != nullptr ? svc_host->version : "";
    const std::string build = svc_host != nullptr ? hex(svc_host->build_id, svc_host->build_id_len) : std::string();
    const std::string seen = g_test.dusklight.empty() ? g_dusklight : g_test.dusklight;
    const bool sameDusklight = seen == hdf::kDusklightVersion && build == hdf::kDusklightBuildId;
    if (!sameDusklight) {
        if (get_bool(g_varIgnoreVersion)) {
            mods::log::warn("game files: Dusklight {} (build {}) is not {} (build {}); checked anyway "
                            "(files-ignore_dusklight_version)", seen, build, hdf::kDusklightVersion,
                hdf::kDusklightBuildId);
        } else {
            mods::log::error("game files: Dusklight {} (build {}) is not the {} build HD Features is made for; the "
                             "stamps and the cave stay off and the stored files are kept", seen, build,
                hdf::kDusklightVersion);
            block(kLineDusklight);
            hdfiles::shutdown();  // nothing of R1's runs: no stored file is read or added this session
            return false;
        }
    }

    // Plan 2.4: the GC disc id at every start. Another disc: features off, the files are kept.
    const std::string disc = dvd::disc_id();
    if (disc != hdf::inputs::kGcGameId) {
        mods::log::error("game files: the GameCube disc is {}, not {}; the stamps and the cave stay off",
            disc.empty() ? "unknown" : disc, hdf::inputs::kGcGameId);
        block(kLineDisc);
        hdfiles::shutdown();
        return false;
    }

    if (get_data_dir()) {
        act_on_requests();  // Correction #14, before the cache check
    }
    if (g_remakeRun && file_exists(g_dataDir / worker::kMarkerFile)) {
        // A remake that never ended while the stored set was still valid (a crash while searching): not repeated
        // by itself (Correction #1c); the stored files are used as before.
        mods::log::warn("game files: making them again stopped unexpectedly last time ({} found); the stored ones "
                        "are used", worker::kMarkerFile);
        g_remakeRun = false;
        remove_file(g_dataDir / kRemakeRequest);
    }

    // R1: the cache check and the 56 overlays. A remake (F6) adds no overlay now: the worker hands over the new
    // files, or the kept ones (hdfiles::adopt). R1 stays Off meanwhile (no stored file is read before that).
    if (g_remakeRun) {
        hdfiles::shutdown();
    }
    const State state = g_remakeRun ? State::NoCache : hdfiles::initialize();
    if (state == State::Registered || state == State::Ready) {
        g_stage = Stage::Ready;
        if (file_exists(g_dataDir / worker::kMarkerFile) && !cache::another_instance_is_making(g_dataDir)) {
            remove_file(g_dataDir / worker::kMarkerFile);  // stale: the files it was about are valid
        }
        return true;
    }
    if (state != State::NoCache || g_dataDir.empty()) {
        // R1 logged the reason. io.read: locked now, not damage (the stamp stays; wave3_R1 8.5).
        block(hdfiles::error_code() == "io.read" ? kLineUnreadable : kLineLoad);
        return false;
    }
    g_update = !g_remakeRun && hdfiles::error_code() == "cache.set";

    // Plan 2.3 step 4: the 13 GC inputs, now, before any overlay of this mod is live. In a mid-session init
    // (installed or updated while Dusklight runs: the game's frame counter has moved) another mod's overlay may
    // already be live: a mismatch then needs a restart, not another disc (S1 spike 8).
    const bool midSession = overlays::frame_counter() != 0;
    auto gc = dvd::read_gc_inputs();
    if (!gc && g_remakeRun) {
        // They can't be made again now: the stored files are used and the request stays for the next start.
        mods::log::warn("game files: the GameCube inputs differ ({}: {}); they are made again at the next start, the "
                        "stored ones are used now", gc.error().code, gc.error().detail);
        g_remakeRun = false;
        g_remakeRequested = true;  // kLineRemake
        const State kept = hdfiles::initialize();
        if (kept == State::Registered || kept == State::Ready) {
            g_stage = Stage::Ready;
            return true;
        }
        g_remakeRequested = false;
        block(hdfiles::error_code() == "io.read" ? kLineUnreadable : kLineLoad);
        return false;
    }
    if (!gc) {
        mods::log::error("game files: the GameCube inputs differ ({}: {}){}", gc.error().code, gc.error().detail,
            midSession ? "; read in a mid-session start, so another mod may overlay them: restart Dusklight" : "");
        block(midSession ? kLineGcRestart : kLineGcCold);
        if (midSession) {
            toast(kToastRestart);  // installed or updated while Dusklight runs: the player is right here
            g_toastProblem = true;  // ...so no second toast for this start (tick())
        }
        return false;
    }
    g_gc = gc->source;
    mods::log::info("game files: read the {} GameCube inputs ({} bytes) in {:.3f} s{}; frame {}", gc->files, gc->bytes,
        gc->seconds, midSession ? " (mid-session start)" : "", overlays::frame_counter());

    // Correction #15 before #1c (wave3_R2_verify F3): another Dusklight making the files in this folder holds the
    // lock, and its setup.running is no crash. Wait for it (tick() polls the lock).
    if (cache::another_instance_is_making(g_dataDir)) {
        mods::log::info("game files: another Dusklight is making them in this folder; waiting for it");
        g_stage = Stage::Waiting;
        g_waitTimer = 0;
        return false;
    }
    // Correction #1c: a run that never ended (a crash or a killed process) is not repeated by itself.
    if (file_exists(g_dataDir / worker::kMarkerFile)) {
        mods::log::warn("game files: setup stopped unexpectedly last time ({} found); it starts again with Try again "
                        "in Mods > HD Features", worker::kMarkerFile);
        g_stage = Stage::Interrupted;
        return false;
    }
    g_stage = Stage::Pending;
    mods::log::info("game files: {}; setup starts at the first update ({:.3f} s)",
        g_update ? "made by another HD Features version, so they are made again" : "they are made now", seconds_since(t0));
    return false;
}

void tick() {
    ++g_ticks;
    run_test_steps();
    drain_log();
    switch (g_stage) {
    case Stage::Pending:
        start_worker(true);
        break;
    case Stage::Blocked:
    case Stage::Interrupted:
        // wave 4 review M1: nothing is set up this start (another Dusklight build, another disc, the stored
        // files can't be read, ...) or it waits for "Try again": say so once, from the first mod_update (not
        // from mod_initialize), so the player knows where to look.
        toast_problem();
        break;
    case Stage::Waiting:
        if (++g_waitTimer >= 120) {
            g_waitTimer = 0;
            if (cache::another_instance_is_making(g_dataDir)) {
                break;
            }
            // The other run removes its setup.running before it lets the lock go (worker.cpp: the Marker is
            // destroyed before the LockFile). Still there: that run never ended (Correction #1c).
            if (file_exists(g_dataDir / worker::kMarkerFile) && !cache::Cache::open(g_dataDir)) {
                mods::log::warn("game files: the other Dusklight's setup stopped unexpectedly ({} left); it starts "
                                "again with Try again in Mods > HD Features", worker::kMarkerFile);
                g_stage = Stage::Interrupted;
                break;
            }
            start_worker(true);
        }
        break;
    case Stage::Running:
        if (g_worker == nullptr) {
            g_stage = Stage::Failed;
            g_line = kLineOther;
            break;
        }
        if (const worker::Phase phase = g_worker->phase(); phase != g_lastPhase) {
            g_lastPhase = phase;
            if (phase == worker::Phase::Making && !g_update && !g_toastSetup) {
                g_toastSetup = true;
                toast(kToastSetup);
            }
        }
        if (g_worker->finished()) {
            finish_worker();
        }
        break;
    default:
        break;
    }
    // The panel line in the log whenever the state changes (not at every percent): the log shows what the
    // player is told.
    std::string shown = line();
    std::erase_if(shown, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    if (shown != g_loggedLine) {
        g_loggedLine = shown;
        mods::log::info("game files: panel line \"{}\"", line());
    }
}

void stop_worker() {
    if (g_worker == nullptr) {
        return;
    }
    g_worker->request_stop();
    const double seconds = g_worker->join();
    drain_log();
    const worker::Result r = g_worker->take_result();
    g_worker.reset();
    const bool slow = seconds > 1.0;  // Correction #5: a join that takes over 1 s is logged as such
    if (slow) {
        mods::log::warn("game files: setup stopped for the shutdown; the join took {:.3f} s", seconds);
    } else {
        mods::log::info("game files: setup stopped for the shutdown (join {:.3f} s, {})", seconds,
            r.outcome == worker::Outcome::Stopped ? "stopped" : "it had finished");
    }
    if (g_stage == Stage::Running) {
        g_stage = Stage::Pending;
    }
}

void shutdown() {
    if (g_deleteRequested && !g_dataDir.empty() && g_worker == nullptr &&
        !cache::another_instance_is_making(g_dataDir))
    {
        const bool done = delete_files();
        if (done) {
            remove_file(g_dataDir / kDeleteRequest);
        }
        mods::log::info("game files: {}", done ? "deleted (asked for in Troubleshooting)" :
                                                 "could not all be deleted now; trying again at the next start");
    }
    g_gc.reset();
    g_picked.reset();
    g_stage = Stage::Off;
}

std::string line() {
    switch (g_stage) {
    case Stage::Off:
        return {};
    case Stage::Blocked:
    case Stage::Failed:
        return g_line;
    case Stage::Ready:
        if (g_deleteRequested) {
            return kLineDelete;
        }
        if (g_remakeRequested) {
            return kLineRemake;
        }
        if (hdfiles::damaged()) {
            return kLineDamaged;
        }
        return g_remakeKept ? kLineRemakeNeedFolder : kLineReady;
    case Stage::Pending:
        return kLineSearching;
    case Stage::Interrupted:
        return kLineInterrupted;
    case Stage::Waiting:
        return kLineOtherInstance;
    case Stage::NeedFolder:
        return g_update ? kLineUpdateNeedFolder : kLineNeedFolder;
    case Stage::Running: {
        const worker::Phase phase = g_worker != nullptr ? g_worker->phase() : worker::Phase::Starting;
        if (phase == worker::Phase::Starting || phase == worker::Phase::Searching) {
            return kLineSearching;
        }
        const int pct = static_cast<int>(std::clamp(progress(), 0.0f, 0.99f) * 100.0f);
        return g_update ? updating_line(pct) : making_line(pct);
    }
    }
    return {};
}

float progress() {
    if (g_stage != Stage::Running || g_worker == nullptr) {
        return -1.0f;
    }
    const worker::Phase phase = g_worker->phase();
    if (phase != worker::Phase::Making && phase != worker::Phase::Storing) {
        return -1.0f;
    }
    const hdf::Progress& p = g_worker->progress();
    const std::uint32_t total = p.total.load(std::memory_order_relaxed);
    const std::uint32_t done = p.done.load(std::memory_order_relaxed);
    if (total == 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f);
}

bool show_setup_buttons() {
    switch (g_stage) {
    case Stage::Pending:
    case Stage::Interrupted:
    case Stage::Waiting:
    case Stage::Running:
    case Stage::NeedFolder:
    case Stage::Failed:
        return true;
    default:
        return false;
    }
}

bool show_folder_button() {
    return show_setup_buttons() || (g_stage == Stage::Ready && g_remakeKept);
}

bool can_choose_folder() {
    switch (g_stage) {
    case Stage::Pending:
    case Stage::Interrupted:
    case Stage::NeedFolder:
    case Stage::Failed:
        return true;
    case Stage::Ready:
        return g_remakeKept && !g_remakeRequested && !g_deleteRequested;
    case Stage::Running:
        return g_worker != nullptr &&
               (g_worker->phase() == worker::Phase::Starting || g_worker->phase() == worker::Phase::Searching);
    default:
        return false;
    }
}

bool can_try_again() {
    return g_stage == Stage::Interrupted || g_stage == Stage::NeedFolder || g_stage == Stage::Failed;
}

bool show_manage_buttons() {
    return g_stage == Stage::Ready;
}

bool can_manage() {
    return g_stage == Stage::Ready && !g_remakeRequested && !g_deleteRequested;
}

void choose_folder() {
    if (!can_choose_folder()) {
        return;
    }
    if (svc_file == nullptr) {
        mods::log::warn("game files: the folder picker is not available");
        return;
    }
    FilePickOptions options = FILE_PICK_OPTIONS_INIT;
    const std::string current = get_string(g_varFolder);
    if (!current.empty()) {
        options.default_location = current.c_str();
    }
    const ModResult result = svc_file->pick_folder(mod_ctx, &options, &on_pick, nullptr);
    if (result != MOD_OK && result != MOD_CONFLICT) {  // MOD_CONFLICT: a picker is already open
        mods::log::warn("game files: the folder picker could not open (result {})", static_cast<int>(result));
    }
}

void try_again() {
    if (!can_try_again()) {
        return;
    }
    mods::log::info("game files: trying again");
    start_worker(false);
}

void request_remake() {
    if (!can_manage()) {
        return;
    }
    g_remakeRequested = write_request(kRemakeRequest);
    mods::log::info("game files: {}", g_remakeRequested ? "they are made again at the next start" :
                                                          "could not write the request");
}

void request_delete() {
    if (!can_manage()) {
        return;
    }
    g_deleteRequested = write_request(kDeleteRequest);
    mods::log::info("game files: {}", g_deleteRequested ? "they are deleted when Dusklight closes" :
                                                          "could not write the request");
}

}  // namespace hdfiles::setup
