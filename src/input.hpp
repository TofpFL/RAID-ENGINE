// Timestamped keyboard/gamepad input. The main loop polls far more often than
// it renders, so every press carries a ~1 ms accurate timestamp that gameplay
// converts into song time.
#pragma once

#include <vector>

#include "settings.hpp"

namespace gh {

struct InputEvent {
    int code;
    bool down;
    double t;  // nowSec() at poll
};

class Input {
public:
    static constexpr int kMaxCodes = kPadBase + 32;

    void poll(double now);   // call right after raylib's PollInputEvents()
    void beginFrame(double dt);
    void clear();

    const std::vector<InputEvent>& events() const { return frame_; }
    bool pressed(int code) const;
    bool isDown(int code) const { return code > 0 && code < kMaxCodes && down_[code]; }
    int firstPressed() const;  // first code pressed this frame, 0 if none

    bool up() const { return nav_[0]; }
    bool down() const { return nav_[1]; }
    bool left() const { return nav_[2]; }
    bool right() const { return nav_[3]; }
    bool accept() const;
    bool back() const;

private:
    std::vector<InputEvent> pending_, frame_;
    bool down_[kMaxCodes] = {};
    bool nav_[4] = {};
    double held_[4] = {};
    double repeatAcc_[4] = {};
};

}  // namespace gh
