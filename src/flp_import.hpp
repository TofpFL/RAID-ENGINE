// FL Studio projects (.flp) and scores (.fsc) as charts, read the way SNIFF
// (SiIva Note Importer For FNF, https://github.com/JordanSantiagoYT/SNIFF)
// reads them: the notes of one pattern of the FPC channel (the first channel
// without one): every piano-roll key charts onto the four lanes
// (pitch % 4, C-anchored - C is L, C# D, D U, D# R, repeating), and each
// pattern takes ONE side voted from its notes (named channels vote their
// side, chart-channel notes vote by register). Channels named "Opp"/"Opponent"/"Player" -
// optionally indexed 1-50 like "Opp3" or "Player 12", so singing and atonal
// instruments share the one side - instead chart all of their notes by
// channel (every key, pitch % 4 lanes, their channel's side). A channel named
// "Cam Events" (aliases: CamEvents, Cam, Camera) carries the section markers
// instead of notes: F3 (53) and F#3 (54) point the camera at the player or the
// opponent from that section on, A3 (57) and A#3 (58) give the section's
// opponent or player notes their alt animation; its other keys are ignored.
// Those pitches on chart channels are ordinary notes now. A note held for a
// beat or more, or played softly (velocity under 64), becomes a hold.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "chart.hpp"

namespace gh {

struct FlpNote {
    uint32_t time = 0, duration = 0, pitch = 0;
    uint16_t channel = 0;
    uint8_t velocity = 100, flags = 0;
};

struct FlpPlaylistItem {
    int pattern = 0;        // FL pattern number (item_index - pattern_base)
    int track = 0;          // playlist track
    uint32_t startTick = 0, lengthTicks = 0;
};

struct FlpPattern {
    int number = 0;
    std::string name;
    std::vector<FlpNote> notes;
};

struct FlpProject {
    int ppqn = 96;          // ticks per beat
    double bpm = 0;         // 0: not in the file (.fsc scores)
    std::string title;
    int chartChannel = 0;   // the FPC channel (SNIFF's choice), else the first
    // Strings seen inside each channel's definition block (its name and
    // plugin name). flpToChart matches "Opp"/"Player"/"Cam Events" here:
    // side channels chart every key by channel, Cam Events feeds markers.
    std::map<uint32_t, std::vector<std::string>> channelNames;
    std::vector<FlpPlaylistItem> playlist;  // arrangement clips, in file order
    std::vector<FlpPattern> patterns;
};

bool readFlp(std::string_view bytes, FlpProject& out, std::string& err);
bool readFlpFile(const std::string& path, FlpProject& out, std::string& err);

// The patterns holding chart notes, by number. Strict: nearly all of the
// pattern's notes sit on chart channels, so pure music is not a chart.
std::vector<int> flpChartPatterns(const FlpProject& p, bool strict = true);
// A pattern's name for a difficulty list ("Hard", or "Pattern 3").
std::string flpPatternLabel(const FlpProject& p, int number);

// SNIFF's conversion of one pattern (-1: the first with chart notes). The
// project's tempo is used unless `bpm` is given (scores have none: 120).
bool flpToChart(const FlpProject& p, int pattern, Chart& out, std::string& err, double bpm = 0);

// "song.flp#3" names pattern 3 of song.flp (the library lists each pattern
// with chart notes as a difficulty). Splits it; pattern -1 when none given.
bool flpChartPath(const std::string& chartPath, std::string& file, int& pattern);

}  // namespace gh
