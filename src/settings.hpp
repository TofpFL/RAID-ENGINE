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
    bool fullscreen = false;
    bool showTimings = true;
    bool opponentStrums = true;
    bool touchControls = false;  // on-screen lanes (the mouse acts as a finger)
    int fpsCap = 240;          // 0 = unlimited
    float offsetMs = 0;        // audio/visual offset (positive = audio heard late)
    float scrollSpeed = 0;     // 0 = use chart speed
    float masterVol = 0.8f, musicVol = 1.0f, sfxVol = 0.7f, hitsoundVol = 0.0f;

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
    int misses = 0;
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
