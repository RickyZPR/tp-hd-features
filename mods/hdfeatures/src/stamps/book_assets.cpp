#include "prelude.hpp" // library headers before any game header (see prelude.hpp)
#include "hdfiles/files.hpp"

#include "book_assets.hpp"

#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "m_Do/m_Do_ext.h"

#include <array>
#include <cstring>
#include <new>
#include <string_view>

namespace hdstamps::book::assets {
namespace {

constexpr char kArchivePath[] = "stamps/stampbook/stampResR_gc.arc"; // res/stamps/stampbook
// The pipeline keeps the archive below 16 MiB (check_book); anything larger is not ours.
constexpr size_t kMaxArchiveSize = 16u * 1024u * 1024u;
constexpr size_t kMaxLooseSize = 1024u * 1024u;
constexpr size_t kAlignment = 32;

// The four layouts the book loads (J2DScreen::setPriority looks them up by name, case-folded).
constexpr std::array<std::string_view, 4> kLayouts = {
    "zelda_stamps_background.blo",
    "zelda_stamps_menu.blo",
    "zelda_stamps_buttons.blo",
    "zelda_stamps_message.blo",
};

struct Blob {
    uint8_t* data = nullptr;
    size_t size = 0;

    void reset() {
        if (data != nullptr) {
            ::operator delete(data, std::align_val_t{kAlignment});
        }
        data = nullptr;
        size = 0;
    }
};

Blob g_archive;
Blob g_slotIcon;
Blob g_filePlate;
Blob g_filePlus;
Blob g_fileIcon;
bool g_archiveReady = false;
const char* g_archiveProblem = "not loaded";
std::string g_archiveProblemText; // owns g_archiveProblem when it comes from hdfiles::problem
int g_mounted = 0; // live JKRMemArchive objects (diagnostics; must be 0 when unloading)

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// Copies one of the stored game files (hdfiles/files.hpp, checked when read) into a fresh 32-byte-aligned buffer
// that stays put for the session (JKRMemArchive and the GX textures need both).
bool load_file(const char* path, size_t maxSize, Blob& out, bool optional) {
    out.reset();
    std::vector<uint8_t> bytes;
    if (!hdfiles::load(path, bytes)) {
        if (!optional) {
            mods::log::warn("{}", hdfiles::problem(path));
        } else {
            mods::log::info("res/{} not loaded", path);
        }
        return false;
    }
    bool ok = false;
    if (bytes.empty() || bytes.size() > maxSize) {
        mods::log::warn("res/{} has an unexpected size ({} bytes)", path, bytes.size());
    } else {
        void* copy = ::operator new(bytes.size(), std::align_val_t{kAlignment}, std::nothrow);
        if (copy == nullptr) {
            mods::log::error("out of memory copying res/{} ({} bytes)", path, bytes.size());
        } else {
            std::memcpy(copy, bytes.data(), bytes.size());
            out.data = static_cast<uint8_t*>(copy);
            out.size = bytes.size();
            ok = true;
        }
    }
    return ok;
}

// A complete GC BTI: 0x20-byte header, sane size and format, pixel data inside the file, no palette.
bool valid_bti(const Blob& blob) {
    if (blob.data == nullptr || blob.size < 0x40) {
        return false;
    }
    const uint8_t* h = blob.data;
    const uint8_t format = h[0];
    const uint16_t width = be16(h + 2);
    const uint16_t height = be16(h + 4);
    const uint32_t imageOffset = be32(h + 0x1C);
    if (width == 0 || height == 0 || width > 1024 || height > 1024 || format > 0x0E) {
        return false;
    }
    return imageOffset >= 0x20 && imageOffset < blob.size;
}

bool validate_archive(const Blob& blob, const char*& why) {
    const uint8_t* a = blob.data;
    const size_t size = blob.size;
    if (a == nullptr || size < 0x40 || std::memcmp(a, "RARC", 4) != 0) {
        why = "not a RARC archive (a compressed archive cannot be used in place)";
        return false;
    }
    if (be32(a + 0x08) != 0x20) {
        why = "unexpected RARC header size";
        return false;
    }
    const uint64_t dataBase = 0x20ull + be32(a + 0x0C);
    const uint32_t numNodes = be32(a + 0x20);
    const uint64_t nodes = 0x20ull + be32(a + 0x24);
    const uint32_t numEntries = be32(a + 0x28);
    const uint64_t entries = 0x20ull + be32(a + 0x2C);
    const uint32_t strSize = be32(a + 0x30);
    const uint64_t strings = 0x20ull + be32(a + 0x34);
    if (numNodes == 0 || numNodes > 0x100 || numEntries == 0 || numEntries > 0x1000 ||
        nodes + numNodes * 0x10ull > size || entries + numEntries * 0x14ull > size ||
        strings + strSize > size || dataBase > size)
    {
        why = "RARC tables out of range";
        return false;
    }
    std::array<bool, kLayouts.size()> found{};
    for (uint32_t i = 0; i < numEntries; ++i) {
        const uint8_t* e = a + entries + i * 0x14ull;
        const uint32_t typeAndName = be32(e + 4);
        if (((typeAndName >> 24) & 1u) == 0) {
            continue; // directory entry
        }
        const uint32_t nameOffset = typeAndName & 0xFFFFFFu;
        const uint64_t fileStart = dataBase + be32(e + 8);
        const uint32_t fileSize = be32(e + 12);
        if (nameOffset >= strSize || fileStart + fileSize > size) {
            why = "RARC file entry out of range";
            return false;
        }
        if ((fileStart % kAlignment) != 0) {
            why = "RARC member data is not 32-byte aligned";
            return false;
        }
        const char* name = reinterpret_cast<const char*>(a + strings + nameOffset);
        const size_t maxLen = strSize - nameOffset;
        const std::string_view entryName{name, strnlen(name, maxLen)};
        for (size_t k = 0; k < kLayouts.size(); ++k) {
            if (entryName == kLayouts[k]) {
                if (fileSize < 0x20 || std::memcmp(a + fileStart, "SCRNblo", 7) != 0) {
                    why = "a stamp layout is not a SCRN blo file";
                    return false;
                }
                found[k] = true;
            }
        }
    }
    for (bool f : found) {
        if (!f) {
            why = "a zelda_stamps_*.blo layout is missing from the archive";
            return false;
        }
    }
    return true;
}

void load_loose(const char* path, Blob& blob) {
    if (load_file(path, kMaxLooseSize, blob, true) && !valid_bti(blob)) {
        mods::log::warn("res/{} is not a usable BTI; ignoring it", path);
        blob.reset();
    }
}

const ResTIMG* timg(const Blob& blob) {
    return blob.data != nullptr ? reinterpret_cast<const ResTIMG*>(blob.data) : nullptr;
}

} // namespace

bool load() {
    if (g_archive.data != nullptr || g_slotIcon.data != nullptr) {
        unload(); // forgets, never frees (see unload)
    }
    if (load_file(kArchivePath, kMaxArchiveSize, g_archive, false)) {
        const char* why = "";
        if (validate_archive(g_archive, why)) {
            g_archiveReady = true;
            g_archiveProblem = "";
        } else {
            mods::log::error("res/{} is not usable ({}); the stamp book falls back to the RmlUi list",
                kArchivePath, why);
            g_archiveProblem = why;
            g_archive.reset();
        }
    } else {
        g_archiveProblemText = hdfiles::problem(kArchivePath); // missing, didn't pass the check, can't be read
        g_archiveProblem = g_archiveProblemText.c_str();
    }
    load_loose("stamps/stampbook/stampicon.bti", g_slotIcon);
    load_loose("stamps/stampbook/stampbutton.bti", g_filePlate);
    load_loose("stamps/stampbook/stampbuticon.bti", g_filePlus);
    load_loose("stamps/stampbook/filesel_stampicon.bti", g_fileIcon);
    mods::log::info("stamp book assets: archive {} ({} bytes), slot icon {}, file-select plate {}",
        g_archiveReady ? "ready" : "unavailable", g_archive.size,
        g_slotIcon.data != nullptr ? "ready" : "missing",
        g_filePlate.data != nullptr && g_filePlus.data != nullptr && g_fileIcon.data != nullptr ?
            "ready" :
            "missing");
    return g_archiveReady;
}

void unload() {
    if (g_mounted != 0) {
        mods::log::error("stamp archive still mounted at unload");
    }
    // Nothing is freed. Aurora keys GPU textures by texel pointer, so a texel address must never
    // be reused for other data while the game runs (a reloaded mod instance could get the same
    // allocation back), and the loose textures may still be referenced by a Collection or
    // file-select screen that outlives this mod instance. The buffers (about 12 MB) stay
    // allocated for the process; the CRT heap (/MD) outlives the DLL.
    g_archive = Blob{};
    g_slotIcon = Blob{};
    g_filePlate = Blob{};
    g_filePlus = Blob{};
    g_fileIcon = Blob{};
    g_archiveReady = false;
    g_archiveProblem = "not loaded";
}

bool archive_ready() {
    return g_archiveReady;
}

size_t archive_size() {
    return g_archive.size;
}

const char* archive_problem() {
    return g_archiveProblem;
}

const ResTIMG* slot_icon() {
    return timg(g_slotIcon);
}

const ResTIMG* filesel_plate() {
    return timg(g_filePlate);
}

const ResTIMG* filesel_plus() {
    return timg(g_filePlus);
}

const ResTIMG* filesel_icon() {
    return timg(g_fileIcon);
}

JKRArchive* mount() {
    if (!g_archiveReady) {
        return nullptr;
    }
    // On the archive heap, as the game's own in-memory archives (d_meter_HIO): never freeAll'd, so
    // the object can always be unmounted (it is linked into the global volume list) even if the
    // 2D heap the book is built on is wiped under it.
    JKRHeap* heap = mDoExt_getArchiveHeap();
    if (heap == nullptr) {
        heap = mDoExt_getCurrentHeap();
    }
    // C4291: the JKR placement new has no matching placement delete; the constructor never
    // throws (the game builds without exceptions for these paths).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4291)
#endif
    auto* archive = JKR_NEW_ARGS(heap, 0) JKRMemArchive(g_archive.data, static_cast<u32>(g_archive.size),
        JKRMEMBREAK_FLAG_UNKNOWN0);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (archive == nullptr) {
        mods::log::error("could not allocate the stamp archive object");
        return nullptr;
    }
    if (!archive->isMounted()) {
        mods::log::error("mounting the stamp archive failed");
        JKR_DELETE(archive);
        return nullptr;
    }
    ++g_mounted;
    return archive;
}

void unmount(JKRArchive*& archive) {
    if (archive == nullptr) {
        return;
    }
    // mMountCount is 1 after construction: unmount() deletes the object (JKRFileLoader::unmount),
    // which removes it from the volume list. The buffer is never freed (JKRMEMBREAK_FLAG_UNKNOWN0).
    archive->unmount();
    archive = nullptr;
    if (g_mounted > 0) {
        --g_mounted;
    }
}

} // namespace hdstamps::book::assets
