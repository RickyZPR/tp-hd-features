// hdf_convert: the driver, the output check and the cache stamp (module G "gen"; src/gen/*).
//
// Follows the order of tools/build_assets.py (no port of its CLI, install or validation). Cache layout and
// write protocol: plan 2.4 with Correction #9 (no folder rename: files are written straight into a fresh
// files-<set8>, cache.json is the only commit point, MoveFileExW replace + write-through with retries) and
// Correction #10 (cache.json records size + SHA-256; the mod hashes each file when it reads it).
//
// Wave 2 (module G) additions, all additive: GenOut::log, prepare_files(), kStampTmpFile, and comments on the
// order, the errors and the progress of generate() and on the checks of validate_stamp(), commit() and cleanup().
// The protocol for a caller (the mod's worker, the CLI): prepare_files(data_dir) -> FolderSink on the returned
// folder -> generate() -> commit(); at start, read_stamp() + validate_stamp(), then cleanup(data_dir, &stamp).
// Wave 4: each step also takes the worker's stop token (FolderSink through its constructor).
#pragma once

#include "hdf/core.hpp"
#include "hdf/outputs.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdf {

// Progress::stage values.
enum class GenStage : u32 {
    Idle = 0,
    Inputs,     // hashing the HD + GC inputs (Correction #1)
    Cave,       // cave::build (56 overlays)
    Stamps,     // ui::build_stamps (PNGs + icons)
    Text,       // text::build
    Amiibo,     // ui::build_amiibo
    Stampbook,  // ui::build_stampbook
    Confirm,    // ui::build_confirm
    Tables,     // stage::hd_tags_bin, stage::hd_chests_bin
    Check,      // every output against outputs::expected()
    Done,
};
std::string_view stage_name(GenStage s);

struct GenOptions {
    std::vector<std::string> only;    // CLI --only cave,text,...: empty = everything
    bool keep_going = false;          // CLI --keep-going: continue after a failed step
    bool keep_overlays = true;        // keep the 56 overlay buffers in GenOut::overlays (the mod's hand-over)
};

struct GenOut {
    std::vector<OutputRecord> files;                       // every written file, sorted by rel
    std::vector<std::pair<std::string, Bytes>> overlays;   // rel "overlay/res/..." -> bytes
    double seconds = 0;
    std::vector<std::pair<std::string, double>> stage_seconds;  // --time
    // (G addition) Log lines for the mod's log, in order: one per stage, plus cavebuild's own lines (no paths,
    // no game text).
    std::vector<std::string> log;
};

// hd, gc: raw sources; generate() wraps them in inputs::CheckedSource itself, so no converter ever sees a byte
// before its file's full SHA-256 matched (Correction #1a; G's unit test must show that an unlisted or
// wrong-hash file never reaches a converter, fix-wave1 L4). Writes the 176 kept files to out and checks them
// (Error "out.check" with the first differing file when a hash differs). Runs under the default floating-point
// environment: G sets FE_TONEAREST at entry (fix-wave1 L9; the worker is a new thread, which starts that way).
//
// (G) Order, as tools/build_assets.py: the input pass (GenStage::Inputs: the 13 GC and 136 HD files read and
// hashed in full first, so a wrong copy fails before any conversion), then cave (cavebuild.build: the 56
// overlays, Obj_Stamp included), tables (hd_tags.bin from cavebuild, hd_chests.bin from stampbuild), stamps
// (stampbuild PNGs + gen_stamp_table.build_icons), text and amiibo (hdtext.build), stampbook, confirm
// (cosconfirm), then the check. The outputs are independent, so this order only decides the write order.
// The FE rounding mode of the calling thread is restored on return.
// Errors: "stopped"; the input pass: "hd.missing" / "hd.size" / "hd.read" / "hd.hash" (and "gc.*"), detail "<n> of
// <total> files missing, <n> wrong size, <n> differ, <n> unreadable; first: <path> (<what>)" plus "; read error:
// <code>: <detail>" when a file can't be read. "hd.read" (wave 4, G-L4): a listed file with the right size that
// can't be read (a disk error, an online-only file); a damaged .wua block keeps its "wua.*" code. The mod's panel
// maps "hd.read" to "Some Twilight Princess HD files can't be read." A step's own error code with "<step>: " before the
// detail; "out.check"; "gen.only" (an unknown --only name); "gen.exception" (a step threw). With keep_going the
// steps after a failed one still run, and the first failure is returned at the end (the files made stay in out).
// Progress: p.stage = GenStage, p.total = the sum of the stage weights (src/gen/generate.cpp), p.done rises to
// p.total; cave::build adds 1 per file during the cave stage.
Result<GenOut> generate(const Source& hd, const Source& gc, Sink& out, Progress& p, std::stop_token stop,
                        const GenOptions& options = {});

std::string_view set_id();                            // compute_set_id(outputs::expected()), computed once
std::string folder_name(std::string_view set_id);     // "files-" + the first 8 hex digits

// ---------------------------------------------------------------------------------------------- cache.json
inline constexpr int kStampFormat = 1;
inline constexpr int kConverterVersion = 1;           // changes only when the stamp format changes
inline constexpr std::string_view kModId = "local.ricky.hdfeatures";
inline constexpr std::string_view kStampFile = "cache.json";
// The Dusklight build this mod is made for, as HostService reports it (S1, VERIFIED: svc_host->version and
// build_id, 20 bytes as hex). G writes kDusklightVersion into cache.json "dusklight"; R2 compares both with
// HostService at start (Correction #8). One constant for both (fix-wave1 L6).
inline constexpr std::string_view kDusklightVersion = "2.0.3.0";
inline constexpr std::string_view kDusklightBuildId = "3e441e7c7f09432d9cfb111c22596cb601000000";

struct StampHd {
    std::string kind;          // "folder", "wua", "mlc"
    std::string location;      // the HD location used (tried first after a mod update, Correction #7)
    std::string title;         // e.g. "000500001019e500"
    u32 version = 0;
};

struct StampInfo {             // what the caller knows; commit() adds set, folder, files
    std::string mod_version;   // "2.1.0"
    std::string dusklight;     // kDusklightVersion, "2.0.3.0"
    std::string gc = "GZ2E01";
    std::string made;          // UTC, "2026-10-22T12:00:00Z"
    StampHd hd;
};

struct Stamp {
    int format = 0;
    std::string mod, mod_version;
    int converter = 0;
    std::string set, folder, made;
    double seconds = 0;
    std::string dusklight, gc;
    StampHd hd;
    std::vector<std::pair<std::string, std::pair<u64, std::string>>> files;  // rel -> (size, sha256)
};

// cache.json in data_dir. Error "cache.missing" / "cache.bad" (unreadable or another format).
Result<Stamp> read_stamp(const std::filesystem::path& data_dir);
// Valid at start (plan 2.4): format, set == set_id(), folder exists, every file exists with its size.
// (G) Also: mod and converter as above, folder == folder_name(set_id()), and the files listed = the embedded
// expected list (paths, sizes, SHA-256), so a cut or edited stamp is never valid. Errors: "cache.bad" (format,
// mod, converter, folder name or file list), "cache.set" (another set: a mod update changed the outputs),
// "cache.incomplete" (the folder or a file is missing, or a file has another size).
Result<void> validate_stamp(const std::filesystem::path& data_dir, const Stamp& stamp);
// Commit point (Correction #9): files_dir = data_dir / folder_name(set_id()) already holds every file of out.
// Writes cache.json.tmp, then replaces cache.json (MoveFileExW REPLACE_EXISTING | WRITE_THROUGH, retries for
// about 5 s on access-denied or sharing errors). Every output and the .tmp are on disk (FlushFileBuffers)
// before the move (fix-wave1 L5): FolderSink::write flushes each file it writes; write the .tmp with it too.
// (G) Before the stamp: out.files must be the 176 expected files, identical ("out.check"), and each must exist
// in files_dir with its size ("out.missing"); files_dir must be data_dir / folder_name(set_id()) ("cache.folder").
// StampInfo::made empty = now (UTC); StampInfo::dusklight empty = kDusklightVersion. After the stamp:
// cleanup(data_dir, &stamp) (plan 2.4 step 5; never fatal).
// (wave 4) The text is read back (parse_stamp + validate) before it is written, so the next start never refuses
// it; a location the reader can't take (a lone surrogate) is dropped from the stamp (wave2_G_verify L1).
// stop (wave 4, G L3): only the waits end early. A stop during the replace retries gives "stopped" (no new
// stamp); a stop during the cleanup after the stamp just leaves the rest for the next start.
Result<void> commit(const std::filesystem::path& data_dir, const std::filesystem::path& files_dir,
                    const GenOut& out, const StampInfo& info, std::stop_token stop = {});
// Delete the files-* and tmp-* folders the stamp doesn't name (with retries; never fatal). Returns the count.
// (G) Also removes a stale cache.json.tmp. Only direct children of data_dir; a link is removed, never followed.
// stop (wave 4, G L3): ends the retries and leaves the other folders for the next start.
u32 cleanup(const std::filesystem::path& data_dir, const Stamp* keep, std::stop_token stop = {});

// (G addition) Before generate(): the fresh files-<set8> of Correction #9. A cache.json that names that folder
// (or that can't be read) is deleted first, so a stamp never names a folder that is being rewritten; then a
// leftover files-<set8> is removed (retries for about 5 s) and made again, empty. Returns
// data_dir / folder_name(set_id()) for a FolderSink. Error "io.write". A stamp naming another folder (an older
// set) is kept: commit() replaces it.
// stop (wave 4, G L3): "stopped" at the start or when it ends the retries of a locked file early.
// The mod's worker passes its stop token to prepare_files, FolderSink, commit and cleanup (src/hdfiles/worker.cpp),
// so a game exit never waits for these retries (up to about 11 s while antivirus or the indexer holds a file).
// "io.write" details from an OS call end in "(os error N)" (base::os_error_text), for the mod's panel words.
Result<std::filesystem::path> prepare_files(const std::filesystem::path& data_dir, std::stop_token stop = {});
inline constexpr std::string_view kStampTmpFile = "cache.json.tmp";

}  // namespace hdf
