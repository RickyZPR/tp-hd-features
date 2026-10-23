// hdf_convert: finding the player's TP HD files (module D "detect"; src/detect/*).
//
// Ports tools/dist/hdf_inputs.py detection: Cemu settings.xml / title_list_cache.xml, mlc base + update
// layering, meta.xml, refusing .wux/.wud/NUS. Order (plan 2.5, runtime 8): remembered/picked location,
// title_list_cache.xml (format 1 folder, 3 .wua; 2/4 noted as encrypted), mlc installs, GamePaths scan
// (depth <= 4, *.wua and title folders whose meta.xml product code starts with "WUP-P-AZA").
// Correction #5: skip remote, removable and CD drives and OFFLINE / RECALL_ON_DATA_ACCESS files in auto-detect
// (only a picked or remembered location may be remote); an entry and time budget per root; no reparse points.
// Correction #6: base folder or .wua + update in the mlc; a .wua holding _v0 and _v81 (update first).
// Correction #1(b): XML and folder names are untrusted: bounds-checked string search, entity decoding only.
//
// Wave 2 (module D) notes:
// - find() with Env::picked searches only the picked location (plan 1.3, Correction #7: a pick replaces the
//   search). Without it: Env::remembered first, then %APPDATA%\Cemu and %LOCALAPPDATA%\Cemu (title list, mlc,
//   GamePaths). A picked or remembered location may be a folder (searched kPickedDepth levels down) or a file
//   (.wua, .wux, .wud, Cemu.exe, settings.xml, title_list_cache.xml).
// - Every candidate that is not encrypted is opened and size-checked inside find(); `problem` says why one
//   can't be used. An update (mlc, Cemu title list, sibling 0005000e folder, or another found TP HD title folder
//   with the update id) is layered above a base that has none of its own; when the layered copy fails but the
//   base alone passes, the base alone is kept.
// - Never throws. Returns nothing when the stop token fires (a half-checked list must never look usable).
//
// Fix pass notes (after the wave 2 verification):
// - Order of the result: usable (remembered first, then folder or mlc before .wua), then readable copies with a
//   problem, then the encrypted ones, then update-only titles. An update title (0005000E) that lacks files gets
//   the problem "update only: missing N of 136" (or "update only: different version: ..., N missing") when most
//   of the 136 are missing, or when it is an update Cemu installed (<mlc>/usr/title/0005000e/<id>). Otherwise,
//   for example a merged copy whose meta.xml says 0005000E with a few files missing, it is "missing N of 136"
//   (wave 4, F1).
// - Links: links and junctions met while scanning are never followed, file links included. The picked or
//   remembered location may itself be a link. In auto-detect, a root or a listed file that is itself a link is
//   used only when its target (read from the link, not opened) is on a local fixed drive.
// - Paths written in a picked or remembered Cemu's files (title list, mlc_path, GamePaths) get the auto-detect
//   rules, except that they may use the picked location's own drive or share. They keep the "picked" or
//   "remembered" origin.
// - An update found elsewhere (mlc, title list, another folder) is not layered over a base whose known version is
//   already that version or newer (a merged _v81 copy). The sibling 0005000e folder is layered as in Python.
// - The per-root time budget also counts the .wua and disc headers opened while scanning.
// - Env::log may be called many times; if it throws, the line is dropped and the search goes on.
#pragma once

#include "hdf/core.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::detect {

inline constexpr u32 kPickedDepth = 3;     // picked / remembered folder: levels searched below it (plan 1.3)
inline constexpr u32 kGamePathsDepth = 4;  // Cemu GamePaths roots (plan 2.5)

struct Env {
    std::filesystem::path appdata, localappdata;          // %APPDATA%, %LOCALAPPDATA%
    std::optional<std::filesystem::path> picked;          // the folder picker (searched up to 3 levels down)
    std::optional<std::filesystem::path> remembered;      // cache.json hd.location / config files-hd_folder
    u32 entry_budget = 20000;                             // directory entries per scanned root
    std::chrono::milliseconds time_budget{3000};          // per scanned root
    // Optional: one short line per note (a skipped drive or file, a budget reached, a damaged file). Called on
    // the thread that runs find(). Lines hold paths: the mod shortens them for its log (Correction #19).
    std::function<void(std::string_view)> log;
};

struct Candidate {
    enum Kind { Folder, Wua, Mlc, Encrypted } kind = Folder;
    std::filesystem::path path;      // title folder (code/content/meta), .wua file, or the encrypted file
    std::string title_dir;           // .wua: the top folder of the base or only title ("000500001019e500_v81")
    u32 version = 0;                 // title_version when known
    std::string problem;             // why it can't be used ("encrypted .wux", "missing 12 of 136"), "" if usable
    std::optional<std::filesystem::path> update;  // an update title folder (mlc 0005000e/<id>) layered above
    std::string origin;              // "remembered", "picked", "title_list_cache", "mlc", "game_paths"
    std::string title_id;            // 16 upper-case hex digits when known (meta.xml, title list, .wua top, TMD)
};

// "folder", "wua", "mlc", "encrypted" (CLI, logs, cache.json "kind").
std::string_view kind_name(Candidate::Kind k);

// Candidates best first: usable ones (folder before .wua when both pass, plan 2.5), then the encrypted ones.
// (In full: usable, then readable copies with a problem, then encrypted, then update-only titles; see above.)
std::vector<Candidate> find(const Env& env, std::stop_token stop);
// A LayeredSource (update first) over the candidate's content folder(s) or .wua title(s).
Result<std::unique_ptr<Source>> open(const Candidate& c);
// All 136 present with the embedded sizes. Errors (Correction #7): "hd.missing" (detail "12 of 136"),
// "hd.version" (present but other sizes: another version or region).
Result<void> check_sizes(const Source& hd);

// ---------------------------------------------------------------------------------------------- parsers
// Exposed for the synthetic fixtures of self-test (no game data).
std::string xml_unescape(std::string_view s);   // &amp; &lt; &gt; &quot; &apos; &#NN; &#xNN;

struct TitleListEntry {          // title_list_cache.xml <title titleId=".." version="..">
    std::string title_id;        // 16 hex digits, as written
    u32 version = 0;
    int format = 0;              // 1 folder, 2 WUD/WUX, 3 .wua, 4 NUS, 5 WUHB (TitleInfo.h)
    std::string path, sub_path;  // unescaped
};
std::vector<TitleListEntry> parse_title_list(std::string_view xml);

struct CemuSettings {            // settings.xml
    std::string mlc_path;
    std::vector<std::string> game_paths;
};
CemuSettings parse_settings(std::string_view xml);

struct MetaXml {                 // meta/meta.xml
    std::string title_id;        // upper-case
    std::string product_code;    // e.g. "WUP-P-AZAE"
    std::optional<u32> title_version;
};
MetaXml parse_meta(std::string_view xml);

bool is_tphd_title(std::string_view title_id);  // base id low half 1019E500 / 1019E600 / 1019C800 (any case)

}  // namespace hdf::detect
