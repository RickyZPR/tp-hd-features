#include "prelude.hpp" // library headers before any game header (see prelude.hpp)

#include "items.hpp"

#include "hdtext.hpp"
#include "state.hpp"
#include "ui.hpp"
#include "hdfiles/files.hpp"

#include <mods/items.h>
#include <mods/svc/flow.hpp>
#include <mods/svc/hook.hpp>
#include <mods/svc/item.h>
#include <mods/svc/log.hpp>
#include <mods/svc/message.h>
#include <mods/svc/stage.h>

#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_itembase.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_info.h"
#include "d/d_resorce.h"
#include "f_op/f_op_actor_mng.h"

#include <dolphin/dvd.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// d_item.cpp; exported (?getItemFunc@@YAXE@Z) but not declared in a header. The randomizer hooks it
// the same way (reference/randomizer/src/hooks.cpp:117).
extern void getItemFunc(u8);

// Game functions hooked by this mod. All five are exported from dusklight.exe (verified in the
// v2.0.3 import library) and are reached through real calls in the shipped binary (call-site
// scan, see the stamps API review), so they are named directly; none is a replace-hook.
//
// - getItemFunc (pre + post): marks "a stamp's item function is running" so the first-bit hook
//   below only acts on the first-get bit that getItemFunc itself sets for a stamp.
// - dComIfGs_onItemFirstBit (DUSK_NOINLINE): skipped for that one call, so receiving a stamp leaves
//   no new "item first-get" bit in the .gci. Every other caller (other mods, debug menus) is left
//   alone. Stamp ownership lives only in the SaveService blob.
// - daItemBase_c::CreateItemHeap: if the Obj_Stamp archive's file order differs from the validated
//   index, re-points the bmd index at "stamp.bmd" before the model is built.
// - dMeter2Info_readItemTexture (DUSK_NOINLINE, the randomizer hooks it the same way): copies the
//   stamp's own 48x48 icon into the item-get window's texture buffer.
// - daAlink_c::setGetSubBgm: stamps are SETYPE_NONE in GC's fanfare table, so the item number is
//   swapped for one that plays the short item-get jingle.
DEFINE_HOOK(&getItemFunc, HdsGetItemFunc);
DEFINE_HOOK(&dComIfGs_onItemFirstBit, HdsOnItemFirstBit);
DEFINE_HOOK(&daItemBase_c::CreateItemHeap, HdsCreateItemHeap);
DEFINE_HOOK(&dMeter2Info_readItemTexture, HdsReadItemTexture);
DEFINE_HOOK(&daAlink_c::setGetSubBgm, HdsSetGetSubBgm);

namespace hdstamps::items {
namespace {

constexpr char kStampArc[] = "Obj_Stamp";
constexpr char kStampArcPath[] = "/res/Object/Obj_Stamp.arc";
constexpr char kStampBmdName[] = "stamp.bmd";
// Upper bound for reading/decompressing the archive during validation (the converted one is 74 KB).
constexpr uint32_t kMaxStampArcSize = 4u * 1024u * 1024u;
// itemicon.arc "letter_.bti" (dRes_INDEX_ITEMICON_BTI_LETTER__e): a valid GC icon shown in the
// item-get window when no custom stamp icon is available. HD's own indices (123+i) do not exist
// in the GC archive (118 entries) and would make JKRReadIdxResource fail.
constexpr s16 kFallbackIconIndex = 0x39;
constexpr size_t kIconBufferSize = 0xC00; // dMeter2Info_c::readItemTexture buffer size

// The archive name written into dItem_data::item_resource. It is copied to the heap and never
// freed on purpose: the item table is a game global that outlives this DLL, and another mod (the
// randomizer snapshots and restores the whole table) may write an entry back after we unloaded.
// A pointer into this DLL's .rdata would then dangle; a leaked 10-byte heap string stays valid.
const char* make_stamp_arc_name() {
    char* name = new (std::nothrow) char[sizeof(kStampArc)];
    if (name == nullptr) {
        return kStampArc;
    }
    std::memcpy(name, kStampArc, sizeof(kStampArc));
    return name;
}
const char* const g_stampArcName = make_stamp_arc_name();

constexpr uint8_t kGroupZel00 = 0;
// HD's text is English (a US disc only has Msgus); like the own wording it is installed for every
// language so a PAL language setting still shows the stamps' text instead of GC's empty slots.
constexpr std::array<uint8_t, 6> kLanguages = {
    MESSAGE_LANGUAGE_ENGLISH, MESSAGE_LANGUAGE_GERMAN, MESSAGE_LANGUAGE_FRENCH,
    MESSAGE_LANGUAGE_SPANISH, MESSAGE_LANGUAGE_ITALIAN, MESSAGE_LANGUAGE_JAPANESE};

// Non-chest triggers.
constexpr char kCastleTownInteriors[] = "R_SP160";
constexpr int kJovaniRoom = 5;
// Field items are named "freestanding:<stage>:<bit>" (d_a_obj_item.cpp:245, item_checks.cpp:147);
// Gengle's silver rupee (d_a_npc_ne.cpp message()) is a field item created in R_SP160.
constexpr std::string_view kCastleTownFieldItemCheck = "freestanding:R_SP160:";
// Shop checks are named "shop:<stage>:<room>:<item>" (src/dusk/mods/item_checks.cpp); the Castle
// Town Malo Mart is R_SP160 room 0.
constexpr std::string_view kMaloMartShopCheck = "shop:R_SP160:0:";
constexpr int kMaloMartRoom = 0;
constexpr uint16_t kMaloMartOpenFlag = 0x2210; // d_com_inf_game.cpp: R_SP160 room 0 uses layer 1
constexpr u8 kPoeSoulsForGengle = 60;

// HD moved the slingshot seeds of Hyrule Castle chest 16 (now the Happy Zelda stamp) into chest 29
// of the same room, which holds a green rupee on GC (research/final_stamps.md, "Chest changes that
// are not stamps"; re-checked against both dumps: GC R08 TRES tboxNo 29 item 0x01, HD item 0x12).
constexpr char kSeedMoveCheck[] = "chest:D_MN09:29";
constexpr int kSeedMoveRoom = 8;

// Item number whose fanfare (SETYPE_ITEM_GET_MINI in daAlink_c::setGetSubBgm) stamps borrow.
constexpr int kFanfareDonorItem = dItemNo_ORANGE_RUPEE_e;

dItem_itemResource g_original[kStampCount]{};
dItem_itemResource g_written[kStampCount]{};
bool g_originalCaptured = false; // g_original is captured once per loaded DLL image
bool g_tablesApplied = false;
bool g_modelFound = false;   // Obj_Stamp.arc exists and passed validation
bool g_modelInvalid = false; // Obj_Stamp.arc exists but is unusable: keep the placeholder
int g_modelProbeTimer = 0;
int g_modelDisableTimer = 0; // > 0: switch to the placeholder model after this many frames
int16_t g_bmdIndex = kGeneratedStampBmdIndex;

std::vector<StageActorHandle> g_stageEdits;
std::vector<ItemCheckHandle> g_resolvers;
std::vector<MessageOverrideHandle> g_messageOverrides;
ItemGiveHandle g_giveObserver = 0;

std::array<std::vector<uint8_t>, kStampCount> g_icons;
int g_iconCount = 0;

int g_hooksInstalled = 0;
constexpr int kHooksWanted = 6; // getItemFunc pre + post, and one each for the other four
bool g_getItemFuncHooked = false;
// Item number of the stamp whose getItemFunc is running, -1 otherwise.
int g_stampItemFuncItem = -1;

// Stamps whose demo give was requested by an NPC/shop trigger but not yet received.
uint64_t g_pendingGive = 0; // waiting to be queued
uint64_t g_queuedGive = 0;  // handed to ItemService::give_item
// Stamps whose HD line (Gengle / Malo Mart, npc_line_override) went into the talk that is running
// now. The host resolves one message display more than once (measure and render passes), so the
// text has to stay the same until that talk is over; release_npc_lines() clears it on the first
// frame without an event. Later talks see the stamp pending and keep GC's text.
uint64_t g_npcLineShown = 0;
// Frames without a running event. NPC rewards are followed by more talk (Agitha re-opens her
// conversation after the bug reward), so a requested stamp is only queued once things are quiet;
// ItemService then still waits for Link to be idle before starting the get-item demo.
int g_quietFrames = 0;
constexpr int kQuietFramesBeforeGive = 45;
// While HD's own Agitha dialogue is installed (see "Agitha's stamp dialogue" below) she hands the
// stamp over herself, in the talk that follows the bug reward. The queued give is then only a
// safety net for a talk that never came, so it waits much longer.
constexpr int kAgithaDialogueFallbackFrames = 600;
// ItemService drops an in-flight give without telling anyone after 5 failed dispatches
// (item_gives.cpp item_gives_tick). A queued stamp that has not arrived after this many quiet
// frames (~1.5 min at 30 Hz) is granted without the get-item scene so it is not lost.
int g_queuedQuietFrames = 0;
constexpr int kQueuedGiveTimeoutFrames = 2700;

uint64_t g_warnedChest = 0;

// Agitha's stamp dialogue (HD zel_04 flow 1900 head), see the section of that name below.
struct NativeFlowStart {
    uint16_t node = 0;     // GC FLI1 start node of the flow
    FlowNodeData data{};   // its original 8 bytes
};
mods::flow::Query g_agithaQuery;
mods::flow::Graph g_agithaGraph; // engaged while the patch is live (vanilla save selected)
std::vector<mods::flow::RegisteredMessage> g_agithaMessages;
std::optional<NativeFlowStart> g_agithaStart;
bool g_agithaGiveUp = false; // a permanent failure was logged: keep GC's flow
int g_agithaProbeTimer = 0;
int g_agithaProbeCount = 0;

// --- small helpers -----------------------------------------------------------------------------

bool read_be32(const std::vector<uint8_t>& data, size_t offset, uint32_t& out) {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    out = (uint32_t{data[offset]} << 24) | (uint32_t{data[offset + 1]} << 16) |
          (uint32_t{data[offset + 2]} << 8) | uint32_t{data[offset + 3]};
    return true;
}

bool read_be16(const std::vector<uint8_t>& data, size_t offset, uint16_t& out) {
    if (offset > data.size() || data.size() - offset < 2) {
        return false;
    }
    out = static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
    return true;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

// --- stamp archive validation -------------------------------------------------------------------

// Reads a whole file through the game's DVD layer, i.e. exactly what dRes_control_c will load
// (the static overlay included).
bool read_dvd_file(const char* path, std::vector<uint8_t>& out, std::string& why) {
    DVDFileInfo info{};
    if (!DVDOpen(path, &info)) {
        why = "DVDOpen failed";
        return false;
    }
    const uint32_t length = info.length;
    bool ok = false;
    if (length < 0x40 || length > kMaxStampArcSize) {
        why = fmt::format("unexpected size {}", length);
    } else {
        out.assign(length, 0);
        const s32 got = DVDReadPrio(&info, out.data(), static_cast<s32>(length), 0, 2);
        if (got < static_cast<s32>(length)) {
            why = fmt::format("read {} of {} bytes", got, length);
        } else {
            ok = true;
        }
    }
    DVDClose(&info);
    return ok;
}

bool yaz0_decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    uint32_t size = 0;
    if (!read_be32(in, 4, size) || size == 0 || size > kMaxStampArcSize || in.size() < 16) {
        return false;
    }
    out.assign(size, 0);
    size_t src = 16;
    size_t dst = 0;
    while (dst < size) {
        if (src >= in.size()) {
            return false;
        }
        uint8_t code = in[src++];
        for (int bit = 0; bit < 8 && dst < size; ++bit, code = static_cast<uint8_t>(code << 1)) {
            if ((code & 0x80) != 0) {
                if (src >= in.size()) {
                    return false;
                }
                out[dst++] = in[src++];
                continue;
            }
            if (src + 1 >= in.size()) {
                return false;
            }
            const uint8_t b1 = in[src++];
            const uint8_t b2 = in[src++];
            const size_t distance = ((static_cast<size_t>(b1 & 0x0F) << 8) | b2) + 1;
            size_t count = b1 >> 4;
            if (count == 0) {
                if (src >= in.size()) {
                    return false;
                }
                count = static_cast<size_t>(in[src++]) + 0x12;
            } else {
                count += 2;
            }
            if (distance > dst) {
                return false;
            }
            for (size_t k = 0; k < count && dst < size; ++k, ++dst) {
                out[dst] = out[dst - distance];
            }
        }
    }
    return true;
}

bool is_model_node(uint32_t type) {
    // dRes_info_c::loadResource builds J3DModelData only for these folder types.
    switch (type) {
    case 'BMDP':
    case 'BMDR':
    case 'BMDV':
    case 'BMDE':
    case 'BMWR':
    case 'BMWE':
    case 'BMDG':
    case 'BMDA':
        return true;
    default:
        return false;
    }
}

// Returns the dRes file index of stamp.bmd (the index dComIfG_getObjectRes(arc, int) uses: the
// RARC file-entry index, see dRes_info_c::loadResource), or -1 with a reason.
int find_stamp_bmd_index(const std::vector<uint8_t>& arc, std::string& why) {
    if (arc.size() < 0x40 || std::memcmp(arc.data(), "RARC", 4) != 0) {
        why = "not a RARC archive";
        return -1;
    }
    uint32_t dataOffset = 0, numNodes = 0, nodeOffset = 0, numEntries = 0, entryOffset = 0,
             strSize = 0, strOffset = 0;
    if (!read_be32(arc, 0x0C, dataOffset) || !read_be32(arc, 0x20, numNodes) ||
        !read_be32(arc, 0x24, nodeOffset) || !read_be32(arc, 0x28, numEntries) ||
        !read_be32(arc, 0x2C, entryOffset) || !read_be32(arc, 0x30, strSize) ||
        !read_be32(arc, 0x34, strOffset))
    {
        why = "truncated header";
        return -1;
    }
    const uint64_t size = arc.size();
    const uint64_t dataBase = 0x20ull + dataOffset;
    const uint64_t nodes = 0x20ull + nodeOffset;
    const uint64_t entries = 0x20ull + entryOffset;
    const uint64_t strings = 0x20ull + strOffset;
    if (numNodes == 0 || numNodes > 0x1000 || numEntries == 0 || numEntries > 0x10000 ||
        nodes + numNodes * 0x10ull > size || entries + numEntries * 0x14ull > size ||
        strings + strSize > size || dataBase > size)
    {
        why = "header tables out of range";
        return -1;
    }
    for (uint32_t n = 0; n < numNodes; ++n) {
        const size_t node = static_cast<size_t>(nodes + n * 0x10ull);
        uint32_t type = 0, first = 0;
        uint16_t count = 0;
        read_be32(arc, node, type);
        read_be16(arc, node + 0x0A, count);
        read_be32(arc, node + 0x0C, first);
        for (uint32_t j = 0; j < count; ++j) {
            const uint64_t index = uint64_t{first} + j;
            if (index >= numEntries) {
                why = "directory entry out of range";
                return -1;
            }
            const size_t entry = static_cast<size_t>(entries + index * 0x14ull);
            uint32_t typeAndName = 0, fileOffset = 0, fileSize = 0;
            read_be32(arc, entry + 4, typeAndName);
            read_be32(arc, entry + 8, fileOffset);
            read_be32(arc, entry + 12, fileSize);
            if (((typeAndName >> 24) & 1u) == 0) {
                continue; // directory or placeholder entry (JKRArchive::isFileEntry)
            }
            const uint32_t nameOffset = typeAndName & 0xFFFFFFu;
            if (nameOffset >= strSize) {
                continue;
            }
            const char* name = reinterpret_cast<const char*>(arc.data() + strings + nameOffset);
            const char* nameEnd = std::find(name, name + (strSize - nameOffset), '\0');
            if (!iequals(std::string_view{name, static_cast<size_t>(nameEnd - name)}, kStampBmdName)) {
                continue;
            }
            if (!is_model_node(type)) {
                why = "stamp.bmd is not in a model folder";
                return -1;
            }
            const uint64_t fileStart = dataBase + fileOffset;
            if (fileSize < 0x20 || fileStart + fileSize > size || index > 0x7FFF) {
                why = "stamp.bmd lies outside the archive";
                return -1;
            }
            const char* magic = reinterpret_cast<const char*>(arc.data() + fileStart);
            if (std::memcmp(magic, "J3D2bmd3", 8) != 0 && std::memcmp(magic, "J3D2bdl4", 8) != 0) {
                why = "stamp.bmd is not a J3D model";
                return -1;
            }
            return static_cast<int>(index);
        }
    }
    why = "no stamp.bmd in the archive";
    return -1;
}

int validate_stamp_archive(std::string& why) {
    std::vector<uint8_t> raw;
    if (!read_dvd_file(kStampArcPath, raw, why)) {
        return -1;
    }
    if (std::memcmp(raw.data(), "Yaz0", 4) == 0) {
        std::vector<uint8_t> decoded;
        if (!yaz0_decode(raw, decoded)) {
            why = "bad Yaz0 data";
            return -1;
        }
        return find_stamp_bmd_index(decoded, why);
    }
    return find_stamp_bmd_index(raw, why);
}

// --- item table ---------------------------------------------------------------------------

bool is_our_entry(const dItem_itemResource& entry) {
    return entry.mArcName == g_stampArcName;
}

dItem_itemResource desired_entry(int index) {
    dItem_itemResource entry = g_original[index];
    if (g_modelFound) {
        // HD: {"Obj_Stamp", bmd 4, no btk/bck/brk/btp, tev -1, btp frame -1}. The bmd index is the
        // overlay archive's validated file-entry index (4 with the HD layout that
        // the converter keeps), re-checked by the CreateItemHeap hook.
        entry.mArcName = g_stampArcName;
        entry.mBmdName = g_bmdIndex;
        entry.mBtkName = -1;
        entry.mBckName = -1;
        entry.mBrkName = -1;
        entry.mBtpName = -1;
        entry.mTevFrm = -1;
        entry.mBtpFrm = -1;
    }
    // Without a valid archive the GC placeholder model (F_gD_rupy) stays: a wrong arc name would
    // fail the demo item's resource load and leave Link waiting in the get-item pose.
    entry.mTexture = kFallbackIconIndex;
    entry.mTexScale = 100; // 48x48 icons, same as GC's own item icons
    entry.field_0x14 = 0;
    return entry;
}

bool same_entry(const dItem_itemResource& a, const dItem_itemResource& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void write_tables() {
    for (int i = 0; i < kStampCount; ++i) {
        dItem_itemResource& live = dItem_data::item_resource[kStamps[i].item];
        if (g_tablesApplied && !same_entry(live, g_written[i])) {
            mods::log::warn("item_resource[{:#04x}] was changed by someone else; leaving it alone",
                kStamps[i].item);
            continue;
        }
        g_written[i] = desired_entry(i);
        live = g_written[i];
    }
}

void apply_tables() {
    if (g_tablesApplied) {
        write_tables();
        return;
    }
    if (!g_originalCaptured) {
        // Captured once per loaded image: a later re-activation must not mistake our own (or a
        // restored copy of our own) entries for the game's originals.
        for (int i = 0; i < kStampCount; ++i) {
            g_original[i] = dItem_data::item_resource[kStamps[i].item];
        }
        g_originalCaptured = true;
    }
    write_tables();
    g_tablesApplied = true;
    mods::log::info("stamp item table applied (model {}, bmd index {})",
        g_modelFound ? "Obj_Stamp" : "GC placeholder", g_bmdIndex);
}

void restore_tables() {
    if (!g_tablesApplied) {
        return;
    }
    for (int i = 0; i < kStampCount; ++i) {
        dItem_itemResource& live = dItem_data::item_resource[kStamps[i].item];
        if (same_entry(live, g_written[i])) {
            live = g_original[i];
        } else if (is_our_entry(live)) {
            // Someone changed other fields but our archive name is still in there: restore anyway,
            // so no entry keeps pointing at this mod's archive once it is off.
            mods::log::warn("item_resource[{:#04x}] changed since we patched it; restoring the original",
                kStamps[i].item);
            live = g_original[i];
        } else {
            mods::log::warn("item_resource[{:#04x}] changed since we patched it; not restoring",
                kStamps[i].item);
        }
    }
    g_tablesApplied = false;
    mods::log::info("stamp item table restored");
}

// --- stage edits (HD-only chests) --------------------------------------------------------

// res/stamps/stamps/hd_chests.bin, made by the mod from the player's own HD copy (port of
// tools/pipeline/stampbuild.py; one of the stored files, hdfiles/files.hpp): 'HDFC' u8 version=1 u8 count
// u16 0, then per chest u8 stamp index, s8 layer (-1 = TRES, n = TREn), u8 room, u8 0, u8 record[0x20] (HD's
// stage record of that chest).
constexpr char kHdChestsPath[] = "stamps/stamps/hd_chests.bin";
constexpr size_t kHdChestEntrySize = 4 + 0x20;

struct HdChestRecord {
    uint8_t index = 0;
    int8_t layer = -1;
    uint8_t room = 0;
    std::array<uint8_t, 0x20> record{};
};

std::vector<HdChestRecord> g_hdChests;
bool g_hdChestsRead = false;

void read_hd_chests() {
    if (g_hdChestsRead) {
        return;
    }
    g_hdChestsRead = true;
    g_hdChests.clear();
    std::vector<uint8_t> bytes;
    if (!hdfiles::load(kHdChestsPath, bytes)) {
        mods::log::warn("{}: the 7 HD-only stamp chests are not added", hdfiles::problem(kHdChestsPath));
        return;
    }
    const uint8_t* data = bytes.data();
    const size_t size = bytes.size();
    if (data != nullptr && size >= 8 && std::memcmp(data, "HDFC", 4) == 0 && data[4] == 1 &&
        size == 8 + size_t{data[5]} * kHdChestEntrySize)
    {
        for (size_t i = 0; i < data[5]; ++i) {
            const uint8_t* entry = data + 8 + i * kHdChestEntrySize;
            HdChestRecord rec;
            rec.index = entry[0];
            rec.layer = static_cast<int8_t>(entry[1]);
            rec.room = entry[2];
            std::memcpy(rec.record.data(), entry + 4, rec.record.size());
            g_hdChests.push_back(rec);
        }
    } else {
        mods::log::warn("res/{} is invalid ({} bytes); the HD-only stamp chests are not added", kHdChestsPath, size);
    }
}

// HD's record of this stamp's HD-only chest, checked against the table (stage room, layer, chest number,
// item); nullptr when res/ has none or it does not match.
const HdChestRecord* hd_chest_of(const StampDef& def) {
    for (const HdChestRecord& rec : g_hdChests) {
        if (rec.index != def.index) {
            continue;
        }
        if (rec.room != static_cast<uint8_t>(def.room) || rec.layer != def.layer ||
            !hd_chest_record_matches(def, rec.record.data()))
        {
            mods::log::warn("res/{}: the record for stamp {} does not match {} room {}; chest not added",
                kHdChestsPath, def.index, def.stage, def.room);
            return nullptr;
        }
        return &rec;
    }
    return nullptr;
}

void register_stage_edits() {
    if (!g_stageEdits.empty() || svc_stage == nullptr) {
        return;
    }
    read_hd_chests();
    for (const StampDef& def : kStamps) {
        if (def.source != StampSource::HdChest) {
            continue;
        }
        const HdChestRecord* chest = hd_chest_of(def);
        if (chest == nullptr) {
            if (!g_hdChests.empty()) {
                mods::log::warn("no HD chest record for stamp {}; its chest is not added", def.index);
            }
            continue;
        }
        // TRES records go through dStage_actorCreate like ACTR records (critic report (b)); a
        // non-layered TRES chunk is registered for every layer (-1), TREn for layer n.
        StageActorHandle handle = 0;
        const std::array<uint8_t, 0x20>& tres = chest->record;
        const ModResult result = svc_stage->add_actor(mod_ctx, def.stage,
            static_cast<uint8_t>(def.room), def.layer, tres.data(), tres.size(), &handle);
        if (result == MOD_OK) {
            g_stageEdits.push_back(handle);
        } else {
            mods::log::error("could not add the chest of stamp {} to {} room {} (result {})", def.index,
                def.stage, def.room, static_cast<int>(result));
        }
    }
    mods::log::info("added {} HD-only stamp chest(s)", g_stageEdits.size());
}

void remove_stage_edits() {
    for (StageActorHandle handle : g_stageEdits) {
        svc_stage->remove_actor_edit(mod_ctx, handle);
    }
    g_stageEdits.clear();
}

// --- chest resolvers -----------------------------------------------------------------------

bool resolve_chest(
    ModContext*, const ItemCheckInfo* info, ItemCheckResolution* outResult, void* userData) {
    const auto* def = static_cast<const StampDef*>(userData);
    // slot_ready(): without a usable blob store the stamp could not be recorded, so the chest
    // keeps GC's rupees instead of handing out an item that grant() would then refuse.
    if (def == nullptr || info == nullptr || outResult == nullptr || !slot_ready()) {
        return false;
    }
    const uint64_t bit = stamp_bit(def->index);
    if (info->vanilla_item != def->gcItem) {
        // Same stage and tboxNo but not the chest HD re-stocked: leave it alone.
        if ((g_warnedChest & bit) == 0) {
            g_warnedChest |= bit;
            mods::log::warn("{}: vanilla item {:#04x} != expected {:#04x}; not replacing", info->name,
                info->vanilla_item, def->gcItem);
        }
        return false;
    }
    if (info->giver_actor != nullptr) {
        const auto* chest = static_cast<const fopAc_ac_c*>(info->giver_actor);
        if (fopAcM_GetRoomNo(chest) != def->room) {
            if ((g_warnedChest & bit) == 0) {
                g_warnedChest |= bit;
                mods::log::warn("{}: chest is in room {}, expected {}; not replacing", info->name,
                    fopAcM_GetRoomNo(chest), def->room);
            }
            return false;
        }
    }
    if (is_owned(def->index)) {
        return false; // already have this stamp: keep GC's rupees
    }
    outResult->item = def->item;
    outResult->display_item = dItemNo_NONE_e;
    outResult->was_resolved = true;
    return true;
}

// HD's only non-stamp item change that follows from a stamp: the seeds of Happy Zelda's chest
// went to chest 29 next to it. Independent of the stamp collection, so only mod_active().
bool resolve_seed_move(
    ModContext*, const ItemCheckInfo* info, ItemCheckResolution* outResult, void*) {
    if (info == nullptr || outResult == nullptr || !mod_active() ||
        info->vanilla_item != dItemNo_GREEN_RUPEE_e)
    {
        return false;
    }
    if (info->giver_actor != nullptr &&
        fopAcM_GetRoomNo(static_cast<const fopAc_ac_c*>(info->giver_actor)) != kSeedMoveRoom)
    {
        return false;
    }
    outResult->item = dItemNo_PACHINKO_SHOT_e;
    outResult->display_item = dItemNo_NONE_e;
    outResult->was_resolved = true;
    return true;
}

void register_resolvers() {
    for (const StampDef& def : kStamps) {
        if (def.source != StampSource::Chest) {
            continue;
        }
        const std::string name = fmt::format("{}{}:{}", ITEM_CHECK_CHEST_PREFIX, def.stage, def.tboxNo);
        ItemCheckHandle handle = 0;
        const ModResult result = svc_item->set_check_resolver(
            mod_ctx, name.c_str(), resolve_chest, const_cast<StampDef*>(&def), &handle);
        if (result == MOD_OK) {
            g_resolvers.push_back(handle);
        } else {
            mods::log::error("could not register resolver {} (result {})", name, static_cast<int>(result));
        }
    }
    mods::log::info("registered {} chest resolver(s)", g_resolvers.size());

    ItemCheckHandle seedHandle = 0;
    if (svc_item->set_check_resolver(mod_ctx, kSeedMoveCheck, resolve_seed_move, nullptr, &seedHandle) ==
        MOD_OK)
    {
        g_resolvers.push_back(seedHandle);
    } else {
        mods::log::warn("could not register {}; Hyrule Castle keeps GC's green rupee there", kSeedMoveCheck);
    }
}

// --- non-chest triggers --------------------------------------------------------------------

bool starts_with(const char* text, std::string_view prefix) {
    return text != nullptr && std::strncmp(text, prefix.data(), prefix.size()) == 0;
}

bool in_room(const char* stage, int room) {
    const char* current = dComIfGp_getStartStageName();
    return current != nullptr && std::strcmp(current, stage) == 0 &&
           dComIfGp_roomControl_getStayNo() == room;
}

void request_give(int index, const char* why) {
    if (index < 0 || index >= kStampCount) {
        return;
    }
    const uint64_t bit = stamp_bit(index);
    if (is_owned(index) || ((g_pendingGive | g_queuedGive) & bit) != 0) {
        return;
    }
    g_pendingGive |= bit;
    mods::log::info("{}: stamp {} will be handed over", why, index);
}

void observe_give(ModContext*, const ItemGiveInfo* info, void*) {
    if (info == nullptr || !mod_active()) {
        return;
    }
    const int stamp = stamp_index_for_item(info->item);
    if (stamp >= 0) {
        g_pendingGive &= ~stamp_bit(stamp);
        g_queuedGive &= ~stamp_bit(stamp);
        grant(stamp, GrantKind::Game);
        return;
    }

    // Agitha: HD hands over her stamp in the talk that follows the reward for the 12th golden bug
    // (zel_04 flow 1900, node 1755). Dusklight names bug rewards "bug:<insect>" (d_a_npc_ins.cpp
    // talk()); the bug's "handed in" label is already set when the reward is given.
    if (starts_with(info->check_name, ITEM_CHECK_BUG_PREFIX)) {
        if (agitha_stamp_earned()) {
            request_give(kAgithaStampIndex, "golden bug reward");
        }
        return;
    }

    // Gengle: HD gives the Rupee stamp from the cat's reward talk (zel_04 flow 1145, node 2690)
    // once all 60 Poe souls are in. GC's cat (d_a_npc_ne.cpp message()) drops a silver rupee
    // there instead; it is a field item, so its give is always named "freestanding:R_SP160:<bit>".
    // Untagged silver rupees (Jovani's own talk() gift, other mods' queued gives) do not count.
    if (info->item == dItemNo_SILVER_RUPEE_e && starts_with(info->check_name, kCastleTownFieldItemCheck) &&
        in_room(kCastleTownInteriors, kJovaniRoom) && dComIfGs_getPohSpiritNum() >= kPoeSoulsForGengle)
    {
        request_give(kGengleStampIndex, "Gengle's reward");
        return;
    }

    // Malo Mart: HD sells the stamp in the Castle Town branch (TGSPITM type 0xBE + flow 21). GC
    // has no shop slot or flow for it, so the first purchase in that branch earns it instead.
    if (starts_with(info->check_name, kMaloMartShopCheck) && dComIfGs_isEventBit(kMaloMartOpenFlag))
    {
        request_give(kMaloMartStampIndex, "Malo Mart purchase");
    }
}

void dispatch_pending_gives() {
    if (g_pendingGive == 0 || !slot_ready()) {
        g_quietFrames = 0;
        return;
    }
    if (dComIfGp_event_runCheck()) {
        g_quietFrames = 0;
        return;
    }
    if (g_quietFrames < kAgithaDialogueFallbackFrames) {
        ++g_quietFrames;
    }
    if (g_quietFrames < kQuietFramesBeforeGive) {
        return;
    }
    for (int i = 0; i < kStampCount; ++i) {
        const uint64_t bit = stamp_bit(i);
        if ((g_pendingGive & bit) == 0) {
            continue;
        }
        if (i == kAgithaStampIndex && g_agithaGraph && g_quietFrames < kAgithaDialogueFallbackFrames) {
            continue; // her own talk hands it over (on_stamp_owned then clears the bit)
        }
        g_pendingGive &= ~bit;
        if (is_owned(i)) {
            continue;
        }
        // Plain give (no check name): the host waits until Link is idle, then runs the regular
        // get-item demo with this item number; execItemGet then reaches observe_give above.
        const ModResult result = svc_item->give_item(mod_ctx, nullptr, kStamps[i].item, 0);
        if (result == MOD_OK) {
            g_queuedGive |= bit;
        } else {
            mods::log::warn("give_item for stamp {} failed (result {}); granting silently", i,
                static_cast<int>(result));
            grant(i, GrantKind::Game);
        }
    }
    if (g_pendingGive == 0) {
        g_quietFrames = 0;
    }
}

// A queued give that never arrives (dropped by the host after repeated failed dispatches) would
// otherwise block that stamp until the slot changes, and Gengle/Malo Mart are not recovered on
// load. After a long quiet stretch the stamp is granted without its scene. If the host does show
// the scene later after all, grant() just finds the stamp already owned.
void expire_queued_gives() {
    if (g_queuedGive == 0 || !slot_ready()) {
        g_queuedQuietFrames = 0;
        return;
    }
    if (dComIfGp_event_runCheck()) {
        return; // a get-item scene (or any event) is running: not quiet
    }
    if (++g_queuedQuietFrames < kQueuedGiveTimeoutFrames) {
        return;
    }
    g_queuedQuietFrames = 0;
    const uint64_t expired = g_queuedGive;
    g_queuedGive = 0;
    for (int i = 0; i < kStampCount; ++i) {
        if ((expired & stamp_bit(i)) != 0 && !is_owned(i)) {
            mods::log::warn("stamp {}: its get-item scene never ran; granting it directly", i);
            grant(i, GrantKind::Game);
        }
    }
}

// --- get-item text -------------------------------------------------------------------------
//
// HD's own item-get text (zel_00 101 + item, res/stamps/hdtext) is used when it was extracted. HD's
// item-get messages have no "N of 50" count (HD shows its found/total count only on its
// Collection screen, which this mod's stamp book reproduces), so the count stays only in the own
// wording below, which is the fallback when res/stamps/hdtext/messages.bin is missing.

void put_bytes(std::vector<uint8_t>& out, std::initializer_list<uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_text(std::vector<uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

// JMessage control tags (same encoding as GC zel_00: 0x1A, size, group, u16 id, args).
void put_instant_on(std::vector<uint8_t>& out) {
    put_bytes(out, {0x1A, 0x05, 0x00, 0x00, 0x01});
}
void put_instant_off(std::vector<uint8_t>& out) {
    put_bytes(out, {0x1A, 0x05, 0x00, 0x00, 0x02});
}
void put_color(std::vector<uint8_t>& out, uint8_t color) {
    put_bytes(out, {0x1A, 0x06, 0xFF, 0x00, 0x00, color});
}

std::vector<uint8_t> build_get_text(const StampDef& def) {
    const int count = std::popcount(owned_mask() | stamp_bit(def.index));
    std::vector<uint8_t> text;
    text.reserve(160);
    put_instant_on(text);
    const std::string name = hdtext::stamp_name(def.index); // HD's name, or "Stamp N"
    switch (def.source) {
    case StampSource::Gengle:
        put_text(text, "Gengle the cat had a stamp for you: ");
        put_color(text, MESSAGE_COLOR_RED);
        put_text(text, name);
        put_color(text, MESSAGE_COLOR_DEFAULT);
        put_text(text, "!");
        break;
    case StampSource::Agitha:
        put_text(text, "Agitha shared a stamp from her collection: ");
        put_color(text, MESSAGE_COLOR_RED);
        put_text(text, name);
        put_color(text, MESSAGE_COLOR_DEFAULT);
        put_text(text, "!");
        break;
    case StampSource::MaloMart:
        put_text(text, "A thank-you stamp from Malo Mart: ");
        put_color(text, MESSAGE_COLOR_RED);
        put_text(text, name);
        put_color(text, MESSAGE_COLOR_DEFAULT);
        put_text(text, "!");
        break;
    case StampSource::Automatic:
        put_text(text, "Every other stamp is yours, so this one joins them: ");
        put_color(text, MESSAGE_COLOR_RED);
        put_text(text, name);
        put_color(text, MESSAGE_COLOR_DEFAULT);
        put_text(text, "!");
        break;
    default:
        put_text(text, "A new stamp: ");
        put_color(text, MESSAGE_COLOR_RED);
        put_text(text, name);
        put_color(text, MESSAGE_COLOR_DEFAULT);
        put_text(text, "!");
        break;
    }
    put_instant_off(text);
    put_text(text, fmt::format("\nYour stamp book now holds {} of {}.", count, kStampCount));
    text.push_back(0);
    return text;
}

// A callback (not a fixed override) for both HD and own text: the randomizer reuses these item
// ids for its own items, so the text must step aside (return false) whenever HD Stamps is
// inactive. GC's slots for 101 + stamp item are empty item-get entries that GC never shows
// (the stamp ids are NOENTRY items), so nothing of GC's is replaced.
bool message_override(
    ModContext*, const MessageOverrideContext* message, MessageTextData* outText, void* userData) {
    const auto* def = static_cast<const StampDef*>(userData);
    if (def == nullptr || message == nullptr || outText == nullptr || !mod_active()) {
        return false;
    }
    // The host copies the text before this returns; resolution happens before the item is
    // granted, so build_get_text() counts this stamp as owned already.
    thread_local std::vector<uint8_t> buffer;
    if (const hdtext::Message* hd = hdtext::find(hdtext::kGroupZel00, get_message_id(def->item));
        hd != nullptr)
    {
        buffer = hd->text; // validated when loaded (hdtext::valid_encoded_text)
    } else {
        buffer = build_get_text(*def);
    }
    outText->text = buffer.data();
    outText->text_size = buffer.size();
    return true;
}

void register_messages() {
    if (svc_message == nullptr) {
        return;
    }
    int failures = 0;
    int hdTexts = 0;
    for (const StampDef& def : kStamps) {
        hdTexts += hdtext::find(hdtext::kGroupZel00, get_message_id(def.item)) != nullptr ? 1 : 0;
        for (uint8_t language : kLanguages) {
            MessageOverrideHandle handle = 0;
            const ModResult result = svc_message->override_message_fn(mod_ctx, kGroupZel00,
                get_message_id(def.item), language, message_override, const_cast<StampDef*>(&def),
                &handle);
            if (result == MOD_OK) {
                g_messageOverrides.push_back(handle);
            } else {
                ++failures;
            }
        }
    }
    if (failures != 0) {
        mods::log::error("{} stamp message override(s) failed", failures);
    }
    mods::log::info("item-get text: {} stamp(s) use HD's text, {} this mod's own wording", hdTexts,
        kStampCount - hdTexts);
}

// --- what Gengle and the Malo Mart clerk say (HD zel_04 5466 / 5465) -----------------------------
//
// HD gives these two stamps through HD-only flows (zel_04 1145 and 21, res/stamps/hdtext/
// flows.json): Gengle says 5466 (animal-speech box) before the Rupee stamp, and
// the Malo Mart clerk says 5465 after the stamp is paid for. GC has neither flow, and Gengle's actor
// (d_a_npc_ne.cpp message()) only drops its silver rupee when its talk ENDS on GC message 6305 or
// 6306 (getNowMsgNo 0x18A1 / 0x18A2), so a flow patch that adds a node after them would cost the
// rupee. Instead, while the stamp is due, HD's line is appended to that same GC message as its
// own page (blank lines up to the 4-line page break): the box, the speaker and the message id
// stay GC's, so the rupee still drops. Showing the line is what earns the stamp; its get-item
// scene follows once the talk is over (dispatch_pending_gives). As in HD the line is said once:
// a talk in the short wait before that scene keeps GC's text (npc_line_due).
//
// GC ids (VERIFIED in the GC zel_04.bmg this game loads, cache/gc_extract):
// - 6305 / 6306: Gengle's reward talks (flow 1102 nodes 2143 / 2144 and their twins 2194 / 2195),
//   animal box (INF1 box kind 0x0E, the same box kind as HD's 5466).
// - 5430 / 5433: the clerk's line once a purchase passed the rupee check (every Castle Town Malo
//   Mart shop flow, R_SP160 room 0 layer 1 TGSPITM flows 1021-1059 and the bomb/arrow flows they
//   jump to; 5433 is the magic armor). Bombs and arrows end in a refill, not an item give, so this
//   line (not the shop give) is the first-purchase trigger whenever HD's text is present.
// Both ids are shared with other shops/rooms, hence the room checks.

struct NpcLine {
    uint16_t gcMessage; // zel_04 id the line is appended to
    uint16_t hdMessage; // zel_04 id of HD's line in res/stamps/hdtext
    int stamp;
};
constexpr std::array<NpcLine, 4> kNpcLines = {{
    {6305, hdtext::kMsgGengleGive, kGengleStampIndex},
    {6306, hdtext::kMsgGengleGive, kGengleStampIndex},
    {5430, hdtext::kMsgMaloMartThanks, kMaloMartStampIndex},
    {5433, hdtext::kMsgMaloMartThanks, kMaloMartStampIndex},
}};

bool gengle_line_place() {
    return in_room(kCastleTownInteriors, kJovaniRoom) &&
           dComIfGs_getPohSpiritNum() >= kPoeSoulsForGengle;
}

bool malo_mart_line_place() {
    return in_room(kCastleTownInteriors, kMaloMartRoom) && dComIfGs_isEventBit(kMaloMartOpenFlag);
}

// Whether HD's line for `stamp` goes into the GC message being resolved now. HD says it once:
// once a talk has shown it, the stamp is pending (or queued) until its get-item scene has run, and
// later talks in that window (a second Malo Mart purchase right after the first) keep GC's text.
bool npc_line_due(int stamp) {
    if (!slot_ready()) {
        return false;
    }
    const uint64_t bit = stamp_bit(stamp);
    if ((g_npcLineShown & bit) != 0) {
        return true; // the talk that first showed the line: same text for every pass
    }
    if (is_owned(stamp) || ((g_pendingGive | g_queuedGive) & bit) != 0) {
        return false;
    }
    return stamp == kGengleStampIndex ? gengle_line_place() : malo_mart_line_place();
}

void release_npc_lines() {
    if (g_npcLineShown != 0 && !dComIfGp_event_runCheck()) {
        g_npcLineShown = 0; // that talk is over
    }
}

bool npc_line_override(
    ModContext*, const MessageOverrideContext* message, MessageTextData* outText, void* userData) {
    const auto* line = static_cast<const NpcLine*>(userData);
    if (line == nullptr || message == nullptr || outText == nullptr ||
        message->group != hdtext::kGroupZel04)
    {
        return false;
    }
    if (!npc_line_due(line->stamp)) {
        return false; // GC's text, untouched
    }
    const hdtext::Message* hd = hdtext::find(hdtext::kGroupZel04, line->hdMessage);
    if (hd == nullptr) {
        return false;
    }
    thread_local std::vector<uint8_t> buffer;
    if (!hdtext::append_page(message->original_text, message->original_text_size, hd->text, buffer)) {
        return false;
    }
    g_npcLineShown |= stamp_bit(line->stamp);
    request_give(line->stamp,
        line->stamp == kGengleStampIndex ? "Gengle's stamp line" : "Malo Mart's stamp line");
    outText->text = buffer.data();
    outText->text_size = buffer.size();
    return true;
}

void register_npc_lines() {
    if (svc_message == nullptr || !hdtext::loaded()) {
        return; // own-wording mode: the give/shop triggers in observe_give stay the only path
    }
    int failures = 0;
    for (const NpcLine& line : kNpcLines) {
        if (hdtext::find(hdtext::kGroupZel04, line.hdMessage) == nullptr) {
            continue;
        }
        for (uint8_t language : kLanguages) {
            MessageOverrideHandle handle = 0;
            if (svc_message->override_message_fn(mod_ctx, hdtext::kGroupZel04, line.gcMessage,
                    language, npc_line_override, const_cast<NpcLine*>(&line), &handle) == MOD_OK)
            {
                g_messageOverrides.push_back(handle);
            } else {
                ++failures;
            }
        }
    }
    if (failures != 0) {
        mods::log::warn("{} Gengle/Malo Mart line override(s) failed", failures);
    }
}

// --- Agitha's stamp dialogue (HD zel_04 flow 1900 head) ---------------------------------------
//
// HD put a new head on Agitha's flow 1900 (res/stamps/hdtext/flows.json, VERIFIED from HD's
// bytes): when her stamp is due (HD: temp flag 18, set by the bug-reward flows 1813/1814 after an
// HD-only query) she says 5468, 5469 and 6138, then the flow ends on START_EVENT(1, 0xBD) and
// daNpcIns_c::talk hands the item over with its own get-item event, exactly like her bug rewards.
// Her actor re-opens flow 1900 right after every reward (talk(): mOrderSpeakEvent), so the lines
// follow the 12th bug's reward as in HD.
//
// The same chain is built here with FlowService on GC's graph: GC's start node of flow 1900
// becomes a branch on a mod query (12 golden bugs handed in and the stamp not owned yet, the same
// rule recovery uses); its "no" edge runs a copy of GC's original start node, so the rest of her
// talk is GC's. GC's start node is read from the zel_04.bmg on the disc through the game's own DVD
// layer (FLI1, last match wins like dMsgFlow_c::getInitNodeIndex), never hard-coded. Agitha is
// in R_SP160 room 3 with flow 1900 (VERIFIED, GC stage data), and R_SP160 uses message group 4.
// The three lines are registered as custom messages with HD's INF1 entries (speaker, camera and
// animation bytes), so no GC slot is touched.

constexpr uint16_t kAgithaFlowId = 1900;
// The game loads /res/<dusk::language::msg_folder()>/bmgres4.arc (d_msg_object.cpp
// readMessageGroupLocal), which is not exported. A GC US disc (GZ2E01) only has Msgus;
// PAL discs have one folder per language. The patch is only installed when every folder on the
// disc agrees on the start node, so it fits whichever language the game loads. The first two
// entries are the English folders; one of them must exist (HD's lines are English).
constexpr std::array<const char*, 6> kZel04ArchivePaths = {"/res/Msgus/bmgres4.arc",
    "/res/Msguk/bmgres4.arc", "/res/Msgde/bmgres4.arc", "/res/Msgfr/bmgres4.arc",
    "/res/Msgsp/bmgres4.arc", "/res/Msgit/bmgres4.arc"};
constexpr size_t kEnglishZel04Archives = 2;
constexpr char kZel04BmgName[] = "zel_04.bmg";
constexpr int kAgithaProbeAttempts = 30; // once a second while the disc's files are not readable

bool agitha_stamp_due() {
    return slot_ready() && !is_owned(kAgithaStampIndex) && agitha_stamp_earned();
}

// A permanent failure was logged: GC's flow stays. The mods panel's "Text:" line names the state of
// the patch, and it only redraws when the UI version changes.
void give_up_agitha_dialogue() {
    g_agithaGiveUp = true;
    ui::notify_state_changed();
}

uint16_t query_agitha_stamp_due(ModContext*, const FlowQueryContext* query, void*) {
    // Result 0: HD's stamp lines; 1: Agitha's usual talk (GC's original start node).
    if (query == nullptr || query->result_count < 2) {
        return 0;
    }
    return agitha_stamp_due() ? 0 : 1;
}

// Byte range of a file in an uncompressed RARC (any folder), by name.
bool find_arc_member(const std::vector<uint8_t>& arc, std::string_view wanted, size_t& outStart,
    size_t& outSize, std::string& why) {
    if (arc.size() < 0x40 || std::memcmp(arc.data(), "RARC", 4) != 0) {
        why = "not a RARC archive";
        return false;
    }
    uint32_t dataOffset = 0, numEntries = 0, entryOffset = 0, strSize = 0, strOffset = 0;
    if (!read_be32(arc, 0x0C, dataOffset) || !read_be32(arc, 0x28, numEntries) ||
        !read_be32(arc, 0x2C, entryOffset) || !read_be32(arc, 0x30, strSize) ||
        !read_be32(arc, 0x34, strOffset))
    {
        why = "truncated header";
        return false;
    }
    const uint64_t size = arc.size();
    const uint64_t dataBase = 0x20ull + dataOffset;
    const uint64_t entries = 0x20ull + entryOffset;
    const uint64_t strings = 0x20ull + strOffset;
    if (numEntries == 0 || numEntries > 0x10000 || entries + numEntries * 0x14ull > size ||
        strings + strSize > size || dataBase > size)
    {
        why = "header tables out of range";
        return false;
    }
    for (uint32_t index = 0; index < numEntries; ++index) {
        const size_t entry = static_cast<size_t>(entries + index * 0x14ull);
        uint32_t typeAndName = 0, fileOffset = 0, fileSize = 0;
        read_be32(arc, entry + 4, typeAndName);
        read_be32(arc, entry + 8, fileOffset);
        read_be32(arc, entry + 12, fileSize);
        if (((typeAndName >> 24) & 1u) == 0) {
            continue; // directory entry
        }
        const uint32_t nameOffset = typeAndName & 0xFFFFFFu;
        if (nameOffset >= strSize) {
            continue;
        }
        const char* name = reinterpret_cast<const char*>(arc.data() + strings + nameOffset);
        const char* nameEnd = std::find(name, name + (strSize - nameOffset), '\0');
        if (!iequals(std::string_view{name, static_cast<size_t>(nameEnd - name)}, wanted)) {
            continue;
        }
        const uint64_t fileStart = dataBase + fileOffset;
        if (fileStart + fileSize > size) {
            why = fmt::format("{} lies outside the archive", wanted);
            return false;
        }
        outStart = static_cast<size_t>(fileStart);
        outSize = fileSize;
        return true;
    }
    why = fmt::format("no {} in the archive", wanted);
    return false;
}

// Section of a BMG (svc/flow.cpp detail::find_section, with bounds checks). The returned size is
// clamped to the bytes present: GC's zel_04.bmg declares FLI1 16 bytes past the end of the file
// (archive padding), while its label table itself ends exactly at the end (VERIFIED).
bool find_bmg_section(const std::vector<uint8_t>& bmg, const char (&tag)[5], size_t& outOffset,
    size_t& outSize) {
    uint32_t count = 0;
    if (bmg.size() < 0x20 || std::memcmp(bmg.data(), "MESGbmg1", 8) != 0 ||
        !read_be32(bmg, 0x0C, count))
    {
        return false;
    }
    size_t offset = 0x20;
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        uint32_t sectionSize = 0;
        if (!read_be32(bmg, offset + 4, sectionSize) || sectionSize < 8) {
            return false;
        }
        const size_t available = bmg.size() - offset;
        if (std::memcmp(bmg.data() + offset, tag, 4) == 0) {
            outOffset = offset;
            outSize = std::min<size_t>(sectionSize, available);
            return true;
        }
        if (sectionSize >= available) {
            return false; // nothing follows inside the file
        }
        offset += sectionSize;
    }
    return false;
}

enum class ProbeResult { Found, NotYet, Failed };

// GC's start node of `flowId` in one bmgres4.arc. False with a reason.
bool read_flow_start_from(const char* path, uint16_t flowId, NativeFlowStart& out, std::string& why) {
    std::vector<uint8_t> raw;
    if (!read_dvd_file(path, raw, why)) {
        return false;
    }
    std::vector<uint8_t> arc;
    if (std::memcmp(raw.data(), "Yaz0", 4) == 0) {
        if (!yaz0_decode(raw, arc)) {
            why = "bad Yaz0 data";
            return false;
        }
    } else {
        arc = std::move(raw);
    }
    size_t start = 0;
    size_t size = 0;
    if (!find_arc_member(arc, kZel04BmgName, start, size, why)) {
        return false;
    }
    const std::vector<uint8_t> bmg(arc.begin() + static_cast<std::ptrdiff_t>(start),
        arc.begin() + static_cast<std::ptrdiff_t>(start + size));
    size_t fli = 0, fliSize = 0, flw = 0, flwSize = 0;
    if (!find_bmg_section(bmg, "FLI1", fli, fliSize) || !find_bmg_section(bmg, "FLW1", flw, flwSize)) {
        why = "zel_04.bmg has no FLI1/FLW1 section";
        return false;
    }
    uint16_t labelCount = 0;
    uint16_t nodeCount = 0;
    if (!read_be16(bmg, fli + 8, labelCount) || !read_be16(bmg, flw + 8, nodeCount) ||
        0x10ull + labelCount * 8ull > fliSize || 0x10ull + nodeCount * 8ull > flwSize)
    {
        why = "zel_04.bmg flow tables are out of range";
        return false;
    }
    int node = -1;
    for (uint16_t i = 0; i < labelCount; ++i) {
        uint32_t key = 0;
        uint16_t index = 0;
        read_be32(bmg, fli + 0x10 + i * 8u, key);
        read_be16(bmg, fli + 0x10 + i * 8u + 4, index);
        if ((key >> 16) == flowId) {
            node = index; // the last match wins, as in dMsgFlow_c::getInitNodeIndex
        }
    }
    if (node < 0 || node >= nodeCount) {
        why = fmt::format("flow {} has no start node in zel_04.bmg", flowId);
        return false;
    }
    out.node = static_cast<uint16_t>(node);
    std::memcpy(out.data.bytes, bmg.data() + flw + 0x10 + static_cast<size_t>(node) * 8, 8);
    if (out.data.bytes[0] < 1 || out.data.bytes[0] > 3) {
        why = fmt::format("flow {} starts on a node of unknown type {}", flowId, out.data.bytes[0]);
        return false;
    }
    return true;
}

std::string node_hex(const NativeFlowStart& start) {
    std::string hex;
    for (uint8_t byte : start.data.bytes) {
        hex += fmt::format("{:02x}", byte);
    }
    return hex;
}

// GC's start node of `flowId`, read from every message folder on the disc (kZel04ArchivePaths).
ProbeResult read_native_flow_start(uint16_t flowId, NativeFlowStart& out, std::string& why) {
    std::optional<NativeFlowStart> agreed;
    const char* agreedPath = nullptr;
    bool english = false;
    int folders = 0;
    for (size_t i = 0; i < kZel04ArchivePaths.size(); ++i) {
        const char* path = kZel04ArchivePaths[i];
        if (DVDConvertPathToEntrynum(path) < 0) {
            continue;
        }
        NativeFlowStart start;
        std::string reason;
        if (!read_flow_start_from(path, flowId, start, reason)) {
            why = fmt::format("{}: {}", path, reason);
            return ProbeResult::Failed;
        }
        if (agreed && (start.node != agreed->node ||
                          std::memcmp(start.data.bytes, agreed->data.bytes, sizeof(start.data.bytes)) != 0))
        {
            why = fmt::format("flow {} starts at node {} ({}) in {} but at node {} ({}) in {}", flowId,
                agreed->node, node_hex(*agreed), agreedPath, start.node, node_hex(start), path);
            return ProbeResult::Failed;
        }
        if (!agreed) {
            agreed = start;
            agreedPath = path;
        }
        english = english || i < kEnglishZel04Archives;
        ++folders;
    }
    if (!agreed || !english) {
        why = "no English zel_04 message archive (Msgus/Msguk bmgres4.arc) is readable";
        return ProbeResult::NotYet;
    }
    out = *agreed;
    mods::log::info("zel_04 flow {} starts at node {} ({}) in {} message folder(s), first {}", flowId,
        out.node, node_hex(out), folders, agreedPath);
    return ProbeResult::Found;
}

void install_agitha_dialogue() {
    if (g_agithaGraph || !g_agithaStart || !g_agithaQuery) {
        return;
    }
    if (g_agithaMessages.empty()) {
        // Registered once and kept until shutdown (only the graph follows the active state).
        for (uint16_t id : hdtext::kMsgAgithaLines) {
            const hdtext::Message* line = hdtext::find(hdtext::kGroupZel04, id);
            if (line == nullptr) {
                mods::log::warn("HD message zel_04:{} is missing from res/stamps/hdtext; Agitha hands over "
                                "her stamp without HD's lines",
                    id);
                g_agithaMessages.clear();
                give_up_agitha_dialogue();
                return;
            }
            std::vector<mods::flow::MessageVariant> variants;
            for (uint8_t language : kLanguages) {
                variants.emplace_back(static_cast<MessageLanguage>(language), line->entry, line->text);
            }
            mods::flow::RegisteredMessage registered =
                mods::flow::register_message(hdtext::kGroupZel04, variants);
            if (!registered) {
                mods::log::error("could not register HD message zel_04:{} (result {})", id,
                    static_cast<int>(registered.result()));
                g_agithaMessages.clear();
                give_up_agitha_dialogue();
                return;
            }
            g_agithaMessages.push_back(std::move(registered));
        }
    }

    mods::flow::GraphBuilder builder{hdtext::kGroupZel04};
    const mods::flow::NodeRef first = builder.add_message(g_agithaMessages[0].id());
    const mods::flow::NodeRef second = builder.add_message(g_agithaMessages[1].id());
    const mods::flow::NodeRef third = builder.add_message(g_agithaMessages[2].id());
    // HD node 1755: START_EVENT p0 = 1 (item get), p1 = the Agitha stamp item.
    const mods::flow::NodeRef give =
        builder.add_event(FLOW_EVENT_START_EVENT, {0x00, 0x01, 0x00, kAgithaStampItem});
    const mods::flow::NodeRef original = builder.add_node(g_agithaStart->data);
    first.next(second);
    second.next(third);
    third.next(give);
    give.next(mods::flow::kEnd);
    const std::array<uint16_t, 2> targets = {first.id(), original.id()};
    builder.patch_branch(g_agithaStart->node, g_agithaQuery.id(), 0, targets);
    g_agithaGraph = builder.commit();
    if (!g_agithaGraph) {
        mods::log::error("could not patch Agitha's flow {} (result {}); she hands over her stamp "
                         "without HD's lines",
            kAgithaFlowId, static_cast<int>(g_agithaGraph.result()));
        g_agithaGraph = mods::flow::Graph{};
        give_up_agitha_dialogue();
        return;
    }
    mods::log::info("Agitha's flow {} now opens with HD's stamp lines (node {} patched)",
        kAgithaFlowId, g_agithaStart->node);
    ui::notify_state_changed(); // the mods panel's "Text:" line
}

void remove_agitha_dialogue() {
    if (g_agithaGraph) {
        g_agithaGraph = mods::flow::Graph{}; // removes the custom nodes and the node patch
    }
}

void update_agitha_dialogue() {
    if (!mod_active() || g_agithaGraph || g_agithaGiveUp || !hdtext::loaded() || !g_agithaQuery) {
        return;
    }
    if (!g_agithaStart) {
        if (g_agithaProbeTimer > 0) {
            --g_agithaProbeTimer;
            return;
        }
        g_agithaProbeTimer = 60;
        NativeFlowStart start;
        std::string why;
        switch (read_native_flow_start(kAgithaFlowId, start, why)) {
        case ProbeResult::Found:
            g_agithaStart = start;
            break;
        case ProbeResult::NotYet:
            if (++g_agithaProbeCount >= kAgithaProbeAttempts) {
                mods::log::warn("{}; Agitha hands over her stamp without HD's lines", why);
                give_up_agitha_dialogue();
            }
            return;
        case ProbeResult::Failed:
            mods::log::error("reading GC's zel_04 flows failed ({}); Agitha hands over her stamp "
                             "without HD's lines",
                why);
            give_up_agitha_dialogue();
            return;
        }
    }
    install_agitha_dialogue();
}

// --- icons -----------------------------------------------------------------------------------

void load_icons() {
    g_iconCount = 0;
    for (int i = 0; i < kStampCount; ++i) {
        const std::string path = fmt::format("stamps/stamp_icons/stamp{:02}.bti", i);
        std::vector<uint8_t> bytes;
        if (!hdfiles::load(path, bytes)) {
            continue;
        }
        // Must be a complete BTI that fits the 0xC00-byte icon buffer.
        if (bytes.size() >= sizeof(ResTIMG) && bytes.size() <= kIconBufferSize) {
            g_icons[i] = std::move(bytes);
            ++g_iconCount;
        } else {
            mods::log::warn("{} has size {}; ignoring it", path, bytes.size());
        }
    }
    mods::log::info("{} custom stamp icon(s) loaded", g_iconCount);
}

// --- hooks -------------------------------------------------------------------------------------

HookAction on_get_item_func_pre(ModContext*, void* args, void*, void*) {
    const u8 item = mods::arg<u8>(args, 0);
    g_stampItemFuncItem = mod_active() && stamp_index_for_item(item) >= 0 ? item : -1;
    return HOOK_CONTINUE;
}

void on_get_item_func_post(ModContext*, void*, void*, void*) {
    g_stampItemFuncItem = -1;
}

HookAction on_item_first_bit_pre(ModContext*, void* args, void*, void*) {
    const u8 item = mods::arg<u8>(args, 0);
    if (!mod_active() || stamp_index_for_item(item) < 0) {
        return HOOK_CONTINUE;
    }
    if (g_getItemFuncHooked) {
        // Only the bit getItemFunc sets for the stamp being received.
        return g_stampItemFuncItem == item ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
    }
    // Fallback without the getItemFunc hook: never set first-get bits for stamp ids.
    return HOOK_SKIP_ORIGINAL;
}

int find_resource_index(void* wanted) {
    dRes_info_c* info = dComIfG_getObjectResInfo(kStampArc);
    JKRArchive* archive = info != nullptr ? info->getArchive() : nullptr;
    if (archive == nullptr) {
        return -1;
    }
    const u32 count = archive->countFile();
    for (u32 i = 0; i < count && i < 0x7FFF; ++i) {
        if (dComIfG_getObjectRes(kStampArc, static_cast<int>(i)) == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

HookAction on_create_item_heap_pre(ModContext*, void* args, void* retval, void*) {
    const char* arcName = mods::arg<const char*>(args, 1);
    if (arcName == nullptr || std::strcmp(arcName, kStampArc) != 0) {
        return HOOK_CONTINUE;
    }
    s16& bmd = mods::arg_ref<s16>(args, 2);
    void* wanted = dComIfG_getObjectRes(kStampArc, kStampBmdName);
    void* current = bmd >= 0 ? dComIfG_getObjectRes(kStampArc, static_cast<int>(bmd)) : nullptr;
    if (wanted != nullptr && current == wanted) {
        return HOOK_CONTINUE;
    }
    if (wanted != nullptr) {
        const int index = find_resource_index(wanted);
        if (index >= 0) {
            mods::log::warn("Obj_Stamp.arc: stamp.bmd is at index {}, not {}; using {}", index, bmd, index);
            bmd = static_cast<s16>(index);
            g_bmdIndex = static_cast<int16_t>(index);
            if (g_tablesApplied) {
                write_tables(); // same archive name, so the item's later DeleteBase is unaffected
            }
            return HOOK_CONTINUE;
        }
    }
    if (current != nullptr) {
        // The validated index still holds a resource (only the by-name lookup failed): it is the
        // model find_stamp_bmd_index() checked.
        return HOOK_CONTINUE;
    }
    // Not reachable with an archive that passed validation (a failed archive load fails the
    // item's resource phase before this point). Failing the heap avoids handing NULL to
    // mDoExt_J3DModel__create; the item table is switched back to the placeholder a moment later,
    // after this item's Delete has released "Obj_Stamp" under the name it was loaded with.
    mods::log::error("Obj_Stamp.arc has no usable stamp.bmd; switching stamps to the placeholder model");
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 0;
    }
    if (g_modelDisableTimer == 0) {
        g_modelDisableTimer = 30;
    }
    return HOOK_SKIP_ORIGINAL;
}

void on_read_item_texture_post(ModContext*, void* args, void*, void*) {
    if (!mod_active() || g_iconCount == 0) {
        return;
    }
    const int index = stamp_index_for_item(mods::arg<u8>(args, 0));
    if (index < 0 || g_icons[index].empty()) {
        return;
    }
    void* buffer = mods::arg<void*>(args, 1);
    const int explicitTexture = mods::arg<int>(args, 9);
    if (buffer == nullptr || explicitTexture >= 0) {
        return;
    }
    std::memcpy(buffer, g_icons[index].data(), std::min(g_icons[index].size(), kIconBufferSize));
    if (auto* picture = mods::arg<J2DPicture*>(args, 2); picture != nullptr) {
        picture->changeTexture(static_cast<const ResTIMG*>(buffer), 0);
    }
}

HookAction on_set_get_sub_bgm_pre(ModContext*, void* args, void*, void*) {
    if (mod_active()) {
        int& item = mods::arg_ref<int>(args, 1);
        if (item >= 0 && item <= 0xFF && stamp_index_for_item(static_cast<uint8_t>(item)) >= 0) {
            item = kFanfareDonorItem;
        }
    }
    return HOOK_CONTINUE;
}

template <class Entry>
bool install_pre(HookPreFn fn, const char* what) {
    const ModResult result = mods::hook::add_pre<Entry>(fn);
    if (result == MOD_OK) {
        ++g_hooksInstalled;
        return true;
    }
    mods::log::error("could not hook {} (result {}); continuing without it", what,
        static_cast<int>(result));
    return false;
}

template <class Entry>
bool install_post(HookPostFn fn, const char* what) {
    const ModResult result = mods::hook::add_post<Entry>(fn);
    if (result == MOD_OK) {
        ++g_hooksInstalled;
        return true;
    }
    mods::log::error("could not hook {} (result {}); continuing without it", what,
        static_cast<int>(result));
    return false;
}

void set_model_found(bool found, const char* why) {
    if (found == g_modelFound) {
        return;
    }
    g_modelFound = found;
    mods::log::info("Obj_Stamp.arc {}", why);
    if (g_tablesApplied) {
        write_tables();
    }
    ui::notify_state_changed();
}

void probe_model_archive() {
    if (g_modelFound || g_modelInvalid) {
        return;
    }
    // Static overlays are mounted at a frame boundary after load: until then the path does not
    // exist and the GC placeholder model stays.
    if (DVDConvertPathToEntrynum(kStampArcPath) < 0) {
        return;
    }
    std::string why;
    const int index = validate_stamp_archive(why);
    if (index < 0) {
        g_modelInvalid = true;
        mods::log::error("Obj_Stamp.arc is not usable ({}); stamps keep GC's placeholder model", why);
        ui::notify_state_changed();
        return;
    }
    if (index != g_bmdIndex) {
        mods::log::info("Obj_Stamp.arc: stamp.bmd is file {} (table default {})", index, g_bmdIndex);
    }
    g_bmdIndex = static_cast<int16_t>(index);
    set_model_found(true, "found and validated (stamp model enabled)");
}

} // namespace

ModResult initialize() {
    load_icons();
    hdtext::load(); // HD's own text; without it every text below keeps the own wording
    register_resolvers();
    register_messages();
    register_npc_lines();
    if (hdtext::loaded()) {
        if (svc_flow == nullptr) {
            mods::log::warn("FlowService is unavailable: Agitha hands over her stamp without HD's lines");
        } else {
            g_agithaQuery =
                mods::flow::register_query("HD Stamps: Agitha stamp due", query_agitha_stamp_due);
            if (!g_agithaQuery) {
                mods::log::error("could not register the Agitha flow query (result {})",
                    static_cast<int>(g_agithaQuery.result()));
            }
        }
    }
    if (svc_item->observe_gives(mod_ctx, observe_give, nullptr, &g_giveObserver) != MOD_OK) {
        mods::log::error("could not observe item gives: stamps cannot be collected in game");
        g_giveObserver = 0;
    }

    const bool itemFuncPre = install_pre<HdsGetItemFunc>(on_get_item_func_pre, "getItemFunc");
    const bool itemFuncPost = install_post<HdsGetItemFunc>(on_get_item_func_post, "getItemFunc (post)");
    g_getItemFuncHooked = itemFuncPre && itemFuncPost;
    install_pre<HdsOnItemFirstBit>(on_item_first_bit_pre, "dComIfGs_onItemFirstBit");
    install_pre<HdsCreateItemHeap>(on_create_item_heap_pre, "daItemBase_c::CreateItemHeap");
    install_post<HdsReadItemTexture>(on_read_item_texture_post, "dMeter2Info_readItemTexture");
    install_pre<HdsSetGetSubBgm>(on_set_get_sub_bgm_pre, "daAlink_c::setGetSubBgm");
    mods::log::info("{}/{} hooks installed", g_hooksInstalled, kHooksWanted);

    probe_model_archive();
    return MOD_OK;
}

void shutdown() {
    restore_tables();
    remove_stage_edits();
    g_hdChests.clear();
    g_hdChestsRead = false;
    remove_agitha_dialogue();
    g_agithaMessages.clear(); // RegisteredMessage removes each message
    g_agithaStart.reset();
    g_agithaGiveUp = false;
    g_agithaProbeTimer = 0;
    g_agithaProbeCount = 0;
    for (MessageOverrideHandle handle : g_messageOverrides) {
        svc_message->remove_override(mod_ctx, handle);
    }
    g_messageOverrides.clear();
    hdtext::unload();
    for (ItemCheckHandle handle : g_resolvers) {
        svc_item->clear_check_resolver(mod_ctx, handle);
    }
    g_resolvers.clear();
    if (g_giveObserver != 0) {
        svc_item->unobserve_gives(mod_ctx, g_giveObserver);
        g_giveObserver = 0;
    }
}

void update() {
    // Cheap FST lookup once a second until the converted archive shows up and has been checked.
    if (!g_modelFound && !g_modelInvalid && ++g_modelProbeTimer >= 60) {
        g_modelProbeTimer = 0;
        probe_model_archive();
    }
    if (g_modelDisableTimer > 0 && --g_modelDisableTimer == 0) {
        g_modelInvalid = true;
        set_model_found(false, "unusable at runtime (GC placeholder model)");
    }
    if (mod_active()) {
        update_agitha_dialogue(); // installs the flow patch once GC's zel_04 is readable
        release_npc_lines();
        dispatch_pending_gives();
        expire_queued_gives();
    }
}

void on_active_changed(bool active) {
    if (active) {
        apply_tables();
        register_stage_edits();
        // Agitha's flow patch is (re)installed by update() while the vanilla save is selected.
    } else {
        // Other game modes (the randomizer uses the same item ids for its own items): hand them back.
        restore_tables();
        remove_stage_edits();
        remove_agitha_dialogue(); // other modes may patch her flow themselves
        g_pendingGive = 0;
        g_queuedGive = 0;
        g_queuedQuietFrames = 0;
        g_npcLineShown = 0;
    }
}

void on_slot_changed() {
    // SaveService/ItemService drop queued gives when the slot changes.
    g_pendingGive = 0;
    g_queuedGive = 0;
    g_queuedQuietFrames = 0;
    g_npcLineShown = 0;
}

void on_stamp_owned(int index) {
    g_pendingGive &= ~stamp_bit(index);
    g_queuedGive &= ~stamp_bit(index);
}

bool model_archive_found() {
    return g_modelFound;
}

bool model_archive_invalid() {
    return g_modelInvalid;
}

bool gives_in_flight() {
    return mod_active() && (g_pendingGive | g_queuedGive) != 0;
}

bool hd_chest_stage_loaded() {
    if (g_stageEdits.empty() || dComIfGp_getLinkPlayer() == nullptr) {
        return false;  // no HD-only chest was added, or no stage is being played
    }
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr) {
        return false;
    }
    for (const StampDef& def : kStamps) {
        if (def.source == StampSource::HdChest && def.stage != nullptr && std::strcmp(stage, def.stage) == 0) {
            return true;
        }
    }
    return false;
}

bool tables_applied() {
    return g_tablesApplied;
}

int custom_icon_count() {
    return g_iconCount;
}

int hooks_installed() {
    return g_hooksInstalled;
}

int hooks_wanted() {
    return kHooksWanted;
}

bool hd_text_loaded() {
    return hdtext::loaded();
}

bool agitha_dialogue_active() {
    return static_cast<bool>(g_agithaGraph);
}

bool agitha_dialogue_unavailable() {
    return hdtext::loaded() && !g_agithaGraph && (g_agithaGiveUp || !g_agithaQuery);
}

} // namespace hdstamps::items
