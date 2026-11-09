// AutoChart: makes a playable FNF-style chart from any song's audio, so a
// folder with just an Inst.ogg (or a lone .mp3 in songs/) can be played.
// Onsets come from spectral flux, the tempo from the onset envelope's
// autocorrelation; notes are snapped to the beat grid, thinned per
// difficulty, spread across lanes by pitch and traded between the opponent
// and the player every four bars.
#pragma once

#include <cstddef>
#include <cstdint>

#include "chart.hpp"

namespace gh {

struct AutoChartInfo {
    double bpm = 0, firstBeatMs = 0;
    size_t onsets = 0;
};

// samples: interleaved 16-bit PCM. difficulty: 0 easy, 1 normal, 2 hard.
bool autoChart(const int16_t* samples, size_t frames, int channels, int sampleRate, int difficulty, Chart& out,
               AutoChartInfo* info = nullptr);

}  // namespace gh
