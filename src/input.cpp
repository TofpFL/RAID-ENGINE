#include "input.hpp"

#include "raylib.h"

namespace gh {

void Input::poll(double now) {
    for (int k = GetKeyPressed(); k != 0; k = GetKeyPressed()) {
        if (k <= 0 || k >= kPadBase) continue;
        pending_.push_back({k, true, now});
        down_[k] = true;
    }
    for (int k = 1; k < 400 && k < kPadBase; ++k) {
        if (!down_[k]) continue;
        if (!IsKeyDown(k)) {
            pending_.push_back({k, false, now});
            down_[k] = false;
        }
    }
    if (IsGamepadAvailable(0)) {
        for (int b = 1; b < 18; ++b) {
            const int code = kPadBase + b;
            const bool d = IsGamepadButtonDown(0, b);
            if (d != down_[code]) {
                pending_.push_back({code, d, now});
                down_[code] = d;
            }
        }
    }
}

void Input::beginFrame(double dt) {
    frame_.swap(pending_);
    pending_.clear();
    static const int navKeys[4][3] = {
        {KEY_UP, KEY_W, kPadBase + GAMEPAD_BUTTON_LEFT_FACE_UP},
        {KEY_DOWN, KEY_S, kPadBase + GAMEPAD_BUTTON_LEFT_FACE_DOWN},
        {KEY_LEFT, KEY_A, kPadBase + GAMEPAD_BUTTON_LEFT_FACE_LEFT},
        {KEY_RIGHT, KEY_D, kPadBase + GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
    };
    for (int d = 0; d < 4; ++d) {
        nav_[d] = false;
        bool anyDown = false;
        for (int c : navKeys[d]) {
            if (pressed(c)) nav_[d] = true;
            anyDown |= isDown(c);
        }
        if (anyDown) {
            held_[d] += dt;
            if (held_[d] > 0.38) {
                repeatAcc_[d] += dt;
                if (repeatAcc_[d] >= 0.065) { repeatAcc_[d] = 0; nav_[d] = true; }
            }
        } else {
            held_[d] = 0;
            repeatAcc_[d] = 0;
        }
    }
}

void Input::clear() {
    pending_.clear();
    frame_.clear();
    for (bool& n : nav_) n = false;
}

bool Input::pressed(int code) const {
    for (const InputEvent& e : frame_)
        if (e.down && e.code == code) return true;
    return false;
}

int Input::firstPressed() const {
    for (const InputEvent& e : frame_)
        if (e.down) return e.code;
    return 0;
}

bool Input::accept() const {
    return pressed(KEY_ENTER) || pressed(KEY_KP_ENTER) || pressed(KEY_SPACE) ||
           pressed(kPadBase + GAMEPAD_BUTTON_RIGHT_FACE_DOWN) || pressed(kPadBase + GAMEPAD_BUTTON_MIDDLE_RIGHT);
}

bool Input::back() const {
    return pressed(KEY_ESCAPE) || pressed(KEY_BACKSPACE) || pressed(kPadBase + GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
}

}  // namespace gh
