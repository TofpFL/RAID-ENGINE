// The Visualizer Bar: a thin strip for gameplay — framed top/bottom lines,
// dark bed, a slowly precessing atom icon on the left and a live spectrum of
// the song's mixed audio across the rest. Pure decoration: it never takes
// input and sits under the notes and HUD (drawn first in the HUD canvas).
#pragma once

#include <complex>

namespace gh {

class VizBar {
public:
    // Call once per frame before draw(). dt in seconds, now = app time.
    void update(double now, float dt);
    // Draws at the configured edge (Settings::vizBar: 1 top, 2 bottom).
    void draw();
    void reset();

private:
    static constexpr int kFftN = 2048;
    static constexpr int kBands = 56;

    float band_[kBands] = {};
    std::complex<float> fftBuf_[kFftN];
    float window_[kFftN];
    float atomSpin_ = 0;
    bool windowReady_ = false;
};

}  // namespace gh
