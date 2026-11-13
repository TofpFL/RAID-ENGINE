// The duel. Built-in songs use the game's own stages and figures; mod songs
// that ship Psych Engine content (characters, stages, Lua scripts) run in
// "Psych mode": a Flixel-style scene driven by the scripts, with the same
// judgement, input and HUD underneath.
#pragma once

#include <deque>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "characters.hpp"
#include "gfx.hpp"
#include "lua_host.hpp"
#include "raylib.h"
#include "replay.hpp"
#include "scene.hpp"
#include "scene_draw.hpp"
#include "video.hpp"
#include "stage.hpp"

namespace gh {

class PlayState : public State, public ScriptGame {
public:
    explicit PlayState(PlayRequest r);
    ~PlayState() override;
    bool wantsMenuMusic() const override { return false; }
    TouchLayout touchLayout() const override;
    static constexpr int kPauseItems = 4;  // Resume, Restart, Options, Exit to menu
    void openChartEditor();
    void openOptions();
    // Keys bound to a lane are notes, not volume keys.
    bool keyReserved(int key) const override {
        return key > 0 && key < int(keyLane_.size()) && keyLane_[size_t(key)] >= 0;
    }
    void onResume() override;
    void enter() override;
    void update(double dt) override;
    void draw() override;

    // ---- ScriptGame (play_psych.cpp)
    double songPosition() const override { return songMs_; }
    bool getGameProp(std::string_view path, scene::Value& out) override;
    bool setGameProp(std::string_view path, const scene::Value& v) override;
    void triggerEvent(const std::string& name, const std::string& v1, const std::string& v2) override;
    void debugPrint(const std::string& text, uint32_t argb) override;
    void debugNotice(const std::string& text, bool error, bool debugMode) override;
    void endSong() override;
    void restartSong() override;
    void exitSong() override;
    void startCountdown() override;
    void startAndEnd() override;
    bool keyState(std::string_view action, int mode) override;
    bool keyboardState(std::string_view key, int mode) override;
    bool mouseState(std::string_view button, int mode) override;
    float mouseX() override;
    float mouseY() override;
    void playSound(const std::string& file, double volume, const std::string& tag, bool loop) override;
    void soundCommand(const std::string& tag, std::string_view cmd, double a, double b, double c) override;
    double soundQuery(const std::string& tag, std::string_view what) override;
    int noteCount(std::string_view group) override;
    bool noteGet(std::string_view group, int index, std::string_view field, scene::Value& out) override;
    bool noteSet(std::string_view group, int index, std::string_view field, const scene::Value& v) override;
    void changeCharacter(int who, const std::string& name) override;
    void healthBarColors(uint32_t left, uint32_t right) override;
    void timeBarColors(uint32_t left, uint32_t right) override;
    bool startVideo(const std::string& name, bool canSkip) override;
    std::string saveDir() override;
    void precacheCharacter(const std::string& name, int who) override;
    void openCustomSubstate(const std::string& name, bool pauseGame) override;
    void closeCustomSubstate() override;
    void insertToCustomSubstate(const std::string& tag) override;
    bool loadSong(const std::string& name, int difficulty) override;
    bool makeVideoSprite(const std::string& tag, const std::string& name, float x, float y, const std::string& cam,
                         bool loop) override;

private:
    enum class Phase { Loading, Waiting, Playing, Paused, Dead, Ending, Error };

    // ---- flow (state_play.cpp)
    void setup();
    void restart();
    void beginCountdown();
    void handleInput();
    void handleNoteEvents();
    void processChartEvents();
    void onBeat(int b);
    void onStep(int s);
    void onSection(int s);
    void pause();
    void resume();
    void updatePause();
    void updateDead();
    void die();
    void exitToMenu();
    void finish();
    void popup(const std::string& text, Color c, const std::string& timing);
    void splash(int side, int lane);
    CharacterState& charOf(int side) { return (side == 1) == playerLeft_ ? left_ : right_; }

    // ---- layout and drawing (state_play.cpp)
    float noteSize() const;
    void layoutStrums();
    Vector2 strumCenter(int side, int lane) const;
    scene::Strum& strum(int side, int lane);
    const scene::Strum& strum(int side, int lane) const;
    void drawMine(Vector2 c, float size, float alpha, float rot);
    void drawLanes();
    void adaptNoteGap();  // Note detail "Adaptive": see noteGap_
    void drawDebugOverlay();  // F3: chart, clock, audio, notes, frame times (S2X's debug menu)
    void drawHud();
    void drawUnderNotes();  // rating popups and countdown (under the notes, as Psych's comboGroup)
    void drawHudLayer();  // lanes, effects and HUD with the HUD camera
    void drawHudCentered(bool underNotes = false);
    void setGameWidth(float w);  // FlxG.width
    void drawPause();
    void drawPsychPause();  // the Psych Engine interface's pause menu
    void stopPauseMusic();
    void drawDead();
    void drawLoading();
    void drawDebugLog();
    bool hudShown(const char* name, float* alpha = nullptr) const;
    double scrollSpeed() const;

    // ---- Psych mode (play_psych.cpp)
    bool setupPsych();
    scene::CharacterPtr makeCharacter(const std::string& name, bool isPlayer, int slot);
    void placeCharacter(scene::Character& c, int slot);
    void loadScripts(bool afterHud);
    void setScriptGlobals();
    void updateStatGlobals();
    void psychUpdate(double dt);
    void psychDraw();
    void psychNoteHit(const GameEvent& ev, bool opponent);
    void psychMiss(const GameEvent& ev, bool ghost);
    void psychBeat(int b);
    void psychFocus(int side);
    void focusWho(int who);            // camera on 0 bf, 1 dad, 2 gf (Psych moveCamera)
    void moveCameraSection(int sec);   // Psych moveCameraSection
    void psychHoldRefresh();
    void handleBuiltinEvent(const psych::ChartEvent& e);
    scene::Character* charForSide(int side);
    std::string singAnim(int lane, uint8_t flags) const;
    void loadIcons();
    void updateSounds(double dt);
    void releaseAudio();
    bool shadersEnabled() override;
    std::string modsDir() override;
    void loopSong(double ms) override;
    bool gamepadState(int pad, std::string_view button, int mode) override;
    double gamepadAxis(int pad, int axis) override;
    bool makeExtraCharacter(const std::string& tag, const std::string& name, bool isPlayer, float x, float y) override;
    bool playVideoOnTop(const std::string& tag, const std::string& name, bool loop) override;
    // JS Engine events: camera twist, fake length, popups, credits, bot energy...
    bool handleJsEngineEvent(const psych::ChartEvent& e);
    void updateJsEngine(double dt);
    void drawNotices();
    void drawBotEnergy();
    double shownLengthMs() const { return fakeLenMs_ >= 0 ? fakeLenMs_ : chart_.lengthMs; }
    struct NoteRef { uint8_t side, lane; uint32_t idx; };
    LaneNote* noteRef(std::string_view group, int index, int* sideOut = nullptr, int* laneOut = nullptr);

    struct Popup {
        std::string text, timing;
        Color c;
        double t0;
        long long combo;
        int rating = -1;     // 0 sick .. 3 shit (the Psych interface's pictures)
        uint32_t seed = 0;   // Psych's random fall for this one (popUpScore's velocities)
        float beat = 0.5f;   // a beat (s) when it popped: Psych fades the rating a beat later
    };
    struct DebugLine {
        std::string text;
        Color c;
        double t;
    };
    // Tagged script sounds stream (they can seek, report time and change pitch).
    struct ScriptSound {
        Music mus{};
        std::shared_ptr<std::vector<unsigned char>> bytes;  // the stream reads from here
        double volume = 1, fadeFrom = 0, fadeTo = 0, fadeDur = 0, fadeT = 0, pitch = 1;
        bool loop = false, fading = false, paused = false, started = false, finishedSent = false;
    };
    struct Substate {
        std::string name;
        bool active = false, pausesGame = false;
        std::vector<std::string> members;  // objects inserted into it
    };
    Substate substate_;
    Color timeBarFill_{0, 0, 0, 0}, timeBarBack_{0, 0, 0, 0};
    struct NoteVis {
        float alpha = 1, offsetX = 0, offsetY = 0, multSpeed = 1;
        bool visible = true;
    };

    PlayRequest req_;
    const SongEntry* entry_ = nullptr;
    Phase phase_ = Phase::Loading;
    std::future<Prepared> job_;
    Prepared prep_;
    std::string error_;
    Chart chart_;
    Session session_;
    SessionConfig cfg_;
    Stage stage_;
    CharacterState left_, right_;
    Color leftColor_{}, rightColor_{};
    bool playerLeft_ = false;
    double songMs_ = -1e9, leadMs_ = 0, rate_ = 1.0, speedMult_ = 1.0;
    float gameW_ = gfx::W;  // Flixel's FlxG.width (scripts may narrow the picture)
    std::map<std::string, scene::Value, std::less<>> classVars_;  // openfl/lime window values and friends
    int lastBeat_ = -100000, lastStep_ = -100000, lastSection_ = -100000, lastFocus_ = -1;
    float camZoom_ = 1, hudZoom_ = 1, beatPulse_ = 0, minePulse_ = 0, healthShown_ = 1;
    Vector2 camFocus_{640, 370};
    float confirm_[2][9] = {};
    int heldCount_[9] = {};
    std::vector<int> keyLane_ = std::vector<int>(Input::kMaxCodes, -1);
    std::vector<Popup> popups_;
    // Notes per second for the note counter (JS Engine / H-Slice): hits in the last second.
    struct NpsMark {
        double t;
        long long player, opponent;  // hits so far
    };
    std::deque<NpsMark> npsWindow_;
    long long nps_ = 0, maxNps_ = 0, npsOpp_ = 0, maxNpsOpp_ = 0;  // yours, the opponent's
    // Spam charts (S2X's adaptive note budget): notes of a lane closer than
    // noteGap_ pixels are drawn once. 1 = every pixel; doubled while the frame
    // rate stays under its target with thousands of notes on screen, halved
    // again once the screen holds clearly fewer than when it had to grow.
    float noteGap_ = 1.0f;
    bool debugOverlay_ = false;
    bool rendering_ = false;  // the rendering mode records this song (App::renderer)
    bool uiFromMod_ = false;  // the Psych interface took pieces from this song's mod
    void stopRendering();
    int drawnNotes_ = 0, candidateNotes_ = 0;  // last frame: notes drawn / that 1 px would draw
    double drawnSum_ = 0, candidateSum_ = 0, gapTime_ = 0;
    int gapFrames_ = 0;
    double gapTooMuch_[6] = {};  // notes on screen with which gap 2^k was too slow
    gfx::Particles fx_;
    int pauseSel_ = 0, deadSel_ = 0;
    float pauseScroll_ = 0;  // the Psych pause menu's items glide to the selection
    double pausedAt_ = 0;
    Music pauseMusic_{};     // Psych's breakfast, fading in while paused
    float pauseMusicVol_ = 0;
    double deadAt_ = 0, loadStart_ = 0, lastHitsound_ = 0, lastMissSfx_ = 0, waitStart_ = 0, endWaitStart_ = 0;
    double lastSplash_[9] = {};
    bool finished_ = false, songStarted_ = false, countdownStarted_ = false, endRequested_ = false;
    double songStartAt_ = -1;  // when the song started (Psych's time bar fades in)
    double scoreBopAt_ = -10;  // last note you hit (Psych's score text zoom)
    size_t eventIdx_ = 0;
    std::deque<DebugLine> debug_;
    std::unique_ptr<scene::Scene> scene_ = std::make_unique<scene::Scene>();

    // Psych mode
    bool psych_ = false;
    std::string stageName_ = "stage";
    bool baseStage_ = false;  // stage built from the original game's layout
    RenderTexture2D camRt_{};  // a camera with shaders is drawn here first
    struct VideoSlot {
        std::unique_ptr<VideoPlayer> player;
        std::string name, tag;
        bool loop = false, skippable = true, waiting = false, fit = false;  // fit: stretched over the screen
    };
    std::unique_ptr<VideoSlot> cutscene_;
    std::map<std::string, VideoSlot> videoSprites_;
    bool openVideo(VideoSlot& v);
    bool updateVideos(double dt);  // true while a cutscene holds the game
    void drawCutscene();
    bool capturing_ = false;
    std::unique_ptr<ScriptHost> scripts_;
    SceneRenderer renderer_;
    std::vector<NoteRef> noteOrder_;  // every note in time order (script note ids)
    std::map<uint32_t, NoteVis> noteVis_;
    std::map<std::string, ScriptSound> sounds_;
    std::vector<Sound> oneShots_;
    Texture2D iconTex_[2] = {};
    int iconFrames_[2] = {0, 0};
    bool hasPsychIcon_[2] = {false, false};
    uint32_t barColor_[2] = {0xFF0000, 0x66FF33};
    bool customBarColors_ = false;
    double holdRefresh_[2][9] = {};
    RenderTexture2D hudRt_{};
    std::string ratingNameOverride_, ratingFcOverride_;
    double speedTweenFrom_ = 1, speedTweenTo_ = 1, speedTweenT_ = 0, speedTweenDur_ = 0;
    double mustHitSectionFlag_ = 1;
    bool camZoomOnBeat_ = true, cameraZoomsSet_ = false, inCutscene_ = false;
    bool zoomingOn_ = false;  // built-in camera: bops and zoom easing start at the first note hit
    int bopEvery_ = 4;
    // JS Engine events
    bool camTwist_ = false, twisted_ = false;
    double twistIntensity_ = 0;
    double fakeLenMs_ = -1, fakeLenFrom_ = 0, fakeLenTo_ = 0, fakeLenT_ = 0, fakeLenDur_ = 0;
    std::string songNameShown_;  // "Change Song Name" ("" = the chart's title)
    int rainbowEndStep_ = -1;
    scene::ShaderPtr rainbow_;
    struct Notice {
        std::string title, text;
        double t0 = 0;
        int kind = 0;  // 0 credits popup, 1 notification, 2 popup window
    };
    std::vector<Notice> notices_;
    bool popupHold_ = false;  // "Popup": the song waits until it is closed
    bool botEnergyOn_ = false, noEnergy_ = false, usingBotEnergy_ = false;
    double botEnergy_ = 1, energyDrain_ = 1, energyRefill_ = 1, energyCooldown_ = 0;
    float energyAlpha_ = 0;
    bool prevKeys_[9] = {};
    bool keyNow_[9] = {};
    bool prevMouse_[3] = {}, mouseNow_[3] = {};
    long long lastCombo_ = 0;
    bool backdrop_ = false;
    std::map<std::string, scene::CharacterPtr> charCache_;  // "slot:name" (Change Character)
    // Replays and touch input
    Replay record_, playback_;
    bool replaying_ = false;
    size_t replayIdx_ = 0;
    bool touchDown_[9] = {};
    void laneDown(int lane, double t);
    void laneUp(int lane, double t);
    void handleTouch();
};

}  // namespace gh
