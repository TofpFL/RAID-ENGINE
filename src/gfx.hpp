// Drawing helpers: virtual 1280x720 canvas, fonts, note textures, glow,
// particles and a few shape primitives that are robust to vertex order.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "raylib.h"

namespace gh::gfx {

constexpr float W = 1280.0f, H = 720.0f;

void init(const std::string& baseDir);
void shutdown();

// Virtual canvas: everything is drawn in 1280x720 units, scaled and letterboxed.
Camera2D screenCamera(float zoom = 1.0f, Vector2 focus = {W / 2, H / 2}, float rotationDeg = 0.0f);
// While drawing into a render texture of this size, the canvas maps onto it
// instead of the window (0, 0 = the window, or the frame target, again).
void setRenderTargetSize(int w, int h);
// Rendering mode: whole frames are drawn into `rt` (a video's size) instead
// of the window. beginTexture/endTexture stand for raylib's Begin/
// EndTextureMode in the game's own passes: ending one goes back to the frame
// target, not to the window.
void beginFrameTarget(const RenderTexture2D& rt);
void endFrameTarget();
bool frameTargetActive();
void beginTexture(const RenderTexture2D& rt);
void endTexture();
int targetWidth();   // pixels of what is drawn into now (window or frame target)
int targetHeight();
void beginCanvas(float zoom = 1.0f, Vector2 focus = {W / 2, H / 2}, float rotationDeg = 0.0f);
void endCanvas();
void drawLetterbox();
// The part of the canvas shown in the window: x from 0 to viewWidth(), like
// Flixel's FlxG.width (a script can narrow it to 960 for a 4:3 picture).
void setViewWidth(float w);
float viewWidth();
float canvasScale();     // window pixels per canvas unit
Vector2 canvasOrigin();  // window position of canvas point (0, 0)
Vector2 toCanvas(Vector2 windowPos);

// ---- text
enum class FontId { Title, Ui };
enum class Align { Left, Center, Right };
void text(FontId f, std::string_view s, float x, float y, float size, Color c, Align a = Align::Left,
          float outline = 0.0f, Color outlineColor = {10, 6, 18, 255});
Vector2 measure(FontId f, std::string_view s, float size);
const Font& fontOf(FontId f);
void glowText(FontId f, std::string_view s, float x, float y, float size, Color c, Color glow, Align a = Align::Center);

// ---- colours
Color rgb(unsigned hex, unsigned char a = 255);
Color fade(Color c, float a);
Color mix(Color a, Color b, float t);
Color brighten(Color c, float amount);  // -1..1

// ---- shapes (vertex order does not matter)
void tri(Vector2 a, Vector2 b, Vector2 c, Color col);
void quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color col);
void convex(const std::vector<Vector2>& pts, Color col);  // convex polygon
void capsule(Vector2 a, Vector2 b, float radius, Color col);
void gradientRect(Rectangle r, Color top, Color bottom);
void glow(Vector2 c, float radius, Color col);  // additive soft light
void softCircle(Vector2 c, float radius, Color col);  // alpha-blended soft disc
Texture2D& glowTexture();

// ---- notes
enum NoteDir { kLeft = 0, kDown = 1, kUp = 2, kRight = 3, kCenter = 4 };
int laneDir(int keys, int lane);
Color dirColor(int dir);
void drawNote(int dir, Vector2 c, float size, float alpha = 1.0f, float scale = 1.0f, float extraRotDeg = 0.0f);
void drawReceptor(int dir, Vector2 c, float size, Color tint, float scale = 1.0f, float extraRotDeg = 0.0f);
void drawSustain(int dir, float x, float y0, float y1, float width, float alpha, bool capAtY1);

// ---- particles
struct Particle {
    Vector2 p, v;
    float life, maxLife, size, spin;
    Color c;
    int kind;  // 0 soft dot, 1 spark line, 2 ring
};
class Particles {
public:
    void add(const Particle& p) { if (ps_.size() < 2000) ps_.push_back(p); }
    void update(float dt, float gravity = 0.0f);
    void draw(bool additive = true) const;
    void clear() { ps_.clear(); }
    size_t size() const { return ps_.size(); }

private:
    std::vector<Particle> ps_;
};

float randf(float lo, float hi);

}  // namespace gh::gfx
