// Chart files of any size (see chart_stream.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chart.hpp"

namespace gh {

struct StreamedNote {
    double time = 0;
    float length = 0;
    uint8_t data = 0;   // raw noteData (lane + side half)
    bool alt = false;   // legacy `true` in slot 3
    uint16_t type = 0;  // 1-based index into StreamedNotes::types (0 = none)
};

struct StreamedNotes {
    std::vector<StreamedNote> notes;
    std::vector<uint32_t> rangeStart;  // per sectionNotes array, plus a final end marker
    std::vector<std::string> types;
    int maxData = -1;
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
