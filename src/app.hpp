// Application shell: main loop, global services and the game-state machine.
#pragma once

#include <array>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "audio.hpp"
#include "input.hpp"
#include "library.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "touch_ui.hpp"

namespace gh {

class State {
public:
    virtual ~State() = default;
    virtual void enter() {}
    virtual void update(double dt) = 0;
    virtual void draw() = 0;
    virtual bool wantsMenuMusic() const { return true; }
    // On-screen buttons for touchscreens (arrows, OK and BACK by default).
    virtual TouchLayout touchLayout() const { return {}; }
    // The app came back from the background (Android).
    virtual void onResume() {}
    // This screen uses the key itself (a lane, typing, rebinding), so the
    // game-wide volume keys leave it alone.
    virtual bool keyReserved(int key) const { return false; }
};

struct Modifiers {
    double rate = 1.0;
    bool botplay = false, practice = false, mirror = false;
};

struct PlayRequest {
    std::string songId, diff;
    bool story = false;
    std::string replayPath;  // watch this replay instead of playing
    std::vector<std::string> storyQueue;  // songs still to play after this one
    long long storyScore = 0;
    long long storyMisses = 0;
    int storySongs = 0;
    double storyAccSum = 0;
};

struct ResultData {
    PlayRequest req;
    std::string title, diff;
    long long score = 0;
    double acc = 0;
    long long misses = 0, maxCombo = 0;
    std::array<long long, kRatingCount> counts{};
    long long totalNotes = 0;  // player notes in the chart, stacked ones included
    long long opponentHits = 0, opponentNotes = 0;  // the opponent's side, for the "both sides" note count
    std::string rank, clear;
    std::vector<Session::HitError> errors;
    double lengthMs = 1;
    bool newBest = false, botplay = false, practice = false, replay = false;
    std::string replayPath;  // replay saved for this run ("" if none)
    double rate = 1.0;
    unsigned color = 0xFFFFFF;
};

class App {
public:
    std::string base;  // folder with assets/, songs/, mods/ (ends with '/')
    bool listSongs = false;
    bool checkCharts = false;  // --check-charts with --list-songs
    Settings settings;
    Scores scores;
    Library library;
    Mixer mixer;
    Input input;
    TouchUi touch;
    bool touchMode = false;  // on-screen buttons (Android, or --touch)
    Modifiers mods;
    double now = 0, dt = 0, startTime = 0;
    float fps = 0;
    bool quit = false;
    // Frame timing (ms, smoothed) for the debug overlay; --perf prints it every second.
    bool perf = false;
    float updateMs = 0, drawMs = 0, worstFrameMs = 0;

    // remembered menu positions
    int mainSel = 0, freeplaySel = 0, freeplayDiff = 1, storyDiff = 1;

    struct Automation {
        bool enabled = false;
        std::string song, diff = "Hard", state;
        std::vector<double> shots;
        std::string shotPrefix = "shot";
        double quitAfter = -1;
        bool botplay = true;
        std::string editorSong, editorExport;  // --editor <song> [diff], --editor-export <file>
    } autoRun;

    // Runs the game with a fresh App (Android can call main() again in the
    // same process after the game was closed).
    static int start(int argc, char** argv);
    int run(int argc, char** argv);
    void change(std::unique_ptr<State> next);
    void startMenuMusic();
    void stopMenuMusic();
    bool menuMusicPlaying() const { return menuMusic_; }
    double menuBeat();
    double menuBpm() const { return menuBpm_; }
    void applyVolumes();
    float effectiveVolume() const { return settings.muted ? 0.0f : settings.masterVol; }
    void saveSettings();
    void toggleFullscreen();
    // Loads (or drops) the "Psych Engine" interface from settings: textures,
    // sounds and menu music. `psychReport` says what was found.
    void applyPsychUi();
    std::string psychReport;
    std::string psychFolderInUse;  // the folder chosen, or the one found automatically
    bool psychAutomatic = false;   // no folder chosen: the best copy found on disk
    // Options opened from a song's pause menu: leaving them plays it again.
    std::unique_ptr<PlayRequest> optionsReturn;
    // Deaths on the song played last (Psych's "Blueballed" in the pause menu).
    int blueballed = 0;
    std::string blueballedSong;

private:
    void frame();
    void volumeKeys();       // - and = change the master volume, 0 mutes (as in Flixel)
    void drawVolumeTray();
    double volumeShownAt_ = -10;
    double perfSince_ = 0;
    int perfFrames_ = 0;
    float perfUpd_ = 0, perfDraw_ = 0, perfWorst_ = 0;
    std::unique_ptr<State> state_, next_;
    float fade_ = 0;
    int fadeDir_ = 0;
    bool menuMusic_ = false;
    double menuBpm_ = 100, menuLoop_ = 0;
    PcmPtr menuPcm_;
    std::future<PcmPtr> menuJob_;
    bool psychMenuMusic_ = false;  // menuPcm_ is Psych's freakyMenu
    size_t shotIndex_ = 0;
    int winW_ = 1280, winH_ = 720;
    int shownW_ = 0, shownH_ = 0;  // window size last frame (logged on Android when it changes)
};

App& app();

// State factories
std::unique_ptr<State> makeTitle();
std::unique_ptr<State> makeMainMenu();
std::unique_ptr<State> makeStory();
std::unique_ptr<State> makeFreeplay();
std::unique_ptr<State> makeOptions();
std::unique_ptr<State> makeCredits();
std::unique_ptr<State> makeCalibration();
std::unique_ptr<State> makePlay(const PlayRequest& req);
std::unique_ptr<State> makeResults(const ResultData& r);
std::unique_ptr<State> makeChartEditor(const std::string& songId, const std::string& diff);
std::unique_ptr<State> makeModPacks();

// Waits (bounded) for detached preview/compose jobs before shutdown.
void waitForBackgroundJobs();

// Shared menu visuals
void drawMenuBackground(double now, unsigned tintHex, float pulse);
float menuPulse();  // 1 on each menu-music beat, decaying

}  // namespace gh
