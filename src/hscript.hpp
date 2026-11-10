// HScript: the Haxe subset Psych Engine runs through runHaxeCode() and .hx
// script files. A small tree-walking interpreter (expressions, var/final,
// functions and arrow functions, closures, if/while/do/for/switch/try,
// string interpolation, arrays, maps and anonymous objects) bridged to the
// scene: game (PlayState.instance), FlxG, FlxSprite, FlxText, FlxTween,
// FlxTimer, FlxEase, FlxColor, Paths, ShaderFilter, FlxRuntimeShader, Std,
// Math, StringTools and the Psych helpers (setVar, getVar, debugPrint, ...).
// Classes that do not exist here resolve to null instead of crashing.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "scene.hpp"

namespace gh {
class ScriptGame;
class ScriptHost;
}

namespace gh::hx {

enum class T : uint8_t { Null, Bool, Num, Str, Array, Object, Map, Func, Host };

struct Func;
struct Host;
struct Value;
using ValueMap = std::map<std::string, Value>;

struct Value {
    T t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::shared_ptr<std::vector<Value>> arr;
    std::shared_ptr<ValueMap> map;  // Object and Map
    std::shared_ptr<Func> fn;
    std::shared_ptr<Host> host;

    Value() = default;
    Value(bool v) : t(T::Bool), b(v) {}
    Value(double v) : t(T::Num), n(v) {}
    Value(int v) : t(T::Num), n(v) {}
    Value(const char* v) : t(T::Str), s(v) {}
    Value(std::string v) : t(T::Str), s(std::move(v)) {}
    static Value array(std::vector<Value> v = {});
    static Value object(ValueMap m = {});
    static Value mapOf(ValueMap m = {});
    static Value native(std::string name, std::function<Value(std::vector<Value>&)> f);
    static Value hostOf(std::shared_ptr<Host> h);
    bool isNull() const { return t == T::Null; }
    bool truthy() const;
    double num() const;
    std::string str() const;  // Std.string
};

// A handle on something in the game (sprite, camera, class, group...).
struct Host {
    std::string kind;  // "game", "class", "sprite", "path", "camera", "anim", "point", "group", "members",
                       // "note", "shader", "atlas", "timer", "tween", "sound", "music", "random", "keys",
                       // "keystate", "mouse", "save", "savedata", "cameras", "vars", "luaobjects"
    std::string path;  // scene path or class name
    int index = -1;
    scene::SpritePtr sprite;
    scene::ShaderPtr shader;
    Value extra;
};

struct Shared;  // state shared by every interpreter of one script host

struct Node;
struct Program;
struct Env;

struct Func {
    std::string name;
    std::vector<std::string> params;
    std::vector<const Node*> defaults;
    const Node* body = nullptr;
    bool exprBody = false;  // arrow function: body is an expression
    std::shared_ptr<Env> env;
    std::shared_ptr<Program> prog;  // keeps the syntax tree alive
    std::function<Value(std::vector<Value>&)> native;
};

class Interp {
public:
    Interp(scene::Scene& sc, ScriptGame& game, ScriptHost* host, std::shared_ptr<Shared> shared);
    ~Interp();

    // Runs a chunk of code; result is the value of its last statement or `return`.
    bool run(const std::string& code, const std::string& name, Value& result, std::string& err);
    // Calls a function defined by earlier code. found=false when it does not exist.
    bool call(const std::string& fn, std::vector<Value> args, Value& result, std::string& err, bool* found = nullptr);
    bool hasFunction(const std::string& fn) const;
    void setGlobal(const std::string& name, Value v);
    Value getGlobal(const std::string& name) const;

    static Value fromScene(const scene::Value& v);
    static scene::Value toScene(const Value& v);

    double budget = 2.0;  // seconds before a runaway script is stopped
    std::string scriptName;
    std::string luaPath;  // runHaxeCode: the Lua script it runs for (Psych 0.7 parentLua)

    // Implementation access for the bridge.
    scene::Scene& sc;
    ScriptGame& game;
    ScriptHost* host;
    std::shared_ptr<Shared> shared;
    Value callValue(const Value& f, std::vector<Value>& args);

private:
    friend struct Evaluator;
    std::shared_ptr<Env> globals_;
    std::vector<std::shared_ptr<Program>> programs_;
};

// Shared between the interpreters of one ScriptHost: setVar() values and the
// callbacks of tweens and timers created from HScript.
struct NumTween {
    double from = 0, to = 0, dur = 1, t = 0, delay = 0;
    std::string ease = "linear";
    Value update, complete;
    Interp* owner = nullptr;
    std::string tag;
};

struct Shared {
    ValueMap vars;
    struct Callback {
        Value fn;
        Value arg;
        Interp* owner = nullptr;
    };
    std::map<std::string, Callback> callbacks;  // by tween/timer tag ("__hx...")
    std::vector<NumTween> numTweens;
    int nextId = 0;
    std::vector<Interp*> interps;  // live interpreters (for callbacks)
    std::function<void(const std::string&)> report;  // errors raised inside callbacks
    void forget(Interp* ip);
    void update(double dt);                         // advances FlxTween.num tweens
    bool complete(const std::string& tag, int loopsLeft);  // runs a tween/timer callback
};

// The bridge (hscript_bridge.cpp).
Value globalLookup(Interp& ip, const std::string& name, bool& found);
Value hostGet(Interp& ip, const Host& h, const std::string& field);
void hostSet(Interp& ip, const Host& h, const std::string& field, const Value& v);
Value hostCall(Interp& ip, const Host& h, const std::string& method, std::vector<Value>& args);
Value construct(Interp& ip, const std::string& cls, std::vector<Value>& args);
Value hostIndex(Interp& ip, const Host& h, const Value& index);

}  // namespace gh::hx
