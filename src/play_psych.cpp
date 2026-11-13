// Psych mode of the play state: builds the scene from a mod's characters,
// stage and scripts, forwards gameplay to the scripts, handles Psych events,
// and implements the gameplay half of the Lua API.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

#include "basegame.hpp"
#include "common.hpp"
#include "effects.hpp"
#include "platform.hpp"
#include "psych_ui.hpp"
#include "state_play.hpp"
#include "rlgl.h"

namespace gh {

using scene::Character;
using scene::CharacterPtr;
using scene::Value;
using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {
namespace fs = std::filesystem;
const Color kGold{255, 214, 120, 255};
const Color kIvory{246, 236, 214, 255};

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool isScriptFile(const std::string& name) {
    return endsWithI(name, ".lua") || endsWithI(name, ".hx") || endsWithI(name, ".hscript") || endsWithI(name, ".hxs");
}

// Lua and HScript files directly in dir.
std::vector<std::string> luaFilesIn(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(toPath(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && isScriptFile(pathStr(it->path().filename()))) out.push_back(pathStr(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// stages/foo.lua and stages/foo.hx, whichever exist.
std::vector<std::string> scriptsNamed(const scene::Files& files, const std::string& base) {
    std::vector<std::string> out;
    for (const char* ext : {".lua", ".hx", ".hscript", ".hxs"})
        if (std::string f = files.find(base + ext); !f.empty()) out.push_back(f);
    return out;
}

double numOr(const std::string& s, double def) {
    const std::string t = trim(s);
    if (t.empty()) return def;
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    return end == t.c_str() || !std::isfinite(d) ? def : d;
}

Value parseLooseValue(const std::string& s) {
    const std::string t = trim(s);
    if (t == "true") return true;
    if (t == "false") return false;
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    if (!t.empty() && end && *end == 0 && std::isfinite(d)) return d;
    return s;
}

int keyCodeFromName(std::string_view nameIn) {
    const std::string n = toLower(trim(nameIn));
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return KEY_A + (n[0] - 'a');
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return KEY_ZERO + (n[0] - '0');
    static const char* digits[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    for (int i = 0; i < 10; ++i)
        if (n == digits[i]) return KEY_ZERO + i;
    if (n.size() >= 2 && n[0] == 'f' && std::isdigit((unsigned char)n[1])) {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return KEY_F1 + (f - 1);
    }
    if (n == "space") return KEY_SPACE;
    if (n == "enter") return KEY_ENTER;
    if (n == "escape") return KEY_ESCAPE;
    if (n == "shift") return KEY_LEFT_SHIFT;
    if (n == "control") return KEY_LEFT_CONTROL;
    if (n == "alt") return KEY_LEFT_ALT;
    if (n == "tab") return KEY_TAB;
    if (n == "backspace") return KEY_BACKSPACE;
    if (n == "left") return KEY_LEFT;
    if (n == "right") return KEY_RIGHT;
    if (n == "up") return KEY_UP;
    if (n == "down") return KEY_DOWN;
    return 0;
}

// Character selectors used by Psych's built-in events.
int whoForPlayAnimation(const std::string& v) {
    const std::string l = toLower(trim(v));
    if (l == "bf" || l == "boyfriend") return 0;
    if (l == "gf" || l == "girlfriend") return 2;
    const int n = int(std::lround(numOr(l, 0)));
    return n == 1 ? 0 : n == 2 ? 2 : 1;
}
}  // namespace

// ------------------------------------------------------------------ setup --

Character* PlayState::charForSide(int side) {
    scene::Scene& sc = *scene_;
    const bool humanIsDad = playerLeft_;
    if (side == 1) return humanIsDad ? sc.dad.get() : sc.bf.get();
    return humanIsDad ? sc.bf.get() : sc.dad.get();
}

std::string PlayState::singAnim(int lane, uint8_t flags) const {
    static const char* names[4] = {"singLEFT", "singDOWN", "singUP", "singRIGHT"};
    int d = gfx::laneDir(chart_.keys, lane);
    if (d == gfx::kCenter) d = 2;
    std::string a = names[clampv(d, 0, 3)];
    if (flags & kFlagAlt) a += "-alt";
    return a;
}

void PlayState::placeCharacter(Character& c, int slot) {
    const psych::StageDef& st = scene_->stage;
    const float* g = slot == 0 ? st.bf : slot == 1 ? st.dad : st.gf;
    float gx = g[0], gy = g[1];
    c.scrollX = c.scrollY = 1;
    if (slot == 2 || (slot == 1 && c.name.rfind("gf", 0) == 0)) {
        gx = st.gf[0];
        gy = st.gf[1];
        c.scrollX = c.scrollY = 0.95f;
        if (slot == 1) c.danceEvery = 2;
    }
    c.groupX = gx;
    c.groupY = gy;
    c.x = gx + c.posX;
    c.y = gy + c.posY;
}

CharacterPtr PlayState::makeCharacter(const std::string& name, bool isPlayer, int slot) {
    scene::Scene& sc = *scene_;
    auto c = std::make_shared<Character>();
    c->kind = scene::Kind::Character;
    c->name = name;
    c->isPlayer = isPlayer;
    c->tag = slot == 0 ? "boyfriend" : slot == 1 ? "dad" : "gf";
    std::string path = sc.files.find("characters/" + name + ".json");
    if (path.empty()) path = sc.files.find("data/characters/" + name + ".json");
    psych::CharacterDef def;
    bool ok = false;
    std::string err;
    if (!path.empty()) {
        std::string text;
        ok = scene::readFile(path, text) && psych::parseCharacter(text, def, err);
        if (!ok) debugNotice("character " + name + ": " + err, false, false);
    } else if (const char* base = basegame::characterJson(name)) {
        // Older engines hard-code the original characters; their images are in the mod.
        ok = psych::parseCharacter(base, def, err) && !def.images.empty() &&
             !sc.files.find("images/" + def.images[0] + ".png").empty();
    }
    scene::GraphicPtr g;
    if (ok) {
        std::string keys;
        for (const std::string& im : def.images) keys += (keys.empty() ? "" : ",") + im;
        g = sc.gfx.atlas(keys);
        if (!g) debugNotice("character " + name + ": image not found (" + keys + ")", false, false);
    }
    if (ok && g) {
        c->def = def;
        c->setGraphic(g);
        for (const psych::AnimDef& ad : def.anims) {
            scene::Anim a;
            a.name = ad.anim;
            a.frames = ad.indices.empty() ? psych::framesByPrefix(g->frames, ad.prefix)
                                          : psych::framesByIndices(g->frames, ad.prefix, ad.indices);
            if (a.frames.empty()) continue;
            a.fps = ad.fps;
            a.loop = ad.loop;
            a.hasOffset = true;
            a.offX = ad.offX;
            a.offY = ad.offY;
            c->addAnim(std::move(a));
        }
        if (def.scale != 1.0) {
            c->setGraphicSize(std::floor(c->width * float(def.scale)), 0);
            c->updateHitbox();
        }
        c->flipX = def.flipX != isPlayer;
        c->antialiasing = def.antialiasing;
        c->camX = def.camX;
        c->camY = def.camY;
        c->posX = def.posX;
        c->posY = def.posY;
        c->icon = def.icon;
        c->barColor = def.barColor;
        c->singDuration = def.singDuration;
        for (const char* m : {"singLEFTmiss", "singDOWNmiss", "singUPmiss", "singRIGHTmiss"})
            if (c->hasAnim(m)) c->hasMiss = true;
        c->recalcDance(true);
        c->dance();
    } else {
        if (slot == 2) return nullptr;  // no stand-in for a missing girlfriend
        c->builtin = true;
        c->builtinId = isPlayer ? int(CharId::Herald) : int(CharId::Shade);
        c->frameW = c->width = 440;
        c->frameH = c->height = 480;
        c->centerOrigin();
        c->posY = 290;
        c->icon = "";
        const Color cc = isPlayer ? charColor(CharId::Herald) : gfx::rgb(entry_->color);
        c->barColor = (uint32_t(cc.r) << 16) | (uint32_t(cc.g) << 8) | cc.b;
        c->recalcDance(true);
    }
    placeCharacter(*c, slot);
    return c;
}

void PlayState::setScriptGlobals() {
    App& a = app();
    const Settings& s = a.settings;
    ScriptHost& h = *scripts_;
    scene::Scene& sc = *scene_;
    const double bpm = chart_.bpmAt(0);
    auto set = [&](const char* n, const Value& v) { h.setGlobal(n, v); };
    set("luaDebugMode", false);
    set("luaDeprecatedWarnings", true);
    set("inChartEditor", false);
    set("version", std::string("0.7.3"));
    set("curBpm", bpm);
    set("bpm", bpm);
    set("scrollSpeed", chart_.speed);
    set("crochet", 60000.0 / bpm);
    set("stepCrochet", 15000.0 / bpm);
    set("songLength", chart_.lengthMs);
    set("songName", chart_.title.empty() ? entry_->title : chart_.title);
    set("songPath", entry_->songKey);
    set("startedCountdown", false);
    set("curStage", stageName_);
    set("isStoryMode", req_.story);
    set("difficulty", 1.0);
    set("difficultyName", req_.diff);
    set("difficultyPath", toLower(req_.diff));
    set("weekRaw", 0.0);
    set("week", std::string());
    set("seenCutscene", true);
    set("hasVocals", prep_.voiceP != nullptr || prep_.voiceO != nullptr);
    set("cameraX", 0.0);
    set("cameraY", 0.0);
    set("screenWidth", double(gfx::W));
    set("screenHeight", double(gfx::H));
    set("curSection", 0.0);
    set("curBeat", 0.0);
    set("curStep", 0.0);
    set("curDecBeat", 0.0);
    set("curDecStep", 0.0);
    set("score", 0.0);
    set("misses", 0.0);
    set("hits", 0.0);
    set("combo", 0.0);
    set("rating", 0.0);
    set("ratingName", std::string("?"));
    set("ratingFC", std::string());
    set("inGameOver", false);
    const SectionInfo* sec0 = chart_.sections.empty() ? nullptr : &chart_.sections[0];
    set("mustHitSection", sec0 ? sec0->mustHit : chart_.focusAt(0) == 1);
    set("altAnim", sec0 && sec0->alt);
    set("gfSection", sec0 && sec0->gf);
    set("healthGainMult", 1.0);
    set("healthLossMult", 1.0);
    set("playbackRate", rate_);
    set("guitarHeroSustains", false);
    set("instakillOnMiss", false);
    set("botPlay", cfg_.botplay);
    set("practice", cfg_.practice);
    for (int i = 0; i < chart_.keys && i < 9; ++i) {
        const scene::Strum& p = strum(1, i);
        const scene::Strum& o = strum(0, i);
        set(TextFormat("defaultPlayerStrumX%d", i), double(p.x));
        set(TextFormat("defaultPlayerStrumY%d", i), double(p.y));
        set(TextFormat("defaultOpponentStrumX%d", i), double(o.x));
        set(TextFormat("defaultOpponentStrumY%d", i), double(o.y));
    }
    set("defaultBoyfriendX", double(sc.stage.bf[0]));
    set("defaultBoyfriendY", double(sc.stage.bf[1]));
    set("defaultOpponentX", double(sc.stage.dad[0]));
    set("defaultOpponentY", double(sc.stage.dad[1]));
    set("defaultGirlfriendX", double(sc.stage.gf[0]));
    set("defaultGirlfriendY", double(sc.stage.gf[1]));
    set("boyfriendName", sc.bf ? sc.bf->name : std::string());
    set("dadName", sc.dad ? sc.dad->name : std::string());
    set("gfName", sc.gf ? sc.gf->name : std::string());
    set("downscroll", s.downscroll);
    set("middlescroll", s.middlescroll);
    set("framerate", double(s.fpsCap > 0 ? s.fpsCap : 240));
    set("ghostTapping", s.ghostTapping);
    set("hideHud", false);
    set("timeBarType", std::string("Time Left"));
    set("scoreZoom", true);
    set("cameraZoomOnBeat", s.camZoom);
    set("flashingLights", s.flashing);
    set("noteOffset", double(s.offsetMs));
    set("healthBarAlpha", 1.0);
    set("noResetButton", true);
    set("lowQuality", s.lowDetail);
    set("shadersEnabled", s.shaders);
    {
        // The song's folder inside mods/, as in Psych ("MyMod" for mods/MyMod/,
        // "" outside mods/), so 'mods/'..currentModDirectory..'/images/x.png'
        // finds the file.
        std::string dir = entry_->modRoot, mods = app().base + "mods/", cur;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        std::replace(mods.begin(), mods.end(), '\\', '/');
        if (dir.rfind(mods, 0) == 0) cur = dir.substr(mods.size());
        while (!cur.empty() && cur.back() == '/') cur.pop_back();
        set("currentModDirectory", cur);
    }
    set("noteSkin", std::string());
    set("noteSkinPostfix", std::string());
    set("splashSkin", std::string());
    set("splashSkinPostfix", std::string());
    set("splashAlpha", 1.0);
    set("buildTarget", std::string(
#if defined(_WIN32)
        "windows"
#elif defined(__APPLE__)
        "mac"
#else
        "linux"
#endif
        ));
    for (int i = 0; i < 2 * chart_.keys && i < 18; ++i) {  // Kade modcharts
        const scene::Strum& st = sc.strums[size_t(i)];
        set(TextFormat("defaultStrum%dX", i), double(st.x));
        set(TextFormat("defaultStrum%dY", i), double(st.y));
    }
    set("scrollspeed", chart_.speed);
    set("mustHit", chart_.focusAt(0) == 1);
    set("strumLine1Visible", true);
    set("cameraAngle", 0.0);
    set("camHudAngle", 0.0);
    set("followXOffset", 0.0);
    set("followYOffset", 0.0);
    set("hudWidth", double(gfx::W));
    set("hudHeight", double(gfx::H));
    set("keyCount", double(chart_.keys));
    set("playerKeyCount", double(chart_.keys));
    set("opponentKeyCount", double(chart_.keys));
}

void PlayState::updateStatGlobals() {
    if (!scripts_) return;
    ScriptHost& h = *scripts_;
    h.setGlobal("score", double(session_.score));
    h.setGlobal("misses", double(session_.misses));
    h.setGlobal("hits", double(session_.judged()));
    h.setGlobal("combo", double(session_.combo));
    h.setGlobal("rating", session_.accuracy());
    h.setGlobal("ratingName", ratingNameOverride_.empty() ? session_.rank() : ratingNameOverride_);
    h.setGlobal("ratingFC", ratingFcOverride_.empty() ? session_.clearLabel() : ratingFcOverride_);
}

void PlayState::loadScripts(bool afterHud) {
    scene::Scene& sc = *scene_;
    ScriptHost& h = *scripts_;
    if (!afterHud) {
        for (const char* ext : {".lua", ".hx", ".hscript", ".hxs"})
            for (const std::string& f : sc.files.list("scripts", ext)) h.load(f);
        const auto stageScripts = scriptsNamed(sc.files, "stages/" + stageName_);
        for (const std::string& f : stageScripts) h.load(f);
        if (stageScripts.empty() && baseStage_) h.loadString("stages/" + stageName_ + ".lua", basegame::stageLua(stageName_));
        for (Character* c : {sc.gf.get(), sc.dad.get(), sc.bf.get()}) {
            if (!c || c->builtin) continue;
            for (const std::string& f : scriptsNamed(sc.files, "characters/" + c->name))
                if (!h.isLoaded(f)) h.load(f);
        }
        return;
    }
    for (size_t i = 1; i < chart_.noteTypes.size(); ++i)
        for (const std::string& f : scriptsNamed(sc.files, "custom_notetypes/" + chart_.noteTypes[i])) h.load(f);
    std::vector<std::string> seen;
    for (const psych::ChartEvent& e : chart_.events) {
        if (std::find(seen.begin(), seen.end(), e.name) != seen.end()) continue;
        seen.push_back(e.name);
        for (const std::string& f : scriptsNamed(sc.files, "custom_events/" + e.name)) h.load(f);
    }
    for (const std::string& f : luaFilesIn(entry_->dataDir)) h.load(f);
}

bool PlayState::setupPsych() {
    App& a = app();
    scene::Scene& sc = *scene_;
    std::vector<std::string> roots = entry_->assetRoots;
    if (roots.empty() && !entry_->modRoot.empty()) roots.push_back(entry_->modRoot);
    roots.push_back(a.base + "mods/");
    sc.files.setRoots(roots);
    sc.files.setSharedRoots(a.library.sharedAssetRootsFor(*entry_));  // what this mod lacks, from the other mods
    // The Psych interface: this song's own mod first, as Psych's Paths does.
    {
        std::vector<std::string> own = entry_->assetRoots;
        if (own.empty() && !entry_->modRoot.empty()) own.push_back(entry_->modRoot);
        if (psychUi().active() && !own.empty() && hasPsychPieces(own)) {
            a.psychUiForSong(own);
            uiFromMod_ = true;
        }
    }

    // Charts from older engines name no stage: the original game picks it from the song.
    auto baseStageAvailable = [&](const std::string& st) {
        const char* probe = basegame::stageProbeImage(st);
        return probe && !sc.files.find(std::string("images/") + probe + ".png").empty();
    };
    std::string stageName = chart_.stage;
    if (stageName.empty()) {
        const std::string guess = basegame::stageForSong(entry_->songKey);
        stageName = baseStageAvailable(guess) ? guess : "stage";
    }
    stageName_ = stageName;
    const std::string playerName = chart_.player.empty() ? "bf" : chart_.player;
    const std::string oppName = chart_.opponent.empty() ? "dad" : chart_.opponent;
    std::string gfName = chart_.gf.empty() ? "gf" : chart_.gf;
    if (chart_.gf.empty() && baseStageAvailable(stageName)) gfName = basegame::gfForStage(stageName);
    const std::string stageJson = sc.files.find("stages/" + stageName + ".json");
    std::string stageLua = sc.files.find("stages/" + stageName + ".lua");
    if (stageLua.empty()) stageLua = sc.files.find("stages/" + stageName + ".hx");
    baseStage_ = stageLua.empty() && basegame::stageLua(stageName) && baseStageAvailable(stageName);
    bool any = !stageJson.empty() || !stageLua.empty() || baseStage_ || !luaFilesIn(entry_->dataDir).empty() ||
               !sc.files.list("scripts", ".lua").empty() || !sc.files.list("scripts", ".hx").empty();
    for (const std::string& n : {playerName, oppName, gfName}) {
        if (!sc.files.find("characters/" + n + ".json").empty()) any = true;
        if (const char* base = basegame::characterJson(n)) {
            psych::CharacterDef d;
            std::string e;
            if (psych::parseCharacter(base, d, e) && !d.images.empty() && !sc.files.find("images/" + d.images[0] + ".png").empty())
                any = true;
        }
    }
    if (!any) return false;

    if (!stageJson.empty()) {
        std::string text, err;
        if (!scene::readFile(stageJson, text) || !psych::parseStage(text, sc.stage, err))
            debugNotice("stage " + stageName + ": " + err, false, false);
    } else if (const char* bs = basegame::stageJson(stageName); bs && baseStageAvailable(stageName)) {
        std::string err;
        psych::parseStage(bs, sc.stage, err);
    }
    sc.defaultCamZoom = sc.stage.zoom;
    sc.camGame.zoom = float(sc.stage.zoom);
    sc.cameraSpeed = sc.stage.camSpeed;

    sc.gfMarker = std::make_shared<scene::Sprite>();
    sc.gfMarker->kind = scene::Kind::Marker;
    sc.gfMarker->tag = "gfGroup";
    if (!sc.stage.hideGf) sc.gf = makeCharacter(gfName, false, 2);
    sc.dad = makeCharacter(oppName, false, 1);
    sc.bf = makeCharacter(playerName, true, 0);
    if (sc.dad && sc.gf && sc.dad->name.rfind("gf", 0) == 0) sc.gf->visible = false;

    // Stage-editor objects (Psych 0.7.3+) carry the character layers too.
    bool placedChars = false;
    for (const psych::StageObject& o : sc.stage.objects) {
        const std::string t = toLower(o.type);
        if (t == "gf" || t == "gfgroup") {
            sc.order.push_back(sc.gfMarker);
            if (sc.gf) sc.order.push_back(sc.gf);
            placedChars = true;
            continue;
        }
        if (t == "dad" || t == "dadgroup") { if (sc.dad) sc.order.push_back(sc.dad); continue; }
        if (t == "boyfriend" || t == "boyfriendgroup") { if (sc.bf) sc.order.push_back(sc.bf); continue; }
        auto s = std::make_shared<scene::Sprite>();
        s->tag = o.name;
        if (t == "square") s->setGraphic(sc.gfx.solid(1, 1, 0xFF000000u | o.color));
        else if (t == "animatedsprite") s->setGraphic(sc.gfx.atlas(o.image));
        else s->setGraphic(sc.gfx.image(o.image));
        for (const psych::AnimDef& ad : o.anims) {
            if (!s->graphic) break;
            scene::Anim an;
            an.name = ad.anim;
            an.frames = ad.indices.empty() ? psych::framesByPrefix(s->graphic->frames, ad.prefix)
                                           : psych::framesByIndices(s->graphic->frames, ad.prefix, ad.indices);
            an.fps = ad.fps;
            an.loop = ad.loop;
            an.hasOffset = true;
            an.offX = ad.offX;
            an.offY = ad.offY;
            if (!an.frames.empty()) s->addAnim(std::move(an));
        }
        if (!o.firstAnim.empty()) s->play(o.firstAnim, true);
        s->x = o.x;
        s->y = o.y;
        s->scaleX = o.scaleX;
        s->scaleY = o.scaleY;
        if (t == "square") s->setGraphicSize(o.scaleX, o.scaleY), s->scaleX = o.scaleX, s->scaleY = o.scaleY;
        s->updateHitbox();
        s->scrollX = o.scrollX;
        s->scrollY = o.scrollY;
        s->alpha = o.alpha;
        s->angle = o.angle;
        s->color = o.color;
        s->flipX = o.flipX;
        s->flipY = o.flipY;
        s->antialiasing = o.antialiasing;
        s->inScene = true;
        if (!o.name.empty()) sc.objects[o.name] = s;
        sc.order.push_back(s);
    }
    if (!placedChars) {
        sc.order.push_back(sc.gfMarker);
        if (sc.gf) sc.order.push_back(sc.gf);
        if (sc.dad) sc.order.push_back(sc.dad);
        if (sc.bf) sc.order.push_back(sc.bf);
    } else {
        if (sc.dad && std::find(sc.order.begin(), sc.order.end(), sc.dad) == sc.order.end()) sc.order.push_back(sc.dad);
        if (sc.bf && std::find(sc.order.begin(), sc.order.end(), sc.bf) == sc.order.end()) sc.order.push_back(sc.bf);
    }
    for (const char* hudName : {"iconP1", "iconP2", "healthBar", "healthBarBG", "scoreTxt", "timeBar", "timeBarBG", "timeTxt",
                                "botplayTxt", "comboGroup", "grpNoteSplashes"}) {
        auto s = std::make_shared<scene::Sprite>();
        s->tag = hudName;
        s->cam = scene::Cam::Hud;
        sc.hud[hudName] = s;
    }
    sc.keys = chart_.keys;
    psych_ = true;  // layoutStrums uses Psych positions from here on
    layoutStrums();

    scripts_ = std::make_unique<ScriptHost>(sc, *this);
    scripts_->songDataDir = "data/" + entry_->songKey + "/";
    setScriptGlobals();
    loadScripts(false);
    sc.hudMarker = std::make_shared<scene::Sprite>();
    sc.hudMarker->kind = scene::Kind::Marker;
    sc.hudMarker->tag = "hud";
    sc.order.push_back(sc.hudMarker);
    loadScripts(true);
    for (const psych::ChartEvent& e : chart_.events)
        scripts_->call("onEventPushed", {Value(e.name), Value(e.v1), Value(e.v2), Value(e.timeMs)});
    // Psych's camera start: on the girlfriend, heading for the first section's
    // focus. Scripts may move both in onCreatePost.
    {
        float gx = sc.stage.camGf[0], gy = sc.stage.camGf[1];
        if (sc.gf) {
            gx += sc.gf->x + sc.gf->frameW * 0.5f + sc.gf->camX;
            gy += sc.gf->y + sc.gf->frameH * 0.5f + sc.gf->camY;
        }
        sc.followX = gx;
        sc.followY = gy;
        sc.camGame.scrollX = gx - gfx::W * 0.5f;
        sc.camGame.scrollY = gy - gfx::H * 0.5f;
        moveCameraSection(0);
        if (!chart_.sections.empty()) lastSection_ = 0;  // Psych calls sectionHit from section 1 on
    }
    scripts_->call("onCreatePost");
    scripts_->dispatchCompletions();

    // Characters a "Change Character" event will need, created now so their
    // textures load with everything else.
    for (const psych::ChartEvent& e : chart_.events) {
        if (e.name != "Change Character") continue;
        const std::string l = toLower(trim(e.v1));
        int who = l == "gf" || l == "girlfriend" ? 2 : l == "dad" || l == "opponent" ? 1 : int(numOr(l, 0));
        who = clampv(who, 0, 2);
        const std::string key = std::to_string(who) + ":" + trim(e.v2);
        if (!charCache_.count(key)) charCache_[key] = makeCharacter(trim(e.v2), who == 0, who);
    }

    for (Character* c : {sc.dad.get(), sc.bf.get()})
        if (c) barColor_[c == sc.dad.get() ? 0 : 1] = c->barColor;
    loadIcons();
    renderer_.preload(sc);
    backdrop_ = stageLua.empty() && !baseStage_ && sc.stage.objects.empty();
    if (!sc.gfx.missing.empty()) {
        std::string list;
        for (size_t i = 0; i < sc.gfx.missing.size() && i < 4; ++i) list += (i ? ", " : "") + sc.gfx.missing[i];
        debugNotice(std::to_string(sc.gfx.missing.size()) + " image(s) missing: " + list, false, false);
    }
    return true;
}

void PlayState::loadIcons() {
    scene::Scene& sc = *scene_;
    for (int i = 0; i < 2; ++i) {
        if (iconTex_[i].id && IsWindowReady()) UnloadTexture(iconTex_[i]);
        iconTex_[i] = Texture2D{};
        hasPsychIcon_[i] = false;
        Character* c = i == 0 ? sc.dad.get() : sc.bf.get();
        if (!c || c->builtin) continue;
        std::string f = sc.files.find("images/icons/icon-" + c->icon + ".png");
        if (f.empty()) f = sc.files.find("images/icons/" + c->icon + ".png");
        if (f.empty()) f = sc.files.find("images/icons/icon-face.png");
        if (f.empty()) continue;
        std::string bytes;
        if (!scene::readFile(f, bytes)) continue;
        Image img = LoadImageFromMemory(".png", reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()));
        if (!img.data) continue;
        iconTex_[i] = LoadTextureFromImage(img);
        UnloadImage(img);
        SetTextureFilter(iconTex_[i], c->antialiasing ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
        iconFrames_[i] = std::max(1, int(std::lround(double(iconTex_[i].width) / std::max(1, iconTex_[i].height))));
        hasPsychIcon_[i] = true;
    }
}

// ----------------------------------------------------------------- update --

void PlayState::psychUpdate(double dt) {
    App& a = app();
    scene::Scene& sc = *scene_;
    for (int l = 0; l < 9; ++l) {
        prevKeys_[l] = keyNow_[l];
        keyNow_[l] = heldCount_[l] > 0;
    }
    for (int b = 0; b < 3; ++b) {
        prevMouse_[b] = mouseNow_[b];
        mouseNow_[b] = IsMouseButtonDown(b);
    }
    scripts_->setGlobal("curDecBeat", chart_.beatAt(songMs_));
    scripts_->call("onUpdate", {Value(dt)});
    bool holding = false;
    for (int l = 0; l < chart_.keys; ++l)
        holding |= heldCount_[l] > 0 || (session_.botplay() && session_.isHolding(1, l));
    sc.update(dt, chart_.stepMs(std::max(0.0, songMs_)), holding);
    scripts_->dispatchCompletions();
    updateSounds(dt);
    if (scripts_->kadeMode) scripts_->call("update", {Value(dt)});
    scripts_->call("onUpdatePost", {Value(dt)});
    (void)a;
}

void PlayState::psychHoldRefresh() {
    const double now = app().now;
    const double stepSec = chart_.stepMs(std::max(0.0, songMs_)) / 1000.0 / rate_;
    for (int side = 0; side < 2; ++side)
        for (int l = 0; l < chart_.keys; ++l) {
            const long long h = session_.activeHold(side, l);
            if (h < 0) continue;
            if (now - holdRefresh_[side][l] < stepSec) continue;
            holdRefresh_[side][l] = now;
            const LaneNote& n = session_.lane(side, l)[size_t(h)];
            Character* ch = (n.flags & kFlagGF) && scene_->gf ? scene_->gf.get() : charForSide(side);
            if (ch && !(n.flags & kFlagNoAnim)) {
                std::string anim = singAnim(l, n.flags);
                if ((n.flags & kFlagAlt) && !ch->builtin && !ch->hasAnim(anim)) anim = singAnim(l, 0);
                ch->playAnim(anim, true);
                ch->holdTimer = 0;
            }
            const char* fn = side == 1 ? "goodNoteHit" : "opponentNoteHit";
            if (app().settings.luaNoteCallbacks)
                scripts_->call(fn, {Value(double(n.id)), Value(double(l)), Value(chart_.noteTypes[n.type]), Value(true)});
        }
}

void PlayState::psychNoteHit(const GameEvent& ev, bool opponent) {
    scene::Scene& sc = *scene_;
    const int side = opponent ? 0 : 1;
    Character* ch = (ev.flags & kFlagGF) && sc.gf ? sc.gf.get() : charForSide(side);
    if (ch && !(ev.flags & kFlagNoAnim)) {
        std::string anim = singAnim(ev.lane, ev.flags);
        if ((ev.flags & kFlagAlt) && !ch->builtin && !ch->hasAnim(anim)) anim = singAnim(ev.lane, 0);
        ch->playAnim(anim, true);
        ch->holdTimer = 0;
        if (ev.flags & kFlagHey) {
            const char* chk = ch == sc.gf.get() ? "cheer" : "hey";
            if (ch->hasAnim(chk)) {
                ch->playAnim(chk, true);
                ch->specialAnim = true;
                ch->heyTimer = 0.6;
            }
        }
    }
    holdRefresh_[side][ev.lane] = app().now;
    const std::string& type = chart_.noteTypes[ev.noteType < chart_.noteTypes.size() ? ev.noteType : 0];
    // A note hit on either side starts the camera bops and lets zoom events
    // ease back (Psych / JS Engine: camZooming; Tutorial keeps its own zoom).
    if (toLower(entry_->songKey) != "tutorial") sc.camZooming = true;
    if (opponent) {
        app().mixer.setTrackVolume(Mixer::kVoiceOpponent, 1.0f);
        if (app().settings.luaNoteCallbacks)
            scripts_->call("opponentNoteHit", {Value(double(ev.id)), Value(double(ev.lane)), Value(type), Value(false)});
    } else {
        lastCombo_ = session_.combo;
        if (app().settings.luaNoteCallbacks)
            scripts_->call("goodNoteHit", {Value(double(ev.id)), Value(double(ev.lane)), Value(type), Value(false)});
    }
}

void PlayState::psychMiss(const GameEvent& ev, bool ghost) {
    scene::Scene& sc = *scene_;
    Character* ch = (ev.flags & kFlagGF) && sc.gf ? sc.gf.get() : charForSide(1);
    if (ghost) {
        if (ch && ch->hasMiss) ch->playAnim(singAnim(ev.lane, 0) + "miss", true);
        if (app().settings.luaNoteCallbacks) scripts_->call("noteMissPress", {Value(double(ev.lane))});
        return;
    }
    if (ev.type == GameEvent::PlayerMine) {
        if (ch && ch->hasAnim("hurt")) {
            ch->playAnim("hurt", true);
            ch->specialAnim = true;
        }
    } else if (ch && ch->hasMiss && !(ev.flags & kFlagNoMissAnim)) {
        ch->playAnim(singAnim(ev.lane, 0) + "miss", true);
    }
    if (lastCombo_ > 5 && sc.gf && ch != sc.gf.get() && sc.gf->hasAnim("sad")) {
        sc.gf->playAnim("sad", true);
        sc.gf->specialAnim = true;
    }
    lastCombo_ = 0;
    const std::string& type = chart_.noteTypes[ev.noteType < chart_.noteTypes.size() ? ev.noteType : 0];
    if (app().settings.luaNoteCallbacks)
        scripts_->call("noteMiss", {Value(double(ev.id)), Value(double(ev.lane)), Value(type),
                                    Value(ev.type == GameEvent::HoldDrop)});
}

void PlayState::psychBeat(int b) {
    App& a = app();
    scene::Scene& sc = *scene_;
    scripts_->setGlobal("curBeat", double(b));
    scripts_->setGlobal("curDecBeat", chart_.beatAt(songMs_));
    for (Character* c : {sc.bf.get(), sc.dad.get(), sc.gf.get()}) {
        if (!c) continue;
        const int every = c == sc.gf.get() ? std::max(1, int(std::lround(sc.gfSpeed * c->danceEvery))) : std::max(1, c->danceEvery);
        const std::string an = c->builtin ? c->lastAnim : c->curAnimName();
        if (((b % every) + every) % every == 0 && an.rfind("sing", 0) != 0 && !c->stunned) c->dance();
    }
    if (b >= 0 && b % std::max(1, bopEvery_) == 0 && sc.camZooming && sc.camGame.zoom < 1.35f && a.settings.camZoom) {
        sc.camGame.zoom += float(0.015 * sc.camZoomingMult);
        sc.camHud.zoom += float(0.03 * sc.camZoomingMult);
    }
    const int gfSpeed = std::max(1, int(std::lround(sc.gfSpeed)));
    if (camTwist_ && b >= 0 && b % gfSpeed == 0) {
        // JS Engine "Camera Twist": both cameras tilt one way, then the other, easing back.
        const float angle = float(twistIntensity_ * (twisted_ ? -1 : 1));
        twisted_ = !twisted_;
        const double bpm = chart_.bpmAt(std::max(0.0, songMs_));
        for (const char* cam : {"camHUD", "camGame"}) {
            sc.setProp(std::string(cam) + ".angle", double(angle));
            scene::Tween tw;
            tw.tag = std::string("__twist") + cam;
            tw.target = cam;
            tw.fields.push_back({"angle", 0, 0, false});
            tw.dur = 45.0 / std::max(1.0, bpm) * gfSpeed / rate_;
            tw.ease = "circOut";
            sc.tween(std::move(tw));
        }
    }
    scripts_->call("onBeatHit", {Value(double(b))});  // the beat argument is a PulseForge extension
    if (scripts_->kadeMode) scripts_->call("beatHit", {Value(double(b))});
}

void PlayState::psychFocus(int side) {
    scene::Scene& sc = *scene_;
    Character* c = charForSide(side == 1 ? 1 : 0);
    focusWho(c && c == sc.bf.get() ? 0 : 1);
}

void PlayState::focusWho(int who) {
    scene::Scene& sc = *scene_;
    if (who == 2 && !sc.gf) who = 0;
    sc.focusOn(who);
    if (scripts_) {
        scripts_->call("onMoveCamera", {Value(std::string(who == 0 ? "boyfriend" : who == 1 ? "dad" : "gf"))});
        if (scripts_->kadeMode && who < 2) scripts_->call(who == 0 ? "playerOneTurn" : "playerTwoTurn");
        scripts_->setGlobal("mustHit", who == 0);
    }
}

void PlayState::moveCameraSection(int sec) {
    if (chart_.sections.empty()) {
        psychFocus(chart_.focusAt(songMs_));
        return;
    }
    const SectionInfo& s = chart_.sections[size_t(clampv(sec, 0, int(chart_.sections.size()) - 1))];
    focusWho(s.gf && scene_->gf ? 2 : s.mustHit ? 0 : 1);
}

void PlayState::handleBuiltinEvent(const psych::ChartEvent& e) {
    App& a = app();
    const std::string& n = e.name;
    if (n == "Add Camera Zoom") {
        // Adds to the zoom like Psych / JS Engine: before the first note is hit
        // (camZooming off) nothing eases it back, so the zoom stays and every
        // further event adds to it (Eons pulls the camera back at 0 s and moves
        // it in a little at each bar of the intro). Works whatever "Camera zoom
        // on beat" says: that option only stops the beat bops.
        // Each camera stops growing at 1.35 (Psych checks the game camera, JS
        // Engine the HUD; checking each keeps the HUD on screen while the
        // camera keeps adding up).
        const float gz = float(numOr(e.v1, 0.015)), hz = float(numOr(e.v2, 0.03));
        float& game = psych_ ? scene_->camGame.zoom : camZoom_;
        float& hud = psych_ ? scene_->camHud.zoom : hudZoom_;
        if (game < 1.35f || gz < 0) game = std::max(0.05f, game + gz);
        if (hud < 1.35f || hz < 0) hud = std::max(0.05f, hud + hz);
        return;
    }
    if (n == "Change Scroll Speed") {
        const double mult = numOr(e.v1, 1.0), dur = numOr(e.v2, 0.0);
        if (dur <= 0) {
            speedMult_ = mult;
            speedTweenDur_ = 0;
        } else {
            speedTweenFrom_ = speedMult_;
            speedTweenTo_ = mult;
            speedTweenT_ = 0;
            speedTweenDur_ = dur / rate_;
        }
        return;
    }
    if (handleJsEngineEvent(e)) return;
    if (!psych_) return;
    scene::Scene& sc = *scene_;
    if (n == "Hey!") {
        const std::string l = toLower(trim(e.v1));
        int which = 2;
        if (l == "bf" || l == "boyfriend" || l == "0") which = 0;
        else if (l == "gf" || l == "girlfriend" || l == "1") which = 1;
        const double t = numOr(e.v2, 0.6) <= 0 ? 0.6 : numOr(e.v2, 0.6);
        if (which != 0) {
            Character* g = sc.dad && sc.dad->name.rfind("gf", 0) == 0 ? sc.dad.get() : sc.gf.get();
            if (g) { g->playAnim("cheer", true); g->specialAnim = true; g->heyTimer = t; }
        }
        if (which != 1 && sc.bf) { sc.bf->playAnim("hey", true); sc.bf->specialAnim = true; sc.bf->heyTimer = t; }
    } else if (n == "Set GF Speed") {
        sc.gfSpeed = std::max(1.0, std::round(numOr(e.v1, 1)));
    } else if (n == "Play Animation") {
        Character* c = sc.character(std::to_string(whoForPlayAnimation(e.v2)));
        if (c) { c->playAnim(trim(e.v1), true); c->specialAnim = true; }
    } else if (n == "Camera Follow Pos") {
        sc.camForced = false;
        const std::string v1 = trim(e.v1), v2 = trim(e.v2);
        if (!v1.empty() || !v2.empty()) {
            sc.camForced = true;
            sc.followX = float(numOr(v1, 0));
            sc.followY = float(numOr(v2, 0));
        } else {
            moveCameraSection(chart_.sectionAt(songMs_));
        }
    } else if (n == "Alt Idle Animation") {
        const std::string l = toLower(trim(e.v1));
        int who = 1;
        if (l == "gf" || l == "girlfriend") who = 2;
        else if (l == "boyfriend" || l == "bf") who = 0;
        else { const int v = int(numOr(l, 0)); who = v == 1 ? 0 : v == 2 ? 2 : 1; }
        if (Character* c = sc.character(std::to_string(who))) {
            c->idleSuffix = e.v2;
            c->recalcDance(false);
        }
    } else if (n == "Screen Shake") {
        const std::string vals[2] = {e.v1, e.v2};
        scene::Camera* cams[2] = {&sc.camGame, &sc.camHud};
        for (int i = 0; i < 2; ++i) {
            const size_t comma = vals[i].find(',');
            const double dur = numOr(vals[i].substr(0, comma), 0);
            const double inten = comma == std::string::npos ? 0 : numOr(vals[i].substr(comma + 1), 0);
            if (dur > 0 && inten != 0 && a.settings.flashing) {
                cams[i]->shakeIntensity = float(inten);
                cams[i]->shakeTime = float(dur);
            }
        }
    } else if (n == "Change Character") {
        const std::string l = toLower(trim(e.v1));
        int who = l == "gf" || l == "girlfriend" ? 2 : l == "dad" || l == "opponent" ? 1 : int(numOr(l, 0));
        changeCharacter(clampv(who, 0, 2), trim(e.v2));
    } else if (n == "Set Property") {
        const std::string path = trim(e.v1);
        const Value v = parseLooseValue(e.v2);
        if (!setGameProp(path, v)) sc.setProp(path, v);
    } else if (n == "Play Sound") {
        playSound(trim(e.v1), numOr(e.v2, 1.0), "", false);
    } else if (n == "__vslice_zoom") {
        const size_t p1 = e.v2.find('|');
        const size_t p2 = p1 == std::string::npos ? std::string::npos : e.v2.find('|', p1 + 1);
        const std::string mode = p2 == std::string::npos ? "stage" : e.v2.substr(p2 + 1);
        double z = numOr(e.v1, 1.0);
        if (mode != "direct") z *= sc.stage.zoom;
        sc.defaultCamZoom = z;
        sc.camZooming = true;
    } else if (n == "__vslice_bop") {
        bopEvery_ = std::max(1, int(numOr(e.v1, 4)));
        sc.camZoomingMult = numOr(e.v2, 1.0);
    }
}

// ------------------------------------------------------ JS Engine events --

bool PlayState::handleJsEngineEvent(const psych::ChartEvent& e) {
    App& a = app();
    const std::string& n = e.name;
    const std::string v1 = trim(e.v1), v2 = trim(e.v2);
    scene::Scene* sc = psych_ ? scene_.get() : nullptr;
    auto zoomValue = [&]() {  // empty or "default": the stage's own zoom
        const double base = sc ? sc->stage.zoom : 1.0;
        return v1.empty() || toLower(v1) == "default" ? base : numOr(v1, base);
    };
    if (n == "Set Camera Zoom" || n == "Set Cam Zoom") {
        // JS Engine: the camera is the default zoom plus what zoom events added.
        // While camZooming is on it eases to the new default; before that it
        // moves at once and keeps what the events added.
        if (sc) {
            const double z = zoomValue();
            if (!sc->camZooming) sc->camGame.zoom = std::max(0.05f, sc->camGame.zoom + float(z - sc->defaultCamZoom));
            sc->defaultCamZoom = z;
        }
        return true;
    }
    if (n == "Tween Camera Zoom") {
        if (!sc) return true;
        const double z = zoomValue();
        const size_t comma = v2.find(',');
        const double dur = std::max(0.0, numOr(trim(v2.substr(0, comma)), 0)) / rate_;
        const std::string ease = comma == std::string::npos ? "linear" : trim(v2.substr(comma + 1));
        scene::Tween tw;
        tw.tag = "__jsCamZoom";
        tw.ease = ease.empty() ? "linear" : ease;
        tw.dur = std::max(1e-4, dur);
        if (sc->camZooming) {
            tw.target = "";
            tw.fields.push_back({"defaultCamZoom", 0, z, false});
        } else {
            sc->defaultCamZoom = z;
            tw.target = "camGame";
            tw.fields.push_back({"zoom", 0, z, false});
        }
        sc->tween(std::move(tw));
        return true;
    }
    if (n == "Enable Camera Bop") {
        if (sc) sc->camZooming = true;
        return true;
    }
    if (n == "Disable Camera Bop") {
        if (sc) {
            sc->camZooming = false;
            sc->camGame.zoom = float(sc->defaultCamZoom);
            sc->camHud.zoom = 1;
        }
        return true;
    }
    if (n == "Camera Bopping") {
        bopEvery_ = std::max(1, int(numOr(v1, 4)));
        if (sc) sc->camZoomingMult = numOr(v2, 1);
        return true;
    }
    if (n == "Camera Twist") {
        twistIntensity_ = numOr(v1, 0);
        camTwist_ = numOr(v2, 0) != 0;
        if (!camTwist_ && sc) {
            for (const char* cam : {"camHUD", "camGame"}) {
                scene::Tween tw;
                tw.tag = std::string("__twist") + cam;
                tw.target = cam;
                tw.fields = {{"angle", 0, 0, false}, {"x", 0, 0, false}, {"y", 0, 0, false}};
                tw.dur = 1;
                tw.ease = "sineOut";
                sc->tween(std::move(tw));
            }
        }
        return true;
    }
    if (n == "Change Note Multiplier") {
        const long long m = std::max(1LL, std::llround(clampv(numOr(v1, 1), 1.0, 1e15)));
        if (v2.empty() || v2 == "1") session_.setNoteMultiplier(playerLeft_ ? 1 : 0, m);
        if (v2.empty() || v2 == "2") session_.setNoteMultiplier(playerLeft_ ? 0 : 1, m);
        return true;
    }
    if (n == "Fake Song Length") {
        double target = numOr(v1, -1) * 1000.0;
        if (v1.empty() || target < 0) target = chart_.lengthMs;
        if (toLower(v2) == "true") {
            fakeLenFrom_ = shownLengthMs();
            fakeLenTo_ = target;
            fakeLenT_ = 0;
            fakeLenDur_ = 1.0;
            fakeLenMs_ = fakeLenFrom_;
        } else {
            fakeLenDur_ = 0;
            fakeLenMs_ = target;
        }
        return true;
    }
    if (n == "Change Song Name") {
        songNameShown_ = v1.size() > 1 ? e.v1 : std::string();
        return true;
    }
    if (n == "Rainbow Eyesore") {
        const double speed = numOr(v2, 1) <= 0 ? 1.0 : numOr(v2, 1);
        if (!sc || !a.settings.flashing || !a.settings.shaders || lastStep_ >= int(numOr(v1, 0))) return true;
        rainbowEndStep_ = int(numOr(v1, 0));
        if (!rainbow_) {
            rainbow_ = fx::make(*sc, "__fx_pulse", true);
            if (rainbow_) sc->camGame.filters.push_back(rainbow_);
        }
        if (rainbow_) {
            // JS Engine: its time starts anywhere from -1000 to 1000 and
            // advances by elapsed * speed (PulseShader.update).
            rainbow_->floats["uWaveAmplitude"] = {1.0f};
            rainbow_->floats["uFrequency"] = {2.0f};
            rainbow_->floats["uSpeed"] = {float(speed * rate_)};
            rainbow_->floats["uTime"] = {float(GetRandomValue(-1000000, 1000000)) / 1000.0f};
            rainbow_->timeRate = float(speed * rate_);
            rainbow_->ints["uEnabled"] = {1};
        }
        return true;
    }
    if (n == "Popup" || n == "Popup (No Pause)") {
        notices_.push_back({e.v1, e.v2, a.now, 2});
        if (n == "Popup" && !popupHold_) {
            popupHold_ = true;
            a.mixer.pause();
        }
        return true;
    }
    if (n == "Credits Popup") {
        notices_.push_back({v1.size() > 1 ? e.v1 : (chart_.title.empty() ? entry_->title : chart_.title),
                            v2.size() > 1 ? e.v2 : std::string(), a.now, 0});
        return true;
    }
    if (n == "Windows Notification") {
        notices_.push_back({v1.empty() ? std::string("God's Hand") : e.v1, e.v2, a.now, 1});
        return true;
    }
    if (n == "Enable Bot Energy" || n == "Disable Bot Energy") {
        botEnergyOn_ = n == "Enable Bot Energy" && !cfg_.botplay;
        if (!botEnergyOn_) {
            usingBotEnergy_ = false;
            session_.setAutoLanes(0);
        }
        return true;
    }
    if (n == "Set Bot Energy Speeds") {
        energyDrain_ = numOr(v1, 1);
        energyRefill_ = numOr(v2, 1);
        return true;
    }
    return false;
}

void PlayState::updateJsEngine(double dt) {
    App& a = app();
    if (fakeLenDur_ > 0) {  // expoOut, like JS Engine's length tween
        fakeLenT_ = std::min(fakeLenDur_, fakeLenT_ + dt);
        const double k = fakeLenT_ >= fakeLenDur_ ? 1.0 : 1.0 - std::pow(2.0, -10.0 * fakeLenT_ / fakeLenDur_);
        fakeLenMs_ = fakeLenFrom_ + (fakeLenTo_ - fakeLenFrom_) * k;
        if (fakeLenT_ >= fakeLenDur_) fakeLenDur_ = 0;
    }
    if (rainbow_) {  // fades out once its last step has passed
        auto& amp = rainbow_->floats["uWaveAmplitude"];
        if (amp.empty()) amp.push_back(0);
        if (lastStep_ >= rainbowEndStep_ || phase_ == Phase::Dead) amp[0] -= float(dt / 2);
        if (amp[0] <= 0) {
            auto& f = scene_->camGame.filters;
            f.erase(std::remove(f.begin(), f.end(), rainbow_), f.end());
            rainbow_.reset();
        }
    }
    // Notices disappear on their own; a popup window waits for OK.
    notices_.erase(std::remove_if(notices_.begin(), notices_.end(),
                                  [&](const Notice& n) {
                                      return (n.kind == 0 && a.now - n.t0 > 4.6) || (n.kind == 1 && a.now - n.t0 > 4.5);
                                  }),
                   notices_.end());
    // Bot energy: hold CTRL (or BOT on a touchscreen) and the bot plays the
    // lanes you are holding while the bar drains; it refills otherwise.
    energyAlpha_ = approach(energyAlpha_, botEnergyOn_ ? 1.0f : 0.0f, 6.0f, float(dt));
    if (!botEnergyOn_ || cfg_.botplay) return;
    const bool key = a.input.isDown(KEY_LEFT_CONTROL) || a.input.isDown(KEY_RIGHT_CONTROL);
    usingBotEnergy_ = key && !noEnergy_;
    unsigned mask = 0;
    int held = 0;
    for (int l = 0; l < chart_.keys && l < 9; ++l)
        if (heldCount_[l] > 0) {
            mask |= 1u << l;
            ++held;
        }
    session_.setAutoLanes(usingBotEnergy_ ? mask : 0);
    if (usingBotEnergy_) botEnergy_ -= dt / 5.0 * held * energyDrain_ * rate_;
    else botEnergy_ += dt / 5.0 * energyRefill_ * rate_;
    botEnergy_ = std::min(2.0, botEnergy_);
    if (botEnergy_ <= 0 && !noEnergy_) {
        botEnergy_ = 0;
        noEnergy_ = true;
        energyCooldown_ = 1;
        session_.setAutoLanes(0);
    }
    if (noEnergy_) {
        energyCooldown_ -= dt;
        if (energyCooldown_ <= 0 && !key) noEnergy_ = false;
    }
}

void PlayState::drawNotices() {
    const double now = app().now;
    for (const Notice& n : notices_) {
        const double age = now - n.t0;
        if (n.kind == 0) {  // credits: slides in from the left, waits, slides back
            const float w = std::max(360.0f, gfx::measure(FontId::Title, n.title, 34).x + 60);
            float k;
            if (age < 0.5) k = easeOutBack(float(age / 0.5));
            else if (age < 3.5) k = 1;
            else {
                const float t = float(std::min(1.0, (age - 3.5) / 1.0));
                const float c1 = 1.70158f, c3 = c1 + 1;
                k = 1 - (c3 * t * t * t - c1 * t * t);
            }
            const float x = -w + w * k;
            DrawRectangleRec({x, 200, w, n.text.empty() ? 70.0f : 110.0f}, Color{10, 6, 18, 215});
            DrawRectangleRec({x + w - 6, 200, 6, n.text.empty() ? 70.0f : 110.0f}, kGold);
            gfx::text(FontId::Title, n.title, x + 24, 214, 34, WHITE, Align::Left, 2.0f);
            if (!n.text.empty()) gfx::text(FontId::Ui, n.text, x + 24, 262, 26, fade(kIvory, 0.9f), Align::Left, 1.5f);
        } else if (n.kind == 1) {  // notification: a toast at the top right
            const float al = float(std::min({1.0, age * 4.0, (4.5 - age) * 3.0}));
            const float w = std::max(gfx::measure(FontId::Ui, n.text, 22).x, gfx::measure(FontId::Ui, n.title, 26).x) + 40;
            const float x = gameW_ - w - 20;
            DrawRectangleRounded({x, 70, w, 76}, 0.2f, 8, fade(Color{20, 12, 36, 255}, 0.9f * al));
            gfx::text(FontId::Ui, n.title, x + 20, 80, 26, fade(kGold, al), Align::Left, 1.5f);
            gfx::text(FontId::Ui, n.text, x + 20, 112, 22, fade(WHITE, al), Align::Left, 1.5f);
        }
    }
    for (const Notice& n : notices_) {
        if (n.kind != 2) continue;  // popup window, centred, until OK
        const float w = std::clamp(gfx::measure(FontId::Ui, n.text, 26).x + 80, 420.0f, gameW_ - 80);
        const float x = (gameW_ - w) * 0.5f, y = 250;
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 90});
        DrawRectangleRounded({x, y, w, 220}, 0.08f, 8, Color{24, 16, 40, 245});
        DrawRectangleRoundedLinesEx({x, y, w, 220}, 0.08f, 8, 2, fade(kGold, 0.8f));
        gfx::text(FontId::Title, n.title, x + w * 0.5f, y + 18, 32, kGold, Align::Center, 2.0f);
        gfx::text(FontId::Ui, n.text, x + w * 0.5f, y + 80, 26, WHITE, Align::Center, 1.5f);
        gfx::text(FontId::Ui, app().touchMode ? "Tap to close" : "ENTER to close", x + w * 0.5f, y + 168, 22,
                  fade(kIvory, 0.7f), Align::Center);
        break;
    }
}

void PlayState::drawBotEnergy() {
    if (energyAlpha_ <= 0.01f) return;
    const float al = energyAlpha_;
    const float x = gameW_ - 60, y0 = 160, h = 400;
    const float fill = float(clampv(botEnergy_ / 2.0, 0.0, 1.0));
    const Color c = botEnergy_ < 0.2 ? Color{198, 0, 0, 255} : botEnergy_ > 1.8 ? Color{0, 188, 18, 255} : Color{0, 148, 255, 255};
    DrawRectangleRounded({x - 4, y0 - 4, 30, h + 8}, 0.5f, 8, fade(Color{8, 4, 14, 255}, 0.8f * al));
    DrawRectangleRounded({x, y0 + h * (1 - fill), 22, h * fill}, 0.5f, 8, fade(c, al));
    const std::string label = botEnergy_ < 2 ? TextFormat("%d%%", int(std::lround(botEnergy_ * 50))) : "Full";
    gfx::text(FontId::Ui, label, x + 11, y0 + h + 12, 22, fade(usingBotEnergy_ ? kGold : WHITE, al), Align::Center, 1.5f);
    gfx::text(FontId::Ui, "BOT", x + 11, y0 - 34, 20, fade(kIvory, 0.8f * al), Align::Center, 1.5f);
}

void PlayState::changeCharacter(int who, const std::string& name) {
    if (!psych_ || name.empty()) return;
    scene::Scene& sc = *scene_;
    CharacterPtr& slot = who == 0 ? sc.bf : who == 1 ? sc.dad : sc.gf;
    if (slot && slot->name == name) return;
    const std::string key = std::to_string(who) + ":" + name;
    CharacterPtr next;
    if (auto it = charCache_.find(key); it != charCache_.end()) next = it->second;
    if (!next) next = makeCharacter(name, who == 0, who);
    if (!next) return;
    if (slot) {
        next->alpha = slot->alpha;
        next->visible = slot->visible;
        auto it = std::find(sc.order.begin(), sc.order.end(), slot);
        if (it != sc.order.end()) *it = next;
        if (slot->groupX != next->groupX || slot->groupY != next->groupY) {
            next->x += slot->groupX - next->groupX;
            next->y += slot->groupY - next->groupY;
            next->groupX = slot->groupX;
            next->groupY = slot->groupY;
        }
    } else {
        auto it = std::find(sc.order.begin(), sc.order.end(), sc.gfMarker);
        sc.order.insert(it == sc.order.end() ? sc.order.end() : it + 1, next);
    }
    charCache_[std::to_string(who) + ":" + (slot ? slot->name : "")] = slot;
    slot = next;
    renderer_.preload(sc);
    if (!customBarColors_ && who < 2) {
        barColor_[who == 1 ? 0 : 1] = next->barColor;
        leftColor_ = Color{(unsigned char)(barColor_[0] >> 16), (unsigned char)(barColor_[0] >> 8), (unsigned char)barColor_[0], 255};
        rightColor_ = Color{(unsigned char)(barColor_[1] >> 16), (unsigned char)(barColor_[1] >> 8), (unsigned char)barColor_[1], 255};
    }
    if (who < 2) loadIcons();
    if (scripts_) {
        scripts_->setGlobal(who == 0 ? "boyfriendName" : who == 1 ? "dadName" : "gfName", name);
        if (std::string f = sc.files.find("characters/" + name + ".lua"); !f.empty() && !scripts_->isLoaded(f)) scripts_->load(f);
    }
}

// ------------------------------------------------------------------- draw --

void PlayState::drawHudLayer() {
    scene::Scene& sc = *scene_;
    const scene::Camera& cam = sc.camHud;
    if (!cam.visible || cam.alpha <= 0.002f) return;
    gfx::endCanvas();
    const Vector2 focus{gfx::W * 0.5f - (cam.shakeX + cam.x) / std::max(0.01f, cam.zoom),
                        gfx::H * 0.5f - (cam.shakeY + cam.y) / std::max(0.01f, cam.zoom)};
    if (cam.alpha < 0.999f && !capturing_) {
        const int w = gfx::targetWidth(), h = gfx::targetHeight();
        if (!hudRt_.id || hudRt_.texture.width != w || hudRt_.texture.height != h) {
            if (hudRt_.id) UnloadRenderTexture(hudRt_);
            hudRt_ = LoadRenderTexture(w, h);
        }
        gfx::beginTexture(hudRt_);
        ClearBackground(BLANK);
        gfx::beginCanvas(cam.zoom, focus, cam.angle);
        drawHudCentered(true);
        drawLanes();
        fx_.draw(true);
        drawHudCentered();
        gfx::endCanvas();
        gfx::endTexture();
        DrawTextureRec(hudRt_.texture, {0, 0, float(w), -float(h)}, {0, 0}, gfx::fade(WHITE, cam.alpha));
    } else {
        gfx::beginCanvas(cam.zoom, focus, cam.angle);
        drawHudCentered(true);
        drawLanes();
        fx_.draw(true);
        drawHudCentered();
        gfx::endCanvas();
    }
    gfx::beginCanvas();
}

// The health bar and score sit in the middle of the game's width (a script
// may have narrowed it, Flixel's FlxG.width).
void PlayState::drawHudCentered(bool underNotes) {
    const float shift = (gameW_ - gfx::W) * 0.5f;
    auto body = [&] {
        if (underNotes) drawUnderNotes();
        else drawHud();
    };
    if (shift == 0) {
        body();
        return;
    }
    rlDrawRenderBatchActive();
    rlPushMatrix();
    rlTranslatef(shift, 0, 0);
    body();
    rlDrawRenderBatchActive();
    rlPopMatrix();
}

void PlayState::psychDraw() {
    App& a = app();
    scene::Scene& sc = *scene_;
    const double now = a.now;
    gfx::beginCanvas();
    if (backdrop_) {
        gfx::endCanvas();
        gfx::beginCanvas(0.94f, {640, 370});
        stage_.drawBack(now);
        stage_.drawFront(now);
        gfx::endCanvas();
        gfx::beginCanvas();
    }
    auto builtin = [&](const Character& c, Vector2 feet, float scale) {
        CharacterState cs;
        cs.id = CharId(c.builtinId);
        cs.tint = gfx::rgb(entry_->color);
        const double t0 = now - (c.now - c.lastAnimAt);
        if (c.lastAnim.rfind("sing", 0) == 0) {
            int dir = 2;
            if (c.lastAnim.find("LEFT") != std::string::npos) dir = 0;
            else if (c.lastAnim.find("DOWN") != std::string::npos) dir = 1;
            else if (c.lastAnim.find("RIGHT") != std::string::npos) dir = 3;
            if (c.lastAnim.find("miss") != std::string::npos) {
                cs.singDir = dir;
                cs.missUntil = t0 + 0.45;
            } else {
                cs.singDir = dir;
                cs.singStart = t0;
                cs.singUntil = t0 + std::max(0.25, c.holdTimer + 0.15);
            }
        }
        cs.beatAt = now - (c.now - c.lastDanceAt);
        cs.dead = phase_ == Phase::Dead && &c == charForSide(1);
        drawCharacter(cs, feet, scale * 0.94f, now, c.isPlayer);
    };
    bool hudPass = false;
    auto marker = [&](const scene::Sprite& m) {
        if (hudPass && &m == sc.hudMarker.get()) drawHudLayer();
    };
    // One camera; with shaders (ShaderFilter) it is drawn to a texture first.
    auto pass = [&](scene::Cam which, scene::Camera& cam, bool isHud) {
        if (cam.filters.empty() || !cam.visible) {
            hudPass = isHud;
            renderer_.drawCamera(sc, which, marker, builtin);
            hudPass = false;
            renderer_.drawEffects(cam);
            return;
        }
        gfx::endCanvas();
        const float s = gfx::canvasScale();
        const Vector2 origin = gfx::canvasOrigin();
        const int rw = std::max(1, int(gfx::W * s)), rh = std::max(1, int(gfx::H * s));
        if (!camRt_.id || camRt_.texture.width != rw || camRt_.texture.height != rh) {
            if (camRt_.id) UnloadRenderTexture(camRt_);
            camRt_ = LoadRenderTexture(rw, rh);
        }
        gfx::beginTexture(camRt_);
        ClearBackground(BLANK);
        gfx::setRenderTargetSize(rw, rh);
        gfx::beginCanvas();
        capturing_ = true;
        hudPass = isHud;
        renderer_.drawCamera(sc, which, marker, builtin);
        hudPass = false;
        renderer_.drawEffects(cam);
        capturing_ = false;
        gfx::endCanvas();
        gfx::setRenderTargetSize(0, 0);
        gfx::endTexture();
        renderer_.drawFiltered(sc, cam.filters, camRt_, Rectangle{origin.x, origin.y, float(rw), float(rh)});
        gfx::beginCanvas();
    };
    pass(scene::Cam::Game, sc.camGame, false);
    if (minePulse_ > 0) DrawRectangleRec({0, 0, gfx::W, gfx::H}, gfx::fade(Color{200, 0, 30, 255}, minePulse_ * 0.35f));
    pass(scene::Cam::Hud, sc.camHud, true);
    pass(scene::Cam::Other, sc.camOther, false);
    gfx::endCanvas();
    for (const std::string& e : renderer_.errors) debugNotice(e, true, false);
    renderer_.errors.clear();
    drawCutscene();
}

// ------------------------------------------------------------------ sound --

void PlayState::updateSounds(double dt) {
    for (auto& kv : sounds_) {
        ScriptSound& s = kv.second;
        if (s.fading) {
            s.fadeT += dt;
            const double k = s.fadeDur > 0 ? std::min(1.0, s.fadeT / s.fadeDur) : 1.0;
            s.volume = s.fadeFrom + (s.fadeTo - s.fadeFrom) * k;
            SetMusicVolume(s.mus, float(s.volume));
            if (k >= 1.0) s.fading = false;
        }
        if (!s.started || s.paused || phase_ == Phase::Paused) continue;
        UpdateMusicStream(s.mus);
        if (!IsMusicStreamPlaying(s.mus) && !s.finishedSent) {
            s.finishedSent = true;
            s.started = false;
            if (scripts_ && kv.first.rfind("__", 0) != 0) scripts_->call("onSoundFinished", {Value(kv.first)});
        }
    }
}

void PlayState::releaseAudio() {
    if (!IsAudioDeviceReady()) return;
    for (auto& kv : sounds_) UnloadMusicStream(kv.second.mus);
    sounds_.clear();
    for (Sound& s : oneShots_) UnloadSound(s);
    oneShots_.clear();
    if (IsWindowReady())
        for (Texture2D& t : iconTex_)
            if (t.id) { UnloadTexture(t); t = Texture2D{}; }
}

void PlayState::playSound(const std::string& file, double volume, const std::string& tag, bool loop) {
    if (!psych_ || file.empty()) return;
    const bool music = tag == "__music";
    std::string path;
    for (const char* ext : {".ogg", ".mp3", ".wav"}) {
        path = scene_->files.find(std::string(music ? "music/" : "sounds/") + file + ext);
        if (path.empty()) path = scene_->files.find(file + ext);
        if (path.empty() && endsWithI(file, ext)) path = scene_->files.find(file);
        if (!path.empty()) break;
    }
    if (path.empty()) { debugNotice("sound not found: " + file, false, false); return; }
    auto bytes = std::make_shared<std::vector<unsigned char>>();
    {
        std::string data;
        if (!scene::readFile(path, data)) return;
        bytes->assign(data.begin(), data.end());
    }
    const std::string ext = toLower(path.substr(path.find_last_of('.')));
    const float vol = float(clampv(volume, 0.0, 1.0)) * app().settings.sfxVol;
    if (tag.empty()) {
        Wave w = LoadWaveFromMemory(ext.c_str(), bytes->data(), int(bytes->size()));
        if (!w.data) return;
        Sound snd = LoadSoundFromWave(w);
        UnloadWave(w);
        SetSoundVolume(snd, vol);
        PlaySound(snd);
        oneShots_.push_back(snd);
        if (oneShots_.size() > 48) {  // keep memory bounded
            UnloadSound(oneShots_.front());
            oneShots_.erase(oneShots_.begin());
        }
        return;
    }
    Music mus = LoadMusicStreamFromMemory(ext.c_str(), bytes->data(), int(bytes->size()));
    if (!IsMusicValid(mus)) return;
    if (auto it = sounds_.find(tag); it != sounds_.end()) UnloadMusicStream(it->second.mus);
    mus.looping = loop;
    SetMusicVolume(mus, vol);
    PlayMusicStream(mus);
    ScriptSound s;
    s.mus = mus;
    s.bytes = bytes;
    s.volume = vol;
    s.loop = loop;
    s.started = true;
    sounds_[tag] = s;
}

void PlayState::soundCommand(const std::string& tag, std::string_view cmd, double a, double b, double c) {
    auto it = sounds_.find(tag);
    if (it == sounds_.end()) return;
    ScriptSound& s = it->second;
    if (cmd == "stop") { StopMusicStream(s.mus); s.started = false; s.finishedSent = true; }
    else if (cmd == "pause") { PauseMusicStream(s.mus); s.paused = true; }
    else if (cmd == "resume") { ResumeMusicStream(s.mus); s.paused = false; }
    else if (cmd == "volume") { s.volume = clampv(a, 0.0, 1.0); SetMusicVolume(s.mus, float(s.volume)); }
    else if (cmd == "fadeIn") { s.fading = true; s.fadeT = 0; s.fadeDur = a; s.fadeFrom = b; s.fadeTo = c; }
    else if (cmd == "fadeOut") { s.fading = true; s.fadeT = 0; s.fadeDur = a; s.fadeFrom = s.volume; s.fadeTo = b; }
    else if (cmd == "fadeCancel") s.fading = false;
    else if (cmd == "time") {
        const float len = GetMusicTimeLength(s.mus);
        SeekMusicStream(s.mus, float(clampv(a / 1000.0, 0.0, double(std::max(0.0f, len - 0.01f)))));
    } else if (cmd == "pitch") {
        s.pitch = clampv(a, 0.05, 8.0);
        SetMusicPitch(s.mus, float(s.pitch));
    }
}

double PlayState::soundQuery(const std::string& tag, std::string_view what) {
    auto it = sounds_.find(tag);
    if (what == "exists") return it != sounds_.end() ? 1 : 0;
    if (it == sounds_.end()) return 0;
    if (what == "volume") return it->second.volume;
    if (what == "time") return double(GetMusicTimePlayed(it->second.mus)) * 1000.0;
    if (what == "length") return double(GetMusicTimeLength(it->second.mus)) * 1000.0;
    if (what == "pitch") return it->second.pitch;
    return 0;
}


// ----------------------------------------------------------------- videos --

bool PlayState::openVideo(VideoSlot& v) {
    const VideoSource src = findVideo(scene_->files, v.name, app().base);
    if (src.converting) {
        v.waiting = true;
        return true;
    }
    v.waiting = false;
    if (!src.error.empty()) {
        debugNotice(src.error, false, false);
        return false;
    }
    v.player = std::make_unique<VideoPlayer>();
    if (!v.player->open(src.playable, v.loop)) {
        debugNotice("cannot play video " + v.name, false, false);
        v.player.reset();
        return false;
    }
    v.player->setVolume(app().settings.musicVol);
    return true;
}

bool PlayState::startVideo(const std::string& name, bool canSkip) {
    if (!psych_) return false;
    auto slot = std::make_unique<VideoSlot>();
    slot->name = name;
    slot->skippable = canSkip;
    if (!openVideo(*slot)) return false;
    cutscene_ = std::move(slot);
    inCutscene_ = true;
    return true;
}

bool PlayState::makeVideoSprite(const std::string& tag, const std::string& name, float x, float y, const std::string& cam,
                                bool loop) {
    if (!psych_ || tag.empty()) return false;
    scene::Scene& sc = *scene_;
    auto s = std::make_shared<scene::Sprite>();
    s->tag = tag;
    s->x = x;
    s->y = y;
    s->cam = scene::Scene::camFromName(cam);
    if (auto it = sc.objects.find(tag); it != sc.objects.end()) sc.remove(it->second);
    sc.objects[tag] = s;
    sc.addFront(s);
    VideoSlot& v = videoSprites_[tag];
    v = VideoSlot{};
    v.name = name;
    v.tag = tag;
    v.loop = loop;
    return openVideo(v);
}

bool PlayState::playVideoOnTop(const std::string& tag, const std::string& name, bool loop) {
    if (!makeVideoSprite(tag, name, 0, 0, "camOther", loop)) return false;
    videoSprites_[tag].fit = true;
    return true;
}

bool PlayState::makeExtraCharacter(const std::string& tag, const std::string& name, bool isPlayer, float x, float y) {
    if (!psych_ || tag.empty() || name.empty()) return false;
    scene::CharacterPtr c = makeCharacter(name, isPlayer, isPlayer ? 0 : 1);
    if (!c) return false;
    // Psych's Character(x, y, ...) sits exactly there: the JSON position is
    // only applied to the three main characters.
    c->x = x;
    c->y = y;
    c->groupX = c->groupY = 0;
    c->tag = tag;
    scene_->objects[tag] = c;
    renderer_.preload(*scene_);
    return true;
}

bool PlayState::updateVideos(double dt) {
    // Video sprites: (re)attach the frame texture once the video is open.
    for (auto it = videoSprites_.begin(); it != videoSprites_.end();) {
        VideoSlot& v = it->second;
        if (v.waiting && !openVideo(v)) { it = videoSprites_.erase(it); continue; }
        if (v.player) {
            v.player->update(dt);
            const std::string key = "video:" + v.tag;
            scene::Sprite* s = scene_->find(v.tag);
            if (s && (!s->graphic || s->graphic->files.empty() || s->graphic->files[0] != key)) {
                auto g = std::make_shared<scene::Graphic>();
                g->files.push_back(key);
                g->sizes.push_back({v.player->width(), v.player->height()});
                psych::AtlasFrame f;
                f.w = f.fw = float(v.player->width());
                f.h = f.fh = float(v.player->height());
                g->frames.push_back(f);
                s->setGraphic(g);
                if (v.fit) {
                    s->setGraphicSize(gfx::W, gfx::H);
                    s->updateHitbox();
                    s->scrollX = s->scrollY = 0;
                }
            }
            renderer_.setDynamicTexture(key, v.player->texture());
            if (v.player->ended()) {
                renderer_.removeDynamicTexture(key);
                if (scripts_) scripts_->call("onVideoFinished", {Value(v.tag)});
                if (v.tag.rfind("__hx", 0) == 0) {  // an HScript player's finish callback
                    scene::Completion c;
                    c.tag = v.tag;
                    scene_->completions.push_back(c);
                    if (scripts_) scripts_->dispatchCompletions();
                }
                if (s) s->visible = false;
                it = videoSprites_.erase(it);
                continue;
            }
        }
        ++it;
    }
    if (!cutscene_) return false;
    App& a = app();
    VideoSlot& c = *cutscene_;
    bool done = false;
    if (c.waiting) {
        if (!openVideo(c)) done = true;
        if (a.input.back()) done = true;
    } else if (c.player) {
        c.player->update(dt);
        done = c.player->ended() || (c.skippable && (a.input.accept() || a.input.back()));
    } else {
        done = true;
    }
    if (done) {
        cutscene_.reset();
        inCutscene_ = false;
        waitStart_ = a.now;
        if (scripts_) scripts_->call("onVideoFinished", {Value(std::string("cutscene"))});
        startAndEnd();
    }
    return true;
}

void PlayState::drawCutscene() {
    if (!cutscene_) return;
    gfx::beginCanvas();
    DrawRectangleRec({-10, -10, gfx::W + 20, gfx::H + 20}, BLACK);
    if (cutscene_->player && cutscene_->player->texture().id) {
        const Texture2D& t = cutscene_->player->texture();
        const float s = std::min(gfx::W / float(t.width), gfx::H / float(t.height));
        const float w = t.width * s, h = t.height * s;
        DrawTexturePro(t, {0, 0, float(t.width), float(t.height)}, {(gfx::W - w) * 0.5f, (gfx::H - h) * 0.5f, w, h}, {0, 0}, 0, WHITE);
    } else {
        gfx::text(gfx::FontId::Ui, "Preparing video...", gfx::W * 0.5f, gfx::H * 0.5f - 15, 30, gfx::fade(WHITE, 0.8f), gfx::Align::Center);
    }
    if (cutscene_->skippable)
        gfx::text(gfx::FontId::Ui, "Enter: skip", gfx::W - 30.0f, gfx::H - 40.0f, 20, gfx::fade(WHITE, 0.45f), gfx::Align::Right);
    gfx::endCanvas();
}

// ------------------------------------------------------------- script API --

bool PlayState::getGameProp(std::string_view path, Value& out) {
    App& a = app();
    const Settings& s = a.settings;
    auto num = [&](double v) { out = v; return true; };
    auto flag = [&](bool v) { out = v; return true; };
    if (path == "health") return num(session_.health);
    if (path == "songScore") return num(double(session_.score));
    if (path == "songMisses") return num(session_.misses);
    if (path == "songHits") return num(session_.judged());
    if (path == "combo") return num(session_.combo);
    if (path == "ratingPercent") return num(session_.accuracy());
    if (path == "ratingName") { out = ratingNameOverride_.empty() ? session_.rank() : ratingNameOverride_; return true; }
    if (path == "ratingFC") { out = ratingFcOverride_.empty() ? session_.clearLabel() : ratingFcOverride_; return true; }
    if (path == "songSpeed") return num(scrollSpeed());
    if (path == "playbackRate") return num(rate_);
    if (path == "opponentChart" || path == "opponentMode") return flag(playerLeft_);
    if (path == "cpuControlled" || path == "botPlay") return flag(session_.botplay());
    if (path == "practiceMode") return flag(cfg_.practice);
    if (path == "inCutscene") return flag(inCutscene_);
    if (path == "startedCountdown") return flag(countdownStarted_);
    if (path == "startingSong") return flag(songMs_ < 0);
    if (path == "endingSong") return flag(phase_ == Phase::Ending);
    if (path == "generatedMusic") return flag(true);
    if (path == "songLength") return num(chart_.lengthMs);
    if (path == "curBeat") return num(lastBeat_);
    if (path == "curStep") return num(lastStep_);
    if (path == "curSection") return num(std::max(0, lastSection_));
    if (path == "isStoryMode") return flag(req_.story);
    if (path == "notes.length") return num(noteCount("notes"));
    if (path == "unspawnNotes.length") return num(noteCount("unspawnNotes"));
    if (path == "eventNotes.length") return num(noteCount("eventNotes"));
    if (path == "instakillOnMiss") return flag(false);
    if (path == "healthGain" || path == "healthLoss") return num(1);
    if (path == "showRating" || path == "showCombo" || path == "showComboNum") return flag(true);
    if (path == "skipCountdown") return flag(false);
    if (path == "guitarHeroSustains") return flag(false);
    if (path == "spawnTime") return num(2000);
    if (path.rfind("class:", 0) != 0) return false;
    const std::string_view c = path.substr(6);
    // Flixel/OpenFL/Lime values a script can read and change (the window is
    // never really resized or moved: FlxG.width narrows the picture instead).
    if (c == "FlxG.width" || c == "Lib.current.stage.stageWidth") return num(gameW_);
    if (c == "FlxG.height" || c == "Lib.current.stage.stageHeight") return num(gfx::H);
    if (auto it = classVars_.find(c); it != classVars_.end()) { out = it->second; return true; }
    for (std::string_view prefix : {"Lib.application.window.", "Application.current.window.", "Lib.current.window."}) {
        if (c.rfind(prefix, 0) != 0) continue;
        const std::string_view w = c.substr(prefix.size());
        const Vector2 pos = platform::kAndroid ? Vector2{0, 0} : GetWindowPosition();
        if (w == "width") return num(gameW_);
        if (w == "height") return num(gfx::H);
        if (w == "x") return num(pos.x);
        if (w == "y") return num(pos.y);
        if (w == "title") { out = std::string("God's Hand"); return true; }
        if (w == "fullscreen") return flag(IsWindowFullscreen());
        if (w == "resizable") return flag(!platform::kAndroid);
        if (w == "borderless" || w == "maximized" || w == "minimized") return flag(false);
        if (w == "frameRate") return num(s.fpsCap > 0 ? s.fpsCap : 240);
        return false;
    }
    for (const auto& [prefix, mode] : {std::pair<std::string_view, int>{"FlxG.keys.justPressed.", 1},
                                       {"FlxG.keys.pressed.", 0}, {"FlxG.keys.justReleased.", 2}}) {
        if (c.rfind(prefix, 0) == 0) return flag(keyboardState(c.substr(prefix.size()), mode));
    }
    if (c == "FlxG.sound.music.pitch") return num(rate_);
    if (c == "FlxG.sound.music.length") return num(prep_.inst ? a.mixer.lengthSec() * 1000.0 : chart_.lengthMs);
    if (c == "FlxG.sound.music.playing") return flag(phase_ == Phase::Playing && songMs_ >= 0);
    if (c == "FlxG.mouse.visible") return flag(false);
    if (c == "FlxG.timeScale" || c == "FlxG.game.timeScale") return num(1);
    if (c == "FlxG.fullscreen") return flag(IsWindowFullscreen());
    if (c == "FlxG.drawFramerate" || c == "FlxG.updateFramerate") return num(s.fpsCap > 0 ? s.fpsCap : 240);
    if (c == "FlxG.game.ticks") return num(a.now * 1000.0);
    if (c == "FlxG.autoPause") return flag(true);
    if (c == "Conductor.safeZoneOffset") return num(kMissWindowMs);
    if (c == "Conductor.offset") return num(0);
    if (c == "PlayState.deathCounter" || c == "PlayState.storyWeek" || c == "PlayState.campaignScore" ||
        c == "PlayState.campaignMisses")
        return num(0);
    if (c == "PlayState.isPixelStage") return flag(scene_ && scene_->stage.pixel);
    if (c == "PlayState.seenCutscene") return flag(true);
    if (c == "PlayState.SONG.player1") { out = chart_.player; return true; }
    if (c == "PlayState.SONG.player2") { out = chart_.opponent; return true; }
    if (c == "PlayState.SONG.gfVersion" || c == "PlayState.SONG.player3") { out = chart_.gf; return true; }
    if (c == "PlayState.SONG.stage") { out = stageName_; return true; }
    if (c == "PlayState.SONG.needsVoices") return flag(prep_.voiceP != nullptr || prep_.voiceO != nullptr);
    if (c == "MainMenuState.psychEngineVersion") { out = std::string("0.7.3"); return true; }
    if (c == "GameOverSubstate.characterName") { out = std::string("bf-dead"); return true; }
    if (c == "GameOverSubstate.deathSoundName") { out = std::string("fnf_loss_sfx"); return true; }
    if (c == "GameOverSubstate.loopSoundName") { out = std::string("gameOver"); return true; }
    if (c == "GameOverSubstate.endSoundName") { out = std::string("gameOverEnd"); return true; }
    if (c == "ClientPrefs.noteSplashes") return flag(s.noteSplashes);
    if (c == "ClientPrefs.enableColorShader" || c == "ClientPrefs.charsAndBG" || c == "ClientPrefs.showNotes") return flag(true);
    if (c == "ClientPrefs.oldSusStyle" || c == "ClientPrefs.ffmpegMode" || c == "ClientPrefs.showcaseMode" ||
        c == "ClientPrefs.lessBotLag" || c == "ClientPrefs.ezSpam")
        return flag(false);
    if (c == "ClientPrefs.ratingOffset" || c.rfind("ClientPrefs.comboOffset[", 0) == 0) return num(0);
    if (c == "ClientPrefs.sickWindow") return num(45);
    if (c == "ClientPrefs.goodWindow") return num(90);
    if (c == "ClientPrefs.badWindow") return num(135);
    if (c == "ClientPrefs.safeFrames") return num(10);
    if (c == "ClientPrefs.iconBounceType") { out = std::string("Psych"); return true; }
    if (c == "ClientPrefs.downScroll") return flag(s.downscroll);
    if (c == "ClientPrefs.middleScroll") return flag(s.middlescroll);
    if (c == "ClientPrefs.ghostTapping") return flag(s.ghostTapping);
    if (c == "ClientPrefs.flashing") return flag(s.flashing);
    if (c == "ClientPrefs.camZooms") return flag(s.camZoom);
    if (c == "ClientPrefs.lowQuality") return flag(s.lowDetail);
    if (c == "ClientPrefs.framerate") return num(s.fpsCap > 0 ? s.fpsCap : 240);
    if (c == "ClientPrefs.antialiasing" || c == "ClientPrefs.globalAntialiasing") return flag(true);
    if (c == "ClientPrefs.noteOffset") return num(s.offsetMs);
    if (c == "ClientPrefs.hideHud") return flag(false);
    if (c == "ClientPrefs.shaders") return flag(s.shaders);
    if (c == "ClientPrefs.healthBarAlpha") return num(1);
    if (c == "ClientPrefs.timeBarType") { out = std::string("Time Left"); return true; }
    if (c == "Conductor.songPosition") return num(songMs_);
    if (c == "Conductor.bpm") return num(chart_.bpmAt(std::max(0.0, songMs_)));
    if (c == "Conductor.crochet") return num(60000.0 / chart_.bpmAt(std::max(0.0, songMs_)));
    if (c == "Conductor.stepCrochet") return num(15000.0 / chart_.bpmAt(std::max(0.0, songMs_)));
    if (c == "PlayState.SONG.song") { out = chart_.title; return true; }
    if (c == "PlayState.SONG.speed") return num(chart_.speed);
    if (c == "PlayState.SONG.bpm") return num(chart_.bpmAt(0));
    if (c == "PlayState.isStoryMode") return flag(req_.story);
    if (c == "PlayState.storyDifficulty") return num(1);
    if (c == "PlayState.chartingMode") return flag(false);
    if (c == "FlxG.width") return num(gfx::W);
    if (c == "FlxG.height") return num(gfx::H);
    if (c == "FlxG.sound.music.time") return num(songMs_);
    if (c == "FlxG.sound.music.volume") return num(1);
    if (c == "FlxG.mouse.x") return num(mouseX());
    if (c == "FlxG.mouse.y") return num(mouseY());
    if (c == "FlxG.elapsed") return num(a.dt);
    return false;
}

bool PlayState::setGameProp(std::string_view path, const Value& v) {
    const double d = scene::toNum(v);
    if (path == "health") { session_.setHealth(clampv(d, 0.0, 2.0)); return true; }
    if (path == "songScore") { session_.score = (long long)d; return true; }
    if (path == "songMisses") { session_.misses = (long long)d; return true; }
    if (path == "combo") { session_.combo = (long long)d; return true; }
    if (path == "songHits") return true;
    if (path == "ratingName") { ratingNameOverride_ = scene::toStr(v); return true; }
    if (path == "ratingFC") { ratingFcOverride_ = scene::toStr(v); return true; }
    if (path == "ratingPercent") return true;
    if (path == "songSpeed") {
        const Settings& s = app().settings;
        const double base = s.scrollSpeed > 0.001f ? s.scrollSpeed : chart_.speed;
        if (d > 0 && base > 0) speedMult_ = d / base;
        return true;
    }
    if (path == "cpuControlled" || path == "botPlay") { session_.setBotplay(scene::toBool(v)); cfg_.botplay = scene::toBool(v); return true; }
    if (path == "practiceMode") { session_.setPractice(scene::toBool(v)); cfg_.practice = scene::toBool(v); return true; }
    if (path == "inCutscene") { inCutscene_ = scene::toBool(v); return true; }
    if (path.rfind("class:", 0) != 0) return false;
    const std::string_view c = path.substr(6);
    if (c == "FlxG.sound.music.volume") { app().mixer.setTrackVolume(Mixer::kInst, float(d)); return true; }
    if (c == "FlxG.width" || c == "Lib.current.stage.stageWidth" || c == "Lib.application.window.width" ||
        c == "Application.current.window.width") {
        // Only the game's width matters here; the window keeps its size.
        if (c == "FlxG.width") setGameWidth(float(d));
        return true;
    }
    if (c == "PlayState.SONG.speed") {
        const Settings& s = app().settings;
        if (s.scrollSpeed <= 0.001f && d > 0 && chart_.speed > 0) speedMult_ = d / chart_.speed;
        return true;
    }
    // Live values (song position, music, input) are not overwritten by a stored copy.
    for (std::string_view live : {"Conductor.", "FlxG.sound.", "FlxG.mouse.", "FlxG.keys.", "FlxG.elapsed", "FlxG.game."})
        if (c.rfind(live, 0) == 0) return true;
    classVars_[std::string(c)] = v;  // window position, title, GameOverSubstate names...: kept for reading back
    return true;
}

void PlayState::setGameWidth(float w) {
    if (!(w > 0)) return;
    gameW_ = clampv(w, 320.0f, gfx::W);
    if (countdownStarted_ || !scene_) return;
    // Psych lays the strums out when the countdown starts, from FlxG.width.
    layoutStrums();
    if (!scripts_) return;
    for (int i = 0; i < chart_.keys && i < 9; ++i) {
        const scene::Strum& p = strum(1, i);
        const scene::Strum& o = strum(0, i);
        scripts_->setGlobal(TextFormat("defaultPlayerStrumX%d", i), double(p.x));
        scripts_->setGlobal(TextFormat("defaultPlayerStrumY%d", i), double(p.y));
        scripts_->setGlobal(TextFormat("defaultOpponentStrumX%d", i), double(o.x));
        scripts_->setGlobal(TextFormat("defaultOpponentStrumY%d", i), double(o.y));
    }
}

void PlayState::triggerEvent(const std::string& name, const std::string& v1, const std::string& v2) {
    psych::ChartEvent e;
    e.timeMs = songMs_;
    e.name = name;
    e.v1 = v1;
    e.v2 = v2;
    handleBuiltinEvent(e);
    if (scripts_) scripts_->call("onEvent", {Value(name), Value(v1), Value(v2), Value(songMs_)});
}

// Options > DEBUG: each kind is shown always (0), only for scripts that set
// luaDebugMode as in Psych (1), or never (2).
void PlayState::debugNotice(const std::string& text, bool error, bool debugMode) {
    const Settings& s = app().settings;
    const int mode = error ? s.luaErrors : s.luaWarnings;
    if (mode == 2 || (mode == 1 && !debugMode)) {
        if (app().autoRun.enabled) std::printf("[lua, hidden] %s\n", text.c_str());
        return;
    }
    debugPrint(text, error ? 0xFFFF4050 : 0xFFFFD040);
}

void PlayState::debugPrint(const std::string& text, uint32_t argb) {
    const Color c{(unsigned char)(argb >> 16), (unsigned char)(argb >> 8), (unsigned char)argb, 255};
    debug_.push_back({text, c, app().now});
    while (debug_.size() > 12) debug_.pop_front();
    if (app().autoRun.enabled) std::printf("[lua] %s\n", text.c_str());
}

void PlayState::endSong() {
    if (phase_ == Phase::Ending) endRequested_ = true;
    finish();
}

void PlayState::restartSong() {
    App& a = app();
    if (psych_) {
        a.mixer.stop();
        a.change(makePlay(req_));  // fresh scene and scripts, like Psych's resetState
        return;
    }
    restart();
}

void PlayState::exitSong() { exitToMenu(); }

void PlayState::startCountdown() {
    if (phase_ == Phase::Waiting) beginCountdown();
}

void PlayState::startAndEnd() {
    if (phase_ == Phase::Ending) {
        endRequested_ = true;
        finish();
    } else {
        startCountdown();
    }
}

bool PlayState::keyState(std::string_view action, int mode) {
    App& a = app();
    auto laneState = [&](int lane) {
        if (lane < 0 || lane >= 9) return false;
        if (mode == 0) return keyNow_[lane];
        if (mode == 1) return keyNow_[lane] && !prevKeys_[lane];
        return !keyNow_[lane] && prevKeys_[lane];
    };
    if (action == "left" || action == "note_left") return laneState(0);
    if (action == "down" || action == "note_down") return laneState(1);
    if (action == "up" || action == "note_up") return laneState(2);
    if (action == "right" || action == "note_right") return laneState(3);
    std::vector<int> codes;
    if (action == "accept") codes = {KEY_ENTER, KEY_SPACE};
    else if (action == "back") codes = {KEY_ESCAPE, KEY_BACKSPACE};
    else if (action == "pause") codes = {KEY_ENTER, KEY_ESCAPE};
    else if (action == "reset") codes = {KEY_R};
    else if (action == "space") codes = {KEY_SPACE};
    for (int c : codes)
        if (keyboardState(TextFormat("%d", c), mode) || (mode == 0 ? a.input.isDown(c) : mode == 1 ? a.input.pressed(c) : false))
            return true;
    return false;
}

bool PlayState::keyboardState(std::string_view key, int mode) {
    App& a = app();
    int code = 0;
    if (!key.empty() && std::isdigit((unsigned char)key[0]) && key.size() > 1) code = std::atoi(std::string(key).c_str());
    else code = keyCodeFromName(key);
    if (code <= 0) return false;
    if (mode == 0) return a.input.isDown(code) || (code == KEY_LEFT_SHIFT && a.input.isDown(KEY_RIGHT_SHIFT));
    if (mode == 1) return a.input.pressed(code);
    for (const InputEvent& e : a.input.events())
        if (e.code == code && !e.down) return true;
    return false;
}

bool PlayState::mouseState(std::string_view button, int mode) {
    const int b = button == "right" ? 1 : button == "middle" ? 2 : 0;
    if (mode == 0) return mouseNow_[b];
    if (mode == 1) return mouseNow_[b] && !prevMouse_[b];
    return !mouseNow_[b] && prevMouse_[b];
}

float PlayState::mouseX() { return gfx::toCanvas(GetMousePosition()).x; }

float PlayState::mouseY() { return gfx::toCanvas(GetMousePosition()).y; }

LaneNote* PlayState::noteRef(std::string_view group, int index, int* sideOut, int* laneOut) {
    if (index < 0) return nullptr;
    const double spawn = 2000.0 * rate_ / std::max(0.1, std::min(1.0, scrollSpeed()));
    const auto& notes = chart_.notes;
    size_t k = size_t(std::upper_bound(notes.begin(), notes.end(), songMs_ + spawn,
                                       [](double t, const Note& n) { return t < n.time; }) - notes.begin());
    size_t idx;
    if (group == "unspawnNotes") {
        idx = k + size_t(index);
    } else {
        const size_t start = size_t(std::lower_bound(notes.begin(), notes.end(), songMs_ - 350.0,
                                                     [](const Note& n, double t) { return n.time < t; }) - notes.begin());
        idx = start + size_t(index);
        if (idx >= k) return nullptr;
    }
    if (idx >= noteOrder_.size()) return nullptr;
    const NoteRef& r = noteOrder_[idx];
    auto& lane = session_.laneMut(r.side, r.lane);
    if (r.idx >= lane.size()) return nullptr;
    if (sideOut) *sideOut = r.side;
    if (laneOut) *laneOut = r.lane;
    return &lane[r.idx];
}

// Psych's eventNotes: the chart events that have not fired yet.
static bool isEventGroup(std::string_view g) { return equalsI(g, "eventNotes"); }

int PlayState::noteCount(std::string_view group) {
    if (isEventGroup(group)) return int(chart_.events.size() - std::min(eventIdx_, chart_.events.size()));
    const double spawn = 2000.0 * rate_ / std::max(0.1, std::min(1.0, scrollSpeed()));
    const auto& notes = chart_.notes;
    const size_t k = size_t(std::upper_bound(notes.begin(), notes.end(), songMs_ + spawn,
                                             [](double t, const Note& n) { return t < n.time; }) - notes.begin());
    if (group == "unspawnNotes") return int(notes.size() - k);
    const size_t start = size_t(std::lower_bound(notes.begin(), notes.end(), songMs_ - 350.0,
                                                 [](const Note& n, double t) { return n.time < t; }) - notes.begin());
    return int(k > start ? k - start : 0);
}

bool PlayState::noteGet(std::string_view group, int index, std::string_view f, Value& out) {
    if (isEventGroup(group)) {
        const size_t i = eventIdx_ + size_t(std::max(0, index));
        if (index < 0 || i >= chart_.events.size()) return false;
        const psych::ChartEvent& e = chart_.events[i];
        if (f == "strumTime") { out = e.timeMs; return true; }
        if (f == "event") { out = e.name; return true; }
        if (f == "value1") { out = e.v1; return true; }
        if (f == "value2") { out = e.v2; return true; }
        if (f == "value3") { out = std::string(); return true; }
        return false;
    }
    int side = 0, lane = 0;
    LaneNote* n = noteRef(group, index, &side, &lane);
    if (!n) return false;
    const NoteVis* vis = nullptr;
    if (auto it = noteVis_.find(n->id); it != noteVis_.end()) vis = &it->second;
    if (f == "strumTime") { out = n->t; return true; }
    if (f == "noteData") { out = double(lane); return true; }
    if (f == "mustPress") { out = side == 1; return true; }
    if (f == "noteType") { out = chart_.noteTypes[n->type < chart_.noteTypes.size() ? n->type : 0]; return true; }
    if (f == "isSustainNote") { out = false; return true; }
    if (f == "sustainLength") { out = n->len; return true; }
    if (f == "ignoreNote") { out = (n->flags & kFlagIgnore) != 0; return true; }
    if (f == "hitCausesMiss") { out = n->kind == kMine; return true; }
    if (f == "noAnimation") { out = (n->flags & kFlagNoAnim) != 0; return true; }
    if (f == "noMissAnimation") { out = (n->flags & kFlagNoMissAnim) != 0; return true; }
    if (f == "gfNote") { out = (n->flags & kFlagGF) != 0; return true; }
    if (f == "animSuffix") { out = std::string((n->flags & kFlagAlt) ? "-alt" : ""); return true; }
    if (f == "wasGoodHit") { out = n->state == kHit || n->state == kHolding || n->state == kHeldDone; return true; }
    if (f == "tooLate") { out = n->state == kMissed; return true; }
    if (f == "hitByOpponent") { out = side == 0 && n->state != kPending; return true; }
    if (f == "canBeHit") { out = std::fabs(n->t - songMs_) <= kMissWindowMs * rate_; return true; }
    if (f == "alpha" || f == "multAlpha") { out = double(vis ? vis->alpha : 1.0f); return true; }
    if (f == "visible") { out = vis ? vis->visible : true; return true; }
    if (f == "offsetX") { out = double(vis ? vis->offsetX : 0.0f); return true; }
    if (f == "offsetY") { out = double(vis ? vis->offsetY : 0.0f); return true; }
    if (f == "multSpeed") { out = double(vis ? vis->multSpeed : 1.0f); return true; }
    if (f == "x") { out = double(strum(side, lane).x + (vis ? vis->offsetX : 0.0f)); return true; }
    if (f == "texture") { out = std::string(); return true; }
    if (f == "copyAlpha" || f == "copyX" || f == "copyY" || f == "copyAngle") { out = true; return true; }
    if (f == "lowPriority") { out = false; return true; }
    if (f == "hitHealth") { out = 0.023; return true; }
    if (f == "missHealth") { out = 0.0475; return true; }
    return false;
}

bool PlayState::noteSet(std::string_view group, int index, std::string_view f, const Value& v) {
    if (isEventGroup(group)) {
        const size_t i = eventIdx_ + size_t(std::max(0, index));
        if (index < 0 || i >= chart_.events.size()) return false;
        psych::ChartEvent& e = chart_.events[i];
        if (f == "strumTime") e.timeMs = scene::toNum(v, e.timeMs);  // order is kept: Psych does not re-sort either
        else if (f == "event") e.name = scene::toStr(v);
        else if (f == "value1") e.v1 = scene::toStr(v);
        else if (f == "value2") e.v2 = scene::toStr(v);
        return true;
    }
    LaneNote* n = noteRef(group, index);
    if (!n) return false;
    auto bit = [&](uint8_t b) {
        if (scene::toBool(v)) n->flags |= b;
        else n->flags &= uint8_t(~b);
        return true;
    };
    if (f == "ignoreNote") return bit(kFlagIgnore);
    if (f == "noAnimation") return bit(kFlagNoAnim);
    if (f == "noMissAnimation") return bit(kFlagNoMissAnim);
    if (f == "gfNote") return bit(kFlagGF);
    if (f == "animSuffix") {
        if (scene::toStr(v) == "-alt") n->flags |= kFlagAlt;
        else n->flags &= uint8_t(~kFlagAlt);
        return true;
    }
    if (f == "hitCausesMiss") { n->kind = scene::toBool(v) ? kMine : kNormal; return true; }
    if (f == "noteType") { n->type = chart_.typeIndex(scene::toStr(v)); return true; }
    if (f == "alpha" || f == "multAlpha") { noteVis_[n->id].alpha = float(clampv(scene::toNum(v, 1), 0.0, 1.0)); return true; }
    if (f == "visible") { noteVis_[n->id].visible = scene::toBool(v, true); return true; }
    if (f == "offsetX") { noteVis_[n->id].offsetX = float(scene::toNum(v)); return true; }
    if (f == "offsetY") { noteVis_[n->id].offsetY = float(scene::toNum(v)); return true; }
    if (f == "multSpeed") { noteVis_[n->id].multSpeed = float(std::max(0.05, scene::toNum(v, 1))); return true; }
    return true;  // texture, copyX and friends: accepted, no visual change
}

void PlayState::healthBarColors(uint32_t left, uint32_t right) {
    customBarColors_ = true;
    barColor_[0] = left & 0xFFFFFF;
    barColor_[1] = right & 0xFFFFFF;
    leftColor_ = Color{(unsigned char)(left >> 16), (unsigned char)(left >> 8), (unsigned char)left, 255};
    rightColor_ = Color{(unsigned char)(right >> 16), (unsigned char)(right >> 8), (unsigned char)right, 255};
}

void PlayState::timeBarColors(uint32_t left, uint32_t right) {
    timeBarFill_ = Color{(unsigned char)(left >> 16), (unsigned char)(left >> 8), (unsigned char)left, 255};
    timeBarBack_ = Color{(unsigned char)(right >> 16), (unsigned char)(right >> 8), (unsigned char)right, 255};
}

std::string PlayState::saveDir() { return app().userDir + "saves/"; }

bool PlayState::shadersEnabled() { return app().settings.shaders; }

// Only the first gamepad is read (the one the game itself uses).
static int padButton(std::string_view name) {
    std::string n;
    for (char c : name)
        if (c != '_' && c != ' ') n += char(std::toupper((unsigned char)c));
    if (n == "A") return GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
    if (n == "B") return GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
    if (n == "X") return GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
    if (n == "Y") return GAMEPAD_BUTTON_RIGHT_FACE_UP;
    if (n == "LEFTSHOULDER" || n == "LB") return GAMEPAD_BUTTON_LEFT_TRIGGER_1;
    if (n == "RIGHTSHOULDER" || n == "RB") return GAMEPAD_BUTTON_RIGHT_TRIGGER_1;
    if (n == "LEFTTRIGGER" || n == "LT") return GAMEPAD_BUTTON_LEFT_TRIGGER_2;
    if (n == "RIGHTTRIGGER" || n == "RT") return GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
    if (n == "BACK" || n == "SELECT") return GAMEPAD_BUTTON_MIDDLE_LEFT;
    if (n == "START") return GAMEPAD_BUTTON_MIDDLE_RIGHT;
    if (n == "GUIDE") return GAMEPAD_BUTTON_MIDDLE;
    if (n == "LEFTSTICKCLICK") return GAMEPAD_BUTTON_LEFT_THUMB;
    if (n == "RIGHTSTICKCLICK") return GAMEPAD_BUTTON_RIGHT_THUMB;
    if (n == "DPADUP") return GAMEPAD_BUTTON_LEFT_FACE_UP;
    if (n == "DPADDOWN") return GAMEPAD_BUTTON_LEFT_FACE_DOWN;
    if (n == "DPADLEFT") return GAMEPAD_BUTTON_LEFT_FACE_LEFT;
    if (n == "DPADRIGHT") return GAMEPAD_BUTTON_LEFT_FACE_RIGHT;
    return -1;
}

bool PlayState::gamepadState(int pad, std::string_view button, int mode) {
    const int b = padButton(button);
    if (pad > 0 || b < 0) return false;
    const int code = kPadBase + b;
    Input& in = app().input;
    if (mode == 0) return in.isDown(code);
    if (mode == 1) return in.pressed(code);
    for (const InputEvent& e : in.events())
        if (e.code == code && !e.down) return true;
    return false;
}

double PlayState::gamepadAxis(int pad, int axis) {
    if (pad > 0 || !IsGamepadAvailable(0)) return 0;
    static const int axes[4] = {GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y};
    return GetGamepadAxisMovement(0, axes[axis & 3]);
}

std::string PlayState::modsDir() { return app().base + "mods/"; }

// JS Engine loopTheSong: back to `ms`, the notes from there on can be played again.
void PlayState::loopSong(double ms) {
    App& a = app();
    ms = clampv(ms, 0.0, std::max(0.0, chart_.lengthMs));
    a.mixer.play(ms / 1000.0);
    songMs_ = ms;
    session_.rewindTo(ms);
    eventIdx_ = size_t(std::lower_bound(chart_.events.begin(), chart_.events.end(), ms,
                                        [](const psych::ChartEvent& e, double t) { return e.timeMs < t; }) -
                       chart_.events.begin());
    lastBeat_ = int(std::floor(chart_.beatAt(ms))) - 1;
    lastStep_ = int(std::floor(chart_.beatAt(ms) * 4.0)) - 1;
    if (!chart_.sections.empty()) lastSection_ = std::max(0, chart_.sectionAt(ms) - 1);
    if (phase_ == Phase::Ending) phase_ = Phase::Playing;
    finished_ = endRequested_ = false;
}

void PlayState::precacheCharacter(const std::string& name, int who) {
    if (!psych_ || name.empty()) return;
    who = clampv(who, 0, 2);
    const std::string key = std::to_string(who) + ":" + name;
    if (!charCache_.count(key)) charCache_[key] = makeCharacter(name, who == 0, who);
    renderer_.preload(*scene_);
}

void PlayState::openCustomSubstate(const std::string& name, bool pauseGame) {
    if (substate_.active) closeCustomSubstate();
    substate_ = Substate{};
    substate_.name = name;
    substate_.active = true;
    substate_.pausesGame = pauseGame;
    if (pauseGame) {
        app().mixer.pause();
        for (auto& kv : sounds_)
            if (kv.second.started && !kv.second.paused) PauseMusicStream(kv.second.mus);
    }
    if (scripts_) {
        scripts_->call("onCustomSubstateCreate", {Value(name)});
        scripts_->call("onCustomSubstateCreatePost", {Value(name)});
    }
}

void PlayState::closeCustomSubstate() {
    if (!substate_.active) return;
    const Substate st = substate_;
    substate_.active = false;
    for (const std::string& tag : st.members)
        if (auto it = scene_->objects.find(tag); it != scene_->objects.end()) scene_->remove(it->second);
    if (st.pausesGame && phase_ == Phase::Playing) {
        app().mixer.resume();
        for (auto& kv : sounds_)
            if (kv.second.started && !kv.second.paused) ResumeMusicStream(kv.second.mus);
    }
    if (scripts_) scripts_->call("onCustomSubstateDestroy", {Value(st.name)});
}

void PlayState::insertToCustomSubstate(const std::string& tag) {
    if (!substate_.active) return;
    auto it = scene_->objects.find(tag);
    if (it == scene_->objects.end()) return;
    it->second->cam = scene::Cam::Other;
    scene_->addFront(it->second);
    substate_.members.push_back(tag);
}

bool PlayState::loadSong(const std::string& name, int difficulty) {
    // Another song of the same mod (Psych's loadSong).
    App& a = app();
    const std::string want = songPathName(name.empty() ? entry_->songKey : name);
    for (const SongEntry& e : a.library.songs()) {
        if (e.builtin || songPathName(e.songKey) != want || e.modRoot != entry_->modRoot) continue;
        PlayRequest r = req_;
        r.songId = e.id;
        if (difficulty >= 0 && difficulty < int(e.diffs.size())) r.diff = e.diffs[size_t(difficulty)];
        else if (std::find(e.diffs.begin(), e.diffs.end(), r.diff) == e.diffs.end() && !e.diffs.empty()) r.diff = e.diffs[0];
        a.mixer.stop();
        a.change(makePlay(r));
        return true;
    }
    debugNotice("loadSong: no song " + name + " in this mod", false, false);
    return false;
}

}  // namespace gh
