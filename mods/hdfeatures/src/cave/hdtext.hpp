#pragma once

// Cave of Shadows: TP HD's own text and message flows, read at run time from res/cave/hdtext/ (made on the
// player's PC from their own HD copy and stored by the mod, hdfiles/files.hpp; never distributed), and
// installed through MessageService / FlowService. Without those files the mod falls back to its own
// wording (Midna's menu, the arrival message and the UI texts).
//
// Flows (HD zel_05, message group 5 = D_SB11's STAG group, VERIFIED by the extractor):
//   1111  Midna's menu inside the cave (tphd: daMidna_c::execute uses 0x457 = 1111 instead of 0xBB9)
//   1120  arrival: the voice's intro / "new path" / "return now" (started by floor 1's r00_in cutscene,
//         which plays only while HD label 829 is set: after an entry that announced something new)
//   1121  end of a section (Tag_ms on floors 5 and 19)
//   1122  HD's Great Fairy reward talk: replaced by a reward talk of our own built from HD's lines
// HD-only queries/events and labels >= 822 become custom FlowService queries/events (critic #8).

#include <cstdint>
#include <string>

namespace cave::hdtext {

inline constexpr std::uint8_t kZel00 = 0;
inline constexpr std::uint8_t kZel05 = 5;
inline constexpr std::uint16_t kFlowMidna = 1111;
inline constexpr std::uint16_t kFlowArrival = 1120;
inline constexpr std::uint16_t kFlowSectionEnd = 1121;
inline constexpr std::uint16_t kFlowReward = 1122;

// Loads res/cave/hdtext, registers messages, custom queries/events and flow graphs, and (when the graphs
// exist) the Tag_ms stage edits that start flows 1121/1122. Never fatal: problems are logged.
void initialize();
void shutdown();

bool text_loaded();        // messages.bin parsed
bool hd_flows();           // HD graphs from flows.json are installed (false: fallback or none)
// Plain UTF-8 text of an HD message ("" when missing). `playerName` replaces the name tag.
std::string text(std::uint8_t file, std::uint16_t id);
std::string text_or(std::uint8_t file, std::uint16_t id, const char* fallback);
// Custom start node of an installed flow (kFlow*), 0 when that flow is not installed.
std::uint16_t flow_start(std::uint16_t flowId);
// True when a Tag_ms that starts `flowId` was added to D_SB11 (section-end / reward talks).
bool talk_tag(std::uint16_t flowId);
// ... to that room of D_SB11 (the floor-5 and floor-19 section-end talks are separate stage edits).
bool talk_tag_in_room(std::uint16_t flowId, int room);
// Arrival message for floor 1's cutscene: evaluates flow 1120 (or the fallback), runs the flow's events
// (event001 clears label 829, event044 = Withdraw) and returns the message id to show (0 = none).
std::uint16_t arrival_message();
// One line describing what was installed (log and mod panel).
std::string summary();

}  // namespace cave::hdtext
