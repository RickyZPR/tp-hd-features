// hdf-convert (developer only, never packaged): shared CLI helpers and the oracle-test registry.
#pragma once

#include "hdf/core.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::cli {

// Exit codes.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFail = 1;     // a check failed
inline constexpr int kExitUsage = 2;    // bad arguments, refused --out
inline constexpr int kExitTodo = 3;     // not ported yet (a wave-1 stub answered Error{"todo"})

// Parsed command line: the sub-command, "--name value" options, "--flag" switches, positionals.
struct Args {
    std::string command;
    std::vector<std::pair<std::string, std::string>> options;  // UTF-8
    std::vector<std::string> flags;
    std::vector<std::string> positional;

    std::optional<std::string> get(std::string_view name) const;
    std::optional<std::filesystem::path> path(std::string_view name) const;
    bool has(std::string_view flag) const;
};

std::filesystem::path u8path(std::string_view utf8);
std::string to_utf8(const std::filesystem::path& p);

// Prints "hdf-convert <cmd>: <code>: <detail>" and returns the exit code for it (kExitTodo for "todo").
int report(std::string_view command, const Error& e);

// Correction #18: outputs are Nintendo-derived. Refuses mods/, tools/, third_party/, publish/, dist/, art/,
// repo_docs/, cache/oracle/, testenv/ and anything else in the project except build-*/; also the input folders,
// %APPDATA%\TwilitRealm, and the HD/GC dumps. Allows build-*/ and folders outside the project (scratch).
Result<void> check_out_dir(const std::filesystem::path& out, const std::vector<std::filesystem::path>& inputs);

// ---------------------------------------------------------------------------------------------- C2 helpers
// cli/base_cli.cpp. --time prints "time: <what>: wall 1.234 s, peak working set 12.3 MB" (plan 4 items 4-5).
u64 peak_working_set();                        // bytes (PeakWorkingSetSize), 0 if unknown
std::string time_line(double seconds);         // "wall 1.234 s, peak working set 12.3 MB"

// The particle budget recomputed from an extracted GC disc: largest res/Particle/Pscene*.jpc - 4096.
std::optional<u64> pscene_budget(const std::filesystem::path& gc);

// --hd: a .wua, a title or content folder, or a folder holding one title (inputs::open_hd_location).
Result<std::unique_ptr<Source>> open_hd(const std::filesystem::path& p);

// SHA-256 of a whole file: {size, hex}.
Result<std::pair<u64, std::string>> file_sha256(const std::filesystem::path& p, std::stop_token stop = {});

// seed-cache: the 176 kept files of the pinned oracle into data_dir/files-<set8>/ plus cache.json, as the mod
// would leave them (R1/R2 sandbox work before the converters exist). cache.json comes from hdf::commit once
// module G has it; until then from the CLI's own writer (plan 2.4 format, Corrections #9 and #10).
struct SeedResult {
    std::filesystem::path files_dir;
    u32 files = 0;
    u64 bytes = 0;
    bool stamp_from_cli = false;  // hdf::commit still answered "todo"
};
Result<SeedResult> seed_cache(const std::filesystem::path& oracle, const std::filesystem::path& data_dir,
                              std::string_view mod_version, std::stop_token stop);

// ---------------------------------------------------------------------------------------------- oracle tests
// `hdf-convert test <module>`: each module owns cli/oracle/<module>.cpp and registers itself with
// HDF_ORACLE_TEST. Golden lists come from tools/dev/golden.py (kept in the build folder, never committed).
struct OracleArgs {
    std::optional<std::filesystem::path> hd, gc, oracle, work;  // --hd --gc --oracle --work
    bool verbose = false;
    const Args* args = nullptr;  // everything else
};
using OracleFn = int (*)(const OracleArgs&);

struct OracleTest {
    std::string_view module;
    std::string_view what;
    OracleFn fn;
};
std::vector<OracleTest>& oracle_tests();

struct OracleReg {
    OracleReg(std::string_view module, std::string_view what, OracleFn fn);
};

}  // namespace hdf::cli

#define HDF_CLI_CAT2(a, b) a##b
#define HDF_CLI_CAT(a, b) HDF_CLI_CAT2(a, b)
// HDF_ORACLE_TEST("io", "short description", &function)
#define HDF_ORACLE_TEST(module, what, fn) \
    static ::hdf::cli::OracleReg HDF_CLI_CAT(hdf_oracle_reg_, __LINE__)(module, what, fn)
