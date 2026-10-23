// Internal API of module G (src/gen/*): the driver with injectable steps and lists, and the cache stamp with an
// explicit expected list. Not a public header: only src/gen/*.cpp, the G oracle test and tests/gen_test.cpp use
// it. generate(), commit(), validate_stamp() and prepare_files() in generate.hpp are these functions with the
// embedded tables (data/inputs.inc, data/expected_outputs.inc) and the production steps.
#pragma once

#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::gen {

// Where a step puts its outputs. The driver hashes and writes each one at once (one file in memory at a time,
// except the overlays kept for the mod's hand-over), and advances the progress inside the step.
class Emitter {
public:
    virtual ~Emitter() = default;
    virtual Result<void> file(std::string rel, Bytes data) = 0;
    // common.png_bytes: the driver encodes the RGBA (io::png_encode_rgba, zlib level 9) and hashes the bytes.
    virtual Result<void> png(std::string rel, u32 w, u32 h, View rgba) = 0;
    virtual void log(std::string line) = 0;
};

// One step of build_assets.py's order. hd and gc are the CheckedSources the driver made; a step never sees the
// raw sources.
struct Step {
    GenStage stage = GenStage::Idle;
    std::string_view name;    // GenOptions::only and GenOut::stage_seconds use it
    u32 weight = 1;           // progress units (Progress::done); cave::build adds 1 per file itself
    u32 files = 0;            // outputs the step makes (spreads its weight while the driver writes them)
    bool self_progress = false;  // the step advances Progress::done itself (cave::build)
    std::function<Result<void>(const Source& hd, const Source& gc, Emitter& out, Progress& p, std::stop_token stop)>
        run;
};

// What the driver checks against. production() is the embedded tables and the real converters.
struct Config {
    std::span<const inputs::InputFile> hd_list, gc_list;
    std::vector<Step> steps;
    u32 inputs_weight = 1;    // Progress units of the input hash pass (spread by bytes)
    u32 check_weight = 1;
    // The made records (sorted by rel) against the expected list. `partial`: GenOptions::only is set, so only
    // the files made are checked (each must be identical; nothing may be extra).
    std::function<Result<void>(std::span<const OutputRecord> made, bool partial)> check;
    bool skip_dropped = true;  // files on outputs::drop_list() are not written (a converter option can make them)
};

const Config& production();

// The driver: generate() = run(hd, gc, out, p, stop, options, production()).
Result<GenOut> run(const Source& hd, const Source& gc, Sink& out, Progress& p, std::stop_token stop,
                   const GenOptions& options, const Config& config);

// The input pass (Correction #1a): every listed file read and hashed in full before any step runs. Counts all
// problems; the error names the first kind found: "<kind>.missing", then "<kind>.size", then "<kind>.read" (a file
// with the right size that can't be read; a "wua.*" read error keeps its code), then "<kind>.hash" (detail: "<n> of
// <total> files missing, <n> wrong size, <n> differ, <n> unreadable; first: <path> (<what>)", plus "; read error:
// <the source's code>: <its detail>" when a file can't be read). Also "stopped".
// progress: p.done advances from `base` by up to `weight` units, spread by bytes.
struct InputTotals {
    u64 files = 0, bytes = 0;
};
Result<InputTotals> check_inputs(const Source& src, std::span<const inputs::InputFile> list, std::string_view kind,
                                 Progress& p, u32 base, u32 weight, std::stop_token stop);

// The production check: outputs::check, 176 identical; partial: every made file identical, none extra.
Result<void> check_against(std::span<const outputs::ExpectedFile> expected, std::span<const OutputRecord> made,
                           bool partial);

// ---------------------------------------------------------------------------------------------- the stamp
// cache.json text (plan 2.4 format, Correction #10 sizes + SHA-256), the files in `files` order.
std::string stamp_json(const Stamp& s);
// Parses cache.json text. Error "cache.bad" (not JSON, another format, a field of the wrong type). Bounded:
// at most 4 MiB, depth 16, 100,000 values.
Result<Stamp> parse_stamp(std::string_view text);

// validate_stamp / commit / prepare_files with an explicit expected list and set id (tests use small lists).
Result<void> validate_stamp_with(const std::filesystem::path& data_dir, const Stamp& stamp,
                                 std::span<const outputs::ExpectedFile> expected, std::string_view set);
// (wave 4) stop: as commit() / prepare_files() in generate.hpp.
Result<void> commit_with(const std::filesystem::path& data_dir, const std::filesystem::path& files_dir,
                         const GenOut& out, const StampInfo& info, std::span<const outputs::ExpectedFile> expected,
                         std::string_view set, std::stop_token stop = {});
Result<std::filesystem::path> prepare_files_with(const std::filesystem::path& data_dir, std::string_view set,
                                                 std::stop_token stop = {});

// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), retried on access-denied, sharing and lock
// errors for up to `budget_ms` (Correction #9). Error "io.write"; "stopped" when `stop` fires while it waits to
// retry (wave 4, G L3).
Result<void> replace_file(const std::filesystem::path& from, const std::filesystem::path& to, int budget_ms = 5000,
                          std::stop_token stop = {});

// UTC "YYYY-MM-DDTHH:MM:SSZ".
std::string utc_now();

// src/gen/png.cpp: the bytes of io::png_encode_rgba (common.png_bytes), with a stop check every 64 KiB of deflate
// input. Errors "out.png", "stopped".
Result<Bytes> png_encode(u32 w, u32 h, View rgba, std::stop_token stop = {});

}  // namespace hdf::gen
