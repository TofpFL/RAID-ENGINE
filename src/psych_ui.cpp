#include "psych_ui.hpp"

#include "rlgl.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "audio.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "psych.hpp"
#include "scene.hpp"

namespace gh {

namespace {

std::string findAny(const scene::Files& f, std::initializer_list<std::string> rels) {
    for (const std::string& r : rels) {
        std::string p = f.find(r);
        if (!p.empty()) return p;
    }
    return {};
}

Texture2D loadTex(const std::string& path) {
    if (path.empty()) return Texture2D{};
    Texture2D t = LoadTexture(path.c_str());
    if (t.id) SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    return t;
}

void unloadTex(Texture2D& t) {
    if (t.id && IsWindowReady()) UnloadTexture(t);
    t = Texture2D{};
}

const char* kColors[4] = {"purple", "blue", "green", "red"};

// Psych Engine 0.7's default note colours: main, highlight, outline.
const unsigned kPalette[4][3] = {{0xC24B99, 0xFFFFFF, 0x3C1F56},
                                 {0x00FFFF, 0xFFFFFF, 0x1542B7},
                                 {0x12FA05, 0xFFFFFF, 0x0A4447},
                                 {0xF9393F, 0xFFFFFF, 0x651038}};

// red, green and blue of the template become the three colours.
const char* kPaletteGl330 = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 r;
uniform vec3 g;
uniform vec3 b;
out vec4 finalColor;
void main() {
    vec4 c = texture(texture0, fragTexCoord);
    finalColor = vec4(min(c.r * r + c.g * g + c.b * b, vec3(1.0)), c.a) * colDiffuse * fragColor;
}
)GLSL";
const char* kPaletteGles = R"GLSL(#version 100
precision mediump float;
varying vec2 fragTexCoord;
varying vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 r;
uniform vec3 g;
uniform vec3 b;
void main() {
    vec4 c = texture2D(texture0, fragTexCoord);
    gl_FragColor = vec4(min(c.r * r + c.g * g + c.b * b, vec3(1.0)), c.a) * colDiffuse * fragColor;
}
)GLSL";
const char* kDirs[4] = {"left", "down", "up", "right"};
const char* kArrows[4] = {"arrowLEFT", "arrowDOWN", "arrowUP", "arrowRIGHT"};

}  // namespace

PsychUi& psychUi() {
    static PsychUi ui;
    return ui;
}

int PsychUi::load(const std::string& folder, std::string& report) {
    unload();
    root_ = folder;
    if (!root_.empty() && root_.back() != '/' && root_.back() != '\\') root_ += '/';
    // The engine's own folder, its assets/ folder or the library folders
    // inside it: wherever the player pointed, the same names are found.
    scene::Files f;
    std::vector<std::string> roots;
    for (const char* sub : {"", "assets/", "assets/shared/", "assets/preload/", "assets/embed/", "shared/", "preload/", "../",
                            "../shared/", "../preload/"})
        roots.push_back(root_ + sub);
    f.setRoots(roots);
    int found = 0;
    std::vector<std::string> missing;

    const std::string notePng = findAny(f, {"images/noteskins/NOTE_assets.png", "images/noteSkins/NOTE_assets.png",
                                            "images/NOTE_assets.png"});
    if (!notePng.empty()) {
        std::string xml;
        psych::Atlas atlas;
        if (scene::readFile(notePng.substr(0, notePng.size() - 4) + ".xml", xml) && atlas.parseSparrow(xml)) {
            notes_ = loadTex(notePng);
            for (const psych::AtlasFrame& a : atlas.frames) {
                Frame fr;
                fr.src = {a.x, a.y, a.w, a.h};
                fr.fx = a.fx;
                fr.fy = a.fy;
                fr.fw = a.fw > 0 ? a.fw : a.w;
                fr.fh = a.fh > 0 ? a.fh : a.h;
                fr.rotated = a.rotated;
                atlas_.push_back({a.name, fr});
            }
            if (notes_.id) ++found;
        }
        // Skins in noteSkins/ (Psych 0.7, JS Engine) are colour templates.
        template_ = notePng.find("oteskins/") != std::string::npos || notePng.find("oteSkins/") != std::string::npos;
        if (template_) {
            palette_ = LoadShaderFromMemory(nullptr, rlGetVersion() == RL_OPENGL_ES_20 ? kPaletteGles : kPaletteGl330);
            locR_ = GetShaderLocation(palette_, "r");
            locG_ = GetShaderLocation(palette_, "g");
            locB_ = GetShaderLocation(palette_, "b");
            if (!palette_.id || palette_.id == rlGetShaderIdDefault()) template_ = false;
        }
    }
    if (!notes_.id) missing.push_back("note skin (NOTE_assets)");

    const char* rnames[4] = {"sick", "good", "bad", "shit"};
    for (int i = 0; i < 4; ++i) {
        ratings_[i] = loadTex(findAny(f, {std::string("images/ratings/default/") + rnames[i] + ".png",
                                          std::string("images/") + rnames[i] + ".png"}));
        if (ratings_[i].id) ++found;
    }
    for (int i = 0; i < 10; ++i) {
        nums_[i] = loadTex(findAny(f, {"images/ratings/default/num" + std::to_string(i) + ".png", "images/num" + std::to_string(i) + ".png"}));
        if (nums_[i].id) ++found;
    }
    if (!ratings_[0].id) missing.push_back("ratings (sick.png...)");
    const char* cnames[3] = {"ready", "set", "go"};
    for (int i = 0; i < 3; ++i) {
        count_[i] = loadTex(findAny(f, {std::string("images/") + cnames[i] + ".png"}));
        if (count_[i].id) ++found;
    }
    bar_ = loadTex(findAny(f, {"images/healthBar.png"}));
    if (bar_.id) ++found;
    const std::string ttf = findAny(f, {"fonts/vcr.ttf"});
    if (!ttf.empty()) {
        font_ = LoadFontEx(ttf.c_str(), 48, nullptr, 0);
        if (font_.texture.id) {
            SetTextureFilter(font_.texture, TEXTURE_FILTER_BILINEAR);
            ++found;
        }
    }
    auto sound = [&](Sfx id, const std::string& name) {
        const std::string p = findAny(f, {"sounds/" + name + ".ogg", "sounds/" + name + ".mp3", "sounds/" + name + ".wav"});
        if (!p.empty()) {
            sounds_.push_back({int(id), p});
            ++found;
        }
    };
    sound(Sfx::Scroll, "scrollMenu");
    sound(Sfx::Confirm, "confirmMenu");
    sound(Sfx::Cancel, "cancelMenu");
    sound(Sfx::Count3, "intro3");
    sound(Sfx::Count2, "intro2");
    sound(Sfx::Count1, "intro1");
    sound(Sfx::CountGo, "introGo");
    sound(Sfx::Miss, "missnote1");
    sound(Sfx::Hit, "hitsound");
    sound(Sfx::Death, "fnf_loss_sfx");
    if (sounds_.empty()) missing.push_back("sounds");
    menuMusic_ = findAny(f, {"music/freakyMenu.ogg", "music/freakyMenu.mp3"});
    if (!menuMusic_.empty()) ++found;

    report = std::to_string(found) + " pieces found";
    if (!missing.empty()) {
        report += "; missing: ";
        for (size_t i = 0; i < missing.size(); ++i) report += (i ? ", " : "") + missing[i];
    }
    return found;
}

void PsychUi::unload() {
    unloadTex(notes_);
    if (palette_.id && palette_.id != rlGetShaderIdDefault() && IsWindowReady()) UnloadShader(palette_);
    palette_ = Shader{};
    template_ = false;
    for (Texture2D& t : ratings_) unloadTex(t);
    for (Texture2D& t : nums_) unloadTex(t);
    for (Texture2D& t : count_) unloadTex(t);
    unloadTex(bar_);
    if (font_.texture.id && IsWindowReady()) UnloadFont(font_);
    font_ = Font{};
    atlas_.clear();
    sounds_.clear();
    menuMusic_.clear();
    active_ = false;
}

void PsychUi::applySounds() const {
    sfxClearOverrides();
    if (!active_) return;
    for (const auto& [id, path] : sounds_) sfxOverride(Sfx(id), path);
}

// Frames named exactly prefix + number, in order ("purple0000"...).
std::vector<PsychUi::Frame> PsychUi::frames(const std::string& prefix) const {
    std::vector<std::pair<int, Frame>> found;
    for (const auto& [name, fr] : atlas_) {
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = name.substr(prefix.size());
        if (!std::all_of(rest.begin(), rest.end(), [](char c) { return std::isdigit((unsigned char)c); })) continue;
        found.push_back({std::atoi(rest.c_str()), fr});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Frame> out;
    for (auto& p : found) out.push_back(p.second);
    return out;
}

bool PsychUi::drawFrame(const Frame& f, Vector2 c, float scale, float alpha, float angle, bool flipY) const {
    if (!notes_.id) return false;
    // The untrimmed frame is centred on c; the stored part sits inside it.
    const float w = f.rotated ? f.src.height : f.src.width, h = f.rotated ? f.src.width : f.src.height;
    const float ox = (-f.fx + w * 0.5f - f.fw * 0.5f) * scale, oy = (-f.fy + h * 0.5f - f.fh * 0.5f) * scale;
    const float rad = angle * DEG2RAD;
    const Vector2 center{c.x + ox * std::cos(rad) - oy * std::sin(rad), c.y + ox * std::sin(rad) + oy * std::cos(rad)};
    Rectangle src = f.src;
    if (flipY) src.height = -src.height;
    const Color tint = gfx::fade(WHITE, alpha);
    if (f.rotated) {
        const Rectangle dst{center.x, center.y, f.src.width * scale, f.src.height * scale};
        DrawTexturePro(notes_, src, dst, {dst.width * 0.5f, dst.height * 0.5f}, angle - 90.0f, tint);
    } else {
        const Rectangle dst{center.x, center.y, w * scale, h * scale};
        DrawTexturePro(notes_, src, dst, {dst.width * 0.5f, dst.height * 0.5f}, angle, tint);
    }
    return true;
}

void PsychUi::beginDir(int dir) const {
    if (!active_ || !template_ || dir < 0 || dir > 3) return;
    BeginShaderMode(palette_);
    const int locs[3] = {locR_, locG_, locB_};
    for (int k = 0; k < 3; ++k) {
        const unsigned c = kPalette[dir][k];
        const float v[3] = {float((c >> 16) & 255) / 255.0f, float((c >> 8) & 255) / 255.0f, float(c & 255) / 255.0f};
        if (locs[k] >= 0) SetShaderValue(palette_, locs[k], v, SHADER_UNIFORM_VEC3);
    }
}

void PsychUi::endDir() const {
    if (active_ && template_) EndShaderMode();
}

bool PsychUi::note(int dir, Vector2 c, float size, float alpha, float angle) const {
    if (!active_ || dir < 0 || dir > 3) return false;
    const std::vector<Frame> fr = frames(kColors[dir]);
    if (fr.empty()) return false;
    return drawFrame(fr[0], c, size / std::max(1.0f, fr[0].fw), alpha, angle);
}

bool PsychUi::strum(int dir, Vector2 c, float size, int state, float alpha, float angle, double t) const {
    if (!active_ || dir < 0 || dir > 3) return false;
    const std::vector<Frame> idle = frames(kArrows[dir]);
    if (idle.empty()) return false;
    const float scale = size / std::max(1.0f, idle[0].fw);
    if (state == 2) {
        const std::vector<Frame> hit = frames(std::string(kDirs[dir]) + " confirm");
        if (!hit.empty()) return drawFrame(hit[size_t(int(t * 24.0) % int(hit.size()))], c, scale, alpha, angle);
    } else if (state == 1) {
        const std::vector<Frame> press = frames(std::string(kDirs[dir]) + " press");
        if (!press.empty()) return drawFrame(press[0], c, scale, alpha, angle);
    }
    return drawFrame(idle[0], c, scale, alpha, angle);
}

bool PsychUi::hold(int dir, float x, float y0, float y1, float width, float alpha) const {
    if (!active_ || dir < 0 || dir > 3 || y0 == y1) return false;
    const std::vector<Frame> piece = frames(std::string(kColors[dir]) + " hold piece");
    std::vector<Frame> end = frames(std::string(kColors[dir]) + " hold end");
    if (end.empty() && dir == 0) end = frames("pruple end hold");  // the original game's spelling
    const std::vector<Frame> noteFr = frames(kColors[dir]);
    if (piece.empty() || end.empty() || noteFr.empty()) return false;
    const float scale = (width / 0.3f) / std::max(1.0f, noteFr[0].fw);  // callers pass 0.3 x the note size
    const Frame& p = piece[0];
    const Frame& e = end[0];
    const float pw = p.src.width * scale, eh = e.src.height * scale;
    const bool down = y1 > y0;
    const float len = std::fabs(y1 - y0);
    const float capStart = down ? y1 - std::min(eh, len) : y1 + std::min(eh, len);
    const Color tint = gfx::fade(WHITE, alpha * 0.6f);
    // Body: the piece stretched from the note to the cap.
    const float bodyLen = std::max(0.0f, len - eh);
    if (bodyLen > 0.5f) {
        const float top = down ? y0 : capStart;
        DrawTexturePro(notes_, p.src, {x - pw * 0.5f, top, pw, bodyLen}, {0, 0}, 0, tint);
    }
    // Cap (flipped when the notes come from below).
    Rectangle src = e.src;
    if (!down) src.height = -src.height;
    const float ch = std::min(eh, len);
    DrawTexturePro(notes_, src, {x - e.src.width * scale * 0.5f, down ? capStart : y1, e.src.width * scale, ch}, {0, 0}, 0, tint);
    return true;
}

bool PsychUi::rating(int which, Vector2 c, float scale, float alpha) const {
    if (!active_ || which < 0 || which > 3 || !ratings_[which].id) return false;
    const Texture2D& t = ratings_[which];
    const float w = t.width * scale, h = t.height * scale;
    DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {c.x - w * 0.5f, c.y - h * 0.5f, w, h}, {0, 0}, 0, gfx::fade(WHITE, alpha));
    return true;
}

bool PsychUi::combo(int value, Vector2 c, float scale, float alpha) const {
    if (!active_ || !nums_[0].id) return false;
    std::string digits = std::to_string(std::max(0, value));
    while (digits.size() < 3) digits = "0" + digits;  // Psych shows at least three digits
    const float step = 43.0f * scale / 0.5f;
    float x = c.x - step * float(digits.size() - 1) * 0.5f;
    for (char d : digits) {
        const Texture2D& t = nums_[d - '0'];
        if (t.id) {
            const float w = t.width * scale, h = t.height * scale;
            DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {x - w * 0.5f, c.y - h * 0.5f, w, h}, {0, 0}, 0,
                           gfx::fade(WHITE, alpha));
        }
        x += step;
    }
    return true;
}

bool PsychUi::countdown(int i, Vector2 c, float alpha) const {
    if (!active_ || i < 0 || i > 2 || !count_[i].id) return false;
    const Texture2D& t = count_[i];
    DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {c.x - t.width * 0.5f, c.y - t.height * 0.5f, float(t.width), float(t.height)},
                   {0, 0}, 0, gfx::fade(WHITE, alpha));
    return true;
}

bool PsychUi::healthBar(Rectangle bar, Color left, Color right, float leftFrac, float alpha) const {
    if (!active_ || !bar_.id) return false;
    // healthBar.png is the frame; the colours fill it 4 pixels in.
    DrawTexturePro(bar_, {0, 0, float(bar_.width), float(bar_.height)}, {bar.x - 4, bar.y - 4, bar.width + 8, bar.height + 8}, {0, 0}, 0,
                   gfx::fade(WHITE, alpha));
    const float split = bar.x + bar.width * leftFrac;
    DrawRectangleRec({bar.x, bar.y, split - bar.x, bar.height}, gfx::fade(left, alpha));
    DrawRectangleRec({split, bar.y, bar.x + bar.width - split, bar.height}, gfx::fade(right, alpha));
    return true;
}

}  // namespace gh
