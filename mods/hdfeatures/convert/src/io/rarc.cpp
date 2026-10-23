// RARC (JKRArchive) reading and writing (module C1).
//
// Ports tools/formats/rarc.py (Rarc, Member: header, node/entry tables, the node walk that skips '.'/'..'),
// tools/formats/rarc_write.py (name_hash, build, build_from_rarc: GC retail layout) and tools/pipeline/rarcx.py
// (_entry_paths, entry_index, rebuild_same_layout: the source's tables kept, only the file data replaced).
//
// Names are kept as the archive's raw bytes (Shift-JIS; all ASCII in practice). Python decodes them and
// encodes them again when writing, which gives the same bytes for valid Shift-JIS.
// Every table read is bounds-checked (Correction #1): a bad archive gives Error{"fmt.io"}, never a crash.
// Memory is bounded too (fix-wave1 L2): names are at most 255 bytes, the walk at most 64 folders deep, at most
// one member per entry, and all member paths together at most 16 MiB. Python has none of these limits; no real
// archive comes near them (85 input + 57 oracle archives, VERIFIED fix-wave1: longest name 55 B, 2 folders deep,
// at most 229 members, at most 7,393 path bytes per archive), so they only change what a damaged archive gives.
#include "io_internal.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>

namespace hdf::io {

namespace {

Error bad(std::string what) { return Error{"fmt.io", "RARC: " + std::move(what)}; }

constexpr std::size_t kMaxName = 255;              // bytes per node / entry name
constexpr std::size_t kMaxDepth = 64;              // folders below the root
constexpr u64 kMaxPathBytes = u64(16) << 20;       // all member paths and folder prefixes of one archive

// Python: `x.split("/", 1)[1]` for a member path (root name prefix removed)
std::string_view after_first_slash(std::string_view p) {
    const std::size_t i = p.find('/');
    return i == std::string_view::npos ? std::string_view{} : p.substr(i + 1);
}

std::string_view last_component(std::string_view p) {
    const std::size_t i = p.rfind('/');
    return i == std::string_view::npos ? p : p.substr(i + 1);
}

// rarc_write: typ = (nm.upper() + "    ")[:4].encode("ascii", "replace"). ASCII letters are upper-cased; a
// non-ASCII character (one or two Shift-JIS bytes) becomes one '?'.
std::string node_type(std::string_view nm) {
    std::string t;
    std::size_t i = 0;
    while (i < nm.size() && t.size() < 4) {
        const u8 c = static_cast<u8>(nm[i]);
        if (c < 0x80) {
            t += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
            ++i;
        } else {
            t += '?';
            const bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
            i += (lead && i + 1 < nm.size()) ? 2 : 1;
        }
    }
    t.append(4 - t.size(), ' ');
    return t;
}

u64 align32(u64 v) { return (v + 31) / 32 * 32; }

// rarcx._entry_paths: entry index -> member path for every FILE entry the node walk reaches (dict order: the
// first visit fixes the position, a later visit of the same index replaces the path). Uses the same walk as
// rarc_read, so it can't fail on an archive rarc_read accepted; indices out of range are skipped.
std::vector<std::pair<u32, std::string>> entry_paths(const Rarc& arc) {
    std::vector<std::pair<u32, std::string>> out;
    std::unordered_map<u32, std::size_t> pos;
    if (arc.nodes.empty()) return out;
    std::vector<char> seen(arc.nodes.size(), 0);
    struct Frame {
        u32 node;
        u32 k;
        std::string prefix;
    };
    std::vector<Frame> stack;
    seen[0] = 1;
    stack.push_back(Frame{0, 0, arc.nodes[0].name});
    while (!stack.empty()) {
        const std::size_t top = stack.size() - 1;
        const RarcNode& n = arc.nodes[stack[top].node];
        if (stack[top].k >= n.count) {
            stack.pop_back();
            continue;
        }
        const u64 ei = static_cast<u64>(n.first) + stack[top].k++;
        if (ei >= arc.entries.size()) continue;
        const RarcEntry& e = arc.entries[static_cast<std::size_t>(ei)];
        if (e.name == "." || e.name == "..") continue;
        if (e.flags & kRarcDir) {
            // the same depth limit as rarc_read (a hand-made Rarc may not have passed it)
            if (e.off < arc.nodes.size() && !seen[e.off] && stack.size() <= kMaxDepth) {
                seen[e.off] = 1;
                std::string prefix = stack[top].prefix + "/" + e.name;
                stack.push_back(Frame{e.off, 0, std::move(prefix)});
            }
        } else if (e.flags & kRarcFile) {
            std::string path = stack[top].prefix + "/" + e.name;
            const u32 idx = static_cast<u32>(ei);
            if (auto it = pos.find(idx); it != pos.end()) {
                out[it->second].second = std::move(path);
            } else {
                pos.emplace(idx, out.size());
                out.emplace_back(idx, std::move(path));
            }
        }
    }
    return out;
}

const Bytes* find_replace(const Replace& replace, std::string_view path, std::string_view rel) {
    if (auto it = replace.find(path); it != replace.end()) return &it->second;
    if (auto it = replace.find(rel); it != replace.end()) return &it->second;
    return nullptr;
}

struct BuildEntry {
    u16 id;
    u16 hash;
    u8 flags;
    u32 name_off;
    u32 off;
    u32 size;
    bool file = false;  // Python: id None, set to the entry index afterwards
};

struct BuildNode {
    std::string type;
    u32 name_off;
    u16 hash;
    u16 count;
    u32 first;
};

// rarc_write.build with its limits checked (Python's struct.pack raises when a value doesn't fit).
Result<Bytes> build_impl(std::string_view root_name, std::span<const std::pair<std::string, View>> entries) {
    enum Kind { kDirItem, kFileItem };
    struct Item {
        Kind kind;
        std::string name;
    };
    // directory tree: dir path -> ordered (kind, name)
    std::map<std::string, std::vector<Item>, std::less<>> dirs;
    dirs[""];
    std::map<std::string, std::size_t, std::less<>> data_of;  // stripped path -> entries index (last wins)
    auto strip = [](std::string_view p) {
        while (!p.empty() && p.front() == '/') p.remove_prefix(1);
        while (!p.empty() && p.back() == '/') p.remove_suffix(1);
        return p;
    };
    for (std::size_t idx = 0; idx < entries.size(); ++idx) {
        const std::string_view path = strip(entries[idx].first);
        std::vector<std::string_view> parts;
        std::size_t s = 0;
        for (;;) {
            const std::size_t j = path.find('/', s);
            if (j == std::string_view::npos) {
                parts.push_back(path.substr(s));
                break;
            }
            parts.push_back(path.substr(s, j - s));
            s = j + 1;
        }
        std::string cur;
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
            std::string nxt = cur.empty() ? std::string(parts[k]) : cur + "/" + std::string(parts[k]);
            if (dirs.find(nxt) == dirs.end()) {
                dirs[nxt];
                dirs[cur].push_back(Item{kDirItem, std::string(parts[k])});
            }
            cur = std::move(nxt);
        }
        dirs[cur].push_back(Item{kFileItem, std::string(parts.back())});
        data_of[std::string(path)] = idx;
    }
    // BFS node order like retail archives
    std::vector<std::string> node_order{""};
    std::deque<std::string> q{""};
    while (!q.empty()) {
        const std::string d = q.front();
        q.pop_front();
        for (const Item& it : dirs[d]) {
            if (it.kind != kDirItem) continue;
            std::string sub = d.empty() ? it.name : d + "/" + it.name;
            node_order.push_back(sub);
            q.push_back(std::move(sub));
        }
    }
    std::map<std::string, u32, std::less<>> node_index;
    for (std::size_t i = 0; i < node_order.size(); ++i) node_index[node_order[i]] = static_cast<u32>(i);
    // strings: ".\0..\0", then per node (BFS): the node name, then the names of its files
    Bytes strtab{'.', 0, '.', '.', 0};
    std::map<std::string, u32, std::less<>> str_off{{".", 0u}, {"..", 2u}};
    auto sid = [&](std::string_view s) -> u32 {
        auto it = str_off.find(s);
        if (it != str_off.end()) return it->second;
        const u32 o = static_cast<u32>(strtab.size());
        str_off.emplace(std::string(s), o);
        strtab.insert(strtab.end(), s.begin(), s.end());
        strtab.push_back(0);
        return o;
    };
    for (const std::string& d : node_order) {
        sid(d.empty() ? root_name : last_component(d));
        for (const Item& it : dirs[d])
            if (it.kind == kFileItem) sid(it.name);
    }
    std::vector<BuildNode> nodes;
    std::vector<BuildEntry> ents;
    std::vector<std::pair<u64, View>> file_blobs;
    u64 data_cursor = 0;
    constexpr u32 kNone = 0xFFFFFFFFu;
    for (const std::string& d : node_order) {
        const std::string_view nm = d.empty() ? root_name : last_component(d);
        const std::size_t first = ents.size();
        for (const Item& it : dirs[d]) {
            if (it.kind == kDirItem) {
                const std::string sub = d.empty() ? it.name : d + "/" + it.name;
                ents.push_back(
                    BuildEntry{0xFFFF, rarc_name_hash(it.name), kRarcDir, sid(it.name), node_index[sub], 0x10});
            } else {
                const std::string key = d.empty() ? it.name : d + "/" + it.name;
                auto f = data_of.find(key);
                if (f == data_of.end()) return fail("fmt.io", "RARC build: no data for " + key);
                const View blob = entries[f->second].second;
                if (data_cursor > 0xFFFFFFFFull || blob.size() > 0xFFFFFFFFull)
                    return fail("fmt.io", "RARC build: archive larger than 4 GB");
                ents.push_back(BuildEntry{0, rarc_name_hash(it.name), static_cast<u8>(kRarcFile | kRarcMram),
                                          sid(it.name), static_cast<u32>(data_cursor), static_cast<u32>(blob.size()),
                                          true});
                file_blobs.emplace_back(data_cursor, blob);
                data_cursor = align32(data_cursor + blob.size());
            }
        }
        ents.push_back(BuildEntry{0xFFFF, rarc_name_hash("."), kRarcDir, 0, node_index[d], 0x10});
        u32 parent = kNone;
        if (!d.empty()) {
            const std::size_t sl = d.rfind('/');
            parent = node_index[sl == std::string::npos ? std::string() : d.substr(0, sl)];
        }
        ents.push_back(BuildEntry{0xFFFF, rarc_name_hash(".."), kRarcDir, 2, parent, 0x10});
        const std::size_t count = ents.size() - first;
        if (count > 0xFFFF) return fail("fmt.io", "RARC build: more than 65535 entries in one folder");
        nodes.push_back(BuildNode{d.empty() ? std::string("ROOT") : node_type(nm), sid(nm), rarc_name_hash(nm),
                                  static_cast<u16>(count), static_cast<u32>(first)});
    }
    if (ents.size() > 0xFFFF) return fail("fmt.io", "RARC build: more than 65535 entries");
    for (std::size_t i = 0; i < ents.size(); ++i)
        if (ents[i].file) ents[i].id = static_cast<u16>(i);
    const u64 node_off = 0x20;
    const u64 ent_off = align32(0x20 + nodes.size() * 0x10);
    const u64 str_rel = align32(ent_off + ents.size() * 0x14);
    const u64 str_len = align32(strtab.size());
    const u64 data_rel = align32(str_rel + str_len);
    const u64 data_len = file_blobs.empty() ? 0 : align32(data_cursor);
    const u64 total = 0x20 + data_rel + data_len;
    if (total > 0xFFFFFFFFull || strtab.size() > 0xFFFFFFull)
        return fail("fmt.io", "RARC build: archive larger than 4 GB");
    Bytes out(static_cast<std::size_t>(total), 0);
    std::memcpy(out.data(), "RARC", 4);
    put32(out, 4, static_cast<u32>(total));
    put32(out, 8, 0x20);
    put32(out, 12, static_cast<u32>(data_rel));
    put32(out, 16, static_cast<u32>(data_len));
    put32(out, 20, static_cast<u32>(data_len));
    put32(out, 0x20, static_cast<u32>(nodes.size()));
    put32(out, 0x24, static_cast<u32>(node_off));
    put32(out, 0x28, static_cast<u32>(ents.size()));
    put32(out, 0x2C, static_cast<u32>(ent_off));
    put32(out, 0x30, static_cast<u32>(str_len));
    put32(out, 0x34, static_cast<u32>(str_rel));
    put16(out, 0x38, static_cast<u16>(ents.size()));
    out[0x3A] = 1;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::size_t o = static_cast<std::size_t>(0x20 + node_off + i * 0x10);
        std::memcpy(out.data() + o, nodes[i].type.data(), 4);
        put32(out, o + 4, nodes[i].name_off);
        put16(out, o + 8, nodes[i].hash);
        put16(out, o + 10, nodes[i].count);
        put32(out, o + 12, nodes[i].first);
    }
    for (std::size_t i = 0; i < ents.size(); ++i) {
        const BuildEntry& e = ents[i];
        const std::size_t o = static_cast<std::size_t>(0x20 + ent_off + i * 0x14);
        put16(out, o, e.id);
        put16(out, o + 2, e.hash);
        put32(out, o + 4, (static_cast<u32>(e.flags) << 24) | e.name_off);
        put32(out, o + 8, e.off);
        put32(out, o + 12, e.size);
        put32(out, o + 16, 0);
    }
    std::memcpy(out.data() + 0x20 + str_rel, strtab.data(), strtab.size());
    const std::size_t base = static_cast<std::size_t>(0x20 + data_rel);
    for (const auto& [off, blob] : file_blobs)
        if (!blob.empty()) std::memcpy(out.data() + base + off, blob.data(), blob.size());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- reading
View Rarc::raw(const RarcMember& m) const {
    const u64 b0 = std::min<u64>(buf.size(), static_cast<u64>(data_base) + m.offset);
    const u64 b1 = std::min<u64>(buf.size(), b0 + m.size);  // Python slices clamp
    return View(buf.data() + b0, static_cast<std::size_t>(b1 - b0));
}

Result<Bytes> Rarc::data(const RarcMember& m) const { return maybe_yaz0(raw(m)); }

const RarcMember* Rarc::get(std::string_view key) const {
    const std::string k = ascii_lower(key);
    const std::string slash_k = "/" + k;
    for (const RarcMember& m : members) {
        const std::string p = ascii_lower(m.path);
        if (p == k || ends_with(p, slash_k)) return &m;
    }
    for (const RarcMember& m : members)
        if (ascii_lower(m.name) == k) return &m;
    return nullptr;
}

std::string_view Rarc::root_name() const { return nodes.empty() ? std::string_view{} : nodes[0].name; }

Result<Rarc> rarc_read(View in) {
    Rarc a;
    a.was_compressed =
        in.size() >= 4 && (std::memcmp(in.data(), "Yaz0", 4) == 0 || std::memcmp(in.data(), "Yay0", 4) == 0);
    auto dec = maybe_yaz0(in);
    if (!dec) return std::unexpected(dec.error());
    a.buf = std::move(*dec);
    const View b = a.buf;
    const u64 size = b.size();
    if (size < 4 || std::memcmp(b.data(), "RARC", 4) != 0) return std::unexpected(bad("not a RARC archive"));
    if (size < 32) return std::unexpected(bad("header truncated"));
    a.file_size = be32(b, 4);
    a.hdr_size = be32(b, 8);
    const u32 data_off = be32(b, 12);
    a.data_len = be32(b, 16);
    a.mram = be32(b, 20);
    a.aram = be32(b, 24);
    a.dvd = be32(b, 28);
    const u64 ib = a.hdr_size;
    if (ib + 0x1B > size) return std::unexpected(bad("info block out of range"));
    a.num_nodes = be32(b, ib);
    const u32 node_off = be32(b, ib + 4);
    a.num_entries = be32(b, ib + 8);
    const u32 entry_off = be32(b, ib + 12);
    a.str_len = be32(b, ib + 16);
    const u32 str_off = be32(b, ib + 20);
    a.next_id = be16(b, ib + 0x18);
    a.sync = b[ib + 0x1A];
    // absolute offsets; values past the end are clamped (they can only be used for range checks or empty
    // slices, as in Python)
    auto clampu = [&](u64 v) { return static_cast<u32>(std::min(v, size)); };
    const u64 node_base = ib + node_off, entry_base = ib + entry_off;
    const u64 str_base = ib + str_off, data_base = ib + data_off;
    a.node_base = clampu(node_base);
    a.entry_base = clampu(entry_base);
    a.str_base = clampu(str_base);
    a.data_base = clampu(data_base);
    if (node_base + static_cast<u64>(a.num_nodes) * 0x10 > size) return std::unexpected(bad("node table out of range"));
    if (entry_base + static_cast<u64>(a.num_entries) * 0x14 > size)
        return std::unexpected(bad("entry table out of range"));
    auto str = [&](u64 off, std::string& out) -> bool {
        const u64 p = str_base + off;
        if (p >= size) return false;
        // at most kMaxName bytes before the NUL (fix-wave1 L2: many entries naming one huge "name" would
        // otherwise copy it once per entry)
        const std::size_t span = static_cast<std::size_t>(std::min<u64>(size - p, kMaxName + 1));
        const void* z = std::memchr(b.data() + p, 0, span);
        if (!z) return false;
        out.assign(reinterpret_cast<const char*>(b.data() + p), static_cast<const u8*>(z) - (b.data() + p));
        return true;
    };
    a.nodes.resize(a.num_nodes);
    for (u32 i = 0; i < a.num_nodes; ++i) {
        const std::size_t o = static_cast<std::size_t>(node_base + static_cast<u64>(i) * 0x10);
        RarcNode& n = a.nodes[i];
        n.type.assign(reinterpret_cast<const char*>(b.data() + o), 4);
        if (!str(be32(b, o + 4), n.name)) return std::unexpected(bad("node name out of range or too long"));
        n.hash = be16(b, o + 8);
        n.count = be16(b, o + 10);
        n.first = be32(b, o + 12);
    }
    a.entries.resize(a.num_entries);
    for (u32 i = 0; i < a.num_entries; ++i) {
        const std::size_t o = static_cast<std::size_t>(entry_base + static_cast<u64>(i) * 0x14);
        RarcEntry& e = a.entries[i];
        e.id = be16(b, o);
        e.hash = be16(b, o + 2);
        const u32 fl_name = be32(b, o + 4);
        e.flags = static_cast<u8>(fl_name >> 24);
        if (!str(fl_name & 0xFFFFFF, e.name)) return std::unexpected(bad("entry name out of range or too long"));
        e.off = be32(b, o + 8);
        e.size = be32(b, o + 12);
    }
    // rarc.Rarc._walk(0, root name): depth first, entries in table order, '.'/'..' skipped, each node once.
    if (!a.nodes.empty()) {
        std::vector<char> seen(a.nodes.size(), 0);
        struct Frame {
            u32 node;
            u32 k;
            std::string prefix;
        };
        std::vector<Frame> stack;
        seen[0] = 1;
        stack.push_back(Frame{0, 0, a.nodes[0].name});
        u64 path_bytes = 0;
        while (!stack.empty()) {
            const std::size_t top = stack.size() - 1;
            const RarcNode& n = a.nodes[stack[top].node];
            if (stack[top].k >= n.count) {
                stack.pop_back();
                continue;
            }
            const u64 ei = static_cast<u64>(n.first) + stack[top].k++;
            if (ei >= a.entries.size()) return std::unexpected(bad("node entries out of range"));
            const RarcEntry& e = a.entries[static_cast<std::size_t>(ei)];
            if (e.name == "." || e.name == "..") continue;
            path_bytes += stack[top].prefix.size() + 1 + e.name.size();
            if (path_bytes > kMaxPathBytes) return std::unexpected(bad("member paths too large"));
            if (e.flags & kRarcDir) {
                if (e.off < a.nodes.size() && !seen[e.off]) {
                    if (stack.size() > kMaxDepth) return std::unexpected(bad("folders nested too deep"));
                    seen[e.off] = 1;
                    std::string prefix = stack[top].prefix + "/" + e.name;
                    stack.push_back(Frame{e.off, 0, std::move(prefix)});
                }
            } else {
                // each entry is listed by one node in a real archive: more members than entries means node
                // ranges overlap, which could otherwise multiply the member list (fix-wave1 L2)
                if (a.members.size() >= a.entries.size()) return std::unexpected(bad("more members than entries"));
                RarcMember m;
                m.path = stack[top].prefix + "/" + e.name;
                m.name = e.name;
                m.flags = e.flags;
                m.offset = e.off;
                m.size = e.size;
                m.id = e.id;
                m.entry = static_cast<u32>(ei);
                a.members.push_back(std::move(m));
            }
        }
    }
    return a;
}

std::optional<u32> rarc_entry_index(const Rarc& arc, std::string_view member_path) {
    const std::string low = ascii_lower(member_path);
    const std::string slash_low = "/" + low;
    for (const auto& [ei, p] : entry_paths(arc)) {
        const std::string pl = ascii_lower(p);
        if (pl == low || ends_with(pl, slash_low)) return ei;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------- writing
u16 rarc_name_hash(std::string_view name) {
    u32 h = 0;
    for (const char c : name) h = (h * 3 + static_cast<u8>(c)) & 0xFFFF;
    return static_cast<u16>(h);
}

Bytes rarc_build(std::string_view root_name, std::span<const std::pair<std::string, Bytes>> entries) {
    std::vector<std::pair<std::string, View>> views;
    views.reserve(entries.size());
    for (const auto& [p, d] : entries) views.emplace_back(p, View(d));
    auto r = build_impl(root_name, views);
    return r ? std::move(*r) : Bytes{};
}

// rarc_write.build_from_rarc: every member (walk order) under its path below the root, with replace[path] or
// replace[rel] or its data (Yaz0 members decoded), written in the GC retail layout.
Result<Bytes> rarc_write(const Rarc& arc, const Replace& replace) {
    if (arc.nodes.empty()) return std::unexpected(bad("no root node"));
    const std::string_view root = arc.nodes[0].name;
    std::vector<Bytes> decoded;
    decoded.reserve(arc.members.size());
    std::vector<std::pair<std::string, View>> entries;
    entries.reserve(arc.members.size());
    for (const RarcMember& m : arc.members) {
        const std::string_view rel = after_first_slash(m.path);
        if (const Bytes* r = find_replace(replace, m.path, rel)) {
            entries.emplace_back(std::string(rel), View(*r));
            continue;
        }
        auto d = arc.data(m);
        if (!d) return std::unexpected(d.error());
        decoded.push_back(std::move(*d));
        entries.emplace_back(std::string(rel), View(decoded.back()));
    }
    return build_impl(root, entries);
}

// rarcx.rebuild_same_layout: header + tables copied as they are (same entry indices, ids, names, order); file
// data re-laid in entry order, 32-byte aligned, stored expanded (COMPRESSED/YAZ0 cleared, FILE|MRAM set).
Result<Bytes> rarc_rebuild_same_layout(const Rarc& arc, const Replace& replace) {
    const View buf = arc.buf;
    if (buf.size() < 32) return std::unexpected(bad("header truncated"));
    const u64 ib = arc.hdr_size;
    const u32 data_off_rel = be32(buf, 0xC);
    const u64 head_len = std::min<u64>(buf.size(), ib + data_off_rel);  // Python slice clamps
    Bytes head(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(head_len));
    std::unordered_map<std::string, std::size_t> first_member;
    for (std::size_t i = 0; i < arc.members.size(); ++i) first_member.emplace(arc.members[i].path, i);
    auto paths = entry_paths(arc);
    std::stable_sort(paths.begin(), paths.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    Bytes data;
    for (const auto& [ei, path] : paths) {
        const std::string_view rel = after_first_slash(path);
        const Bytes* blob = find_replace(replace, path, rel);
        Bytes own;
        if (!blob) {
            auto it = first_member.find(path);
            if (it == first_member.end()) return std::unexpected(bad("member not found: " + path));
            auto d = arc.data(arc.members[it->second]);
            if (!d) return std::unexpected(d.error());
            own = std::move(*d);
            blob = &own;
        }
        data.resize(align32(data.size()), 0);
        const u64 off = data.size();
        data.insert(data.end(), blob->begin(), blob->end());
        if (off > 0xFFFFFFFFull || blob->size() > 0xFFFFFFFFull)
            return std::unexpected(bad("archive larger than 4 GB"));
        const u64 eo = static_cast<u64>(arc.entry_base) + static_cast<u64>(ei) * 0x14;
        if (eo + 0x14 > head.size()) return std::unexpected(bad("entry table outside the header block"));
        const std::size_t o = static_cast<std::size_t>(eo);
        const u32 fl_name = be32(head, o + 4);
        u32 flags = (fl_name >> 24) & 0xFF;
        flags = (flags & ~0x84u) | 0x11u;
        put32(head, o + 4, (flags << 24) | (fl_name & 0xFFFFFF));
        put32(head, o + 8, static_cast<u32>(off));
        put32(head, o + 12, static_cast<u32>(blob->size()));
        put32(head, o + 16, 0);
    }
    data.resize(align32(data.size()), 0);
    Bytes out = std::move(head);
    out.insert(out.end(), data.begin(), data.end());
    if (out.size() < 32) return std::unexpected(bad("header truncated"));
    if (out.size() > 0xFFFFFFFFull) return std::unexpected(bad("archive larger than 4 GB"));
    put32(out, 4, static_cast<u32>(out.size()));
    put32(out, 8, static_cast<u32>(ib));
    put32(out, 12, data_off_rel);
    put32(out, 16, static_cast<u32>(data.size()));
    put32(out, 20, static_cast<u32>(data.size()));
    put32(out, 24, 0);
    put32(out, 28, 0);
    return out;
}

}  // namespace hdf::io
