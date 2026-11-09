#include "characters.hpp"

#include <cmath>
#include <initializer_list>
#include <vector>

#include "common.hpp"
#include "gfx.hpp"

namespace gh {

using gfx::fade;
using gfx::mix;

CharId charFromName(const std::string& name) {
    const std::string n = toLower(name);
    if (n == "herald") return CharId::Herald;
    if (n == "principality") return CharId::Principality;
    if (n == "hand") return CharId::Hand;
    if (n == "demise") return CharId::Demise;
    return CharId::Shade;
}

const char* charDisplayName(CharId id) {
    switch (id) {
        case CharId::Herald: return "The Herald";
        case CharId::Principality: return "The Principality";
        case CharId::Hand: return "The Hand";
        case CharId::Demise: return "Demise";
        default: return "A Shade";
    }
}

Color charColor(CharId id) {
    switch (id) {
        case CharId::Herald: return Color{112, 140, 255, 255};
        case CharId::Principality: return Color{255, 205, 100, 255};
        case CharId::Hand: return Color{127, 184, 255, 255};
        case CharId::Demise: return Color{235, 40, 60, 255};
        default: return Color{180, 140, 210, 255};
    }
}

void CharacterState::sing(int dir, double now, double holdSec) {
    singDir = dir;
    singStart = now;
    singUntil = now + std::max(0.32, holdSec + 0.18);
    missUntil = -10;
}
void CharacterState::miss(int dir, double now) {
    singDir = dir;
    singStart = now;
    singUntil = now + 0.35;
    missUntil = now + 0.35;
}
void CharacterState::beat(double now) {
    if (now > singUntil) beatAt = now;
}

namespace {

struct Xf {
    Vector2 o{};
    float s = 1, sx = 1, sy = 1, rot = 0;
    bool flip = false;
    Vector2 operator()(float x, float y) const {
        if (flip) x = -x;
        x *= sx * s;
        y *= sy * s;
        const float c = std::cos(rot), n = std::sin(rot);
        return {o.x + x * c - y * n, o.y + x * n + y * c};
    }
    float len(float v) const { return v * s * (sx + sy) * 0.5f; }
};

void poly(const Xf& X, std::initializer_list<Vector2> pts, Color c) {
    std::vector<Vector2> v;
    v.reserve(pts.size());
    for (const Vector2& p : pts) v.push_back(X(p.x, p.y));
    gfx::convex(v, c);
}

void ell(const Xf& X, float cx, float cy, float rx, float ry, Color c, int seg = 30) {
    std::vector<Vector2> v;
    v.reserve(size_t(seg));
    for (int i = 0; i < seg; ++i) {
        const float a = float(i) / float(seg) * 6.2831853f;
        v.push_back(X(cx + std::cos(a) * rx, cy + std::sin(a) * ry));
    }
    gfx::convex(v, c);
}

void cap(const Xf& X, float ax, float ay, float bx, float by, float r, Color c) {
    gfx::capsule(X(ax, ay), X(bx, by), X.len(r), c);
}

void line(const Xf& X, float ax, float ay, float bx, float by, float w, Color c) {
    DrawLineEx(X(ax, ay), X(bx, by), X.len(w), c);
}

// Ring/arc strip between radii r0 (inner) and r1 (outer); ry scales vertically.
void arc(const Xf& X, float cx, float cy, float r0, float r1, float a0, float a1, Color c, float ry = 1.0f,
         int seg = 32, float taper = 0.0f) {
    for (int i = 0; i < seg; ++i) {
        const float t0 = float(i) / seg, t1 = float(i + 1) / seg;
        const float b0 = a0 + (a1 - a0) * t0, b1 = a0 + (a1 - a0) * t1;
        const float o0 = r1 - (r1 - r0) * taper * t0, o1 = r1 - (r1 - r0) * taper * t1;
        const Vector2 p0 = X(cx + std::cos(b0) * r0, cy + std::sin(b0) * r0 * ry);
        const Vector2 p1 = X(cx + std::cos(b1) * r0, cy + std::sin(b1) * r0 * ry);
        const Vector2 q0 = X(cx + std::cos(b0) * o0, cy + std::sin(b0) * o0 * ry);
        const Vector2 q1 = X(cx + std::cos(b1) * o1, cy + std::sin(b1) * o1 * ry);
        gfx::quad(p0, p1, q1, q0, c);
    }
}

// Leaf-shaped feather from (rx,ry) in direction `ang` (radians).
void feather(const Xf& X, float rx, float ry, float ang, float len, float wid, Color c) {
    const float ca = std::cos(ang), sa = std::sin(ang), px = -sa, py = ca;
    std::vector<Vector2> pts;
    const int N = 10;
    for (int i = 0; i <= N; ++i) {
        const float t = float(i) / N;
        const float hw = wid * 0.5f * std::pow(std::sin(3.14159265f * std::pow(t, 0.7f)), 0.8f);
        pts.push_back(X(rx + ca * len * t + px * hw, ry + sa * len * t + py * hw));
    }
    for (int i = N - 1; i > 0; --i) {
        const float t = float(i) / N;
        const float hw = wid * 0.5f * std::pow(std::sin(3.14159265f * std::pow(t, 0.7f)), 0.8f);
        pts.push_back(X(rx + ca * len * t - px * hw, ry + sa * len * t - py * hw));
    }
    gfx::convex(pts, c);
}

struct Pose {
    float dx = 0, dy = 0, sx = 1, sy = 1, rot = 0, pop = 0;
    int dir = -1;
    bool singing = false, missing = false;
    float bop = 0;
};

Pose computePose(const CharacterState& s, double now) {
    Pose p;
    p.singing = now < s.singUntil && s.singDir >= 0;
    p.missing = now < s.missUntil;
    p.bop = float(std::exp(-(now - s.beatAt) * 7.0));
    if (p.singing) {
        const float pop = float(std::exp(-(now - s.singStart) * 12.0));
        p.pop = pop;
        p.dir = s.singDir;
        switch (s.singDir) {
            case 0: p.dx = -12 - 10 * pop; p.rot = -0.06f; break;
            case 3: p.dx = 12 + 10 * pop; p.rot = 0.06f; break;
            case 2: p.dy = -10 - 10 * pop; p.sy = 1.05f + 0.04f * pop; p.sx = 0.97f; break;
            case 1: p.dy = 8 + 6 * pop; p.sy = 0.92f - 0.03f * pop; p.sx = 1.05f; break;
            default: break;
        }
    } else {
        p.sy = 1.0f - 0.035f * p.bop;
        p.sx = 1.0f + 0.02f * p.bop;
    }
    return p;
}

// Hand target for a pose, in screen-oriented coordinates (x < 0 = screen left).
Vector2 handTarget(const Pose& P, Vector2 idle, Vector2 l, Vector2 d, Vector2 u, Vector2 r) {
    if (!P.singing) return idle;
    switch (P.dir) {
        case 0: return l;
        case 1: return d;
        case 2: return u;
        case 3: return r;
        default: return idle;
    }
}

// ------------------------------------------------------------ the Herald --

void drawHerald(const CharacterState& s, const Pose& P, Vector2 feet, float sc, double now, bool flip, bool shade) {
    Color cloak{46, 38, 92, 255}, cloakDark{30, 24, 64, 255}, trim{232, 190, 92, 255};
    const Color voidC{12, 8, 24, 255};
    Color eyes{170, 230, 255, 255};
    if (shade) {
        cloak = mix(s.tint, Color{20, 16, 30, 255}, 0.55f);
        cloakDark = mix(s.tint, Color{10, 8, 16, 255}, 0.75f);
        trim = mix(s.tint, WHITE, 0.35f);
        eyes = mix(s.tint, WHITE, 0.6f);
    }
    if (P.missing) {
        cloak = mix(cloak, Color{70, 80, 120, 255}, 0.6f);
        eyes = Color{120, 130, 160, 255};
    }
    const float m = flip ? -1.0f : 1.0f;  // screen-oriented -> local x
    Xf base{feet, sc};
    ell(base, 0, 0, 72, 13, fade(BLACK, 0.35f));
    Xf X{{feet.x + P.dx * sc, feet.y + P.dy * sc}, sc, P.sx, P.sy, P.rot, flip};

    // back arm
    cap(X, 36, -150, 54, -96, 11, cloakDark);
    ell(X, 55, -92, 8, 8, Color{220, 214, 245, 255});
    // body
    poly(X, {{-48, -168}, {48, -168}, {76, 0}, {-76, 0}}, cloak);
    poly(X, {{8, -168}, {48, -168}, {76, 0}, {20, 0}}, fade(cloakDark, 0.55f));
    line(X, -4, -150, -10, -10, 3, fade(cloakDark, 0.9f));
    poly(X, {{-76, 0}, {76, 0}, {73, -12}, {-73, -12}}, trim);
    poly(X, {{-42, -152}, {42, -152}, {38, -141}, {-38, -141}}, trim);
    // head and hood
    ell(X, 4, -196, 44, 44, cloak);
    poly(X, {{14, -232}, {46, -204}, {44, -254}}, cloak);
    ell(X, -10, -190, 27, 31, voidC);
    const float eyeA = P.singing ? 1.0f : 0.8f;
    if (P.missing) {
        for (float ex : {-22.0f, -6.0f}) {
            line(X, ex - 4, -198, ex + 4, -190, 2.2f, eyes);
            line(X, ex - 4, -190, ex + 4, -198, 2.2f, eyes);
        }
    } else {
        const float blink = std::fmod(float(now) + 0.7f, 4.2f) < 0.1f ? 0.25f : 1.0f;
        ell(X, -22, -194, 4.5f, 6.0f * blink, fade(eyes, eyeA));
        ell(X, -6, -194, 4.5f, 6.0f * blink, fade(eyes, eyeA));
        gfx::glow(X(-14, -194), 26 * sc, fade(eyes, 0.35f * eyeA));
    }
    // front arm holding the orb of grace
    const Vector2 t = handTarget(P, {-62, -104}, {-104, -150}, {-66, -76}, {-48, -230}, {40, -176});
    const float hx = t.x * m, hy = t.y + (P.singing ? 0.0f : std::sin(float(now) * 2.0f) * 3.0f);
    cap(X, -34, -150, hx, hy, 11, cloak);
    const Vector2 orb = X(hx, hy - 8);
    const float pulse = 1.0f + 0.25f * P.bop + 0.4f * P.pop;
    gfx::glow(orb, 46 * sc * pulse, fade(trim, P.missing ? 0.2f : 0.55f));
    DrawCircleV(orb, 13 * sc, P.missing ? Color{120, 110, 90, 255} : trim);
    DrawCircleV(orb, 5.5f * sc, fade(WHITE, P.missing ? 0.3f : 0.95f));
    ell(X, hx, hy, 8, 8, Color{220, 214, 245, 255});
}

// ------------------------------------------------------- the Principality --

void drawPrincipality(const CharacterState& s, const Pose& P, Vector2 feet, float sc, double now, bool flip) {
    const Color ivory{243, 234, 210, 255}, ivoryDark{205, 188, 150, 255}, gold{240, 196, 90, 255};
    const Color porcelain{250, 246, 235, 255}, wing{255, 251, 242, 235};
    const float m = flip ? -1.0f : 1.0f;
    const float bob = std::sin(float(now) * 2.2f) * 8.0f - 26.0f;
    Xf base{feet, sc};
    ell(base, 0, 0, 52, 9, fade(BLACK, 0.22f));
    Xf X{{feet.x + P.dx * sc, feet.y + (P.dy + bob) * sc}, sc, P.sx, P.sy, P.rot, flip};
    gfx::glow(X(0, -200), 190 * sc, fade(gold, 0.30f + 0.15f * P.bop));
    // wings: fans of leaf-shaped feathers
    const float flap = std::sin(float(now) * 3.0f) * 0.07f + P.bop * 0.08f + P.pop * 0.1f;
    const float angs[5] = {-0.12f, -0.42f, -0.74f, -1.04f, -1.32f};
    const float lens[5] = {118, 150, 176, 166, 136}, wids[5] = {30, 34, 36, 32, 26};
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 4; i >= 0; --i) {
            float a = angs[i] - flap * (0.6f + 0.2f * i);
            if (side < 0) a = 3.14159265f - a;
            const float rx = side * 14.0f, ry = -204;
            feather(X, rx, ry, a, lens[i] + 6, wids[i] + 5, fade(gold, 0.85f));
            feather(X, rx, ry, a, lens[i], wids[i], wing);
            const float tx = rx + std::cos(a) * lens[i] * 0.7f, ty = ry + std::sin(a) * lens[i] * 0.7f;
            line(X, rx, ry, tx, ty, 1.6f, fade(Color{220, 200, 160, 255}, 0.7f));
        }
        ell(X, side * 22.0f, -200, 22, 16, wing);
    }
    // robe
    poly(X, {{-34, -222}, {34, -222}, {66, -34}, {0, -12}, {-66, -34}}, ivory);
    poly(X, {{6, -222}, {34, -222}, {66, -34}, {0, -12}}, fade(ivoryDark, 0.45f));
    line(X, -14, -190, -30, -40, 2.5f, fade(ivoryDark, 0.8f));
    line(X, 14, -190, 26, -40, 2.5f, fade(ivoryDark, 0.8f));
    poly(X, {{-30, -192}, {30, -192}, {27, -181}, {-27, -181}}, gold);
    // arms
    const Vector2 l = handTarget(P, {-50, -150}, {-112, -196}, {-64, -108}, {-52, -276}, {-40, -170});
    const Vector2 r = handTarget(P, {50, -150}, {40, -170}, {64, -108}, {52, -276}, {112, -196});
    for (const Vector2& h : {l, r}) {
        const float sxs = h.x < 0 ? -1.0f : 1.0f;
        cap(X, sxs * m * 28, -210, h.x * m, h.y, 12, ivoryDark);
        ell(X, h.x * m, h.y, 8, 8, porcelain);
    }
    // head with a single eye
    ell(X, 0, -252, 30, 31, porcelain);
    arc(X, 0, -252, 28, 31, 0, 6.2831853f, gold, 1.0f, 40);
    ell(X, 0, -254, 15, 8.5f, WHITE);
    float gx = 0, gy = 0;
    if (P.singing) {
        if (P.dir == 0) gx = -5 * m;
        if (P.dir == 3) gx = 5 * m;
        if (P.dir == 2) gy = -3;
        if (P.dir == 1) gy = 3;
    }
    ell(X, gx, -254 + gy, 6.5f, 6.5f, Color{255, 180, 50, 255});
    ell(X, gx, -254 + gy, 2.6f, 2.6f, Color{40, 20, 10, 255});
    if (P.singing) gfx::glow(X(gx, -254 + gy), 30 * sc, fade(gold, 0.6f));
    line(X, -16, -264, 16, -264, 2.0f, fade(gold, 0.8f));
    // halo
    gfx::glow(X(0, -302), 60 * sc, fade(gold, 0.45f + 0.2f * P.bop));
    arc(X, 0, -302, 22, 29, 0, 6.2831853f, gold, 0.32f, 40);
}

// ---------------------------------------------------------------- the Hand --

void drawHand(const CharacterState& s, const Pose& P, Vector2 feet, float sc, double now, bool flip) {
    const Color marble{236, 232, 224, 255}, shade{196, 190, 182, 255}, gold{226, 182, 80, 255};
    const float hover = std::sin(float(now) * 1.8f) * 10.0f - 20.0f;
    float extraRot = 0, spread = 1, curl = 1, lift = 0;
    if (P.singing) {
        if (P.dir == 0) extraRot = -0.28f;
        if (P.dir == 3) extraRot = 0.28f;
        if (P.dir == 2) { lift = -22; spread = 1.35f; }
        if (P.dir == 1) { lift = 16; curl = 0.62f; }
    }
    Xf base{feet, sc};
    ell(base, 0, 0, 60, 10, fade(BLACK, 0.22f));
    Xf X{{feet.x + P.dx * sc * 0.5f, feet.y + (hover + lift) * sc}, sc, P.sx, P.sy, P.rot + extraRot, flip};
    gfx::glow(X(0, -205), 200 * sc, fade(Color{127, 184, 255, 255}, 0.35f + 0.2f * P.bop));
    // forearm
    cap(X, 0, -30, 0, -150, 34, shade);
    poly(X, {{-36, -64}, {36, -64}, {36, -76}, {-36, -76}}, gold);
    poly(X, {{-35, -92}, {35, -92}, {35, -100}, {-35, -100}}, gold);
    // palm
    ell(X, 0, -205, 64, 72, marble);
    // fingers
    const float lens[4] = {62, 80, 86, 74};
    for (int i = 0; i < 4; ++i) {
        const float bx = -45.0f + i * 30.0f;
        const float l = lens[i] * curl;
        const float dirx = (bx / 45.0f) * 0.22f * spread;
        const float tx = bx + dirx * l, ty = -255 - l;
        cap(X, bx, -250, tx, ty, 15, marble);
        line(X, bx + (tx - bx) * 0.45f - 9, -250 + (ty + 250) * 0.45f, bx + (tx - bx) * 0.45f + 9,
             -250 + (ty + 250) * 0.45f, 1.6f, fade(shade, 0.9f));
        if (i == 2) poly(X, {{bx - 15, -268}, {bx + 15, -268}, {bx + 15, -276}, {bx - 15, -276}}, gold);
    }
    cap(X, 56, -200, 96 * spread, -244, 16, marble);
    // eye in the palm
    const bool blink = std::fmod(float(now), 3.7f) < 0.12f;
    ell(X, 0, -205, 27, blink ? 3.0f : 15.0f, WHITE);
    if (!blink) {
        float gx = 0;
        if (P.singing && P.dir == 0) gx = -8;
        if (P.singing && P.dir == 3) gx = 8;
        if (flip) gx = -gx;
        ell(X, gx, -205, 10, 10, Color{80, 150, 255, 255});
        ell(X, gx, -205, 4, 4, Color{10, 16, 40, 255});
        if (P.singing) gfx::glow(X(gx, -205), 32 * sc, fade(Color{127, 184, 255, 255}, 0.7f));
    }
    line(X, -40, -170, -10, -160, 1.6f, fade(shade, 0.8f));
    line(X, 10, -160, 40, -172, 1.6f, fade(shade, 0.8f));
}

// ------------------------------------------------------------------ Demise --

void drawDemise(const CharacterState& s, const Pose& P, Vector2 feet, float sc, double now, bool flip) {
    const Color cloak{18, 9, 20, 255}, edge{180, 20, 40, 255}, bone{226, 214, 190, 255}, boneDark{150, 130, 110, 255};
    const Color crown{170, 128, 50, 255}, red{255, 40, 60, 255};
    const float m = flip ? -1.0f : 1.0f;
    const float breath = std::sin(float(now) * 1.5f) * 3.0f;
    Xf base{feet, sc};
    ell(base, 0, 0, 110, 16, fade(BLACK, 0.45f));
    Xf X{{feet.x + P.dx * sc, feet.y + P.dy * sc}, sc, P.sx, P.sy * (1.0f + breath * 0.004f), P.rot, flip};
    gfx::glow(X(0, -190), 240 * sc, fade(edge, 0.35f + 0.25f * P.bop));
    // scythe
    cap(X, 72, -6, 56, -344, 6, Color{58, 40, 32, 255});
    arc(X, -16, -334, 64, 84, -0.08f, -2.9f, Color{92, 92, 112, 255}, 1.0f, 30, 0.85f);
    arc(X, -16, -334, 82, 86, -0.08f, -2.9f, fade(red, 0.8f), 1.0f, 30, 0.0f);
    // cloak with a crimson rim
    poly(X, {{-62, -238}, {62, -238}, {102, 4}, {-102, 4}}, fade(edge, 0.85f));
    poly(X, {{-58, -232}, {58, -232}, {96, 0}, {-96, 0}}, cloak);
    for (int i = 0; i < 8; ++i) {
        const float x0 = -96.0f + i * 24.0f;
        poly(X, {{x0, -1}, {x0 + 24, -1}, {x0 + 12, 16.0f + (i % 2) * 12.0f}}, cloak);
    }
    poly(X, {{-22, -228}, {22, -228}, {0, -146}}, Color{48, 18, 26, 255});
    for (int i = 0; i < 3; ++i) line(X, -14.0f + i * 3, -212.0f + i * 18, 14.0f - i * 3, -212.0f + i * 18, 2.4f, boneDark);
    // arms
    const Vector2 l = handTarget(P, {-72, -122}, {-138, -206}, {-84, -78}, {-62, -306}, {-50, -160});
    const Vector2 r = handTarget(P, {72, -122}, {50, -160}, {84, -78}, {62, -306}, {138, -206});
    for (const Vector2& h : {l, r}) {
        const float sh = (h.x < 0 ? -52.0f : 52.0f) * m;
        const float hx = h.x * m, hy = h.y;
        const float ex = (sh + hx) * 0.5f, ey = (-214 + hy) * 0.5f + 12;
        cap(X, sh, -214, ex, ey, 15, cloak);
        cap(X, ex, ey, hx, hy, 6, bone);
        for (int c = -1; c <= 1; ++c) {
            const float ang = std::atan2(hy - ey, hx - ex) + c * 0.45f;
            cap(X, hx, hy, hx + std::cos(ang) * 18, hy + std::sin(ang) * 18, 2.6f, bone);
        }
    }
    // skull
    const float open = P.singing ? 4.0f + 8.0f * P.pop + 3.0f * std::fabs(std::sin(float(now) * 18.0f)) : 0.0f;
    ell(X, 0, -266, 32, 34, bone);
    poly(X, {{-20, -246}, {20, -246}, {16, -226 + open}, {-16, -226 + open}}, bone);
    line(X, -14, -240 + open * 0.5f, 14, -240 + open * 0.5f, 1.6f, boneDark);
    ell(X, -12, -268, 9, 10, Color{8, 4, 8, 255});
    ell(X, 12, -268, 9, 10, Color{8, 4, 8, 255});
    const float eg = P.singing ? 1.0f : 0.7f;
    ell(X, -12, -268, 3.4f, 3.4f, red);
    ell(X, 12, -268, 3.4f, 3.4f, red);
    gfx::glow(X(-12, -268), 22 * sc * (1 + P.pop), fade(red, 0.5f * eg));
    gfx::glow(X(12, -268), 22 * sc * (1 + P.pop), fade(red, 0.5f * eg));
    poly(X, {{-4, -252}, {4, -252}, {0, -246}}, Color{8, 4, 8, 255});
    line(X, 8, -296, 16, -280, 1.5f, boneDark);
    line(X, 16, -280, 12, -272, 1.5f, boneDark);
    // crown
    poly(X, {{-30, -292}, {30, -292}, {28, -283}, {-28, -283}}, crown);
    const float hs[5] = {18, 24, 32, 24, 18};
    for (int i = 0; i < 5; ++i) {
        const float x = -26.0f + i * 13.0f;
        poly(X, {{x - 6, -291}, {x + 6, -291}, {x, -291 - hs[i]}}, crown);
    }
    for (int i = 0; i < 3; ++i) ell(X, -14.0f + i * 14.0f, -287.5f, 2.6f, 2.6f, red);
}

}  // namespace

void drawCharacter(const CharacterState& s, Vector2 feet, float scale, double now, bool faceLeft) {
    const Pose P = computePose(s, now);
    switch (s.id) {
        case CharId::Herald: drawHerald(s, P, feet, scale, now, !faceLeft, false); break;
        case CharId::Shade: drawHerald(s, P, feet, scale, now, !faceLeft, true); break;
        case CharId::Principality: drawPrincipality(s, P, feet, scale, now, faceLeft); break;
        case CharId::Hand: drawHand(s, P, feet, scale, now, faceLeft); break;
        case CharId::Demise: drawDemise(s, P, feet, scale, now, faceLeft); break;
    }
}

void drawIcon(CharId id, Color tint, Vector2 c, float size, bool losing, double now) {
    const float sc = size / 100.0f;
    Xf X{c, sc};
    const Color ring = id == CharId::Shade ? tint : charColor(id);
    DrawCircleV(c, 46 * sc, Color{14, 10, 24, 235});
    DrawRing(c, 43 * sc, 48 * sc, 0, 360, 48, losing ? fade(ring, 0.45f) : ring);
    switch (id) {
        case CharId::Herald:
        case CharId::Shade: {
            const Color cl = id == CharId::Shade ? mix(tint, Color{20, 16, 30, 255}, 0.5f) : Color{56, 46, 110, 255};
            const Color ey = losing ? Color{120, 130, 160, 255} : (id == CharId::Shade ? mix(tint, WHITE, 0.6f) : Color{170, 230, 255, 255});
            ell(X, 2, 4, 32, 32, cl);
            poly(X, {{10, -22}, {30, -4}, {30, -36}}, cl);
            ell(X, -6, 8, 20, 22, Color{12, 8, 24, 255});
            if (losing) {
                for (float ex : {-14.0f, 0.0f}) {
                    line(X, ex - 3, 2, ex + 3, 8, 2, ey);
                    line(X, ex - 3, 8, ex + 3, 2, 2, ey);
                }
            } else {
                ell(X, -14, 5, 3.5f, 4.5f, ey);
                ell(X, 0, 5, 3.5f, 4.5f, ey);
            }
            break;
        }
        case CharId::Principality: {
            arc(X, 0, -30, 18, 23, 0, 6.2831853f, Color{240, 196, 90, 255}, 0.32f, 32);
            ell(X, 0, 4, 27, 28, Color{250, 246, 235, 255});
            ell(X, 0, 2, 13, losing ? 3.0f : 8.0f, WHITE);
            if (!losing) ell(X, 0, 2, 6, 6, Color{255, 180, 50, 255});
            if (losing) line(X, 6, -22, -4, 26, 1.8f, Color{120, 100, 80, 255});
            break;
        }
        case CharId::Hand: {
            for (int i = 0; i < 4; ++i) {
                const float bx = -21.0f + i * 14.0f;
                cap(X, bx, -8, bx * 1.15f, -30.0f - (i == 1 || i == 2 ? 8.0f : 0.0f), 6.5f, Color{236, 232, 224, 255});
            }
            ell(X, 0, 10, 30, 28, Color{236, 232, 224, 255});
            ell(X, 0, 10, 13, losing ? 2.0f : 8.0f, WHITE);
            if (!losing) ell(X, 0, 10, 5, 5, Color{80, 150, 255, 255});
            break;
        }
        case CharId::Demise: {
            ell(X, 0, 6, 26, 28, Color{226, 214, 190, 255});
            poly(X, {{-16, 22}, {16, 22}, {12, 36}, {-12, 36}}, Color{226, 214, 190, 255});
            ell(X, -10, 4, 7, 8, Color{8, 4, 8, 255});
            ell(X, 10, 4, 7, 8, Color{8, 4, 8, 255});
            const Color e = losing ? Color{90, 30, 40, 255} : Color{255, 40, 60, 255};
            ell(X, -10, 4, 2.6f, 2.6f, e);
            ell(X, 10, 4, 2.6f, 2.6f, e);
            Xf C = X;
            if (losing) C.rot = -0.3f;
            for (int i = 0; i < 5; ++i) {
                const float x = -20.0f + i * 10.0f;
                poly(C, {{x - 5, -18}, {x + 5, -18}, {x, -18 - (i == 2 ? 18.0f : 11.0f)}}, Color{170, 128, 50, 255});
            }
            poly(C, {{-24, -20}, {24, -20}, {22, -14}, {-22, -14}}, Color{170, 128, 50, 255});
            break;
        }
    }
    (void)now;
}

}  // namespace gh
