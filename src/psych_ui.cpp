#include "psych_ui.hpp"

#include "rlgl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <system_error>

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

// Psych 0.7's healthBar.png and timeBar.png are frames with a see-through
// middle, drawn over the bar's colours; 0.6's are solid backgrounds under them.
bool seeThroughMiddle(const std::string& path) {
    if (path.empty()) return true;
    Image img = LoadImage(path.c_str());
    if (!img.data) return true;
    const Color c = GetImageColor(img, img.width / 2, img.height / 2);
    UnloadImage(img);
    return c.a < 128;
}

// A TrueType font's em square and line (hhea ascent and descent), to size
// text the way Flash does.
bool fontMetrics(const std::string& path, float& upm, float& ascent, float& descent) {
    int size = 0;
    unsigned char* d = LoadFileData(path.c_str(), &size);
    if (!d) return false;
    auto u16 = [&](int o) { return o + 2 <= size ? int(d[o]) << 8 | d[o + 1] : 0; };
    auto s16 = [&](int o) { return int(int16_t(uint16_t(u16(o)))); };
    bool ok = false;
    int head = -1, hhea = -1;
    const int tables = u16(4);
    for (int i = 0; i < tables && 12 + 16 * i + 16 <= size; ++i) {
        const unsigned char* t = d + 12 + 16 * i;
        const int off = int(uint32_t(t[8]) << 24 | uint32_t(t[9]) << 16 | uint32_t(t[10]) << 8 | t[11]);
        if (std::equal(t, t + 4, "head")) head = off;
        if (std::equal(t, t + 4, "hhea")) hhea = off;
    }
    if (head >= 0 && hhea >= 0 && head + 20 <= size && hhea + 8 <= size) {
        upm = float(u16(head + 18));
        ascent = float(s16(hhea + 4));
        descent = float(s16(hhea + 6));
        ok = upm > 0 && ascent - descent > 0;
    }
    UnloadFileData(d);
    return ok;
}

const char* kColors[4] = {"purple", "blue", "green", "red"};

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

void PsychUi::setPalette(const uint32_t colors[4][3]) {
    for (int d = 0; d < 4; ++d)
        for (int k = 0; k < 3; ++k) palette_[d][k] = colors[d][k] & 0xFFFFFFu;
}

void PsychUi::setSmooth(bool smooth) {
    smooth_ = smooth;
    const int filter = smooth ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT;
    for (Texture2D* t : {&notes_, &bar_, &timeBar_, &alphabet_})
        if (t->id) SetTextureFilter(*t, filter);
    for (Texture2D& t : ratings_)
        if (t.id) SetTextureFilter(t, filter);
    for (Texture2D& t : nums_)
        if (t.id) SetTextureFilter(t, filter);
    for (Texture2D& t : count_)
        if (t.id) SetTextureFilter(t, filter);
}

std::vector<std::string> psychNoteSkins(const std::string& folder) {
    std::vector<std::string> out;
    if (folder.empty()) return out;
    std::string root = folder;
    if (root.back() != '/' && root.back() != '\\') root += '/';
    namespace fs = std::filesystem;
    for (const char* sub : {"", "assets/", "assets/shared/", "shared/", "mods/"}) {
        std::error_code ec;
        const std::string dir = root + sub + "images/noteSkins";
        for (const auto& e : fs::directory_iterator(fs::path(std::u8string(dir.begin(), dir.end())), ec)) {
            const auto u = e.path().filename().u8string();
            const std::string n(u.begin(), u.end());
            const std::string pre = "NOTE_assets-";
            if (n.size() > pre.size() + 4 && n.rfind(pre, 0) == 0 && endsWithI(n, ".png")) {
                const std::string skin = n.substr(pre.size(), n.size() - pre.size() - 4);
                if (std::find(out.begin(), out.end(), skin) == out.end()) out.push_back(skin);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

PsychUi& psychUi() {
    static PsychUi ui;
    return ui;
}

int PsychUi::load(const std::string& folder, std::string& report) { return load(folder, report, {}); }

int PsychUi::load(const std::string& folder, std::string& report, const std::vector<std::string>& firstRoots) {
    unload();
    root_ = folder;
    if (!root_.empty() && root_.back() != '/' && root_.back() != '\\') root_ += '/';
    // The engine's own folder, its assets/ folder or the library folders
    // inside it: wherever the player pointed, the same names are found.
    scene::Files f;
    std::vector<std::string> roots = firstRoots;
    for (const char* sub : {"", "assets/", "assets/shared/", "assets/preload/", "assets/embed/", "shared/", "preload/", "../",
                            "../shared/", "../preload/"})
        roots.push_back(root_ + sub);
    f.setRoots(roots);
    int found = 0;
    std::vector<std::string> missing;

    std::string notePng;
    if (!skin_.empty())
        notePng = findAny(f, {"images/noteSkins/NOTE_assets-" + skin_ + ".png", "images/noteskins/NOTE_assets-" + skin_ + ".png"});
    if (notePng.empty())
        notePng = findAny(f, {"images/noteskins/NOTE_assets.png", "images/noteSkins/NOTE_assets.png", "images/NOTE_assets.png"});
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
            paletteShader_ = LoadShaderFromMemory(nullptr, rlGetVersion() == RL_OPENGL_ES_20 ? kPaletteGles : kPaletteGl330);
            locR_ = GetShaderLocation(paletteShader_, "r");
            locG_ = GetShaderLocation(paletteShader_, "g");
            locB_ = GetShaderLocation(paletteShader_, "b");
            if (!paletteShader_.id || paletteShader_.id == rlGetShaderIdDefault()) template_ = false;
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
    const std::string barPng = findAny(f, {"images/healthBar.png"}), timePng = findAny(f, {"images/timeBar.png"});
    bar_ = loadTex(barPng);
    barOnTop_ = seeThroughMiddle(barPng);
    if (bar_.id) ++found;
    timeBar_ = loadTex(timePng);
    timeOnTop_ = timePng.empty() ? barOnTop_ : seeThroughMiddle(timePng);
    if (timeBar_.id) ++found;
    // Alphabet: the bold letters of the pause menu and other menus.
    const std::string abc = findAny(f, {"images/alphabet.png"});
    if (!abc.empty()) {
        std::string xml;
        psych::Atlas atlas;
        if (scene::readFile(abc.substr(0, abc.size() - 4) + ".xml", xml) && atlas.parseSparrow(xml)) {
            alphabet_ = loadTex(abc);
            for (const psych::AtlasFrame& a : atlas.frames) {
                Frame fr;
                fr.src = {a.x, a.y, a.w, a.h};
                fr.fx = a.fx;
                fr.fy = a.fy;
                fr.fw = a.fw > 0 ? a.fw : a.w;
                fr.fh = a.fh > 0 ? a.fh : a.h;
                fr.rotated = a.rotated;
                letters_.push_back({a.name, fr});
            }
            if (alphabet_.id) ++found;
        }
    }
    if (!alphabet_.id) missing.push_back("alphabet");
    const std::string ttf = findAny(f, {"fonts/vcr.ttf"});
    if (!ttf.empty()) {
        font_ = LoadFontEx(ttf.c_str(), 48, nullptr, 0);
        if (font_.texture.id) {
            SetTextureFilter(font_.texture, TEXTURE_FILTER_BILINEAR);
            ++found;
        }
        float upm = 0, asc = 0, desc = 0;
        fontScale_ = 1.0f;
        fontAscent_ = 1.0f;
        if (fontMetrics(ttf, upm, asc, desc)) {
            fontScale_ = (asc - desc) / upm;  // vcr.ttf: 1800 / 2048
            fontAscent_ = asc / upm;
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
    pauseMusic_ = findAny(f, {"music/breakfast.ogg", "music/breakfast.mp3"});
    if (!pauseMusic_.empty()) ++found;
    teaTime_ = findAny(f, {"music/tea-time.ogg", "music/tea-time.mp3"});
    if (!teaTime_.empty()) ++found;

    if (!smooth_) setSmooth(false);
    report = std::to_string(found) + " pieces found";
    if (!missing.empty()) {
        report += "; missing: ";
        for (size_t i = 0; i < missing.size(); ++i) report += (i ? ", " : "") + missing[i];
    }
    return found;
}

void PsychUi::unload() {
    unloadTex(notes_);
    if (paletteShader_.id && paletteShader_.id != rlGetShaderIdDefault() && IsWindowReady()) UnloadShader(paletteShader_);
    paletteShader_ = Shader{};
    template_ = false;
    for (Texture2D& t : ratings_) unloadTex(t);
    for (Texture2D& t : nums_) unloadTex(t);
    for (Texture2D& t : count_) unloadTex(t);
    unloadTex(bar_);
    unloadTex(timeBar_);
    unloadTex(alphabet_);
    letters_.clear();
    pauseMusic_.clear();
    teaTime_.clear();
    fontScale_ = fontAscent_ = 1.0f;
    barOnTop_ = timeOnTop_ = true;
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
    BeginShaderMode(paletteShader_);
    const int locs[3] = {locR_, locG_, locB_};
    for (int k = 0; k < 3; ++k) {
        const unsigned c = palette_[dir][k];
        const float v[3] = {float((c >> 16) & 255) / 255.0f, float((c >> 8) & 255) / 255.0f, float(c & 255) / 255.0f};
        if (locs[k] >= 0) SetShaderValue(paletteShader_, locs[k], v, SHADER_UNIFORM_VEC3);
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

bool PsychUi::combo(long long value, Vector2 c, float scale, float alpha) const {
    if (!active_ || !nums_[0].id) return false;
    std::string digits = std::to_string(std::max(0LL, value));
    while (digits.size() < 3) digits = "0" + digits;  // Psych shows at least three digits
    // Many digits (spam charts) shrink to stay within 900 pixels.
    if (43.0f * scale / 0.5f * float(digits.size()) > 900.0f) scale = 900.0f * 0.5f / (43.0f * float(digits.size()));
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

Vector2 PsychUi::ratingSize(int which) const {
    if (which < 0 || which > 3 || !ratings_[which].id) return {0, 0};
    return {float(ratings_[which].width), float(ratings_[which].height)};
}

Vector2 PsychUi::numSize(int digit) const {
    if (digit < 0 || digit > 9 || !nums_[digit].id) return {0, 0};
    return {float(nums_[digit].width), float(nums_[digit].height)};
}

static void drawAt(const Texture2D& t, float x, float y, float scale, float alpha) {
    DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {x, y, t.width * scale, t.height * scale}, {0, 0}, 0,
                   gfx::fade(WHITE, alpha));
}

bool PsychUi::ratingAt(int which, float x, float y, float scale, float alpha) const {
    if (!active_ || which < 0 || which > 3 || !ratings_[which].id) return false;
    drawAt(ratings_[which], x, y, scale, alpha);
    return true;
}

bool PsychUi::numAt(int digit, float x, float y, float scale, float alpha) const {
    if (!active_ || digit < 0 || digit > 9 || !nums_[digit].id) return false;
    drawAt(nums_[digit], x, y, scale, alpha);
    return true;
}

std::string PsychUi::pauseMusic(int which) const {
    if (which <= 0) return {};
    if (which == 2) return teaTime_.empty() ? pauseMusic_ : teaTime_;
    return pauseMusic_.empty() ? teaTime_ : pauseMusic_;
}

bool PsychUi::countdown(int i, Vector2 c, float alpha) const {
    if (!active_ || i < 0 || i > 2 || !count_[i].id) return false;
    const Texture2D& t = count_[i];
    DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {c.x - t.width * 0.5f, c.y - t.height * 0.5f, float(t.width), float(t.height)},
                   {0, 0}, 0, gfx::fade(WHITE, alpha));
    return true;
}

Vector2 PsychUi::healthBarSize() const {
    if (!active_ || !bar_.id) return {0, 0};
    return {float(bar_.width), float(bar_.height)};
}

// Psych's Bar: the left colour fills leftFrac of the bar, the right colour
// the rest; 0.7 draws healthBar.png over them, 0.6 under them.
bool PsychUi::healthBar(Rectangle frame, Color left, Color right, float leftFrac, float alpha, float frameAlpha) const {
    if (!active_ || !bar_.id) return false;
    if (frameAlpha < 0) frameAlpha = alpha;
    const float in = barInset();
    const Rectangle bar{frame.x + in, frame.y + in, frame.width - 2 * in, frame.height - 2 * in};
    const Rectangle src{0, 0, float(bar_.width), float(bar_.height)};
    if (!barOnTop_) DrawTexturePro(bar_, src, frame, {0, 0}, 0, gfx::fade(WHITE, frameAlpha));
    const float split = bar.width * std::clamp(leftFrac, 0.0f, 1.0f);
    DrawRectangleRec({bar.x, bar.y, split, bar.height}, gfx::fade(left, alpha));
    DrawRectangleRec({bar.x + split, bar.y, bar.width - split, bar.height}, gfx::fade(right, alpha));
    if (barOnTop_) DrawTexturePro(bar_, src, frame, {0, 0}, 0, gfx::fade(WHITE, frameAlpha));
    return true;
}

Vector2 PsychUi::timeBarSize() const {
    return timeBar_.id ? Vector2{float(timeBar_.width), float(timeBar_.height)} : Vector2{400, 19};
}

bool PsychUi::timeBar(Rectangle frame, float frac, float alpha, float frameAlpha) const {
    if (!active_) return false;
    if (frameAlpha < 0) frameAlpha = alpha;
    // Psych: timeBar.png around a white filled part and a black empty part.
    const float in = timeOnTop_ ? 3.0f : 4.0f;
    const Rectangle bar{frame.x + in, frame.y + in, frame.width - 2 * in, frame.height - 2 * in};
    const Rectangle src{0, 0, float(timeBar_.width), float(timeBar_.height)};
    if (!timeOnTop_ || !timeBar_.id) {
        if (timeBar_.id) DrawTexturePro(timeBar_, src, frame, {0, 0}, 0, gfx::fade(WHITE, frameAlpha));
        else DrawRectangleRec(frame, gfx::fade(BLACK, frameAlpha));
    }
    const float fill = bar.width * std::clamp(frac, 0.0f, 1.0f);
    DrawRectangleRec({bar.x, bar.y, fill, bar.height}, gfx::fade(WHITE, alpha));
    DrawRectangleRec({bar.x + fill, bar.y, bar.width - fill, bar.height}, gfx::fade(BLACK, alpha));
    if (timeOnTop_ && timeBar_.id) DrawTexturePro(timeBar_, src, frame, {0, 0}, 0, gfx::fade(WHITE, frameAlpha));
    return true;
}

// FlxText: the text sits 2 pixels in from its box, and its OUTLINE border is
// the text drawn in black around itself, in rings a pixel apart (borderSize
// 2: rings at 1 and 2 pixels; 1.25: one ring at 1.25).
bool PsychUi::text(const std::string& s, float x, float y, float size, Color c, int align, float border) const {
    if (!active_ || !font_.texture.id) return false;
    const float px = size * fontScale_;
    const float w = MeasureTextEx(font_, s.c_str(), px, 0).x;
    const float left = align == 0 ? x - w * 0.5f : align < 0 ? x + 2 : x - 2 - w;
    const float top = y + 2;
    if (border > 0) {
        const Color edge{0, 0, 0, c.a};
        const int rings = std::max(1, int(border));
        for (int r = 1; r <= rings; ++r) {
            const float d = border * float(r) / float(rings);
            for (const Vector2 o : {Vector2{-d, -d}, Vector2{0, -d}, Vector2{d, -d}, Vector2{d, 0}, Vector2{d, d}, Vector2{0, d},
                                    Vector2{-d, d}, Vector2{-d, 0}})
                DrawTextEx(font_, s.c_str(), {left + o.x, top + o.y}, px, 0, edge);
        }
    }
    DrawTextEx(font_, s.c_str(), {left, top}, px, 0, c);
    return true;
}

float PsychUi::textWidth(const std::string& s, float size) const {
    if (!font_.texture.id) return 0;
    return MeasureTextEx(font_, s.c_str(), size * fontScale_, 0).x;
}

float PsychUi::lineHeight(float size) const { return size * fontScale_; }

// Alphabet's bold letters: "A bold0000" in the original game, "a bold
// instance 10000" in Psych 0.7 and later; symbols go by the character itself
// ("& bold") or by name ("question bold", "question mark bold"...).
const PsychUi::Frame* PsychUi::letter(char c) const {
    std::vector<std::string> wants;
    if (std::isalpha((unsigned char)c)) {
        wants = {std::string(1, char(std::toupper((unsigned char)c))) + " bold",
                 std::string(1, char(std::tolower((unsigned char)c))) + " bold"};
    } else if (std::isdigit((unsigned char)c)) {
        wants = {std::string(1, c) + " bold"};
    } else {
        static const std::pair<char, std::vector<const char*>> names[] = {
            {'?', {"question", "question mark"}}, {'!', {"exclamation", "exclamation point"}}, {'.', {"period"}},
            {',', {"comma"}}, {'\'', {"apostrophe"}}, {'"', {"quote"}}, {'-', {"dash"}}, {'&', {"amp"}},
            {'(', {"start parentheses"}}, {')', {"end parentheses"}}, {'/', {"forward slash"}}};
        wants = {std::string(1, c) + " bold"};
        for (const auto& [ch, list] : names)
            if (ch == c)
                for (const char* n : list) wants.push_back(std::string(n) + " bold");
    }
    for (const std::string& want : wants)
        for (const auto& [name, fr] : letters_) {
            if (name.size() <= want.size() || name.compare(0, want.size(), want) != 0) continue;
            const std::string rest = name.substr(want.size());
            const bool frameNo = std::all_of(rest.begin(), rest.end(), [](char d) { return std::isdigit((unsigned char)d); });
            if (frameNo || rest.rfind(" instance", 0) == 0) return &fr;
        }
    return nullptr;
}

float PsychUi::alphabet(const std::string& s, float x, float y, float height, float alpha) const {
    if (!active_ || !alphabet_.id) return -1;
    const Frame* ref = letter('A');
    const float scale = height / std::max(1.0f, ref ? ref->fh : 70.0f);
    float pen = x;
    for (char c : s) {
        if (c == ' ') { pen += 40.0f * scale; continue; }
        const Frame* f = letter(c);
        if (!f) { pen += 40.0f * scale; continue; }
        // Letters sit on a shared baseline: the bottom of their untrimmed box.
        const float w = f->rotated ? f->src.height : f->src.width, h = f->rotated ? f->src.width : f->src.height;
        const float dx = pen - f->fx * scale, dy = y + (height - f->fh * scale) - f->fy * scale;
        if (f->rotated)
            DrawTexturePro(alphabet_, f->src, {dx + w * scale * 0.5f, dy + h * scale * 0.5f, f->src.width * scale, f->src.height * scale},
                           {f->src.width * scale * 0.5f, f->src.height * scale * 0.5f}, -90.0f, gfx::fade(WHITE, alpha));
        else
            DrawTexturePro(alphabet_, f->src, {dx, dy, w * scale, h * scale}, {0, 0}, 0, gfx::fade(WHITE, alpha));
        pen += f->fw * scale;
    }
    return pen - x;
}

// ------------------------------------------------- engine copies on disk --

namespace {

namespace fs = std::filesystem;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8str(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The same library folders PsychUi::load() looks in.
scene::Files libraryRoots(const std::string& folder) {
    std::string root = folder;
    if (!root.empty() && root.back() != '/' && root.back() != '\\') root += '/';
    std::vector<std::string> roots;
    for (const char* sub : {"", "assets/", "assets/shared/", "assets/preload/", "assets/embed/", "shared/", "preload/"})
        roots.push_back(root + sub);
    scene::Files f;
    f.setRoots(roots);
    return f;
}

// A folder holding an engine build: its assets/ has a note skin.
bool isEngineFolder(const fs::path& d) {
    static const char* const kMarks[] = {
        "assets/shared/images/NOTE_assets.png", "assets/shared/images/noteSkins/NOTE_assets.png",
        "assets/shared/images/noteskins/NOTE_assets.png", "assets/images/NOTE_assets.png",
        "assets/images/noteSkins/NOTE_assets.png", "assets/preload/images/NOTE_assets.png",
    };
    std::error_code ec;
    for (const char* m : kMarks)
        if (fs::is_regular_file(d / m, ec)) return true;
    return false;
}

// Folders that hold a mod's or an engine's content, never another engine.
bool skipWhenSearching(const std::string& name) {
    static const char* const kSkip[] = {"assets", "images", "songs", "data", "music", "sounds", "characters", "stages",
                                        "videos", "shaders", "scripts", "weeks", "fonts", "custom_events", "custom_notetypes",
                                        "manifest", "plugins", "lime", "flixel", "openfl", "achievements", "replays"};
    if (name.empty() || name[0] == '.') return true;
    const std::string low = toLower(name);
    for (const char* s : kSkip)
        if (low == s) return true;
    return false;
}

}  // namespace

bool hasPsychPieces(const std::vector<std::string>& roots) {
    scene::Files f;
    f.setRoots(roots);
    for (const char* rel : {"images/NOTE_assets.png", "images/noteSkins/NOTE_assets.png", "images/noteskins/NOTE_assets.png",
                            "images/healthBar.png", "images/timeBar.png", "fonts/vcr.ttf", "images/sick.png",
                            "images/ratings/default/sick.png", "images/num0.png", "images/alphabet.png", "images/ready.png",
                            "sounds/scrollMenu.ogg", "sounds/confirmMenu.ogg", "sounds/missnote1.ogg", "music/breakfast.ogg"})
        if (!f.find(rel).empty()) return true;
    return false;
}

int psychFolderScore(const std::string& folder) {
    const scene::Files f = libraryRoots(folder);
    int score = 0;
    if (!findAny(f, {"images/noteskins/NOTE_assets.png", "images/noteSkins/NOTE_assets.png", "images/NOTE_assets.png"}).empty())
        score += 4;
    for (const char* r : {"sick", "good", "bad", "shit"})
        if (!findAny(f, {std::string("images/ratings/default/") + r + ".png", std::string("images/") + r + ".png"}).empty()) ++score;
    if (!findAny(f, {"images/ratings/default/num0.png", "images/num0.png"}).empty()) ++score;
    for (const char* c : {"ready", "set", "go"})
        if (!f.find(std::string("images/") + c + ".png").empty()) ++score;
    for (const char* rel : {"images/healthBar.png", "images/timeBar.png", "fonts/vcr.ttf", "music/freakyMenu.ogg", "music/breakfast.ogg",
                            "sounds/scrollMenu.ogg", "sounds/confirmMenu.ogg", "sounds/cancelMenu.ogg"})
        if (!f.find(rel).empty()) ++score;
    if (!f.find("images/alphabet.png").empty()) score += 2;
    return score;
}

std::vector<PsychFolder> findPsychFolders(const std::string& gameBase) {
    std::vector<PsychFolder> out;
    std::error_code ec;
    const fs::path base = u8path(gameBase.empty() ? std::string("./") : gameBase);
    struct Item {
        fs::path dir;
        int depth;
    };
    std::deque<Item> todo;
    todo.push_back({base / "mods", 4});  // engine builds installed as mod packs
    todo.push_back({base, 3});            // unpacked inside the game's folder
#if !defined(__ANDROID__)
    const fs::path game = fs::weakly_canonical(base, ec);
    if (!ec && game.has_parent_path() && game.parent_path() != game.root_path()) todo.push_back({game.parent_path(), 2});  // or next to it
#endif
    std::vector<fs::path> seen;
    int visited = 0;
    while (!todo.empty() && visited < 6000) {
        const Item it = todo.front();
        todo.pop_front();
        const fs::path canon = fs::weakly_canonical(it.dir, ec);
        if (ec || std::find(seen.begin(), seen.end(), canon) != seen.end()) continue;
        seen.push_back(canon);
        ++visited;
        if (isEngineFolder(it.dir)) {
            const std::string p = u8str(it.dir);
            const int sc = psychFolderScore(p);
            if (sc >= 6) out.push_back({p, sc});
            continue;
        }
        if (it.depth <= 0) continue;
        for (fs::directory_iterator d(it.dir, fs::directory_options::skip_permission_denied, ec), end; !ec && d != end; d.increment(ec)) {
            std::error_code e2;
            if (!d->is_directory(e2)) continue;
            const std::string name = u8str(d->path().filename());
            if (skipWhenSearching(name)) continue;
            todo.push_back({d->path(), it.depth - 1});
        }
    }
    // Most complete first; an engine's own build (its name says engine,
    // psych, slice...) before a mod built on one; then the shorter path.
    auto engineName = [](const std::string& p) {
        const std::string low = toLower(p);
        for (const char* k : {"engine", "psych", "slice", "s2x"})
            if (low.find(k) != std::string::npos) return 1;
        return 0;
    };
    std::stable_sort(out.begin(), out.end(), [&](const PsychFolder& a, const PsychFolder& b) {
        if (a.score != b.score) return a.score > b.score;
        if (engineName(a.path) != engineName(b.path)) return engineName(a.path) > engineName(b.path);
        return a.path.size() < b.path.size();
    });
    return out;
}

}  // namespace gh
