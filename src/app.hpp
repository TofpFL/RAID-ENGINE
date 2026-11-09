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

namespace gh {

class State {
public:
    virtual ~State() = default;
    virtual void enter() {}
    virtual void update(double dt) = 0;
    virtual void draw() = 0;
    virtual bool wantsMenuMusic() const { return true; }
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
    int storyMisses = 0, storySongs = 0;
    double storyAccSum = 0;
};

struct ResultData {
    PlayRequest req;
    std::string title, diff;
    long long score = 0;
    double acc = 0;
    int misses = 0, maxCombo = 0;
    std::array<int, kRatingCount> counts{};
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
    Settings settings;
    Scores scores;
    Library library;
    Mixer mixer;
    Input input;
    Modifiers mods;
    double now = 0, dt = 0, startTime = 0;
    float fps = 0;
    bool quit = false;

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

    int run(int argc, char** argv);
    void change(std::unique_ptr<State> next);
    void startMenuMusic();
    void stopMenuMusic();
    bool menuMusicPlaying() const { return menuMusic_; }
    double menuBeat();
    double menuBpm() const { return menuBpm_; }
    void applyVolumes();
    void saveSettings();
    void toggleFullscreen();

private:
    void frame();
    std::unique_ptr<State> state_, next_;
    float fade_ = 0;
    int fadeDir_ = 0;
    bool menuMusic_ = false;
    double menuBpm_ = 100, menuLoop_ = 0;
    PcmPtr menuPcm_;
    std::future<PcmPtr> menuJob_;
    size_t shotIndex_ = 0;
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
