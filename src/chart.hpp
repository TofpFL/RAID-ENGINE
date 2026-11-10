// Chart model shared by built-in songs and imported FNF-style charts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "psych.hpp"

namespace gh {

enum NoteKind : uint8_t { kNormal = 0, kMine = 1 };
enum NoteFlag : uint8_t {
    kFlagNoAnim = 1,      // "No Animation"
    kFlagAlt = 2,         // "-alt" sing animation
    kFlagGF = 4,          // girlfriend sings it ("GF Sing", gfSection)
    kFlagHey = 8,         // "Hey!" note
    kFlagNoMissAnim = 16,
    kFlagIgnore = 32,     // passes without a miss (scripts: ignoreNote)
};

struct Note {
    double time = 0;     // ms
    double length = 0;   // ms of sustain (0 = tap)
    uint8_t lane = 0;    // 0..keys-1
    uint8_t side = 0;    // 0 = opponent, 1 = player
    uint8_t kind = kNormal;
    uint8_t flags = 0;
    uint16_t type = 0;   // index into Chart::noteTypes (0 = default)
    uint16_t stack = 1;  // notes of the file stacked here (same lane within 1 ms): each one counts
};

constexpr uint16_t kMaxStack = 65535;

struct TempoPoint { double timeMs = 0; double bpm = 120; double beat = 0; };
struct FocusPoint { double timeMs = 0; uint8_t side = 0; };

struct Chart {
    std::string title;
    std::string difficulty;
    std::string opponent;  // character id (built-ins) or source name
    std::string player;
    std::string gf, stage;     // Psych scene fields (empty if absent)
    std::vector<std::string> noteTypes{""};
    std::vector<psych::ChartEvent> events;  // sorted by time
    int keys = 4;
    double speed = 2.0;        // FNF scroll speed (pixels/ms = 0.45 * speed)
    std::vector<Note> notes;   // sorted by time
    std::vector<TempoPoint> tempo;
    std::vector<FocusPoint> focus;
    double lengthMs = 0;       // end of the last note
    uint64_t playerNotes = 0, opponentNotes = 0;  // stacked notes included
    uint64_t rawNoteCount = 0;  // notes in the file, before stacked notes were merged

    double bpmAt(double ms) const;
    double beatAt(double ms) const;
    double msAtBeat(double beat) const;
    double stepMs(double ms) const { return 15000.0 / bpmAt(ms); }
    int focusAt(double ms) const;  // side the camera should follow

    // Sorts notes, removes stacked duplicates, fills tempo beats and stats.
    void finalize(double baseBpm);
    void mirror();  // swap player/opponent (play as the opponent)
    uint16_t typeIndex(const std::string& name);
};

// Parses an FNF-family chart: legacy/Psych 0.x ("song.notes[].sectionNotes"),
// Psych 1.0 ("format": "psych_v1"), or V-Slice ("notes": {diff: [...]}) when
// metadataJson (the -metadata.json text) is given.
bool parseChart(std::string_view json, const std::string& difficulty, Chart& out, std::string& err,
                std::string_view vsliceMetadata = {});

bool readTextFile(const std::string& path, std::string& out);

struct StreamedNotes;
bool parseChartStreamed(std::string_view skeleton, const StreamedNotes& notes, const std::string& difficulty, Chart& out,
                        std::string& err);

}  // namespace gh
