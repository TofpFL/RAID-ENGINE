// Headless tests for the Psych compatibility layer: atlases, character and
// stage JSON, events, the scene model and the Lua API. Also a corpus runner:
// gh_tests --psych-corpus <dir> loads every atlas, character, stage and
// script found under <dir> and reports anything that fails.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "chart.hpp"
#include "common.hpp"
#include "lua_host.hpp"
#include "psych.hpp"
#include "scene.hpp"

using namespace gh;

extern int g_fail, g_pass;
#define PCHECK(cond)                                                             \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static bool pnear(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

namespace {

struct StubGame : ScriptGame {
    double pos = 0, health = 1, score = 0;
    std::vector<std::string> events, prints;
    int countdownStarts = 0;
    double songPosition() const override { return pos; }
    bool getGameProp(std::string_view p, scene::Value& out) override {
        if (p == "health") { out = health; return true; }
        if (p == "songScore") { out = score; return true; }
        if (p == "class:ClientPrefs.downScroll") { out = false; return true; }
        if (p == "notes.length" || p == "unspawnNotes.length" || p == "eventNotes.length") { out = 0.0; return true; }
        return false;
    }
    bool setGameProp(std::string_view p, const scene::Value& v) override {
        if (p == "health") { health = scene::toNum(v); return true; }
        if (p == "songScore") { score = scene::toNum(v); return true; }
        return false;
    }
    void triggerEvent(const std::string& n, const std::string& a, const std::string& b) override {
        events.push_back(n + "|" + a + "|" + b);
    }
    void debugPrint(const std::string& t, uint32_t) override { prints.push_back(t); }
    struct Notice { std::string text; bool error, debugMode; };
    std::vector<Notice> notices;
    void debugNotice(const std::string& t, bool error, bool debugMode) override {
        notices.push_back({t, error, debugMode});
        prints.push_back(t);
    }
    void startCountdown() override { ++countdownStarts; }
    std::string saveRoot;
    std::string saveDir() override { return saveRoot; }
    scene::Scene* scene = nullptr;  // for extra characters (HScript new Character)
    bool makeExtraCharacter(const std::string& tag, const std::string& name, bool isPlayer, float x, float y) override {
        if (!scene) return false;
        auto c = std::make_shared<scene::Character>();
        c->kind = scene::Kind::Character;
        c->name = name;
        c->tag = tag;
        c->builtin = true;
        c->isPlayer = isPlayer;
        c->x = x;
        c->y = y;
        c->width = c->frameW = 400;
        c->height = c->frameH = 400;
        scene->objects[tag] = c;
        return true;
    }
};

const char* kAtlas = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                     "<TextureAtlas imagePath=\"bf.png\">\n"
                     "  <!-- comment <SubTexture name=\"ghost\"/> -->\n"
                     "  <SubTexture name=\"BF idle dance0010\" x=\"0\" y=\"0\" width=\"10\" height=\"20\"/>\n"
                     "  <SubTexture name=\"BF idle dance0002\" x=\"10\" y=\"0\" width=\"10\" height=\"20\" frameX=\"-3\" frameY=\"-4\" frameWidth=\"16\" frameHeight=\"28\"/>\n"
                     "  <SubTexture name=\"BF idle dance0001\" x=\"20\" y=\"0\" width=\"12\" height=\"20\" rotated=\"true\"/>\n"
                     "  <SubTexture name=\"BF NOTE LEFT0000\" x=\"0\" y=\"20\" width=\"8\" height=\"8\"/>\n"
                     "  <SubTexture name=\"Tom &amp; Jerry0000\" x=\"0\" y=\"30\" width=\"4\" height=\"4\"/>\n"
                     "</TextureAtlas>\n";

void testAtlas() {
    psych::Atlas a;
    PCHECK(a.parseSparrow(kAtlas));
    PCHECK(a.frames.size() == 5);
    PCHECK(a.imagePath == "bf.png");
    PCHECK(a.frames[4].name == "Tom & Jerry0000");
    const auto idle = a.byPrefix("BF idle dance");
    PCHECK(idle.size() == 3 && idle[0] == 2 && idle[1] == 1 && idle[2] == 0);  // numeric order 1, 2, 10
    PCHECK(pnear(a.frames[1].fx, -3) && pnear(a.frames[1].fw, 16) && pnear(a.frames[1].fh, 28));
    PCHECK(a.frames[2].rotated && pnear(a.frames[2].fw, 20) && pnear(a.frames[2].fh, 12));
    PCHECK(pnear(a.frames[0].fw, 10));  // no frameWidth: the frame itself
    const auto ind = a.byIndices("BF idle dance", {10, 1, 7});
    PCHECK(ind.size() == 2 && ind[0] == 0 && ind[1] == 2);
    PCHECK(a.byPrefix("nope").empty());
    psych::Atlas p;
    PCHECK(p.parsePacker("idle_0 = 0 0 32 32\nidle_1 = 32 0 32 32\n"));
    PCHECK(p.frames.size() == 2 && pnear(p.frames[1].x, 32));
    const unsigned char png[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                   0, 0, 0x01, 0x2C, 0, 0, 0, 0x96};
    int w = 0, h = 0;
    PCHECK(psych::pngSize(std::string_view(reinterpret_cast<const char*>(png), 24), w, h) && w == 300 && h == 150);
    PCHECK(!psych::pngSize("not a png at all, really not", w, h));
}

void testCharacterAndStage() {
    const char* json = R"({"animations": [
        {"loop": false, "offsets": [-5, 3], "anim": "idle", "fps": 24, "name": "BF idle dance", "indices": []},
        {"loop": true, "offsets": [0, 0], "anim": "danceLeft", "fps": 24, "name": "BF idle dance", "indices": [1, 2]}],
      "no_antialiasing": true, "image": "characters/BOYFRIEND, characters/extra", "position": [0, 350],
      "healthicon": "bf", "flip_x": true, "scale": 1.5, "camera_position": [10, -20],
      "healthbar_colors": [49, 176, 209], "sing_duration": 6.1})";
    psych::CharacterDef d;
    std::string err;
    PCHECK(psych::parseCharacter(json, d, err));
    PCHECK(d.images.size() == 2 && d.images[1] == "characters/extra");
    PCHECK(d.anims.size() == 2 && d.anims[0].anim == "idle" && pnear(d.anims[0].offX, -5));
    PCHECK(d.anims[1].indices.size() == 2 && d.anims[1].loop);
    PCHECK(!d.antialiasing && d.flipX && pnear(d.scale, 1.5) && pnear(d.posY, 350) && pnear(d.camY, -20));
    PCHECK(d.barColor == 0x31B0D1 && d.icon == "bf" && pnear(d.singDuration, 6.1));

    const char* vslice = R"({"version": "1.0.0", "name": "Pico", "renderType": "sparrow", "assetPath": "characters/Pico_Basic",
      "scale": 1, "healthIcon": {"id": "pico"}, "offsets": [0, -400], "cameraOffsets": [0, 0], "singTime": 8,
      "animations": [{"name": "idle", "prefix": "Pico Idle Dance", "offsets": [0, 0]}, {"name": "singLEFT", "prefix": "Pico Note Right", "offsets": [-1, 2]}]})";
    PCHECK(psych::parseCharacter(vslice, d, err));
    PCHECK(d.images.size() == 1 && d.icon == "pico" && d.anims.size() == 2 && pnear(d.posY, -400));

    psych::StageDef st;
    PCHECK(psych::parseStage(R"({"directory": "week6", "defaultZoom": 1.05, "stageUI": "pixel", "boyfriend": [970, 320],
        "girlfriend": [580, 430], "opponent": [100, 100], "hide_girlfriend": true, "camera_boyfriend": [-100, -100],
        "camera_speed": 2, "objects": [{"type": "square", "name": "bg", "x": -5, "y": -6, "scale": [50, 60], "color": "FF0000"},
        {"type": "gfGroup"}]})", st, err));
    PCHECK(st.pixel && st.hideGf && pnear(st.zoom, 1.05) && pnear(st.bf[0], 970) && pnear(st.camBf[1], -100));
    PCHECK(st.objects.size() == 2 && st.objects[0].color == 0xFF0000 && pnear(st.objects[0].scaleY, 60));
    PCHECK(pnear(st.camSpeed, 2));
}

void testEventsAndMeta() {
    const char* chart = R"({"song": {"song": "Ev", "bpm": 100, "player1": "bf-pixel", "player2": "spirit", "gfVersion": "gf-pixel",
      "stage": "schoolEvil", "events": [[2000, [["Hey!", "bf", "0.6"], ["Add Camera Zoom", "", ""]]], [500, [["Change Character", "dad", "monster"]]]],
      "notes": [{"mustHitSection": true, "sectionNotes": [[0, 0, 0], [100, -1, "Play Animation", "hey", "bf"], [200, 1, 0, "GF Sing"]]}]}})";
    std::vector<psych::ChartEvent> ev;
    psych::parseEvents(chart, ev);
    PCHECK(ev.size() == 4);
    PCHECK(ev[0].timeMs == 100 && ev[0].name == "Play Animation" && ev[0].v2 == "bf");
    PCHECK(ev[1].timeMs == 500 && ev[1].name == "Change Character" && ev[1].v2 == "monster");
    PCHECK(ev[2].name == "Hey!" && ev[3].name == "Add Camera Zoom");
    const psych::SongMeta m = psych::parseSongMeta(chart);
    PCHECK(m.player == "bf-pixel" && m.opponent == "spirit" && m.gf == "gf-pixel" && m.stage == "schoolEvil");
    Chart c;
    std::string err;
    PCHECK(parseChart(chart, "Hard", c, err));
    PCHECK(c.events.size() == 4 && c.gf == "gf-pixel" && c.stage == "schoolEvil");
    PCHECK(c.notes.size() == 2 && (c.notes[1].flags & kFlagGF) && c.noteTypes[c.notes[1].type] == "GF Sing");

    std::vector<psych::ChartEvent> vs;
    psych::parseEvents(R"({"notes": {}, "events": [{"t": 10, "e": "ZoomCamera", "v": {"zoom": 1.3, "duration": 4, "ease": "expoOut"}},
        {"t": 20, "e": "PlayAnimation", "v": {"target": "gf", "anim": "cheer", "force": true}}]})", vs);
    PCHECK(vs.size() == 2 && vs[0].name == "__vslice_zoom" && vs[0].v1 == "1.3" && vs[1].name == "Play Animation" && vs[1].v2 == "gf");

    PCHECK(psych::parseColor("FF0000") == 0xFFFF0000u);
    PCHECK(psych::parseColor("0xFF00FF00") == 0xFF00FF00u);
    PCHECK(psych::parseColor("#0000ff") == 0xFF0000FFu);
    PCHECK(psych::parseColor("white") == 0xFFFFFFFFu);
    PCHECK(psych::parseColor("garbage", 7) == 7);
    for (const char* e : {"linear", "quadIn", "quadOut", "cubeInOut", "sineInOut", "bounceOut", "elasticOut", "backIn", "expoInOut",
                          "circOut", "smoothStepInOut", "quartOut", "quintInOut"}) {
        PCHECK(pnear(psych::ease(e, 0.0), 0.0, 1e-3));
        PCHECK(pnear(psych::ease(e, 1.0), 1.0, 1e-3));
    }
    PCHECK(pnear(psych::ease("quadIn", 0.5), 0.25));
}

scene::GraphicPtr fakeAtlas(int frames, float w, float h) {
    auto g = std::make_shared<scene::Graphic>();
    g->files.push_back("fake.png");
    g->sizes.push_back({int(w * frames), int(h)});
    for (int i = 0; i < frames; ++i) {
        psych::AtlasFrame f;
        f.name = "anim" + std::to_string(1000 + i).substr(1);
        f.x = w * float(i);
        f.w = f.fw = w;
        f.h = f.fh = h;
        g->frames.push_back(f);
    }
    return g;
}

void testSceneModel() {
    scene::Sprite s;
    s.setGraphic(fakeAtlas(4, 100, 50));
    PCHECK(pnear(s.width, 100) && pnear(s.originX, 50) && pnear(s.originY, 25));
    s.scaleX = s.scaleY = 2;
    s.updateHitbox();
    PCHECK(pnear(s.width, 200) && pnear(s.offsetX, -50) && pnear(s.offsetY, -25));
    s.setGraphicSize(300, 0);
    PCHECK(pnear(s.scaleX, 3) && pnear(s.scaleY, 3));
    s.screenCenter(true, false);
    PCHECK(pnear(s.x, (1280 - s.width) / 2));

    scene::Anim a;
    a.name = "go";
    a.frames = {0, 1, 2, 3};
    a.fps = 10;
    s.addAnim(a);
    PCHECK(s.play("go", true));
    s.updateAnim(0.25);  // 2.5 frames
    PCHECK(s.animFrame == 2 && !s.finished);
    s.updateAnim(0.06);
    PCHECK(s.animFrame == 3 && !s.finished);  // last frame shows for one frame time
    s.updateAnim(0.1);
    PCHECK(s.finished && s.animFrame == 3);
    PCHECK(!s.play("missing"));

    scene::Character c;
    c.setGraphic(fakeAtlas(4, 100, 100));
    for (const char* n : {"danceLeft", "danceRight", "singLEFT"}) {
        scene::Anim an;
        an.name = n;
        an.frames = {0, 1};
        an.fps = 24;
        an.hasOffset = true;
        an.offX = std::string(n) == "singLEFT" ? 12.0f : 0.0f;
        c.addAnim(an);
    }
    c.recalcDance(true);
    PCHECK(c.danceIdle && c.danceEvery == 1);
    c.dance();
    PCHECK(c.curAnimName() == "danceRight");
    c.dance();
    PCHECK(c.curAnimName() == "danceLeft");
    c.playAnim("singLEFT", true);
    PCHECK(pnear(c.offsetX, 12));
    c.updateChar(0.5, 100, false);  // 0.5 s > 100 ms * 0.0011 * 4: back to dancing
    PCHECK(c.curAnimName().rfind("dance", 0) == 0);

    scene::Scene sc;
    auto spr = std::make_shared<scene::Sprite>();
    spr->tag = "box";
    spr->setGraphic(fakeAtlas(1, 10, 10));
    sc.objects["box"] = spr;
    scene::Tween tw;
    tw.tag = "move";
    tw.target = "box";
    tw.fields.push_back({"x", 0, 100, false});
    tw.dur = 1;
    sc.tween(tw);
    sc.update(0.5, 100, false);
    PCHECK(pnear(spr->x, 50, 0.5));
    sc.update(0.6, 100, false);
    PCHECK(pnear(spr->x, 100) && sc.tweens.empty() && sc.completions.size() == 1);
    scene::Timer t;
    t.tag = "tick";
    t.time = 0.1;
    t.loops = 3;
    sc.completions.clear();
    sc.timer(t);
    sc.update(0.35, 100, false);
    PCHECK(sc.completions.size() == 3 && sc.timers.empty() && sc.completions.back().left == 0);
    scene::Value v;
    PCHECK(sc.setProp("camGame.zoom", 1.5) && sc.getProp("camGame.zoom", v) && pnear(scene::toNum(v), 1.5));
    sc.keys = 4;
    sc.strums.resize(8);
    PCHECK(sc.setProp("playerStrums.members[2].x", 321.0) && pnear(sc.strums[6].x, 321));
    PCHECK(sc.getProp("strumLineNotes.length", v) && pnear(scene::toNum(v), 8));
    PCHECK(sc.setProp("someVariable", std::string("hi")) && sc.getProp("someVariable", v) && scene::toStr(v) == "hi");
}

void testLua() {
    scene::Scene sc;
    sc.keys = 4;
    sc.strums.resize(8);
    sc.bf = std::make_shared<scene::Character>();
    sc.bf->kind = scene::Kind::Character;
    sc.bf->name = "bf";
    sc.gfMarker = std::make_shared<scene::Sprite>();
    sc.gfMarker->kind = scene::Kind::Marker;
    sc.order = {sc.gfMarker, sc.bf};
    StubGame game;
    ScriptHost host(sc, game);
    host.callBudget = 0.3;
    host.loadBudget = 0.5;
    const char* code = R"LUA(
        created = 0
        function onCreate()
            created = created + 1
            makeLuaSprite('bg', '', -100, -50)
            makeGraphic('bg', 200, 100, 'FF0000')
            addLuaSprite('bg', false)
            makeLuaText('label', 'Hello', 0, 10, 20)
            addLuaText('label')
            setProperty('bg.alpha', 0.5)
            setProperty('boyfriend.x', 777)
            setProperty('health', 1.5)
            doTweenX('slide', 'bg', 300, 1, 'linear')
            runTimer('t1', 0.2, 2)
            noteTweenAlpha('fade', 5, 0.25, 0.5, 'quadOut')
            setVar('shared', 42)
        end
        function onUpdate(e) beat = (beat or 0) + 1 end
        function onTimerCompleted(tag, loops, left) last = tag .. ':' .. left end
        function onTweenCompleted(tag) tweened = tag end
        function onStartCountdown() return Function_Stop end
        function label() return 'beat ' .. curBeat end
    )LUA";
    PCHECK(host.loadString("test.lua", code));
    PCHECK(host.errors.empty());
    if (!host.errors.empty()) std::printf("  %s\n", host.errors[0].c_str());
    PCHECK(sc.objects.count("bg") && sc.order.front()->tag == "bg");  // inserted behind the characters
    PCHECK(pnear(sc.objects["bg"]->alpha, 0.5) && pnear(sc.bf->x, 777) && pnear(game.health, 1.5));
    PCHECK(sc.objects["label"]->kind == scene::Kind::Text && sc.objects["label"]->cam == scene::Cam::Hud);
    host.setGlobal("curBeat", 4.0);
    PCHECK(host.call("onStartCountdown") == ScriptHost::kStop);
    for (int i = 0; i < 12; ++i) {
        host.call("onUpdate", {scene::Value(0.1)});
        sc.update(0.1, 100, false);
        host.dispatchCompletions();
    }
    PCHECK(pnear(sc.objects["bg"]->x, 300) && pnear(sc.strums[5].alpha, 0.25));
    scene::Value v;
    PCHECK(sc.getProp("shared", v) && pnear(scene::toNum(v), 42));
    PCHECK(host.errors.empty());

    // Integral numbers print like Psych's Lua ("4", not "4.0"), Lua 5.1 shims work.
    PCHECK(host.loadString("compat.lua", R"LUA(
        function onCreate()
            debugPrint('beat ' .. curBeat)
            debugPrint(tostring(unpack({7, 8})))
            debugPrint(tostring(bit.band(12, 10)))
            debugPrint(tostring(math.pow(2, 3)))
            debugPrint(stringSplit('a,b,c', ',')[2])
            debugPrint(getPropertyFromClass('backend.ClientPrefs', 'data.downScroll'))
        end)LUA"));
    PCHECK(game.prints.size() >= 6);
    if (game.prints.size() >= 6) {
        PCHECK(game.prints[0] == "beat 4");
        PCHECK(game.prints[1] == "7");
        PCHECK(game.prints[2] == "8");
        PCHECK(game.prints[3] == "8.0");
        PCHECK(game.prints[4] == "b");
        PCHECK(game.prints[5] == "false");
    }

    // Sandbox: no io / os.execute / dofile, require only finds the song's own
    // Lua modules, and runaway loops are stopped.
    game.prints.clear();
    PCHECK(host.loadString("sandbox.lua", R"LUA(
        function onCreate()
            debugPrint(tostring(io), tostring(os.execute), tostring(loadfile), tostring(dofile))
            debugPrint(tostring(pcall(require, "io")), tostring(pcall(require, "../../etc/passwd")))
        end
        function onUpdate() while true do end end)LUA"));
    PCHECK(game.prints.size() >= 2 && game.prints[0] == "nilnilnilnil" && game.prints[1] == "falsefalse");
    const size_t before = host.errors.size();
    host.call("onUpdate", {scene::Value(0.016)});
    PCHECK(host.errors.size() == before + 1 && host.errors.back().find("too long") != std::string::npos);

    // os.clock counts real time, as on Windows: a shader's iTime keeps moving
    // while the game waits (Linux's C clock() counts CPU time).
    game.prints.clear();
    PCHECK(host.loadString("clock.lua", R"LUA(
        local t0 = 0
        function onCreate() t0 = os.clock() end
        function onBeatHit() debugPrint(tostring(os.clock() - t0)) end)LUA"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    host.call("onBeatHit");
    PCHECK(!game.prints.empty() && std::atof(game.prints.back().c_str()) >= 0.1 && std::atof(game.prints.back().c_str()) < 5);

    // Warnings and errors say whether the script set luaDebugMode (Psych only
    // shows them on screen then; Options > DEBUG decides here).
    game.notices.clear();
    PCHECK(host.loadString("quiet.lua", R"LUA(
        function onCreate() makeLuaSprite('a', 'no_such_image', 0, 0) end
        function onStepHit() local x = nil; x.y = 1 end)LUA"));
    PCHECK(host.loadString("loud.lua", R"LUA(
        luaDebugMode = true
        function onCreate() makeLuaSprite('b', 'no_such_image', 0, 0) end
        function onStepHit() local x = nil; x.y = 1 end)LUA"));
    host.call("onStepHit");
    auto noticeOf = [&](const char* script, bool error) -> const StubGame::Notice* {
        for (const auto& n : game.notices)
            if (n.error == error && n.text.find(script) != std::string::npos) return &n;
        return nullptr;
    };
    PCHECK(noticeOf("quiet.lua", false) && !noticeOf("quiet.lua", false)->debugMode);
    PCHECK(noticeOf("quiet.lua", true) && !noticeOf("quiet.lua", true)->debugMode);
    PCHECK(noticeOf("loud.lua", false) && noticeOf("loud.lua", false)->debugMode);
    PCHECK(noticeOf("loud.lua", true) && noticeOf("loud.lua", true)->debugMode);
    PCHECK(!host.loadString("nope.lua", "function onCreate( end"));  // a script that does not load is always reported
    PCHECK(noticeOf("nope.lua", true) && noticeOf("nope.lua", true)->debugMode);

    // Syntax errors are reported, not fatal.
    PCHECK(!host.loadString("broken.lua", "function onCreate( end"));
    PCHECK(host.errors.back().find("broken.lua") != std::string::npos);

    // Kade modchart helpers address strums by number.
    PCHECK(host.loadString("data/song/modchart.lua", R"LUA(
        function start(song) setActorX(500, 4); setActorAlpha(0.5, 1) end)LUA"));
    PCHECK(host.kadeMode && pnear(sc.strums[4].x, 500) && pnear(sc.strums[1].alpha, 0.5));
}

namespace fs = std::filesystem;

std::string modRootOf(const fs::path& file) {
    // The nearest parent that looks like a Psych mod / asset root.
    for (fs::path p = file.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        std::error_code ec;
        if (fs::is_directory(p / "images", ec) || fs::is_directory(p / "characters", ec) || fs::is_directory(p / "stages", ec) ||
            fs::is_directory(p / "data", ec))
            return p.string() + "/";
    }
    return file.parent_path().string() + "/";
}

}  // namespace

void runPsychTests() {
    testAtlas();
    testCharacterAndStage();
    testEventsAndMeta();
    testSceneModel();
    testLua();
}

// Loads every atlas, character, stage and Lua script under dir.
int runPsychCorpus(const char* dir) {
    int atlases = 0, atlasFail = 0, chars = 0, charFail = 0, stages = 0, stageFail = 0, scripts = 0, scriptFail = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path p = it->path();
        const std::string name = p.filename().string(), parent = p.parent_path().filename().string();
        std::string text;
        if (endsWithI(name, ".xml")) {
            if (!scene::readFile(p.string(), text) || text.find("TextureAtlas") == std::string::npos) continue;
            psych::Atlas a;
            ++atlases;
            std::string err;
            if (!a.parseSparrow(text, &err)) { ++atlasFail; std::printf("atlas FAIL %s: %s\n", p.string().c_str(), err.c_str()); }
        } else if (endsWithI(name, ".json") && (parent == "characters" || parent == "stages")) {
            if (!scene::readFile(p.string(), text)) continue;
            if (text.rfind("version https://git-lfs", 0) == 0) continue;
            std::string err;
            if (parent == "characters") {
                psych::CharacterDef d;
                ++chars;
                if (!psych::parseCharacter(text, d, err)) { ++charFail; std::printf("char FAIL %s: %s\n", p.string().c_str(), err.c_str()); }
            } else {
                psych::StageDef d;
                ++stages;
                if (!psych::parseStage(text, d, err)) { ++stageFail; std::printf("stage FAIL %s: %s\n", p.string().c_str(), err.c_str()); }
            }
        } else if (endsWithI(name, ".lua")) {
            scene::Scene sc;
            sc.files.setRoots({modRootOf(p)});
            sc.keys = 4;
            sc.strums.resize(8);
            for (const char* who : {"bf", "dad", "gf"}) {
                auto c = std::make_shared<scene::Character>();
                c->kind = scene::Kind::Character;
                c->name = who;
                c->builtin = true;
                c->width = c->frameW = 400;
                c->height = c->frameH = 400;
                (std::string(who) == "bf" ? sc.bf : std::string(who) == "dad" ? sc.dad : sc.gf) = c;
            }
            sc.gfMarker = std::make_shared<scene::Sprite>();
            sc.gfMarker->kind = scene::Kind::Marker;
            sc.order = {sc.gfMarker, sc.gf, sc.dad, sc.bf};
            // The HUD objects the game exposes (icons, bars, texts).
            for (const char* hudName : {"iconP1", "iconP2", "healthBar", "healthBarBG", "scoreTxt", "timeBar", "timeBarBG",
                                        "timeTxt", "botplayTxt", "comboGroup", "grpNoteSplashes"}) {
                auto s = std::make_shared<scene::Sprite>();
                s->tag = hudName;
                s->cam = scene::Cam::Hud;
                sc.hud[hudName] = s;
            }
            StubGame game;
            game.scene = &sc;
            ScriptHost host(sc, game);
            host.callBudget = 0.5;
            ++scripts;
            host.setGlobal("curBeat", 0.0);
            host.setGlobal("curStep", 0.0);
            host.setGlobal("songName", std::string("test"));
            for (const char* n : {"bpm", "curBpm"}) host.setGlobal(n, 120.0);
            host.setGlobal("crochet", 500.0);
            host.setGlobal("stepCrochet", 125.0);
            host.setGlobal("scrollSpeed", 2.0);
            host.setGlobal("screenWidth", 1280.0);
            host.setGlobal("screenHeight", 720.0);
            host.setGlobal("downscroll", false);
            host.setGlobal("middlescroll", false);
            host.setGlobal("mustHitSection", true);
            // Globals the game sets for every script.
            host.setGlobal("curDecBeat", 0.0);
            host.setGlobal("curDecStep", 0.0);
            host.setGlobal("currentModDirectory", std::string());
            host.setGlobal("hideHud", false);
            host.setGlobal("gfSection", false);
            host.setGlobal("altAnim", false);
            host.setGlobal("curSection", 0.0);
            host.setGlobal("shadersEnabled", true);
            host.setGlobal("playbackRate", 1.0);
            host.setGlobal("version", std::string("0.7.3"));
            for (const char* n : {"boyfriendName", "dadName", "gfName"}) host.setGlobal(n, std::string(n).substr(0, std::string(n).size() - 4));
            for (int i = 0; i < 4; ++i) {
                host.setGlobal(("defaultPlayerStrumX" + std::to_string(i)).c_str(), 732.0 + 112 * i);
                host.setGlobal(("defaultPlayerStrumY" + std::to_string(i)).c_str(), 50.0);
                host.setGlobal(("defaultOpponentStrumX" + std::to_string(i)).c_str(), 92.0 + 112 * i);
                host.setGlobal(("defaultOpponentStrumY" + std::to_string(i)).c_str(), 50.0);
            }
            host.load(p.string());
            host.call("onCreatePost");
            for (int f = 0; f < 120; ++f) {
                game.pos = f * 16.0;
                host.setGlobal("curDecBeat", f / 8.0);
                host.setGlobal("curDecStep", f / 2.0);
                host.call("onUpdate", {scene::Value(1.0 / 60)});
                if (f % 8 == 0) {
                    host.setGlobal("curBeat", double(f / 8));
                    host.call("onBeatHit", {scene::Value(double(f / 8))});
                    host.call("beatHit", {scene::Value(double(f / 8))});
                }
                if (f % 2 == 0) {
                    host.setGlobal("curStep", double(f / 2));
                    host.call("onStepHit");
                }
                if (f == 30) host.call("goodNoteHit", {scene::Value(0.0), scene::Value(1.0), scene::Value(std::string()), scene::Value(false)});
                if (f == 40) host.call("opponentNoteHit", {scene::Value(0.0), scene::Value(2.0), scene::Value(std::string()), scene::Value(false)});
                if (f == 50 && parent == "custom_events")
                    host.call("onEvent", {scene::Value(p.stem().string()), scene::Value(std::string("1")), scene::Value(std::string("0.5"))});
                sc.update(1.0 / 60, 120, false);
                host.dispatchCompletions();
                host.call("onUpdatePost", {scene::Value(1.0 / 60)});
            }
            if (!host.errors.empty()) {
                ++scriptFail;
                std::printf("script %s: %zu error(s), first: %s\n", p.string().c_str(), host.errors.size(), host.errors[0].c_str());
            }
        }
    }
    std::printf("atlases %d (%d failed), characters %d (%d failed), stages %d (%d failed), scripts %d (%d with errors)\n", atlases,
                atlasFail, chars, charFail, stages, stageFail, scripts, scriptFail);
    return atlasFail + charFail + stageFail;
}

// ------------------------------------------------- AutoChart, replays, zips --
#include "autochart.hpp"
#include "chart_stream.hpp"
#include "effects.hpp"
#include "shader_glsl.hpp"
#include "composer.hpp"
#include "miniz.h"
#include "mod_install.hpp"
#include "replay.hpp"
#include "session.hpp"

namespace {

void testAutoChart() {
    // 40 s of clicks at 128 BPM: low thumps on beats, high ticks on off-beats.
    const int rate = 44100;
    const double bpm = 128, beat = 60.0 / bpm, first = 0.37;
    std::vector<int16_t> pcm(size_t(rate * 40), 0);
    for (double t = first; t < 39.5; t += beat / 2) {
        const bool onBeat = std::fmod((t - first) / beat + 1e-6, 1.0) < 0.25;
        const double f = onBeat ? 90 : 3200;
        for (int i = 0; i < rate / 12; ++i) {
            const size_t k = size_t(t * rate) + size_t(i);
            if (k >= pcm.size()) break;
            const double env = std::exp(-double(i) / (rate * 0.02));
            pcm[k] = int16_t(std::clamp(pcm[k] + 14000.0 * env * std::sin(2 * kPi * f * i / rate), -32767.0, 32767.0));
        }
    }
    Chart c;
    AutoChartInfo info;
    PCHECK(autoChart(pcm.data(), pcm.size(), 1, rate, 2, c, &info));
    PCHECK(std::fabs(info.bpm - bpm) < 1.5);
    PCHECK(c.notes.size() > 100 && c.playerNotes > 30 && c.opponentNotes > 30);
    // Notes sit on the detected grid near the real clicks.
    int onGrid = 0;
    for (const Note& n : c.notes) {
        const double rel = (n.time / 1000.0 - first) / (beat / 4);
        if (std::fabs(rel - std::round(rel)) < 0.2) ++onGrid;
    }
    PCHECK(onGrid > int(c.notes.size() * 9 / 10));
    Chart easy;
    PCHECK(autoChart(pcm.data(), pcm.size(), 1, rate, 0, easy) && easy.notes.size() < c.notes.size());
    std::vector<int16_t> silence(size_t(rate) * 10, 0);
    PCHECK(!autoChart(silence.data(), silence.size(), 1, rate, 1, c));
}

void testReplayDeterminism() {
    Chart c;
    c.keys = 4;
    for (int i = 0; i < 64; ++i) c.notes.push_back({1000.0 + i * 180.0, i % 5 == 0 ? 300.0 : 0.0, uint8_t(i % 4), 1, kNormal, 0, 0});
    c.finalize(120);
    SessionConfig cfg;
    Session a;
    a.init(c, cfg);
    Replay rec;
    Rng rng(7);
    std::vector<ReplayEvent> evs;
    for (const Note& n : c.notes) {
        const double err = (rng.uniform() - 0.5) * 140.0;
        evs.push_back({n.time + err, n.lane, true});
        evs.push_back({n.time + err + std::max(40.0, n.length + (rng.uniform() - 0.6) * 200.0), n.lane, false});
    }
    std::sort(evs.begin(), evs.end(), [](const ReplayEvent& x, const ReplayEvent& y) { return x.t < y.t; });
    auto run = [&](Session& s, const std::vector<ReplayEvent>& list) {
        size_t i = 0;
        for (double t = 0; t < 14000; t += 4) {
            while (i < list.size() && list[i].t <= t) {
                const ReplayEvent& e = list[i++];
                if (e.down) s.press(e.lane, e.t);
                else s.release(e.lane, e.t);
            }
            s.update(t);
            s.events.clear();
        }
    };
    run(a, evs);
    rec.songId = "custom:test/song";
    rec.diff = "Hard";
    rec.events = evs;
    const std::string path = (std::filesystem::temp_directory_path() / "gh_replay_test.ghr").string();
    PCHECK(rec.save(path));
    Replay back;
    PCHECK(back.load(path) && back.events.size() == evs.size() && back.songId == rec.songId);
    Session b;
    b.init(c, cfg);
    run(b, back.events);
    PCHECK(a.score == b.score && a.misses == b.misses && a.counts == b.counts && a.maxCombo == b.maxCombo);
    PCHECK(replayFileName("custom:My Mod/song", "Hard", "best") == "custom_my_mod_song-hard-best.ghr");
    std::filesystem::remove(path);
}

void testChartStreaming() {
    // A Dave & Bambi style 9K chart: notes stacked in one lane, a legacy
    // event note, note types, options.mania, fields after the notes.
    std::string j = "{\"song\": {\"player2\": \"bambi\", \"notes\": [";
    Rng rng(3);
    int stacked = 0;
    for (int sec = 0; sec < 40; ++sec) {
        if (sec) j += ",";
        j += "{\"sectionNotes\": [";
        for (int k = 0; k < 30; ++k) {
            const double t = sec * 2000.0 + k * 60.0 + (rng.uniform() < 0.5 ? 0.25 : 0.0);
            const int d = int(rng.uniform() * 18);
            if (k) j += ",";
            char buf[160];
            if (k == 7) std::snprintf(buf, sizeof buf, "[%.3f, %d, 0, \"Hurt Note\"]", t, d);
            else if (k == 9) std::snprintf(buf, sizeof buf, "[%.3f, -1, \"Hey!\", \"bf\", \"0.6\"]", t);
            else std::snprintf(buf, sizeof buf, "[%.6f, %d, %d]", t, d, k == 3 ? 400 : 0);
            j += buf;
            if (k % 5 == 0 && k != 9) {  // a stacked copy 0.3 ms later
                std::snprintf(buf, sizeof buf, ",[%.6f, %d, 0]", t + 0.3, d);
                j += buf;
                ++stacked;
            }
        }
        j += "], \"mustHitSection\": ";
        j += sec % 2 ? "true" : "false";
        j += ", \"lengthInSteps\": 16}";
    }
    j += "], \"options\": {\"mania\": 3, \"speed\": 7}, \"song\": \"Blocked\", \"stage\": \"farm\", \"bpm\": 120}}";
    const std::string path = (std::filesystem::temp_directory_path() / "gh_stream_test.json").string();
    std::ofstream(path, std::ios::binary) << j;
    Chart dom, streamed;
    std::string err;
    PCHECK(parseChart(j, "Hard", dom, err));
    PCHECK(loadChartFile(path, "Hard", streamed, err, {}, 1));  // force streaming
    PCHECK(dom.keys == 9 && streamed.keys == 9);
    PCHECK(dom.notes.size() == streamed.notes.size() && dom.notes.size() > 1000);
    bool same = dom.notes.size() == streamed.notes.size();
    for (size_t i = 0; same && i < dom.notes.size(); ++i) {
        const Note& a = dom.notes[i];
        const Note& b = streamed.notes[i];
        same = std::fabs(a.time - b.time) < 1e-6 && std::fabs(a.length - b.length) < 1e-3 && a.lane == b.lane &&
               a.side == b.side && a.kind == b.kind && a.flags == b.flags && a.stack == b.stack;
    }
    PCHECK(same);
    PCHECK(streamed.title == "Blocked" && streamed.stage == "farm" && streamed.opponent == "bambi");
    PCHECK(streamed.events.size() == dom.events.size() && streamed.events.size() == 40 && streamed.events[0].name == "Hey!");
    PCHECK(streamed.rawNoteCount == streamed.notes.size() + size_t(stacked));
    // Every note of the file is still counted.
    PCHECK(streamed.playerNotes + streamed.opponentNotes == streamed.rawNoteCount);
    PCHECK(dom.playerNotes + dom.opponentNotes == streamed.rawNoteCount);
    const ChartPeek pk = peekChart(path);
    PCHECK(pk.song == "Blocked" && pk.player2 == "bambi" && pk.stage == "farm" && pk.isChart);
    std::filesystem::remove(path);
}

void testShaderTranslation() {
    const std::string out = translateShader(
        "#pragma header\nprecision mediump float;\nuniform sampler2D bitmap;\nvarying vec2 openfl_TextureCoordv;\n"
        "uniform sampler2D texture;\nuniform float iTime;\nvoid main() {\n"
        "  vec4 a = texture2D(bitmap, openfl_TextureCoordv);\n  vec4 b = texture2D( texture , vec2(0.5));\n"
        "  vec4 c = flixel_texture2D(bitmap, openfl_TextureCoordv);\n  gl_FragColor = a + b + c;\n}\n");
    PCHECK(out.rfind("#version 330", 0) == 0);
    PCHECK(out.find("gh_texBitmap( openfl_TextureCoordv)") != std::string::npos);
    PCHECK(out.find("gh_tex( gh_userTexture , vec2(0.5))") != std::string::npos);
    PCHECK(out.find("gh_FragColor = a + b + c") != std::string::npos);
    PCHECK(out.find("uniform sampler2D bitmap") == std::string::npos);  // supplied by the prelude
    PCHECK(out.find("varying") == std::string::npos && out.find("#pragma") == std::string::npos);
    PCHECK(out.find("uniform float iTime;") != std::string::npos);
    // OpenGL ES 2 (Android): GLSL ES 1.0 with a float precision and varyings.
    const std::string es = translateShader("#pragma header\nvoid main() { gl_FragColor = texture2D(bitmap, openfl_TextureCoordv); }\n", true);
    PCHECK(es.rfind("#version 100", 0) == 0);
    PCHECK(es.find("precision mediump float;") != std::string::npos && es.find("varying vec2 fragTexCoord;") != std::string::npos);
    PCHECK(es.find("gh_FragColor = gh_texBitmap( openfl_TextureCoordv)") != std::string::npos);
    PCHECK(es.find("#define gh_FragColor gl_FragColor") != std::string::npos && es.find(" in vec2") == std::string::npos);
}

void testHScript() {
    scene::Scene sc;
    sc.keys = 4;
    sc.strums.resize(8);
    sc.bf = std::make_shared<scene::Character>();
    sc.bf->kind = scene::Kind::Character;
    sc.bf->name = "bf";
    sc.gfMarker = std::make_shared<scene::Sprite>();
    sc.gfMarker->kind = scene::Kind::Marker;
    sc.order = {sc.gfMarker, sc.bf};
    StubGame game;
    ScriptHost host(sc, game);
    host.callBudget = 0.5;
    host.loadBudget = 0.5;

    // Language: closures, interpolation, loops, switch, maps, arrow functions, try/catch.
    hx::Interp ip(sc, game, &host, host.hxShared);
    hx::Value r;
    std::string err;
    PCHECK(ip.run(R"HX(
        var total = 0;
        for (i in 0...5) total += i;
        var arr = [3, 1, 2];
        arr.sort((a, b) -> a - b);
        var m = ["a" => 1, "b" => 2];
        var sum = 0;
        for (k => v in m) sum += v;
        function fib(n:Int):Int { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
        var counter = function() { var c = 0; return function() { return ++c; }; }();
        counter(); counter();
        var kind = switch (total) { case 10: 'ten'; default: 'other'; };
        var caught = '';
        try { throw 'boom'; } catch (e:Dynamic) { caught = e; }
        var obj = {name: 'bf', hp: 2};
        obj.hp -= 0.5;
        var s = 'total=$total arr=${arr.join(",")} sum=$sum fib=${fib(10)} c=${counter()} $kind $caught ${obj.hp}';
        s;
    )HX", "lang", r, err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    PCHECK(r.str() == "total=10 arr=1,2,3 sum=3 fib=55 c=3 ten boom 1.5");
    PCHECK(!ip.run("var x = ;", "bad", r, err) && err.find("line 1") != std::string::npos);
    err.clear();
    PCHECK(!ip.run("while (true) {}", "loop", r, err) && err.find("too long") != std::string::npos);

    // Lua -> runHaxeCode -> scene, shaders on cameras, tweens with callbacks, setVar.
    PCHECK(host.loadString("hx.lua", R"LUA(
        function onCreate()
            makeLuaSprite('bg', '', 0, 0)
            makeGraphic('bg', 10, 10, 'FFFFFF')
            addLuaSprite('bg', false)
            local x = runHaxeCode([[
                var bg = game.getLuaObject('bg');
                bg.x = 50;
                bg.scale.set(2, 3);
                var s = new FlxRuntimeShader('#pragma header
void main() { gl_FragColor = flixel_texture2D(bitmap, openfl_TextureCoordv); }');
                s.setFloat('amount', 0.5);
                game.camGame.setFilters([new ShaderFilter(s)]);
                var spr = new FlxSprite(5, 6);
                spr.makeGraphic(4, 4, FlxColor.RED);
                add(spr);
                setVar('fromHaxe', 42);
                FlxTween.tween(bg, {y: 100}, 0.5, {ease: FlxEase.quadOut, onComplete: function(t) { setVar('tweenDone', true); }});
                new FlxTimer().start(0.2, function(t) { game.boyfriend.x = 777; });
                createCallback('haxeDouble', function(n) { return n * 2; });
                function later(a, b) { return a + b; }
                return bg.x + bg.scale.y;
            ]])
            debugPrint('x=' .. x)
            debugPrint('fn=' .. runHaxeFunction('later', {20, 22}))
            debugPrint('cb=' .. haxeDouble(21))
            debugPrint('var=' .. getVar('fromHaxe'))
        end)LUA"));
    for (const std::string& e : host.errors) std::printf("  %s\n", e.c_str());
    PCHECK(host.errors.empty());
    PCHECK(game.prints.size() >= 4);
    if (game.prints.size() >= 4) {
        PCHECK(game.prints[0] == "x=53");
        PCHECK(game.prints[1] == "fn=42");
        PCHECK(game.prints[2] == "cb=42");
        PCHECK(game.prints[3] == "var=42");
    }
    PCHECK(pnear(sc.objects["bg"]->scaleX, 2) && pnear(sc.objects["bg"]->scaleY, 3));
    PCHECK(sc.camGame.filters.size() == 1 && pnear(sc.camGame.filters[0]->floats["amount"][0], 0.5));
    PCHECK(sc.order.back()->graphic && sc.order.back()->graphic->solid && pnear(sc.order.back()->x, 5));
    for (int i = 0; i < 8; ++i) {
        host.call("onUpdate", {scene::Value(0.1)});
        sc.update(0.1, 100, false);
        host.dispatchCompletions();
    }
    PCHECK(pnear(sc.objects["bg"]->y, 100) && scene::toBool(sc.vars["tweenDone"]) && pnear(sc.bf->x, 777));

    // .hx script files with Psych callbacks; Function_Stop works like in Lua.
    PCHECK(host.loadHx("scripts/test.hx", R"HX(
        import flixel.FlxG;
        var hits:Array<Int> = [];
        function onCreate() { setVar('hxCreated', true); }
        function goodNoteHit(note) { hits.push(note.noteData); setVar('lastHit', note.noteData + (note.mustPress ? 10 : 0)); }
        function onStartCountdown() { return Function_Stop; }
    )HX"));
    PCHECK(scene::toBool(sc.vars["hxCreated"]));
    host.call("goodNoteHit", {scene::Value(7.0), scene::Value(2.0), scene::Value(std::string()), scene::Value(false)});
    PCHECK(pnear(scene::toNum(sc.vars["lastHit"]), 12));
    PCHECK(host.call("onStartCountdown") == ScriptHost::kStop);
    PCHECK(host.errors.empty());

    // JS Engine mods: camera lists and live filter lists, extra characters,
    // video players, openfl paths, parentLua (Psych 0.7), CoolUtil.
    game.scene = &sc;
    game.prints.clear();
    sc.camGame.filters.clear();
    PCHECK(host.loadString("jsengine.lua", R"LUA(
        function onCreatePost()
            debugPrint(runHaxeCode([[
                for (cam in FlxG.cameras.list) cam.setFilters([]);
                FlxG.cameras.cameraAdded.add(function(c) {});
                var s = new FlxRuntimeShader('#pragma header
void main() { gl_FragColor = flixel_texture2D(bitmap, openfl_TextureCoordv); }');
                game.camGame._filters.push(new ShaderFilter(s));
                var n = 0;
                for (f in game.camGame._filters) if (f.shader == s) n++;
                var ghost = new Character(100, 200, 'pico', false);
                game.add(ghost);
                ghost.alpha = 0.5;
                var video = new MP4Handler();
                video.finishCallback = function() { setVar('videoDone', true); };
                video.playVideo(Paths.video('intro'));
                openfl.Lib.application.window.title = 'Spam';
                var nums = CoolUtil.numberArray(3, 0);
                return FlxG.cameras.list.length + ' ' + n + ' ' + ghost.x + ' ' + nums.length + ' ' +
                       (parentLua.scriptName != null) + ' ' + openfl.Lib.application.window.title;
            ]]))
            addChromaticAbberationEffect('camHUD', 0.01)
            addWiggleEffect('game', 'flag', 1, 2, 0.05)
            addVCREffect('dad')
            clearEffects('v2')
        end)LUA"));
    host.call("onCreatePost");
    for (const std::string& e : host.errors) std::printf("  %s\n", e.c_str());
    PCHECK(host.errors.empty());
    PCHECK(!game.prints.empty() && game.prints[0] == "3 1 100 3 true Spam");
    PCHECK(sc.objects.count("__hxchar1") || std::any_of(sc.objects.begin(), sc.objects.end(), [](const auto& kv) {
               return kv.second->kind == scene::Kind::Character && pnear(kv.second->x, 100);
           }));
    PCHECK(sc.camHud.filters.size() == 1 && sc.camHud.filters[0]->name == "__fx_chromatic");
    PCHECK(sc.camGame.filters.empty());  // clearEffects with an unknown name clears the game camera
    host.dispatchCompletions();
    PCHECK(scene::toBool(sc.vars["videoDone"]));  // no video player in tests: the callback still runs
}

void testLuaExtras() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "gh_lua_extras";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "mod" / "data", ec);
    std::ofstream(root / "mod" / "data" / "settings.json")
        << R"([{"name": "Flashy", "save": "flashy", "type": "bool", "value": true}, {"name": "Speed", "save": "spd", "type": "float", "value": 1.5}])";
    for (int run = 0; run < 2; ++run) {
        scene::Scene sc;
        sc.files.setRoots({(root / "mod").string() + "/"});
        sc.keys = 4;
        sc.strums.resize(8);
        StubGame game;
        game.saveRoot = (root / "saves").string() + "/";
        ScriptHost host(sc, game);
        PCHECK(host.loadString("extras.lua", R"LUA(
            function onCreate()
                initSaveData('progress', 'mymod/')
                local n = getDataFromSave('progress', 'plays', 0)
                setDataFromSave('progress', 'plays', n + 1)
                setDataFromSave('progress', 'name', 'bf "the" kid')
                flushSaveData('progress')
                debugPrint('plays=' .. getDataFromSave('progress', 'plays') .. ' ' .. getDataFromSave('progress', 'name'))
                makeLuaSprite('a', '', 0, 0); makeGraphic('a', 100, 100, 'FFFFFF')
                makeLuaSprite('b', '', 50, 50); makeGraphic('b', 100, 100, 'FFFFFF')
                makeLuaSprite('c', '', 500, 0); makeGraphic('c', 10, 10, 'FFFFFF')
                debugPrint(tostring(objectsOverlap('a', 'b')) .. ' ' .. tostring(objectsOverlap('a', 'c')))
                debugPrint(tostring(getModSetting('flashy')) .. ' ' .. getModSetting('spd'))
                callMethod('camGame.flash', {'FF0000', 0.5})
                debugPrint(callMethodFromClass('flixel.math.FlxMath', 'lerp', {0, 10, 0.25}))
                removeFromGroup('playerStrums', 1)
                setOnHScript('fromLua', 7)
                debugPrint(runHaxeCode('return fromLua * 6;'))
            end)LUA"));
        for (const std::string& e : host.errors) std::printf("  %s\n", e.c_str());
        PCHECK(host.errors.empty());
        PCHECK(game.prints.size() >= 5);
        if (game.prints.size() >= 5) {
            PCHECK(game.prints[0] == (run == 0 ? "plays=1 bf \"the\" kid" : "plays=2 bf \"the\" kid"));
            PCHECK(game.prints[1] == "true false");
            PCHECK(game.prints[2] == "true 1.5");
            PCHECK(game.prints[3] == "2.5");
        }
        PCHECK(sc.camGame.flashTime > 0 && (sc.camGame.flashColor & 0xFFFFFF) == 0xFF0000);
        PCHECK(!sc.strums[5].visible);
    }
    PCHECK(fs::exists(root / "saves" / "mymod" / "progress.json"));
    fs::remove_all(root, ec);
}

void testZipInstall() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "gh_zip_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "mods", ec);
    auto makeZip = [&](const fs::path& out, const std::vector<std::pair<std::string, std::string>>& files) {
        mz_zip_archive z{};
        mz_zip_writer_init_heap(&z, 0, 0);
        for (const auto& f : files) mz_zip_writer_add_mem(&z, f.first.c_str(), f.second.data(), f.second.size(), MZ_DEFAULT_COMPRESSION);
        void* buf = nullptr;
        size_t size = 0;
        mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
        std::ofstream(out, std::ios::binary).write(static_cast<const char*>(buf), std::streamsize(size));
        mz_free(buf);
        mz_zip_writer_end(&z);
    };
    makeZip(root / "mods" / "Cool Mod v2.zip", {{"Cool Mod/data/song/song.json", "{}"}, {"Cool Mod/pack.json", "{\"name\":1}"},
                                                {"Cool Mod/../evil.txt", "x"}});
    makeZip(root / "mods" / "loose.zip", {{"data/a/a.json", "{}"}, {"images/b.png", "png"}});
    std::string report;
    PCHECK(installModZips((root / "mods").string(), report) == 2);
    PCHECK(fs::exists(root / "mods" / "Cool Mod" / "data" / "song" / "song.json"));
    PCHECK(fs::exists(root / "mods" / "loose" / "images" / "b.png"));
    PCHECK(!fs::exists(root / "evil.txt") && !fs::exists(root / "mods" / "evil.txt"));
    PCHECK(fs::exists(root / "mods" / ".installed" / "loose.zip") && !fs::exists(root / "mods" / "loose.zip"));
    PCHECK(report.find("skipped") != std::string::npos);
    fs::remove_all(root, ec);
}

// JS Engine's screen effects: its own GLSL, the parameters its Effect classes
// set, and each one's clock (blocked glitch: "time" from its argument; VCR:
// "iTime"; grain: "uTime" from 0 to 8).
void testJsEngineEffects() {
    scene::Scene sc;
    sc.keys = 4;
    sc.strums.resize(8);
    StubGame game;
    ScriptHost host(sc, game);
    PCHECK(host.loadString("fx.lua", R"LUA(
        function onCreatePost()
            addBlockedGlitchEffect('camHUD', 640, 2.5, 1, true)
            addBlockedGlitchEffect('camHUD', 640, 2.5, 1, false)
            addVCREffect('camGame', 0.3, false)
            addGrainEffect('camOther', 2, 0.5, true)
            addWiggleEffect('camOther', 'flag', 1, 2, 0.05)
            addPulseEffect('camOther', 1, 2, 3)
        end)LUA"));
    host.call("onCreatePost");
    for (const std::string& e : host.errors) std::printf("  %s\n", e.c_str());
    PCHECK(host.errors.empty());
    PCHECK(sc.camHud.filters.size() == 1);  // without its colour transform JS Engine adds nothing
    PCHECK(sc.camGame.filters.size() == 1 && sc.camOther.filters.size() == 3);
    if (sc.camHud.filters.size() != 1 || sc.camGame.filters.size() != 1 || sc.camOther.filters.size() != 3) return;
    scene::ShaderInstance& bg = *sc.camHud.filters[0];
    PCHECK(bg.name == "__fx_blocked" && bg.timeName == "time" && pnear(bg.floats["time"][0], 2.5));
    PCHECK(bg.floats["screenSize"].size() == 2 && pnear(bg.floats["screenSize"][1], 640));
    scene::ShaderInstance& vcr = *sc.camGame.filters[0];
    PCHECK(vcr.timeName == "iTime" && pnear(vcr.floats["glitchModifier"][0], 0.3));
    PCHECK(vcr.ints["distortionOn"] == std::vector<int>{0} && vcr.ints["perspectiveOn"] == std::vector<int>{1} &&
           vcr.ints["vignetteOn"] == std::vector<int>{1} && vcr.ints["scanlinesOn"] == std::vector<int>{1});
    scene::ShaderInstance& grain = *sc.camOther.filters[0];
    PCHECK(pnear(grain.floats["grainsize"][0], 2) && pnear(grain.floats["lumamount"][0], 0.5) && grain.ints["lockAlpha"] == std::vector<int>{1});
    const float g0 = grain.floats["uTime"][0];
    PCHECK(g0 >= 0 && g0 <= 8);
    scene::ShaderInstance& wig = *sc.camOther.filters[1];
    PCHECK(wig.ints["effectType"] == std::vector<int>{4} && pnear(wig.floats["uWaveAmplitude"][0], 0.05));
    scene::ShaderInstance& pulse = *sc.camOther.filters[2];
    PCHECK(pulse.ints["uEnabled"] == std::vector<int>{0} && pulse.timeRate == 0);  // stays off, its time still
    sc.update(0.5, 125.0, false);
    PCHECK(pnear(bg.floats["time"][0], 3.0) && pnear(vcr.floats["iTime"][0], 0.5) && pnear(grain.floats["uTime"][0], g0 + 0.5));
    // Every effect's GLSL goes through the translator into a whole shader.
    for (const char* n : {"__fx_chromatic", "__fx_scanline", "__fx_grain", "__fx_tiltshift", "__fx_vcr", "__fx_wiggle", "__fx_glitch",
                          "__fx_distort", "__fx_pulse", "__fx_invert", "__fx_greyscale", "__fx_3d", "__fx_bloom", "__fx_blocked"}) {
        const std::string_view src = fx::source(n);
        PCHECK(!src.empty() && src.find("void main") != std::string_view::npos);
        for (bool es : {false, true}) {
            const std::string g = translateShader(src, es);
            PCHECK(g.find("gh_FragColor") != std::string::npos && g.find("#pragma header") == std::string::npos);
        }
    }
}

}  // namespace

void runExtraTests(bool audio) {
    testAutoChart();
    testReplayDeterminism();
    testZipInstall();
    testChartStreaming();
    testShaderTranslation();
    testHScript();
    testLuaExtras();
    testJsEngineEffects();
    if (!audio) return;
    // AutoChart on the built-in songs: report the tempo it hears.
    for (const SongDef& def : builtinSongs()) {
        auto comp = compose(def, true);
        Chart c;
        AutoChartInfo info;
        const bool ok = autoChart(comp->inst.data(), comp->inst.size() / 2, 2, 44100, 1, c, &info);
        double ratio = info.bpm / def.bpm;
        while (ratio > 1.4) ratio /= 2;
        while (ratio < 0.7) ratio *= 2;
        std::printf("  AutoChart %-13s %6.1f BPM (real %5.1f)  %zu notes\n", def.title.c_str(), info.bpm, def.bpm, c.notes.size());
        PCHECK(ok && std::fabs(ratio - 1.0) < 0.03);
    }
}
