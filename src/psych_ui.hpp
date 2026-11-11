// The "Psych Engine" interface style: notes, receptors, holds, ratings,
// combo numbers, countdown, health bar, font, sounds and menu music taken
// from a Psych Engine / JS Engine copy that the player points the game at.
// None of those files ship with this game; anything missing falls back to
// the game's own look.
#pragma once

#include <string>
#include <vector>

#include "raylib.h"

namespace gh {

class PsychUi {
public:
    // Looks for the assets under `folder` (the engine's folder, its assets/
    // folder, or shared/). Returns how many pieces were found; `report`
    // says what is missing.
    int load(const std::string& folder, std::string& report);
    void unload();
    bool active() const { return active_; }
    void setActive(bool on) { active_ = on; }

    // Drawing in canvas units. `size` is a note's width, as in gfx::drawNote.
    // Each returns false when that piece is missing (draw the game's own).
    // Psych 0.7 / JS Engine note skins are colour templates: the notes of a
    // direction are drawn between beginDir and endDir to get their colours.
    void beginDir(int dir) const;
    void endDir() const;
    bool note(int dir, Vector2 c, float size, float alpha, float angle) const;
    bool strum(int dir, Vector2 c, float size, int state, float alpha, float angle, double t) const;  // 0 idle, 1 pressed, 2 hit
    bool hold(int dir, float x, float y0, float y1, float width, float alpha) const;
    bool rating(int which, Vector2 c, float scale, float alpha) const;  // 0 sick .. 3 shit
    bool combo(long long value, Vector2 c, float scale, float alpha) const;
    bool countdown(int i, Vector2 c, float alpha) const;  // 0 ready, 1 set, 2 go
    bool healthBar(Rectangle bar, Color left, Color right, float leftFrac, float alpha) const;
    const Font* font() const { return font_.texture.id ? &font_ : nullptr; }
    const std::string& menuMusic() const { return menuMusic_; }
    void applySounds() const;  // menu and gameplay sounds replace the game's own

private:
    struct Frame {
        Rectangle src{};
        float fx = 0, fy = 0, fw = 0, fh = 0;
        bool rotated = false;
    };
    bool drawFrame(const Frame& f, Vector2 c, float scale, float alpha, float angle, bool flipY = false) const;
    std::vector<Frame> frames(const std::string& prefix) const;

    bool active_ = false;
    std::string root_;
    Texture2D notes_{};
    bool template_ = false;  // RGB template skin (recoloured per direction)
    Shader palette_{};
    int locR_ = -1, locG_ = -1, locB_ = -1;
    std::vector<std::pair<std::string, Frame>> atlas_;  // NOTE_assets frames by name
    Texture2D ratings_[4]{}, nums_[10]{}, count_[3]{}, bar_{};
    Font font_{};
    std::vector<std::pair<int, std::string>> sounds_;  // Sfx id -> file
    std::string menuMusic_;
};

PsychUi& psychUi();

}  // namespace gh
