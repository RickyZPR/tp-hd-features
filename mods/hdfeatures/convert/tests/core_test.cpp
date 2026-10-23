// Skeleton tests (C2): core types, facts, vendored library versions, the stub contract.
#include "test.hpp"

#include "hdf/core.hpp"
#include "hdf/facts.hpp"
#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"

#include <algorithm>

HDF_TEST(core_result_and_fail) {
    hdf::Result<int> ok = 7;
    HDF_CHECK(ok.has_value());
    HDF_CHECK_EQ(*ok, 7);
    hdf::Result<int> bad = hdf::fail("io.read", "x");
    HDF_CHECK(!bad.has_value());
    HDF_CHECK_EQ(bad.error().code, std::string("io.read"));
    hdf::Result<void> t = hdf::todo("thing");
    HDF_CHECK(!t.has_value());
    HDF_CHECK_EQ(t.error().code, std::string("todo"));
}

HDF_TEST(core_pi_is_python_math_pi) {
    HDF_CHECK(hdf::kPi == 3.141592653589793);
}

HDF_TEST(core_progress_atomics) {
    hdf::Progress p;
    p.stage = static_cast<hdf::u32>(hdf::GenStage::Cave);
    p.total = 10;
    p.done += 3;
    HDF_CHECK_EQ(p.done.load(), 3u);
    HDF_CHECK_EQ(hdf::stage_name(hdf::GenStage::Cave), std::string_view("cave"));
}

HDF_TEST(facts_counts_and_order) {
    using namespace hdf::facts;
    HDF_CHECK_EQ(kCaveParticleIds.size(), 216u);
    HDF_CHECK(std::is_sorted(kCaveParticleIds.begin(), kCaveParticleIds.end()));
    HDF_CHECK(std::adjacent_find(kCaveParticleIds.begin(), kCaveParticleIds.end()) == kCaveParticleIds.end());
    HDF_CHECK_EQ(kCaveParticleIds.front(), 0x8113);
    HDF_CHECK_EQ(kCaveParticleIds.back(), 0x8C77);
    HDF_CHECK(is_cave_particle(0x84D0));
    HDF_CHECK(!is_cave_particle(0x8114));
    HDF_CHECK_EQ(kStampItemIds.size(), 50u);
    HDF_CHECK_EQ(int(kStampItemIds[0]), 0x13);
    HDF_CHECK_EQ(int(kStampItemIds[49]), 0xD9);
    HDF_CHECK_EQ(kHdChests.size(), 7u);
    HDF_CHECK_EQ(kHdChests[0].stage, std::string_view("D_SB07"));
    HDF_CHECK_EQ(int(kHdChests[6].item), 0xD8);
    HDF_CHECK_EQ(int(kCaveTagFlow), 1121);
    HDF_CHECK_EQ(int(kCaveTagRooms[0]), 10);
    HDF_CHECK_EQ(int(kCaveTagRooms[1]), 48);
    HDF_CHECK_EQ(kLabelCountGc, 822u);
    HDF_CHECK(label(78) != nullptr && label(78)->bit == 0x0904);
    HDF_CHECK(label(79) == nullptr);
    // the HD item of each HD-only chest is that stamp's item id
    for (const HdChest& c : kHdChests) HDF_CHECK_EQ(int(kStampItemIds[c.stamp]), int(c.item));
}

HDF_TEST(constants_match_the_plan) {
    HDF_CHECK_EQ(hdf::inputs::kPsceneBudget, 267040u);
    HDF_CHECK_EQ(hdf::outputs::kKeptCount + hdf::outputs::kDroppedCount, 206u);
    HDF_CHECK_EQ(hdf::outputs::kOracleSha256.size(), 64u);
}

HDF_TEST(vendored_library_versions) {
    const hdf::LibVersions v = hdf::lib_versions();
    HDF_CHECK_EQ(v.zlib, std::string("1.3.2"));
    HDF_CHECK_EQ(v.zstd, std::string("1.5.7"));
}

HDF_TEST(base_tables_present) {
    if (hdf::inputs::hd_files().empty()) HDF_SKIP("todo: data/inputs.inc (tools/dev/gen_tables.py)");
    HDF_CHECK_EQ(hdf::inputs::hd_files().size(), hdf::inputs::kHdCount);
    HDF_CHECK_EQ(hdf::inputs::gc_files().size(), hdf::inputs::kGcCount);
    HDF_CHECK_EQ(hdf::outputs::expected().size(), hdf::outputs::kKeptCount);
    HDF_CHECK_EQ(hdf::outputs::drop_list().size(), hdf::outputs::kDroppedCount);
}
