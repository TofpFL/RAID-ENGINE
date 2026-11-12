// Options menu and audio offset calibration.
#include <cmath>
#include <functional>

#include "app.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "platform.hpp"
#include "psych_ui.hpp"
#include "raylib.h"
#include "synth.hpp"

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {
const Color kGold{255, 214, 120, 255};
const Color kIvory{246, 236, 214, 255};

struct Row {
    enum Kind { Header, Bool, Number, Choice, Action, Key } kind;
    std::string label, help;
    bool* b = nullptr;
    float* f = nullptr;
    float mn = 0, mx = 1, step = 0.1f;
    int* i = nullptr;
    std::vector<int> choices;
    std::vector<std::string> names;  // shown for each choice (default: frames per second)
    std::function<std::string(float)> fmt;
    std::function<void()> action;
    int lane = 0;
};
}  // namespace

class OptionsState : public State {
public:
    void enter() override {
        Settings& s = app().settings;
        auto pct = [](float v) { return std::string(TextFormat("%d%%", int(std::lround(v * 100)))); };
        rows_ = {
            {Row::Header, "GAMEPLAY", ""},
            {Row::Bool, "Downscroll", "Notes fall from the top towards receptors at the bottom.", &s.downscroll},
            {Row::Bool, "Middlescroll", "Centre your lanes and dim the opponent's.", &s.middlescroll},
            {Row::Bool, "Ghost tapping", "When ON, pressing with no note to hit is not punished.", &s.ghostTapping},
            {Row::Number, "Scroll speed", "0 = use each chart's own speed.", nullptr, &s.scrollSpeed, 0, 6, 0.1f},
            {Row::Number, "Audio offset", "Positive if you hear the music late. Hold SHIFT for steps of 10 ms.", nullptr,
             &s.offsetMs, -300, 300, 1},
            {Row::Action, "Calibrate offset...", "Tap along to a metronome to measure your offset."},
            {Row::Bool, "Show timing (ms)", "Shows how early or late each hit was.", &s.showTimings},
            {Row::Bool, "Opponent strums", "Light up the opponent's receptors when they sing.", &s.opponentStrums},
            {Row::Bool, "Note splashes", "Bursts of light on DIVINE hits.", &s.noteSplashes},
            {Row::Bool, "Touch controls", "Tap the lanes on a touchscreen (a mouse click counts as a tap).", &s.touchControls},
            {Row::Bool, "Extra keys", "Charts made for 5 to 9 keys keep them. OFF: every chart has 4 keys, like Psych and JS Engine.",
             &s.extraKeys},
            {Row::Header, "CONTROLS  (4 KEYS)", ""},
            {Row::Key, "Left lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
            {Row::Key, "Down lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
            {Row::Key, "Up lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
            {Row::Key, "Right lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
            {Row::Action, "Reset controls", "Restore D F J K and the arrow keys."},
            {Row::Header, "GRAPHICS", ""},
            {Row::Bool, "Fullscreen", "Borderless fullscreen (F11 works anywhere).", &s.fullscreen},
            {Row::Bool, "VSync", "Waits for the screen's refresh. OFF: the frame limit below decides (most precise input).", &s.vsync},
            {Row::Choice, "Frame limit", "Higher limits make notes smoother. Input is polled at ~1000 Hz regardless.",
             nullptr, nullptr, 0, 0, 0, &s.fpsCap, {60, 120, 144, 165, 240, 360, 500, 0}},
            {Row::Bool, "Show FPS", "", &s.showFps},
            {Row::Bool, "Low detail", "Fewer particles and effects for slower machines.", &s.lowDetail},
            {Row::Bool, "Flashing lights", "Screen flashes on some beats.", &s.flashing},
            {Row::Bool, "Camera zoom on beat", "Only the beat bops: zoom events from charts and scripts always work.", &s.camZoom},
            {Row::Bool, "Shaders", "Mod shaders and screen effects. OFF is faster on weak devices.", &s.shaders},
            {Row::Choice, "Note detail", "Spam charts: Adaptive draws notes stacked a few pixels apart once while the frame rate is "
             "under 60 (every note is still played). Full: every pixel.", nullptr, nullptr, 0, 0, 0, &s.noteDetail, {0, 1},
             {"Adaptive", "Full"}},
            {Row::Header, "INTERFACE", ""},
            {Row::Choice, "Note counter", "Notes hit so far, under the score: your side only, or both sides added up.", nullptr,
             nullptr, 0, 0, 0, &s.noteCounter, {0, 1, 2}, {"Off", "Your side", "Both sides"}},
            {Row::Bool, "Psych Engine interface", "Psych's notes, ratings, health and time bars, fonts, pause menu and sounds, from your own copy.",
             &s.psychUi},
            {Row::Action, "Psych Engine folder...", ""},  // BACKSPACE: back to the copy found automatically
            {Row::Bool, "Hit popups", "Rating pictures and combo numbers that pop up on each hit.", &s.hitPopups},
            {Row::Choice, "Time bar", "The bar at the top of the screen, as in Psych.", nullptr, nullptr, 0, 0, 0, &s.timeBar,
             {0, 1, 2, 3}, {"Time left", "Time elapsed", "Song name", "Off"}},
            {Row::Number, "Health bar opacity", "The health bar and the icons.", nullptr, &s.healthBarAlpha, 0, 1, 0.1f},
            {Row::Header, "AUDIO", ""},
            {Row::Number, "Master volume", "", nullptr, &s.masterVol, 0, 1, 0.05f},
            {Row::Number, "Music volume", "", nullptr, &s.musicVol, 0, 1, 0.05f},
            {Row::Number, "Sound effects", "", nullptr, &s.sfxVol, 0, 1, 0.05f},
            {Row::Number, "Hit sounds", "A tick on every note you hit.", nullptr, &s.hitsoundVol, 0, 1, 0.05f},
            {Row::Header, "DEBUG", ""},
            {Row::Choice, "Lua errors", "Errors of mod scripts on screen. Psych shows them only for scripts that set luaDebugMode.",
             nullptr, nullptr, 0, 0, 0, &s.luaErrors, {0, 1, 2}, {"Always", "luaDebugMode", "Off"}},
            {Row::Choice, "Lua warnings", "Missing images, sounds and objects that mods ask for. Psych: only with luaDebugMode.",
             nullptr, nullptr, 0, 0, 0, &s.luaWarnings, {0, 1, 2}, {"Always", "luaDebugMode", "Off"}},
            {Row::Bool, "Lua note callbacks", "Scripts hear every hit and miss (goodNoteHit, noteMiss...). OFF is faster on spam "
             "charts, but mods that count notes or drain health with them stop doing it.", &s.luaNoteCallbacks},
        };
        for (Row& r : rows_) {
            if (r.label == "Scroll speed") r.fmt = [](float v) { return v <= 0.001f ? std::string("Chart") : std::string(TextFormat("%.1f", v)); };
            else if (r.label == "Audio offset") r.fmt = [](float v) { return std::string(TextFormat("%+d ms", int(std::lround(v)))); };
            else if (r.kind == Row::Number) r.fmt = pct;
            if (r.label == "Calibrate offset...") r.action = [] { app().change(makeCalibration()); };
            if (r.label == "Psych Engine folder...") r.action = [this] { chooseFolder(); };
            if (r.label == "Reset controls")
                r.action = [&s] {
                    const Settings d;
                    for (int l = 0; l < 4; ++l)
                        for (int k = 0; k < 2; ++k) s.keys4[l][k] = d.keys4[l][k];
                    app().saveSettings();
                };
        }
        int lane = 0;
        for (Row& r : rows_)
            if (r.kind == Row::Key) r.lane = lane++;
        if (platform::kAndroid) {
            // No keyboard and no window: the lanes are the screen itself.
            bool controls = false;
            std::vector<Row> kept;
            for (Row& r : rows_) {
                if (r.kind == Row::Header) controls = r.label.rfind("CONTROLS", 0) == 0;
                if (controls || r.label == "Fullscreen" || r.label == "Touch controls") continue;
                kept.push_back(std::move(r));
            }
            rows_ = std::move(kept);
        }
        sel_ = 1;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        anim_ = approach(anim_, float(sel_), 12.0f, float(dt));
        // A folder dropped on the window, or the Android picker finishing.
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0 && DirectoryExists(dropped.paths[0])) setFolder(dropped.paths[0]);
            UnloadDroppedFiles(dropped);
        }
        if (pickingFolder_) {
            int n = 0;
            const platform::Import st = platform::importStatus(&n);
            if (st == platform::Import::Done || st == platform::Import::Nothing) {
                pickingFolder_ = false;
                platform::resetImport();
                if (st == platform::Import::Done) setFolder(platform::dataDir() + "psych-ui/");
                else if (a.settings.psychFolder.empty()) a.settings.psychUi = false;
            }
        }
        refreshFolderHelp();
        Row& r = rows_[size_t(sel_)];
        if (waiting_) {
            const int code = in.firstPressed();
            if (code == KEY_ESCAPE) { waiting_ = false; sfxPlay(Sfx::Cancel); }
            else if (code != 0) {
                a.settings.keys4[r.lane][slot_] = code == KEY_BACKSPACE ? 0 : code;
                waiting_ = false;
                sfxPlay(Sfx::Confirm);
                a.saveSettings();
            }
            return;
        }
        if (in.up()) move(-1);
        if (in.down()) move(1);
        const int dir = in.left() ? -1 : (in.right() ? 1 : 0);
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);
        bool changed = false;
        switch (r.kind) {
            case Row::Bool:
                if (dir || in.accept()) {
                    if (r.label == "Fullscreen") a.toggleFullscreen();
                    else *r.b = !*r.b;
                    if (r.label == "VSync") platform::setVsync(a.settings.vsync);
                    if (r.label == "Psych Engine interface") {
                        a.applyPsychUi();  // no folder chosen: the best copy found among the mods
                        if (a.settings.psychUi && a.settings.psychFolder.empty() && !psychUi().active()) chooseFolder();
                    }
                    changed = true;
                }
                break;
            case Row::Number:
                if (dir) {
                    *r.f = clampv(*r.f + dir * r.step * (shift ? 10.0f : 1.0f), r.mn, r.mx);
                    if (r.step < 1) *r.f = std::round(*r.f / r.step) * r.step;
                    changed = true;
                }
                break;
            case Row::Choice:
                if (dir) {
                    size_t idx = 0;
                    for (size_t k = 0; k < r.choices.size(); ++k)
                        if (r.choices[k] == *r.i) idx = k;
                    idx = (idx + r.choices.size() + size_t(dir)) % r.choices.size();
                    *r.i = r.choices[idx];
                    changed = true;
                }
                break;
            case Row::Action:
                if (in.accept()) { sfxPlay(Sfx::Confirm); r.action(); }
                else if (r.label == "Psych Engine folder..." && in.pressed(KEY_BACKSPACE) && !a.settings.psychFolder.empty()) {
                    a.settings.psychFolder.clear();  // back to the copy found among the mods
                    a.applyPsychUi();
                    a.saveSettings();
                    sfxPlay(Sfx::Cancel);
                }
                break;
            case Row::Key:
                if (dir) { slot_ = 1 - slot_; sfxPlay(Sfx::Scroll); }
                if (in.pressed(KEY_BACKSPACE)) { a.settings.keys4[r.lane][slot_] = 0; a.saveSettings(); }
                else if (in.accept()) { waiting_ = true; sfxPlay(Sfx::Scroll); }
                break;
            default: break;
        }
        if (changed) {
            sfxPlay(Sfx::Scroll);
            a.applyVolumes();
            a.saveSettings();
        }
        if (in.back() && !in.pressed(KEY_BACKSPACE)) {
            a.saveSettings();
            sfxPlay(Sfx::Cancel);
            if (a.optionsReturn) {  // opened from a song's pause menu: back to that song
                const PlayRequest req = *a.optionsReturn;
                a.optionsReturn.reset();
                a.change(makePlay(req));
            } else {
                a.change(makeMainMenu());
            }
        }
    }

    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x5C9CFF, menuPulse());
        gfx::text(FontId::Title, "OPTIONS", 60, 22, 48, kGold, Align::Left, 2.0f);
        for (int i = 0; i < int(rows_.size()); ++i) {
            const float y = 330 + (float(i) - anim_) * 48;
            if (y < 80 || y > 640) continue;
            const Row& r = rows_[size_t(i)];
            const bool on = i == sel_;
            const float alpha = saturate(1.0f - std::fabs(float(i) - anim_) * 0.1f);
            if (r.kind == Row::Header) {
                gfx::text(FontId::Title, r.label, 140, y + 2, 28, fade(kGold, alpha), Align::Left, 1.5f);
                DrawRectangleRec({140, y + 38, 1000, 2}, fade(kGold, 0.4f * alpha));
                continue;
            }
            if (on) DrawRectangleRounded({150, y - 4, 980, 44}, 0.3f, 8, Color{255, 214, 120, 40});
            gfx::text(FontId::Ui, r.label, 180, y, 34, fade(on ? WHITE : kIvory, alpha), Align::Left, 1.5f);
            std::string val;
            switch (r.kind) {
                case Row::Bool: val = (r.label == "Fullscreen" ? a.settings.fullscreen : *r.b) ? "ON" : "OFF"; break;
                case Row::Number: val = r.fmt(*r.f); break;
                case Row::Choice:
                    if (!r.names.empty()) {
                        for (size_t k = 0; k < r.choices.size() && k < r.names.size(); ++k)
                            if (r.choices[k] == *r.i) val = r.names[k];
                    } else {
                        val = *r.i == 0 ? std::string("Unlimited") : std::string(TextFormat("%d FPS", *r.i));
                    }
                    break;
                case Row::Action: val = ">"; break;
                case Row::Key: {
                    for (int k = 0; k < 2; ++k) {
                        const bool hl = on && k == slot_;
                        std::string name = (hl && waiting_) ? "press a key..." : keyName(a.settings.keys4[r.lane][k]);
                        const float x = 820.0f + k * 170.0f;
                        DrawRectangleRounded({x - 75, y - 2, 150, 40}, 0.3f, 8, hl ? Color{255, 214, 120, 90} : Color{0, 0, 0, 90});
                        gfx::text(FontId::Ui, name, x, y + 2, 30, fade(hl ? WHITE : kIvory, alpha), Align::Center);
                    }
                    gfx::drawNote(r.lane, {700, y + 18}, 40, alpha);
                    continue;
                }
                default: break;
            }
            gfx::text(FontId::Ui, (on && r.kind != Row::Action ? "<  " : "") + val + (on && r.kind != Row::Action ? "  >" : ""),
                      1100, y, 34, fade(on ? kGold : kIvory, alpha), Align::Right, 1.5f);
        }
        const Row& r = rows_[size_t(sel_)];
        DrawRectangleRec({0, gfx::H - 84, gfx::W, 44}, Color{0, 0, 0, 120});
        gfx::text(FontId::Ui, r.help, gfx::W / 2, gfx::H - 76, 24, kIvory, Align::Center);
        if (!a.touchMode) {
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
            gfx::text(FontId::Ui, "UP/DOWN select   LEFT/RIGHT change   ENTER toggle   ESC save and back", gfx::W / 2, gfx::H - 33,
                      22, Color{220, 214, 235, 255}, Align::Center);
        }
        gfx::endCanvas();
    }

private:
    void chooseFolder() {
        App& a = app();
        if (platform::kAndroid) {
            pickingFolder_ = true;
            platform::pickFolder("Choose your Psych Engine or JS Engine folder");
            return;
        }
        const std::string path = platform::pickFolder("Choose your Psych Engine or JS Engine folder");
        if (!path.empty()) setFolder(path);
        else if (a.settings.psychFolder.empty()) {
            a.settings.psychUi = false;
            a.psychReport = "No folder chosen (you can also drop the folder on this window)";
        }
    }
    void setFolder(const std::string& path) {
        App& a = app();
        a.settings.psychFolder = path;
        a.settings.psychUi = true;
        a.applyPsychUi();
        a.saveSettings();
        sfxPlay(Sfx::Confirm);
    }
    void refreshFolderHelp() {
        const App& a = app();
        for (Row& r : rows_)
            if (r.label == "Psych Engine folder...") {
                if (pickingFolder_) r.help = "Copying the interface files...";
                else if (a.settings.psychFolder.empty() && !a.psychFolderInUse.empty())
                    r.help = "Found among your mods: " + platform::displayPath(a.psychFolderInUse) + ". Pick another folder, or drop one on this window.";
                else if (a.settings.psychFolder.empty())
                    r.help = "None found among your mods (engine packs such as JS Engine or H-Slice have one). Pick the folder, or drop it on this window.";
                else r.help = platform::displayPath(a.settings.psychFolder) + (a.psychReport.empty() ? "" : "  (" + a.psychReport + ")") +
                              "  BACKSPACE: the copy found among your mods.";
            }
    }
    bool pickingFolder_ = false;

    void move(int d) {
        const int n = int(rows_.size());
        do { sel_ = (sel_ + d + n) % n; } while (rows_[size_t(sel_)].kind == Row::Header);
        sfxPlay(Sfx::Scroll);
    }
    std::vector<Row> rows_;
    int sel_ = 1, slot_ = 0;
    float anim_ = 1;
    bool waiting_ = false;

public:
    bool keyReserved(int) const override { return waiting_; }  // rebinding: the next key is the binding
};

// ------------------------------------------------------------ Calibration --

class CalibrationState : public State {
public:
    bool wantsMenuMusic() const override { return false; }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.tapCode = KEY_SPACE;  // tap anywhere on the beat
        t.keys = {{"APPLY", KEY_ENTER}, {"RESET", KEY_R}, {"CANCEL", KEY_ESCAPE}};
        return t;
    }
    void enter() override {
        App& a = app();
        a.stopMenuMusic();
        // 16 beats at 120 BPM, accent on every bar.
        synth::Stereo s(size_t(8.0 * synth::SR));
        for (int b = 0; b < 16; ++b) {
            synth::bell(s, nullptr, b * 0.5, b % 4 == 0 ? 88 : 81, 0.9f, 0);
            synth::kick(s, b * 0.5, 0.5f, 0.6f);
        }
        auto pcm = std::make_shared<Pcm>();
        pcm->channels = 2;
        pcm->data = synth::toPcm16(s, 0.8f);
        a.mixer.setTracks(pcm, nullptr, nullptr);
        a.mixer.setRate(1.0);
        a.mixer.setLoop(true, 0, 8.0);
        a.mixer.play(0);
    }
    void update(double) override {
        App& a = app();
        const double clock = a.mixer.time(a.now) * 1000.0;
        for (const InputEvent& e : a.input.events()) {
            if (!e.down) continue;
            if (e.code == KEY_ESCAPE || e.code == KEY_BACKSPACE) {
                sfxPlay(Sfx::Cancel);
                a.change(makeOptions());
                return;
            }
            if (e.code == KEY_ENTER) {
                if (taps_.size() >= 4) {
                    a.settings.offsetMs = float(std::lround(mean()));
                    a.saveSettings();
                    sfxPlay(Sfx::Confirm);
                }
                a.change(makeOptions());
                return;
            }
            if (e.code == KEY_R) { taps_.clear(); continue; }
            const double t = clock - (a.now - e.t) * 1000.0;
            const double k = std::round(t / 500.0);
            const double diff = t - k * 500.0;
            taps_.push_back(diff);
            if (taps_.size() > 24) taps_.erase(taps_.begin());
            lastTap_ = a.now;
        }
        clock_ = clock;
    }
    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x5CC8FF, 0);
        gfx::text(FontId::Title, "CALIBRATE OFFSET", gfx::W / 2, 40, 48, kGold, Align::Center, 2.0f);
        gfx::text(FontId::Ui,
                  a.touchMode ? "Tap the screen on every beat you HEAR (not see). Keep going for a few bars."
                              : "Tap any key on every beat you HEAR (not see). Keep going for a few bars.",
                  gfx::W / 2, 110, 28, kIvory, Align::Center, 1.5f);
        // visual metronome, compensated with the current offset
        const double vis = clock_ - a.settings.offsetMs;
        const double frac = std::fmod(vis / 500.0 + 1000.0, 1.0);
        const float pulse = float(std::exp(-frac * 7.0));
        gfx::glow({640, 330}, 120 + 60 * pulse, fade(kGold, 0.4f + 0.4f * pulse));
        DrawCircleV({640, 330}, 50 + 14 * pulse, fade(kGold, 0.85f));
        gfx::drawReceptor(gfx::kDown, {640, 520}, 90, WHITE);
        for (int k = -1; k < 6; ++k) {
            const double beatT = (std::floor(vis / 500.0) + k) * 500.0;
            const float y = 520 + float(beatT - vis) * 0.45f * 2.0f;
            if (y > 520 - 60 && y < 760) gfx::drawNote(gfx::kDown, {640, y}, 90);
        }
        std::string res = "Taps: 0";
        if (!taps_.empty()) {
            double sd = 0;
            const double m = mean();
            for (double t : taps_) sd += (t - m) * (t - m);
            sd = std::sqrt(sd / double(taps_.size()));
            res = TextFormat("Average: %+.0f ms    Spread: %.0f ms    Taps: %d", m, sd, int(taps_.size()));
        }
        gfx::text(FontId::Ui, res, gfx::W / 2, 410, 34, WHITE, Align::Center, 1.5f);
        gfx::text(FontId::Ui, TextFormat("Current offset: %+d ms", int(a.settings.offsetMs)), gfx::W / 2, 450, 24,
                  fade(kIvory, 0.7f), Align::Center);
        DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
        gfx::text(FontId::Ui, a.touchMode ? "APPLY needs 4+ taps" : "ENTER apply (needs 4+ taps)   R reset   ESC cancel", gfx::W / 2,
                  gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
        gfx::endCanvas();
    }

private:
    double mean() const {
        double s = 0;
        for (double t : taps_) s += t;
        return taps_.empty() ? 0 : s / double(taps_.size());
    }
    std::vector<double> taps_;
    double clock_ = 0, lastTap_ = 0;
};

std::unique_ptr<State> makeOptions() { return std::make_unique<OptionsState>(); }
std::unique_ptr<State> makeCalibration() { return std::make_unique<CalibrationState>(); }

}  // namespace gh
