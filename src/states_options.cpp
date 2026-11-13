// Options menu and audio offset calibration.
#include <array>
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
            {Row::Bool, "Auto pause", "The song pauses when the game's window loses focus (Psych's Auto Pause).", &s.autoPause},
            {Row::Bool, "Disable reset button", "OFF: R during a song makes you lose at once, as in Psych.", &s.noReset},
            {Row::Number, "Rating offset", "How late or early you hit for a Sick!. Higher: you have to hit later (Psych's Rating Offset).",
             nullptr, &s.ratingOffset, -30, 30, 1},
            {Row::Number, "Sick! hit window", "The time you have for the best rating (DIVINE), either side of the note.", nullptr,
             &s.sickWindow, 15, 45, 1},
            {Row::Number, "Good hit window", "The time you have for a Good (GRACE).", nullptr, &s.goodWindow, 15, 90, 1},
            {Row::Number, "Bad hit window", "The time you have for a Bad (MORTAL).", nullptr, &s.badWindow, 15, 135, 1},
            {Row::Number, "Safe frames", "How many frames (of 1/60 s) early or late a note can still be hit (Psych's Safe Frames).",
             nullptr, &s.safeFrames, 2, 10, 0.1f},
            {Row::Number, "Scroll speed", "0 = use each chart's own speed.", nullptr, &s.scrollSpeed, 0, 6, 0.1f},
            {Row::Number, "Audio offset", "Positive if you hear the music late. Hold SHIFT for steps of 10 ms.", nullptr,
             &s.offsetMs, -300, 300, 1},
            {Row::Action, "Calibrate offset...", "Tap along to a metronome to measure your offset."},
            {Row::Bool, "Show timing (ms)", "Shows how early or late each hit was.", &s.showTimings},
            {Row::Bool, "Opponent notes", "OFF hides the opponent's receptors and notes, as Psych's Opponent Notes.", &s.opponentNotes},
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
            {Row::Bool, "Show FPS", "The frame rate at the top left.", &s.showFps},
            {Row::Bool, "FPS: memory", "Under the frame rate: the memory the game uses, and the most it has used.", &s.fpsMemory},
            {Row::Bool, "FPS: frame times", "How long updating and drawing take, the slowest frame of the last second, and a graph.",
             &s.fpsFrameTimes},
            {Row::Bool, "FPS: CPU, GPU and RAM", "How much of the processor the game uses, how busy the graphics card is and its memory, and the system's memory in use.", &s.fpsUsage},
            {Row::Bool, "FPS: hardware", "The processor, graphics card, OpenGL version and system the game runs on.", &s.fpsHardware},
            {Row::Bool, "Low detail", "Fewer particles and effects for slower machines.", &s.lowDetail},
            {Row::Bool, "Flashing lights", "Screen flashes on some beats.", &s.flashing},
            {Row::Bool, "Camera zoom on beat", "Only the beat bops: zoom events from charts and scripts always work.", &s.camZoom},
            {Row::Bool, "Shaders", "Mod shaders and screen effects. OFF is faster on weak devices.", &s.shaders},
            {Row::Bool, "Anti-aliasing", "Smooth scaling of mod pictures. OFF: sharp pixels and a little faster (Psych's Anti-Aliasing).",
             &s.antialiasing},
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
            {Row::Bool, "Hide HUD", "Hides the health bar, the icons, the score line and the ratings, as Psych's Hide HUD.", &s.hideHud},
            {Row::Bool, "Score text zoom on hit", "The score line grows a little on each note you hit, as in Psych.", &s.scoreZoom},
            {Row::Bool, "Botplay text", "BOTPLAY (or REPLAY) on screen while the game plays by itself.", &s.botplayText},
            {Row::Bool, "Combo stacking", "Ratings and combo numbers pile up. OFF: only the latest one, easier to read.",
             &s.comboStacking},
            {Row::Number, "Note splash opacity", "How see-through the bursts on your best hits are.", nullptr, &s.splashAlpha, 0, 1, 0.1f},
            {Row::Choice, "Note skin", "The Psych interface's note skin: Psych's noteSkins/ pictures in your copy.", nullptr, nullptr, 0, 0, 0,
             &noteSkinIdx_},
            {Row::Action, "Note colors...", "The Psych interface's note colours, as in Psych's Note Colors menu."},
            {Row::Number, "Rating position X", "Where the ratings pop up (Psych's Adjust Combo).", nullptr, &s.comboOffset[0], -600, 600, 1},
            {Row::Number, "Rating position Y", "", nullptr, &s.comboOffset[1], -400, 400, 1},
            {Row::Number, "Combo position X", "Where the combo digits pop up.", nullptr, &s.comboOffset[2], -600, 600, 1},
            {Row::Number, "Combo position Y", "", nullptr, &s.comboOffset[3], -400, 400, 1},
            {Row::Choice, "Pause screen song", "The pause menu's music with the Psych Engine interface.", nullptr, nullptr, 0, 0, 0,
             &s.pauseMusic, {0, 1, 2}, {"None", "Breakfast", "Tea Time"}},
            {Row::Header, "RENDERING", ""},
            {Row::Bool, "Rendering mode", "Songs play by themselves and are recorded to a video in renders/, frame by frame, as in JS Engine and "
             "H-Slice: smooth even if the computer cannot keep up. Needs ffmpeg next to the game or installed. ESC stops.", &s.renderMode},
            {Row::Choice, "Video size", "", nullptr, nullptr, 0, 0, 0, &s.renderSize, {0, 1, 2, 3},
             {"1280x720", "1920x1080", "2560x1440", "3840x2160"}},
            {Row::Choice, "Video frame rate", "", nullptr, nullptr, 0, 0, 0, &s.renderFps, {30, 60, 120, 240}},
            {Row::Choice, "Video encoder", "x264 and x265 use the processor; the NVIDIA, AMD and Intel ones use the graphics card "
             "(much faster, when the card has one).", nullptr, nullptr, 0, 0, 0, &s.renderEncoder, {0, 1, 2, 3, 4, 5, 6},
             {"H.264 (x264)", "H.265 (x265)", "VP9 (WebM)", "H.264 NVIDIA", "H.265 NVIDIA", "H.264 AMD", "H.264 Intel"}},
            {Row::Choice, "Video bitrate", "Higher is sharper and bigger. Spam charts move a lot: 20 Mbps or more.", nullptr, nullptr, 0,
             0, 0, &s.renderBitrate, {5, 10, 20, 40, 80}, {"5 Mbps", "10 Mbps", "20 Mbps", "40 Mbps", "80 Mbps"}},
            {Row::Bool, "Video audio", "The song (instrumental and voices) in the video.", &s.renderAudio},
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
            else if (r.label == "Audio offset" || r.label == "Rating offset")
                r.fmt = [](float v) { return std::string(TextFormat("%+d ms", int(std::lround(v)))); };
            else if (r.label.find("hit window") != std::string::npos)
                r.fmt = [](float v) { return std::string(TextFormat("%d ms", int(std::lround(v)))); };
            else if (r.label == "Safe frames") r.fmt = [](float v) { return std::string(TextFormat("%.1f", v)); };
            else if (r.label.find("position") != std::string::npos)
                r.fmt = [](float v) { return std::string(TextFormat("%+d", int(std::lround(v)))); };
            else if (r.kind == Row::Number) r.fmt = pct;
            if (r.label == "Calibrate offset...") r.action = [] { app().change(makeCalibration()); };
            if (r.label == "Psych Engine folder...") r.action = [this] { chooseFolder(); };
            if (r.label == "Note colors...") r.action = [] { app().change(makeNoteColors()); };
            if (r.label == "Note skin") {
                // Default (NOTE_assets) and the skins in the copy's noteSkins/.
                skins_ = psychNoteSkins(app().psychFolderInUse);
                r.names = {"Default"};
                r.choices = {0};
                for (size_t k = 0; k < skins_.size(); ++k) {
                    r.names.push_back(skins_[k]);
                    r.choices.push_back(int(k) + 1);
                    if (skins_[k] == s.noteSkin) noteSkinIdx_ = int(k) + 1;
                }
            }
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
                if (r.kind == Row::Header) controls = r.label.rfind("CONTROLS", 0) == 0 || r.label == "RENDERING";  // no ffmpeg on Android
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
                    if (r.label == "Note skin") {
                        a.settings.noteSkin = noteSkinIdx_ <= 0 || noteSkinIdx_ > int(skins_.size()) ? std::string() : skins_[size_t(noteSkinIdx_ - 1)];
                        a.applyPsychUi();
                    }
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
    int noteSkinIdx_ = 0;              // "Note skin": 0 = Default, k = skins_[k - 1]
    std::vector<std::string> skins_;
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


// ------------------------------------------------------------ note colors --

// Psych's Note Colors: each direction's main colour, highlight and outline
// (the three channels of Psych 0.7's colour-template note skins), changed by
// hue, saturation and brightness. The notes show the Psych interface's skin
// when it is on.
class NoteColorsState : public State {
public:
    void enter() override {
        const Settings& s = app().settings;
        for (int d = 0; d < 4; ++d)
            for (int k = 0; k < 3; ++k) hsv_[d][k] = toHsv(s.noteColors[d][k]);
    }
    void update(double) override {
        App& a = app();
        Input& in = a.input;
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);
        if (in.back() && !in.pressed(KEY_BACKSPACE)) {
            a.saveSettings();
            sfxPlay(Sfx::Cancel);
            a.change(makeOptions());
            return;
        }
        if (in.pressed(KEY_Q) || in.pressed(KEY_PAGE_UP)) { dir_ = (dir_ + 3) % 4; sfxPlay(Sfx::Scroll); }
        if (in.pressed(KEY_E) || in.pressed(KEY_PAGE_DOWN)) { dir_ = (dir_ + 1) % 4; sfxPlay(Sfx::Scroll); }
        if (in.up()) { row_ = (row_ + 8) % 9; sfxPlay(Sfx::Scroll); }
        if (in.down()) { row_ = (row_ + 1) % 9; sfxPlay(Sfx::Scroll); }
        const int step = in.left() ? -1 : in.right() ? 1 : 0;
        if (step) {
            float& v = hsv_[dir_][row_ / 3][row_ % 3];
            v = row_ % 3 == 0 ? std::fmod(v + step * (shift ? 30.0f : 3.0f) + 360.0f, 360.0f)
                              : clampv(v + step * (shift ? 0.1f : 0.02f), 0.0f, 1.0f);
            apply();
        }
        if (in.pressed(KEY_R)) {  // this direction back to Psych's colours
            const Settings d;
            for (int k = 0; k < 3; ++k) hsv_[dir_][k] = toHsv(d.noteColors[dir_][k]);
            apply();
            sfxPlay(Sfx::Confirm);
        }
        // The mouse: a click on a note picks it; dragging a bar sets it.
        const Vector2 m = gfx::toCanvas(GetMousePosition());
        if (in.mousePressed(MOUSE_BUTTON_LEFT))
            for (int d = 0; d < 4; ++d)
                if (CheckCollisionPointRec(m, noteBox(d))) dir_ = d;
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            for (int r = 0; r < 9; ++r) {
                const Rectangle b = bar(r);
                if (!CheckCollisionPointRec(m, {b.x - 4, b.y - 6, b.width + 8, b.height + 12})) continue;
                row_ = r;
                const float f = saturate((m.x - b.x) / b.width);
                hsv_[dir_][r / 3][r % 3] = r % 3 == 0 ? f * 359.9f : f;
                apply();
            }
    }
    void draw() override {
        App& a = app();
        const PsychUi& ui = psychUi();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x5C9CFF, menuPulse());
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.35f));
        gfx::text(FontId::Title, "NOTE COLORS", 60, 22, 48, kGold, Align::Left, 2.0f);
        static const char* dirs[4] = {"LEFT", "DOWN", "UP", "RIGHT"};
        for (int d = 0; d < 4; ++d) {
            const Rectangle b = noteBox(d);
            const Vector2 c{b.x + b.width * 0.5f, b.y + b.height * 0.5f};
            if (d == dir_) DrawRectangleRoundedLinesEx(b, 0.15f, 8, 3, kGold);
            ui.beginDir(d);
            const bool drawn = ui.note(d, c, 120, 1, 0);
            ui.endDir();
            if (!drawn) {  // no Psych skin: the three colours as rings
                DrawCircleV(c, 52, rgb(a.settings.noteColors[d][2]));
                DrawCircleV(c, 44, rgb(a.settings.noteColors[d][0]));
                DrawCircleV({c.x - 14, c.y - 14}, 10, rgb(a.settings.noteColors[d][1]));
            }
            gfx::text(FontId::Ui, dirs[d], c.x, b.y + b.height + 6, 20, d == dir_ ? kGold : kIvory, Align::Center, 1.5f);
        }
        static const char* channels[3] = {"Main", "Highlight", "Outline"};
        static const char* parts[3] = {"Hue", "Saturation", "Brightness"};
        for (int r = 0; r < 9; ++r) {
            const Rectangle b = bar(r);
            const int ch = r / 3, part = r % 3;
            if (part == 0) {
                const uint32_t c = a.settings.noteColors[dir_][ch];
                gfx::text(FontId::Ui, channels[ch], 120, b.y - 4, 24, kIvory, Align::Left, 1.5f);
                DrawRectangleRounded({120, b.y + 26, 120, 40}, 0.3f, 6, rgb(c));
                gfx::text(FontId::Ui, TextFormat("#%06X", unsigned(c)), 180, b.y + 72, 16, kIvory, Align::Center, 1.0f);
            }
            // The bar shows what each value would give.
            const Vector3 h = {hsv_[dir_][ch][0], hsv_[dir_][ch][1], hsv_[dir_][ch][2]};
            for (int i = 0; i < 48; ++i) {
                const float f = (i + 0.5f) / 48.0f;
                Vector3 q = h;
                if (part == 0) q.x = f * 360.0f;
                else if (part == 1) q.y = f;
                else q.z = f;
                DrawRectangleRec({b.x + b.width * i / 48.0f, b.y, b.width / 48.0f + 1, b.height}, ColorFromHSV(q.x, q.y, q.z));
            }
            const float v = part == 0 ? h.x / 360.0f : part == 1 ? h.y : h.z;
            DrawRectangleLinesEx(b, r == row_ ? 2.5f : 1.0f, r == row_ ? kGold : fade(kIvory, 0.5f));
            DrawRectangleRec({b.x + b.width * v - 3, b.y - 5, 6, b.height + 10}, WHITE);
            gfx::text(FontId::Ui, parts[part], b.x - 14, b.y - 1, 18, r == row_ ? kGold : fade(kIvory, 0.8f), Align::Right, 1.0f);
        }
        if (!a.touchMode) {
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
            gfx::text(FontId::Ui, "Q/E or click: note   UP/DOWN: bar   LEFT/RIGHT change (SHIFT faster)   R reset   ESC back", gfx::W / 2,
                      gfx::H - 33, 20, Color{220, 214, 235, 255}, Align::Center);
        }
        gfx::endCanvas();
    }

private:
    std::array<float, 3> hsv_[4][3] = {};
    int dir_ = 0, row_ = 0;

    static Color rgb(uint32_t c) { return Color{(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c, 255}; }
    static std::array<float, 3> toHsv(uint32_t c) {
        const Vector3 v = ColorToHSV(rgb(c));
        return {v.x, v.y, v.z};
    }
    Rectangle noteBox(int d) const { return {300.0f + d * 180.0f, 90, 150, 150}; }
    Rectangle bar(int r) const { return {470, 300.0f + (r / 3) * 120.0f + (r % 3) * 34.0f, 640, 22}; }
    void apply() {
        Settings& s = app().settings;
        for (int k = 0; k < 3; ++k) {
            const float* q = hsv_[dir_][k].data();
            const Color c = ColorFromHSV(q[0], q[1], q[2]);
            s.noteColors[dir_][k] = uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | c.b;
        }
        psychUi().setPalette(s.noteColors);
    }
};

std::unique_ptr<State> makeNoteColors() { return std::make_unique<NoteColorsState>(); }

std::unique_ptr<State> makeOptions() { return std::make_unique<OptionsState>(); }
std::unique_ptr<State> makeCalibration() { return std::make_unique<CalibrationState>(); }

}  // namespace gh
