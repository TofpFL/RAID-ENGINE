// On-screen controls for touchscreens: arrows, OK and BACK for the menus,
// labelled buttons each screen can ask for (MODS, PAUSE...) and, during a
// song, the lanes themselves. Everything presses the same key codes a
// keyboard would, with the poll's timestamp, so every screen works unchanged.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "platform.hpp"
#include "raylib.h"

namespace gh {

class Input;

struct TouchPoint {
    int id;
    Vector2 pos;  // window pixels
};
// Fingers on the screen; on desktop, the mouse while its left button is down.
std::vector<TouchPoint> touchPoints();

struct TouchLayout {
    bool pad = true;        // arrows + OK + BACK
    int tapCode = 0;        // a touch outside the buttons holds this key (0 = none)
    int lanes = 0;          // >0: the screen is split into this many lanes (kTouchLane + lane)
    bool keysLeft = false;  // labelled buttons in the top left corner instead of the right
    std::vector<std::pair<std::string, int>> keys;  // labelled buttons
};

class TouchUi {
public:
    // Once per frame: the current screen's buttons (rebuilt when they change).
    void setLayout(Input& in, const TouchLayout& layout, double now);
    // After every Input::poll: fingers press and release the buttons' keys.
    // Android delivers each touch with its own time, so even a tap shorter
    // than a frame presses and releases its key, at the right moments.
    void update(Input& in, double now);
    void draw() const;
    bool covers(Vector2 screenPos) const;  // on one of the buttons
    void releaseAll(Input& in, double now);

private:
    enum Icon { kNone, kUp, kDown, kLeft, kRight, kOk, kBack };
    struct Button {
        Rectangle r;
        int code;
        std::string label;
        Icon icon;
        bool round;
        bool held = false;
    };
    enum Kind { kButton, kFree, kIgnored };  // a finger that began on a button, elsewhere, or on the last screen
    struct Finger {
        int id;
        Vector2 pos;
        Kind kind;
    };
    void build(const TouchLayout& l, int sw, int sh);
    int buttonAt(Vector2 p, float slop) const;
    void apply(const platform::TouchEvent& e);
    void refresh(Input& in, double t);  // keys follow the fingers

    std::vector<Button> buttons_;
    std::string sig_;            // layout + screen size the buttons were built for
    int tapCode_ = 0, lanes_ = 0;
    bool tapHeld_ = false;
    std::vector<Finger> fingers_;
    std::vector<platform::TouchEvent> events_;
    std::vector<bool> laneHeld_;
    float unit_ = 1;
};

}  // namespace gh
