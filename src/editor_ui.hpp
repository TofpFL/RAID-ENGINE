// The editors' widgets (chart, character, stage and event editors): an
// immediate-mode panel toolkit drawn in canvas units. Each frame the editor
// fills in the mouse, clicks and wheel, then calls the widgets, which draw
// themselves and say what changed.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common.hpp"
#include "gfx.hpp"
#include "input.hpp"
#include "raylib.h"

namespace gh::edui {

using gfx::Align;
using gfx::FontId;

inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c >= 0x20) o += c;
    }
    return o + "\"";
}

inline std::string num(double v) {
    char b[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.1f", v);
    else std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos && s.find('e') == std::string::npos) {
        while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    }
    return s;
}

inline std::string utf8(char32_t c) {
    std::string o;
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    return o;
}

inline void popUtf8(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = (unsigned char)s.back();
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

inline const Color kPanel{12, 8, 20, 232};
inline const Color kInk{232, 226, 246, 255};
inline const Color kDim{170, 160, 196, 255};
inline const Color kAccent{255, 214, 120, 255};

struct Ui {
    Vector2 mouse{};
    bool click = false, rclick = false;
    float wheel = 0;
    const Input* in = nullptr;
    std::string focus;  // id of the text field being typed into
    bool used = false;  // the mouse did something in the panel this frame
    // An open drop-down list (drawn over everything by overlays()).
    std::string dropId, dropChanged;
    Rectangle dropRect{};
    std::vector<std::string> dropOpts;
    int dropCur = 0, dropPicked = 0;  // the item shown when it opened; the one clicked
    float dropScroll = 0;
    bool dropJustOpened = false;  // the click that opened it must not close it
    static constexpr int kDropRows = 12;

    // Widgets under an open list do not see the mouse.
    bool hover(Rectangle r) const { return CheckCollisionPointRec(mouse, r) && !(!dropId.empty() && CheckCollisionPointRec(mouse, dropRect)); }
    void box(Rectangle r, bool on = false, bool hot = false) const {
        DrawRectangleRounded(r, 0.25f, 6, on ? Color{90, 66, 140, 255} : hot ? Color{58, 44, 88, 255} : Color{36, 28, 56, 255});
        DrawRectangleRoundedLinesEx(r, 0.25f, 6, 1.5f, on ? kAccent : Color{110, 92, 150, 200});
    }
    bool button(Rectangle r, const std::string& label, bool on = false) {
        const bool h = hover(r);
        box(r, on, h);
        gfx::text(FontId::Ui, label, r.x + r.width * 0.5f, r.y + (r.height - 18) * 0.5f, 18, kInk, Align::Center);
        if (h && click) {
            used = true;
            focus.clear();
            return true;
        }
        return false;
    }
    bool check(Rectangle r, const std::string& label, bool& v) {
        const Rectangle b{r.x, r.y + (r.height - 18) * 0.5f, 18, 18};
        const bool h = hover(r);
        box(b, v, h);
        if (v) gfx::text(FontId::Ui, "x", b.x + 9, b.y - 1, 18, kAccent, Align::Center);
        gfx::text(FontId::Ui, label, r.x + 28, r.y + (r.height - 18) * 0.5f, 18, kInk);
        if (h && click) {
            v = !v;
            used = true;
            focus.clear();
            return true;
        }
        return false;
    }
    // label  [-] value [+]   (the wheel over it works too)
    bool stepper(Rectangle r, const std::string& label, double& v, double step, double lo, double hi, const char* fmt) {
        gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
        const float bw = 28, vx = r.x + r.width - 150;
        const Rectangle minus{vx, r.y, bw, r.height}, plus{r.x + r.width - bw, r.y, bw, r.height};
        bool changed = false;
        const bool shift = in && (in->isDown(KEY_LEFT_SHIFT) || in->isDown(KEY_RIGHT_SHIFT));
        const double s = shift ? step * 10 : step;
        if (button(minus, "-")) { v -= s; changed = true; }
        if (button(plus, "+")) { v += s; changed = true; }
        const Rectangle mid{vx + bw + 2, r.y, 150 - 2 * bw - 4, r.height};
        if (hover(mid) && wheel != 0) {
            v += (wheel > 0 ? s : -s);
            changed = true;
            used = true;
        }
        v = clampv(v, lo, hi);
        box(mid);
        gfx::text(FontId::Ui, TextFormat(fmt, v), mid.x + mid.width * 0.5f, r.y + (r.height - 18) * 0.5f, 18, kInk, Align::Center);
        return changed;
    }
    // A text field: click to type, ENTER or a click elsewhere to finish.
    bool text(const std::string& id, Rectangle r, const std::string& label, std::string& s) {
        float fx = r.x;
        if (!label.empty()) {
            gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
            fx = r.x + 92;
        }
        const Rectangle f{fx, r.y, r.x + r.width - fx, r.height};
        const bool h = hover(f);
        if (h && click) {
            focus = id;
            used = true;
        }
        const bool on = focus == id;
        box(f, on, h);
        bool changed = false;
        if (on && in) {
            for (char32_t c : in->typed())
                if (c >= 32 && c != 127) {
                    s += utf8(c);
                    changed = true;
                }
            if (in->pressed(KEY_BACKSPACE) && !s.empty()) {
                popUtf8(s);
                changed = true;
            }
            if (in->pressed(KEY_ENTER) || in->pressed(KEY_KP_ENTER) || in->pressed(KEY_TAB)) focus.clear();
        }
        BeginScissorMode(int(gfx::canvasOrigin().x + (f.x + 4) * gfx::canvasScale()), int(gfx::canvasOrigin().y + f.y * gfx::canvasScale()),
                         int((f.width - 8) * gfx::canvasScale()), int(f.height * gfx::canvasScale()));
        const std::string shown = s + (on && std::fmod(GetTime(), 1.0) < 0.5 ? "|" : "");
        const float tw = gfx::measure(FontId::Ui, shown, 18).x;
        gfx::text(FontId::Ui, shown, std::min(f.x + 6, f.x + f.width - 8 - tw), f.y + (f.height - 18) * 0.5f, 18, kInk);
        EndScissorMode();
        return changed;
    }
    // label  [option  v]   A click opens the list (wheel to scroll it); a click
    // on an item picks it. True on the frame after a new pick.
    bool dropdown(const std::string& id, Rectangle r, const std::string& label, const std::vector<std::string>& opts, int& idx,
                  float labelW = 92) {
        if (opts.empty()) return false;
        bool changed = false;
        if (dropChanged == id) {  // picked in the list last frame
            dropChanged.clear();
            idx = dropPicked;
            changed = true;
        }
        gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
        const Rectangle f{r.x + labelW, r.y, r.width - labelW, r.height};
        idx = clampv(idx, 0, int(opts.size()) - 1);
        const bool open = dropId == id;
        const bool h = hover(f);
        box(f, open, h);
        const std::string& o = opts[size_t(idx)];
        BeginScissorMode(int(gfx::canvasOrigin().x + (f.x + 4) * gfx::canvasScale()), int(gfx::canvasOrigin().y + f.y * gfx::canvasScale()),
                         int((f.width - 30) * gfx::canvasScale()), int(f.height * gfx::canvasScale()));
        gfx::text(FontId::Ui, o.empty() ? "(normal)" : o, f.x + 8, f.y + (f.height - 18) * 0.5f, 18, kInk);
        EndScissorMode();
        gfx::text(FontId::Ui, open ? "^" : "v", f.x + f.width - 14, f.y + (f.height - 18) * 0.5f, 18, kAccent, Align::Center);
        if (h && click) {
            used = true;
            focus.clear();
            if (open) {
                dropId.clear();
            } else {
                dropId = id;
                dropOpts = opts;
                dropCur = idx;
                dropJustOpened = true;
                const int rows = std::min(kDropRows, int(opts.size()));
                const float rh = 24, listH = rows * rh + 6;
                const float below = f.y + f.height + 2;
                dropRect = {f.x, below + listH <= gfx::H - 8 ? below : std::max(8.0f, f.y - 2 - listH), f.width, listH};
                dropScroll = float(std::clamp(idx - rows / 2, 0, int(opts.size()) - rows));
            }
        }
        return changed;
    }
    // Draws the open list (after the rest of the panel) and handles its clicks.
    void overlays() {
        if (dropId.empty()) return;
        const int n = int(dropOpts.size()), rows = std::min(kDropRows, n);
        const bool over = CheckCollisionPointRec(mouse, dropRect);
        if (over && wheel != 0) dropScroll = std::clamp(dropScroll - wheel * 3, 0.0f, float(std::max(0, n - rows)));
        DrawRectangleRec({dropRect.x - 2, dropRect.y - 2, dropRect.width + 4, dropRect.height + 4}, Color{6, 4, 12, 250});
        DrawRectangleLinesEx(dropRect, 1.5f, kAccent);
        const float rh = 24;
        const int first = int(dropScroll);
        for (int k = 0; k < rows && first + k < n; ++k) {
            const int i = first + k;
            const Rectangle it{dropRect.x + 3, dropRect.y + 3 + k * rh, dropRect.width - 6, rh};
            const bool h = CheckCollisionPointRec(mouse, it);
            if (i == dropCur || h) DrawRectangleRec(it, i == dropCur ? Color{90, 66, 140, 255} : Color{58, 44, 88, 255});
            gfx::text(FontId::Ui, dropOpts[size_t(i)].empty() ? "(normal)" : dropOpts[size_t(i)], it.x + 6, it.y + 3, 18, kInk);
            if (h && click) {
                dropPicked = i;
                dropChanged = dropId;
                dropId.clear();
                used = true;
                return;
            }
        }
        if (n > rows) {  // where the list is
            const float bh = dropRect.height * rows / n, by = dropRect.y + (dropRect.height - bh) * (dropScroll / float(n - rows));
            DrawRectangleRec({dropRect.x + dropRect.width - 5, by, 3, bh}, kDim);
        }
        if (over && click) used = true;
        if ((click && !over && !dropJustOpened) || (in && in->pressed(KEY_ESCAPE))) dropId.clear();
        dropJustOpened = false;
    }

    // label  < option >   (click the arrows, or the wheel over it)
    bool choice(Rectangle r, const std::string& label, const std::vector<std::string>& opts, int& idx, float labelW = 92) {
        if (opts.empty()) return false;
        gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
        const float x0 = r.x + labelW, bw = 28;
        const Rectangle prev{x0, r.y, bw, r.height}, next{r.x + r.width - bw, r.y, bw, r.height};
        const Rectangle mid{x0 + bw + 2, r.y, r.width - labelW - 2 * bw - 4, r.height};
        const int n = int(opts.size());
        int d = 0;
        if (button(prev, "<")) d = -1;
        if (button(next, ">")) d = 1;
        if (hover(mid) && wheel != 0) {
            d = wheel > 0 ? -1 : 1;
            used = true;
        }
        if (d) idx = ((idx + d) % n + n) % n;
        idx = clampv(idx, 0, n - 1);
        box(mid);
        const std::string& o = opts[size_t(idx)];
        gfx::text(FontId::Ui, o.empty() ? "(normal)" : o, mid.x + mid.width * 0.5f, r.y + (r.height - 18) * 0.5f, 18, kInk,
                  Align::Center);
        return d != 0;
    }
};

}  // namespace gh::edui
