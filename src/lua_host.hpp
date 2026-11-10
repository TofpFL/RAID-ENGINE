// Lua 5.4 scripting with the Psych Engine API (plus Kade Engine modchart
// helpers). Each script runs in its own sandboxed state: no io/os/package
// access, scripts only come from the selected song's own content roots, and
// runaway loops are stopped after a time budget.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hscript.hpp"
#include "scene.hpp"

namespace gh {

// Gameplay side of the API, implemented by the play state (and by a stub in
// the tests).
class ScriptGame {
public:
    virtual ~ScriptGame() = default;
    virtual double songPosition() const = 0;  // ms
    // Game-level properties: health, songScore, botPlay, ClientPrefs, ...
    virtual bool getGameProp(std::string_view path, scene::Value& out) = 0;
    virtual bool setGameProp(std::string_view path, const scene::Value& v) = 0;
    virtual void triggerEvent(const std::string& name, const std::string& v1, const std::string& v2) = 0;
    virtual void debugPrint(const std::string& text, uint32_t argb) = 0;
    virtual void endSong() {}
    virtual void restartSong() {}
    virtual void exitSong() {}
    virtual void startCountdown() {}
    virtual void startAndEnd() { startCountdown(); }  // skipped video/dialogue finished
    // mode: 0 held, 1 just pressed, 2 just released
    virtual bool keyState(std::string_view action, int mode) { return false; }
    virtual bool keyboardState(std::string_view key, int mode) { return false; }
    virtual bool mouseState(std::string_view button, int mode) { return false; }
    virtual float mouseX() { return 0; }
    virtual float mouseY() { return 0; }
    virtual void playSound(const std::string& file, double volume, const std::string& tag, bool loop) {}
    virtual void soundCommand(const std::string& tag, std::string_view cmd, double a, double b, double c) {}
    virtual double soundQuery(const std::string& tag, std::string_view what) { return 0; }
    virtual int noteCount(std::string_view group) { return 0; }
    virtual bool noteGet(std::string_view group, int index, std::string_view field, scene::Value& out) { return false; }
    virtual bool noteSet(std::string_view group, int index, std::string_view field, const scene::Value& v) { return false; }
    virtual void changeCharacter(int who, const std::string& name) {}
    // Videos: false when the video cannot play (the script then moves on).
    virtual bool startVideo(const std::string& name, bool canSkip) { return false; }
    virtual bool makeVideoSprite(const std::string& tag, const std::string& name, float x, float y, const std::string& cam,
                                 bool loop) { return false; }
    virtual std::string saveDir() { return ""; }  // where initSaveData files live ("" = memory only)
    virtual void precacheCharacter(const std::string& name, int who) {}
    virtual void openCustomSubstate(const std::string& name, bool pauseGame) {}
    virtual void closeCustomSubstate() {}
    virtual void insertToCustomSubstate(const std::string& tag) {}
    virtual bool loadSong(const std::string& name, int difficulty) { return false; }
    virtual void healthBarColors(uint32_t left, uint32_t right) {}
    virtual void timeBarColors(uint32_t left, uint32_t right) {}
    virtual bool shadersEnabled() { return true; }  // the "Shaders" option
    virtual std::string modsDir() { return ""; }    // saveFile/deleteFile stay inside it ("" = not allowed)
    virtual void loopSong(double ms) {}             // JS Engine loopTheSong
    // Gamepads (Psych 0.7): button names such as A, B, START, DPAD_UP,
    // LEFT_SHOULDER; mode as keyState; pad -1 = any.
    virtual bool gamepadState(int pad, std::string_view button, int mode) { return false; }
    virtual double gamepadAxis(int pad, int axis) { return 0; }  // 0 left x, 1 left y, 2 right x, 3 right y
    // HScript `new Character(x, y, name, isPlayer)`: an extra character (not
    // in the scene yet), registered under `tag`.
    virtual bool makeExtraCharacter(const std::string& tag, const std::string& name, bool isPlayer, float x, float y) {
        return false;
    }
    // HScript video players (MP4Handler, VideoHandler...): a full-screen video
    // on camOther; scripts hear onVideoFinished(tag) when it ends.
    virtual bool playVideoOnTop(const std::string& tag, const std::string& name, bool loop) { return false; }
};

class ScriptHost {
public:
    static constexpr const char* kStop = "##PSYCHLUA_FUNCTIONSTOP";
    static constexpr const char* kContinue = "##PSYCHLUA_FUNCTIONCONTINUE";
    static constexpr const char* kStopLua = "##PSYCHLUA_FUNCTIONSTOPLUA";

    ScriptHost(scene::Scene& sc, ScriptGame& game);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Loads a file, runs its top level, then calls onCreate (like Psych).
    bool load(const std::string& path, bool callCreate = true);
    bool loadString(const std::string& name, const std::string& code, bool callCreate = true);
    bool isLoaded(const std::string& path) const;
    size_t count() const;

    // Calls fn on every running script. Returns the last value that was not
    // Function_Continue (empty string when none), e.g. kStop.
    std::string call(const char* fn, const std::vector<scene::Value>& args = {});
    bool callStops(const char* fn, const std::vector<scene::Value>& args = {}) { return call(fn, args) == kStop; }
    void setGlobal(const char* name, const scene::Value& v);
    void dispatchCompletions();  // tween/timer callbacks queued by the scene

    std::vector<std::string> errors;  // every error, in order (tests read this)
    bool kadeMode = false;            // a Kade modchart was loaded
    std::string songDataDir;          // "data/<song>/" (Kade sprites live there)
    double callBudget = 2.0, loadBudget = 8.0;  // seconds before a runaway script is stopped

    struct Script;
    scene::Scene& scene;
    ScriptGame& game;
    std::vector<std::unique_ptr<Script>>& scriptList() { return scripts_; }
    std::vector<std::string> scriptPaths() const;  // running Lua scripts (HScript game.luaArray)

    // HScript (.hx files and runHaxeCode)
    std::shared_ptr<hx::Shared> hxShared;
    bool loadHx(const std::string& path, const std::string& code, bool callCreate = true);
    // createCallback / createGlobalCallback: a Lua function that calls into HScript.
    void registerHxCallback(const std::string& name, hx::Interp* owner, const hx::Value& fn);
    struct HxCallback {
        hx::Interp* owner = nullptr;
        hx::Value fn;
    };
    std::map<std::string, HxCallback> hxCallbacks;
    void reportError(const std::string& msg);
    void setHxGlobal(const std::string& name, const hx::Value& v);  // setOnHScript

private:
    struct HxScript {
        std::unique_ptr<hx::Interp> ip;
        std::string path, name;
        bool closed = false;
    };
    std::vector<std::unique_ptr<Script>> scripts_;
    std::vector<std::unique_ptr<HxScript>> hxScripts_;
    std::vector<std::pair<std::string, scene::Value>> globals_;
};

}  // namespace gh
