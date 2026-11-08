#include "stage.hpp"

#include <cmath>

#include "common.hpp"

namespace gh {

using gfx::fade;

namespace {
struct Crack { std::vector<Vector2> pts; };
std::vector<Crack> g_cracks;

void drawColumn(float x, float top, float bottom, float w, Color body, Color shade) {
    DrawRectangleRec({x - w * 0.5f, top, w, bottom - top}, body);
    for (int i = 1; i < 5; ++i) {
        const float fx = x - w * 0.5f + w * i / 5.0f;
        DrawLineEx({fx, top}, {fx, bottom}, 3, fade(shade, 0.6f));
    }
    DrawRectangleRec({x - w * 0.5f, top, w * 0.22f, bottom - top}, fade(shade, 0.35f));
    DrawRectangleRec({x - w * 0.68f, bottom - 40, w * 1.36f, 40}, body);
    DrawRectangleRec({x - w * 0.62f, bottom - 58, w * 1.24f, 18}, shade);
}
}  // namespace

void Stage::init(int kind, Color accent, bool lowDetail, bool flashing) {
    kind_ = kind;
    accent_ = accent;
    low_ = lowDetail;
    flashing_ = flashing;
    flash_ = pulse_ = 0;
    motes_.clear();
    stars_.clear();
    Rng rng(0x51A7u + uint64_t(kind));
    for (int i = 0; i < 240; ++i)
        stars_.push_back({float(rng.uniform() * 1760 - 240), float(rng.uniform() * 760 - 200), float(0.6 + rng.uniform() * 1.8),
                          float(rng.uniform() * 6.28)});
    g_cracks.clear();
    for (int c = 0; c < 12; ++c) {
        Crack k;
        float x = float(rng.uniform() * 1500 - 100), y = float(560 + rng.uniform() * 240);
        k.pts.push_back({x, y});
        for (int s = 0; s < 6; ++s) {
            x += float(rng.uniform() * 90 - 45);
            y += float(rng.uniform() * 40 - 8);
            k.pts.push_back({x, y});
        }
        g_cracks.push_back(k);
    }
}

void Stage::update(float dt, double now) {
    flash_ = std::max(0.0f, flash_ - dt * 2.5f);
    pulse_ = std::max(0.0f, pulse_ - dt * 3.0f);
    motes_.update(dt, kind_ == 2 ? -20.0f : 0.0f);
    if (low_) return;
    spawn_ += dt * (kind_ == 2 ? 22.0f : 12.0f);
    while (spawn_ >= 1.0f) {
        spawn_ -= 1.0f;
        gfx::Particle p{};
        p.p = {gfx::randf(-200, 1480), gfx::randf(560, 820)};
        p.kind = 0;
        if (kind_ == 0) {
            p.v = {gfx::randf(-8, 8), gfx::randf(-55, -25)};
            p.c = Color{255, 214, 140, 200};
            p.size = gfx::randf(4, 10);
            p.life = p.maxLife = gfx::randf(4, 7);
        } else if (kind_ == 1) {
            p.p.y = gfx::randf(-100, 700);
            p.v = {gfx::randf(-12, 12), gfx::randf(-14, -4)};
            p.c = Color{150, 200, 255, 200};
            p.size = gfx::randf(3, 7);
            p.life = p.maxLife = gfx::randf(3, 6);
        } else if (kind_ == 2) {
            p.v = {gfx::randf(-20, 20), gfx::randf(-140, -60)};
            p.c = (std::rand() % 3) ? Color{255, 90, 40, 230} : Color{255, 180, 80, 230};
            p.size = gfx::randf(3, 7);
            p.life = p.maxLife = gfx::randf(2, 4);
        } else {
            p.v = {gfx::randf(-10, 10), gfx::randf(-35, -10)};
            p.c = fade(accent_, 0.8f);
            p.size = gfx::randf(3, 8);
            p.life = p.maxLife = gfx::randf(3, 6);
        }
        motes_.add(p);
    }
    (void)now;
}

void Stage::onBeat(int beat) {
    pulse_ = 1.0f;
    if (kind_ == 2 && flashing_ && beat % 16 == 8) flash_ = 0.55f;
}

void Stage::drawBack(double now) const {
    const float t = float(now);
    const Rectangle full{-260, -220, 1800, 1160};
    switch (kind_) {
        case 0: {  // Sanctum Dawn
            gfx::gradientRect({full.x, full.y, full.width, 720}, Color{58, 42, 94, 255}, Color{247, 196, 112, 255});
            gfx::glow({640, 330}, 520, Color{255, 190, 110, 150});
            BeginBlendMode(BLEND_ADDITIVE);
            for (int i = 0; i < 14; ++i) {
                const float a = t * 0.05f + i * 6.2831853f / 14.0f;
                const Vector2 c{640, 330};
                const Vector2 p1{c.x + std::cos(a - 0.05f) * 1300, c.y + std::sin(a - 0.05f) * 1300};
                const Vector2 p2{c.x + std::cos(a + 0.05f) * 1300, c.y + std::sin(a + 0.05f) * 1300};
                gfx::tri(c, p1, p2, Color{255, 240, 210, 14});
            }
            EndBlendMode();
            DrawCircleV({640, 330}, 74, Color{255, 238, 200, 255});
            gfx::glow({640, 330}, 160, Color{255, 230, 180, 200});
            for (int i = 0; i < 7; ++i) {
                const float cx = std::fmod(i * 260.0f + t * (6 + i), 1900.0f) - 300;
                gfx::softCircle({cx, 170.0f + (i % 3) * 60}, 150, Color{255, 236, 220, 60});
            }
            for (int i = 0; i < 11; ++i) {
                const float x = -160 + i * 160.0f;
                drawColumn(x, 330, 520, 34, Color{178, 128, 120, 150}, Color{120, 80, 90, 150});
            }
            gfx::gradientRect({full.x, 500, full.width, 440}, Color{232, 204, 168, 255}, Color{130, 98, 96, 255});
            for (int i = -12; i <= 12; ++i)
                DrawLineEx({640, 470}, {640 + i * 180.0f, 940}, 2, Color{255, 255, 255, 26});
            for (int i = 0; i < 6; ++i) {
                const float y = 520 + std::pow(1.6f, float(i)) * 14;
                DrawLineEx({full.x, y}, {full.x + full.width, y}, 2, Color{120, 80, 70, 40});
            }
            break;
        }
        case 1: {  // Celestial Court
            gfx::gradientRect(full, Color{8, 10, 36, 255}, Color{48, 28, 96, 255});
            gfx::softCircle({380, 200}, 420, Color{120, 60, 200, 50});
            gfx::softCircle({980, 300}, 380, Color{40, 110, 220, 55});
            for (const Star& s : stars_) {
                const float a = 0.5f + 0.5f * std::sin(t * 2.0f + s.ph);
                DrawCircleV({s.x, s.y}, s.s, fade(WHITE, 0.3f + 0.7f * a));
            }
            const Color rc{127, 184, 255, 255};
            for (int r = 0; r < 3; ++r) {
                const float rx = 300.0f + r * 90, ry = 90.0f + r * 26, rot = t * (0.06f + r * 0.03f) * (r % 2 ? -1 : 1);
                Vector2 prev{};
                for (int i = 0; i <= 72; ++i) {
                    const float a = i / 72.0f * 6.2831853f;
                    const float x = std::cos(a) * rx, y = std::sin(a) * ry;
                    const Vector2 p{640 + x * std::cos(rot) - y * std::sin(rot), 290 + x * std::sin(rot) + y * std::cos(rot)};
                    if (i > 0) DrawLineEx(prev, p, 2.0f, fade(rc, 0.25f + 0.2f * pulse_));
                    prev = p;
                    if (i % 12 == 0) DrawCircleV(p, 4, fade(rc, 0.6f));
                }
            }
            gfx::glow({640, 290}, 120 + 30 * pulse_, fade(rc, 0.5f));
            for (Vector2 c : {Vector2{330, 612}, Vector2{960, 622}}) {
                gfx::glow(c, 220, fade(rc, 0.25f));
                DrawEllipse(int(c.x), int(c.y), 190, 34, Color{30, 34, 80, 255});
                DrawEllipse(int(c.x), int(c.y) - 6, 180, 26, Color{70, 84, 160, 255});
                DrawEllipse(int(c.x), int(c.y) - 8, 150, 18, Color{120, 150, 230, 255});
            }
            break;
        }
        case 2: {  // Throne of Demise
            gfx::gradientRect({full.x, full.y, full.width, 420}, Color{4, 2, 6, 255}, Color{50, 6, 14, 255});
            gfx::gradientRect({full.x, 200, full.width, 360}, Color{50, 6, 14, 255}, Color{130, 18, 18, 255});
            gfx::glow({640, 240}, 420, Color{255, 40, 40, (unsigned char)(90 + 60 * pulse_)});
            DrawCircleV({640, 240}, 122, Color{196, 38, 38, 255});
            DrawCircleV({610, 210}, 22, Color{160, 26, 30, 255});
            DrawCircleV({680, 270}, 30, Color{160, 26, 30, 255});
            DrawCircleV({660, 190}, 12, Color{160, 26, 30, 255});
            DrawRing({640, 240}, 118, 124, 200, 340, 32, Color{255, 120, 100, 160});
            const Color sil{22, 6, 10, 255};
            for (int i = 0; i < 12; ++i) {
                const float x = -200 + i * 150.0f + (i % 2) * 40;
                const float h = 120.0f + float((i * 73) % 140);
                gfx::tri({x - 30, 560}, {x + 30, 560}, {x, 560 - h}, sil);
                DrawRectangleRec({x - 18, 560 - h * 0.55f, 36, h * 0.55f}, sil);
            }
            gfx::quad({120, 560}, {170, 560}, {205, 380}, {160, 372}, sil);
            gfx::quad({1110, 560}, {1160, 560}, {1130, 400}, {1084, 410}, sil);
            gfx::gradientRect({full.x, 545, full.width, 400}, Color{30, 8, 12, 255}, Color{8, 2, 4, 255});
            BeginBlendMode(BLEND_ADDITIVE);
            for (const auto& k : g_cracks)
                for (size_t i = 1; i < k.pts.size(); ++i)
                    DrawLineEx(k.pts[i - 1], k.pts[i], 3.0f, Color{255, 60, 30, (unsigned char)(90 + 100 * pulse_)});
            EndBlendMode();
            break;
        }
        default: {  // Void
            gfx::gradientRect(full, gfx::mix(accent_, BLACK, 0.8f), Color{4, 2, 10, 255});
            gfx::softCircle({640, 300}, 520, fade(accent_, 0.18f + 0.1f * pulse_));
            for (const Star& s : stars_) DrawCircleV({s.x, s.y}, s.s, fade(WHITE, 0.25f + 0.35f * std::sin(t + s.ph)));
            for (int i = 0; i < 6; ++i) {
                const float a = t * 0.2f + i;
                const Vector2 c{200.0f + i * 190.0f, 220.0f + 60 * std::sin(t * 0.5f + i)};
                DrawPolyLinesEx(c, 4, 30.0f + 10 * (i % 3), a * 40, 2, fade(accent_, 0.4f));
            }
            gfx::gradientRect({full.x, 600, full.width, 340}, fade(BLACK, 0.0f), fade(BLACK, 0.7f));
            break;
        }
    }
}

void Stage::drawFront(double now) const {
    motes_.draw(true);
    if (kind_ == 0) {
        drawColumn(-30, -220, 940, 120, Color{236, 222, 200, 255}, Color{176, 150, 130, 255});
        drawColumn(1310, -220, 940, 120, Color{236, 222, 200, 255}, Color{176, 150, 130, 255});
    } else if (kind_ == 2) {
        for (int i = 0; i < 8; ++i) {
            const float x = std::fmod(i * 230.0f + float(now) * 20.0f, 1900.0f) - 300;
            gfx::softCircle({x, 760}, 220, Color{60, 6, 12, 120});
        }
    }
}

void Stage::drawOverlay() const {
    if (flash_ > 0 && flashing_)
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{255, 220, 220, (unsigned char)(flash_ * 160)});
}

}  // namespace gh
