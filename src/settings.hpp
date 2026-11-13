// Persistent options, key bindings and best scores (plain text files).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gh {

constexpr int kPadBase = 1000;  // gamepad buttons are mapped to codes 1000+button

struct Settings {
    int keys4[4][2];  // two bindings per lane for 4-key charts
    bool downscroll = false;
    bool middlescroll = false;
    bool ghostTapping = true;
    bool noteSplashes = true;
    bool flashing = true;
    bool camZoom = true;
    bool showFps = false;
    // More in the FPS counter (JS Engine's RAM and debug info, and more):
    bool fpsMemory = false;      // the game's memory and its peak
    bool fpsFrameTimes = false;  // update, draw and worst frame times, with a graph
    bool fpsUsage = false;       // processor and graphics card use, the system's and the card's memory
    bool fpsHardware = false;    // processor, graphics card, OpenGL and system names
    bool lowDetail = false;
    bool vsync = false;
    bool shaders = true;       // mod shaders and screen effects
    // Spam charts: 0 adaptive (notes stacked closer than a few pixels are drawn
    // once while the frame rate is under its target; all are still played),
    // 1 full (every pixel).
    int noteDetail = 0;
    bool luaNoteCallbacks = true;  // goodNoteHit, opponentNoteHit, noteMiss... (OFF: faster spam charts)
    // Rendering mode (desktop): songs play by themselves and are recorded to a
    // video with ffmpeg, frame by frame (render_mode.hpp).
    bool renderMode = false;
    int renderSize = 0;       // 0 1280x720, 1 1920x1080, 2 2560x1440, 3 3840x2160
    int renderFps = 60;
    int renderEncoder = 0;    // renderEncoders()
    int renderBitrate = 20;   // Mbps
    bool renderAudio = true;
    bool extraKeys = true;     // OFF: every chart has 4 keys, like Psych and JS Engine
    int noteCounter = 1;       // HUD note counter: 0 off, 1 your side, 2 both sides
    // Mod script errors and warnings on screen: 0 always, 1 only for scripts
    // that set luaDebugMode (as in Psych), 2 never.
    int luaErrors = 0;
    int luaWarnings = 1;
    bool psychUi = false;      // "Psych Engine" interface (needs psychFolder)
    bool hitPopups = true;     // rating pictures and combo numbers on each hit
    int timeBar = 0;           // 0 time left, 1 time elapsed, 2 song name, 3 off (as in Psych)
    float healthBarAlpha = 1.0f;
    // Psych Engine's own options (Visuals and UI, Gameplay).
    bool hideHud = false;       // health bar, icons, score line and ratings hidden
    bool scoreZoom = true;      // the score line grows a little on each note you hit
    bool botplayText = true;    // BOTPLAY (or REPLAY) on screen while the game plays by itself
    bool comboStacking = true;  // ratings and combo numbers pile up (OFF: only the latest)
    bool opponentNotes = true;  // OFF: the opponent's receptors and notes are hidden
    int pauseMusic = 2;         // Psych interface's pause menu song: 0 none, 1 Breakfast, 2 Tea Time
    // Psych's judgement options (Gameplay): hit windows (ms), Safe Frames
    // (the furthest a note can be hit: frames of 1/60 s) and Rating Offset.
    float sickWindow = 45, goodWindow = 90, badWindow = 135;
    float safeFrames = 10;
    float ratingOffset = 0;
    bool autoPause = true;      // the song pauses when the window loses focus
    bool noReset = false;       // Psych's "Disable Reset Button" (R kills you during a song)
    bool antialiasing = true;   // smooth scaling of mod pictures (OFF: sharp pixels, as Psych's option)
    float splashAlpha = 0.6f;   // note splash opacity
    // Psych's "Adjust Delay and Combo": rating x/y, combo digits x/y (pixels; y up).
    float comboOffset[4] = {0, 0, 0, 0};
    std::string noteSkin;       // Psych interface: images/noteSkins/NOTE_assets-<skin>.png ("" = default)
    // Psych interface's note colours (colour-template skins): per direction
    // (left, down, up, right) the main colour, the highlight and the outline.
    uint32_t noteColors[4][3] = {{0xC24B99, 0xFFFFFF, 0x3C1F56},
                                 {0x00FFFF, 0xFFFFFF, 0x1542B7},
                                 {0x12FA05, 0xFFFFFF, 0x0A4447},
                                 {0xF9393F, 0xFFFFFF, 0x651038}};
    std::string psychFolder;   // a Psych Engine / JS Engine copy to take that interface from
    bool fullscreen = false;
    bool showTimings = true;
    bool opponentStrums = true;
    bool touchControls = false;  // on-screen lanes (the mouse acts as a finger)
    int fpsCap = 240;          // 0 = unlimited
    float offsetMs = 0;        // audio/visual offset (positive = audio heard late)
    float scrollSpeed = 0;     // 0 = use chart speed
    float masterVol = 0.8f, musicVol = 1.0f, sfxVol = 0.7f, hitsoundVol = 0.0f;
    bool muted = false;  // the 0 key (the volume keys bring the sound back)

    Settings();
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    // Key codes for each lane of a K-key chart (4K uses keys4, others presets).
    std::vector<std::vector<int>> laneKeys(int keys) const;
};

const char* keyName(int code);

struct ScoreEntry {
    long long score = 0;
    double acc = 0;
    long long misses = 0;
    std::string rank, clear;
};

class Scores {
public:
    void load(const std::string& path);
    void save() const;
    const ScoreEntry* get(const std::string& key) const;
    bool submit(const std::string& key, const ScoreEntry& e);  // true if new best
    static std::string key(const std::string& songId, const std::string& diff) { return songId + "|" + diff; }

private:
    std::map<std::string, ScoreEntry> map_;
    std::string path_;
};

}  // namespace gh
