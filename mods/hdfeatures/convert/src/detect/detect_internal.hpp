// Internal helpers of module D (src/detect). Not a public header: only src/detect/*.cpp and the D unit tests
// (tests/detect_test.cpp) include it.
#pragma once

#include "hdf/core.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::detect::internal {

// ---------------------------------------------------------------------------------------------- xml.cpp
// A tiny, bounded XML tokenizer for Cemu's settings.xml / title_list_cache.xml / perm_setting.xml and Wii U
// meta.xml (Correction #1(b): the text is untrusted). Comments, <?..?>, <!DOCTYPE ..> are skipped; CDATA is text.
// It never reads past the end and stops at the first malformed tag.
struct XmlToken {
    enum Type { Start, End, Text } type = Text;
    std::string_view name;   // Start / End
    std::string_view attrs;  // Start: the raw attribute text
    bool self_closing = false;
    std::string text;        // Text: entity-decoded (CDATA as is)
};

class XmlScanner {
public:
    explicit XmlScanner(std::string_view xml);  // a UTF-8 BOM is skipped
    bool next(XmlToken& t);                     // false at the end or at the first malformed tag

private:
    std::string_view s_;
    std::size_t i_ = 0;
};

// The decoded value of attribute `name` in a start tag's raw attribute text.
std::optional<std::string> xml_attr(std::string_view attrs, std::string_view name);
// The trimmed, decoded text of the first element `name` anywhere in the document ("" if none).
std::string xml_first_text(std::string_view xml, std::string_view name);
std::string trim(std::string_view s);  // ASCII white space

// UTF-8 -> path. Nothing for text that is not strict UTF-8 (overlong forms, surrogates, > U+10FFFF), that holds
// a control character (< 0x20), or that is empty or longer than 32,767 UTF-16 units. Never throws.
std::optional<std::filesystem::path> path_from_utf8(std::string_view utf8);

bool is_hex16(std::string_view s);
std::string upper_ascii(std::string_view s);
// "<16 hex>_v<digits>" (a .wua top folder) -> {title id upper-case, version}.
std::optional<std::pair<std::string, u32>> top_title(std::string_view top);
// A Wii U product code "WUP-P-AZAE" -> the TP HD base title id ("000500001019E500"), "" for another game.
std::string tphd_title_of_product(std::string_view product);
bool is_tphd_product(std::string_view product);  // "WUP-P-AZA" prefix, any case

// ---------------------------------------------------------------------------------------------- fsutil.cpp
// One directory entry of a scan. Names that are not valid UTF-16 are left out (fix-wave1 L1, base::utf8_exact).
struct DirEntry {
    std::string name;  // UTF-8
    bool dir = false;
    bool reparse = false;  // FILE_ATTRIBUTE_REPARSE_POINT
    bool link = false;     // a name-surrogate reparse point: symbolic link, junction, mount point
    bool cloud = false;    // OFFLINE, RECALL_ON_OPEN or RECALL_ON_DATA_ACCESS (not on this disk)
    u64 size = 0;
};

// Correction #5: per scanned root, an entry and a time budget, and the stop token.
struct Budget {
    u32 entries_left = 0;
    u32 used = 0;
    std::chrono::steady_clock::time_point deadline;
    std::stop_token stop;
};

enum class ListStatus { Ok, Unreadable, Budget, Time, Stopped };
// The entries of one folder, sorted by name (byte order). Every entry seen counts against the budget.
ListStatus list_dir(const std::filesystem::path& dir, Budget& b, std::vector<DirEntry>& out);

struct PathInfo {
    bool exists = false, dir = false, reparse = false, cloud = false;
};
PathInfo path_info(const std::filesystem::path& p);  // no recall: attributes only

enum class DriveClass { Local, Remote, Removable, Optical, Unknown };
// From the path text alone for UNC and device paths (no I/O, so an unreachable server can't block), else
// GetDriveTypeW of the drive root. Local = fixed or RAM disk.
DriveClass drive_class(const std::filesystem::path& p);
std::string_view drive_class_name(DriveClass c);
// The drive or share a path is on, from the text alone: "c:" or "\\server\share", lower-case. "" for a relative,
// device or volume-GUID path (fix pass L3: paths in a picked Cemu's files may use the picked drive or share).
std::string volume_key(const std::filesystem::path& p);

// A name-surrogate reparse point (symbolic link, junction, mount point), read without following it (fix pass L4).
// link = false for a plain item or another kind of reparse point (cloud, dedup, compressed). target: the target as
// a Win32 path ("C:\x", "\\server\share\x", "\\?\Volume{..}\x"), relative ones joined to the link's folder;
// nothing when it can't be read.
struct LinkInfo {
    bool link = false;
    std::optional<std::filesystem::path> target;
};
LinkInfo link_info(const std::filesystem::path& p);

// The product code ("WUP-P-AZAE") at the start of a Wii U disc image: plain .wud, or a .wux (header "WUX0",
// u32 magic 0x1099D02E, u32 sector size, u64 at 0x10 the disc size; a u32 sector index table at 0x20; sector
// data from the next sector-aligned offset). Nothing when the file is not one, is damaged or too short.
std::optional<std::string> disc_product_code(const std::filesystem::path& file);
// The title id (16 upper-case hex) of a NUS title.tmd (RSA-2048 or RSA-4096 signed), else nothing.
std::optional<std::string> tmd_title_id(const std::filesystem::path& file);

// A child found without regard to case (common.exists_ci for one level): the direct path when it exists, else
// a bounded listing. Nothing when absent.
std::optional<std::filesystem::path> child_ci(const std::filesystem::path& dir, std::string_view name);
// dir/a/b/c, each part found without regard to case.
std::optional<std::filesystem::path> path_ci(const std::filesystem::path& dir, std::string_view rel);

// A comparable key for de-duplication: absolute, normal, '/'-separated, ASCII lower-case, no trailing '/'.
std::string path_key(const std::filesystem::path& p);

}  // namespace hdf::detect::internal
