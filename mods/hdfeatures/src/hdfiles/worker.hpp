#pragma once

// HD Features: the setup worker thread that finds the player's TP HD and makes the game files (module R2; plan
// 2.3 "Worker" with Corrections #1c, #4, #5, #9, #15, #19).
//
// One heap object per run, owned by setup.cpp, started from mod_update (never from mod_initialize, Correction
// #4) and always stopped and joined by its owner (mod_shutdown at the latest; never a static destructor). The
// thread uses only the standard library, Win32 file calls and the converter library: no SDK and no game calls.
// It runs below normal priority (Correction #19) and the whole body is inside try / catch (...).
//
// One run, in order:
//   1. <data_dir>\lock opened without sharing (Correction #15; deleted when closed). Held by another Dusklight:
//      OtherInstance, nothing touched.
//   2. <data_dir>\setup.running written (Correction #1c); removed when the run ends, whatever the result. Found
//      at the next start, it means the run never ended (a crash or a killed process).
//   3. A valid stamp made meanwhile by another Dusklight: its overlays are read (each hashed) and handed over.
//      Skipped for "Make the game files again" (Input::keep_stored).
//   4. Leftover files-* / tmp-* folders the old stamp doesn't name are removed (hdf::cleanup, never fatal).
//   5. 200 MB free space (plan 2.9).
//   6. Searching: hdf::detect::find (a picked folder alone, or the remembered one then Cemu's lists).
//   7. Making: hdf::prepare_files -> FolderSink -> hdf::generate -> hdf::commit (cache.json is the commit point).
//   8. Done: the 56 overlay buffers are kept for the hand-over (hdfiles::adopt on the game thread). The worker
//      also opens and checks the new stamp and hashes the buffers (wave3 F4), so the hand-over frame only
//      compares hashes and calls add_buffer.
// keep_stored (wave3_R2_verify F6): the stored set stays valid until a usable TP HD is found, because
// prepare_files() is what drops its stamp. When steps 5-6 find nothing usable, the stored set's overlays are read
// and handed over instead (Outcome::Kept), so the features keep working.
// The stop token is checked by detect and generate at least every 50 ms of work.

#include "hdfiles/cache.hpp"
#include "hdfiles/files.hpp"

#include "hdf/core.hpp"
#include "hdf/source.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hdfiles::worker {

inline constexpr std::uint64_t kMinFreeBytes = 200ull * 1024 * 1024;  // plan 1.2 "200 MB free"
inline constexpr const char* kMarkerFile = "setup.running";
inline constexpr const char* kLockFile = "lock";

enum class Phase : std::uint8_t {
    Starting,   // lock, marker, leftovers, space
    Searching,  // detect::find
    Making,     // generate
    Storing,    // commit
    Finished,   // result() is ready
};

enum class Outcome : std::uint8_t {
    None,
    Done,           // made and committed: overlays hold the 56 buffers
    FromDisk,       // another Dusklight made them meanwhile: overlays read from the stored files
    Kept,           // keep_stored and no usable TP HD: the stored set's overlays (detail says why)
    Stopped,        // the owner asked it to stop
    OtherInstance,  // the lock is held by another Dusklight
    NotFound,       // no TP HD at all (nothing to report about a copy)
    Problem,        // see code / detail / candidate
};

struct Input {
    std::filesystem::path data_dir;
    std::optional<std::filesystem::path> picked;      // chosen in this session: searched alone
    std::optional<std::filesystem::path> remembered;  // files-hd_folder or the old stamp's hd.location
    bool autodetect = true;                           // Cemu's lists, mlc and GamePaths (off: test option)
    bool keep_stored = false;                         // "Make the game files again": see Outcome::Kept
    std::shared_ptr<const hdf::MemorySource> gc;      // the 13 GC inputs (dvd_source.cpp)
    std::string mod_version;                          // "2.1.0"
    std::string dusklight;                            // HostService version, "2.0.3.0"
};

struct Result {
    Outcome outcome = Outcome::None;
    std::string code;        // the error code (hd.missing, io.write, space, gen.exception, ...)
    std::string detail;      // one line for the log
    // The candidate the line is about (Problem from detection): its kind name and problem text.
    std::string candidate_kind;
    std::string candidate_problem;
    // Problem while opening or reading the chosen TP HD copy (detect::open, generate): an "io.read" / "io.open"
    // there means a TP HD file can't be read (wave3 G-L4), not a fault of the mod.
    bool reading_hd = false;
    bool picked = false;     // the run searched a picked folder only
    // Done / FromDisk / Kept: the 56 overlay buffers with their SHA-256 (hashed here), and the stamp they belong
    // to, opened and checked here (cache::Cache::open). wave3 F4: none of that work is left for the game thread.
    std::vector<HandOver> overlays;
    std::optional<cache::Cache> stored;
    std::string hd_kind, hd_location;                           // Done: the copy used (short path for the log)
    double seconds = 0;      // the whole run
    double make_seconds = 0; // generate() alone
};

class Worker {
public:
    explicit Worker(Input input);
    ~Worker();  // stops and joins if the owner did not (never relied on)
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();
    void request_stop();
    // Joins the thread; returns the seconds the join took.
    double join();
    bool started() const { return thread_.joinable(); }

    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    bool finished() const { return phase() == Phase::Finished; }
    const hdf::Progress& progress() const { return progress_; }
    const Input& input() const { return input_; }
    // Log lines queued by the thread (game thread drains them into the mod's log).
    std::vector<std::string> drain_log();
    // After finished(): the result, moved out once.
    Result take_result();

private:
    void run(std::stop_token stop);
    void run_steps(std::stop_token stop, Result& r);
    // keep_stored: hands the stored set over instead (r = Kept, or Stopped). False: nothing to keep, r unchanged.
    bool keep_stored_set(const std::stop_token& stop, Result& r, const std::string& why);
    void log(std::string line);

    Input input_;
    hdf::Progress progress_;
    std::atomic<Phase> phase_{Phase::Starting};
    std::mutex lock_;
    std::vector<std::string> lines_;
    Result result_;
    std::jthread thread_;
};

// Paths in log lines: the user's profile folder becomes %USERPROFILE% (Correction #19: players share logs).
std::string shorten(std::string text);

// What a chosen or remembered location is, for log lines, from its name alone (no file system call): ".wua",
// "folder" (no extension) or "location" (wave3 F8: a remembered .wua was called "folder").
const char* location_word(const std::filesystem::path& location);

}  // namespace hdfiles::worker
