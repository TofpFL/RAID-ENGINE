#include "lua_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

#include "common.hpp"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace gh {

using scene::Character;
using scene::Sprite;
using scene::SpritePtr;
using scene::Value;

struct ScriptHost::Script {
    ScriptHost* host = nullptr;
    lua_State* L = nullptr;
    std::string path, name;
    int index = 0;
    bool closed = false;
    double callStart = 0, budget = 2.0;
    std::string lastErr;
    double lastErrAt = -10;
    std::unique_ptr<hx::Interp> hx;  // runHaxeCode
    ~Script() {
        hx.reset();
        if (L) lua_close(L);
    }
};

namespace {

constexpr const char* kCompat = R"LUA(
unpack = unpack or table.unpack
loadstring = loadstring or load
math.pow = math.pow or function(a, b) return a ^ b end
math.mod = math.mod or math.fmod
math.ldexp = math.ldexp or function(m, e) return m * 2.0 ^ e end
table.getn = table.getn or function(t) return #t end
local function int(x) return math.tointeger(x) or math.floor(x) end
bit = bit or {
  band = function(a, b) return int(a) & int(b) end,
  bor = function(a, b) return int(a) | int(b) end,
  bxor = function(a, b) return int(a) ~ int(b) end,
  bnot = function(a) return ~int(a) end,
  lshift = function(a, n) return int(a) << int(n) end,
  rshift = function(a, n) return (int(a) & 0xFFFFFFFF) >> int(n) end,
  tobit = function(a) a = int(a) & 0xFFFFFFFF; if a >= 0x80000000 then a = a - 0x100000000 end return a end,
}
)LUA";

std::mt19937& rng() {
    static std::mt19937 r(std::random_device{}());
    return r;
}

ScriptHost::Script& S(lua_State* L) { return **static_cast<ScriptHost::Script**>(lua_getextraspace(L)); }
ScriptHost& H(lua_State* L) { return *S(L).host; }
scene::Scene& SC(lua_State* L) { return H(L).scene; }
ScriptGame& G(lua_State* L) { return H(L).game; }

Value toValue(lua_State* L, int i) {
    switch (lua_type(L, i)) {
        case LUA_TBOOLEAN: return bool(lua_toboolean(L, i));
        case LUA_TNUMBER: return double(lua_tonumber(L, i));
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, i, &n);
            return std::string(s, n);
        }
        default: return std::monostate{};
    }
}

void push(lua_State* L, const Value& v) {
    if (auto d = std::get_if<double>(&v)) {
        // Integral numbers as Lua integers so "beat " .. curBeat prints "4", not "4.0".
        if (std::isfinite(*d) && *d == std::floor(*d) && std::fabs(*d) < 9.0e15) lua_pushinteger(L, lua_Integer(*d));
        else lua_pushnumber(L, *d);
    } else if (auto b = std::get_if<bool>(&v)) {
        lua_pushboolean(L, *b);
    } else if (auto s = std::get_if<std::string>(&v)) {
        lua_pushlstring(L, s->data(), s->size());
    } else {
        lua_pushnil(L);
    }
}

std::string str(lua_State* L, int i, const char* def = "") {
    const int t = lua_type(L, i);
    if (t == LUA_TSTRING) {
        size_t n = 0;
        const char* s = lua_tolstring(L, i, &n);
        return std::string(s, n);
    }
    if (t == LUA_TNUMBER) return scene::toStr(Value(double(lua_tonumber(L, i))));
    if (t == LUA_TBOOLEAN) return lua_toboolean(L, i) ? "true" : "false";
    return def;
}

double num(lua_State* L, int i, double def = 0) {
    const int t = lua_type(L, i);
    if (t == LUA_TNUMBER) return lua_tonumber(L, i);
    if (t == LUA_TSTRING || t == LUA_TBOOLEAN) return scene::toNum(toValue(L, i), def);
    return def;
}

bool boolean(lua_State* L, int i, bool def = false) {
    if (lua_isnoneornil(L, i)) return def;
    return scene::toBool(toValue(L, i), def);
}

int ret(lua_State* L, const Value& v) {
    push(L, v);
    return 1;
}
int retNum(lua_State* L, double d) { return ret(L, Value(d)); }
int retBool(lua_State* L, bool b) {
    lua_pushboolean(L, b);
    return 1;
}

void warn(lua_State* L, const std::string& msg) {
    G(L).debugPrint(S(L).name + ": " + msg, 0xFFFFD040);
}

std::string cleanTag(std::string tag) {
    tag.erase(std::remove(tag.begin(), tag.end(), '.'), tag.end());
    return tag;
}

Sprite* obj(lua_State* L, const std::string& name) {
    if (Sprite* s = SC(L).find(name)) return s;
    // "boyfriend.someChild" style paths are not objects here.
    return nullptr;
}

std::vector<int> intListArg(lua_State* L, int i) {
    std::vector<int> out;
    if (lua_istable(L, i)) {
        const lua_Integer n = luaL_len(L, i);
        for (lua_Integer k = 1; k <= n; ++k) {
            lua_geti(L, i, k);
            out.push_back(int(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    } else {
        const std::string s = str(L, i);
        size_t p = 0;
        while (p < s.size()) {
            size_t c = s.find(',', p);
            if (c == std::string::npos) c = s.size();
            const std::string part = trim(std::string_view(s).substr(p, c - p));
            if (!part.empty()) out.push_back(std::atoi(part.c_str()));
            p = c + 1;
        }
    }
    return out;
}

int32_t signedColor(uint32_t argb) { return int32_t(argb); }

// --------------------------------------------------------------- properties --

bool getAny(lua_State* L, const std::string& path, Value& out) {
    if (G(L).getGameProp(path, out)) return true;
    return SC(L).getProp(path, out);
}

bool setAny(lua_State* L, const std::string& path, const Value& v) {
    if (G(L).setGameProp(path, v)) return true;
    return SC(L).setProp(path, v);
}

int l_getProperty(lua_State* L) {
    Value v;
    if (getAny(L, str(L, 1), v)) return ret(L, v);
    lua_pushnil(L);
    return 1;
}

int l_setProperty(lua_State* L) {
    setAny(L, str(L, 1), toValue(L, 2));
    return retBool(L, true);
}

bool isNoteGroup(const std::string& g) {
    const std::string l = toLower(g);
    return l == "notes" || l == "unspawnnotes";
}

int l_getPropertyFromGroup(lua_State* L) {
    const std::string group = str(L, 1);
    const int index = int(num(L, 2));
    const std::string field = str(L, 3);
    Value v;
    if (isNoteGroup(group)) {
        if (G(L).noteGet(group, index, field, v)) return ret(L, v);
    } else if (scene::Strum* s = SC(L).strumByGroup(group, index)) {
        if (s->get(field, v)) return ret(L, v);
    }
    lua_pushnil(L);
    return 1;
}

int l_setPropertyFromGroup(lua_State* L) {
    const std::string group = str(L, 1);
    const int index = int(num(L, 2));
    const std::string field = str(L, 3);
    const Value v = toValue(L, 4);
    if (isNoteGroup(group)) G(L).noteSet(group, index, field, v);
    else if (scene::Strum* s = SC(L).strumByGroup(group, index)) s->set(field, v);
    return 0;
}

std::string classPath(std::string cls, std::string var) {
    const size_t dot = cls.rfind('.');
    if (dot != std::string::npos) cls = cls.substr(dot + 1);
    if (var.rfind("data.", 0) == 0 && cls == "ClientPrefs") var = var.substr(5);
    return "class:" + cls + "." + var;
}

int l_getPropertyFromClass(lua_State* L) {
    const std::string cls = str(L, 1), var = str(L, 2);
    std::string shortCls = cls.substr(cls.rfind('.') == std::string::npos ? 0 : cls.rfind('.') + 1);
    Value v;
    if (shortCls == "FlxG" && var.rfind("camera.", 0) == 0) {
        if (SC(L).getProp("camGame." + var.substr(7), v)) return ret(L, v);
    }
    if (shortCls == "PlayState" && var.rfind("instance.", 0) == 0) {
        if (getAny(L, var.substr(9), v)) return ret(L, v);
    }
    if (G(L).getGameProp(classPath(cls, var), v)) return ret(L, v);
    if (auto it = SC(L).vars.find(classPath(cls, var)); it != SC(L).vars.end()) return ret(L, it->second);
    lua_pushnil(L);
    return 1;
}

int l_setPropertyFromClass(lua_State* L) {
    const std::string cls = str(L, 1), var = str(L, 2);
    const Value v = toValue(L, 3);
    std::string shortCls = cls.substr(cls.rfind('.') == std::string::npos ? 0 : cls.rfind('.') + 1);
    if (shortCls == "FlxG" && var.rfind("camera.", 0) == 0 && SC(L).setProp("camGame." + var.substr(7), v)) return 0;
    if (shortCls == "PlayState" && var.rfind("instance.", 0) == 0) { setAny(L, var.substr(9), v); return 0; }
    if (!G(L).setGameProp(classPath(cls, var), v)) SC(L).vars[classPath(cls, var)] = v;
    return 0;
}

// ------------------------------------------------------------------ sprites --

SpritePtr newSprite(lua_State* L, const std::string& tag) {
    scene::Scene& sc = SC(L);
    if (auto it = sc.objects.find(tag); it != sc.objects.end()) {
        sc.remove(it->second);
        sc.objects.erase(it);
    }
    auto s = std::make_shared<Sprite>();
    s->tag = tag;
    sc.objects[tag] = s;
    return s;
}

int l_makeLuaSprite(lua_State* L) {
    const std::string tag = cleanTag(str(L, 1));
    const std::string image = str(L, 2);
    SpritePtr s = newSprite(L, tag);
    s->x = float(num(L, 3));
    s->y = float(num(L, 4));
    if (!image.empty()) {
        if (auto g = SC(L).gfx.image(image)) s->setGraphic(g);
        else warn(L, "image not found: " + image);
    }
    return 0;
}

int l_makeAnimatedLuaSprite(lua_State* L) {
    const std::string tag = cleanTag(str(L, 1));
    const std::string image = str(L, 2);
    SpritePtr s = newSprite(L, tag);
    s->x = float(num(L, 3));
    s->y = float(num(L, 4));
    if (!image.empty()) {
        if (auto g = SC(L).gfx.atlas(image)) s->setGraphic(g);
        else warn(L, "atlas not found: " + image);
    }
    return 0;
}

int l_makeGraphic(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        const uint32_t c = psych::parseColor(str(L, 4, "FFFFFF"));
        s->setGraphic(SC(L).gfx.solid(int(num(L, 2, 1)), int(num(L, 3, 1)), c));
    }
    return 0;
}

int l_loadGraphic(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        if (auto g = SC(L).gfx.image(str(L, 2), int(num(L, 3)), int(num(L, 4)))) s->setGraphic(g);
    }
    return 0;
}

int l_loadFrames(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        if (auto g = SC(L).gfx.atlas(str(L, 2))) s->setGraphic(g);
    }
    return 0;
}

void addAnimFrames(lua_State* L, Sprite* s, scene::Anim a) {
    const bool first = s->cur < 0;
    const std::string name = a.name;
    if (a.frames.empty()) {
        warn(L, "no frames for animation '" + name + "' on " + s->tag);
        return;
    }
    if (Character* c = dynamic_cast<Character*>(s)) {
        c->addAnim(std::move(a));
        c->recalcDance(false);
        return;
    }
    s->addAnim(std::move(a));
    if (first) s->play(name, true);
}

int l_addAnimationByPrefix(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s || !s->graphic) return 0;
    scene::Anim a;
    a.name = str(L, 2);
    a.frames = psych::framesByPrefix(s->graphic->frames, str(L, 3));
    a.fps = num(L, 4, 24);
    a.loop = boolean(L, 5, true);
    addAnimFrames(L, s, std::move(a));
    return 0;
}

int l_addAnimationByIndices(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s || !s->graphic) return 0;
    scene::Anim a;
    a.name = str(L, 2);
    a.frames = psych::framesByIndices(s->graphic->frames, str(L, 3), intListArg(L, 4));
    a.fps = num(L, 5, 24);
    a.loop = boolean(L, 6, false);
    addAnimFrames(L, s, std::move(a));
    return 0;
}

int l_addAnimationByIndicesLoop(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s || !s->graphic) return 0;
    scene::Anim a;
    a.name = str(L, 2);
    a.frames = psych::framesByIndices(s->graphic->frames, str(L, 3), intListArg(L, 4));
    a.fps = num(L, 5, 24);
    a.loop = true;
    addAnimFrames(L, s, std::move(a));
    return 0;
}

int l_addAnimation(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s || !s->graphic) return 0;
    scene::Anim a;
    a.name = str(L, 2);
    for (int f : intListArg(L, 3))
        if (f >= 0 && f < int(s->graphic->frames.size())) a.frames.push_back(f);
    a.fps = num(L, 4, 24);
    a.loop = boolean(L, 5, true);
    addAnimFrames(L, s, std::move(a));
    return 0;
}

int l_addOffset(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s) return 0;
    if (scene::Anim* a = s->anim(str(L, 2))) {
        a->hasOffset = true;
        a->offX = float(num(L, 3));
        a->offY = float(num(L, 4));
    }
    return 0;
}

int l_playAnim(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s) return retBool(L, false);
    const std::string name = str(L, 2);
    const bool force = boolean(L, 3, false), rev = boolean(L, 4, false);
    const int start = int(num(L, 5, 0));
    if (Character* c = dynamic_cast<Character*>(s)) {
        c->playAnim(name, force, rev, start);
        return retBool(L, true);
    }
    return retBool(L, s->play(name, force, rev, start));
}

int l_objectPlayAnimation(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s) return retBool(L, false);
    const std::string name = str(L, 2);
    const bool force = boolean(L, 3, false);
    const int start = int(num(L, 4, 0));
    if (Character* c = dynamic_cast<Character*>(s)) {
        c->playAnim(name, force, false, start);
        return retBool(L, true);
    }
    return retBool(L, s->play(name, force, false, start));
}

int l_characterPlayAnim(lua_State* L) {
    if (Character* c = SC(L).character(str(L, 1))) c->playAnim(str(L, 2), boolean(L, 3, false));
    return 0;
}

int l_characterDance(lua_State* L) {
    if (Character* c = SC(L).character(str(L, 1))) c->dance();
    return 0;
}

int l_addLuaSprite(lua_State* L) {
    scene::Scene& sc = SC(L);
    auto it = sc.objects.find(str(L, 1));
    if (it == sc.objects.end()) return 0;
    if (boolean(L, 2, false)) sc.addFront(it->second);
    else sc.addBehindCharacters(it->second);
    return 0;
}

int l_removeLuaSprite(lua_State* L) {
    scene::Scene& sc = SC(L);
    auto it = sc.objects.find(str(L, 1));
    if (it == sc.objects.end()) return 0;
    sc.remove(it->second);
    if (boolean(L, 2, true)) sc.objects.erase(it);
    return 0;
}

int l_luaSpriteExists(lua_State* L) {
    auto& o = SC(L).objects;
    auto it = o.find(str(L, 1));
    return retBool(L, it != o.end() && it->second->kind == scene::Kind::Sprite);
}

int l_luaTextExists(lua_State* L) {
    auto& o = SC(L).objects;
    auto it = o.find(str(L, 1));
    return retBool(L, it != o.end() && it->second->kind == scene::Kind::Text);
}

int l_setScrollFactor(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        s->scrollX = float(num(L, 2, 0));
        s->scrollY = float(num(L, 3, 0));
    }
    return 0;
}

int l_scaleObject(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        s->scaleX = float(num(L, 2, 1));
        s->scaleY = float(num(L, 3, 1));
        if (boolean(L, 4, true)) s->updateHitbox();
    }
    return 0;
}

int l_setGraphicSize(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        s->setGraphicSize(float(num(L, 2, 0)), float(num(L, 3, 0)));
        if (boolean(L, 4, true)) s->updateHitbox();
    }
    return 0;
}

int l_updateHitbox(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) s->updateHitbox();
    return 0;
}

int l_screenCenter(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        const std::string pos = toLower(str(L, 2, "xy"));
        s->screenCenter(pos.find('x') != std::string::npos, pos.find('y') != std::string::npos);
    }
    return 0;
}

int l_setObjectCamera(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) s->cam = scene::Scene::camFromName(str(L, 2, "game"));
    return retBool(L, true);
}

int l_setObjectOrder(lua_State* L) {
    SC(L).setOrder(str(L, 1), int(num(L, 2)));
    return 0;
}

int l_getObjectOrder(lua_State* L) { return retNum(L, SC(L).indexOf(str(L, 1))); }

int l_setBlendMode(lua_State* L) {
    if (Sprite* s = obj(L, str(L, 1))) {
        const std::string b = toLower(str(L, 2));
        s->blend = b == "add" ? 1 : b == "multiply" ? 2 : b == "screen" ? 3 : b == "subtract" ? 4 : 0;
    }
    return retBool(L, true);
}

int l_getMidpointX(lua_State* L) { Sprite* s = obj(L, str(L, 1)); return retNum(L, s ? s->x + s->width * 0.5 : 0); }
int l_getMidpointY(lua_State* L) { Sprite* s = obj(L, str(L, 1)); return retNum(L, s ? s->y + s->height * 0.5 : 0); }
int l_getGraphicMidpointX(lua_State* L) { Sprite* s = obj(L, str(L, 1)); return retNum(L, s ? s->x + s->frameW * 0.5 : 0); }
int l_getGraphicMidpointY(lua_State* L) { Sprite* s = obj(L, str(L, 1)); return retNum(L, s ? s->y + s->frameH * 0.5 : 0); }
int l_getScreenPositionX(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    return retNum(L, s ? s->x - SC(L).camGame.scrollX * s->scrollX : 0);
}
int l_getScreenPositionY(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    return retNum(L, s ? s->y - SC(L).camGame.scrollY * s->scrollY : 0);
}

int l_getCharacterX(lua_State* L) { Character* c = SC(L).character(str(L, 1)); return retNum(L, c ? c->groupX : 0); }
int l_getCharacterY(lua_State* L) { Character* c = SC(L).character(str(L, 1)); return retNum(L, c ? c->groupY : 0); }
int l_setCharacterX(lua_State* L) {
    std::string who = toLower(str(L, 1));
    SC(L).setProp((who == "dad" || who == "opponent" ? "dadGroup" : who == "gf" || who == "girlfriend" ? "gfGroup" : "boyfriendGroup") +
                      std::string(".x"),
                  num(L, 2));
    return 0;
}
int l_setCharacterY(lua_State* L) {
    std::string who = toLower(str(L, 1));
    SC(L).setProp((who == "dad" || who == "opponent" ? "dadGroup" : who == "gf" || who == "girlfriend" ? "gfGroup" : "boyfriendGroup") +
                      std::string(".y"),
                  num(L, 2));
    return 0;
}

int l_precacheImage(lua_State* L) {
    SC(L).gfx.atlas(str(L, 1));
    return 0;
}

// --------------------------------------------------------------------- text --

void measureText(Sprite& s) {
    size_t longest = 0, lines = 1, cur = 0;
    for (char c : s.text.text) {
        if (c == '\n') { ++lines; longest = std::max(longest, cur); cur = 0; }
        else ++cur;
    }
    longest = std::max(longest, cur);
    s.width = s.text.fieldWidth > 0 ? s.text.fieldWidth : float(longest) * s.text.size * 0.55f + 4;
    s.height = float(lines) * s.text.size * 1.15f + 4;
    s.frameW = s.width;
    s.frameH = s.height;
    s.centerOrigin();
}

std::string resolveFont(lua_State* L, const std::string& name) {
    std::string f = SC(L).files.find("fonts/" + name);
    if (f.empty()) f = SC(L).files.find(name);
    return f.empty() ? name : f;
}

int l_makeLuaText(lua_State* L) {
    const std::string tag = cleanTag(str(L, 1));
    SpritePtr s = newSprite(L, tag);
    s->kind = scene::Kind::Text;
    s->text.text = str(L, 2);
    s->text.fieldWidth = float(num(L, 3, 0));
    s->x = float(num(L, 4));
    s->y = float(num(L, 5));
    s->text.size = 16;
    s->text.align = 1;
    s->text.borderSize = 2;
    s->text.font = resolveFont(L, "vcr.ttf");
    s->cam = scene::Cam::Hud;
    s->scrollX = s->scrollY = 0;
    measureText(*s);
    return 0;
}

Sprite* textObj(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    return s && s->kind == scene::Kind::Text ? s : nullptr;
}

int l_setTextString(lua_State* L) { if (Sprite* s = textObj(L)) { s->text.text = str(L, 2); measureText(*s); } return 0; }
int l_setTextSize(lua_State* L) { if (Sprite* s = textObj(L)) { s->text.size = float(num(L, 2, 16)); measureText(*s); } return 0; }
int l_setTextWidth(lua_State* L) { if (Sprite* s = textObj(L)) { s->text.fieldWidth = float(num(L, 2)); measureText(*s); } return 0; }
int l_setTextBorder(lua_State* L) {
    if (Sprite* s = textObj(L)) {
        s->text.borderSize = float(num(L, 2, 0));
        if (!lua_isnoneornil(L, 3)) s->text.borderColor = psych::parseColor(str(L, 3), 0xFF000000);
    }
    return 0;
}
int l_setTextColor(lua_State* L) { if (Sprite* s = textObj(L)) s->text.color = psych::parseColor(str(L, 2)); return 0; }
int l_setTextFont(lua_State* L) { if (Sprite* s = textObj(L)) s->text.font = resolveFont(L, str(L, 2)); return 0; }
int l_setTextItalic(lua_State* L) { if (Sprite* s = textObj(L)) s->text.italic = boolean(L, 2); return 0; }
int l_setTextAlignment(lua_State* L) {
    if (Sprite* s = textObj(L)) {
        const std::string a = toLower(str(L, 2, "left"));
        s->text.align = a == "center" ? 1 : a == "right" ? 2 : 0;
    }
    return 0;
}
int l_getTextString(lua_State* L) { Sprite* s = textObj(L); return ret(L, s ? Value(s->text.text) : Value()); }
int l_getTextSize(lua_State* L) { Sprite* s = textObj(L); return retNum(L, s ? s->text.size : 0); }
int l_getTextFont(lua_State* L) { Sprite* s = textObj(L); return ret(L, s ? Value(s->text.font) : Value()); }
int l_getTextWidth(lua_State* L) { Sprite* s = textObj(L); return retNum(L, s ? s->width : 0); }

int l_addLuaText(lua_State* L) {
    scene::Scene& sc = SC(L);
    auto it = sc.objects.find(str(L, 1));
    if (it != sc.objects.end()) sc.addFront(it->second);
    return 0;
}

// ------------------------------------------------------------------- tweens --

bool tweenTargetExists(lua_State* L, const std::string& target, const char* field) {
    Value v;
    return SC(L).getProp(target + "." + field, v);
}

void startFieldTween(lua_State* L, const std::string& tag, const std::string& target, const char* field, double to,
                     double dur, const std::string& ease) {
    if (!tweenTargetExists(L, target, field)) {
        warn(L, "tween target not found: " + target);
        return;
    }
    scene::Tween tw;
    tw.tag = tag;
    tw.target = target;
    tw.fields.push_back({field, 0, to, false});
    tw.dur = dur;
    tw.ease = ease;
    tw.script = S(L).index;
    SC(L).tween(std::move(tw));
}

int tweenField(lua_State* L, const char* field) {
    startFieldTween(L, str(L, 1), str(L, 2), field, num(L, 3), num(L, 4, 1), str(L, 5, "linear"));
    return 0;
}
int l_doTweenX(lua_State* L) { return tweenField(L, "x"); }
int l_doTweenY(lua_State* L) { return tweenField(L, "y"); }
int l_doTweenAngle(lua_State* L) { return tweenField(L, "angle"); }
int l_doTweenAlpha(lua_State* L) { return tweenField(L, "alpha"); }
int l_doTweenZoom(lua_State* L) { return tweenField(L, "zoom"); }
int l_doTweenScaleX(lua_State* L) { return tweenField(L, "scale.x"); }
int l_doTweenScaleY(lua_State* L) { return tweenField(L, "scale.y"); }

int l_doTweenColor(lua_State* L) {
    const std::string target = str(L, 2);
    Value cur;
    if (!SC(L).getProp(target + ".color", cur)) { warn(L, "tween target not found: " + target); return 0; }
    scene::Tween tw;
    tw.tag = str(L, 1);
    tw.target = target;
    tw.isColor = true;
    tw.c0 = uint32_t(int64_t(scene::toNum(cur))) & 0xFFFFFF;
    tw.c1 = psych::parseColor(str(L, 3)) & 0xFFFFFF;
    tw.dur = num(L, 4, 1);
    tw.ease = str(L, 5, "linear");
    tw.script = S(L).index;
    SC(L).tween(std::move(tw));
    return 0;
}

std::string strumPath(lua_State* L, int note) {
    const int n = int(SC(L).strums.size());
    if (n == 0) return "";
    note = ((note % n) + n) % n;
    return "strumLineNotes.members[" + std::to_string(note) + "]";
}

int noteTween(lua_State* L, const char* field) {
    const std::string p = strumPath(L, int(num(L, 2)));
    if (!p.empty()) startFieldTween(L, str(L, 1), p, field, num(L, 3), num(L, 4, 1), str(L, 5, "linear"));
    return 0;
}
int l_noteTweenX(lua_State* L) { return noteTween(L, "x"); }
int l_noteTweenY(lua_State* L) { return noteTween(L, "y"); }
int l_noteTweenAngle(lua_State* L) { return noteTween(L, "angle"); }
int l_noteTweenAlpha(lua_State* L) { return noteTween(L, "alpha"); }
int l_noteTweenDirection(lua_State* L) { return noteTween(L, "direction"); }
int l_noteTweenScaleX(lua_State* L) { return noteTween(L, "scale.x"); }
int l_noteTweenScaleY(lua_State* L) { return noteTween(L, "scale.y"); }

int l_startTween(lua_State* L) {
    scene::Tween tw;
    tw.tag = str(L, 1);
    tw.target = str(L, 2);
    tw.dur = num(L, 4, 1);
    tw.script = S(L).index;
    if (lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const std::string key = lua_tostring(L, -2);
                if (key == "color") {
                    Value cur;
                    SC(L).getProp(tw.target + ".color", cur);
                    tw.isColor = true;
                    tw.c0 = uint32_t(int64_t(scene::toNum(cur))) & 0xFFFFFF;
                    tw.c1 = uint32_t(int64_t(lua_tonumber(L, -1))) & 0xFFFFFF;
                } else {
                    tw.fields.push_back({key, 0, double(lua_tonumber(L, -1)), false});
                }
            }
            lua_pop(L, 1);
        }
    }
    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "ease");
        if (lua_isstring(L, -1)) tw.ease = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 5, "onComplete");
        if (lua_isstring(L, -1)) tw.onComplete = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 5, "startDelay");
        if (lua_isnumber(L, -1)) tw.delay = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    if (tw.fields.empty() && !tw.isColor) return 0;
    SC(L).tween(std::move(tw));
    return 0;
}

int l_cancelTween(lua_State* L) { SC(L).cancelTween(str(L, 1)); return 0; }

int l_runTimer(lua_State* L) {
    scene::Timer t;
    t.tag = str(L, 1);
    t.time = num(L, 2, 1);
    t.loops = int(num(L, 3, 1));
    t.script = S(L).index;
    SC(L).timer(std::move(t));
    return 0;
}

int l_cancelTimer(lua_State* L) { SC(L).cancelTimer(str(L, 1)); return 0; }

// ------------------------------------------------------------------- camera --

scene::Camera* camArg(lua_State* L, int i) {
    scene::Camera* c = SC(L).camera(str(L, i, "game"));
    return c ? c : &SC(L).camGame;
}

int l_cameraShake(lua_State* L) {
    scene::Camera* c = camArg(L, 1);
    c->shakeIntensity = float(num(L, 2, 0.05));
    c->shakeTime = float(num(L, 3, 0.5));
    return 0;
}

int l_cameraFlash(lua_State* L) {
    scene::Camera* c = camArg(L, 1);
    const bool forced = boolean(L, 4, false);
    if (!forced && c->flashTime > 0) return 0;
    c->flashColor = psych::parseColor(str(L, 2, "FFFFFF"));
    c->flashDur = c->flashTime = float(std::max(0.001, num(L, 3, 1)));
    return 0;
}

int l_cameraFade(lua_State* L) {
    scene::Camera* c = camArg(L, 1);
    c->fadeColor = psych::parseColor(str(L, 2, "000000"));
    c->fadeDur = float(std::max(0.001, num(L, 3, 1)));
    c->fadeTime = 0;
    c->fadeIn = boolean(L, 5, false);
    c->fadeAlpha = c->fadeIn ? 1.0f : 0.0f;
    return 0;
}

int whoFromName(const std::string& n) {
    const std::string l = toLower(trim(n));
    if (l == "dad" || l == "opponent") return 1;
    if (l == "gf" || l == "girlfriend") return 2;
    return 0;
}

int l_cameraSetTarget(lua_State* L) {
    scene::Scene& sc = SC(L);
    sc.cameraTarget(sc.followX, sc.followY, whoFromName(str(L, 1)));
    return retBool(L, whoFromName(str(L, 1)) == 1);
}

int l_setCameraScroll(lua_State* L) {
    SC(L).camGame.scrollX = float(num(L, 1) - scene::kW * 0.5);
    SC(L).camGame.scrollY = float(num(L, 2) - scene::kH * 0.5);
    return 0;
}
int l_setCameraFollowPoint(lua_State* L) { SC(L).followX = float(num(L, 1)); SC(L).followY = float(num(L, 2)); return 0; }
int l_addCameraScroll(lua_State* L) { SC(L).camGame.scrollX += float(num(L, 1)); SC(L).camGame.scrollY += float(num(L, 2)); return 0; }
int l_addCameraFollowPoint(lua_State* L) { SC(L).followX += float(num(L, 1)); SC(L).followY += float(num(L, 2)); return 0; }
int l_getCameraScrollX(lua_State* L) { return retNum(L, SC(L).camGame.scrollX + scene::kW * 0.5); }
int l_getCameraScrollY(lua_State* L) { return retNum(L, SC(L).camGame.scrollY + scene::kH * 0.5); }
int l_getCameraFollowX(lua_State* L) { return retNum(L, SC(L).followX); }
int l_getCameraFollowY(lua_State* L) { return retNum(L, SC(L).followY); }

int l_setHealthBarColors(lua_State* L) {
    G(L).healthBarColors(psych::parseColor(str(L, 1, "FF0000")), psych::parseColor(str(L, 2, "66FF33")));
    return 0;
}
int l_setTimeBarColors(lua_State* L) {
    G(L).timeBarColors(psych::parseColor(str(L, 1, "FFFFFF")), psych::parseColor(str(L, 2, "000000")));
    return 0;
}

// -------------------------------------------------------------------- sound --

int l_playSound(lua_State* L) {
    G(L).playSound(str(L, 1), num(L, 2, 1), str(L, 3), false);
    return 0;
}
int l_playMusic(lua_State* L) {
    G(L).playSound(str(L, 1), num(L, 2, 1), "__music", boolean(L, 3, false));
    return 0;
}
int soundCmd(lua_State* L, const char* cmd, int tagIdx = 1) {
    G(L).soundCommand(str(L, tagIdx), cmd, num(L, tagIdx + 1, 0), num(L, tagIdx + 2, 0), num(L, tagIdx + 3, 0));
    return 0;
}
int l_stopSound(lua_State* L) { return soundCmd(L, "stop"); }
int l_pauseSound(lua_State* L) { return soundCmd(L, "pause"); }
int l_resumeSound(lua_State* L) { return soundCmd(L, "resume"); }
int l_soundFadeIn(lua_State* L) {
    G(L).soundCommand(str(L, 1), "fadeIn", num(L, 2, 1), num(L, 3, 0), num(L, 4, 1));
    return 0;
}
int l_soundFadeOut(lua_State* L) {
    G(L).soundCommand(str(L, 1), "fadeOut", num(L, 2, 1), num(L, 3, 0), 0);
    return 0;
}
int l_soundFadeCancel(lua_State* L) { return soundCmd(L, "fadeCancel"); }
int l_setSoundVolume(lua_State* L) { G(L).soundCommand(str(L, 2), "volume", num(L, 1, 1), 0, 0); return 0; }
int l_getSoundVolume(lua_State* L) { return retNum(L, G(L).soundQuery(str(L, 1), "volume")); }
int l_setSoundTime(lua_State* L) { G(L).soundCommand(str(L, 1), "time", num(L, 2), 0, 0); return 0; }
int l_getSoundTime(lua_State* L) { return retNum(L, G(L).soundQuery(str(L, 1), "time")); }
int l_luaSoundExists(lua_State* L) { return retBool(L, G(L).soundQuery(str(L, 1), "exists") > 0); }
int l_musicFadeIn(lua_State* L) { G(L).soundCommand("__music", "fadeIn", num(L, 1, 1), num(L, 2, 0), num(L, 3, 1)); return 0; }
int l_musicFadeOut(lua_State* L) { G(L).soundCommand("__music", "fadeOut", num(L, 1, 1), num(L, 2, 0), 0); return 0; }

// --------------------------------------------------------------------- misc --

int l_debugPrint(lua_State* L) {
    std::string s;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (lua_isnil(L, i)) s += "nil";
        else s += luaL_tolstring(L, i, nullptr), lua_pop(L, 1);
    }
    G(L).debugPrint(s, 0xFFFFFFFF);
    return 0;
}

int l_close(lua_State* L) {
    S(L).closed = true;
    return retBool(L, true);
}

int l_triggerEvent(lua_State* L) {
    G(L).triggerEvent(str(L, 1), str(L, 2), str(L, 3));
    return retBool(L, true);
}

int l_getSongPosition(lua_State* L) { return retNum(L, G(L).songPosition()); }

int l_getRandomInt(lua_State* L) {
    const int lo = int(num(L, 1, 0)), hi = int(num(L, 2, 2147483647));
    const std::vector<int> ex = intListArg(L, 3);
    if (hi < lo) return retNum(L, lo);
    for (int tries = 0; tries < 64; ++tries) {
        const int v = std::uniform_int_distribution<int>(lo, hi)(rng());
        if (std::find(ex.begin(), ex.end(), v) == ex.end()) return retNum(L, v);
    }
    return retNum(L, lo);
}

int l_getRandomFloat(lua_State* L) {
    const double lo = num(L, 1, 0), hi = num(L, 2, 1);
    lua_pushnumber(L, lo + (hi - lo) * std::uniform_real_distribution<double>(0, 1)(rng()));
    return 1;
}

int l_getRandomBool(lua_State* L) {
    return retBool(L, std::uniform_real_distribution<double>(0, 100)(rng()) < num(L, 1, 50));
}

int l_getColorFromHex(lua_State* L) {
    std::string s = str(L, 1);
    if (s.rfind("0x", 0) != 0 && s.rfind("0X", 0) != 0 && (s.empty() || s[0] != '#')) s = "#" + s;
    return retNum(L, signedColor(psych::parseColor(s)));
}
int l_getColorFromString(lua_State* L) { return retNum(L, signedColor(psych::parseColor(str(L, 1)))); }

int l_stringStartsWith(lua_State* L) { const std::string a = str(L, 1), b = str(L, 2); return retBool(L, a.rfind(b, 0) == 0); }
int l_stringEndsWith(lua_State* L) {
    const std::string a = str(L, 1), b = str(L, 2);
    return retBool(L, a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0);
}
int l_stringTrim(lua_State* L) { return ret(L, Value(trim(str(L, 1)))); }
int l_stringSplit(lua_State* L) {
    const std::string s = str(L, 1), sep = str(L, 2);
    lua_newtable(L);
    lua_Integer k = 1;
    if (sep.empty()) {
        for (char c : s) { lua_pushlstring(L, &c, 1); lua_rawseti(L, -2, k++); }
        return 1;
    }
    size_t p = 0;
    while (true) {
        const size_t e = s.find(sep, p);
        const std::string part = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        lua_pushlstring(L, part.data(), part.size());
        lua_rawseti(L, -2, k++);
        if (e == std::string::npos) break;
        p = e + sep.size();
    }
    return 1;
}

int l_getTextFromFile(lua_State* L) {
    const std::string f = SC(L).files.find(str(L, 1));
    std::string text;
    if (!f.empty() && scene::readFile(f, text)) return ret(L, Value(text));
    lua_pushnil(L);
    return 1;
}
int l_checkFileExists(lua_State* L) { return retBool(L, !SC(L).files.find(str(L, 1)).empty()); }

int l_setVar(lua_State* L) { SC(L).vars[str(L, 1)] = toValue(L, 2); return 0; }
int l_getVar(lua_State* L) {
    auto it = SC(L).vars.find(str(L, 1));
    if (it == SC(L).vars.end()) { lua_pushnil(L); return 1; }
    return ret(L, it->second);
}
int l_removeVar(lua_State* L) { return retBool(L, SC(L).vars.erase(str(L, 1)) > 0); }

int l_keyJustPressed(lua_State* L) { return retBool(L, G(L).keyState(toLower(str(L, 1)), 1)); }
int l_keyPressed(lua_State* L) { return retBool(L, G(L).keyState(toLower(str(L, 1)), 0)); }
int l_keyReleased(lua_State* L) { return retBool(L, G(L).keyState(toLower(str(L, 1)), 2)); }
int l_keyboardJustPressed(lua_State* L) { return retBool(L, G(L).keyboardState(str(L, 1), 1)); }
int l_keyboardPressed(lua_State* L) { return retBool(L, G(L).keyboardState(str(L, 1), 0)); }
int l_keyboardReleased(lua_State* L) { return retBool(L, G(L).keyboardState(str(L, 1), 2)); }
int l_mouseClicked(lua_State* L) { return retBool(L, G(L).mouseState(str(L, 1, "left"), 1)); }
int l_mousePressed(lua_State* L) { return retBool(L, G(L).mouseState(str(L, 1, "left"), 0)); }
int l_mouseReleased(lua_State* L) { return retBool(L, G(L).mouseState(str(L, 1, "left"), 2)); }
int l_getMouseX(lua_State* L) { return retNum(L, G(L).mouseX()); }
int l_getMouseY(lua_State* L) { return retNum(L, G(L).mouseY()); }

double gameNum(lua_State* L, const char* path) {
    Value v;
    return G(L).getGameProp(path, v) ? scene::toNum(v) : 0.0;
}
int setGameNum(lua_State* L, const char* path, double v) { G(L).setGameProp(path, v); return 0; }

int l_setHealth(lua_State* L) { return setGameNum(L, "health", num(L, 1)); }
int l_addHealth(lua_State* L) { return setGameNum(L, "health", gameNum(L, "health") + num(L, 1)); }
int l_getHealth(lua_State* L) { return retNum(L, gameNum(L, "health")); }
int l_addScore(lua_State* L) { return setGameNum(L, "songScore", gameNum(L, "songScore") + num(L, 1)); }
int l_setScore(lua_State* L) { return setGameNum(L, "songScore", num(L, 1)); }
int l_getScore(lua_State* L) { return retNum(L, gameNum(L, "songScore")); }
int l_addMisses(lua_State* L) { return setGameNum(L, "songMisses", gameNum(L, "songMisses") + num(L, 1)); }
int l_setMisses(lua_State* L) { return setGameNum(L, "songMisses", num(L, 1)); }
int l_getMisses(lua_State* L) { return retNum(L, gameNum(L, "songMisses")); }
int l_addHits(lua_State* L) { return setGameNum(L, "songHits", gameNum(L, "songHits") + num(L, 1)); }
int l_setHits(lua_State* L) { return setGameNum(L, "songHits", num(L, 1)); }
int l_getHits(lua_State* L) { return retNum(L, gameNum(L, "songHits")); }
int l_setRatingPercent(lua_State* L) { G(L).setGameProp("ratingPercent", num(L, 1)); return 0; }
int l_setRatingName(lua_State* L) { G(L).setGameProp("ratingName", str(L, 1)); return 0; }
int l_setRatingFC(lua_State* L) { G(L).setGameProp("ratingFC", str(L, 1)); return 0; }

int l_endSong(lua_State* L) { G(L).endSong(); return retBool(L, true); }
int l_restartSong(lua_State* L) { G(L).restartSong(); return retBool(L, true); }
int l_exitSong(lua_State* L) { G(L).exitSong(); return retBool(L, true); }
int l_startCountdown(lua_State* L) { G(L).startCountdown(); return retBool(L, true); }
int l_startVideo(lua_State* L) {
    warn(L, "videos are not supported, skipping " + str(L, 1));
    G(L).startAndEnd();
    return retBool(L, false);
}
int l_startDialogue(lua_State* L) {
    G(L).startAndEnd();
    return retBool(L, false);
}

int l_changeCharacter(lua_State* L) {
    G(L).changeCharacter(whoFromName(str(L, 1)), str(L, 2));
    return 0;
}

// Save data lives in the shared variable map for the session.
int l_initSaveData(lua_State* L) { (void)L; return 0; }
int l_flushSaveData(lua_State* L) { (void)L; return 0; }
int l_setDataFromSave(lua_State* L) { SC(L).vars["save:" + str(L, 1) + "." + str(L, 2)] = toValue(L, 3); return 0; }
int l_getDataFromSave(lua_State* L) {
    auto it = SC(L).vars.find("save:" + str(L, 1) + "." + str(L, 2));
    if (it != SC(L).vars.end()) return ret(L, it->second);
    lua_pushvalue(L, 3);
    return 1;
}

// Psych 0.7.3 callMethod: only the common object methods.
int l_callMethod(lua_State* L) {
    const std::string full = str(L, 1);
    const size_t dot = full.rfind('.');
    if (dot == std::string::npos) { lua_pushnil(L); return 1; }
    const std::string objName = full.substr(0, dot), method = full.substr(dot + 1);
    std::vector<Value> args = lua_istable(L, 2) ? [&] {
        std::vector<Value> out;
        const lua_Integer n = luaL_len(L, 2);
        for (lua_Integer k = 1; k <= n; ++k) { lua_geti(L, 2, k); out.push_back(toValue(L, -1)); lua_pop(L, 1); }
        return out;
    }() : std::vector<Value>{};
    Sprite* s = obj(L, objName);
    if (!s) { lua_pushnil(L); return 1; }
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : Value(); };
    Character* c = dynamic_cast<Character*>(s);
    if (method == "getAnimationName") return ret(L, Value(c && c->builtin ? c->lastAnim : s->curAnimName()));
    if (method == "isAnimationFinished") return retBool(L, s->finished);
    if (method == "hasAnimation") return retBool(L, s->hasAnim(scene::toStr(arg(0))));
    if (method == "playAnim") {
        if (c) c->playAnim(scene::toStr(arg(0)), scene::toBool(arg(1)), scene::toBool(arg(2)), int(scene::toNum(arg(3))));
        else s->play(scene::toStr(arg(0)), scene::toBool(arg(1)), scene::toBool(arg(2)), int(scene::toNum(arg(3))));
        return 0;
    }
    if (method == "dance") { if (c) c->dance(); return 0; }
    if (method == "updateHitbox") { s->updateHitbox(); return 0; }
    if (method == "screenCenter") { s->screenCenter(true, true); return 0; }
    if (method == "kill") { s->visible = s->active = false; return 0; }
    if (method == "revive") { s->visible = s->active = true; return 0; }
    lua_pushnil(L);
    return 1;
}

int l_stubFalse(lua_State* L) { return retBool(L, false); }
int l_stubNil(lua_State* L) { (void)L; return 0; }

int l_stubTrue(lua_State* L) { return retBool(L, true); }

// ------------------------------------------------------------------ shaders --

bool loadShaderSource(lua_State* L, const std::string& name) {
    scene::Scene& sc = SC(L);
    if (name.empty()) return false;
    if (sc.shaderSources.count(name)) return true;
    scene::ShaderSource src;
    const std::string frag = sc.files.find("shaders/" + name + ".frag");
    const std::string vert = sc.files.find("shaders/" + name + ".vert");
    if (frag.empty() && vert.empty()) {
        warn(L, "shader not found: " + name);
        return false;
    }
    if (!frag.empty()) scene::readFile(frag, src.frag);
    if (!vert.empty()) scene::readFile(vert, src.vert);
    sc.shaderSources[name] = std::move(src);
    return true;
}

int l_initLuaShader(lua_State* L) { return retBool(L, loadShaderSource(L, str(L, 1))); }

scene::ShaderInstance* shaderOf(lua_State* L, int i) {
    Sprite* s = obj(L, str(L, i));
    return s && s->shader ? s->shader.get() : nullptr;
}

int l_setSpriteShader(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    const std::string name = str(L, 2);
    if (!s || !loadShaderSource(L, name)) return retBool(L, false);
    s->shader = std::make_shared<scene::ShaderInstance>();
    s->shader->name = name;
    return retBool(L, true);
}

int l_removeSpriteShader(lua_State* L) {
    Sprite* s = obj(L, str(L, 1));
    if (!s) return retBool(L, false);
    s->shader.reset();
    return retBool(L, true);
}

std::vector<float> floatList(lua_State* L, int i) {
    std::vector<float> out;
    if (lua_istable(L, i)) {
        const lua_Integer n = luaL_len(L, i);
        for (lua_Integer k = 1; k <= n; ++k) {
            lua_geti(L, i, k);
            out.push_back(float(lua_isboolean(L, -1) ? lua_toboolean(L, -1) : lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    }
    return out;
}

int l_setShaderFloat(lua_State* L) {
    if (auto* sh = shaderOf(L, 1)) sh->floats[str(L, 2)] = {float(num(L, 3))};
    return retBool(L, true);
}
int l_setShaderFloatArray(lua_State* L) {
    if (auto* sh = shaderOf(L, 1)) sh->floats[str(L, 2)] = floatList(L, 3);
    return retBool(L, true);
}
int l_setShaderInt(lua_State* L) {
    if (auto* sh = shaderOf(L, 1)) sh->ints[str(L, 2)] = {int(num(L, 3))};
    return retBool(L, true);
}
int l_setShaderIntArray(lua_State* L) {
    if (auto* sh = shaderOf(L, 1)) {
        std::vector<int> v;
        for (float f : floatList(L, 3)) v.push_back(int(f));
        sh->ints[str(L, 2)] = v;
    }
    return retBool(L, true);
}
int l_setShaderBool(lua_State* L) {
    if (auto* sh = shaderOf(L, 1)) sh->ints[str(L, 2)] = {boolean(L, 3) ? 1 : 0};
    return retBool(L, true);
}
int l_setShaderSampler2D(lua_State* L) {
    if (auto* sh = shaderOf(L, 1)) sh->samplers[str(L, 2)] = str(L, 3);
    return retBool(L, true);
}
int pushNumList(lua_State* L, const std::vector<float>& v, bool asBool) {
    lua_newtable(L);
    for (size_t k = 0; k < v.size(); ++k) {
        if (asBool) lua_pushboolean(L, v[k] != 0);
        else push(L, Value(double(v[k])));
        lua_rawseti(L, -2, lua_Integer(k + 1));
    }
    return 1;
}
int l_getShaderFloat(lua_State* L) {
    auto* sh = shaderOf(L, 1);
    if (!sh) { lua_pushnil(L); return 1; }
    auto it = sh->floats.find(str(L, 2));
    if (it == sh->floats.end() || it->second.empty()) { lua_pushnil(L); return 1; }
    return retNum(L, it->second[0]);
}
int l_getShaderFloatArray(lua_State* L) {
    auto* sh = shaderOf(L, 1);
    if (!sh) { lua_pushnil(L); return 1; }
    auto it = sh->floats.find(str(L, 2));
    if (it == sh->floats.end()) { lua_pushnil(L); return 1; }
    return pushNumList(L, it->second, false);
}
int intUniform(lua_State* L, bool asBool, bool array) {
    auto* sh = shaderOf(L, 1);
    if (!sh) { lua_pushnil(L); return 1; }
    auto it = sh->ints.find(str(L, 2));
    if (it == sh->ints.end() || it->second.empty()) { lua_pushnil(L); return 1; }
    if (array) {
        std::vector<float> f(it->second.begin(), it->second.end());
        return pushNumList(L, f, asBool);
    }
    if (asBool) return retBool(L, it->second[0] != 0);
    return retNum(L, it->second[0]);
}
int l_getShaderInt(lua_State* L) { return intUniform(L, false, false); }
int l_getShaderIntArray(lua_State* L) { return intUniform(L, false, true); }
int l_getShaderBool(lua_State* L) { return intUniform(L, true, false); }
int l_getShaderBoolArray(lua_State* L) { return intUniform(L, true, true); }

// ------------------------------------------------------------------ HScript --

hx::Value luaToHx(lua_State* L, int i, int depth = 0) {
    switch (lua_type(L, i)) {
        case LUA_TBOOLEAN: return hx::Value(bool(lua_toboolean(L, i)));
        case LUA_TNUMBER: return hx::Value(double(lua_tonumber(L, i)));
        case LUA_TSTRING: return hx::Value(str(L, i));
        case LUA_TTABLE: {
            if (depth > 8) return hx::Value();
            i = lua_absindex(L, i);
            const lua_Integer n = luaL_len(L, i);
            if (n > 0) {
                std::vector<hx::Value> arr;
                for (lua_Integer k = 1; k <= n; ++k) {
                    lua_geti(L, i, k);
                    arr.push_back(luaToHx(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return hx::Value::array(arr);
            }
            hx::ValueMap m;
            lua_pushnil(L);
            while (lua_next(L, i)) {
                if (lua_type(L, -2) == LUA_TSTRING) m[lua_tostring(L, -2)] = luaToHx(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return hx::Value::object(m);
        }
        default: return hx::Value();
    }
}

void pushHx(lua_State* L, const hx::Value& v, int depth = 0) {
    switch (v.t) {
        case hx::T::Bool: lua_pushboolean(L, v.b); return;
        case hx::T::Num: push(L, Value(v.n)); return;
        case hx::T::Str: lua_pushlstring(L, v.s.data(), v.s.size()); return;
        case hx::T::Array:
            if (depth > 8) break;
            lua_newtable(L);
            for (size_t k = 0; k < v.arr->size(); ++k) {
                pushHx(L, (*v.arr)[k], depth + 1);
                lua_rawseti(L, -2, lua_Integer(k + 1));
            }
            return;
        case hx::T::Object:
        case hx::T::Map:
            if (depth > 8) break;
            lua_newtable(L);
            for (auto& kv : *v.map) {
                pushHx(L, kv.second, depth + 1);
                lua_setfield(L, -2, kv.first.c_str());
            }
            return;
        default: break;
    }
    lua_pushnil(L);
}

hx::Interp& hxOf(lua_State* L) {
    ScriptHost::Script& s = S(L);
    if (!s.hx) {
        s.hx = std::make_unique<hx::Interp>(SC(L), G(L), &H(L), H(L).hxShared);
        s.hx->scriptName = s.name + " (haxe)";
    }
    s.hx->budget = s.budget;
    return *s.hx;
}

std::vector<hx::Value> hxArgs(lua_State* L, int i) {
    std::vector<hx::Value> out;
    if (!lua_istable(L, i)) return out;
    hx::Value v = luaToHx(L, i);
    if (v.t == hx::T::Array) out = *v.arr;
    return out;
}

int l_runHaxeCode(lua_State* L) {
    hx::Interp& ip = hxOf(L);
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            if (lua_type(L, -2) == LUA_TSTRING) ip.setGlobal(lua_tostring(L, -2), luaToHx(L, -1));
            lua_pop(L, 1);
        }
    }
    hx::Value result;
    std::string err;
    if (!ip.run(str(L, 1), S(L).name + " (runHaxeCode)", result, err)) {
        H(L).reportError(err);
        lua_pushnil(L);
        return 1;
    }
    if (!lua_isnoneornil(L, 3) && !str(L, 3).empty()) {
        if (!ip.call(str(L, 3), hxArgs(L, 4), result, err)) H(L).reportError(err);
    }
    pushHx(L, result);
    return 1;
}

int l_runHaxeFunction(lua_State* L) {
    hx::Interp& ip = hxOf(L);
    hx::Value result;
    std::string err;
    bool found = false;
    if (!ip.call(str(L, 1), hxArgs(L, 2), result, err, &found)) H(L).reportError(err);
    else if (!found) warn(L, "runHaxeFunction: no function " + str(L, 1));
    pushHx(L, result);
    return 1;
}

int l_hxCallback(lua_State* L) {
    const std::string name = lua_tostring(L, lua_upvalueindex(1));
    auto it = H(L).hxCallbacks.find(name);
    if (it == H(L).hxCallbacks.end() || !it->second.owner) { lua_pushnil(L); return 1; }
    std::vector<hx::Value> args;
    for (int i = 1; i <= lua_gettop(L); ++i) args.push_back(luaToHx(L, i));
    hx::Value r;
    try {
        r = it->second.owner->callValue(it->second.fn, args);
    } catch (std::exception& e) {
        H(L).reportError(name + ": " + e.what());
    } catch (...) {
        H(L).reportError(name + ": uncaught throw");
    }
    pushHx(L, r);
    return 1;
}

// ----------------------------------------------------------- script control --

int l_getRunningScripts(lua_State* L);
int l_callOnLuas(lua_State* L);
int l_callScript(lua_State* L);
int l_setOnLuas(lua_State* L);
int l_addLuaScript(lua_State* L);
int l_removeLuaScript(lua_State* L);
int l_isRunning(lua_State* L);
int l_getGlobalFromScript(lua_State* L);
int l_setGlobalFromScript(lua_State* L);

// --------------------------------------------------------- Kade modcharts --

std::string kadeActor(lua_State* L, int i) {
    if (lua_type(L, i) == LUA_TNUMBER) return strumPath(L, int(lua_tonumber(L, i)));
    const std::string id = str(L, i);
    if (!id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return std::isdigit((unsigned char)c); }))
        return strumPath(L, std::atoi(id.c_str()));
    if (id == "girlfriend") return "gf";
    return id;
}

int kadeSet(lua_State* L, const char* field) {
    SC(L).setProp(kadeActor(L, 2) + "." + field, num(L, 1));
    return 0;
}
int kadeGet(lua_State* L, const char* field) {
    Value v;
    if (SC(L).getProp(kadeActor(L, 1) + "." + field, v)) return ret(L, v);
    return retNum(L, 0);
}
int l_setActorX(lua_State* L) { return kadeSet(L, "x"); }
int l_setActorY(lua_State* L) { return kadeSet(L, "y"); }
int l_setActorAngle(lua_State* L) { return kadeSet(L, "angle"); }
int l_setActorAlpha(lua_State* L) { return kadeSet(L, "alpha"); }
int l_setActorScale(lua_State* L) {
    const std::string a = kadeActor(L, 2);
    SC(L).setProp(a + ".scale.x", num(L, 1, 1));
    SC(L).setProp(a + ".scale.y", num(L, 1, 1));
    return 0;
}
int l_setActorScaleXY(lua_State* L) {
    const std::string a = kadeActor(L, 3);
    SC(L).setProp(a + ".scale.x", num(L, 1, 1));
    SC(L).setProp(a + ".scale.y", num(L, 2, 1));
    return 0;
}
int l_setActorFlipX(lua_State* L) { SC(L).setProp(kadeActor(L, 2) + ".flipX", boolean(L, 1)); return 0; }
int l_getActorX(lua_State* L) { return kadeGet(L, "x"); }
int l_getActorY(lua_State* L) { return kadeGet(L, "y"); }
int l_getActorAngle(lua_State* L) { return kadeGet(L, "angle"); }
int l_getActorAlpha(lua_State* L) { return kadeGet(L, "alpha"); }
int l_getActorWidth(lua_State* L) { return kadeGet(L, "width"); }
int l_getActorHeight(lua_State* L) { return kadeGet(L, "height"); }
int l_setCamZoom(lua_State* L) { SC(L).camGame.zoom = float(num(L, 1, 1)); return 0; }
int l_setHudZoom(lua_State* L) { SC(L).camHud.zoom = float(num(L, 1, 1)); return 0; }
int l_setHudAngle(lua_State* L) { SC(L).camHud.angle = float(num(L, 1)); return 0; }
int l_setCamAngle(lua_State* L) { SC(L).camGame.angle = float(num(L, 1)); return 0; }
int l_getScreenWidth(lua_State* L) { return retNum(L, scene::kW); }
int l_getScreenHeight(lua_State* L) { return retNum(L, scene::kH); }

void kadeTween(lua_State* L, const std::string& target, std::vector<std::pair<const char*, double>> fields, double time,
               const std::string& onComplete, const char* ease) {
    if (target.empty()) return;
    scene::Tween tw;
    tw.target = target;
    for (auto& f : fields) tw.fields.push_back({f.first, 0, f.second, false});
    tw.dur = time;
    tw.ease = ease;
    tw.onComplete = onComplete;
    tw.script = S(L).index;
    tw.tag = "__kade" + std::to_string(SC(L).tweens.size()) + target + fields[0].first;
    SC(L).tween(std::move(tw));
}
int l_tweenPos(lua_State* L) { kadeTween(L, kadeActor(L, 1), {{"x", num(L, 2)}, {"y", num(L, 3)}}, num(L, 4, 1), str(L, 5), "linear"); return 0; }
int l_tweenPosXAngle(lua_State* L) { kadeTween(L, kadeActor(L, 1), {{"x", num(L, 2)}, {"angle", num(L, 3)}}, num(L, 4, 1), str(L, 5), "linear"); return 0; }
int l_tweenPosYAngle(lua_State* L) { kadeTween(L, kadeActor(L, 1), {{"y", num(L, 2)}, {"angle", num(L, 3)}}, num(L, 4, 1), str(L, 5), "linear"); return 0; }
int l_tweenAngle(lua_State* L) { kadeTween(L, kadeActor(L, 1), {{"angle", num(L, 2)}}, num(L, 3, 1), str(L, 4), "linear"); return 0; }
int l_tweenFadeIn(lua_State* L) { kadeTween(L, kadeActor(L, 1), {{"alpha", num(L, 2)}}, num(L, 3, 1), str(L, 4), "circin"); return 0; }
int l_tweenFadeOut(lua_State* L) { kadeTween(L, kadeActor(L, 1), {{"alpha", num(L, 2)}}, num(L, 3, 1), str(L, 4), "circout"); return 0; }
int l_tweenCameraZoom(lua_State* L) { kadeTween(L, "camGame", {{"zoom", num(L, 1, 1)}}, num(L, 2, 1), str(L, 3), "linear"); return 0; }
int l_tweenHudZoom(lua_State* L) { kadeTween(L, "camHUD", {{"zoom", num(L, 1, 1)}}, num(L, 2, 1), str(L, 3), "linear"); return 0; }
int l_tweenCameraAngle(lua_State* L) { kadeTween(L, "camGame", {{"angle", num(L, 1)}}, num(L, 2, 1), str(L, 3), "linear"); return 0; }
int l_tweenHudAngle(lua_State* L) { kadeTween(L, "camHUD", {{"angle", num(L, 1)}}, num(L, 2, 1), str(L, 3), "linear"); return 0; }
int l_tweenCameraPos(lua_State* L) {
    kadeTween(L, "camGame", {{"scroll.x", num(L, 1)}, {"scroll.y", num(L, 2)}}, num(L, 3, 1), str(L, 4), "linear");
    return 0;
}

int l_makeSprite(lua_State* L) {  // Kade: makeSprite(path, name, drawBehind)
    const std::string tag = cleanTag(str(L, 2));
    SpritePtr s = newSprite(L, tag);
    const std::string rel = H(L).songDataDir + str(L, 1);
    if (auto g = SC(L).gfx.image(rel)) s->setGraphic(g);
    if (boolean(L, 3, false)) SC(L).addBehindCharacters(s);
    else SC(L).addFront(s);
    lua_pushstring(L, tag.c_str());
    return 1;
}

// ------------------------------------------------------------- registration --

struct Fn { const char* name; lua_CFunction fn; };

const Fn kFunctions[] = {
    {"getProperty", l_getProperty}, {"setProperty", l_setProperty},
    {"getPropertyFromGroup", l_getPropertyFromGroup}, {"setPropertyFromGroup", l_setPropertyFromGroup},
    {"getPropertyFromClass", l_getPropertyFromClass}, {"setPropertyFromClass", l_setPropertyFromClass},
    {"getPropertyLuaSprite", [](lua_State* L) { lua_pushstring(L, (str(L, 1) + "." + str(L, 2)).c_str()); lua_replace(L, 1); return l_getProperty(L); }},
    {"setPropertyLuaSprite", [](lua_State* L) { SC(L).setProp(str(L, 1) + "." + str(L, 2), toValue(L, 3)); return 0; }},
    {"removeFromGroup", l_stubNil},
    {"makeLuaSprite", l_makeLuaSprite}, {"makeAnimatedLuaSprite", l_makeAnimatedLuaSprite},
    {"makeGraphic", l_makeGraphic}, {"luaSpriteMakeGraphic", l_makeGraphic},
    {"loadGraphic", l_loadGraphic}, {"loadFrames", l_loadFrames},
    {"addAnimationByPrefix", l_addAnimationByPrefix}, {"luaSpriteAddAnimationByPrefix", l_addAnimationByPrefix},
    {"addAnimationByIndices", l_addAnimationByIndices}, {"luaSpriteAddAnimationByIndices", l_addAnimationByIndices},
    {"addAnimationByIndicesLoop", l_addAnimationByIndicesLoop}, {"addAnimation", l_addAnimation},
    {"addOffset", l_addOffset}, {"playAnim", l_playAnim}, {"objectPlayAnimation", l_objectPlayAnimation},
    {"luaSpritePlayAnimation", l_objectPlayAnimation}, {"characterPlayAnim", l_characterPlayAnim},
    {"characterDance", l_characterDance},
    {"addLuaSprite", l_addLuaSprite}, {"removeLuaSprite", l_removeLuaSprite}, {"luaSpriteExists", l_luaSpriteExists},
    {"setScrollFactor", l_setScrollFactor}, {"setLuaSpriteScrollFactor", l_setScrollFactor},
    {"scaleObject", l_scaleObject}, {"scaleLuaSprite", l_scaleObject}, {"setGraphicSize", l_setGraphicSize},
    {"updateHitbox", l_updateHitbox}, {"screenCenter", l_screenCenter},
    {"setObjectCamera", l_setObjectCamera}, {"setLuaSpriteCamera", l_setObjectCamera},
    {"setObjectOrder", l_setObjectOrder}, {"getObjectOrder", l_getObjectOrder}, {"setBlendMode", l_setBlendMode},
    {"getMidpointX", l_getMidpointX}, {"getMidpointY", l_getMidpointY},
    {"getGraphicMidpointX", l_getGraphicMidpointX}, {"getGraphicMidpointY", l_getGraphicMidpointY},
    {"getScreenPositionX", l_getScreenPositionX}, {"getScreenPositionY", l_getScreenPositionY},
    {"getCharacterX", l_getCharacterX}, {"getCharacterY", l_getCharacterY},
    {"setCharacterX", l_setCharacterX}, {"setCharacterY", l_setCharacterY},
    {"precacheImage", l_precacheImage}, {"precacheSound", l_stubNil}, {"precacheMusic", l_stubNil},
    {"objectsOverlap", l_stubFalse},
    {"makeLuaText", l_makeLuaText}, {"setTextString", l_setTextString}, {"setTextSize", l_setTextSize},
    {"setTextWidth", l_setTextWidth}, {"setTextBorder", l_setTextBorder}, {"setTextColor", l_setTextColor},
    {"setTextFont", l_setTextFont}, {"setTextItalic", l_setTextItalic}, {"setTextAlignment", l_setTextAlignment},
    {"getTextString", l_getTextString}, {"getTextSize", l_getTextSize}, {"getTextFont", l_getTextFont},
    {"getTextWidth", l_getTextWidth}, {"addLuaText", l_addLuaText},
    {"removeLuaText", l_removeLuaSprite}, {"luaTextExists", l_luaTextExists},
    {"doTweenX", l_doTweenX}, {"doTweenY", l_doTweenY}, {"doTweenAngle", l_doTweenAngle},
    {"doTweenAlpha", l_doTweenAlpha}, {"doTweenZoom", l_doTweenZoom}, {"doTweenColor", l_doTweenColor},
    {"doTweenScaleX", l_doTweenScaleX}, {"doTweenScaleY", l_doTweenScaleY},
    {"noteTweenX", l_noteTweenX}, {"noteTweenY", l_noteTweenY}, {"noteTweenAngle", l_noteTweenAngle},
    {"noteTweenAlpha", l_noteTweenAlpha}, {"noteTweenDirection", l_noteTweenDirection},
    {"noteTweenScaleX", l_noteTweenScaleX}, {"noteTweenScaleY", l_noteTweenScaleY},
    {"startTween", l_startTween}, {"cancelTween", l_cancelTween},
    {"runTimer", l_runTimer}, {"cancelTimer", l_cancelTimer},
    {"cameraShake", l_cameraShake}, {"cameraFlash", l_cameraFlash}, {"cameraFade", l_cameraFade},
    {"cameraSetTarget", l_cameraSetTarget}, {"setCameraScroll", l_setCameraScroll},
    {"setCameraFollowPoint", l_setCameraFollowPoint}, {"addCameraScroll", l_addCameraScroll},
    {"addCameraFollowPoint", l_addCameraFollowPoint}, {"getCameraScrollX", l_getCameraScrollX},
    {"getCameraScrollY", l_getCameraScrollY}, {"getCameraFollowX", l_getCameraFollowX},
    {"getCameraFollowY", l_getCameraFollowY},
    {"setHealthBarColors", l_setHealthBarColors}, {"setTimeBarColors", l_setTimeBarColors},
    {"playSound", l_playSound}, {"playMusic", l_playMusic}, {"stopSound", l_stopSound}, {"pauseSound", l_pauseSound},
    {"resumeSound", l_resumeSound}, {"soundFadeIn", l_soundFadeIn}, {"soundFadeOut", l_soundFadeOut},
    {"soundFadeCancel", l_soundFadeCancel}, {"setSoundVolume", l_setSoundVolume}, {"getSoundVolume", l_getSoundVolume},
    {"setSoundTime", l_setSoundTime}, {"getSoundTime", l_getSoundTime}, {"luaSoundExists", l_luaSoundExists},
    {"musicFadeIn", l_musicFadeIn}, {"musicFadeOut", l_musicFadeOut},
    {"debugPrint", l_debugPrint}, {"close", l_close}, {"triggerEvent", l_triggerEvent},
    {"getSongPosition", l_getSongPosition}, {"getRandomInt", l_getRandomInt}, {"getRandomFloat", l_getRandomFloat},
    {"getRandomBool", l_getRandomBool}, {"getColorFromHex", l_getColorFromHex},
    {"getColorFromString", l_getColorFromString}, {"getColorFromName", l_getColorFromString},
    {"FlxColor", l_getColorFromString},
    {"stringStartsWith", l_stringStartsWith}, {"stringEndsWith", l_stringEndsWith}, {"stringSplit", l_stringSplit},
    {"stringTrim", l_stringTrim}, {"getTextFromFile", l_getTextFromFile}, {"checkFileExists", l_checkFileExists},
    {"setVar", l_setVar}, {"getVar", l_getVar}, {"removeVar", l_removeVar},
    {"keyJustPressed", l_keyJustPressed}, {"keyPressed", l_keyPressed}, {"keyReleased", l_keyReleased},
    {"keyboardJustPressed", l_keyboardJustPressed}, {"keyboardPressed", l_keyboardPressed},
    {"keyboardReleased", l_keyboardReleased}, {"mouseClicked", l_mouseClicked}, {"mousePressed", l_mousePressed},
    {"mouseReleased", l_mouseReleased}, {"getMouseX", l_getMouseX}, {"getMouseY", l_getMouseY},
    {"setHealth", l_setHealth}, {"addHealth", l_addHealth}, {"getHealth", l_getHealth},
    {"addScore", l_addScore}, {"setScore", l_setScore}, {"getScore", l_getScore},
    {"addMisses", l_addMisses}, {"setMisses", l_setMisses}, {"getMisses", l_getMisses},
    {"addHits", l_addHits}, {"setHits", l_setHits}, {"getHits", l_getHits},
    {"setRatingPercent", l_setRatingPercent}, {"setRatingName", l_setRatingName}, {"setRatingFC", l_setRatingFC},
    {"endSong", l_endSong}, {"restartSong", l_restartSong}, {"exitSong", l_exitSong},
    {"startCountdown", l_startCountdown}, {"startVideo", l_startVideo}, {"startDialogue", l_startDialogue},
    {"addCharacterToList", l_stubNil}, {"changeCharacter", l_changeCharacter},
    {"initSaveData", l_initSaveData}, {"flushSaveData", l_flushSaveData}, {"eraseSaveData", l_stubNil},
    {"setDataFromSave", l_setDataFromSave}, {"getDataFromSave", l_getDataFromSave},
    {"getRunningScripts", l_getRunningScripts}, {"callOnLuas", l_callOnLuas}, {"callOnScripts", l_callOnLuas},
    {"callScript", l_callScript}, {"setOnLuas", l_setOnLuas}, {"setOnScripts", l_setOnLuas},
    {"addLuaScript", l_addLuaScript}, {"removeLuaScript", l_removeLuaScript}, {"isRunning", l_isRunning},
    {"getGlobalFromScript", l_getGlobalFromScript}, {"setGlobalFromScript", l_setGlobalFromScript},
    // Discord presence has no meaning here; shaders are drawn by the renderer.
    {"initLuaShader", l_initLuaShader}, {"setSpriteShader", l_setSpriteShader}, {"removeSpriteShader", l_removeSpriteShader},
    {"setShaderFloat", l_setShaderFloat}, {"setShaderFloatArray", l_setShaderFloatArray}, {"setShaderInt", l_setShaderInt},
    {"setShaderIntArray", l_setShaderIntArray}, {"setShaderBool", l_setShaderBool}, {"setShaderBoolArray", l_setShaderIntArray},
    {"setShaderSampler2D", l_setShaderSampler2D}, {"getShaderFloat", l_getShaderFloat}, {"getShaderInt", l_getShaderInt},
    {"getShaderBool", l_getShaderBool}, {"getShaderFloatArray", l_getShaderFloatArray}, {"getShaderIntArray", l_getShaderIntArray},
    {"getShaderBoolArray", l_getShaderBoolArray}, {"createRuntimeShader", l_initLuaShader}, {"addGlitchEffect", l_stubNil},
    {"runHaxeCode", l_runHaxeCode}, {"runHaxeFunction", l_runHaxeFunction}, {"addHaxeLibrary", l_stubTrue},
    {"changePresence", l_stubNil}, {"changeDiscordPresence", l_stubNil}, {"changeDiscordClientID", l_stubNil},
    {"openCustomSubstate", l_stubNil}, {"closeCustomSubstate", l_stubNil}, {"insertToCustomSubstate", l_stubFalse},
    {"getModSetting", l_stubNil}, {"callMethod", l_callMethod}, {"callMethodFromClass", l_stubNil}, {"loadSong", l_stubNil}, {"setOnHScript", l_stubNil},
    // Kade Engine modcharts
    {"setActorX", l_setActorX}, {"setActorY", l_setActorY}, {"setActorAngle", l_setActorAngle},
    {"setActorAlpha", l_setActorAlpha}, {"setActorScale", l_setActorScale}, {"setActorScaleXY", l_setActorScaleXY},
    {"setActorFlipX", l_setActorFlipX}, {"getActorX", l_getActorX}, {"getActorY", l_getActorY},
    {"getActorAngle", l_getActorAngle}, {"getActorAlpha", l_getActorAlpha}, {"getActorWidth", l_getActorWidth},
    {"getActorHeight", l_getActorHeight}, {"setCamZoom", l_setCamZoom}, {"setHudZoom", l_setHudZoom},
    {"setHudAngle", l_setHudAngle}, {"setCamAngle", l_setCamAngle}, {"getScreenWidth", l_getScreenWidth},
    {"getScreenHeight", l_getScreenHeight}, {"tweenPos", l_tweenPos}, {"tweenPosXAngle", l_tweenPosXAngle},
    {"tweenPosYAngle", l_tweenPosYAngle}, {"tweenAngle", l_tweenAngle}, {"tweenFadeIn", l_tweenFadeIn},
    {"tweenFadeOut", l_tweenFadeOut}, {"tweenCameraZoom", l_tweenCameraZoom}, {"tweenHudZoom", l_tweenHudZoom},
    {"tweenCameraAngle", l_tweenCameraAngle}, {"tweenHudAngle", l_tweenHudAngle}, {"tweenCameraPos", l_tweenCameraPos},
    {"makeSprite", l_makeSprite},
};

void timeHook(lua_State* L, lua_Debug*) {
    ScriptHost::Script& s = S(L);
    if (s.callStart > 0 && nowSec() - s.callStart > s.budget) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "script took too long (over %.1f s), stopped", s.budget);
        luaL_error(L, "%s", msg);
    }
}

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "error", 1);
    return 1;
}

}  // namespace

// ---------------------------------------------------- script-control bodies --

namespace {
std::vector<Value> tableArgs(lua_State* L, int i) {
    std::vector<Value> out;
    if (!lua_istable(L, i)) return out;
    const lua_Integer n = luaL_len(L, i);
    for (lua_Integer k = 1; k <= n; ++k) {
        lua_geti(L, i, k);
        out.push_back(toValue(L, -1));
        lua_pop(L, 1);
    }
    return out;
}
}  // namespace

namespace {
bool callOne(ScriptHost::Script& s, const char* fn, const std::vector<Value>& args, Value* result, std::string* err);

int l_getRunningScripts(lua_State* L) {
    lua_newtable(L);
    lua_Integer k = 1;
    for (auto& s : H(L).scriptList())
        if (!s->closed) { lua_pushstring(L, s->path.c_str()); lua_rawseti(L, -2, k++); }
    return 1;
}

int l_callOnLuas(lua_State* L) {
    const std::string fn = str(L, 1);
    const std::vector<Value> args = tableArgs(L, 2);
    const bool ignoreSelf = boolean(L, 4, true);
    auto& list = H(L).scriptList();
    for (size_t i = 0; i < list.size(); ++i) {
        ScriptHost::Script& s = *list[i];
        if (s.closed || (ignoreSelf && &s == &S(L))) continue;
        callOne(s, fn.c_str(), args, nullptr, nullptr);
    }
    return retBool(L, true);
}

ScriptHost::Script* scriptByName(lua_State* L, const std::string& name) {
    for (auto& s : H(L).scriptList()) {
        if (s->closed) continue;
        if (s->path == name || endsWithI(s->path, name) || endsWithI(s->path, name + ".lua")) return s.get();
    }
    return nullptr;
}

int l_callScript(lua_State* L) {
    ScriptHost::Script* s = scriptByName(L, str(L, 1));
    if (!s) { lua_pushnil(L); return 1; }
    Value r;
    callOne(*s, str(L, 2).c_str(), tableArgs(L, 3), &r, nullptr);
    return ret(L, r);
}

int l_setOnLuas(lua_State* L) {
    const std::string name = str(L, 1);
    const Value v = toValue(L, 2);
    for (auto& s : H(L).scriptList()) {
        if (s->closed) continue;
        push(s->L, v);
        lua_setglobal(s->L, name.c_str());
    }
    return 0;
}

int l_addLuaScript(lua_State* L) {
    std::string name = str(L, 1);
    if (!endsWithI(name, ".lua")) name += ".lua";
    if (scriptByName(L, name) && !boolean(L, 2, false)) return 0;
    const std::string f = SC(L).files.find(name);
    if (f.empty()) { warn(L, "script not found: " + name); return 0; }
    H(L).load(f);
    return 0;
}

int l_removeLuaScript(lua_State* L) {
    if (ScriptHost::Script* s = scriptByName(L, str(L, 1))) s->closed = true;
    return retBool(L, true);
}

int l_isRunning(lua_State* L) { return retBool(L, scriptByName(L, str(L, 1)) != nullptr); }

int l_getGlobalFromScript(lua_State* L) {
    ScriptHost::Script* s = scriptByName(L, str(L, 1));
    if (!s) { lua_pushnil(L); return 1; }
    lua_getglobal(s->L, str(L, 2).c_str());
    const Value v = toValue(s->L, -1);
    lua_pop(s->L, 1);
    return ret(L, v);
}

int l_setGlobalFromScript(lua_State* L) {
    if (ScriptHost::Script* s = scriptByName(L, str(L, 1))) {
        push(s->L, toValue(L, 3));
        lua_setglobal(s->L, str(L, 2).c_str());
    }
    return 0;
}

void reportScriptError(ScriptHost::Script& s, const std::string& msg) {
    ScriptHost& h = *s.host;
    const double t = nowSec();
    if (msg == s.lastErr && t - s.lastErrAt < 2.0) {
        h.errors.push_back(msg);
        return;
    }
    s.lastErr = msg;
    s.lastErrAt = t;
    h.reportError(msg);
}

bool callOne(ScriptHost::Script& s, const char* fn, const std::vector<Value>& args, Value* result, std::string* err) {
    lua_State* L = s.L;
    if (!L || s.closed) return false;
    const int top = lua_gettop(L);
    lua_pushcfunction(L, traceback);
    if (lua_getglobal(L, fn) != LUA_TFUNCTION) {
        lua_settop(L, top);
        return false;
    }
    for (const Value& a : args) push(L, a);
    const double prevStart = s.callStart;
    const bool nested = prevStart > 0;
    if (!nested) s.callStart = nowSec();
    const int rc = lua_pcall(L, int(args.size()), 1, top + 1);
    if (!nested) s.callStart = 0;
    if (rc != LUA_OK) {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        const size_t tb = msg.find("\nstack traceback:");
        if (tb != std::string::npos) msg.resize(tb);
        msg = s.name + " (" + fn + "): " + msg;
        reportScriptError(s, msg);
        if (err) *err = msg;
        lua_settop(L, top);
        return false;
    }
    if (result) *result = toValue(L, -1);
    lua_settop(L, top);
    return true;
}
}  // namespace

// ----------------------------------------------------------------- ScriptHost --

ScriptHost::ScriptHost(scene::Scene& sc, ScriptGame& g) : scene(sc), game(g) {
    hxShared = std::make_shared<hx::Shared>();
    hxShared->report = [this](const std::string& m) { reportError(m); };
}
ScriptHost::~ScriptHost() {
    hxCallbacks.clear();
    hxScripts_.clear();
    scripts_.clear();
}

void ScriptHost::reportError(const std::string& msg) {
    errors.push_back(msg);
    game.debugPrint(msg, 0xFFFF4050);
}

bool ScriptHost::loadHx(const std::string& path, const std::string& code, bool callCreate) {
    auto s = std::make_unique<HxScript>();
    s->path = path;
    const size_t slash = path.find_last_of("/\\");
    s->name = slash == std::string::npos ? path : path.substr(slash + 1);
    s->ip = std::make_unique<hx::Interp>(scene, game, this, hxShared);
    s->ip->scriptName = s->name;
    s->ip->budget = loadBudget;
    HxScript* raw = s.get();
    hxScripts_.push_back(std::move(s));
    hx::Value result;
    std::string err;
    if (!raw->ip->run(code, raw->name, result, err)) {
        reportError(err);
        raw->closed = true;
        return false;
    }
    raw->ip->budget = callBudget;
    if (callCreate) {
        if (!raw->ip->call("onCreate", {}, result, err)) reportError(err);
    }
    return true;
}

void ScriptHost::registerHxCallback(const std::string& name, hx::Interp* owner, const hx::Value& fn) {
    hxCallbacks[name] = {owner, fn};
    for (auto& s : scripts_) {
        if (s->closed || !s->L) continue;
        lua_pushstring(s->L, name.c_str());
        lua_pushcclosure(s->L, l_hxCallback, 1);
        lua_setglobal(s->L, name.c_str());
    }
}

size_t ScriptHost::count() const {
    size_t n = 0;
    for (const auto& s : scripts_)
        if (!s->closed) ++n;
    return n;
}

bool ScriptHost::isLoaded(const std::string& path) const {
    for (const auto& s : scripts_)
        if (s->path == path && !s->closed) return true;
    for (const auto& s : hxScripts_)
        if (s->path == path && !s->closed) return true;
    return false;
}

bool ScriptHost::load(const std::string& path, bool callCreate) {
    std::string code;
    if (!scene::readFile(path, code)) {
        errors.push_back("cannot read " + path);
        return false;
    }
    if (endsWithI(path, ".hx") || endsWithI(path, ".hscript") || endsWithI(path, ".hxs")) return loadHx(path, code, callCreate);
    return loadString(path, code, callCreate);
}

bool ScriptHost::loadString(const std::string& path, const std::string& code, bool callCreate) {
    auto s = std::make_unique<Script>();
    s->host = this;
    s->path = path;
    const size_t slash = path.find_last_of("/\\");
    s->name = slash == std::string::npos ? path : path.substr(slash + 1);
    s->index = int(scripts_.size());
    s->L = luaL_newstate();
    if (!s->L) return false;
    lua_State* L = s->L;
    *static_cast<Script**>(lua_getextraspace(L)) = s.get();

    // Sandbox: no io, os, package, debug; a tiny os table for clocks.
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(L, 0);
    for (const char* banned : {"dofile", "loadfile", "require"}) {
        lua_pushnil(L);
        lua_setglobal(L, banned);
    }
    lua_newtable(L);
    lua_pushcfunction(L, [](lua_State* L2) { lua_pushnumber(L2, lua_Number(std::clock()) / CLOCKS_PER_SEC); return 1; });
    lua_setfield(L, -2, "clock");
    lua_pushcfunction(L, [](lua_State* L2) { lua_pushinteger(L2, lua_Integer(std::time(nullptr))); return 1; });
    lua_setfield(L, -2, "time");
    lua_pushcfunction(L, [](lua_State* L2) {
        const std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof buf, luaL_optstring(L2, 1, "%c"), std::localtime(&t));
        lua_pushstring(L2, buf);
        return 1;
    });
    lua_setfield(L, -2, "date");
    lua_setglobal(L, "os");

    for (const Fn& f : kFunctions) {
        lua_pushcfunction(L, f.fn);
        lua_setglobal(L, f.name);
    }
    luaL_dostring(L, kCompat);
    lua_settop(L, 0);

    auto setStr = [&](const char* n, const char* v) { lua_pushstring(L, v); lua_setglobal(L, n); };
    setStr("Function_Stop", kStop);
    setStr("Function_Continue", kContinue);
    setStr("Function_StopLua", kStopLua);
    setStr("Function_StopHScript", "##PSYCHLUA_FUNCTIONSTOPHSCRIPT");
    setStr("Function_StopAll", "##PSYCHLUA_FUNCTIONSTOPALL");
    setStr("scriptName", path.c_str());
    for (const auto& kv : globals_) {
        push(L, kv.second);
        lua_setglobal(L, kv.first.c_str());
    }
    lua_sethook(L, timeHook, LUA_MASKCOUNT, 20000);

    Script* raw = s.get();
    scripts_.push_back(std::move(s));
    if (path.find("modchart") != std::string::npos) kadeMode = true;

    raw->budget = loadBudget;
    raw->callStart = nowSec();
    lua_pushcfunction(L, traceback);
    const std::string chunk = "@" + raw->name;
    int rc = luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t");
    if (rc == LUA_OK) rc = lua_pcall(L, 0, 0, 1);
    raw->callStart = 0;
    if (rc != LUA_OK) {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        const size_t tb = msg.find("\nstack traceback:");
        if (tb != std::string::npos) msg.resize(tb);
        reportScriptError(*raw, raw->name + ": " + msg);
        raw->closed = true;
        lua_settop(L, 0);
        return false;
    }
    lua_settop(L, 0);
    if (callCreate) callOne(*raw, "onCreate", {}, nullptr, nullptr);
    if (kadeMode && callCreate) callOne(*raw, "start", {Value(std::string())}, nullptr, nullptr);
    raw->budget = callBudget;
    return true;
}

std::string ScriptHost::call(const char* fn, const std::vector<Value>& args) {
    std::string result;
    if (!std::strcmp(fn, "onUpdate") && !args.empty()) hxShared->update(scene::toNum(args[0]));
    const size_t n = scripts_.size();
    for (size_t i = 0; i < n && i < scripts_.size(); ++i) {
        Script& s = *scripts_[i];
        if (s.closed) continue;
        Value r;
        if (!callOne(s, fn, args, &r, nullptr)) continue;
        if (const std::string* rs = std::get_if<std::string>(&r)) {
            if (*rs == kStopLua) break;
            if (*rs != kContinue && !rs->empty()) result = *rs;
        }
    }
    if (hxScripts_.empty()) return result;
    // HScript gets note objects where Lua gets (id, direction, type, sustain).
    const bool noteCb = !std::strcmp(fn, "goodNoteHit") || !std::strcmp(fn, "opponentNoteHit") || !std::strcmp(fn, "noteMiss") ||
                        !std::strcmp(fn, "goodNoteHitPre") || !std::strcmp(fn, "opponentNoteHitPre") || !std::strcmp(fn, "noteMissPre");
    std::vector<hx::Value> hargs;
    if (noteCb && args.size() >= 3) {
        auto h = std::make_shared<hx::Host>();
        h->kind = "noteobj";
        h->extra = hx::Value::object({{"ID", hx::Interp::fromScene(args[0])},
                                      {"noteData", hx::Interp::fromScene(args[1])},
                                      {"noteType", hx::Interp::fromScene(args[2])},
                                      {"isSustainNote", args.size() > 3 ? hx::Interp::fromScene(args[3]) : hx::Value(false)},
                                      {"mustPress", hx::Value(std::strcmp(fn, "opponentNoteHit") != 0 && std::strcmp(fn, "opponentNoteHitPre") != 0)},
                                      {"strumTime", hx::Value(game.songPosition())},
                                      {"hitByOpponent", hx::Value(!std::strcmp(fn, "opponentNoteHit"))}});
        hargs.push_back(hx::Value::hostOf(h));
    } else {
        for (const Value& a : args) hargs.push_back(hx::Interp::fromScene(a));
    }
    for (size_t i = 0; i < hxScripts_.size(); ++i) {
        HxScript& s = *hxScripts_[i];
        if (s.closed || !s.ip->hasFunction(fn)) continue;
        hx::Value r;
        std::string err;
        if (!s.ip->call(fn, hargs, r, err)) {
            reportError(err);
            continue;
        }
        if (r.t == hx::T::Str) {
            if (r.s == kStopLua) break;
            if (r.s != kContinue && !r.s.empty()) result = r.s;
        }
    }
    return result;
}

void ScriptHost::setGlobal(const char* name, const Value& v) {
    bool found = false;
    for (auto& kv : globals_)
        if (kv.first == name) { kv.second = v; found = true; break; }
    if (!found) globals_.emplace_back(name, v);
    for (auto& s : scripts_) {
        if (s->closed) continue;
        push(s->L, v);
        lua_setglobal(s->L, name);
    }
}

void ScriptHost::dispatchCompletions() {
    for (int guard = 0; guard < 8 && !scene.completions.empty(); ++guard) {
        std::vector<scene::Completion> list;
        list.swap(scene.completions);
        for (const scene::Completion& c : list) {
            if (c.tag.rfind("__hx", 0) == 0) {  // HScript tween/timer callbacks
                hxShared->complete(c.tag, c.timer ? c.left : 0);
                continue;
            }
            if (c.timer) {
                call("onTimerCompleted", {Value(c.tag), Value(double(c.loops)), Value(double(c.left))});
            } else if (!c.fn.empty()) {
                call(c.fn.c_str(), {Value(c.tag)});
            } else if (c.tag.rfind("__kade", 0) != 0) {
                call("onTweenCompleted", {Value(c.tag)});
            }
        }
    }
}

}  // namespace gh
