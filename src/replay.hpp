// Replays: every key press and release of a run, in song time. Judgement is
// deterministic, so feeding the same events back reproduces the run exactly.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gh {

struct ReplayEvent {
    double t = 0;  // song time (ms), offset already applied
    uint8_t lane = 0;
    bool down = false;
};

struct Replay {
    std::string songId, diff, title, date;
    double rate = 1.0;
    bool mirror = false, ghostTapping = true;
    int keys = 4;
    // The judgement the run was played with (Psych's hit windows and rating
    // offset); older replays have none and were played with these.
    double windows[4] = {45.0, 90.0, 135.0, 166.0};
    double ratingOffset = 0;
    long long score = 0;
    double acc = 0;
    long long misses = 0;
    std::vector<ReplayEvent> events;

    bool save(const std::string& path) const;
    bool load(const std::string& path);
};

// File name for a song's replay ("<id>-<diff>-last.ghr" / "-best.ghr").
std::string replayFileName(const std::string& songId, const std::string& diff, const char* kind);

}  // namespace gh
