// Psych-style scene model for mod songs: sprites, texts, characters, cameras,
// strums, tweens and timers, with Flixel's positioning rules. Pure logic
// (no rendering), so scripts and stages can be tested headless;
// scene_draw.cpp renders it with raylib.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "psych.hpp"

namespace gh::scene {

using Value = std::variant<std::monostate, bool, double, std::string>;
double toNum(const Value& v, double def = 0);
bool toBool(const Value& v, bool def = false);
std::string toStr(const Value& v);

constexpr float kW = 1280, kH = 720;

bool readFile(const std::string& path, std::string& out);  // binary-safe, UTF-8 paths

// File lookup across content roots (the song's mod first), case-insensitive
// so mods made on Windows also work on Linux and macOS.
class Files {
public:
    void setRoots(std::vector<std::string> roots);
    // The other installed mods' asset folders: find() looks there when the
    // song's own roots do not have a file (mods reuse each other's characters,
    // stages, images and sounds; a borrowed stage or character brings its
    // script). list() never does, so another mod's global scripts/ do not run
    // in this one.
    void setSharedRoots(std::vector<std::string> roots);
    const std::vector<std::string>& roots() const { return roots_; }
    const std::vector<std::string>& sharedRoots() const { return shared_; }
    std::string find(const std::string& rel) const;  // full path or ""
    // Files directly inside relDir (across roots, first root wins per name).
    std::vector<std::string> list(const std::string& relDir, const std::string& ext) const;

private:
    std::string resolveIn(const std::string& root, const std::string& rel) const;
    const std::vector<std::string>& dirEntries(const std::string& dir) const;
    std::vector<std::string> roots_, shared_;
    mutable std::map<std::string, std::vector<std::string>> dirCache_;
    mutable std::map<std::string, std::string> sharedFound_;  // rel path -> file ("" = none) in the shared roots
};

struct Graphic {
    std::vector<std::string> files;          // PNG paths, one per texture
    std::vector<std::pair<int, int>> sizes;  // texture sizes
    std::vector<psych::AtlasFrame> frames;
    bool solid = false;
    uint32_t solidColor = 0xFFFFFFFF;        // ARGB
};
using GraphicPtr = std::shared_ptr<const Graphic>;

class GraphicCache {
public:
    explicit GraphicCache(const Files& f) : files_(f) {}
    GraphicPtr image(const std::string& key, int gridW = 0, int gridH = 0);  // plain PNG (optionally a grid)
    GraphicPtr atlas(const std::string& keys);  // Sparrow/Packer; "a,b" merges several
    GraphicPtr solid(int w, int h, uint32_t argb);
    std::vector<std::string> missing;           // keys not found (for the debug log)
    std::vector<std::string> pngFiles() const;  // every texture referenced so far

private:
    bool loadPng(const std::string& key, std::string& path, int& w, int& h);
    const Files& files_;
    std::map<std::string, GraphicPtr> cache_;
};

enum class Cam : uint8_t { Game = 0, Hud = 1, Other = 2 };

// A shader on a sprite or a camera. Like Psych's FlxRuntimeShader, every
// sprite gets its own instance (its own uniform values) of a named source.
struct ShaderInstance {
    std::string name;
    std::map<std::string, std::vector<float>, std::less<>> floats;
    std::map<std::string, std::vector<int>, std::less<>> ints;  // ints and bools
    std::map<std::string, std::string, std::less<>> samplers;   // image keys
    float timeRate = 0;  // built-in effects: their time uniform advances by elapsed * timeRate
    std::string timeName = "uTime";  // that uniform (JS Engine's VCR effect: iTime, blocked glitch: time)
};
using ShaderPtr = std::shared_ptr<ShaderInstance>;
struct ShaderSource {
    std::string frag, vert;
};
enum class Kind : uint8_t { Sprite, Text, Character, Marker };

struct Anim {
    std::string name;
    std::vector<int> frames;
    double fps = 24;
    bool loop = false;
    bool hasOffset = false;
    float offX = 0, offY = 0;
};

struct TextStyle {
    std::string text, font;
    float size = 16;
    uint32_t color = 0xFFFFFFFF, borderColor = 0xFF000000;
    float borderSize = 0;
    int align = 0;  // 0 left, 1 center, 2 right
    float fieldWidth = 0;
    bool italic = false;
};

class Sprite {
public:
    virtual ~Sprite() = default;
    std::string tag;
    Kind kind = Kind::Sprite;
    GraphicPtr graphic;
    float x = 0, y = 0, width = 0, height = 0, frameW = 0, frameH = 0;
    float offsetX = 0, offsetY = 0, originX = 0, originY = 0;
    float scaleX = 1, scaleY = 1, scrollX = 1, scrollY = 1;
    float angle = 0, alpha = 1;
    uint32_t color = 0xFFFFFF;
    bool flipX = false, flipY = false, visible = true, antialiasing = true, active = true;
    Cam cam = Cam::Game;
    int blend = 0;  // 0 normal, 1 add, 2 multiply, 3 screen, 4 subtract
    bool inScene = false;
    ShaderPtr shader;

    std::vector<Anim> anims;
    int cur = -1, animFrame = 0;
    double animTime = 0;
    bool finished = true, paused = false, reversed = false;
    int frame = 0;

    TextStyle text;

    void setGraphic(GraphicPtr g);  // Flixel frames setter: sizes from frame 0, origin centred
    void updateHitbox();
    void setGraphicSize(float w, float h);
    void centerOrigin() { originX = frameW * 0.5f; originY = frameH * 0.5f; }
    void screenCenter(bool cx, bool cy);
    void addAnim(Anim a);
    bool play(const std::string& name, bool force = false, bool rev = false, int startFrame = 0);
    void finishAnim();
    void updateAnim(double dt);
    const Anim* curAnim() const { return cur >= 0 && cur < int(anims.size()) ? &anims[size_t(cur)] : nullptr; }
    std::string curAnimName() const { const Anim* a = curAnim(); return a ? a->name : ""; }
    bool hasAnim(std::string_view n) const;
    Anim* anim(std::string_view n);

    virtual bool get(std::string_view field, Value& out) const;
    virtual bool set(std::string_view field, const Value& v);
};
using SpritePtr = std::shared_ptr<Sprite>;

class Character : public Sprite {
public:
    std::string name;
    psych::CharacterDef def;
    bool isPlayer = false;
    bool builtin = false;  // no JSON found: drawn with the game's own figures
    int builtinId = 0;
    double holdTimer = 0, heyTimer = 0, now = 0, lastAnimAt = -10, lastDanceAt = -10;
    bool specialAnim = false, stunned = false, skipDance = false, danceIdle = false, danced = false, hasMiss = false;
    int danceEvery = 2;
    std::string idleSuffix, lastAnim;
    float camX = 0, camY = 0, posX = 0, posY = 0;
    std::string icon = "face";
    uint32_t barColor = 0xA1A1A1;
    double singDuration = 4;
    float groupX = 0, groupY = 0;  // boyfriendGroup / dadGroup / gfGroup position

    void recalcDance(bool initial);
    void dance();
    void playAnim(const std::string& n, bool force = false, bool rev = false, int startFrame = 0);
    // Psych Character.update: hey/special/miss endings, hold timer, -loop anims.
    void updateChar(double dt, double stepMs, bool playerHolding);
    bool get(std::string_view field, Value& out) const override;
    bool set(std::string_view field, const Value& v) override;
};
using CharacterPtr = std::shared_ptr<Character>;

struct Camera {
    float zoom = 1, angle = 0, alpha = 1, x = 0, y = 0, scrollX = 0, scrollY = 0;
    bool visible = true;
    float shakeIntensity = 0, shakeTime = 0, shakeX = 0, shakeY = 0;
    uint32_t flashColor = 0xFFFFFFFF;
    float flashDur = 0, flashTime = 0;
    uint32_t fadeColor = 0xFF000000;
    float fadeDur = 0, fadeTime = 0, fadeAlpha = 0;
    bool fadeIn = false;
    uint32_t bgColor = 0;
    std::vector<ShaderPtr> filters;  // camera shaders (ShaderFilter), applied in order
    void update(double dt, unsigned seed);
    bool get(std::string_view field, Value& out) const;
    bool set(std::string_view field, const Value& v);
};

struct Strum {
    float x = 0, y = 0, alpha = 1, angle = 0, scaleX = 0.7f, scaleY = 0.7f, direction = 90;
    bool visible = true, downScroll = false;
    float defX = 0, defY = 0;
    bool get(std::string_view field, Value& out) const;
    bool set(std::string_view field, const Value& v);
};

struct Tween {
    std::string tag, target;
    struct Field { std::string name; double from = 0, to = 0; bool init = false; };
    std::vector<Field> fields;
    bool isColor = false;
    uint32_t c0 = 0, c1 = 0;
    double dur = 1, t = 0, delay = 0;
    std::string ease = "linear", onComplete;
    int script = -1;
};

struct Timer {
    std::string tag;
    double time = 1, t = 0;
    int loops = 1, done = 0;
    int script = -1;
};

struct Completion {
    bool timer = false;
    int script = -1;
    std::string tag, fn;
    int loops = 0, left = 0;
};

class Scene {
public:
    Scene() : gfx(files) {}
    Files files;
    GraphicCache gfx;

    std::map<std::string, SpritePtr, std::less<>> objects;  // Lua tags and named stage objects
    std::vector<SpritePtr> order;                            // draw order (characters and markers included)
    CharacterPtr bf, dad, gf;
    SpritePtr gfMarker, hudMarker;
    // HUD elements exposed to scripts (only visibility/alpha/offsets are used)
    std::map<std::string, SpritePtr, std::less<>> hud;

    Camera camGame, camHud, camOther;
    float followX = 0, followY = 0;
    bool camForced = false, camZooming = false;
    double defaultCamZoom = 1.05, cameraSpeed = 1, camZoomingMult = 1, camZoomingDecay = 1;
    double gfSpeed = 1;
    std::vector<Strum> strums;  // opponent 0..K-1 then player K..2K-1
    int keys = 4;
    std::vector<Tween> tweens;
    std::vector<Timer> timers;
    std::vector<Completion> completions;  // consumed by the script host
    psych::StageDef stage;
    std::map<std::string, Value, std::less<>> vars;  // setVar/getVar
    std::map<std::string, ShaderSource, std::less<>> shaderSources;  // initLuaShader
    std::map<std::string, ShaderPtr, std::less<>> cameraShaders;     // setCameraShader, by index name
    double now = 0;
    unsigned frameSeed = 1;

    // Lookups
    Sprite* find(std::string_view name) const;
    Character* character(std::string_view name) const;  // bf/boyfriend/0, dad/opponent/1, gf/girlfriend/2
    Camera* camera(std::string_view name);
    static Cam camFromName(std::string_view name);

    // Ordering (Psych add/insert semantics)
    void addBehindCharacters(const SpritePtr& s);  // addLuaSprite(tag, false)
    void addFront(const SpritePtr& s);             // addLuaSprite(tag, true)
    void remove(const SpritePtr& s);
    int indexOf(std::string_view name) const;
    void setOrder(std::string_view name, int pos);

    // Properties by path: "bf.x", "camGame.zoom", "tag.scale.x",
    // "playerStrums.members[0].x", "gf.animation.curAnim.name"
    bool getProp(std::string_view path, Value& out) const;
    bool setProp(std::string_view path, const Value& v);
    Strum* strumByGroup(std::string_view group, int index);

    // Tweens and timers
    void tween(Tween tw);
    void cancelTween(std::string_view tag);
    void timer(Timer t);
    void cancelTimer(std::string_view tag);

    void update(double dt, double stepMs, const bool playerHolding);
    // Psych camera follow targets
    void focusOn(int who);  // 0 bf, 1 dad, 2 gf
    void cameraTarget(float& x, float& y, int who) const;
};

}  // namespace gh::scene
