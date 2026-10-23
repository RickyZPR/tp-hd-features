#pragma once

// Cave of Shadows (D_SB11) floor layout.
//
// Derived offline from the user's TP HD dump, content/los.bin (50 x 0x20 records
// {room, next, prev, floor, x, y, z, ?, angleY}; dumped with tools/cave/losbin.py) and
// cross-checked against research/cave_floors.md. Only data facts (room/floor numbers) are
// embedded here, so the mod does not depend on res/cave/floors.json being generated.
//
// The main chain is floors 1..41: 19 -> 35 -> 21 -> ... -> 49 -> 14. Rooms 24 and 37 form a
// detached two-room chain (los floors 42/43) that nothing in the data leads to; rooms
// 0, 6, 9, 11, 12, 20 and 39 are model-only leftovers (floor 0).

#include <cstdint>

namespace cave {

inline constexpr int kRoomCount = 50;
inline constexpr int kLastFloor = 41;   // reward room (room 14)
inline constexpr int kFairyFloor = 40;  // room 49, HD end fairy (FSeirei type 4)
inline constexpr int kEntryRoom = 19;   // floor 1
inline constexpr int kFinalRoom = 14;   // floor 41

// HD sections (research/cos_sections_spec.md: 1 = floors 1-5, 2 = floors 7-19, 3 = floors 21-40). Every
// visit starts on floor 1 (room 19), including Continue after a death (VERIFIED, HD 0x02abac38(1) callers
// 0x02a020ac / 0x020b9f6c). Story state only changes what the section-end floors do (tier ALLdie, cave.cpp
// alldie_tier_switch): floor 5/19 ends the run with a talk (flow 1121) and the rest floor 6/20 shows an
// l8warp portal while the next section is locked; once it is unlocked they open straight on. Rooms 26
// and 2 (floors 7 and 21) are only reached by walking down; their r00_in intro is camera-only (flow 0).
inline constexpr int kSection2Room = 26;  // floor 7 (first floor of section 2)
inline constexpr int kSection3Room = 2;   // floor 21 (first floor of section 3)
inline constexpr int kSectionEndRoom1 = 10;  // floor 5: tier-1 ALLdie, swc00 type 11, Tag_ms flow 1121
inline constexpr int kSectionEndRoom2 = 48;  // floor 19: tier-2 ALLdie, swc00 type 11, Tag_ms flow 1121
inline constexpr int kRestRoom1 = 25;     // floor 6 (tier-1 ALLdie, l8warp)
inline constexpr int kRestRoom2 = 34;     // floor 20 (tier-2 ALLdie, l8warp)
// The rest floors' descent gates (rGate, Obj_SwallShutter) open on dungeon switch 0x80 + room, which their
// HD AND_SW2 (c2990002 / c2a20002: C2 = Link in the centre AND C3 = ALLdie's "unlocked" switch) sets; the
// portal's switch C4 is AND_SW2 c1c40002 (C1 = ALLdie's "locked" switch AND C2). VERIFIED in the data.
inline constexpr int kRestGateSwitch1 = 0x80 + kRestRoom1;  // 0x99
inline constexpr int kRestGateSwitch2 = 0x80 + kRestRoom2;  // 0xA2

enum class FloorKind : std::uint8_t {
    None,    // not reachable / not a floor
    Fight,   // enemies; ALLdie opens the descent gate
    Rest,    // chest + l8warp exit (floors 6 and 20)
    Fairy,   // floor 40 (HD end fairy; the GC port opens its gate via ALLdie, see cave_patch.py)
    Reward,  // floor 41: chest + l8warp exit
    Detached // rooms 24/37 (los floors 42/43, unreachable)
};

// room -> floor index (los.bin field 3). VERIFIED against los.bin.
inline constexpr std::uint8_t kFloorOfRoom[kRoomCount] = {
    /*  0 */ 0,  /*  1 */ 37, /*  2 */ 21, /*  3 */ 26, /*  4 */ 29,
    /*  5 */ 9,  /*  6 */ 0,  /*  7 */ 16, /*  8 */ 8,  /*  9 */ 0,
    /* 10 */ 5,  /* 11 */ 0,  /* 12 */ 0,  /* 13 */ 17, /* 14 */ 41,
    /* 15 */ 12, /* 16 */ 35, /* 17 */ 32, /* 18 */ 25, /* 19 */ 1,
    /* 20 */ 0,  /* 21 */ 3,  /* 22 */ 13, /* 23 */ 4,  /* 24 */ 42,
    /* 25 */ 6,  /* 26 */ 7,  /* 27 */ 23, /* 28 */ 10, /* 29 */ 31,
    /* 30 */ 30, /* 31 */ 15, /* 32 */ 24, /* 33 */ 14, /* 34 */ 20,
    /* 35 */ 2,  /* 36 */ 22, /* 37 */ 43, /* 38 */ 36, /* 39 */ 0,
    /* 40 */ 28, /* 41 */ 33, /* 42 */ 38, /* 43 */ 34, /* 44 */ 27,
    /* 45 */ 39, /* 46 */ 18, /* 47 */ 11, /* 48 */ 19, /* 49 */ 40,
};

constexpr int floor_of_room(int room) {
    return (room >= 0 && room < kRoomCount) ? kFloorOfRoom[room] : 0;
}

constexpr FloorKind floor_kind(int floor) {
    if (floor <= 0) {
        return FloorKind::None;
    }
    if (floor > kLastFloor) {
        return FloorKind::Detached;
    }
    switch (floor) {
    case 6:
    case 20:
        return FloorKind::Rest;
    case kFairyFloor:
        return FloorKind::Fairy;
    case kLastFloor:
        return FloorKind::Reward;
    default:
        return FloorKind::Fight;
    }
}

// Every floor 1..41 appears exactly once in the table.
constexpr bool main_chain_is_complete() {
    for (int floor = 1; floor <= kLastFloor; ++floor) {
        int count = 0;
        for (int room = 0; room < kRoomCount; ++room) {
            if (kFloorOfRoom[room] == floor) {
                ++count;
            }
        }
        if (count != 1) {
            return false;
        }
    }
    return true;
}

static_assert(main_chain_is_complete());
static_assert(floor_of_room(kEntryRoom) == 1);
static_assert(floor_of_room(kFinalRoom) == kLastFloor);
static_assert(floor_of_room(49) == kFairyFloor);
static_assert(floor_of_room(kRestRoom1) == 6 && floor_kind(6) == FloorKind::Rest);
static_assert(floor_of_room(kRestRoom2) == 20 && floor_kind(20) == FloorKind::Rest);
static_assert(floor_of_room(kSection2Room) == 7);
static_assert(floor_of_room(kSection3Room) == 21);
static_assert(floor_of_room(kSectionEndRoom1) == 5 && floor_of_room(kSectionEndRoom2) == 19);
static_assert(kRestGateSwitch1 == 0x99 && kRestGateSwitch2 == 0xA2);

}  // namespace cave
