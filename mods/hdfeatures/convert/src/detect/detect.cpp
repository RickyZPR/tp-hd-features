// Module D: find(), open(), check_sizes() (plan 1.3, 2.5, 3.3 D; Corrections #1(b), #5, #6, #7, fix-wave1 L1).
//
// Python spec: tools/dist/hdf_inputs.py detect_hd, _cemu_dirs, _mlc_dirs, _mlc_update, _sibling_update,
// _read_meta, _meta_title, _is_cemu, open_hd, check_hd. Differences, all from the plan:
// - The order is plan 2.5: remembered location, title_list_cache.xml, mlc installs, GamePaths scan (Python had no
//   GamePaths scan and looked at the mlc first). A picked location is searched alone (plan 1.3, Correction #7).
// - Usable = all 136 inputs present with the embedded sizes (plan 2.5 "Choice"), checked here for every
//   candidate. Python sorted folders before files only; this keeps that among usable candidates, puts the
//   remembered one first, then the ones with a problem, then the encrypted ones.
// - Updates (Correction #6): a base folder or .wua without its own update gets one from the mlc, the Cemu title
//   list or another found TP HD title folder with the update id (Cemu 2 mounts an update from its whole title
//   list, INFERRED); the sibling 0005000e folder first, as Python. A .wua with _v0 and _v81 uses its own.
// - Correction #5: auto-detect touches only local fixed drives, skips OFFLINE / RECALL files and every reparse
//   point found while scanning; scans have an entry and a time budget per root; reads are small and bounded.
// - .wux/.wud found by a scan are identified by their product code (an unencrypted header), so another game's
//   .wux is not reported as an encrypted TP HD.
// Fix pass (research/dropin/wave2_D_verify.md): F1 linear entity decoding (xml.cpp); L1 NUS needs NUS files alone;
// L2 no update over a base already at that version; L3 paths from a picked Cemu's files get the auto rules; L4 a
// root or file that is itself a link is checked by its target; L5 update-only titles last; I1-I3, I6, I7.
// Wave 4 (research/dropin/wave3.md F1): "update only" needs most inputs missing or a Cemu-installed update folder.
#include "hdf/detect.hpp"

#include "hdf/inputs.hpp"
#include "hdf/source.hpp"

#include "detect_internal.hpp"

#include "../base/base_util.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <set>

namespace hdf::detect {

namespace {

namespace fs = std::filesystem;
using namespace internal;
using Clock = std::chrono::steady_clock;

// hdf_inputs.TPHD_TITLES (low halves); Cemu's mlc folders use lower-case hex.
constexpr std::string_view kTpLows[] = {"1019e500", "1019e600", "1019c800"};

constexpr u64 kMaxSettings = u64(4) << 20;     // settings.xml read cap
constexpr u64 kMaxTitleList = u64(16) << 20;   // title_list_cache.xml read cap
constexpr u64 kMaxMeta = u64(1) << 20;         // meta.xml read cap (real: 9,867 B)
constexpr u64 kMaxMetaInWua = u64(64) << 10;   // as inputs::open_hd_location
constexpr u64 kMaxPerm = u64(64) << 10;        // perm_setting.xml read cap
constexpr std::size_t kMaxConfigs = 8;         // Cemu config folders looked at
constexpr std::size_t kMaxMlcs = 16;           // mlc folders looked at
constexpr std::size_t kMaxRoots = 16;          // scanned roots (GamePaths, picked): at most 16 x time_budget
constexpr std::size_t kMaxCandidates = 256;

// user = the picked or remembered location itself: it may be remote, and unknown encrypted files are reported
// (Correction #5). origin, when set, replaces the per-source origin ("picked", "remembered").
// Fix pass L3: paths read from the files of a picked or remembered Cemu (title list, mlc_path, GamePaths) get the
// auto-detect rules (user = false) but keep the origin, and volume = the drive or share of the picked location,
// which they may still use (the pick showed it is there).
struct Ctx {
    bool user = false;
    std::string origin;
    std::string volume;
};

Ctx user_ctx(const char* origin, const fs::path& location) {
    std::error_code ec;
    fs::path abs = fs::absolute(location, ec);
    if (ec) abs = location;
    return Ctx{true, origin, volume_key(abs)};
}

// The rules for paths read from a Cemu file found under `c` (fix pass L3).
Ctx files_ctx(const Ctx& c) { return Ctx{false, c.origin, c.volume}; }

struct Found {
    Candidate c;
    bool own_update = false;  // a .wua that holds a 0005000E title of its own
    u32 base_version = 0;
    bool update_only = false;  // fix pass L5: an update title (0005000E) that lacks files: the base is missing
};

struct UpdateTitle {
    fs::path dir;
    std::string key;
    std::string title_id;
    u32 version = 0;
};

struct WuaTitle {
    std::string top, title_id;
    u32 version = 0;
};

std::string lossy(const fs::path& p) { return base::utf8_lossy(p, true); }

std::string lower_name(const fs::path& p) { return base::lower_ascii(base::utf8_lossy(p)); }

bool is_update_id(std::string_view tid) { return tid.size() == 16 && upper_ascii(tid.substr(0, 8)) == "0005000E"; }
bool is_base_id(std::string_view tid) { return tid.size() == 16 && upper_ascii(tid.substr(0, 8)) == "00050000"; }
bool is_dlc_id(std::string_view tid) { return tid.size() == 16 && upper_ascii(tid.substr(0, 8)) == "0005000C"; }
std::string low_of(std::string_view tid) { return tid.size() == 16 ? upper_ascii(tid.substr(8)) : std::string(); }

std::optional<std::string> read_text(const fs::path& p, u64 max, std::stop_token stop) {
    auto r = base::read_file(p, stop, max);
    if (!r) return std::nullopt;
    return std::string(r->begin(), r->end());
}

// _read_meta: meta/meta.xml of a title folder.
std::optional<MetaXml> read_meta(const fs::path& title_dir, std::stop_token stop) {
    auto m = path_ci(title_dir, "meta/meta.xml");
    if (!m) return std::nullopt;
    auto t = read_text(*m, kMaxMeta, stop);
    if (!t) return std::nullopt;
    return parse_meta(*t);
}

// The content folder of a title folder, or the folder itself (a content folder picked directly).
std::optional<fs::path> content_of(const fs::path& p) {
    if (auto c = child_ci(p, "content"); c && path_info(*c).dir) return c;
    if (path_info(p).dir) return p;
    return std::nullopt;
}

// The title folder (code/content/meta) for a title or content folder.
fs::path title_dir_of(const fs::path& p) {
    if (auto c = child_ci(p, "content"); c && path_info(*c).dir) return p;
    return p.parent_path();
}

// hdf_inputs.open_hd, .wua branch: the TP HD titles of an archive (top folders with content/; the title id from
// meta/meta.xml, else from the "<16 hex>_v<N>" name).
std::vector<WuaTitle> wua_titles(const WuaArchive& a, std::stop_token stop) {
    std::vector<WuaTitle> out;
    for (const std::string& top : a.tops()) {
        if (!a.has_folder(top + "/content")) continue;
        std::string tid;
        std::optional<u32> ver;
        const std::string meta_path = top + "/meta/meta.xml";
        if (a.size(meta_path).value_or(kMaxMetaInWua + 1) <= kMaxMetaInWua) {
            if (auto m = a.read(meta_path, stop)) {
                const MetaXml mx = parse_meta(std::string_view(reinterpret_cast<const char*>(m->data()), m->size()));
                tid = mx.title_id;
                ver = mx.title_version;
            }
        }
        const auto tt = top_title(top);
        if (tid.empty() && tt) tid = tt->first;
        if (!ver && tt) ver = tt->second;
        if (is_tphd_title(tid) && !is_dlc_id(tid)) out.push_back(WuaTitle{top, tid, ver.value_or(0)});  // I2
    }
    return out;
}

std::string problem_text(const Error& e) {
    if (e.code == "hd.missing") return "missing " + e.detail;
    if (e.code == "hd.version") return "different version: " + e.detail;
    return e.detail.empty() ? e.code : e.code + ": " + e.detail;
}

// check_sizes' result from its report (Correction #7: missing files apart from another version).
Result<void> size_result(const inputs::CheckReport& r) {
    if (r.ok()) return {};
    const u32 missing = r.total - r.present, differ = r.present - r.size_ok;
    const std::string total = std::to_string(r.total);
    if (differ > 0)
        return fail("hd.version", std::to_string(differ) + " of " + total + " differ" +
                                      (missing ? ", " + std::to_string(missing) + " missing" : std::string()));
    return fail("hd.missing", std::to_string(missing) + " of " + total);
}

// One candidate's check: the problem ("" = usable) and how many of the inputs are there (wave 4, F1).
struct SizeCheck {
    std::string why;
    u32 total = 0, present = 0;
};

// wave 4, F1: Cemu installs an update at <mlc>/usr/title/0005000e/<low id>. Such a folder holds only the files
// the update changed, so it is never a whole game.
bool installed_update_folder(const fs::path& title_dir) {
    const std::string name = lower_name(title_dir.filename()), parent = lower_name(title_dir.parent_path().filename());
    return parent == "0005000e" && std::find(std::begin(kTpLows), std::end(kTpLows), name) != std::end(kTpLows);
}

// ---------------------------------------------------------------------------------------------- Finder
class Finder {
public:
    Finder(const Env& env, std::stop_token stop) : env_(env), stop_(std::move(stop)) {}

    std::vector<Candidate> run() {
        if (env_.picked) {
            location(*env_.picked, user_ctx("picked", *env_.picked));
        } else {
            if (env_.remembered) location(*env_.remembered, user_ctx("remembered", *env_.remembered));
            // _cemu_dirs: %APPDATA%\Cemu and %LOCALAPPDATA%\Cemu (the user's own profile: no drive check)
            if (!env_.appdata.empty()) cemu_config(env_.appdata / "Cemu", Ctx{}, true);
            if (!env_.localappdata.empty()) cemu_config(env_.localappdata / "Cemu", Ctx{}, true);
        }
        drain();
        if (stopped()) return {};
        attach_updates();
        evaluate();
        if (stopped()) return {};
        order();
        std::vector<Candidate> out;
        out.reserve(found_.size());
        for (Found& f : found_) out.push_back(std::move(f.c));
        return out;
    }

private:
    const Env& env_;
    std::stop_token stop_;
    std::vector<Found> found_;
    std::vector<UpdateTitle> updates_;
    std::set<std::string> keys_, config_keys_, mlc_keys_, root_keys_;
    std::vector<std::pair<fs::path, Ctx>> mlc_queue_, root_queue_;

    bool stopped() const { return stop_.stop_requested(); }

    // Fix pass I3: a log callback that throws never stops the search or escapes find().
    void note(const std::string& line) const {
        if (!env_.log) return;
        try {
            env_.log(line);
        } catch (...) {
        }
    }

    static std::string origin_of(const Ctx& c, const char* auto_origin) {
        return c.origin.empty() ? std::string(auto_origin) : c.origin;
    }

    // Correction #5: auto-detect only touches local fixed drives and files that are on this disk (fix pass L3: and
    // the picked drive or share, for paths from a picked Cemu's files).
    bool drive_ok(const fs::path& abs, const Ctx& c, const fs::path& shown, const char* what) const {
        if (!c.volume.empty() && volume_key(abs) == c.volume) return true;
        const DriveClass dc = drive_class(abs);
        if (dc == DriveClass::Local) return true;
        note("skipped " + lossy(shown) + " (" + what + std::string(drive_class_name(dc)) + " drive)");
        return false;
    }

    bool allowed(const fs::path& p, const Ctx& c) const {
        if (c.user) return true;
        std::error_code ec;
        fs::path abs = fs::absolute(p, ec);
        if (ec) abs = p;
        if (!drive_ok(abs, c, p, "")) return false;
        const PathInfo pi = path_info(abs);
        if (pi.cloud) {
            note("skipped " + lossy(p) + " (not on this disk: online-only or offline)");
            return false;
        }
        // Fix pass L4: a path that is itself a link or junction is used only when its target passes the same drive
        // check. The target is read from the link, never opened. Links met while scanning are never followed.
        if (pi.reparse) {
            const LinkInfo li = link_info(abs);
            if (li.link) {
                if (!li.target) {
                    note("skipped " + lossy(p) + " (a link whose target can't be read)");
                    return false;
                }
                std::error_code tec;
                fs::path target = fs::absolute(*li.target, tec);
                if (tec) target = *li.target;
                if (!drive_ok(target, c, p, "link target: ")) return false;
            }
        }
        return true;
    }

    // Fix pass I7: a Cemu file that can't be read (too big, locked) is noted, not skipped silently.
    std::optional<std::string> read_cemu_file(const fs::path& f, u64 max) const {
        auto r = base::read_file(f, stop_, max);
        if (!r) {
            if (r.error().code != "stopped") note("skipped " + lossy(f) + ": " + problem_text(r.error()));
            return std::nullopt;
        }
        return std::string(r->begin(), r->end());
    }

    // A path from a Cemu file: UTF-8, absolute, or relative to the Cemu folder (INFERRED for relative paths).
    static std::optional<fs::path> resolve(const std::string& text, const fs::path& base) {
        auto p = path_from_utf8(text);
        if (!p) return std::nullopt;
        if (p->is_relative()) return base / *p;
        return p;
    }

    bool add(Found f) {
        if (found_.size() >= kMaxCandidates) {
            note("too many candidates: " + lossy(f.c.path) + " left out");
            return false;
        }
        found_.push_back(std::move(f));
        return true;
    }

    // ------------------------------------------------------------------------------------------ sources
    // A picked or remembered location: a folder (searched kPickedDepth levels down) or a file.
    void location(const fs::path& p, const Ctx& c) {
        const PathInfo pi = path_info(p);
        if (!pi.exists) {
            note(c.origin + " location not found: " + lossy(p));
            return;
        }
        if (pi.dir) {
            scan(p, kPickedDepth, c, "picked");
            return;
        }
        const std::string name = lower_name(p.filename()), ext = lower_name(p.extension());
        if (ext == ".wua") wua(p, c, c.origin, {}, false);
        else if (ext == ".wux" || ext == ".wud") disc(p, c, c.origin);
        else if (name == "cemu.exe") cemu_install(p.parent_path(), c);
        else if (name == "settings.xml" || name == "title_list_cache.xml") cemu_config(p.parent_path(), c, false);
        else if (ext == ".tmd" || ext == ".app" || ext == ".tik" || ext == ".h3")
            nus(p.parent_path(), c, c.origin, child_ci(p.parent_path(), "title.tmd").has_value());
        else note(c.origin + " file is not a TP HD location: " + lossy(p));
    }

    // _is_cemu / _cemu_dirs for one install: <Cemu>/portable (Cemu 2 portable), then <Cemu> (Cemu 1.x, or Cemu 2
    // before 2.0-89).
    void cemu_install(const fs::path& dir, const Ctx& c) {
        if (auto p = child_ci(dir, "portable"); p && path_info(*p).dir) cemu_config(*p, c, false);
        cemu_config(dir, c, false);
    }

    // One Cemu config folder: title_list_cache.xml now; its mlc folders and GamePaths queued (plan 2.5 order).
    void cemu_config(const fs::path& dir, const Ctx& c, bool profile) {
        if (stopped()) return;
        const std::string k = path_key(dir);
        if (config_keys_.count(k)) return;
        if (config_keys_.size() >= kMaxConfigs) {
            note("too many Cemu config folders: " + lossy(dir) + " left out");
            return;
        }
        if (!profile && !allowed(dir, c)) return;
        const PathInfo pi = path_info(dir);
        if (!pi.exists || !pi.dir) return;
        config_keys_.insert(k);
        // relative paths: against the Cemu folder (the parent of portable/) - INFERRED, none seen on this PC
        const fs::path base = lower_name(dir.filename()) == "portable" ? dir.parent_path() : dir;
        // fix pass L3: paths written in this Cemu's files are not the picked location itself
        const Ctx fc = files_ctx(c);
        if (auto f = child_ci(dir, "title_list_cache.xml"))
            if (auto t = read_cemu_file(*f, kMaxTitleList)) title_list(*t, base, fc);
        if (auto f = child_ci(dir, "settings.xml")) {
            if (auto t = read_cemu_file(*f, kMaxSettings)) {
                const CemuSettings s = parse_settings(*t);
                if (!s.mlc_path.empty()) {
                    if (auto p = resolve(s.mlc_path, base)) queue_mlc(*p, fc);
                    else note("settings.xml: mlc_path is not a usable path");
                }
                for (const std::string& g : s.game_paths) {
                    if (auto p = resolve(g, base)) queue_root(*p, fc);
                    else note("settings.xml: a GamePaths entry is not a usable path");
                }
            }
        }
        queue_mlc(dir / "mlc01", c);  // _mlc_dirs: each Cemu folder's own mlc01 (inside this folder: its rules)
        // Cemu's permanent storage keeps an mlc path here (runtime 8; INFERRED meaning)
        if (auto f = child_ci(dir, "perm_setting.xml")) {
            if (auto t = read_cemu_file(*f, kMaxPerm)) {
                const std::string m = xml_first_text(*t, "MlcPath");
                if (!m.empty())
                    if (auto p = resolve(m, base)) queue_mlc(*p, fc);
            }
        }
    }

    // detect_hd: the TP HD entries of title_list_cache.xml. Format 1 folder, 3 .wua, 2 WUD/WUX and 4 NUS noted
    // as encrypted (runtime 8, TitleInfo.h).
    void title_list(const std::string& text, const fs::path& base, const Ctx& c) {
        const std::string origin = origin_of(c, "title_list_cache");
        for (const TitleListEntry& e : parse_title_list(text)) {
            if (stopped()) return;
            if (!is_tphd_title(e.title_id) || is_dlc_id(e.title_id)) continue;  // TP HD has no DLC
            const auto p = resolve(e.path, base);
            if (!p) {
                note("title list: a TP HD path is not a usable path (skipped)");
                continue;
            }
            const std::string tid = upper_ascii(e.title_id);
            switch (e.format) {
                case 1:
                    folder(*p, c, origin, Candidate::Folder, tid, e.version);
                    break;
                case 3:
                    wua(*p, c, origin, e.sub_path, true);
                    break;
                case 2:
                case 4: {
                    if (!allowed(*p, c)) break;
                    const PathInfo pi = path_info(*p);
                    if (!pi.exists) {
                        note("title list: not found: " + lossy(*p));
                        break;
                    }
                    const std::string ext = lower_name(p->extension());
                    std::string why = e.format == 4 ? "encrypted NUS"
                                      : ext == ".wud" ? "encrypted .wud"
                                      : ext == ".wux" ? "encrypted .wux"
                                                      : "encrypted disc image";
                    encrypted(*p, std::move(why), tid, e.version, origin);
                    break;
                }
                default:
                    note("title list: format " + std::to_string(e.format) + " can't be read (" + lossy(*p) + ")");
                    break;
            }
        }
    }

    void queue_mlc(const fs::path& p, const Ctx& c) { mlc_queue_.emplace_back(p, c); }
    void queue_root(const fs::path& p, const Ctx& c) { root_queue_.emplace_back(p, c); }

    // The queues fill while they are drained (a Cemu found by a scan adds its own mlc and GamePaths).
    void drain() {
        std::size_t mi = 0, ri = 0;
        while (!stopped()) {
            if (mi < mlc_queue_.size()) {
                const auto item = mlc_queue_[mi++];
                mlc(item.first, item.second);
            } else if (ri < root_queue_.size()) {
                const auto item = root_queue_[ri++];
                scan(item.first, kGamePathsDepth, item.second, "game_paths");
            } else {
                break;
            }
        }
    }

    // detect_hd + _mlc_update: <mlc>/usr/title/00050000/<low> (base) and 0005000e/<low> (update).
    void mlc(const fs::path& m, const Ctx& c) {
        const std::string k = path_key(m);
        if (mlc_keys_.count(k)) return;
        if (mlc_keys_.size() >= kMaxMlcs) {
            note("too many mlc folders: " + lossy(m) + " left out");
            return;
        }
        mlc_keys_.insert(k);
        if (!allowed(m, c)) return;
        if (!path_info(m).dir) return;
        const auto title = path_ci(m, "usr/title");
        if (title) {
            const std::string origin = origin_of(c, "mlc");
            for (std::string_view low : kTpLows) {
                if (stopped()) return;
                if (auto b = path_ci(*title, "00050000/" + std::string(low)); b && child_ci(*b, "content"))
                    folder(*b, c, origin, Candidate::Mlc, "00050000" + upper_ascii(low), 0);
                if (auto u = path_ci(*title, "0005000e/" + std::string(low)); u && child_ci(*u, "content"))
                    add_update(*u, "0005000E" + upper_ascii(low), std::nullopt);
            }
        }
        // a Cemu folder next to this mlc (runtime 8: <mlc>/../portable and <mlc>/.. as hints)
        const fs::path parent = m.parent_path();
        for (const fs::path& d : {parent / "portable", parent})
            if (child_ci(d, "settings.xml") || child_ci(d, "title_list_cache.xml")) cemu_config(d, c, false);
    }

    void add_update(const fs::path& dir, const std::string& tid, std::optional<u32> version) {
        const auto content = content_of(dir);
        if (!content) return;
        const std::string k = path_key(*content);
        for (const UpdateTitle& u : updates_)
            if (u.key == k) return;
        if (!version)
            if (auto mx = read_meta(dir, stop_)) version = mx->title_version;
        updates_.push_back(UpdateTitle{dir, k, tid, version.value_or(0)});
    }

    // A title folder (code/content/meta) or a content folder: TP HD by meta.xml (product code "WUP-P-AZA" or a TP
    // HD title id), else by content/los.bin when there is no usable meta.xml.
    void folder(const fs::path& dir, const Ctx& c, const std::string& origin, Candidate::Kind kind,
                const std::string& known_tid, u32 known_version) {
        if (stopped() || !allowed(dir, c)) return;
        if (!path_info(dir).dir) {
            note("not a folder: " + lossy(dir));
            return;
        }
        fs::path content, title_dir = dir;
        if (auto cc = child_ci(dir, "content"); cc && path_info(*cc).dir) {
            content = *cc;
        } else if (child_ci(dir, "los.bin")) {
            content = dir;
            title_dir = dir.parent_path();
        } else {
            note("no content folder: " + lossy(dir));
            return;
        }
        const std::string key = path_key(content);
        if (keys_.count(key)) return;
        std::string tid = known_tid;
        u32 version = known_version;
        bool tp = is_tphd_title(known_tid);
        if (!tp) {
            const auto mx = read_meta(title_dir, stop_);
            const bool meta_tp = mx && (is_tphd_product(mx->product_code) || is_tphd_title(mx->title_id));
            const bool meta_other = mx && !meta_tp && (!mx->title_id.empty() || !mx->product_code.empty());
            if (meta_other) return;  // another game (BotW, WW HD, ...): Cemu's ScanGamePath doesn't go in either
            if (mx) {
                tid = mx->title_id;
                version = mx->title_version.value_or(0);
            }
            tp = meta_tp || child_ci(content, "los.bin").has_value();
            if (!tp) return;
        } else if (known_version == 0) {
            if (const auto mx = read_meta(title_dir, stop_)) version = mx->title_version.value_or(0);
        }
        if (is_dlc_id(tid)) return;  // fix pass I2: TP HD has no DLC (the title list already skipped them)
        keys_.insert(key);
        if (is_update_id(tid)) add_update(dir, tid, version);
        Found f;
        f.c.kind = kind;
        f.c.path = dir;
        f.c.title_id = tid;
        f.c.version = version;
        f.c.origin = origin;
        f.base_version = version;
        add(std::move(f));
    }

    // A .wua (Cemu's compressed archive; WuaArchive is hardened, fix-wave1 M1, so any found file may be opened).
    void wua(const fs::path& file, const Ctx& c, const std::string& origin, const std::string& sub_path, bool listed) {
        if (stopped() || !allowed(file, c)) return;
        const PathInfo pi = path_info(file);
        if (!pi.exists || pi.dir) {
            if (listed) note("title list: not found: " + lossy(file));
            return;
        }
        const std::string key = path_key(file);
        if (keys_.count(key)) return;
        auto a = WuaArchive::open(file);
        if (!a) {
            if (listed || c.user) {
                keys_.insert(key);
                Found f;
                f.c.kind = Candidate::Wua;
                f.c.path = file;
                f.c.problem = "damaged .wua: " + problem_text(a.error());
                f.c.origin = origin;
                add(std::move(f));
            } else {
                note("skipped " + lossy(file) + ": " + problem_text(a.error()));
            }
            return;
        }
        const std::vector<WuaTitle> titles = wua_titles(**a, stop_);
        if (titles.empty()) {
            if (listed || c.user) note("no TP HD in " + lossy(file));
            return;
        }
        keys_.insert(key);
        // the base: the title list's sub_path when it names a base title, else the first base, else the first
        const WuaTitle* base = nullptr;
        for (const WuaTitle& t : titles)
            if (!is_update_id(t.title_id) && base::lower_ascii(t.top) == base::lower_ascii(sub_path)) base = &t;
        for (const WuaTitle& t : titles)
            if (!base && !is_update_id(t.title_id)) base = &t;
        if (!base) base = &titles.front();
        Found f;
        f.c.kind = Candidate::Wua;
        f.c.path = file;
        f.c.title_dir = base->top;
        f.c.title_id = base->title_id;
        f.c.origin = origin;
        f.base_version = base->version;
        for (const WuaTitle& t : titles) {
            f.c.version = std::max(f.c.version, t.version);
            if (is_update_id(t.title_id)) f.own_update = true;
        }
        add(std::move(f));
    }

    void encrypted(const fs::path& p, std::string why, const std::string& tid, u32 version, const std::string& origin) {
        const std::string key = path_key(p);
        if (keys_.count(key)) return;
        keys_.insert(key);
        Found f;
        f.c.kind = Candidate::Encrypted;
        f.c.path = p;
        f.c.problem = std::move(why);
        f.c.title_id = tid;
        f.c.version = version;
        f.c.origin = origin;
        add(std::move(f));
    }

    // A .wux/.wud found by a scan or picked: TP HD by the product code at the start of the disc.
    void disc(const fs::path& file, const Ctx& c, const std::string& origin) {
        if (stopped() || !allowed(file, c)) return;
        if (keys_.count(path_key(file))) return;
        const std::string ext = lower_name(file.extension());
        const std::string why = "encrypted " + ext;
        if (const auto code = disc_product_code(file)) {
            if (!is_tphd_product(*code)) return;  // another game
            encrypted(file, why, tphd_title_of_product(*code), 0, origin);
        } else if (c.user) {
            encrypted(file, why, {}, 0, origin);  // a picked file we can't identify: it is still encrypted
        } else {
            note("skipped " + lossy(file) + " (not a readable Wii U disc header)");
        }
    }

    // NUS files (.app + title.tmd): encrypted. TP HD by the TMD's title id.
    void nus(const fs::path& dir, const Ctx& c, const std::string& origin, bool has_tmd) {
        if (stopped() || !allowed(dir, c)) return;
        std::optional<std::string> tid;
        if (has_tmd)
            if (auto t = child_ci(dir, "title.tmd")) tid = tmd_title_id(*t);
        if (tid) {
            if (is_tphd_title(*tid)) encrypted(dir, "encrypted NUS", *tid, 0, origin);
        } else if (c.user) {
            encrypted(dir, "encrypted NUS", {}, 0, origin);
        }
    }

    // ------------------------------------------------------------------------------------------ scans
    // Breadth first, at most max_depth levels below root. Each folder is classified from its own listing, as
    // Cemu's ScanGamePath does: a Cemu install, an mlc tree, a title folder, a content folder or NUS files are not
    // entered; elsewhere *.wua / *.wux / *.wud files are looked at and sub-folders queued.
    void scan(const fs::path& root, u32 max_depth, const Ctx& c, const char* auto_origin) {
        if (stopped()) return;
        const std::string rk = path_key(root);
        if (root_keys_.count(rk)) return;
        if (root_keys_.size() >= kMaxRoots) {
            note("too many folders to search: " + lossy(root) + " left out");
            return;
        }
        root_keys_.insert(rk);
        if (!allowed(root, c)) return;
        const PathInfo pi = path_info(root);
        if (!pi.exists || !pi.dir) {
            note("folder not found: " + lossy(root));
            return;
        }
        const auto t0 = Clock::now();
        Budget b;
        b.entries_left = env_.entry_budget;
        b.deadline = t0 + env_.time_budget;
        b.stop = stop_;
        const std::string origin = origin_of(c, auto_origin);
        std::deque<std::pair<fs::path, u32>> queue;
        queue.emplace_back(root, 0);
        std::vector<DirEntry> es;
        u32 folders = 0;
        const char* why = nullptr;
        while (!queue.empty()) {
            const auto [dir, depth] = std::move(queue.front());
            queue.pop_front();
            const ListStatus st = list_dir(dir, b, es);
            if (st == ListStatus::Stopped) return;
            if (st == ListStatus::Budget || st == ListStatus::Time) {
                why = st == ListStatus::Budget ? "entry budget" : "time budget";
                break;
            }
            if (st == ListStatus::Unreadable) continue;
            ++folders;
            const bool in_time = classify(dir, es, depth, max_depth, c, origin, queue, b.deadline);
            if (stopped()) return;
            if (!in_time) {
                why = "time budget";
                break;
            }
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        std::string line = "searched " + lossy(root) + ": " + std::to_string(folders) + " folders, " +
                           std::to_string(b.used) + " entries, " + std::to_string(ms) + " ms";
        if (why) line += std::string(" (stopped: ") + why + ")";
        note(line);
    }

    // false when the root's time budget ran out while files were looked at (fix pass I6: opening a .wua or a disc
    // header counts against the budget too, not only the listings).
    bool classify(const fs::path& dir, const std::vector<DirEntry>& es, u32 depth, u32 max_depth, const Ctx& c,
                  const std::string& origin, std::deque<std::pair<fs::path, u32>>& queue, Clock::time_point deadline) {
        auto entry = [&](std::string_view lower) -> const DirEntry* {
            for (const DirEntry& e : es)
                if (e.name.size() == lower.size() && base::lower_ascii(e.name) == lower) return &e;
            return nullptr;
        };
        auto is_dir = [&](std::string_view n) {
            const DirEntry* e = entry(n);
            return e && e->dir;
        };
        auto is_file = [&](std::string_view n) {
            const DirEntry* e = entry(n);
            return e && !e->dir;
        };
        // _is_cemu: Cemu.exe, or Cemu's config (settings.xml with mlc01 or title_list_cache.xml), or portable/
        if (is_file("cemu.exe") || (is_file("settings.xml") && (is_dir("mlc01") || is_file("title_list_cache.xml"))) ||
            (is_dir("portable") && child_ci(dir / "portable", "settings.xml"))) {
            cemu_install(dir, c);
            return true;
        }
        if (is_dir("usr") && path_ci(dir, "usr/title")) {  // an mlc01 tree
            queue_mlc(dir, c);
            return true;
        }
        if (is_dir("content") && (is_dir("meta") || is_dir("code"))) {  // a title folder
            folder(dir, c, origin, Candidate::Folder, {}, 0);
            return true;
        }
        if (is_file("los.bin")) {  // a TP HD content folder
            folder(dir, c, origin, Candidate::Folder, {}, 0);
            return true;
        }
        if (is_file("title.tmd")) {
            nus(dir, c, origin, true);
            return true;
        }
        // NUS files without title.tmd. Fix pass L1: only a folder that holds NUS files alone (*.app with *.h3,
        // *.tik, *.cert; no sub-folder) counts. A stray *.app no longer hides the .wua/.wux files and the
        // sub-folders next to it.
        bool any_app = false, only_nus = true;
        for (const DirEntry& e : es) {
            const std::string lower = base::lower_ascii(e.name);
            if (!e.dir && lower.ends_with(".app")) {
                any_app = true;
            } else if (e.dir || !(lower.ends_with(".h3") || lower.ends_with(".tik") || lower.ends_with(".cert"))) {
                only_nus = false;
                break;
            }
        }
        if (any_app && only_nus) {
            nus(dir, c, origin, false);
            return true;
        }
        u32 skipped = 0;
        auto note_skipped = [&] {
            if (skipped) note("skipped " + std::to_string(skipped) + " link(s) or online-only item(s) in " + lossy(dir));
        };
        for (const DirEntry& e : es) {
            if (stopped()) return true;
            // Correction #5: never follow links or junctions (fix pass I1: file links too, also in a pick);
            // auto-detect skips every reparse point and every file that is not on this disk
            if (e.link) {
                ++skipped;
                continue;
            }
            if (!c.user && (e.reparse || e.cloud)) {
                ++skipped;
                continue;
            }
            const auto name = path_from_utf8(e.name);
            if (!name) continue;
            const fs::path p = dir / *name;
            if (e.dir) {
                if (depth < max_depth) queue.emplace_back(p, depth + 1);
                continue;
            }
            const std::string lower = base::lower_ascii(e.name);
            const bool is_wua = lower.ends_with(".wua"), is_disc = lower.ends_with(".wux") || lower.ends_with(".wud");
            if (!is_wua && !is_disc) continue;
            if (Clock::now() > deadline) {
                note_skipped();
                return false;
            }
            if (is_wua) wua(p, c, origin, {}, false);
            else disc(p, c, origin);
        }
        note_skipped();
        return true;
    }

    // ------------------------------------------------------------------------------------------ updates, checks
    // _sibling_update: .../00050000/<low> -> .../0005000e/<low> when that update has content/.
    std::optional<fs::path> sibling_update(const fs::path& title_dir) const {
        const fs::path t = title_dir.lexically_normal();
        fs::path low = t.filename(), group = t.parent_path();
        if (low.empty()) {  // a trailing separator
            low = t.parent_path().filename();
            group = t.parent_path().parent_path();
        }
        if (lower_name(group.filename()) != "00050000") return std::nullopt;
        auto upd_group = child_ci(group.parent_path(), "0005000e");
        if (!upd_group) return std::nullopt;
        const auto low_text = base::utf8_exact(low);
        if (!low_text) return std::nullopt;
        auto cand = child_ci(*upd_group, *low_text);
        if (!cand || !child_ci(*cand, "content")) return std::nullopt;
        return cand;
    }

    void attach_updates() {
        for (Found& f : found_) {
            if (stopped()) return;
            if (f.c.kind == Candidate::Encrypted || f.own_update || !f.c.problem.empty()) continue;
            if (is_update_id(f.c.title_id)) continue;  // an update itself (or a merged copy that says so)
            if (f.c.kind != Candidate::Wua) {
                if (auto s = sibling_update(title_dir_of(f.c.path))) {
                    f.c.update = *s;
                    if (const auto mx = read_meta(*s, stop_))
                        f.c.version = std::max(f.c.version, mx->title_version.value_or(0));
                    continue;
                }
            }
            if (!is_base_id(f.c.title_id)) continue;  // _mlc_update needs a base title id
            const std::string low = low_of(f.c.title_id);
            const std::string own = f.c.kind == Candidate::Wua ? std::string() : path_key(content_of(f.c.path).value_or(f.c.path));
            const UpdateTitle* best = nullptr;
            for (const UpdateTitle& u : updates_)
                if (low_of(u.title_id) == low && u.key != own && (!best || u.version > best->version)) best = &u;
            // Fix pass L2: a base that is already at that version or newer (a merged copy such as a _v81 .wua)
            // gets no update from elsewhere: an older update with the same sizes would pass the size check and
            // then fail G's hash check. The sibling update above is kept as in Python (_sibling_update).
            if (best && f.base_version > 0 && best->version <= f.base_version) {
                note("update " + lossy(best->dir) + " (v" + std::to_string(best->version) + ") not layered over " +
                     lossy(f.c.path) + ": that copy is already v" + std::to_string(f.base_version));
                best = nullptr;
            }
            if (best) {
                f.c.update = best->dir;
                f.c.version = std::max(f.c.version, best->version);
            }
        }
    }

    SizeCheck check(const Candidate& c) const {
        auto src = detect::open(c);
        if (!src) return SizeCheck{problem_text(src.error())};
        const inputs::CheckReport r = inputs::check_sizes(**src, inputs::hd_files());
        auto ok = size_result(r);
        return SizeCheck{ok ? std::string() : problem_text(ok.error()), r.total, r.present};
    }

    void evaluate() {
        for (Found& f : found_) {
            if (stopped()) return;
            if (f.c.kind == Candidate::Encrypted || !f.c.problem.empty()) continue;
            SizeCheck k = check(f.c);
            if (!k.why.empty() && f.c.update) {
                Candidate alone = f.c;
                alone.update.reset();
                SizeCheck a = check(alone);
                if (a.why.empty()) {
                    note("update " + lossy(*f.c.update) + " not used: " + k.why);
                    f.c.update.reset();
                    f.c.version = f.base_version;
                    k = std::move(a);
                }
            }
            // Fix pass L5: an update title (0005000E) that lacks files is the update alone, for example the one
            // Cemu lists next to a .wux base. It is labelled and ranked last, so the panel speaks about the .wux
            // (or the missing base) and not about missing files. A merged copy that says 0005000E and has all 136
            // files ([AZAP01]) is usable and stays first.
            // Wave 4, F1: only when most inputs are missing, or when the folder is an update Cemu installed in its
            // mlc. A merged copy with a few files missing gets "missing N of 136" (the panel: use a complete copy).
            if (is_update_id(f.c.title_id) && k.present < k.total &&
                (u64(k.present) * 2 < k.total ||
                 (f.c.kind != Candidate::Wua && installed_update_folder(title_dir_of(f.c.path))))) {
                k.why = "update only: " + k.why;
                f.update_only = true;
            }
            f.c.problem = std::move(k.why);
        }
    }

    void order() {
        // usable, then readable copies with a problem, then the encrypted ones, then update-only titles (L5)
        auto rank = [](const Found& f) {
            if (f.update_only) return 3;
            return f.c.kind == Candidate::Encrypted ? 2 : (f.c.problem.empty() ? 0 : 1);
        };
        std::stable_sort(found_.begin(), found_.end(), [&](const Found& a, const Found& b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            if (ra != 0) return false;
            const int pa = a.c.origin == "remembered" ? 0 : 1, pb = b.c.origin == "remembered" ? 0 : 1;
            if (pa != pb) return pa < pb;
            const int ka = a.c.kind == Candidate::Wua ? 1 : 0, kb = b.c.kind == Candidate::Wua ? 1 : 0;
            return ka < kb;  // plan 2.5: a folder before a .wua (no zstd)
        });
    }
};

}  // namespace

std::string_view kind_name(Candidate::Kind k) {
    switch (k) {
        case Candidate::Folder: return "folder";
        case Candidate::Wua: return "wua";
        case Candidate::Mlc: return "mlc";
        case Candidate::Encrypted: return "encrypted";
    }
    return "?";
}

std::vector<Candidate> find(const Env& env, std::stop_token stop) {
    // Fix pass I3: nothing escapes, not even from a log callback that throws.
    auto log_error = [&env](const char* what) {
        if (!env.log) return;
        try {
            env.log(std::string("detect: unexpected error: ") + what);
        } catch (...) {
        }
    };
    try {
        return Finder(env, std::move(stop)).run();
    } catch (const std::exception& e) {
        log_error(e.what());
    } catch (...) {
        log_error("not a std::exception");
    }
    return {};
}

Result<std::unique_ptr<Source>> open(const Candidate& c) {
    try {
        if (c.kind == Candidate::Encrypted)
            return fail("hd.encrypted", c.problem.empty() ? std::string("encrypted") : c.problem);
        auto src = std::make_unique<LayeredSource>();
        if (c.update) {  // update first (hdf_inputs.HdSource order)
            const auto uc = content_of(*c.update);
            if (!uc) return fail("hd.not_found", "update " + lossy(*c.update));
            src->add(std::make_unique<FolderSource>(*uc));
        }
        if (c.kind == Candidate::Wua) {
            auto a = WuaArchive::open(c.path);
            if (!a) return std::unexpected(a.error());
            std::shared_ptr<const WuaArchive> archive = *a;
            std::vector<WuaTitle> titles = wua_titles(*archive, {});
            // only the candidate's own title (same low id) when it is known
            std::string low;
            if (const auto tt = top_title(c.title_dir)) low = low_of(tt->first);
            else low = low_of(c.title_id);
            std::vector<WuaTitle> use;
            for (WuaTitle& t : titles)
                if (low.empty() || low_of(t.title_id) == low) use.push_back(std::move(t));
            if (use.empty())
                return fail("hd.not_tphd", base::utf8_lossy(c.path.filename()) + " holds no Twilight Princess HD");
            // updates (0005000E) first, then the base (hdf_inputs.open_hd); otherwise the archive's sorted order
            std::stable_sort(use.begin(), use.end(), [](const WuaTitle& x, const WuaTitle& y) {
                return is_update_id(x.title_id) > is_update_id(y.title_id);
            });
            for (const WuaTitle& t : use) src->add(std::make_unique<WuaSource>(archive, t.top));
        } else {
            const auto content = content_of(c.path);
            if (!content) return fail("hd.not_found", lossy(c.path));
            src->add(std::make_unique<FolderSource>(*content));
        }
        return std::unique_ptr<Source>(std::move(src));
    } catch (const std::exception& e) {
        return fail("detect.internal", e.what());
    }
}

Result<void> check_sizes(const Source& hd) {
    // check_hd, sizes only (plan 2.5); Correction #7 separates missing files from another version.
    return size_result(inputs::check_sizes(hd, inputs::hd_files()));
}

}  // namespace hdf::detect
