// The "Psych Engine" interface style: notes, receptors, holds, ratings,
// combo numbers, countdown, health bar, font, sounds and menu music taken
// from a Psych Engine / JS Engine copy that the player points the game at.
// None of those files ship with this game; anything missing falls back to
// the game's own look.
#pragma once

#include <cstdint>
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
    // The same, looking in `firstRoots` before the folder: a song's own mod
    // brings its own fonts, health bar, notes, ratings and sounds, as in Psych
    // (Paths looks in the current mod first).
    int load(const std::string& folder, std::string& report, const std::vector<std::string>& firstRoots);
    void unload();
    // Before load(): Psych's "Note Skins" option (images/noteSkins/NOTE_assets-<skin>.png,
    // "" = NOTE_assets) and "Note Colors" (per direction: main, highlight, outline).
    void setNoteSkin(const std::string& skin) { skin_ = skin; }
    void setPalette(const uint32_t colors[4][3]);
    bool active() const { return active_; }
    // Psych's antialiasing option: smooth (bilinear) or sharp scaling of the pictures.
    void setSmooth(bool smooth);
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
    // The rating and digit pictures with their top-left corner at (x, y), as
    // Psych's popUpScore places them; their sizes ({0, 0} when missing).
    Vector2 ratingSize(int which) const;
    Vector2 numSize(int digit) const;
    bool ratingAt(int which, float x, float y, float scale, float alpha) const;
    bool numAt(int digit, float x, float y, float scale, float alpha) const;
    bool countdown(int i, Vector2 c, float alpha) const;  // 0 ready, 1 set, 2 go
    // healthBar.png drawn at `frame` (its own size: healthBarSize(), {0, 0}
    // without it). Psych 0.7's picture is a frame drawn over the colours, 3
    // pixels in; 0.6's is a background under them, 4 pixels in (barInset()).
    Vector2 healthBarSize() const;
    float barInset() const { return barOnTop_ ? 3.0f : 4.0f; }
    bool barOnTop() const { return barOnTop_; }
    // frameAlpha: the picture's own (Psych 0.6 scripts fade healthBarBG on its own); < 0: alpha.
    bool healthBar(Rectangle frame, Color left, Color right, float leftFrac, float alpha, float frameAlpha = -1) const;
    // Psych's time bar (timeBar.png around a black bar and its white fill)
    // at `frame` (timeBarSize()); false when the interface is off.
    Vector2 timeBarSize() const;
    bool timeBar(Rectangle frame, float frac, float alpha, float frameAlpha = -1) const;
    // HUD text in vcr.ttf with Psych's black outline, placed as a FlxText
    // whose box is at (x, y): centred on x when align is 0 (left edge at x:
    // -1, right edge: 1), `size` as FlxText's (Flash's em size, not raylib's
    // line height). False without the font.
    bool text(const std::string& s, float x, float y, float size, Color c, int align = 0, float border = 1.25f) const;
    float textWidth(const std::string& s, float size) const;  // as text() draws it
    float lineHeight(float size) const;                       // a FlxText line (OpenFL's ascent + descent)
    // Alphabet's bold letters (images/alphabet.png), the pause menu's font;
    // `height` is a capital letter's height. Width of the drawn text, or -1
    // without the alphabet.
    float alphabet(const std::string& s, float x, float y, float height, float alpha) const;
    bool hasAlphabet() const { return alphabet_.id != 0; }
    const Font* font() const { return font_.texture.id ? &font_ : nullptr; }
    const std::string& menuMusic() const { return menuMusic_; }
    // The pause menu's song: 0 none, 1 breakfast, 2 tea-time (the other one
    // when the chosen one is missing).
    std::string pauseMusic(int which) const;
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
    std::string root_, skin_;
    uint32_t palette_[4][3] = {{0xC24B99, 0xFFFFFF, 0x3C1F56},
                               {0x00FFFF, 0xFFFFFF, 0x1542B7},
                               {0x12FA05, 0xFFFFFF, 0x0A4447},
                               {0xF9393F, 0xFFFFFF, 0x651038}};
    bool smooth_ = true;
    Texture2D notes_{};
    bool template_ = false;  // RGB template skin (recoloured per direction)
    Shader paletteShader_{};
    int locR_ = -1, locG_ = -1, locB_ = -1;
    std::vector<std::pair<std::string, Frame>> atlas_;  // NOTE_assets frames by name
    Texture2D ratings_[4]{}, nums_[10]{}, count_[3]{}, bar_{}, timeBar_{};
    bool barOnTop_ = true, timeOnTop_ = true;  // Psych 0.7's frames (see barInset())
    Font font_{};
    // raylib sizes a font by its line height (ascent - descent), Flash by its
    // em square: a FlxText of size 20 is drawn at 20 * fontScale_.
    float fontScale_ = 1.0f, fontAscent_ = 1.0f;
    Texture2D alphabet_{};
    std::vector<std::pair<std::string, Frame>> letters_;  // alphabet frames ("A bold0000"...)
    const Frame* letter(char c) const;
    std::vector<std::pair<int, std::string>> sounds_;  // Sfx id -> file
    std::string menuMusic_, pauseMusic_, teaTime_;
};

PsychUi& psychUi();

// Psych Engine / JS Engine copies already on the player's disk: engine builds
// installed as mod packs (mods/), or unpacked in or next to the game's folder.
// The most complete first (an engine's own build before a mod's reskin).
// The game uses the best one when no folder was chosen.
struct PsychFolder {
    std::string path;
    int score = 0;  // pieces of the interface found
};
std::vector<PsychFolder> findPsychFolders(const std::string& gameBase);
int psychFolderScore(const std::string& folder);
// Whether a mod's folders hold any piece of the interface (a font, the health
// bar, a note skin, ratings, sounds...).
bool hasPsychPieces(const std::vector<std::string>& roots);
// The note skins a copy has (images/noteSkins/NOTE_assets-<name>.png), by name.
std::vector<std::string> psychNoteSkins(const std::string& folder);

}  // namespace gh
