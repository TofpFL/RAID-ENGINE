#include "settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common.hpp"
#include "raylib.h"

namespace gh {

Settings::Settings() {
    const int def[4][2] = {{KEY_D, KEY_LEFT}, {KEY_F, KEY_DOWN}, {KEY_J, KEY_UP}, {KEY_K, KEY_RIGHT}};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) keys4[i][j] = def[i][j];
}

bool Settings::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        const double d = std::atof(v.c_str());
        const bool b = v == "1" || v == "true";
        if (k.rfind("key", 0) == 0 && k.size() == 5) {
            const int lane = k[3] - '0', slot = k[4] - '0';
            if (lane >= 0 && lane < 4 && slot >= 0 && slot < 2) keys4[lane][slot] = int(d);
        } else if (k == "downscroll") downscroll = b;
        else if (k == "middlescroll") middlescroll = b;
        else if (k == "ghostTapping") ghostTapping = b;
        else if (k == "noteSplashes") noteSplashes = b;
        else if (k == "flashing") flashing = b;
        else if (k == "camZoom") camZoom = b;
        else if (k == "showFps") showFps = b;
        else if (k == "lowDetail") lowDetail = b;
        else if (k == "vsync") vsync = b;
        else if (k == "shaders") shaders = b;
        else if (k == "noteDetail") noteDetail = clampv(int(d), 0, 1);
        else if (k == "luaNoteCallbacks") luaNoteCallbacks = b;
        else if (k == "extraKeys") extraKeys = b;
        else if (k == "noteCounter") noteCounter = clampv(int(d), 0, 2);
        else if (k == "luaErrors") luaErrors = clampv(int(d), 0, 2);
        else if (k == "luaWarnings") luaWarnings = clampv(int(d), 0, 2);
        else if (k == "psychUi") psychUi = b;
        else if (k == "hitPopups") hitPopups = b;
        else if (k == "timeBar") timeBar = clampv(int(d), 0, 3);
        else if (k == "healthBarAlpha") healthBarAlpha = clampv(float(d), 0.0f, 1.0f);
        else if (k == "psychFolder") psychFolder = v;
        else if (k == "fullscreen") fullscreen = b;
        else if (k == "showTimings") showTimings = b;
        else if (k == "opponentStrums") opponentStrums = b;
        else if (k == "touchControls") touchControls = b;
        else if (k == "fpsCap") fpsCap = clampv(int(d), 0, 1000);
        else if (k == "offsetMs") offsetMs = clampv(float(d), -500.0f, 500.0f);
        else if (k == "scrollSpeed") scrollSpeed = clampv(float(d), 0.0f, 10.0f);
        else if (k == "masterVol") masterVol = clampv(float(d), 0.0f, 1.0f);
        else if (k == "musicVol") musicVol = clampv(float(d), 0.0f, 1.0f);
        else if (k == "sfxVol") sfxVol = clampv(float(d), 0.0f, 1.0f);
        else if (k == "hitsoundVol") hitsoundVol = clampv(float(d), 0.0f, 1.0f);
        else if (k == "muted") muted = b;
    }
    return true;
}

bool Settings::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) f << "key" << i << j << "=" << keys4[i][j] << "\n";
    f << "downscroll=" << downscroll << "\nmiddlescroll=" << middlescroll << "\nghostTapping=" << ghostTapping
      << "\nnoteSplashes=" << noteSplashes << "\nflashing=" << flashing << "\ncamZoom=" << camZoom
      << "\nshowFps=" << showFps << "\nlowDetail=" << lowDetail << "\nvsync=" << vsync << "\nshaders=" << shaders << "\nextraKeys=" << extraKeys << "\nnoteCounter=" << noteCounter
      << "\nnoteDetail=" << noteDetail << "\nluaNoteCallbacks=" << luaNoteCallbacks
      << "\nluaErrors=" << luaErrors << "\nluaWarnings=" << luaWarnings << "\nhitPopups=" << hitPopups
      << "\ntimeBar=" << timeBar << "\nhealthBarAlpha=" << healthBarAlpha
      << "\nfullscreen=" << fullscreen << "\nshowTimings=" << showTimings << "\nopponentStrums=" << opponentStrums << "\ntouchControls=" << touchControls
      << "\nfpsCap=" << fpsCap << "\noffsetMs=" << offsetMs << "\nscrollSpeed=" << scrollSpeed
      << "\nmasterVol=" << masterVol << "\nmusicVol=" << musicVol << "\nsfxVol=" << sfxVol
      << "\nhitsoundVol=" << hitsoundVol << "\nmuted=" << muted << "\npsychUi=" << psychUi << "\npsychFolder=" << psychFolder << "\n";
    return bool(f);
}

std::vector<std::vector<int>> Settings::laneKeys(int keys) const {
    std::vector<std::vector<int>> r;
    if (keys == 4) {
        for (int i = 0; i < 4; ++i) r.push_back({keys4[i][0], keys4[i][1]});
        // Gamepad: d-pad and face buttons.
        r[0].push_back(kPadBase + GAMEPAD_BUTTON_LEFT_FACE_LEFT);
        r[0].push_back(kPadBase + GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
        r[1].push_back(kPadBase + GAMEPAD_BUTTON_LEFT_FACE_DOWN);
        r[1].push_back(kPadBase + GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        r[2].push_back(kPadBase + GAMEPAD_BUTTON_LEFT_FACE_UP);
        r[2].push_back(kPadBase + GAMEPAD_BUTTON_RIGHT_FACE_UP);
        r[3].push_back(kPadBase + GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
        r[3].push_back(kPadBase + GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
        return r;
    }
    static const std::vector<std::vector<int>> presets = {
        {KEY_SPACE},
        {KEY_D, KEY_K},
        {KEY_D, KEY_SPACE, KEY_K},
        {},
        {KEY_D, KEY_F, KEY_SPACE, KEY_J, KEY_K},
        {KEY_S, KEY_D, KEY_F, KEY_J, KEY_K, KEY_L},
        {KEY_S, KEY_D, KEY_F, KEY_SPACE, KEY_J, KEY_K, KEY_L},
        {KEY_A, KEY_S, KEY_D, KEY_F, KEY_H, KEY_J, KEY_K, KEY_L},
        {KEY_A, KEY_S, KEY_D, KEY_F, KEY_SPACE, KEY_H, KEY_J, KEY_K, KEY_L},
    };
    const auto& p = presets[size_t(clampv(keys, 1, 9) - 1)];
    for (int k : p) r.push_back({k});
    return r;
}

const char* keyName(int code) {
    static char buf[32];
    if (code >= kPadBase) {
        switch (code - kPadBase) {
            case GAMEPAD_BUTTON_LEFT_FACE_UP: return "Pad Up";
            case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return "Pad Down";
            case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return "Pad Left";
            case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return "Pad Right";
            case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return "Pad A";
            case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "Pad B";
            case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return "Pad X";
            case GAMEPAD_BUTTON_RIGHT_FACE_UP: return "Pad Y";
            default: std::snprintf(buf, sizeof buf, "Pad %d", code - kPadBase); return buf;
        }
    }
    if (code >= KEY_A && code <= KEY_Z) { buf[0] = char(code); buf[1] = 0; return buf; }
    if (code >= KEY_ZERO && code <= KEY_NINE) { buf[0] = char(code); buf[1] = 0; return buf; }
    switch (code) {
        case 0: return "---";
        case KEY_LEFT: return "Left";
        case KEY_RIGHT: return "Right";
        case KEY_UP: return "Up";
        case KEY_DOWN: return "Down";
        case KEY_SPACE: return "Space";
        case KEY_ENTER: return "Enter";
        case KEY_ESCAPE: return "Esc";
        case KEY_TAB: return "Tab";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_LEFT_SHIFT: return "LShift";
        case KEY_RIGHT_SHIFT: return "RShift";
        case KEY_LEFT_CONTROL: return "LCtrl";
        case KEY_RIGHT_CONTROL: return "RCtrl";
        case KEY_LEFT_ALT: return "LAlt";
        case KEY_RIGHT_ALT: return "RAlt";
        case KEY_SEMICOLON: return ";";
        case KEY_COMMA: return ",";
        case KEY_PERIOD: return ".";
        case KEY_SLASH: return "/";
        case KEY_APOSTROPHE: return "'";
        case KEY_LEFT_BRACKET: return "[";
        case KEY_RIGHT_BRACKET: return "]";
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_BACKSLASH: return "\\";
        case KEY_GRAVE: return "`";
        default: break;
    }
    if (code >= KEY_KP_0 && code <= KEY_KP_9) { std::snprintf(buf, sizeof buf, "Num%d", code - KEY_KP_0); return buf; }
    if (code >= KEY_F1 && code <= KEY_F12) { std::snprintf(buf, sizeof buf, "F%d", code - KEY_F1 + 1); return buf; }
    std::snprintf(buf, sizeof buf, "Key %d", code);
    return buf;
}

void Scores::load(const std::string& path) {
    path_ = path;
    map_.clear();
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string key, score, acc, misses, rank, clear;
        if (!std::getline(ss, key, '\t') || !std::getline(ss, score, '\t') || !std::getline(ss, acc, '\t') ||
            !std::getline(ss, misses, '\t'))
            continue;
        std::getline(ss, rank, '\t');
        std::getline(ss, clear, '\t');
        ScoreEntry e;
        e.score = std::atoll(score.c_str());
        e.acc = std::atof(acc.c_str());
        e.misses = std::atoll(misses.c_str());
        e.rank = rank;
        e.clear = clear;
        map_[key] = e;
    }
}

void Scores::save() const {
    if (path_.empty()) return;
    std::ofstream f(path_);
    for (const auto& [k, e] : map_)
        f << k << '\t' << e.score << '\t' << e.acc << '\t' << e.misses << '\t' << e.rank << '\t' << e.clear << '\n';
}

const ScoreEntry* Scores::get(const std::string& key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

bool Scores::submit(const std::string& key, const ScoreEntry& e) {
    auto it = map_.find(key);
    if (it != map_.end() && it->second.score >= e.score) return false;
    map_[key] = e;
    save();
    return true;
}

}  // namespace gh
