// HD Features: the 13 GameCube inputs from the running disc (see dvd_source.hpp). Module R2.

// Library headers first, game headers last (stamps/prelude.hpp explains why).
#include "stamps/prelude.hpp"

#include "hdfiles/dvd_source.hpp"

#include "hdf/inputs.hpp"
#include "hdf/source.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <dolphin/dvd.h>

namespace hdfiles::dvd {

std::string disc_id() {
    const DVDDiskID* id = DVDGetCurrentDiskID();
    if (id == NULL) {
        return {};
    }
    std::string out;
    for (const char c : id->gameName) {
        out.push_back(c);
    }
    for (const char c : id->company) {
        out.push_back(c);
    }
    for (const char c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
            return {};  // not a printable id
        }
    }
    return out;
}

hdf::Result<GcInputs> read_gc_inputs() {
    const auto t0 = std::chrono::steady_clock::now();
    auto source = std::make_shared<hdf::MemorySource>("GameCube disc");
    GcInputs out;
    std::uint32_t unreadable = 0, wrongSize = 0, differ = 0;
    std::string first;
    const auto note = [&](std::uint32_t& counter, std::string what) {
        ++counter;
        if (first.empty()) {
            first = std::move(what);
        }
    };
    for (const hdf::inputs::InputFile& file : hdf::inputs::gc_files()) {
        // The embedded paths are lower-case; the game's file table is searched case-insensitively
        // (aurora dvd.cpp findInDir).
        const std::string path = "/" + std::string(file.path);
        DVDFileInfo info{};
        if (!DVDOpen(path.c_str(), &info)) {
            note(unreadable, path + " (can't open)");
            continue;
        }
        const std::uint32_t length = info.length;
        if (length != file.size) {
            DVDClose(&info);
            note(wrongSize, path + " (" + std::to_string(length) + " bytes)");
            continue;
        }
        hdf::Bytes data(length);
        const s32 got = length == 0 ? 0 : DVDReadPrio(&info, data.data(), static_cast<s32>(length), 0, 2);
        DVDClose(&info);
        if (got < static_cast<s32>(length)) {
            note(unreadable, path + " (read " + std::to_string(got) + " of " + std::to_string(length) + ")");
            continue;
        }
        if (hdf::sha256_hex(data) != file.sha256) {
            note(differ, path);
            continue;
        }
        out.bytes += data.size();
        ++out.files;
        source->add(file.path, std::move(data));
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::uint32_t problems = unreadable + wrongSize + differ;
    if (problems != 0) {
        const std::string detail = std::to_string(problems) + " of " + std::to_string(hdf::inputs::gc_files().size()) +
                                   " differ; first: " + first;
        return hdf::fail(unreadable != 0 ? "gc.read" : wrongSize != 0 ? "gc.size" : "gc.hash", detail);
    }
    out.source = std::move(source);
    return out;
}

}  // namespace hdfiles::dvd
