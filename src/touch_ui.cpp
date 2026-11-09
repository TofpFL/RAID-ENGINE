#include "touch_ui.hpp"

#include <algorithm>
#include <cmath>

#include "gfx.hpp"
#include "input.hpp"

namespace gh {

std::vector<TouchPoint> touchPoints() {
    std::vector<TouchPoint> v;
    const int n = GetTouchPointCount();
    for (int i = 0; i < n; ++i) v.push_back({GetTouchPointId(i), GetTouchPosition(i)});
    if (v.empty() && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) v.push_back({-1, GetMousePosition()});
    return v;
}

void TouchUi::build(const TouchLayout& l, int sw, int sh) {
    buttons_.clear();
    const float W = float(sw), H = float(sh);
    const float u = std::max(0.5f, std::min(W, H) / 720.0f);
    unit_ = u;
    const float m = 24 * u;
    if (l.pad) {
        // Arrows bottom left, OK and BACK bottom right.
        const float s = 92 * u, g = 10 * u;
        const float cx = m + s * 1.5f + g, cy = H - m - s * 1.5f - g;
        buttons_.push_back({{cx - s / 2, cy - s * 1.5f - g, s, s}, KEY_UP, "", kUp, false});
        buttons_.push_back({{cx - s / 2, cy + s * 0.5f + g, s, s}, KEY_DOWN, "", kDown, false});
        buttons_.push_back({{cx - s * 1.5f - g, cy - s / 2, s, s}, KEY_LEFT, "", kLeft, false});
        buttons_.push_back({{cx + s * 0.5f + g, cy - s / 2, s, s}, KEY_RIGHT, "", kRight, false});
        const float ok = 124 * u, bk = 96 * u;
        buttons_.push_back({{W - m - ok, H - m - ok - 36 * u, ok, ok}, KEY_ENTER, "OK", kOk, true});
        buttons_.push_back({{W - m - ok - 18 * u - bk, H - m - bk, bk, bk}, KEY_ESCAPE, "BACK", kBack, true});
    }
    const float h = 58 * u, ts = 26 * u, gap = 12 * u;
    auto width = [&](const std::string& label) {
        return std::max(h * 1.7f, gfx::measure(gfx::FontId::Ui, label, ts).x + 40 * u);
    };
    if (l.keysLeft) {
        float x = m;
        for (const auto& [label, code] : l.keys) {
            const float w = width(label);
            buttons_.push_back({{x, m, w, h}, code, label, kNone, false});
            x += w + gap;
        }
    } else {
        float x = W - m;
        for (auto it = l.keys.rbegin(); it != l.keys.rend(); ++it) {
            const float w = width(it->first);
            x -= w;
            buttons_.push_back({{x, m, w, h}, it->second, it->first, kNone, false});
            x -= gap;
        }
    }
}

// The button nearest to p (inside, or within slop pixels of its edge).
int TouchUi::buttonAt(Vector2 p, float slop) const {
    int best = -1;
    float bestD = slop + 0.001f;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Rectangle& r = buttons_[i].r;
        float d;
        if (buttons_[i].round) {
            const float rad = r.width * 0.5f;
            d = std::max(0.0f, std::hypot(p.x - (r.x + rad), p.y - (r.y + rad)) - rad);
        } else {
            const float dx = std::max({r.x - p.x, 0.0f, p.x - (r.x + r.width)});
            const float dy = std::max({r.y - p.y, 0.0f, p.y - (r.y + r.height)});
            d = std::hypot(dx, dy);
        }
        if (d < bestD) {
            bestD = d;
            best = int(i);
        }
    }
    return best;
}

bool TouchUi::covers(Vector2 p) const { return buttonAt(p, 14 * unit_) >= 0; }

void TouchUi::setLayout(Input& in, const TouchLayout& l, double now) {
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    std::string sig = std::to_string(sw) + "x" + std::to_string(sh) + (l.pad ? "P" : "-") + std::to_string(l.tapCode) + ":" +
                      std::to_string(l.lanes) + (l.keysLeft ? "L" : "R");
    for (const auto& k : l.keys) sig += "|" + k.first + "=" + std::to_string(k.second);
    if (sig == sig_) return;
    releaseAll(in, now);
    sig_ = sig;
    build(l, sw, sh);
    tapCode_ = l.tapCode;
    lanes_ = std::clamp(l.lanes, 0, 9);
    laneHeld_.assign(size_t(lanes_), false);
    // Fingers already down belong to the old screen: ignore them until lifted.
    for (Finger& f : fingers_) f.kind = kIgnored;
}

void TouchUi::update(Input& in, double now) {
    events_.clear();
    if (platform::touchEvents()) {
        platform::takeTouchEvents(events_);
    } else {
        // Polled fingers (desktop: the mouse while clicked) become events.
        const std::vector<TouchPoint> pts = touchPoints();
        for (const Finger& f : fingers_) {
            const bool still = std::any_of(pts.begin(), pts.end(), [&](const TouchPoint& p) { return p.id == f.id; });
            if (!still) events_.push_back({platform::TouchEvent::Up, f.id, f.pos.x, f.pos.y, now});
        }
        for (const TouchPoint& p : pts) {
            const bool known = std::any_of(fingers_.begin(), fingers_.end(), [&](const Finger& f) { return f.id == p.id; });
            events_.push_back({known ? platform::TouchEvent::Move : platform::TouchEvent::Down, p.id, p.pos.x, p.pos.y, now});
        }
    }
    for (const platform::TouchEvent& e : events_) {
        apply(e);
        refresh(in, std::min(e.t, now));
    }
}

void TouchUi::apply(const platform::TouchEvent& e) {
    auto same = [&](const Finger& f) { return f.id == e.id; };
    switch (e.kind) {
        case platform::TouchEvent::Down:
            fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), same), fingers_.end());
            fingers_.push_back({e.id, {e.x, e.y}, buttonAt({e.x, e.y}, 0) >= 0 ? kButton : kFree});
            break;
        case platform::TouchEvent::Move:
            for (Finger& f : fingers_)
                if (f.id == e.id) f.pos = {e.x, e.y};
            break;
        case platform::TouchEvent::Up:
            fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), same), fingers_.end());
            break;
        case platform::TouchEvent::Cancel: fingers_.clear(); break;
    }
}

void TouchUi::refresh(Input& in, double t) {
    std::vector<bool> held(buttons_.size(), false), lanes(size_t(lanes_), false);
    bool anyFree = false;
    const float sw = float(std::max(1, GetScreenWidth()));
    for (const Finger& f : fingers_) {
        if (f.kind == kIgnored) continue;
        if (f.kind == kFree) {
            anyFree = true;
            if (lanes_ > 0) lanes[size_t(std::clamp(int(f.pos.x / sw * float(lanes_)), 0, lanes_ - 1))] = true;
            continue;
        }
        const int b = buttonAt(f.pos, 14 * unit_);
        if (b >= 0) held[size_t(b)] = true;
    }
    for (size_t i = 0; i < buttons_.size(); ++i)
        if (held[i] != buttons_[i].held) {
            buttons_[i].held = held[i];
            in.setVirtual(buttons_[i].code, held[i], t);
        }
    if (tapCode_ && anyFree != tapHeld_) {
        tapHeld_ = anyFree;
        in.setVirtual(tapCode_, anyFree, t);
    }
    for (int l = 0; l < lanes_; ++l)
        if (lanes[size_t(l)] != laneHeld_[size_t(l)]) {
            laneHeld_[size_t(l)] = lanes[size_t(l)];
            in.setVirtual(kTouchLane + l, lanes[size_t(l)], t);
        }
}

void TouchUi::releaseAll(Input& in, double now) {
    for (Button& b : buttons_)
        if (b.held) {
            b.held = false;
            in.setVirtual(b.code, false, now);
        }
    if (tapHeld_) {
        tapHeld_ = false;
        in.setVirtual(tapCode_, false, now);
    }
    for (int l = 0; l < lanes_; ++l)
        if (laneHeld_[size_t(l)]) {
            laneHeld_[size_t(l)] = false;
            in.setVirtual(kTouchLane + l, false, now);
        }
}

void TouchUi::draw() const {
    const float u = unit_;
    if (lanes_ > 0) {
        // Gameplay: one column per lane, lit while a finger is on it.
        const float sw = float(GetScreenWidth()), sh = float(GetScreenHeight());
        const float w = sw / float(lanes_);
        for (int l = 0; l < lanes_; ++l) {
            const Color c = gfx::dirColor(gfx::laneDir(lanes_, l));
            const bool on = laneHeld_[size_t(l)];
            if (on) gfx::gradientRect({l * w, sh * 0.35f, w, sh * 0.65f}, gfx::fade(c, 0.0f), gfx::fade(c, 0.24f));
            DrawRectangleRec({l * w + 2 * u, sh - 7 * u, w - 4 * u, 7 * u}, gfx::fade(c, on ? 0.95f : 0.4f));
            if (l > 0) DrawRectangleRec({l * w - 1, sh * 0.5f, 2, sh * 0.5f}, Color{255, 255, 255, 22});
        }
    }
    for (const Button& b : buttons_) {
        const Color fill = b.held ? Color{255, 214, 120, 165} : Color{20, 12, 36, 125};
        const Color edge = b.held ? Color{255, 240, 200, 235} : Color{255, 255, 255, 95};
        const Vector2 c{b.r.x + b.r.width * 0.5f, b.r.y + b.r.height * 0.5f};
        if (b.round) {
            DrawCircleV(c, b.r.width * 0.5f, fill);
            DrawRing(c, b.r.width * 0.5f - 3 * u, b.r.width * 0.5f, 0, 360, 48, edge);
        } else {
            DrawRectangleRounded(b.r, 0.3f, 8, fill);
            DrawRectangleRoundedLinesEx(b.r, 0.3f, 8, 3 * u, edge);
        }
        const Color ic = b.held ? Color{34, 18, 44, 255} : Color{246, 236, 214, 235};
        const float a = b.r.width * 0.25f;
        switch (b.icon) {
            case kUp: gfx::tri({c.x, c.y - a}, {c.x - a, c.y + a * 0.6f}, {c.x + a, c.y + a * 0.6f}, ic); break;
            case kDown: gfx::tri({c.x, c.y + a}, {c.x - a, c.y - a * 0.6f}, {c.x + a, c.y - a * 0.6f}, ic); break;
            case kLeft: gfx::tri({c.x - a, c.y}, {c.x + a * 0.6f, c.y - a}, {c.x + a * 0.6f, c.y + a}, ic); break;
            case kRight: gfx::tri({c.x + a, c.y}, {c.x - a * 0.6f, c.y - a}, {c.x - a * 0.6f, c.y + a}, ic); break;
            default: {
                const float ts = (b.icon == kOk ? 40 : 26) * u;
                const Vector2 sz = gfx::measure(gfx::FontId::Ui, b.label, ts);
                gfx::text(gfx::FontId::Ui, b.label, c.x, c.y - sz.y * 0.5f, ts, ic, gfx::Align::Center);
            }
        }
    }
}

}  // namespace gh
