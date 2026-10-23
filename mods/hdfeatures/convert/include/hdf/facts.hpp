// hdf_convert: facts the pipeline needs that are not in the player's files (owner C2, Correction #11).
// Numbers, ids and this project's own short descriptions only, as tools/pipeline/facts.py. The values are in
// data/facts.inc, generated from facts.py by tools/dev/gen_facts_inc.py. Header-only (constexpr).
#pragma once

#include "hdf/core.hpp"

#include <array>
#include <string_view>

namespace hdf::facts {

struct SaveLabel {             // facts.SAVE_LABELS: index -> (name, bit, description)
    u16 index;
    std::string_view name;
    u16 bit;
    std::string_view description;
};

struct HdChest {               // facts.HD_CHESTS
    u8 stamp;                  // stamp index 0-49
    std::string_view stage;    // e.g. "D_SB01"
    u8 room;
    u8 chest;                  // chest number
    u8 item;                   // HD item id
};

#include "../../data/facts.inc"

static_assert(kCaveParticleIds.size() == 216);
static_assert(kStampItemIds.size() == 50);
static_assert(kHdChests.size() == 7);

// facts.label_table(): kLabelCountGc entries, nullptr where facts.py has None.
constexpr const SaveLabel* label(u32 index) {
    for (const SaveLabel& l : kSaveLabels)
        if (l.index == index) return &l;
    return nullptr;
}

constexpr bool is_cave_particle(u16 id) {
    for (u16 v : kCaveParticleIds)
        if (v == id) return true;
    return false;
}

}  // namespace hdf::facts
