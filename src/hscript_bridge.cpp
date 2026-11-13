// HScript bridge: what `game`, FlxG, FlxSprite, FlxTween... mean in this
// engine. Objects are handles (Host) on scene paths, so `game.boyfriend.x`
// reads the same property Lua's getProperty('boyfriend.x') does.
#include <algorithm>
#include <cmath>
#include <random>

#include "common.hpp"
#include "hscript.hpp"
#include "lua_host.hpp"
#include "psych.hpp"

namespace gh::hx {

namespace {

std::mt19937& rng() {
    static std::mt19937 r(std::random_device{}());
    return r;
}

std::shared_ptr<Host> mk(std::string kind, std::string path = {}, int index = -1) {
    auto h = std::make_shared<Host>();
    h->kind = std::move(kind);
    h->path = std::move(path);
    h->index = index;
    return h;
}
Value hv(std::string kind, std::string path = {}, int index = -1) { return Value::hostOf(mk(std::move(kind), std::move(path), index)); }

Value arg(std::vector<Value>& a, size_t i) { return i < a.size() ? a[i] : Value(); }

std::string newTag(Interp& ip, const char* prefix) { return std::string(prefix) + std::to_string(++ip.shared->nextId); }

scene::Sprite* spriteOf(Interp& ip, const Host& h) {
    if (h.sprite) return h.sprite.get();
    return ip.sc.find(h.path);
}

// Path of an object in scene property terms ("boyfriend", "camGame", "tag.scale").
std::string pathOf(const Value& v) {
    if (v.t != T::Host || !v.host) return "";
    return v.host->path;
}

Value sceneProp(Interp& ip, const std::string& path) {
    scene::Value out;
    if (ip.sc.getProp(path, out)) return Interp::fromScene(out);
    if (ip.game.getGameProp(path, out)) return Interp::fromScene(out);
    return Value();
}

bool hasSceneProp(Interp& ip, const std::string& path, Value& v) {
    scene::Value out;
    if (ip.sc.getProp(path, out) || ip.game.getGameProp(path, out)) {
        v = Interp::fromScene(out);
        return true;
    }
    return false;
}

void setSceneProp(Interp& ip, const std::string& path, const Value& v) {
    const scene::Value sv = Interp::toScene(v);
    if (!ip.game.setGameProp(path, sv)) ip.sc.setProp(path, sv);
}

Value objectHost(Interp& ip, const std::string& name) {
    const std::string l = toLower(name);
    if (l == "camgame" || l == "camhud" || l == "camother") return hv("camera", name);
    if (ip.sc.find(name)) return hv("sprite", name);
    return Value();
}

uint32_t colorOf(const Value& v) {
    if (v.t == T::Str) return psych::parseColor(v.s);
    return uint32_t(int64_t(v.num()));
}

scene::Cam camOf(const Value& v) {
    if (v.t == T::Array && !v.arr->empty()) return camOf((*v.arr)[0]);
    return scene::Scene::camFromName(pathOf(v).empty() ? "game" : pathOf(v));
}

void addToScene(Interp& ip, const Value& obj, int where, int pos = 0) {
    scene::Sprite* s = obj.t == T::Host && obj.host ? spriteOf(ip, *obj.host) : nullptr;
    if (!s) return;
    scene::SpritePtr sp;
    for (auto& kv : ip.sc.objects)
        if (kv.second.get() == s) sp = kv.second;
    if (!sp) return;
    scene::Scene& sc = ip.sc;
    if (where == 0) { sc.addFront(sp); return; }
    sc.remove(sp);
    if (where == 1) {  // insert(pos, obj)
        pos = clampv(pos, 0, int(sc.order.size()));
        sc.order.insert(sc.order.begin() + pos, sp);
    } else {  // behind gf / bf / dad
        const scene::Sprite* anchor = where == 2 ? sc.gfMarker.get() : where == 3 ? static_cast<scene::Sprite*>(sc.bf.get())
                                                                                   : static_cast<scene::Sprite*>(sc.dad.get());
        size_t at = sc.order.size();
        for (size_t i = 0; i < sc.order.size(); ++i)
            if (sc.order[i].get() == anchor) { at = i; break; }
        sc.order.insert(sc.order.begin() + std::ptrdiff_t(at), sp);
    }
    sp->inScene = true;
}

void removeFromScene(Interp& ip, const Value& obj) {
    scene::Sprite* s = obj.t == T::Host && obj.host ? spriteOf(ip, *obj.host) : nullptr;
    if (!s) return;
    for (auto& kv : ip.sc.objects)
        if (kv.second.get() == s) { ip.sc.remove(kv.second); return; }
}

scene::ShaderPtr shaderFromSource(Interp& ip, const std::string& name) {
    scene::Scene& sc = ip.sc;
    if (!sc.shaderSources.count(name)) {
        scene::ShaderSource src;
        const std::string frag = sc.files.find("shaders/" + name + ".frag");
        const std::string vert = sc.files.find("shaders/" + name + ".vert");
        if (frag.empty() && vert.empty()) {
            ip.game.debugNotice("shader not found: " + name, false, false);
            return nullptr;
        }
        if (!frag.empty()) scene::readFile(frag, src.frag);
        if (!vert.empty()) scene::readFile(vert, src.vert);
        sc.shaderSources[name] = std::move(src);
    }
    auto inst = std::make_shared<scene::ShaderInstance>();
    inst->name = name;
    return inst;
}

Value shaderHost(scene::ShaderPtr s) {
    if (!s) return Value();
    auto h = mk("shader", s->name);
    h->shader = std::move(s);
    return Value::hostOf(h);
}

// Psych's runtimeShaders: name -> [fragment, vertex] of every initLuaShader'd shader.
Value runtimeShaderMap(Interp& ip) {
    Value m = Value::mapOf();
    for (const auto& [name, src] : ip.sc.shaderSources)
        if (name.rfind("__", 0) != 0) (*m.map)[name] = Value::array({Value(src.frag), Value(src.vert)});
    return m;
}

std::vector<float> floatsOf(const Value& v) {
    std::vector<float> out;
    if (v.t == T::Array)
        for (const Value& x : *v.arr) out.push_back(float(x.num()));
    else if (!v.isNull())
        out.push_back(float(v.num()));
    return out;
}

// FlxTween.tween / color / angle: a scene tween with an optional HScript callback.
Value startTween(Interp& ip, const std::string& target, std::vector<std::pair<std::string, double>> fields, double dur,
                 const Value& opts, bool isColor = false, uint32_t c0 = 0, uint32_t c1 = 0) {
    scene::Tween tw;
    tw.tag = newTag(ip, "__hxtw");
    tw.target = target;
    for (auto& f : fields) tw.fields.push_back({f.first, 0, f.second, false});
    tw.isColor = isColor;
    tw.c0 = c0 & 0xFFFFFF;
    tw.c1 = c1 & 0xFFFFFF;
    tw.dur = std::max(1e-6, dur);
    Value onComplete;
    if (opts.t == T::Object || opts.t == T::Map) {
        auto& m = *opts.map;
        if (auto it = m.find("ease"); it != m.end() && it->second.t == T::Str) tw.ease = it->second.s;
        if (auto it = m.find("startDelay"); it != m.end()) tw.delay = it->second.num();
        if (auto it = m.find("onComplete"); it != m.end()) onComplete = it->second;
    }
    Value handle = hv("tween", tw.tag);
    if (onComplete.t == T::Func) ip.shared->callbacks[tw.tag] = {onComplete, handle, &ip};
    ip.sc.tween(std::move(tw));
    return handle;
}

Value mathCall(const std::string& m, std::vector<Value>& a, bool& ok) {
    ok = true;
    const double x = arg(a, 0).num(), y = arg(a, 1).num();
    if (m == "abs") return Value(std::fabs(x));
    if (m == "floor" || m == "ffloor") return Value(std::floor(x));
    if (m == "ceil" || m == "fceil") return Value(std::ceil(x));
    if (m == "round" || m == "fround") return Value(std::floor(x + 0.5));
    if (m == "sqrt") return Value(std::sqrt(x));
    if (m == "pow") return Value(std::pow(x, y));
    if (m == "sin") return Value(std::sin(x));
    if (m == "cos") return Value(std::cos(x));
    if (m == "tan") return Value(std::tan(x));
    if (m == "asin") return Value(std::asin(x));
    if (m == "acos") return Value(std::acos(x));
    if (m == "atan") return Value(std::atan(x));
    if (m == "atan2") return Value(std::atan2(x, y));
    if (m == "exp") return Value(std::exp(x));
    if (m == "log") return Value(std::log(x));
    if (m == "min") return Value(std::min(x, y));
    if (m == "max") return Value(std::max(x, y));
    if (m == "random") return Value(std::uniform_real_distribution<double>(0, 1)(rng()));
    if (m == "isNaN") return Value(std::isnan(x));
    if (m == "isFinite") return Value(std::isfinite(x));
    ok = false;
    return Value();
}

}  // namespace

// ------------------------------------------------------------- globals ----

Value globalLookup(Interp& ip, const std::string& name, bool& found) {
    found = true;
    if (name == "game" || name == "this") return hv("game");
    if (name == "Function_Stop") return Value(ScriptHost::kStop);
    if (name == "Function_Continue") return Value(ScriptHost::kContinue);
    if (name == "Function_StopLua" || name == "Function_StopHScript" || name == "Function_StopAll")
        return Value(ScriptHost::kStopLua);
    if (name == "setVar")
        return Value::native(name, [&ip](std::vector<Value>& a) {
            const std::string k = arg(a, 0).str();
            ip.shared->vars[k] = arg(a, 1);
            const scene::Value sv = Interp::toScene(arg(a, 1));
            if (!std::holds_alternative<std::monostate>(sv)) ip.sc.vars[k] = sv;
            return arg(a, 1);
        });
    if (name == "getVar")
        return Value::native(name, [&ip](std::vector<Value>& a) {
            const std::string k = arg(a, 0).str();
            if (auto it = ip.shared->vars.find(k); it != ip.shared->vars.end()) return it->second;
            if (auto it = ip.sc.vars.find(k); it != ip.sc.vars.end()) return Interp::fromScene(it->second);
            return Value();
        });
    if (name == "removeVar")
        return Value::native(name, [&ip](std::vector<Value>& a) {
            const std::string k = arg(a, 0).str();
            const bool had = ip.shared->vars.erase(k) > 0;
            return Value(ip.sc.vars.erase(k) > 0 || had);
        });
    if (name == "debugPrint" || name == "trace")
        return Value::native(name, [&ip](std::vector<Value>& a) {
            std::string s;
            for (size_t i = 0; i < a.size(); ++i) s += (i ? ", " : "") + a[i].str();
            ip.game.debugPrint(s, 0xFFFFFFFF);
            return Value();
        });
    if (name == "add") return Value::native(name, [&ip](std::vector<Value>& a) { addToScene(ip, arg(a, 0), 0); return arg(a, 0); });
    if (name == "insert")
        return Value::native(name, [&ip](std::vector<Value>& a) { addToScene(ip, arg(a, 1), 1, int(arg(a, 0).num())); return arg(a, 1); });
    if (name == "remove") return Value::native(name, [&ip](std::vector<Value>& a) { removeFromScene(ip, arg(a, 0)); return arg(a, 0); });
    if (name == "addBehindGF" || name == "addBehindBF" || name == "addBehindDad") {
        const int w = name == "addBehindGF" ? 2 : name == "addBehindBF" ? 3 : 4;
        return Value::native(name, [&ip, w](std::vector<Value>& a) { addToScene(ip, arg(a, 0), w); return arg(a, 0); });
    }
    if (name == "getLuaObject")
        return Value::native(name, [&ip](std::vector<Value>& a) { return objectHost(ip, arg(a, 0).str()); });
    if (name == "initLuaShader" || name == "createRuntimeShader")
        return Value::native(name, [&ip, name](std::vector<Value>& a) {
            scene::ShaderPtr s = shaderFromSource(ip, arg(a, 0).str());
            if (name == "initLuaShader") return Value(s != nullptr);
            return shaderHost(s);
        });
    if (name == "createCallback" || name == "createGlobalCallback")
        return Value::native(name, [&ip](std::vector<Value>& a) {
            if (ip.host && arg(a, 1).t == T::Func) ip.host->registerHxCallback(arg(a, 0).str(), &ip, arg(a, 1));
            return Value();
        });
    if (name == "parentLua") {  // Psych 0.7: the Lua script running this code
        if (ip.luaPath.empty()) return Value();
        return Value::object({{"scriptName", Value(ip.luaPath)}, {"runtimeShaders", runtimeShaderMap(ip)}});
    }
    if (name == "customSubstate" || name == "customSubstateName") return Value();
    // Fully qualified names: openfl.Lib.application.window, flixel.FlxG...
    if (name == "openfl" || name == "flixel" || name == "lime" || name == "haxe" || name == "sys" || name == "states" ||
        name == "backend" || name == "objects" || name == "psychlua" || name == "funkin")
        return hv("package", name);
    if (name == "buildTarget") return Value(std::string("desktop"));
    if (name == "Math" || name == "Std" || name == "StringTools" || name == "Reflect" || name == "FlxG" ||
        name == "FlxTween" || name == "FlxEase" || name == "FlxColor" || name == "Paths" || name == "PlayState" ||
        name == "Conductor" || name == "ClientPrefs" || name == "FlxMath" || name == "Type")
        return hv("class", name);
    // Any other class name (FlxSprite, FlxText, FlxTimer, ShaderFilter...)
    // resolves to a class handle so `new` and imports work.
    if (!name.empty() && std::isupper((unsigned char)name[0])) return hv("class", name);
    found = false;
    return Value();
}

// ------------------------------------------------------------ fields -----

Value hostGet(Interp& ip, const Host& h, const std::string& f) {
    const std::string& k = h.kind;
    if (k == "package") {  // a.b.C: lower-case parts are packages, the first capitalised one the class
        if (!f.empty() && std::isupper((unsigned char)f[0])) return hv("class", f);
        return hv("package", h.path + "." + f);
    }
    if (k == "classpath") {  // openfl.Lib.application.window..., lime Application.current.window...
        Value v;
        if (hasSceneProp(ip, "class:" + h.path + "." + f, v)) return v;
        return hv("classpath", h.path + "." + f);
    }
    if (k == "video") {
        if (f == "onEndReached" || f == "onVideoEnd" || f == "onEnd" || f == "finished") return hv("videosignal", h.path);
        if (f == "bitmap") return hv("video", h.path);
        if (f == "alpha" || f == "visible" || f == "x" || f == "y") return sceneProp(ip, h.path + "." + f);
        return Value();
    }
    if (k == "signal" || k == "videosignal") return Value();
    if (k == "filterlist") {
        scene::Camera* c = ip.sc.camera(h.path);
        if (f == "length") return Value(double(c ? c->filters.size() : 0));
        return Value();
    }
    if (k == "cameras") {
        if (f == "list" || f == "defaults")
            return Value::array({hv("camera", "camGame"), hv("camera", "camHUD"), hv("camera", "camOther")});
        if (f == "cameraAdded" || f == "cameraRemoved" || f == "cameraResized") return hv("signal", f);
        return Value();
    }
    if (k == "game") {
        if (f == "camGame" || f == "camHUD" || f == "camOther") return hv("camera", f);
        if (f == "luaArray") {
            std::vector<Value> list;
            if (ip.host)
                for (const std::string& path : ip.host->scriptPaths())
                    list.push_back(Value::object({{"scriptName", Value(path)}, {"runtimeShaders", runtimeShaderMap(ip)}}));
            return Value::array(list);
        }
        if (f == "runtimeShaders") return runtimeShaderMap(ip);
        if (f == "singAnimations")
            return Value::array({Value(std::string("singLEFT")), Value(std::string("singDOWN")), Value(std::string("singUP")),
                                 Value(std::string("singRIGHT"))});
        if (f == "variables") return hv("vars");
        if (f == "modchartSprites" || f == "modchartTexts") return hv("luaobjects");
        if (f == "strumLineNotes" || f == "playerStrums" || f == "opponentStrums") return hv("group", f);
        if (f == "notes" || f == "unspawnNotes") return hv("notegroup", f);
        if (f == "camFollow" || f == "camFollowPos") return hv("point", "camFollow");
        scene::Value out;
        if (ip.game.getGameProp(f, out)) return Interp::fromScene(out);
        if (ip.sc.find(f)) return hv("sprite", f);
        if (f == "boyfriendGroup" || f == "dadGroup" || f == "gfGroup") return hv("sprite", f);
        if (ip.sc.getProp(f, out)) return Interp::fromScene(out);
        return Value();
    }
    if (k == "sprite") {
        scene::Sprite* s = spriteOf(ip, h);
        if (f == "animation") { auto a = mk("anim", h.path); a->sprite = h.sprite; return Value::hostOf(a); }
        if (f == "scale" || f == "scrollFactor" || f == "offset" || f == "origin") return hv("point", h.path + "." + f);
        if (f == "shader") return s && s->shader ? shaderHost(s->shader) : Value();
        if (f == "camera") return s ? hv("camera", s->cam == scene::Cam::Hud ? "camHUD" : s->cam == scene::Cam::Other ? "camOther" : "camGame") : Value();
        if (f == "cameras") return s ? Value::array({hostGet(ip, h, "camera")}) : Value();
        if (f == "text" && s) return Value(s->text.text);
        if (f == "alive" || f == "exists") return Value(s != nullptr && s->active);
        if (f == "frames" || f == "graphic") return Value();
        return sceneProp(ip, h.path + "." + f);
    }
    if (k == "point") {
        if (h.path == "camFollow") return Value(double(f == "x" ? ip.sc.followX : ip.sc.followY));
        return sceneProp(ip, h.path + "." + f);
    }
    if (k == "anim") {
        if (f == "curAnim") return hv("curanim", h.path);
        if (f == "name" || f == "finished" || f == "paused") return sceneProp(ip, h.path + ".animation." + f);
        if (f == "frameIndex" || f == "curFrame") return sceneProp(ip, h.path + ".animation.curAnim.curFrame");
        return Value();
    }
    if (k == "curanim") return sceneProp(ip, h.path + ".animation.curAnim." + f);
    if (k == "camera") {
        if (f == "scroll") return hv("point", h.path + ".scroll");
        if (f == "filters" || f == "_filters") return hv("filterlist", h.path);  // live: push/remove change the camera
        return sceneProp(ip, h.path + "." + f);
    }
    if (k == "group") {
        if (f == "members") return hv("members", h.path);
        return sceneProp(ip, h.path + "." + f);
    }
    if (k == "members") return sceneProp(ip, h.path + "." + f);
    if (k == "path") {
        Value v;
        if (hasSceneProp(ip, h.path + "." + f, v)) return v;
        return hv("path", h.path + "." + f);
    }
    if (k == "notegroup" || k == "notemembers") {
        if (f == "length") return Value(double(ip.game.noteCount(h.path)));
        if (f == "members") return hv("notemembers", h.path);
        return Value();
    }
    if (k == "note") {
        scene::Value out;
        if (ip.game.noteGet(h.path, h.index, f, out)) return Interp::fromScene(out);
        return Value();
    }
    if (k == "noteobj") {
        if (h.extra.t == T::Object)
            if (auto it = h.extra.map->find(f); it != h.extra.map->end()) return it->second;
        return Value();
    }
    if (k == "shader") {
        if (f == "shader") return Value::hostOf(std::make_shared<Host>(h));  // ShaderFilter.shader
        if (f == "data") {
            auto d = mk("shaderdata", h.path);
            d->shader = h.shader;
            return Value::hostOf(d);
        }
        return Value();
    }
    if (k == "shaderdata") {
        auto p = mk("shaderparam", f);
        p->shader = h.shader;
        return Value::hostOf(p);
    }
    if (k == "shaderparam") {
        if (f != "value" || !h.shader) return Value();
        if (auto it = h.shader->floats.find(h.path); it != h.shader->floats.end()) {
            std::vector<Value> v;
            for (float x : it->second) v.push_back(Value(double(x)));
            return Value::array(v);
        }
        if (auto it = h.shader->ints.find(h.path); it != h.shader->ints.end()) {
            std::vector<Value> v;
            for (int x : it->second) v.push_back(Value(double(x)));
            return Value::array(v);
        }
        return Value();
    }
    if (k == "class") {
        const std::string& c = h.path;
        if (c == "FlxG") {
            if (f == "width") return Value(double(scene::kW));
            if (f == "height") return Value(double(scene::kH));
            if (f == "camera") return hv("camera", "camGame");
            if (f == "sound" || f == "random" || f == "keys" || f == "mouse" || f == "save" || f == "cameras") return hv(f);
            if (f == "timeScale") return Value(1.0);
            if (f == "stage") return hv("signal", "stage");
            if (f == "game") return hv("classpath", "FlxG.game");
            return sceneProp(ip, "class:FlxG." + f);
        }
        if (c == "FlxEase") return Value(f);
        if (c == "FlxColor") {
            static const std::pair<const char*, uint32_t> colors[] = {
                {"TRANSPARENT", 0x00000000}, {"WHITE", 0xFFFFFFFF}, {"GRAY", 0xFF808080}, {"BLACK", 0xFF000000},
                {"GREEN", 0xFF008000},       {"LIME", 0xFF00FF00},  {"YELLOW", 0xFFFFFF00}, {"ORANGE", 0xFFFFA500},
                {"RED", 0xFFFF0000},         {"PURPLE", 0xFF800080}, {"BLUE", 0xFF0000FF}, {"BROWN", 0xFF8B4513},
                {"PINK", 0xFFFFC0CB},        {"MAGENTA", 0xFFFF00FF}, {"CYAN", 0xFF00FFFF}};
            for (auto& [n, v] : colors)
                if (f == n) return Value(double(int32_t(v)));
            return Value();
        }
        if (c == "Math") {
            if (f == "PI") return Value(3.14159265358979323846);
            if (f == "NaN") return Value(std::nan(""));
            if (f == "POSITIVE_INFINITY") return Value(HUGE_VAL);
            if (f == "NEGATIVE_INFINITY") return Value(-HUGE_VAL);
            return Value();
        }
        if (c == "PlayState") {
            if (f == "instance") return hv("game");
            if (f == "SONG") return hv("song");
            return sceneProp(ip, "class:PlayState." + f);
        }
        if (c == "ClientPrefs" && f == "data") return hv("class", "ClientPrefs");
        if ((c == "Lib" && (f == "application" || f == "current")) || (c == "Application" && f == "current"))
            return hv("classpath", c + "." + f);
        if (c == "Conductor" || c == "ClientPrefs") return sceneProp(ip, "class:" + c + "." + f);
        return Value();
    }
    if (k == "song") return sceneProp(ip, "class:PlayState.SONG." + f);
    if (k == "sound") {
        if (f == "music") return hv("music");
        if (f == "volume") return Value(1.0);
        return Value();
    }
    if (k == "music") return sceneProp(ip, "class:FlxG.sound.music." + f);
    if (k == "keys") {
        if (f == "pressed") return hv("keystate", "", 0);
        if (f == "justPressed") return hv("keystate", "", 1);
        if (f == "justReleased") return hv("keystate", "", 2);
        return Value();
    }
    if (k == "keystate") return Value(ip.game.keyboardState(f, h.index));
    if (k == "mouse") {
        if (f == "x" || f == "screenX") return Value(double(ip.game.mouseX()));
        if (f == "y" || f == "screenY") return Value(double(ip.game.mouseY()));
        if (f == "pressed") return Value(ip.game.mouseState("left", 0));
        if (f == "justPressed") return Value(ip.game.mouseState("left", 1));
        if (f == "justReleased") return Value(ip.game.mouseState("left", 2));
        if (f == "pressedRight") return Value(ip.game.mouseState("right", 0));
        if (f == "justPressedRight") return Value(ip.game.mouseState("right", 1));
        if (f == "visible") return Value(true);
        return Value();
    }
    if (k == "save") {
        if (f == "data") return hv("savedata");
        return Value();
    }
    if (k == "savedata") {
        if (auto it = ip.sc.vars.find("save:hx." + f); it != ip.sc.vars.end()) return Interp::fromScene(it->second);
        if (auto it = ip.shared->vars.find("save:hx." + f); it != ip.shared->vars.end()) return it->second;
        return Value();
    }
    if (k == "timer" || k == "tween") {
        if (f == "active") return Value(true);
        if (f == "finished") return Value(false);
        if (f == "loopsLeft" || f == "elapsedLoops") return Value(h.extra.num());
        return Value();
    }
    return Value();
}

void hostSet(Interp& ip, const Host& h, const std::string& f, const Value& v) {
    const std::string& k = h.kind;
    if (k == "game") {
        setSceneProp(ip, f, v);
        return;
    }
    if (k == "sprite") {
        scene::Sprite* s = spriteOf(ip, h);
        if (!s) return;
        if (f == "shader") {
            s->shader = v.t == T::Host && v.host ? v.host->shader : nullptr;
            return;
        }
        if (f == "frames") {
            if (v.t == T::Host && v.host && v.host->kind == "atlas")
                if (auto g = ip.sc.gfx.atlas(v.host->path)) s->setGraphic(g);
            return;
        }
        if (f == "camera" || f == "cameras") {
            s->cam = camOf(v);
            return;
        }
        if (f == "text") {
            s->text.text = v.str();
            return;
        }
        setSceneProp(ip, h.path + "." + f, v);
        return;
    }
    if (k == "point") {
        if (h.path == "camFollow") {
            (f == "x" ? ip.sc.followX : ip.sc.followY) = float(v.num());
            return;
        }
        setSceneProp(ip, h.path + "." + f, v);
        return;
    }
    if (k == "camera") {
        if (f == "filters") {
            if (scene::Camera* c = ip.sc.camera(h.path)) {
                c->filters.clear();
                if (v.t == T::Array)
                    for (const Value& x : *v.arr)
                        if (x.t == T::Host && x.host && x.host->shader) c->filters.push_back(x.host->shader);
            }
            return;
        }
        setSceneProp(ip, h.path + "." + f, v);
        return;
    }
    if (k == "curanim") { setSceneProp(ip, h.path + ".animation.curAnim." + f, v); return; }
    if (k == "anim") { setSceneProp(ip, h.path + ".animation." + f, v); return; }
    if (k == "path" || k == "members" || k == "group") { setSceneProp(ip, h.path + "." + f, v); return; }
    if (k == "note") { ip.game.noteSet(h.path, h.index, f, Interp::toScene(v)); return; }
    if (k == "noteobj") {
        if (h.extra.t == T::Object) (*h.extra.map)[f] = v;
        return;
    }
    if (k == "shaderparam") {
        if (f != "value" || !h.shader) return;
        const std::vector<float> fl = floatsOf(v);
        if (h.shader->ints.count(h.path)) {
            std::vector<int> iv;
            for (float x : fl) iv.push_back(int(x));
            h.shader->ints[h.path] = iv;
        } else {
            h.shader->floats[h.path] = fl;
        }
        return;
    }
    if (k == "savedata") {
        const scene::Value sv = Interp::toScene(v);
        if (!std::holds_alternative<std::monostate>(sv)) ip.sc.vars["save:hx." + f] = sv;
        else ip.shared->vars["save:hx." + f] = v;
        return;
    }
    if (k == "music") { setSceneProp(ip, "class:FlxG.sound.music." + f, v); return; }
    if (k == "classpath") { setSceneProp(ip, "class:" + h.path + "." + f, v); return; }
    if (k == "video") {
        if (f == "finishCallback" || f == "onEnd" || f == "onComplete") {
            if (v.t == T::Func) ip.shared->callbacks[h.path] = {v, Value::hostOf(std::make_shared<Host>(h)), &ip};
            return;
        }
        if (f == "alpha" || f == "visible" || f == "x" || f == "y") setSceneProp(ip, h.path + "." + f, v);
        return;
    }
    if (k == "class") {
        if (h.path == "ClientPrefs" || h.path == "PlayState" || h.path == "Conductor") setSceneProp(ip, "class:" + h.path + "." + f, v);
        return;
    }
}

Value hostIndex(Interp& ip, const Host& h, const Value& index) {
    const int i = int(index.num());
    if (h.kind == "members" || h.kind == "group") return hv("path", h.path + ".members[" + std::to_string(i) + "]");
    if (h.kind == "notegroup" || h.kind == "notemembers") return hv("note", h.path, i);
    if (h.kind == "vars") {
        if (auto it = ip.shared->vars.find(index.str()); it != ip.shared->vars.end()) return it->second;
        return Value();
    }
    if (h.kind == "filterlist") {
        scene::Camera* c = ip.sc.camera(h.path);
        if (!c || i < 0 || i >= int(c->filters.size())) return Value();
        return shaderHost(c->filters[size_t(i)]);
    }
    return Value();
}

// ------------------------------------------------------------ methods ----

Value hostCall(Interp& ip, const Host& h, const std::string& m, std::vector<Value>& a) {
    const std::string& k = h.kind;
    scene::Scene& sc = ip.sc;
    if (k == "game") {
        if (m == "add" || m == "insert" || m == "remove" || m == "addBehindGF" || m == "addBehindBF" || m == "addBehindDad" ||
            m == "getLuaObject" || m == "initLuaShader" || m == "createRuntimeShader") {
            bool found = false;
            Value f = globalLookup(ip, m, found);
            return f.fn->native(a);
        }
        if (m == "callOnLuas" || m == "callOnScripts" || m == "callOnHScript") {
            if (!ip.host) return Value();
            std::vector<scene::Value> args;
            if (arg(a, 1).t == T::Array)
                for (const Value& x : *arg(a, 1).arr) args.push_back(Interp::toScene(x));
            return Value(ip.host->call(arg(a, 0).str().c_str(), args));
        }
        if (m == "setOnLuas" || m == "setOnScripts" || m == "setOnHScript") {
            if (ip.host) ip.host->setGlobal(arg(a, 0).str().c_str(), Interp::toScene(arg(a, 1)));
            return Value();
        }
        if (m == "triggerEvent" || m == "triggerEventNote") {
            ip.game.triggerEvent(arg(a, 0).str(), arg(a, 1).str(), arg(a, 2).str());
            return Value();
        }
        if (m == "addTextToDebug") {
            ip.game.debugPrint(arg(a, 0).str(), a.size() > 1 ? colorOf(arg(a, 1)) : 0xFFFFFFFF);
            return Value();
        }
        if (m == "startCountdown") { ip.game.startCountdown(); return Value(true); }
        if (m == "endSong") { ip.game.endSong(); return Value(true); }
        if (m == "moveCamera") { sc.focusOn(arg(a, 0).truthy() ? 1 : 0); return Value(); }
        if (m == "getProperty") return sceneProp(ip, arg(a, 0).str());
        return Value();
    }
    if (k == "sprite") {
        scene::Sprite* s = spriteOf(ip, h);
        if (!s) return Value();
        auto* c = dynamic_cast<scene::Character*>(s);
        if (m == "playAnim") {
            if (c) c->playAnim(arg(a, 0).str(), arg(a, 1).truthy(), arg(a, 2).truthy(), int(arg(a, 3).num()));
            else s->play(arg(a, 0).str(), arg(a, 1).truthy(), arg(a, 2).truthy(), int(arg(a, 3).num()));
            return Value();
        }
        if (m == "dance") { if (c) c->dance(); return Value(); }
        if (m == "recalculateDanceIdle") { if (c) c->recalcDance(false); return Value(); }
        if (m == "setGraphicSize") { s->setGraphicSize(float(arg(a, 0).num()), float(arg(a, 1).num())); return Value(); }
        if (m == "updateHitbox") { s->updateHitbox(); return Value(); }
        if (m == "screenCenter") {
            const std::string ax = toLower(arg(a, 0).str());
            const bool x = a.empty() || ax.find('x') != std::string::npos || ax == "1" || ax == "17";
            const bool y = a.empty() || ax.find('y') != std::string::npos || ax == "16" || ax == "17";
            s->screenCenter(x, y);
            return hv(h.kind, h.path);
        }
        if (m == "setPosition") { s->x = float(arg(a, 0).num()); s->y = float(arg(a, 1).num()); return Value(); }
        if (m == "makeGraphic") {
            s->setGraphic(sc.gfx.solid(int(arg(a, 0).num()), int(arg(a, 1).num()), a.size() > 2 ? colorOf(arg(a, 2)) : 0xFFFFFFFF));
            return hv(h.kind, h.path);
        }
        if (m == "loadGraphic") {
            const Value g = arg(a, 0);
            if (g.t == T::Str)
                if (auto gr = sc.gfx.image(g.s, int(arg(a, 2).num()), int(arg(a, 3).num()))) s->setGraphic(gr);
            return hv(h.kind, h.path);
        }
        if (m == "kill") { s->visible = s->active = false; return Value(); }
        if (m == "revive") { s->visible = s->active = true; return Value(); }
        if (m == "destroy") { removeFromScene(ip, Value::hostOf(std::make_shared<Host>(h))); return Value(); }
        if (m == "getMidpoint" || m == "getGraphicMidpoint") {
            const float w = m == "getMidpoint" ? s->width : s->frameW, hh = m == "getMidpoint" ? s->height : s->frameH;
            return Value::object({{"x", Value(double(s->x + w * 0.5f))}, {"y", Value(double(s->y + hh * 0.5f))}});
        }
        if (m == "getScreenPosition")
            return Value::object({{"x", Value(double(s->x - sc.camGame.scrollX * s->scrollX))},
                                  {"y", Value(double(s->y - sc.camGame.scrollY * s->scrollY))}});
        if (m == "setFormat") {  // font, size, color, alignment, borderStyle, borderColor
            if (a.size() > 0 && arg(a, 0).t == T::Str) s->text.font = arg(a, 0).s;
            if (a.size() > 1) s->text.size = float(arg(a, 1).num());
            if (a.size() > 2) s->text.color = colorOf(arg(a, 2));
            if (a.size() > 3) {
                const std::string al = toLower(arg(a, 3).str());
                s->text.align = al == "center" ? 1 : al == "right" ? 2 : 0;
            }
            if (a.size() > 5) { s->text.borderColor = colorOf(arg(a, 5)); s->text.borderSize = std::max(1.0f, s->text.borderSize); }
            return hv(h.kind, h.path);
        }
        if (m == "setBorderStyle") {
            if (a.size() > 1) s->text.borderColor = colorOf(arg(a, 1));
            s->text.borderSize = a.size() > 2 ? float(arg(a, 2).num()) : 1.0f;
            return hv(h.kind, h.path);
        }
        if (m == "setColorTransform") {
            // Multipliers (0..1) and offsets (0..255): a tint is the closest thing here.
            auto ch = [&](size_t mi, size_t oi) {
                const double mult = a.size() > mi ? arg(a, mi).num() : 1.0, off = a.size() > oi ? arg(a, oi).num() : 0.0;
                return uint32_t(clampv(mult * 255.0 + off, 0.0, 255.0));
            };
            s->color = (ch(0, 4) << 16) | (ch(1, 5) << 8) | ch(2, 6);
            if (a.size() > 3) s->alpha = float(clampv(arg(a, 3).num() + (a.size() > 7 ? arg(a, 7).num() / 255.0 : 0.0), 0.0, 1.0));
            return Value();
        }
        if (m == "addOffset") {
            if (scene::Anim* an = s->anim(arg(a, 0).str())) {
                an->hasOffset = true;
                an->offX = float(arg(a, 1).num());
                an->offY = float(arg(a, 2).num());
            }
            return Value();
        }
        return Value();
    }
    if (k == "anim") {
        scene::Sprite* s = spriteOf(ip, h);
        if (!s) return Value();
        auto* c = dynamic_cast<scene::Character*>(s);
        if (m == "play") {
            if (c) c->playAnim(arg(a, 0).str(), arg(a, 1).truthy(), arg(a, 2).truthy(), int(arg(a, 3).num()));
            else s->play(arg(a, 0).str(), arg(a, 1).truthy(), arg(a, 2).truthy(), int(arg(a, 3).num()));
            return Value();
        }
        if (m == "addByPrefix" || m == "addByIndices" || m == "add") {
            if (!s->graphic) return Value();
            scene::Anim an;
            an.name = arg(a, 0).str();
            if (m == "addByPrefix") {
                an.frames = psych::framesByPrefix(s->graphic->frames, arg(a, 1).str());
                an.fps = a.size() > 2 ? arg(a, 2).num() : 24;
                an.loop = a.size() > 3 ? arg(a, 3).truthy() : true;
            } else if (m == "addByIndices") {
                std::vector<int> idx;
                if (arg(a, 2).t == T::Array)
                    for (const Value& x : *arg(a, 2).arr) idx.push_back(int(x.num()));
                an.frames = psych::framesByIndices(s->graphic->frames, arg(a, 1).str(), idx);
                an.fps = a.size() > 4 ? arg(a, 4).num() : 24;
                an.loop = a.size() > 5 ? arg(a, 5).truthy() : true;
            } else {
                if (arg(a, 1).t == T::Array)
                    for (const Value& x : *arg(a, 1).arr)
                        if (x.num() >= 0 && x.num() < double(s->graphic->frames.size())) an.frames.push_back(int(x.num()));
                an.fps = a.size() > 2 ? arg(a, 2).num() : 24;
                an.loop = a.size() > 3 ? arg(a, 3).truthy() : true;
            }
            if (an.frames.empty()) return Value();
            const bool first = s->cur < 0;
            const std::string nm = an.name;
            if (c) {
                c->addAnim(std::move(an));
                c->recalcDance(false);
            } else {
                s->addAnim(std::move(an));
                if (first) s->play(nm, true);
            }
            return Value();
        }
        if (m == "exists") return Value(s->hasAnim(arg(a, 0).str()));
        if (m == "getByName") return s->hasAnim(arg(a, 0).str()) ? hv("curanim", h.path) : Value();
        if (m == "finish") { s->finishAnim(); return Value(); }
        if (m == "pause" || m == "stop") { s->paused = true; return Value(); }
        if (m == "resume") { s->paused = false; return Value(); }
        return Value();
    }
    if (k == "point") {
        if (m == "set") {
            hostSet(ip, h, "x", arg(a, 0));
            hostSet(ip, h, "y", a.size() > 1 ? arg(a, 1) : arg(a, 0));
        } else if (m == "add") {
            hostSet(ip, h, "x", Value(hostGet(ip, h, "x").num() + arg(a, 0).num()));
            hostSet(ip, h, "y", Value(hostGet(ip, h, "y").num() + arg(a, 1).num()));
        }
        return Value();
    }
    if (k == "camera") {
        scene::Camera* c = sc.camera(h.path);
        if (!c) return Value();
        if (m == "flash") {
            if (!arg(a, 3).truthy() && c->flashTime > 0) return Value();
            c->flashColor = a.empty() ? 0xFFFFFFFF : colorOf(arg(a, 0));
            c->flashDur = c->flashTime = float(std::max(0.001, a.size() > 1 ? arg(a, 1).num() : 1.0));
            return Value();
        }
        if (m == "shake") {
            c->shakeIntensity = float(a.empty() ? 0.05 : arg(a, 0).num());
            c->shakeTime = float(a.size() > 1 ? arg(a, 1).num() : 0.5);
            return Value();
        }
        if (m == "fade") {
            c->fadeColor = a.empty() ? 0xFF000000 : colorOf(arg(a, 0));
            c->fadeDur = float(std::max(0.001, a.size() > 1 ? arg(a, 1).num() : 1.0));
            c->fadeTime = 0;
            c->fadeIn = arg(a, 2).truthy();
            c->fadeAlpha = c->fadeIn ? 1.0f : 0.0f;
            return Value();
        }
        if (m == "setFilters") { hostSet(ip, h, "filters", arg(a, 0)); return Value(); }
        if (m == "stopFX" || m == "stopFlash") { c->flashTime = 0; c->fadeAlpha = 0; c->shakeTime = 0; return Value(); }
        return Value();
    }
    if (k == "group" || k == "members" || k == "notegroup" || k == "notemembers") {
        if (m == "forEach" || m == "forEachAlive" || m == "forEachExists") {
            const double len = hostGet(ip, h, "length").num();
            for (double i = 0; i < len; i += 1) {
                std::vector<Value> ca{hostIndex(ip, h, Value(i))};
                ip.callValue(arg(a, 0), ca);
            }
            return Value();
        }
        return Value();
    }
    if (k == "shader") {
        if (!h.shader) return Value();
        const std::string name = arg(a, 0).str();
        if (m == "setFloat" || m == "setFloatArray") { h.shader->floats[name] = floatsOf(arg(a, 1)); return Value(); }
        if (m == "setInt" || m == "setIntArray" || m == "setBool" || m == "setBoolArray") {
            std::vector<int> iv;
            for (float x : floatsOf(arg(a, 1).t == T::Bool ? Value(arg(a, 1).num()) : arg(a, 1))) iv.push_back(int(x));
            h.shader->ints[name] = iv;
            return Value();
        }
        if (m == "setSampler2D" || m == "setBitmapData") {
            h.shader->samplers[name] = arg(a, 1).t == T::Host ? arg(a, 1).host->path : arg(a, 1).str();
            return Value();
        }
        if (m == "getFloat") {
            auto it = h.shader->floats.find(name);
            return it == h.shader->floats.end() || it->second.empty() ? Value() : Value(double(it->second[0]));
        }
        if (m == "getInt" || m == "getBool") {
            auto it = h.shader->ints.find(name);
            if (it == h.shader->ints.end() || it->second.empty()) return Value();
            return m == "getBool" ? Value(it->second[0] != 0) : Value(double(it->second[0]));
        }
        if (m == "getFloatArray") {
            auto it = h.shader->floats.find(name);
            std::vector<Value> v;
            if (it != h.shader->floats.end())
                for (float x : it->second) v.push_back(Value(double(x)));
            return Value::array(v);
        }
        return Value();
    }
    if (k == "class") {
        const std::string& c = h.path;
        if (c == "Math") {
            bool ok = false;
            Value r = mathCall(m, a, ok);
            return r;
        }
        if (c == "Std") {
            if (m == "string") return Value(arg(a, 0).str());
            if (m == "int") return Value(std::trunc(arg(a, 0).num()));
            if (m == "parseInt") {
                const std::string s = trim(arg(a, 0).str());
                if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) return Value(double(std::strtoll(s.c_str() + 2, nullptr, 16)));
                char* end = nullptr;
                const long long v = std::strtoll(s.c_str(), &end, 10);
                return end == s.c_str() ? Value() : Value(double(v));
            }
            if (m == "parseFloat") {
                const std::string s = trim(arg(a, 0).str());
                char* end = nullptr;
                const double v = std::strtod(s.c_str(), &end);
                return end == s.c_str() ? Value(std::nan("")) : Value(v);
            }
            if (m == "random") return Value(double(std::uniform_int_distribution<int>(0, std::max(0, int(arg(a, 0).num()) - 1))(rng())));
            if (m == "is" || m == "isOfType") return Value(!arg(a, 0).isNull());
            return Value();
        }
        if (c == "StringTools") {
            const std::string s = arg(a, 0).str();
            if (m == "replace") {
                std::string r = s;
                const std::string from = arg(a, 1).str(), to = arg(a, 2).str();
                if (from.empty()) return Value(r);
                for (size_t p = 0; (p = r.find(from, p)) != std::string::npos; p += to.size()) r.replace(p, from.size(), to);
                return Value(r);
            }
            if (m == "startsWith") return Value(s.rfind(arg(a, 1).str(), 0) == 0);
            if (m == "endsWith") {
                const std::string e = arg(a, 1).str();
                return Value(s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0);
            }
            if (m == "trim") return Value(trim(s));
            if (m == "ltrim") { size_t p = 0; while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; return Value(s.substr(p)); }
            if (m == "rtrim") { size_t p = s.size(); while (p > 0 && std::isspace((unsigned char)s[p - 1])) --p; return Value(s.substr(0, p)); }
            if (m == "contains") return Value(s.find(arg(a, 1).str()) != std::string::npos);
            if (m == "hex") {
                char b[32];
                std::snprintf(b, sizeof b, "%0*X", int(arg(a, 1).num()), unsigned(uint32_t(int64_t(arg(a, 0).num()))));
                return Value(std::string(b));
            }
            if (m == "lpad" || m == "rpad") {
                std::string r = s;
                const std::string pad = arg(a, 1).str();
                const size_t len = size_t(std::max(0.0, arg(a, 2).num()));
                while (!pad.empty() && r.size() < len) r = m == "lpad" ? pad + r : r + pad;
                return Value(r);
            }
            return Value();
        }
        if (c == "Reflect") {
            const Value o = arg(a, 0);
            const std::string f = arg(a, 1).str();
            if (m == "field" || m == "getProperty") {
                if (o.t == T::Host) return hostGet(ip, *o.host, f);
                if (o.t == T::Object || o.t == T::Map) {
                    auto it = o.map->find(f);
                    return it == o.map->end() ? Value() : it->second;
                }
                return Value();
            }
            if (m == "setField" || m == "setProperty") {
                if (o.t == T::Host) hostSet(ip, *o.host, f, arg(a, 2));
                else if (o.t == T::Object || o.t == T::Map) (*o.map)[f] = arg(a, 2);
                return Value();
            }
            if (m == "hasField") return Value((o.t == T::Object || o.t == T::Map) && o.map->count(f) > 0);
            if (m == "fields") {
                std::vector<Value> r;
                if (o.t == T::Object || o.t == T::Map)
                    for (auto& kv : *o.map) r.push_back(Value(kv.first));
                return Value::array(r);
            }
            if (m == "isFunction") return Value(o.t == T::Func);
            if (m == "callMethod") {
                std::vector<Value> ca;
                if (arg(a, 2).t == T::Array) ca = *arg(a, 2).arr;
                return ip.callValue(arg(a, 1), ca);
            }
            return Value();
        }
        if (c == "CoolUtil") {
            const double x = arg(a, 0).num(), y = arg(a, 1).num();
            if (m == "numberArray") {  // [min, max)
                std::vector<Value> r;
                const int hi = int(x), lo = a.size() > 1 ? int(y) : 0;
                for (int i = lo; i < hi && r.size() < 100000; ++i) r.push_back(Value(double(i)));
                return Value::array(r);
            }
            if (m == "boundTo") return Value(std::clamp(x, y, arg(a, 2).num()));
            if (m == "floorDecimal") { const double p = std::pow(10.0, y); return Value(std::floor(x * p) / p); }
            if (m == "quantize") return Value(y != 0 ? std::round(x * y) / y : x);
            if (m == "capitalize") {
                std::string t = toLower(arg(a, 0).str());
                if (!t.empty()) t[0] = char(std::toupper((unsigned char)t[0]));
                return Value(t);
            }
            if (m == "coolTextFile") {
                std::vector<Value> r;
                std::string text;
                const std::string f = sc.files.find(arg(a, 0).str());
                if (!f.empty() && scene::readFile(f, text)) {
                    size_t p = 0;
                    while (p <= text.size()) {
                        size_t e = text.find('\n', p);
                        if (e == std::string::npos) e = text.size();
                        r.push_back(Value(trim(text.substr(p, e - p))));
                        p = e + 1;
                    }
                }
                return Value::array(r);
            }
            return Value();
        }
        if (c == "FlxMath") {
            const double x = arg(a, 0).num(), y = arg(a, 1).num(), z = arg(a, 2).num();
            if (m == "lerp") return Value(x + (y - x) * z);
            if (m == "bound") return Value(std::clamp(x, a.size() > 1 && !arg(a, 1).isNull() ? y : -HUGE_VAL, a.size() > 2 && !arg(a, 2).isNull() ? z : HUGE_VAL));
            if (m == "remapToRange") {
                const double w = arg(a, 3).num(), v = arg(a, 4).num();
                return Value(w + (x - y) * (v - w) / (z - y));
            }
            if (m == "roundDecimal") { const double p = std::pow(10.0, y); return Value(std::round(x * p) / p); }
            if (m == "isOdd") return Value(std::fmod(std::fabs(x), 2.0) == 1.0);
            if (m == "isEven") return Value(std::fmod(std::fabs(x), 2.0) == 0.0);
            if (m == "maxInt") return Value(std::max(x, y));
            if (m == "minInt") return Value(std::min(x, y));
            if (m == "signOf") return Value(x < 0 ? -1.0 : 1.0);
            return Value();
        }
        if (c == "FlxColor") {
            if (m == "fromRGB") {
                const uint32_t r = uint32_t(clampv(arg(a, 0).num(), 0.0, 255.0)), g = uint32_t(clampv(arg(a, 1).num(), 0.0, 255.0)),
                               b = uint32_t(clampv(arg(a, 2).num(), 0.0, 255.0)),
                               al = a.size() > 3 ? uint32_t(clampv(arg(a, 3).num(), 0.0, 255.0)) : 255u;
                return Value(double(int32_t((al << 24) | (r << 16) | (g << 8) | b)));
            }
            if (m == "fromRGBFloat") {
                auto c8 = [&](size_t i, double def) { return uint32_t(clampv((a.size() > i ? arg(a, i).num() : def) * 255.0, 0.0, 255.0)); };
                return Value(double(int32_t((c8(3, 1) << 24) | (c8(0, 0) << 16) | (c8(1, 0) << 8) | c8(2, 0))));
            }
            if (m == "fromString") return Value(double(int32_t(psych::parseColor(arg(a, 0).str()))));
            if (m == "fromInt") return Value(arg(a, 0).num());
            if (m == "interpolate") {
                const uint32_t c1 = colorOf(arg(a, 0)), c2 = colorOf(arg(a, 1));
                const double t = clampv(a.size() > 2 ? arg(a, 2).num() : 0.5, 0.0, 1.0);
                uint32_t out = 0;
                for (int sh = 0; sh <= 24; sh += 8) {
                    const double x = double((c1 >> sh) & 255), y = double((c2 >> sh) & 255);
                    out |= uint32_t(std::lround(x + (y - x) * t)) << sh;
                }
                return Value(double(int32_t(out)));
            }
            return Value();
        }
        if (c == "Paths") {
            const std::string key = arg(a, 0).str();
            if (m == "image" || m == "sound" || m == "music" || m == "txt" || m == "json" || m == "xml" || m == "video")
                return Value(key);
            if (m == "getSparrowAtlas" || m == "getPackerAtlas" || m == "getAtlas" || m == "getMultiAtlas") return hv("atlas", key);
            if (m == "font") {
                std::string f = sc.files.find("fonts/" + key);
                return Value(f.empty() ? key : f);
            }
            if (m == "formatToSongPath") return Value(songPathName(key));
            if (m == "fileExists") return Value(!sc.files.find(key).empty());
            return Value(key);
        }
        if (c == "FlxTween") {
            if (m == "tween") {
                const Value obj = arg(a, 0);
                std::string target = pathOf(obj);
                if (obj.t == T::Host && obj.host && obj.host->kind == "game") target = "";
                std::vector<std::pair<std::string, double>> fields;
                if (arg(a, 1).t == T::Object || arg(a, 1).t == T::Map)
                    for (auto& kv : *arg(a, 1).map) fields.push_back({kv.first, kv.second.num()});
                return startTween(ip, target, fields, arg(a, 2).num(), arg(a, 3));
            }
            if (m == "color") {
                return startTween(ip, pathOf(arg(a, 0)), {}, arg(a, 1).num(), arg(a, 4), true, colorOf(arg(a, 2)), colorOf(arg(a, 3)));
            }
            if (m == "angle") {
                const std::string target = pathOf(arg(a, 0));
                sc.setProp(target + ".angle", arg(a, 1).num());
                return startTween(ip, target, {{"angle", arg(a, 2).num()}}, arg(a, 3).num(), arg(a, 4));
            }
            if (m == "num") {
                NumTween nt;
                nt.from = arg(a, 0).num();
                nt.to = arg(a, 1).num();
                nt.dur = std::max(1e-6, arg(a, 2).num());
                const Value opts = arg(a, 3);
                if (opts.t == T::Object || opts.t == T::Map) {
                    auto& mp = *opts.map;
                    if (auto it = mp.find("ease"); it != mp.end()) nt.ease = it->second.str();
                    if (auto it = mp.find("startDelay"); it != mp.end()) nt.delay = it->second.num();
                    if (auto it = mp.find("onComplete"); it != mp.end()) nt.complete = it->second;
                }
                nt.update = arg(a, 4);
                nt.owner = &ip;
                nt.tag = newTag(ip, "__hxnum");
                ip.shared->numTweens.push_back(nt);
                return hv("tween", nt.tag);
            }
            if (m == "cancelTweensOf") {
                const std::string target = pathOf(arg(a, 0));
                sc.tweens.erase(std::remove_if(sc.tweens.begin(), sc.tweens.end(),
                                               [&](const scene::Tween& t) { return t.target == target; }),
                                sc.tweens.end());
                return Value();
            }
            return Value();
        }
        return Value();
    }
    if (k == "tween") {
        if (m == "cancel") {
            sc.cancelTween(h.path);
            ip.shared->callbacks.erase(h.path);
            auto& nt = ip.shared->numTweens;
            nt.erase(std::remove_if(nt.begin(), nt.end(), [&](const NumTween& t) { return t.tag == h.path; }), nt.end());
        }
        return Value();
    }
    if (k == "timer") {
        if (m == "start") {
            scene::Timer t;
            t.tag = h.path;
            t.time = a.empty() ? 1.0 : arg(a, 0).num();
            t.loops = a.size() > 2 ? int(arg(a, 2).num()) : 1;
            if (arg(a, 1).t == T::Func) ip.shared->callbacks[h.path] = {arg(a, 1), Value::hostOf(std::make_shared<Host>(h)), &ip};
            sc.timer(std::move(t));
            return Value::hostOf(std::make_shared<Host>(h));
        }
        if (m == "cancel") {
            sc.cancelTimer(h.path);
            ip.shared->callbacks.erase(h.path);
        }
        return Value();
    }
    if (k == "sound") {
        if (m == "play" || m == "load") {
            ip.game.playSound(arg(a, 0).str(), a.size() > 1 ? arg(a, 1).num() : 1.0, "", arg(a, 2).truthy());
            return hv("music");
        }
        if (m == "playMusic") {
            ip.game.playSound(arg(a, 0).str(), a.size() > 1 ? arg(a, 1).num() : 1.0, "__music", a.size() > 2 ? arg(a, 2).truthy() : true);
            return Value();
        }
        return Value();
    }
    if (k == "random") {
        if (m == "int") {
            const int lo = int(arg(a, 0).num()), hi = a.size() > 1 ? int(arg(a, 1).num()) : 2147483647;
            std::vector<int> ex;
            if (arg(a, 2).t == T::Array)
                for (const Value& x : *arg(a, 2).arr) ex.push_back(int(x.num()));
            if (hi < lo) return Value(double(lo));
            for (int tries = 0; tries < 64; ++tries) {
                const int v = std::uniform_int_distribution<int>(lo, hi)(rng());
                if (std::find(ex.begin(), ex.end(), v) == ex.end()) return Value(double(v));
            }
            return Value(double(lo));
        }
        if (m == "float") {
            const double lo = a.empty() ? 0 : arg(a, 0).num(), hi = a.size() > 1 ? arg(a, 1).num() : 1;
            return Value(lo + (hi - lo) * std::uniform_real_distribution<double>(0, 1)(rng()));
        }
        if (m == "bool") return Value(std::uniform_real_distribution<double>(0, 100)(rng()) < (a.empty() ? 50 : arg(a, 0).num()));
        if (m == "sign") return Value(std::uniform_real_distribution<double>(0, 100)(rng()) < (a.empty() ? 50 : arg(a, 0).num()) ? 1.0 : -1.0);
        if (m == "getObject" && arg(a, 0).t == T::Array && !arg(a, 0).arr->empty()) {
            const auto& v = *arg(a, 0).arr;
            return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng())];
        }
        return Value();
    }
    if (k == "vars") {
        const std::string key = arg(a, 0).str();
        if (m == "get") {
            auto it = ip.shared->vars.find(key);
            return it == ip.shared->vars.end() ? Value() : it->second;
        }
        if (m == "set") {
            ip.shared->vars[key] = arg(a, 1);
            const scene::Value sv = Interp::toScene(arg(a, 1));
            if (!std::holds_alternative<std::monostate>(sv)) sc.vars[key] = sv;
            return Value();
        }
        if (m == "exists") return Value(ip.shared->vars.count(key) > 0);
        if (m == "remove") return Value(ip.shared->vars.erase(key) > 0);
        if (m == "keys") {
            std::vector<Value> r;
            for (auto& kv : ip.shared->vars) r.push_back(Value(kv.first));
            return Value::array(r);
        }
        return Value();
    }
    if (k == "luaobjects") {
        if (m == "get") return objectHost(ip, arg(a, 0).str());
        if (m == "exists") return Value(sc.objects.count(arg(a, 0).str()) > 0);
        return Value();
    }
    if (k == "cameras") return arg(a, 0);
    if (k == "signal") return Value();  // camera signals, the stage: nothing to listen to
    if (k == "filterlist") {
        scene::Camera* c = sc.camera(h.path);
        if (!c) return Value();
        auto shaderOf = [](const Value& v) { return v.t == T::Host && v.host ? v.host->shader : nullptr; };
        if (m == "push" || m == "unshift" || m == "insert") {
            const Value v = m == "insert" ? arg(a, 1) : arg(a, 0);
            if (scene::ShaderPtr sh = shaderOf(v)) {
                if (m == "push") c->filters.push_back(sh);
                else c->filters.insert(c->filters.begin() + (m == "insert" ? std::ptrdiff_t(clampv(int(arg(a, 0).num()), 0, int(c->filters.size()))) : 0), sh);
            }
            return Value(double(c->filters.size()));
        }
        if (m == "remove") {
            const scene::ShaderPtr sh = shaderOf(arg(a, 0));
            const auto it = std::find(c->filters.begin(), c->filters.end(), sh);
            if (it == c->filters.end()) return Value(false);
            c->filters.erase(it);
            return Value(true);
        }
        if (m == "indexOf" || m == "contains") {
            const auto it = std::find(c->filters.begin(), c->filters.end(), shaderOf(arg(a, 0)));
            if (m == "contains") return Value(it != c->filters.end());
            return Value(it == c->filters.end() ? -1.0 : double(it - c->filters.begin()));
        }
        if (m == "pop" && !c->filters.empty()) {
            const scene::ShaderPtr sh = c->filters.back();
            c->filters.pop_back();
            return shaderHost(sh);
        }
        if (m == "splice" || m == "resize") {
            c->filters.clear();
            return Value();
        }
        return Value();
    }
    if (k == "videosignal") {
        if (m == "add" && arg(a, 0).t == T::Func) ip.shared->callbacks[h.path] = {arg(a, 0), hv("video", h.path), &ip};
        return Value();
    }
    if (k == "video") {
        if (m == "playVideo" || m == "play" || m == "load" || m == "playMP4" || m == "startVideo") {
            std::string name = a.empty() || arg(a, 0).isNull() ? h.extra.str() : arg(a, 0).str();
            for (const char* pre : {"assets/videos/", "videos/"})
                if (name.rfind(pre, 0) == 0) name = name.substr(std::string_view(pre).size());
            if (const size_t dot = name.rfind('.'); dot != std::string::npos && name.size() - dot <= 5) name = name.substr(0, dot);
            const bool loop = m == "load" ? arg(a, 1).truthy() : false;
            if (name.empty() || !ip.game.playVideoOnTop(h.path, name, loop)) {
                // No video: finish at once so the script moves on.
                if (auto it = ip.shared->callbacks.find(h.path); it != ip.shared->callbacks.end()) {
                    scene::Completion c;
                    c.tag = h.path;
                    ip.sc.completions.push_back(c);
                }
                return Value(false);
            }
            return Value(true);
        }
        if (m == "stop" || m == "dispose" || m == "destroy" || m == "kill") {
            setSceneProp(ip, h.path + ".visible", Value(false));
            return Value();
        }
        return Value();
    }
    if (k == "music") {
        if (m == "pause") ip.game.soundCommand("__music", "pause", 0, 0, 0);
        else if (m == "resume" || m == "play") ip.game.soundCommand("__music", "resume", 0, 0, 0);
        else if (m == "stop") ip.game.soundCommand("__music", "stop", 0, 0, 0);
        else if (m == "fadeOut") ip.game.soundCommand("__music", "fadeOut", arg(a, 0).num(), arg(a, 1).num(), 0);
        else if (m == "fadeIn") ip.game.soundCommand("__music", "fadeIn", arg(a, 0).num(), arg(a, 1).num(), a.size() > 2 ? arg(a, 2).num() : 1.0);
        return Value();
    }
    return Value();
}

// ------------------------------------------------------------- new ------

Value construct(Interp& ip, const std::string& cls, std::vector<Value>& a) {
    scene::Scene& sc = ip.sc;
    if (cls == "FlxSprite" || cls == "FlxSkewedSprite" || cls == "ModchartSprite" || cls == "FlxBackdrop") {
        auto s = std::make_shared<scene::Sprite>();
        s->tag = newTag(ip, "__hxspr");
        s->x = float(arg(a, 0).num());
        s->y = float(arg(a, 1).num());
        if (arg(a, 2).t == T::Str)
            if (auto g = sc.gfx.image(arg(a, 2).s)) s->setGraphic(g);
        sc.objects[s->tag] = s;
        return hv("sprite", s->tag);
    }
    if (cls == "FlxText") {
        auto s = std::make_shared<scene::Sprite>();
        s->tag = newTag(ip, "__hxtxt");
        s->kind = scene::Kind::Text;
        s->x = float(arg(a, 0).num());
        s->y = float(arg(a, 1).num());
        s->text.fieldWidth = float(arg(a, 2).num());
        s->text.text = arg(a, 3).isNull() ? "" : arg(a, 3).str();
        s->text.size = a.size() > 4 ? float(arg(a, 4).num()) : 8.0f;
        s->frameW = s->width = s->text.fieldWidth > 0 ? s->text.fieldWidth : 100;
        s->frameH = s->height = s->text.size * 1.2f;
        s->centerOrigin();
        sc.objects[s->tag] = s;
        return hv("sprite", s->tag);
    }
    if (cls == "Character" || cls == "Boyfriend") {  // new Character(x, y, name, isPlayer)
        const std::string tag = newTag(ip, "__hxchar");
        const std::string name = arg(a, 2).isNull() ? std::string("bf") : arg(a, 2).str();
        const bool player = cls == "Boyfriend" || arg(a, 3).truthy();
        if (!ip.game.makeExtraCharacter(tag, name, player, float(arg(a, 0).num()), float(arg(a, 1).num()))) {
            ip.game.debugNotice("HScript: character " + name + " could not be made", false, false);
            return Value();
        }
        return hv("sprite", tag);
    }
    if (cls == "MP4Handler" || cls == "VideoHandler" || cls == "FlxVideo" || cls == "VideoSprite" || cls == "FlxVideoSprite" ||
        cls == "MP4Sprite" || cls == "VideoSpriteBase") {
        auto h = mk("video", newTag(ip, "__hxvid"));
        if (arg(a, 0).t == T::Str) h->extra = arg(a, 0);  // VideoSprite(fileName, ...)
        return Value::hostOf(h);
    }
    if (cls == "FlxTimer") return hv("timer", newTag(ip, "__hxtimer"));
    if (cls == "ShaderFilter") return arg(a, 0);
    if (cls == "FlxRuntimeShader") {
        const std::string frag = arg(a, 0).str(), vert = arg(a, 1).str();
        // Same source -> same compiled program.
        std::string name = "__hxsrc" + std::to_string(std::hash<std::string>{}(frag) & 0xFFFFFFF);
        scene::ShaderSource& src = sc.shaderSources[name];
        src.frag = frag;
        src.vert = vert == "null" ? "" : vert;
        auto inst = std::make_shared<scene::ShaderInstance>();
        inst->name = name;
        return shaderHost(inst);
    }
    if (cls == "FlxCamera") return hv("camera", "camOther");
    if (cls == "FlxPoint" || cls == "FlxBasePoint")
        return Value::object({{"x", Value(arg(a, 0).num())}, {"y", Value(arg(a, 1).num())}});
    if (cls == "Map" || cls == "StringMap" || cls == "IntMap" || cls == "ObjectMap" || cls == "EnumValueMap") return Value::mapOf();
    if (cls == "Array") return Value::array();
    if (cls == "FlxTypedGroup" || cls == "FlxGroup" || cls == "FlxSpriteGroup") return Value::array();
    ip.game.debugNotice("HScript: class " + cls + " is not available here", false, false);
    return Value();
}

}  // namespace gh::hx
