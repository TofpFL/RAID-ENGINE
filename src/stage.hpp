// Animated backgrounds for each song.
#pragma once

#include <vector>

#include "gfx.hpp"

namespace gh {

class Stage {
public:
    // kind: 0 Sanctum Dawn, 1 Celestial Court, 2 Throne of Demise, 3 Void (custom songs)
    void init(int kind, Color accent, bool lowDetail, bool flashing);
    void update(float dt, double now);
    void onBeat(int beat);
    void drawBack(double now) const;
    void drawFront(double now) const;
    void drawOverlay() const;  // screen-space flashes
    int kind() const { return kind_; }

private:
    int kind_ = 0;
    Color accent_ = WHITE;
    bool low_ = false, flashing_ = true;
    float flash_ = 0, pulse_ = 0;
    struct Star { float x, y, s, ph; };
    std::vector<Star> stars_;
    gfx::Particles motes_;
    float spawn_ = 0;
};

}  // namespace gh
