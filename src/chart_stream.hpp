// Chart files of any size (see chart_stream.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chart.hpp"

namespace gh {

struct StreamedNote {
    double time = 0;
    float length = 0;
    uint8_t data = 0;   // raw noteData (0..63, lane + side half); bit 7: legacy `true` in slot 3
    uint8_t stack = 1;  // notes of the file merged here (same value within 1 ms)
    uint16_t type = 0;  // 1-based index into StreamedNotes::types (0 = none)
    int noteData() const { return data & 63; }
    bool alt() const { return (data & 128) != 0; }
};

struct StreamedNotes {
    std::vector<StreamedNote> notes;
    std::vector<uint32_t> rangeStart;  // per sectionNotes array, plus a final end marker
    std::vector<std::string> types;
    int maxData = -1;
    std::array<uint64_t, 64> dataCount{};  // notes of the file per noteData value
    uint64_t rawCount = 0;  // notes in the file before stacked notes were merged
    uint16_t typeIndex(const std::string& name);
};

// Reads the file, sending every sectionNotes array to `notes` and the rest of
// the JSON to `skeleton` (with {"__gh_stream": k} where array k was).
bool streamChartSkeleton(const std::string& path, std::string& skeleton, StreamedNotes& notes, std::string& err);

// Loads any chart file: small ones are parsed whole, big ones are streamed.
bool loadChartFile(const std::string& path, const std::string& difficulty, Chart& out, std::string& err,
                   std::string_view vsliceMetadata = {}, size_t streamThreshold = 0);

// Quick look at a chart's song name, opponent and stage without parsing it
// (reads only the start and end of the file).
struct ChartPeek {
    std::string song, player2, stage;
    bool isChart = false;
};
ChartPeek peekChart(const std::string& path);

uint64_t fileSize(const std::string& path);
bool fileExistsAt(const std::string& path);

}  // namespace gh
