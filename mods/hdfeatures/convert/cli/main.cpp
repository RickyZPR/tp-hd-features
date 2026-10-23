// hdf-convert: the developer CLI of the HD Features converter (plan section 4). Never packaged in the .dusk.
//
//   hdf-convert make --hd <folder|.wua|title folder> --gc <extracted files folder> --out <dir>
//                    [--gc-mode memory] [--only cave,text,...] [--time] [--keep-going] [--stop-after <ms>]
//   hdf-convert compare --out <dir|.dusk> --oracle <hd_features.dusk or folder> [--skip-missing]
//   hdf-convert test <module|list> [--hd ...] [--gc ...] [--oracle ...] [--work <dir>] [--verbose]
//   hdf-convert check-inputs --hd ... --gc ...
//   hdf-convert detect [--appdata d] [--localappdata d] [--picked d]
//   hdf-convert seed-cache --from <oracle .dusk> --data-dir <dir> [--mod-version 2.1.0]
//   hdf-convert self-test [filter] [--verbose]
//   hdf-convert version
//   any command: --time (wall time + peak working set)
#include "cli.hpp"

#include "hdf/detect.hpp"
#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include "../tests/test.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hdf::cli {
namespace {

std::stop_source g_stop;

#ifdef _WIN32
BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop.request_stop();
        return TRUE;
    }
    return FALSE;
}
#endif

void usage() {
    std::printf(
        "hdf-convert: HD Features converter, developer tool (never shipped)\n"
        "  make --hd <folder|.wua|title folder> --gc <files folder> --out <dir> [--gc-mode memory]\n"
        "       [--only cave,text,...] [--time] [--keep-going] [--stop-after <ms>]\n"
        "  compare --out <dir|.dusk> --oracle <hd_features.dusk or folder> [--skip-missing]\n"
        "  test <module|list> [--hd <..>] [--gc <..>] [--oracle <..>] [--work <dir>] [--verbose]\n"
        "  check-inputs --hd <..> --gc <..>\n"
        "  detect [--appdata <d>] [--localappdata <d>] [--picked <d>]\n"
        "  seed-cache --from <oracle .dusk> --data-dir <dir> [--mod-version 2.1.0]\n"
        "  self-test [filter] [--verbose]\n"
        "  version\n"
        "Any command: --time prints the wall time and the peak working set at the end.\n"
        "Outputs are made from your own games: keep them in build-*/ or a scratch folder.\n");
}

std::optional<std::string> env_utf8(const char* name) {
#ifdef _WIN32
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), v.data(), n);
    v.resize(n);
    return to_utf8(std::filesystem::path(v));
#else
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

// --gc-mode memory: the 13 GC inputs copied into a MemorySource first, as the mod's DvdSource does.
Result<std::unique_ptr<Source>> open_gc(const std::filesystem::path& p, bool memory) {
    auto folder = std::make_unique<FolderSource>(p);
    if (!memory) return std::unique_ptr<Source>(std::move(folder));
    if (inputs::gc_files().empty()) return todo("inputs::gc_files (data/inputs.inc)");
    auto mem = std::make_unique<MemorySource>("GC inputs in memory (" + to_utf8(p) + ")");
    for (const inputs::InputFile& f : inputs::gc_files()) {
        auto data = folder->read(f.path, g_stop.get_token());
        if (!data) return std::unexpected(data.error());
        mem->add(f.path, std::move(*data));
    }
    return std::unique_ptr<Source>(std::move(mem));
}

std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= s.size()) {
        std::size_t j = s.find(',', i);
        if (j == std::string_view::npos) j = s.size();
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

int cmd_make(const Args& a) {
    auto hd_path = a.path("hd"), gc_path = a.path("gc"), out = a.path("out");
    if (!hd_path || !gc_path || !out) {
        std::fprintf(stderr, "hdf-convert make: --hd, --gc and --out are required\n");
        return kExitUsage;
    }
    if (auto ok = check_out_dir(*out, {*hd_path, *gc_path}); !ok) {
        report("make", ok.error());
        return kExitUsage;
    }
    const bool memory = a.get("gc-mode").value_or("folder") == "memory";
    GenOptions opt;
    if (auto only = a.get("only")) opt.only = split_csv(*only);
    opt.keep_going = a.has("keep-going");

    std::optional<std::jthread> stopper;
    if (auto ms = a.get("stop-after")) {
        const long long delay = std::stoll(*ms);
        stopper.emplace([delay](std::stop_token own) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
            while (!own.stop_requested() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!own.stop_requested()) g_stop.request_stop();
        });
    }

    auto hd = open_hd(*hd_path);
    if (!hd) return report("make", hd.error());
    auto gc = open_gc(*gc_path, memory);
    if (!gc) return report("make", gc.error());

    const std::filesystem::path files_dir = *out / u8path(folder_name(set_id()));
    FolderSink sink(files_dir);
    Progress progress;
    const auto t0 = std::chrono::steady_clock::now();
    auto made = generate(**hd, **gc, sink, progress, g_stop.get_token(), opt);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!made) return report("make", made.error());
    if (a.has("time")) {
        for (const auto& [stage, s] : made->stage_seconds) std::printf("time %-10s %8.3f s\n", stage.c_str(), s);
        std::printf("time total      %8.3f s (wall %.3f s)\n", made->seconds, wall);
    }
    StampInfo info;
    info.mod_version = "dev";
    info.dusklight = "cli";
    info.hd.kind = "cli";
    info.hd.location = to_utf8(*hd_path);
    if (auto ok = commit(*out, files_dir, *made, info); !ok) return report("make", ok.error());
    std::printf("make: %zu files in %s\n", made->files.size(), to_utf8(files_dir).c_str());
    return kExitOk;
}

int cmd_compare(const Args& a) {
    auto out = a.path("out"), oracle = a.path("oracle");
    if (!out || !oracle) {
        std::fprintf(stderr, "hdf-convert compare: --out and --oracle are required\n");
        return kExitUsage;
    }
    std::error_code ec;
    if (a.has("skip-missing") && !std::filesystem::exists(*out, ec)) {
        std::printf("compare: %s does not exist yet: skipped\n", to_utf8(*out).c_str());
        return kExitTodo;
    }
    auto rep = outputs::compare(*out, *oracle, g_stop.get_token());
    if (!rep) return report("compare", rep.error());
    static constexpr const char* kKind[] = {"identical", "PNG pixels only", "DIFF", "missing", "extra",
                                           "dropped by design"};
    for (const outputs::Verdict& v : rep->rows)
        std::printf("%-18s %s%s%s\n", kKind[v.kind], v.rel.c_str(), v.detail.empty() ? "" : "  ", v.detail.c_str());
    std::printf("%s\n", rep->summary().c_str());
    return rep->pass() ? kExitOk : kExitFail;
}

int cmd_test(const Args& a) {
    std::vector<OracleTest> tests = oracle_tests();
    std::stable_sort(tests.begin(), tests.end(), [](const OracleTest& x, const OracleTest& y) { return x.module < y.module; });
    const std::string module = a.positional.empty() ? std::string("list") : a.positional[0];
    if (module == "list") {
        for (const OracleTest& t : tests)
            std::printf("%-8s %.*s\n", std::string(t.module).c_str(), int(t.what.size()), t.what.data());
        return kExitOk;
    }
    for (const OracleTest& t : tests) {
        if (t.module != module) continue;
        OracleArgs oa;
        oa.hd = a.path("hd");
        oa.gc = a.path("gc");
        oa.oracle = a.path("oracle");
        oa.work = a.path("work");
        oa.verbose = a.has("verbose");
        oa.args = &a;
        if (oa.work) {
            std::vector<std::filesystem::path> in;
            if (oa.hd) in.push_back(*oa.hd);
            if (oa.gc) in.push_back(*oa.gc);
            if (oa.oracle) in.push_back(*oa.oracle);
            if (auto wua = a.path("wua")) in.push_back(*wua);
            if (auto ok = check_out_dir(*oa.work, in); !ok) {
                report("test", ok.error());
                return kExitUsage;
            }
        }
        return t.fn(oa);
    }
    std::fprintf(stderr, "hdf-convert test: no module '%s' (try: hdf-convert test list)\n", module.c_str());
    return kExitUsage;
}

int print_check(const char* what, const inputs::CheckReport& r) {
    std::printf("%s: %u files, %u present, %u right size, %u right hash\n", what, r.total, r.present, r.size_ok,
                r.hash_ok);
    for (const inputs::Problem& p : r.problems) std::printf("  %s: %s\n", p.path.c_str(), p.what.c_str());
    return r.ok() ? kExitOk : kExitFail;
}

int cmd_check_inputs(const Args& a) {
    auto hd_path = a.path("hd"), gc_path = a.path("gc");
    if (!hd_path && !gc_path) {
        std::fprintf(stderr, "hdf-convert check-inputs: give --hd and/or --gc\n");
        return kExitUsage;
    }
    int rc = kExitOk;
    if (gc_path) {
        const auto t0 = std::chrono::steady_clock::now();
        if (auto b = pscene_budget(*gc_path))
            std::printf("gc: Pscene budget %llu (embedded %llu): %s\n", static_cast<unsigned long long>(*b),
                        static_cast<unsigned long long>(inputs::kPsceneBudget),
                        *b == inputs::kPsceneBudget ? "same" : "DIFFERENT");
        else
            std::printf("gc: no res/Particle/Pscene*.jpc found\n");
        auto gc = open_gc(*gc_path, false);
        if (!gc) return report("check-inputs", gc.error());
        std::printf("gc: %s\n", (*gc)->describe().c_str());
        auto r = inputs::check_hashes(**gc, inputs::gc_files(), nullptr, g_stop.get_token());
        if (!r) return report("check-inputs", r.error());
        rc = std::max(rc, print_check("gc", *r));
        if (a.has("time"))
            std::printf("time: gc: %s\n",
                        time_line(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()).c_str());
    }
    if (hd_path) {
        const auto t0 = std::chrono::steady_clock::now();
        auto hd = open_hd(*hd_path);
        if (!hd) return report("check-inputs", hd.error());
        std::printf("hd: %s\n", (*hd)->describe().c_str());
        auto r = inputs::check_hashes(**hd, inputs::hd_files(), nullptr, g_stop.get_token());
        if (!r) return report("check-inputs", r.error());
        rc = std::max(rc, print_check("hd", *r));
        if (a.has("time"))
            std::printf("time: hd: %s\n",
                        time_line(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()).c_str());
    }
    return rc;
}

int cmd_detect(const Args& a) {
    detect::Env env;
    if (auto p = a.path("appdata")) env.appdata = *p;
    else if (auto v = env_utf8("APPDATA")) env.appdata = u8path(*v);
    if (auto p = a.path("localappdata")) env.localappdata = *p;
    else if (auto v = env_utf8("LOCALAPPDATA")) env.localappdata = u8path(*v);
    env.picked = a.path("picked");
    const std::vector<detect::Candidate> found = detect::find(env, g_stop.get_token());
    static constexpr const char* kKind[] = {"folder", "wua", "mlc", "encrypted"};
    for (const detect::Candidate& c : found) {
        std::printf("%-9s %-17s %s%s%s%s\n", kKind[c.kind], c.origin.c_str(), to_utf8(c.path).c_str(),
                    c.title_dir.empty() ? "" : (" [" + c.title_dir + "]").c_str(),
                    c.update ? (" + update " + to_utf8(*c.update)).c_str() : "",
                    c.problem.empty() ? "" : ("  (" + c.problem + ")").c_str());
    }
    std::printf("detect: %zu candidate(s)\n", found.size());
    return found.empty() ? kExitFail : kExitOk;
}

int cmd_seed_cache(const Args& a) {
    auto from = a.path("from"), data_dir = a.path("data-dir");
    if (!from || !data_dir) {
        std::fprintf(stderr, "hdf-convert seed-cache: --from and --data-dir are required\n");
        return kExitUsage;
    }
    if (auto ok = check_out_dir(*data_dir, {*from}); !ok) {
        report("seed-cache", ok.error());
        return kExitUsage;
    }
    auto r = seed_cache(*from, *data_dir, a.get("mod-version").value_or("2.1.0"), g_stop.get_token());
    if (!r) return report("seed-cache", r.error());
    std::printf("seed-cache: %u files, %llu bytes in %s\n", r->files, static_cast<unsigned long long>(r->bytes),
                to_utf8(r->files_dir).c_str());
    std::printf("seed-cache: cache.json written by %s (set %.*s)\n",
                r->stamp_from_cli ? "the CLI (hdf::commit is still a stub)" : "hdf::commit",
                int(outputs::embedded_set_id().size()), outputs::embedded_set_id().data());
    return kExitOk;
}

int cmd_self_test(const Args& a) {
    const std::string filter = a.positional.empty() ? std::string() : a.positional[0];
    const int failed = hdf::test::run(filter, a.has("verbose"));
    return failed == 0 ? kExitOk : kExitFail;
}

int cmd_version() {
    const LibVersions v = lib_versions();
    std::printf("hdf-convert (HD Features converter, developer build)\nzlib %s, zstd %s\n", v.zlib.c_str(),
                v.zstd.c_str());
    return kExitOk;
}

Args parse(const std::vector<std::string>& argv) {
    Args a;
    std::size_t i = 0;
    if (i < argv.size()) a.command = argv[i++];
    // switches that take no value
    static constexpr std::string_view kFlags[] = {"time", "keep-going", "verbose", "help", "skip-missing"};
    while (i < argv.size()) {
        const std::string& s = argv[i++];
        if (s.size() > 2 && s.rfind("--", 0) == 0) {
            std::string name = s.substr(2);
            if (std::find(std::begin(kFlags), std::end(kFlags), name) != std::end(kFlags) || i >= argv.size()) {
                a.flags.push_back(std::move(name));
            } else {
                a.options.emplace_back(std::move(name), argv[i++]);
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

int dispatch(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        usage();
        return kExitUsage;
    }
    const Args a = parse(argv);
    if (a.command == "-h" || a.command == "--help" || a.command == "help" || a.has("help")) {
        usage();
        return kExitOk;
    }
    const auto t0 = std::chrono::steady_clock::now();
    int rc = kExitUsage;
    if (a.command == "make") rc = cmd_make(a);
    else if (a.command == "compare") rc = cmd_compare(a);
    else if (a.command == "test") rc = cmd_test(a);
    else if (a.command == "check-inputs") rc = cmd_check_inputs(a);
    else if (a.command == "detect") rc = cmd_detect(a);
    else if (a.command == "seed-cache") rc = cmd_seed_cache(a);
    else if (a.command == "self-test" || a.command == "--self-test") rc = cmd_self_test(a);
    else if (a.command == "version" || a.command == "--version") rc = cmd_version();
    else {
        std::fprintf(stderr, "hdf-convert: unknown command '%s'\n", a.command.c_str());
        usage();
        return kExitUsage;
    }
    if (a.has("time"))
        std::printf("time: %s: %s\n", a.command.c_str(),
                    time_line(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()).c_str());
    return rc;
}

}  // namespace
}  // namespace hdf::cli

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(hdf::cli::on_ctrl, TRUE);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(hdf::cli::to_utf8(std::filesystem::path(argv[i])));
    return hdf::cli::dispatch(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return hdf::cli::dispatch(args);
}
#endif
