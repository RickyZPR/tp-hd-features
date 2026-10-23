// HD Features: the setup worker thread (see worker.hpp). Module R2.
//
// No game headers and no SDK in this file (it runs on its own thread). windows.h is fine here: it is only a
// problem next to the game headers (dolphin/types.h), which this file never includes.

#include "hdfiles/worker.hpp"

#include "hdfiles/cache.hpp"

#include "hdf/detect.hpp"
#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hdfiles::worker {
namespace {

namespace fs = std::filesystem;

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

fs::path env_path(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? fs::path(std::wstring(buf, n)) : fs::path();
}

std::string utc_now() {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// <data_dir>\lock, held without sharing while this run lasts (Correction #15). FILE_FLAG_DELETE_ON_CLOSE: the
// file goes away when the handle is closed, also when the process dies.
struct LockFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    LockFile() = default;
    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;
    ~LockFile() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

// <data_dir>\setup.running (Correction #1c): removed when the run ends, whatever the result (also on an
// exception: stack unwinding runs this).
struct Marker {
    fs::path path;
    bool written = false;
    ~Marker() {
        if (written) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
};

void problem(Result& r, std::string code, std::string detail) {
    r.outcome = Outcome::Problem;
    r.code = std::move(code);
    r.detail = std::move(detail);
}

void problem(Result& r, const hdf::Error& e) {
    problem(r, e.code, e.detail);
}

// The 56 overlays of a valid stored set, each read whole and hashed (cache::Cache::read), into r.overlays; on
// FromDisk the set itself is moved into r.stored for the hand-over. Returns FromDisk, Stopped or Problem (r says
// why); the buffers are dropped unless FromDisk.
Outcome read_stored_overlays(cache::Cache&& stored, const std::stop_token& stop, Result& r) {
    Outcome outcome = Outcome::FromDisk;
    for (const cache::Entry* e : stored.overlays()) {
        if (stop.stop_requested()) {
            outcome = Outcome::Stopped;
            break;
        }
        auto data = stored.read(*e);
        if (!data) {
            problem(r, data.error());
            outcome = Outcome::Problem;
            break;
        }
        // read() compared the bytes' SHA-256 with this entry, so the entry's hash is the bytes' hash (wave3 F4).
        r.overlays.push_back(HandOver{e->rel, std::move(*data), e->sha256});
    }
    if (outcome != Outcome::FromDisk) {
        r.overlays.clear();
    } else {
        r.stored.emplace(std::move(stored));
    }
    return outcome;
}

// wave3 F4: the freshly committed set for the hand-over, opened and checked here (cache::Cache::open: about
// 7 ms), with each overlay buffer hashed here (about 18 ms for the 48 MB), not on the game thread.
bool prepare_hand_over(const std::filesystem::path& dataDir, std::vector<std::pair<std::string, hdf::Bytes>>&& made,
                       Result& r, double& seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    auto opened = cache::Cache::open(dataDir);
    if (!opened) {
        problem(r, opened.error());
        return false;
    }
    r.overlays.clear();
    r.overlays.reserve(made.size());
    for (auto& [rel, bytes] : made) {
        std::string sha = hdf::sha256_hex(bytes);
        r.overlays.push_back(HandOver{std::move(rel), std::move(bytes), std::move(sha)});
    }
    made.clear();
    r.stored.emplace(std::move(*opened));
    seconds = seconds_since(t0);
    return true;
}

}  // namespace

std::string shorten(std::string text) {
    try {
        const fs::path profile = env_path(L"USERPROFILE");
        if (profile.empty()) {
            return text;
        }
        const std::string generic = cache::path_utf8(profile);  // '/' separators
        std::string native = generic;
        std::replace(native.begin(), native.end(), '/', '\\');
        for (const std::string& root : {native, generic}) {
            if (root.size() < 4) {
                continue;
            }
            std::size_t at = 0;
            while (true) {
                // ASCII case-insensitive search (drive letters and folder names may differ in case)
                const auto it = std::search(text.begin() + static_cast<std::ptrdiff_t>(at), text.end(), root.begin(),
                    root.end(), [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                    });
                if (it == text.end()) {
                    break;
                }
                const std::size_t pos = static_cast<std::size_t>(it - text.begin());
                text.replace(pos, root.size(), "%USERPROFILE%");
                at = pos + 13;
            }
        }
    } catch (...) {
    }
    return text;
}

const char* location_word(const fs::path& location) {
    // From the name only: setup.cpp calls this on the game thread, and a remembered location may be on a share
    // that is slow to answer (Correction #5), so no file system call here.
    const std::string ext = lower_ascii(cache::path_utf8(location.extension()));
    return ext == ".wua" ? ".wua" : ext.empty() ? "folder" : "location";
}

Worker::Worker(Input input) : input_(std::move(input)) {}

Worker::~Worker() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Worker::start() {
    thread_ = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

void Worker::request_stop() {
    thread_.request_stop();
}

double Worker::join() {
    const auto t0 = std::chrono::steady_clock::now();
    if (thread_.joinable()) {
        thread_.join();
    }
    return seconds_since(t0);
}

std::vector<std::string> Worker::drain_log() {
    std::lock_guard guard(lock_);
    std::vector<std::string> out;
    out.swap(lines_);
    return out;
}

Result Worker::take_result() {
    std::lock_guard guard(lock_);
    Result r = std::move(result_);
    result_ = Result{};
    return r;
}

void Worker::log(std::string line) {
    try {
        std::lock_guard guard(lock_);
        if (lines_.size() < 400) {  // a bounded queue: the game thread drains it every frame
            lines_.push_back(std::move(line));
        }
    } catch (...) {
    }
}

void Worker::run(std::stop_token stop) {
    // Correction #19: the title screen keeps the CPU first.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto t0 = std::chrono::steady_clock::now();
    Result r;
    try {
        run_steps(stop, r);
    } catch (const std::exception& e) {
        r = Result{};
        problem(r, "gen.exception", e.what());
    } catch (...) {
        r = Result{};
        problem(r, "gen.exception", "unknown exception");
    }
    r.picked = input_.picked.has_value();
    r.seconds = seconds_since(t0);
    {
        std::lock_guard guard(lock_);
        result_ = std::move(r);
    }
    phase_.store(Phase::Finished, std::memory_order_release);
}

bool Worker::keep_stored_set(const std::stop_token& stop, Result& r, const std::string& why) {
    if (!input_.keep_stored) {
        return false;
    }
    auto stored = cache::Cache::open(input_.data_dir);
    if (!stored) {
        log(std::format("not made again ({}), and the stored game files can't be used either ({})", why,
            stored.error().code));
        return false;
    }
    Result kept;
    const Outcome outcome = read_stored_overlays(std::move(*stored), stop, kept);
    if (outcome == Outcome::Problem) {
        log(std::format("not made again ({}), and the stored game files can't be read ({}: {})", why, kept.code,
            kept.detail));
        return false;
    }
    r = std::move(kept);
    r.outcome = outcome == Outcome::FromDisk ? Outcome::Kept : Outcome::Stopped;
    r.detail = why;
    if (r.outcome == Outcome::Kept) {
        log(std::format("not made again ({}): the stored game files are kept and used", why));
    }
    return true;
}

void Worker::run_steps(std::stop_token stop, Result& r) {
    const fs::path& dataDir = input_.data_dir;
    if (dataDir.empty() || input_.gc == nullptr) {
        problem(r, "setup.input", dataDir.empty() ? "no data folder" : "no GameCube inputs");
        return;
    }
    std::error_code ec;

    // 1. The lock (Correction #15).
    LockFile lock;
    const fs::path lockPath = dataDir / kLockFile;
    for (int attempt = 0; attempt < 20; ++attempt) {
        lock.handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock.handle != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION) {
            r.outcome = Outcome::OtherInstance;
            log("another Dusklight is making the game files in this folder; nothing touched");
            return;
        }
        if (err != ERROR_ACCESS_DENIED || stop.stop_requested()) {  // access denied: a lock being deleted
            // "(os error N)": lines.cpp picks the panel's words by the number (the text is in the Windows
            // display language; wave 4 review L3)
            problem(r, "io.write", "lock: " + std::system_category().message(static_cast<int>(err)) + " (os error " +
                                       std::to_string(err) + ")");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (lock.handle == INVALID_HANDLE_VALUE) {
        problem(r, "io.write", "lock: access denied (os error 5)");
        return;
    }

    // 2. The marker (Correction #1c).
    Marker marker{dataDir / kMarkerFile};
    {
        std::ofstream f(marker.path, std::ios::binary | std::ios::trunc);
        f << "pid " << GetCurrentProcessId() << " " << utc_now() << " HD Features " << input_.mod_version << "\n";
        f.flush();
        if (!f) {
            problem(r, "io.write", std::string(kMarkerFile) + ": can't write");
            return;
        }
    }
    marker.written = true;

    // 3. Made meanwhile by another Dusklight (it held the lock until it was done). Not for "Make the game files
    // again": the stored set is the one to replace.
    if (!input_.keep_stored) {
        if (auto stored = cache::Cache::open(dataDir)) {
            log("a valid set of game files is already stored: reading its overlays");
            r.outcome = read_stored_overlays(std::move(*stored), stop, r);
            return;
        }
    }

    // 4. Leftovers (plan 2.4, Correction #9): files-* / tmp-* folders the old stamp doesn't name. An older
    // set's folder stays until the new stamp replaces it (commit cleans up after itself). The stop token ends
    // the retries of a locked leftover (G L3; wave 4 review L1): the rest waits for the next run.
    {
        auto old = hdf::read_stamp(dataDir);
        const std::uint32_t removed = hdf::cleanup(dataDir, old ? &*old : nullptr, stop);
        if (removed != 0) {
            log(std::format("removed {} leftover folder(s)", removed));
        }
        if (stop.stop_requested()) {
            r.outcome = Outcome::Stopped;
            return;
        }
    }

    // 5. Space (plan 2.9).
    const fs::space_info space = fs::space(dataDir, ec);
    if (!ec && space.available < kMinFreeBytes) {
        const std::string free = std::format("{:.0f} MB free", static_cast<double>(space.available) / 1048576.0);
        if (!keep_stored_set(stop, r, "space: " + free)) {
            problem(r, "space", free);
        }
        return;
    }

    // 6. Searching.
    phase_.store(Phase::Searching, std::memory_order_release);
    hdf::detect::Env env;
    if (input_.autodetect) {
        env.appdata = env_path(L"APPDATA");
        env.localappdata = env_path(L"LOCALAPPDATA");
    }
    if (input_.picked) {
        env.picked = input_.picked;
    } else {
        env.remembered = input_.remembered;
    }
    env.log = [this](std::string_view line) { log("search: " + shorten(std::string(line))); };
    const auto tSearch = std::chrono::steady_clock::now();
    std::vector<hdf::detect::Candidate> found = hdf::detect::find(env, stop);
    if (stop.stop_requested()) {
        r.outcome = Outcome::Stopped;
        return;
    }
    const std::string rememberedWord = input_.remembered ? location_word(*input_.remembered) : "";
    log(std::format("search: {} {} in {:.3f} s ({})", found.size(), found.size() == 1 ? "copy" : "copies",
        seconds_since(tSearch),
        input_.picked     ? std::string("the chosen folder") :
        input_.autodetect ? (input_.remembered ? "the remembered " + rememberedWord + ", then Cemu's lists" :
                                                 std::string("Cemu's lists")) :
        input_.remembered ? "the remembered " + rememberedWord + " only" :
                            std::string("nothing: no folder chosen and the automatic search is off")));
    const hdf::detect::Candidate* use = nullptr;
    for (std::size_t i = 0; i < found.size(); ++i) {
        const hdf::detect::Candidate& c = found[i];
        if (i < 12) {
            log(std::format("search:   {} ({}): {}{}", hdf::detect::kind_name(c.kind), c.origin,
                shorten(cache::short_path(c.path)), c.problem.empty() ? std::string() : " - " + c.problem));
        }
        if (use == nullptr && c.problem.empty() && c.kind != hdf::detect::Candidate::Encrypted) {
            use = &c;
        }
    }
    if (use == nullptr) {
        const std::string why = found.empty() ? std::string("no Twilight Princess HD found") :
            std::string(hdf::detect::kind_name(found.front().kind)) + ": " + found.front().problem;
        if (keep_stored_set(stop, r, why)) {
            return;
        }
        if (found.empty()) {
            r.outcome = Outcome::NotFound;
            return;
        }
        const hdf::detect::Candidate& first = found.front();
        problem(r, "hd.candidate", why);
        r.candidate_kind = hdf::detect::kind_name(first.kind);
        r.candidate_problem = first.problem;
        return;
    }
    auto hd = hdf::detect::open(*use);
    if (!hd) {
        if (!keep_stored_set(stop, r, hd.error().code + ": " + shorten(hd.error().detail))) {
            problem(r, hd.error());
            r.reading_hd = true;  // G-L4: an io.* error here is the TP HD copy that can't be read
        }
        return;
    }
    log(std::format("using the {} {}{}", hdf::detect::kind_name(use->kind), shorten(cache::short_path(use->path)),
        use->update ? " with its update" : ""));

    // 7. Making (plan 2.4 with Correction #9: written straight into a fresh files-<set8>).
    phase_.store(Phase::Making, std::memory_order_release);
    if (input_.keep_stored) {
        log("a usable copy was found: the stored game files are replaced now");
    }
    // prepare_files, the sink and commit take the stop token (G L2/L3; wave 4 review L1), so a game exit or a
    // disable never waits for their retries on a locked file (up to about 11 s before): "stopped" is a stop.
    auto filesDir = hdf::prepare_files(dataDir, stop);  // drops a stamp that names this folder (the kept set's)
    if (!filesDir) {
        if (filesDir.error().code == "stopped" || stop.stop_requested()) {
            r.outcome = Outcome::Stopped;
        } else {
            problem(r, filesDir.error());
        }
        return;
    }
    hdf::FolderSink sink(*filesDir, stop);  // checks the token before each file and between 4 MiB pieces
    const auto tMake = std::chrono::steady_clock::now();
    auto out = hdf::generate(**hd, *input_.gc, sink, progress_, stop);
    r.make_seconds = seconds_since(tMake);
    if (!out) {
        if (out.error().code == "stopped" || stop.stop_requested()) {
            r.outcome = Outcome::Stopped;
        } else {
            problem(r, out.error());
            // G-L4: generate() reads only the TP HD copy and the GC inputs (in memory); its sink only writes
            // ("io.write"). So an "io.read" / "io.open" from it is a TP HD file that can't be read.
            r.reading_hd = true;
        }
        return;
    }
    for (const std::string& line : out->log) {
        log("make: " + line);
    }

    // 8. Storing: cache.json is the only commit point.
    phase_.store(Phase::Storing, std::memory_order_release);
    hdf::StampInfo info;
    info.mod_version = input_.mod_version;
    info.dusklight = input_.dusklight;
    info.gc = std::string(hdf::inputs::kGcGameId);
    info.hd.kind = std::string(hdf::detect::kind_name(use->kind));
    info.hd.location = cache::path_utf8(use->path);
    info.hd.title = lower_ascii(use->title_id);
    info.hd.version = use->version;
    if (auto committed = hdf::commit(dataDir, *filesDir, *out, info, stop); !committed) {
        // "stopped": the stop came during the cache.json replace retries, so no new stamp was written (the
        // made folder is a leftover for the next run). A stop during commit's cleanup is no error.
        if (committed.error().code == "stopped") {
            r.outcome = Outcome::Stopped;
        } else {
            problem(r, committed.error());
        }
        return;
    }
    double handOverSeconds = 0;
    if (!prepare_hand_over(dataDir, std::move(out->overlays), r, handOverSeconds)) {
        return;  // the stamp just written can't be opened again (r says why)
    }
    log(std::format("hand-over ready: the new stamp checked and {} overlays hashed in {:.3f} s", r.overlays.size(),
        handOverSeconds));
    r.outcome = Outcome::Done;
    r.hd_kind = info.hd.kind;
    r.hd_location = shorten(cache::short_path(use->path));
}

}  // namespace hdfiles::worker
