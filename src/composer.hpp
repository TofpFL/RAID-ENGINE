// Procedural composer for the built-in songs. A song is described by a short
// definition (tempo, key, structure); compose() turns it into audio stems and
// melody data, and buildChart() derives playable charts from that melody.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "chart.hpp"

namespace gh {

enum class PartKind : uint8_t { Intro, Call, Answer, Duet, Break, Outro };
struct Part { PartKind kind; int bars; int intensity; };

struct SongDef {
    std::string id, title, opponent, blurb;
    double bpm = 120;
    int root = 50;  // MIDI note of the tonic
    std::vector<int> scale;  // 7 semitone offsets
    uint32_t seed = 1;
    int style = 0;  // 0 serene, 1 ethereal, 2 drive, 3 dark
    std::vector<Part> parts;
    std::vector<std::string> diffs;
    uint32_t color = 0xFFFFFF;
    std::vector<std::vector<int>> progressions;  // chord roots as scale degrees
    int oppOctave = 0;
    bool vocals = true;
    int stage = 0;
};

struct MelNote {
    double beat = 0, len = 0;
    int deg = 0;  // scale-degree index (octave * 7 + degree)
    uint8_t side = 0;
    bool phraseStart = false;
    int intensity = 0;
};

struct Composition {
    double bpm = 120, lengthSec = 0, loopSec = 0;
    std::vector<int16_t> inst;    // interleaved stereo, 44.1 kHz
    std::vector<int16_t> voiceP;  // mono, 44.1 kHz
    std::vector<int16_t> voiceO;  // mono, 44.1 kHz
    std::vector<MelNote> melody, arp;
    std::vector<std::pair<double, int>> focus;  // (beat, side)
};

const std::vector<SongDef>& builtinSongs();
const SongDef* findBuiltin(const std::string& id);
const SongDef& menuTheme();
int degToMidi(const SongDef& def, int deg);

std::shared_ptr<Composition> compose(const SongDef& def, bool renderAudio);
bool buildChart(const SongDef& def, const Composition& comp, const std::string& diff, Chart& out);

}  // namespace gh
