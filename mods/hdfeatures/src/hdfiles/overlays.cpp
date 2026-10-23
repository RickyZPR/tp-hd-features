// HD Features: the overlay files as runtime overlays (see overlays.hpp). Module R1.

// Library headers first, game headers last (stamps/prelude.hpp explains why).
#include "hdfiles/overlays.hpp"

#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <mods/api.h>
#include <mods/svc/log.hpp>
#include <mods/svc/overlay.h>

#include <dolphin/dvd.h>
#include "SSystem/SComponent/c_counter.h"

namespace hdfiles::overlays {
namespace {

std::vector<OverlayHandle> g_handles;

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

bool add_one(const std::string& path, const void* data, std::size_t size, std::string& why) {
    if (svc_overlay == NULL) {
        why = "OverlayService unavailable";
        return false;
    }
    OverlayHandle handle = 0;
    const ModResult result = svc_overlay->add_buffer(mod_ctx, path.c_str(), data, size, &handle);
    if (result != MOD_OK || handle == 0) {
        why = path + ": add_buffer result " + std::to_string(static_cast<int>(result));
        return false;
    }
    g_handles.push_back(handle);
    return true;
}

}  // namespace

std::string disc_path(std::string_view rel) {
    constexpr std::string_view kPrefix = "overlay/";
    if (!rel.starts_with(kPrefix) || rel.size() <= kPrefix.size()) {
        return {};
    }
    return "/" + std::string(rel.substr(kPrefix.size()));
}

hdf::Result<Report> register_from_cache(const cache::Cache& cache) {
    const std::vector<const cache::Entry*> list = cache.overlays();
    if (list.size() != hdf::outputs::kOverlayCount) {
        return hdf::fail("overlay.count", std::to_string(list.size()) + " overlay files in the stamp");
    }
    Report report;
    const std::size_t before = g_handles.size();
    auto rollback = [&] {
        while (g_handles.size() > before) {
            if (svc_overlay != NULL) {
                svc_overlay->remove(mod_ctx, g_handles.back());
            }
            g_handles.pop_back();
        }
    };
    for (const cache::Entry* e : list) {
        const auto t0 = std::chrono::steady_clock::now();
        auto data = cache.read(*e);
        report.read_seconds += seconds_since(t0);
        if (!data) {
            rollback();
            return std::unexpected(data.error());
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::string why;
        const bool ok = add_one(disc_path(e->rel), data->data(), data->size(), why);
        report.add_seconds += seconds_since(t1);
        if (!ok) {
            rollback();
            return hdf::fail("overlay.add", why);
        }
        ++report.files;
        report.bytes += data->size();
    }
    return report;
}

hdf::Result<Report> register_buffers(const cache::Cache& cache, std::vector<HandOver>&& files) {
    if (files.size() != hdf::outputs::kOverlayCount || cache.overlays().size() != files.size()) {
        return hdf::fail("overlay.count", std::to_string(files.size()) + " overlay buffers handed over");
    }
    Report report;
    const std::size_t before = g_handles.size();
    auto rollback = [&] {
        while (g_handles.size() > before) {
            if (svc_overlay != NULL) {
                svc_overlay->remove(mod_ctx, g_handles.back());
            }
            g_handles.pop_back();
        }
    };
    // Each stamp entry at most once: with the count equal to 56, that means every overlay exactly once
    // (wave3_R1_verify L3; a path handed over twice would leave another overlay unregistered).
    std::vector<const cache::Entry*> used;
    used.reserve(files.size());
    for (HandOver& h : files) {
        const std::string& rel = h.rel;
        std::vector<std::uint8_t>& bytes = h.bytes;
        const cache::Entry* e = cache.find(rel);
        if (e != NULL && std::find(used.begin(), used.end(), e) != used.end()) {
            rollback();
            return hdf::fail("overlay.count", rel + ": handed over twice");
        }
        used.push_back(e);
        // wave3 F4: h.sha256 was computed from these bytes on the worker thread (worker.cpp), so the 48 MB are not
        // hashed again in this frame. The size is still compared here.
        const auto t0 = std::chrono::steady_clock::now();
        const bool same = e != NULL && e->overlay && bytes.size() == e->size && !h.sha256.empty() &&
                          h.sha256 == e->sha256;
        report.read_seconds += seconds_since(t0);
        if (!same) {
            rollback();
            return hdf::fail("cache.hash", rel + ": the handed-over buffer differs from the stamp");
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::string why;
        const bool ok = add_one(disc_path(e->rel), bytes.data(), bytes.size(), why);
        report.add_seconds += seconds_since(t1);
        if (!ok) {
            rollback();
            return hdf::fail("overlay.add", why);
        }
        ++report.files;
        report.bytes += bytes.size();
        std::vector<std::uint8_t>().swap(bytes);  // the host has its own copy
    }
    return report;
}

void remove_all() {
    if (svc_overlay != NULL) {
        for (const OverlayHandle h : g_handles) {
            svc_overlay->remove(mod_ctx, h);
        }
    }
    g_handles.clear();
}

std::size_t count() {
    return g_handles.size();
}

bool probe_live() {
    return DVDConvertPathToEntrynum(kProbePath) >= 0;
}

std::uint32_t frame_counter() {
    return g_Counter.mCounter0;
}

}  // namespace hdfiles::overlays
