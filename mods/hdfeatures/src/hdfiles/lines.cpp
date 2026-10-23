// HD Features: the line for a worker problem (see lines.hpp). Module R2.

#include "hdfiles/lines.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace hdfiles::setup {
namespace {

// "N of M" from a detail such as "12 of 136" or "3 of 136 files missing, ...": the first such pair.
std::string count_of(std::string_view detail) {
    for (std::size_t i = 0; i < detail.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(detail[i])) || (i > 0 && std::isdigit(static_cast<unsigned char>(detail[i - 1])))) {
            continue;
        }
        std::size_t j = i;
        while (j < detail.size() && std::isdigit(static_cast<unsigned char>(detail[j]))) {
            ++j;
        }
        if (detail.substr(j).starts_with(" of ")) {
            std::size_t k = j + 4;
            while (k < detail.size() && std::isdigit(static_cast<unsigned char>(detail[k]))) {
                ++k;
            }
            if (k > j + 4) {
                return std::string(detail.substr(i, k - i));
            }
        }
    }
    return {};
}

std::string missing_line(std::string_view detail) {
    const std::string count = count_of(detail);
    return "Some Twilight Princess HD files are missing" + (count.empty() ? std::string() : " (" + count + ")") +
           ": use a complete copy.";
}

// wave 4 review L4: the line names the button that searches again, and every encrypted kind detection reports
// (detect.cpp: "encrypted NUS", "encrypted .wud", "encrypted .wux", "encrypted disc image").
std::string encrypted_line(std::string_view problem) {
    const char* what = problem.find("NUS") != std::string_view::npos    ? "is in NUS format" :
                       problem.find(".wud") != std::string_view::npos   ? "is a .wud" :
                       problem.find(".wux") != std::string_view::npos   ? "is a .wux" :
                                                                           "is an encrypted disc image";
    return std::string("Your Twilight Princess HD ") + what + ": convert it to .wua in Cemu, then press Try again.";
}

// The Windows error number of a detail ending in "(os error N)" (the converter's base::os_error_text and the
// worker's lock), or -1.
long os_error_of(std::string_view detail) {
    const std::size_t at = detail.rfind("(os error ");
    if (at == std::string_view::npos) {
        return -1;
    }
    long value = 0;
    std::size_t i = at + 10;
    const std::size_t start = i;
    while (i < detail.size() && std::isdigit(static_cast<unsigned char>(detail[i])) && i - start < 9) {
        value = value * 10 + (detail[i] - '0');
        ++i;
    }
    return i > start && i < detail.size() && detail[i] == ')' ? value : -1;
}

// "Couldn't save the game files: <reason in 2-4 words>." (plan 1.2). By the Windows error number first (wave 4
// review L3: the error text follows the Windows display language), else by the English words of the text.
std::string write_line(std::string_view detail) {
    const char* reason = nullptr;
    switch (os_error_of(detail)) {
    case 5:     // ERROR_ACCESS_DENIED
        reason = "access denied";
        break;
    case 39:    // ERROR_HANDLE_DISK_FULL
    case 112:   // ERROR_DISK_FULL
    case 1295:  // ERROR_DISK_QUOTA_EXCEEDED
        reason = "the disk is full";
        break;
    case 32:    // ERROR_SHARING_VIOLATION
    case 33:    // ERROR_LOCK_VIOLATION
        reason = "a file is in use";
        break;
    case 19:    // ERROR_WRITE_PROTECT
        reason = "the folder is read-only";
        break;
    default:
        break;
    }
    if (reason != nullptr) {
        return std::string("Couldn't save the game files: ") + reason + ".";
    }
    std::string lower(detail);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto has = [&](const char* word) { return lower.find(word) != std::string::npos; };
    reason = "a write error";
    if (has("denied")) {
        reason = "access denied";
    } else if (has("space") || has("full")) {
        reason = "the disk is full";
    } else if (has("being used") || has("in use")) {
        reason = "a file is in use";
    } else if (has("read-only") || has("write-protect")) {
        reason = "the folder is read-only";
    }
    return std::string("Couldn't save the game files: ") + reason + ".";
}

// wave3 G-L4: the converter's code for a TP HD input that exists but can't be read ("hd.read"; also accepted
// under the name "hd.unreadable"), and the plain read errors it passes on (base_util.cpp: "io.open" when a file
// can't be opened, "io.read" when a read fails).
bool is_hd_read_code(std::string_view code) {
    return code == "hd.read" || code == "hd.unreadable";
}
bool is_read_error(std::string_view code) {
    return code == "io.read" || code == "io.open";
}

// A detection problem (detect.cpp problem_text: "<code>: <detail>", or "damaged .wua: <code>: <detail>") that is
// only a read error, not a broken copy.
bool problem_is_read_error(std::string_view p) {
    if (p.starts_with("damaged .wua: ")) {
        p.remove_prefix(14);
    }
    for (const std::string_view code : {"hd.read", "hd.unreadable", "io.read", "io.open"}) {
        if (p.starts_with(code) && (p.size() == code.size() || p[code.size()] == ':')) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string making_line(int pct) {
    return "Making the game files from your Twilight Princess HD (" + std::to_string(pct) + "%)…";
}

std::string updating_line(int pct) {
    return "Updating the game files (" + std::to_string(pct) + "%)…";
}

std::string line_for(const worker::Result& r) {
    const std::string& code = r.code;
    if (code == "hd.candidate") {
        const std::string& p = r.candidate_problem;
        if (r.candidate_kind == "encrypted") {
            return encrypted_line(p);
        }
        if (p.starts_with("update only")) {
            return kLineUpdateOnly;
        }
        if (p.starts_with("missing ")) {
            return missing_line(p);
        }
        if (p.starts_with("different version")) {
            return kLineVersion;
        }
        if (problem_is_read_error(p)) {
            return kLineHdUnreadable;
        }
        if (p.starts_with("damaged .wua")) {
            return kLineDamagedWua;
        }
        return kLineOther;
    }
    if (is_hd_read_code(code) || (r.reading_hd && is_read_error(code))) {
        return kLineHdUnreadable;
    }
    if (code == "hd.missing") {
        return missing_line(r.detail);
    }
    if (code == "hd.size" || code == "hd.hash" || code == "hd.version") {
        return kLineVersion;
    }
    if (code == "hd.encrypted") {
        return encrypted_line(r.detail);
    }
    if (code == "hd.not_found" || code == "hd.not_tphd") {
        return r.picked ? kLineEmptyFolder : kLineOther;
    }
    if (code.starts_with("wua.")) {
        return kLineDamagedWua;
    }
    if (code.starts_with("gc.")) {
        return kLineGcCold;
    }
    if (code == "space") {
        return kLineSpace;
    }
    if (code == "io.write") {
        return write_line(r.detail);
    }
    if (code == "out.check" || code == "out.missing" || code == "out.duplicate" || code == "cache.folder" ||
        code.starts_with("cache."))
    {
        return kLineCheck;
    }
    return kLineOther;
}


}  // namespace hdfiles::setup
