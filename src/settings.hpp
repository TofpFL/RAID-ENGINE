// Persistent options, key bindings and best scores (plain text files).
#pragma once

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
    bool lowDetail = false;
    bool vsync = false;
    bool shaders = true;       // mod shaders and screen effects
    // Spam charts: 0 adaptive (notes stacked closer than a few pixels are drawn
    // once while the frame rate is under its target; all are still played),
    // 1 full (every pixel).
    int noteDetail = 0;
    bool luaNoteCallbacks = true;  // goodNoteHit, opponentNoteHit, noteMiss... (OFF: faster spam charts)
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
