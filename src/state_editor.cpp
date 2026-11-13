// Chart editor (Freeplay: 7). A Psych / JS Engine style grid: an event lane,
// the opponent's lanes, then the player's, time flowing down past a fixed
// line; tabs on the right edit the song, the section under the line, the
// selected notes, the events and playback. Saves legacy/Psych 0.x JSON that
// this game, Psych Engine and JS Engine all read.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <random>

#include "app.hpp"
#include "audio_decode.hpp"
#include "chart_stream.hpp"
#include "flp_import.hpp"
#include "common.hpp"
#include "editor_ui.hpp"
#include "gfx.hpp"
#include "raylib.h"
#include "scene.hpp"

namespace gh {

namespace {
namespace fs = std::filesystem;
using gfx::Align;
using gfx::FontId;
using namespace edui;

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

const char* kBaseNoteTypes[] = {"", "Alt Animation", "Hurt Note", "GF Sing", "No Animation", "Hey!"};
const int kSnaps[] = {4, 8, 12, 16, 24, 32, 48, 64, 96, 192};

// Built-in events (Psych Engine and JS Engine), with what their values do.
struct EventInfo {
    const char* name;
    const char* help;
};
const EventInfo kEvents[] = {
    {"Hey!", "Value 1: BF, GF or empty for both. Value 2: seconds (default 0.6)."},
    {"Set GF Speed", "Value 1: beats per head bop (1 normal, 2 half speed...)."},
    {"Add Camera Zoom", "Value 1: game zoom to add (0.015). Value 2: HUD zoom to add (0.03)."},
    {"Set Camera Zoom", "Value 1: new default zoom ('default' or empty: the stage's)."},
    {"Tween Camera Zoom", "Value 1: zoom ('default' or empty: the stage's). Value 2: 'seconds, ease'."},
    {"Camera Bopping", "Value 1: beats between bops (4). Value 2: strength (1)."},
    {"Enable Camera Bop", "The camera bops on beats from here on."},
    {"Disable Camera Bop", "The camera stops bopping and returns to its zoom."},
    {"Camera Twist", "Value 1: strength. Value 2: 0 stops the twist, anything else starts it."},
    {"Camera Follow Pos", "Value 1: X, Value 2: Y. Both empty: back to following the singer."},
    {"Screen Shake", "Values 1 (camera) and 2 (HUD): 'seconds, strength', e.g. '1, 0.05'."},
    {"Play Animation", "Value 1: animation. Value 2: BF, GF or Dad (default Dad)."},
    {"Alt Idle Animation", "Value 1: BF, GF or Dad. Value 2: idle suffix, e.g. -alt."},
    {"Change Character", "Value 1: BF, GF or Dad. Value 2: the new character's name."},
    {"Change Scroll Speed", "Value 1: speed multiplier (1). Value 2: seconds to get there."},
    {"Change Note Multiplier", "Value 1: notes counted per hit. Value 2: 1 opponent, 2 player, empty both."},
    {"Fake Song Length", "Value 1: seconds shown on the time bar (empty: the real length). Value 2: 'true' to tween."},
    {"Change Song Name", "Value 1: name on the time bar (empty: the real one)."},
    {"Credits Popup", "Value 1: title (empty: the song). Value 2: composer(s)."},
    {"Popup", "Value 1: title. Value 2: message. The song waits until it is closed."},
    {"Popup (No Pause)", "Value 1: title. Value 2: message. The song keeps going."},
    {"Windows Notification", "Value 1: title. Value 2: message (shown in the game)."},
    {"Rainbow Eyesore", "Flashing colours until a step. Value 1: last step. Value 2: speed."},
    {"Enable Bot Energy", "The player can hold CTRL (BOT on touch) to let the bot play while energy lasts."},
    {"Disable Bot Energy", "Turns bot energy off."},
    {"Set Bot Energy Speeds", "Value 1: drain speed (1). Value 2: refill speed (1)."},
    {"Set Property", "Value 1: property (e.g. camGame.alpha). Value 2: new value."},
    {"Play Sound", "Value 1: sound file. Value 2: volume (1)."},
    {"Philly Glow", "Week 3 only. Value 1: 0 off, 1 on, 2 new colour."},
    {"Kill Henchmen", "Week 4 only."},
    {"BG Freaks Expression", "Week 6 (school stage) only."},
    {"Trigger BG Ghouls", "Week 6 (evil school stage) only."},
    {"Dadbattle Spotlight", "Week 1 only. Value 1: 0/1 off/on, 2 Dad, 3 BF."},
};

struct EdNote {
    double t = 0;
    int col = 0;  // 0..K-1 opponent, K..2K-1 player
    double len = 0;
    std::string type;
    int stack = 1;  // notes of the file stacked here (written back that many times)
    bool sel = false;
};

struct EdSection {
    bool mustHit = true, altAnim = false, gf = false;
    double bpm = 120, beats = 4;
};

bool writeWav(const std::string& path, const Pcm& p) { return writeWavFile(path, p); }



// Immediate-mode widgets for the side panel (canvas coordinates).

}  // namespace

class ChartEditorState : public State {
public:
    ChartEditorState(std::string songId, std::string diff) : songId_(std::move(songId)), diff_(std::move(diff)) {}

    void enter() override {
        App& a = app();
        a.stopMenuMusic();
        a.mixer.stop();
        entry_ = a.library.find(songId_);
        if (!entry_) {
            error_ = "song not found";
            return;
        }
        const SongEntry copy = *entry_;
        const std::string d = diff_;
        job_ = std::async(std::launch::async, [copy, d] { return app().library.prepare(copy, d, false); });
    }

    bool wantsMenuMusic() const override { return false; }
    // - and = change the playback speed here; every key while typing.
    bool keyReserved(int key) const override { return typing_ || key == KEY_MINUS || key == KEY_EQUAL; }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        if (loaded_ && error_.empty())
            t.keys = {{"PLAY", KEY_SPACE}, {"SAVE", KEY_F2}, {"UNDO", KEY_F3}, {"BACK", KEY_ESCAPE}};
        return t;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        if (job_.valid()) {
            if (job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            Prepared p = job_.get();
            if (!p.ok) error_ = p.error.empty() ? "cannot load the chart" : p.error;
            else load(p);
        }
        if (!error_.empty() || !loaded_) {
            if (in.back() || in.accept()) a.change(makeFreeplay());
            if (!error_.empty() && !a.autoRun.editorExport.empty()) a.quit = true;
            return;
        }
        if (!a.autoRun.editorExport.empty()) {  // testing: write the chart and quit
            std::ofstream(toPath(a.autoRun.editorExport), std::ios::binary) << buildJson(eventsInChart());
            a.quit = true;
            return;
        }
        (void)dt;
        // SNIFF in the editor: an FL Studio project or score dropped on the window.
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            for (unsigned i = 0; i < dropped.count; ++i) {
                const std::string f = dropped.paths[i];
                if (endsWithI(f, ".flp") || endsWithI(f, ".fsc")) importFlp(f);
            }
            UnloadDroppedFiles(dropped);
        }
        ui_.in = &in;
        ui_.mouse = gfx::toCanvas(GetMousePosition());
        ui_.click = in.mousePressed(MOUSE_BUTTON_LEFT) && !(a.touchMode && a.touch.covers(GetMousePosition()));
        ui_.rclick = in.mousePressed(MOUSE_BUTTON_RIGHT);
        ui_.wheel = in.wheel();
        ui_.used = false;
        typing_ = !ui_.focus.empty();
        const bool ctrl = in.isDown(KEY_LEFT_CONTROL) || in.isDown(KEY_RIGHT_CONTROL);
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);

        // Playback
        if (playing_) {
            const double t = a.mixer.time(a.now) * 1000.0;
            tickSounds(lastT_, t);
            lastT_ = t;
            cur_ = t;
            if (a.mixer.ended() || cur_ > songLength() + 2000) stopPlay();
        }
        if (typing_) {
            if (in.pressed(KEY_ESCAPE)) ui_.focus.clear();
            return;  // keys go to the text field (the panel still runs in draw)
        }
        if (in.pressed(KEY_SPACE)) togglePlay();

        // Navigation (the wheel scrolls the grid unless it is over the panel)
        const bool overPanel = ui_.mouse.x >= kPanelX - 10;
        const float wheel = overPanel ? 0.0f : in.wheel();
        if (wheel != 0 || in.pressed(KEY_UP) || in.pressed(KEY_DOWN) || in.pressed(KEY_W) || in.pressed(KEY_S)) {
            if (!(ctrl && in.pressed(KEY_S))) {
                if (playing_) stopPlay();
                int steps = wheel != 0 ? -int(std::round(wheel)) : (in.pressed(KEY_UP) || in.pressed(KEY_W) ? -1 : 1);
                if (shift) steps *= 4;
                const double b = snapBeat(beatAt(cur_)) + steps * snapBeats();
                cur_ = std::max(0.0, msAtBeat(b));
            }
        }
        const int jump = shift ? 4 : 1;  // SHIFT: four sections at a time
        if (in.pressed(KEY_A) && !ctrl) gotoSection(std::max(0, sectionAt(cur_) - jump));
        if (in.pressed(KEY_D) || in.pressed(KEY_PAGE_DOWN)) gotoSection(sectionAt(cur_) + jump);
        if (in.pressed(KEY_PAGE_UP)) gotoSection(sectionAt(cur_) - 1);
        if (in.pressed(KEY_HOME)) cur_ = 0;
        if (in.pressed(KEY_END)) cur_ = songLength();
        if (in.pressed(KEY_LEFT)) snapIdx_ = std::max(0, snapIdx_ - 1);
        if (in.pressed(KEY_RIGHT)) snapIdx_ = std::min(int(std::size(kSnaps)) - 1, snapIdx_ + 1);
        if (in.pressed(KEY_Z) && !ctrl) zoom_ = std::max(0.25f, zoom_ / 1.25f);
        if (in.pressed(KEY_X)) zoom_ = std::min(8.0f, zoom_ * 1.25f);

        // Editing
        if ((ctrl && in.pressed(KEY_Z)) || in.pressed(KEY_F3)) undo();
        if (ctrl && in.pressed(KEY_Y)) redo();
        if ((ctrl && in.pressed(KEY_S)) || in.pressed(KEY_F2)) save(false);
        if (ctrl && in.pressed(KEY_A)) selectSection();
        if (in.pressed(KEY_TAB)) {
            snapshot();
            EdSection& s = section(sectionAt(cur_));
            s.mustHit = !s.mustHit;
            dirty_ = true;
        }
        if (in.pressed(KEY_T)) {
            typeIdx_ = (typeIdx_ + 1) % int(noteTypes_.size());
            applyToSelection([&](EdNote& n) { n.type = noteTypes_[size_t(typeIdx_)]; });
        }
        if (in.pressed(KEY_Q) || in.pressed(KEY_E)) {
            const bool longer = in.pressed(KEY_E);
            applyToSelection([&](EdNote& n) {
                const double stepMs = 60000.0 / bpmAt(n.t) * snapBeats();
                n.len = std::max(0.0, n.len + (longer ? stepMs : -stepMs));
            });
        }
        if (in.pressed(KEY_LEFT_BRACKET) || in.pressed(KEY_RIGHT_BRACKET)) {
            double nb = section(sectionAt(cur_)).bpm + (in.pressed(KEY_RIGHT_BRACKET) ? 1 : -1) * (shift ? 0.1 : 1.0);
            setSectionBpm(sectionAt(cur_), nb);
        }
        if (in.pressed(KEY_MINUS)) { speed_ = std::max(0.1, std::round((speed_ - 0.1) * 10) / 10); dirty_ = true; }
        if (in.pressed(KEY_EQUAL)) { speed_ = std::min(10.0, std::round((speed_ + 0.1) * 10) / 10); dirty_ = true; }
        if (ctrl && in.pressed(KEY_C)) copySelectionOrSection();
        if (ctrl && in.pressed(KEY_X)) cutSelectionOrSection();
        if (ctrl && in.pressed(KEY_V)) paste();
        if (in.pressed(KEY_M)) mirror();
        if (in.pressed(KEY_DELETE)) {
            if (anySelected()) deleteSelection();
            else clearSection(true, false);
        }
        if (in.pressed(KEY_ENTER) || in.pressed(KEY_KP_ENTER)) {
            if (save(true)) return;
        }
        if (in.back() && !in.pressed(KEY_BACKSPACE)) {
            if (dirty_ && a.now - escAt_ > 2.0) {
                escAt_ = a.now;
                flash("Unsaved changes: ESC again to leave, Ctrl+S to save");
            } else {
                stopPlay();
                a.change(makeFreeplay());
                return;
            }
        }
    }

    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        ClearBackground(Color{14, 10, 22, 255});
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{14, 10, 22, 255});
        if (!error_.empty()) {
            gfx::text(FontId::Ui, "Chart editor: " + error_, gfx::W / 2, gfx::H / 2 - 20, 28, gfx::rgb(0xFF8080), Align::Center);
            gfx::text(FontId::Ui, "ESC back", gfx::W / 2, gfx::H / 2 + 24, 22, gfx::fade(WHITE, 0.6f), Align::Center);
            gfx::endCanvas();
            return;
        }
        if (!loaded_) {
            gfx::text(FontId::Ui, "Loading chart...", gfx::W / 2, gfx::H / 2 - 14, 28, WHITE, Align::Center);
            gfx::endCanvas();
            return;
        }
        drawGrid();
        drawInfo();
        drawPanel();  // also handles the panel's clicks (immediate mode)
        if (!ui_.used && !typing_) handleGridMouse();
        if (a.now - flashAt_ < 3.0)
            gfx::text(FontId::Ui, flash_, kGridCenter, gfx::H - 40, 24,
                      gfx::fade(gfx::rgb(0xFFE08A), float(std::min(1.0, 3.0 - (a.now - flashAt_)))), Align::Center, 1.5f);
        gfx::endCanvas();
    }

private:
    static constexpr float kPanelX = 880, kGridCenter = 555;
    struct UndoStep {
        std::vector<EdNote> notes;
        std::vector<EdSection> sections;
        std::vector<psych::ChartEvent> events;
        double speed = 1;
    };


    // --------------------------------------------------------- loading --
    void load(const Prepared& p) {
        const Chart& c = p.chart;
        keys_ = c.keys;
        speed_ = c.speed;
        title_ = c.title.empty() ? entry_->title : c.title;
        player_ = c.player.empty() ? "bf" : c.player;
        opponent_ = c.opponent.empty() ? "dad" : c.opponent;
        gf_ = c.gf;
        stage_ = c.stage;
        inst_ = p.inst;
        voiceP_ = p.voiceP;
        voiceO_ = p.voiceO;
        needsVoices_ = voiceP_ || voiceO_;
        const double audioMs = inst_ ? double(inst_->frames()) * 1000.0 / Mixer::kRate : 0.0;
        const double endMs = std::max({c.lengthMs + 4000.0, audioMs, 8000.0});
        if (!c.sections.empty()) {
            // The chart's own sections, lengths and tempos included.
            for (const SectionInfo& si : c.sections) {
                EdSection s;
                s.mustHit = si.mustHit;
                s.gf = si.gf;
                s.altAnim = si.alt;
                s.beats = si.beats > 0 ? si.beats : 4;
                s.bpm = si.bpm > 0 ? si.bpm : c.bpmAt(si.timeMs + 0.01);
                sections_.push_back(s);
            }
        } else {
            // Sections of 4 beats with the BPM in force at their start.
            double t = 0;
            while (t < endMs && sections_.size() < 200000) {
                EdSection s;
                s.bpm = c.bpmAt(t + 0.01);
                s.mustHit = c.focusAt(t + 1) == 1;
                sections_.push_back(s);
                t += 4 * 60000.0 / s.bpm;
            }
        }
        notes_.reserve(c.notes.size());
        for (const Note& n : c.notes) {
            EdNote e;
            e.t = n.time;
            e.col = n.side ? keys_ + n.lane : n.lane;
            e.len = n.length;
            e.type = n.type < c.noteTypes.size() ? c.noteTypes[n.type] : "";
            if (e.type.empty() && n.kind == kMine) e.type = "Hurt Note";
            e.stack = std::max<int>(1, n.stack);
            notes_.push_back(e);
        }
        sortNotes();
        events_ = c.events;
        sortEvents();
        rebuildStarts();
        // Psych keeps events in events.json when the song has one: keep doing so.
        if (!entry_->dataDir.empty()) {
            std::error_code ec;
            for (const auto& f : fs::directory_iterator(toPath(entry_->dataDir), ec))
                if (equalsI(pathStr(f.path().filename()), "events.json")) eventsFile_ = pathStr(f.path());
        }
        scanNames(c);
        loaded_ = true;
        App& a = app();
        a.mixer.setTracks(inst_, voiceP_, voiceO_);
        a.mixer.setRate(1.0);
        if (entry_->builtin) flash("Built-in song: saving makes an editable copy in songs/");
        else if (entry_->autoChart) flash("AutoChart draft: save to keep it as a real chart");
    }

    // Note types and events offered in the lists: the built-in ones, the
    // mod's custom_notetypes / custom_events, and whatever the chart uses.
    void scanNames(const Chart& c) {
        scene::Files files;
        std::vector<std::string> roots = entry_->assetRoots;
        if (roots.empty() && !entry_->modRoot.empty()) roots.push_back(entry_->modRoot);
        roots.push_back(app().base + "mods/");
        files.setRoots(roots);
        auto addUnique = [](std::vector<std::string>& v, const std::string& s) {
            if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
        };
        auto stem = [](const std::string& path) {
            std::string n = pathStr(toPath(path).filename());
            const size_t dot = n.rfind('.');
            return dot == std::string::npos ? n : n.substr(0, dot);
        };
        for (const char* t : kBaseNoteTypes) addUnique(noteTypes_, t);
        for (const char* ext : {".lua", ".hx", ".txt"})
            for (const std::string& f : files.list("custom_notetypes", ext)) addUnique(noteTypes_, stem(f));
        for (const std::string& t : c.noteTypes) addUnique(noteTypes_, t);
        for (const EventInfo& e : kEvents) addUnique(eventNames_, e.name);
        for (const char* ext : {".lua", ".hx", ".txt"})
            for (const std::string& f : files.list("custom_events", ext)) {
                const std::string n = stem(f);
                addUnique(eventNames_, n);
                if (endsWithI(f, ".txt")) {
                    std::string text;
                    if (scene::readFile(f, text)) eventHelp_[n] = trim(text);
                }
            }
        for (const psych::ChartEvent& e : events_) addUnique(eventNames_, e.name);
    }

    // ------------------------------------------------------------ timing --
    void rebuildStarts() {
        starts_.assign(sections_.size() + 1, 0.0);
        beatStarts_.assign(sections_.size() + 1, 0.0);
        for (size_t i = 0; i < sections_.size(); ++i) {
            starts_[i + 1] = starts_[i] + sections_[i].beats * 60000.0 / sections_[i].bpm;
            beatStarts_[i + 1] = beatStarts_[i] + sections_[i].beats;
        }
    }
    EdSection& section(int i) {
        i = std::max(0, i);
        if (int(sections_.size()) <= i) {
            while (int(sections_.size()) <= i) {
                EdSection s = sections_.empty() ? EdSection{} : sections_.back();
                s.altAnim = s.gf = false;
                sections_.push_back(s);
            }
            rebuildStarts();
        }
        return sections_[size_t(i)];
    }
    double lastBpm() const { return sections_.empty() ? 120 : sections_.back().bpm; }
    double lastBeats() const { return sections_.empty() ? 4 : sections_.back().beats; }
    int sectionAt(double ms) {
        if (starts_.size() != sections_.size() + 1) rebuildStarts();
        if (ms < 0) return 0;
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), ms);
        int i = int(it - starts_.begin()) - 1;
        if (i >= int(sections_.size())) i = int(sections_.size()) + int((ms - starts_.back()) / (lastBeats() * 60000.0 / lastBpm()));
        return std::max(0, i);
    }
    double sectionStart(int i) {
        if (starts_.size() != sections_.size() + 1) rebuildStarts();
        if (i < int(starts_.size())) return starts_[size_t(std::max(0, i))];
        return starts_.back() + (i - int(sections_.size())) * lastBeats() * 60000.0 / lastBpm();
    }
    double sectionBeatStart(int i) {
        if (i < int(beatStarts_.size())) return beatStarts_[size_t(std::max(0, i))];
        return beatStarts_.back() + (i - int(sections_.size())) * lastBeats();
    }
    double bpmAt(double ms) {
        const int s = sectionAt(ms);
        return s < int(sections_.size()) ? sections_[size_t(s)].bpm : lastBpm();
    }
    double beatAt(double ms) {
        const int s = sectionAt(ms);
        return sectionBeatStart(s) + (ms - sectionStart(s)) * bpmAt(ms) / 60000.0;
    }
    double msAtBeat(double beat) {
        if (beat <= 0) return beat * 60000.0 / bpmAt(0);
        int s;
        if (beat < beatStarts_.back()) s = int(std::upper_bound(beatStarts_.begin(), beatStarts_.end(), beat) - beatStarts_.begin()) - 1;
        else s = int(sections_.size()) + int((beat - beatStarts_.back()) / lastBeats());
        const double bpm = s < int(sections_.size()) ? sections_[size_t(s)].bpm : lastBpm();
        return sectionStart(s) + (beat - sectionBeatStart(s)) * 60000.0 / bpm;
    }
    double snapBeats() const { return 4.0 / kSnaps[snapIdx_]; }
    double snapBeat(double b) const { return std::round(b / snapBeats()) * snapBeats(); }
    double songLength() {
        double l = inst_ ? double(inst_->frames()) * 1000.0 / Mixer::kRate : 0.0;
        if (!notes_.empty()) l = std::max(l, notes_.back().t + notes_.back().len);
        return l;
    }
    void gotoSection(int s) {
        if (playing_) stopPlay();
        cur_ = sectionStart(std::max(0, s));
    }
    // A section's tempo; the following sections that shared the old one follow.
    void setSectionBpm(int s, double bpm) {
        bpm = clampv(bpm, 10.0, 2000.0);
        snapshot();
        const double old = section(s).bpm;
        for (size_t i = size_t(s); i < sections_.size() && std::fabs(sections_[i].bpm - old) < 1e-9; ++i) sections_[i].bpm = bpm;
        rebuildStarts();
        dirty_ = true;
    }

    // ------------------------------------------------------------ editing --
    void sortNotes() {
        std::stable_sort(notes_.begin(), notes_.end(), [](const EdNote& a, const EdNote& b) {
            return a.t != b.t ? a.t < b.t : a.col < b.col;
        });
    }
    void sortEvents() {
        std::stable_sort(events_.begin(), events_.end(),
                         [](const psych::ChartEvent& a, const psych::ChartEvent& b) { return a.timeMs < b.timeMs; });
    }
    void snapshot() {
        undo_.push_back({notes_, sections_, events_, speed_});
        const size_t cap = notes_.size() > 200000 ? 4 : 60;
        while (undo_.size() > cap) undo_.erase(undo_.begin());
        redo_.clear();
    }
    void restore(std::vector<UndoStep>& from, std::vector<UndoStep>& to, const char* what) {
        if (from.empty()) return;
        to.push_back({notes_, sections_, events_, speed_});
        notes_ = std::move(from.back().notes);
        sections_ = std::move(from.back().sections);
        events_ = std::move(from.back().events);
        speed_ = from.back().speed;
        from.pop_back();
        rebuildStarts();
        selEvent_ = -1;
        dirty_ = true;
        flash(what);
    }
    void undo() { restore(undo_, redo_, "Undo"); }
    void redo() { restore(redo_, undo_, "Redo"); }

    bool anySelected() const {
        return std::any_of(notes_.begin(), notes_.end(), [](const EdNote& n) { return n.sel; });
    }
    EdNote* firstSelected() {
        for (EdNote& n : notes_)
            if (n.sel) return &n;
        return nullptr;
    }
    int selectedCount() const {
        return int(std::count_if(notes_.begin(), notes_.end(), [](const EdNote& n) { return n.sel; }));
    }
    void clearSelection() {
        for (EdNote& n : notes_) n.sel = false;
    }
    template <class F>
    void applyToSelection(F f) {
        if (!anySelected()) return;
        snapshot();
        for (EdNote& n : notes_)
            if (n.sel) f(n);
        sortNotes();
        dirty_ = true;
    }
    void selectSection() {
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1);
        for (EdNote& n : notes_) n.sel = n.t >= a - 0.5 && n.t < b - 0.5;
        flash(TextFormat("%d notes selected", selectedCount()));
    }
    void deleteSelection() {
        snapshot();
        notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [](const EdNote& n) { return n.sel; }), notes_.end());
        dirty_ = true;
    }

    long findNote(double t, int col, double tolMs) {
        auto lo = std::lower_bound(notes_.begin(), notes_.end(), t - tolMs, [](const EdNote& n, double v) { return n.t < v; });
        for (auto it = lo; it != notes_.end() && it->t <= t + tolMs; ++it)
            if (it->col == col) return long(it - notes_.begin());
        return -1;
    }
    long findEvent(double t, double tolMs) {
        for (size_t i = 0; i < events_.size(); ++i)
            if (std::fabs(events_[i].timeMs - t) <= tolMs) return long(i);
        return -1;
    }

    // Copy: the selected notes (or the section under the line), relative to
    // the first one's snapped time; paste puts them at the line.
    void copySelectionOrSection() {
        clipboard_.clear();
        clipEvents_.clear();
        double base;
        if (anySelected()) {
            base = msAtBeat(snapBeat(beatAt(firstSelected()->t)));
            for (const EdNote& n : notes_)
                if (n.sel) clipboard_.push_back(n);
        } else {
            const int s = sectionAt(cur_);
            base = sectionStart(s);
            const double b = sectionStart(s + 1);
            for (const EdNote& n : notes_)
                if (n.t >= base - 0.5 && n.t < b - 0.5) clipboard_.push_back(n);
            for (const psych::ChartEvent& e : events_)
                if (e.timeMs >= base - 0.5 && e.timeMs < b - 0.5) clipEvents_.push_back(e);
        }
        for (EdNote& n : clipboard_) {
            n.t -= base;
            n.sel = false;
        }
        for (psych::ChartEvent& e : clipEvents_) e.timeMs -= base;
        flash(TextFormat("Copied %d notes", int(clipboard_.size())));
    }
    // Cut: copy, then remove what was copied (the selection, or the section's notes and events).
    void cutSelectionOrSection() {
        const bool sel = anySelected();
        copySelectionOrSection();
        if (sel) deleteSelection();
        else clearSection(true, true);
        flash(TextFormat("Cut %d notes", int(clipboard_.size())));
    }
    void paste() {
        if (clipboard_.empty() && clipEvents_.empty()) return;
        snapshot();
        clearSelection();
        const double at = anySelected() ? cur_ : msAtBeat(snapBeat(beatAt(cur_)));
        for (EdNote c : clipboard_) {
            c.t += at;
            c.sel = true;
            if (findNote(c.t, c.col, 0.5) < 0) notes_.push_back(c);
        }
        for (psych::ChartEvent e : clipEvents_) {
            e.timeMs += at;
            events_.push_back(e);
        }
        sortNotes();
        sortEvents();
        dirty_ = true;
        flash(TextFormat("Pasted %d notes", int(clipboard_.size())));
    }
    void clearSection(bool notes, bool events) {
        snapshot();
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1);
        if (notes)
            notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const EdNote& n) { return n.t >= a - 0.5 && n.t < b - 0.5; }),
                         notes_.end());
        if (events)
            events_.erase(std::remove_if(events_.begin(), events_.end(),
                                         [&](const psych::ChartEvent& e) { return e.timeMs >= a - 0.5 && e.timeMs < b - 0.5; }),
                          events_.end());
        selEvent_ = -1;
        dirty_ = true;
    }
    // Notes of the selection (or the section) change sides.
    void swapSides() {
        const bool sel = anySelected();
        snapshot();
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1);
        for (EdNote& n : notes_)
            if (sel ? n.sel : (n.t >= a - 0.5 && n.t < b - 0.5)) n.col = n.col >= keys_ ? n.col - keys_ : n.col + keys_;
        sortNotes();
        dirty_ = true;
    }
    // Left becomes right within each side.
    void mirror() {
        const bool sel = anySelected();
        snapshot();
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1);
        for (EdNote& n : notes_)
            if (sel ? n.sel : (n.t >= a - 0.5 && n.t < b - 0.5)) {
                const int side = n.col >= keys_ ? 1 : 0, lane = n.col % keys_;
                n.col = side * keys_ + (keys_ - 1 - lane);
            }
        sortNotes();
        dirty_ = true;
        flash("Mirrored");
    }
    // Copies the notes of the section `back` sections earlier into this one.
    void copyLastSection(int back) {
        const int s = sectionAt(cur_), from = s - back;
        if (from < 0) return;
        snapshot();
        const double fa = sectionStart(from), fb = sectionStart(from + 1), to = sectionStart(s);
        std::vector<EdNote> add;
        for (const EdNote& n : notes_)
            if (n.t >= fa - 0.5 && n.t < fb - 0.5) {
                EdNote c = n;
                c.t = to + (n.t - fa);
                c.sel = false;
                add.push_back(c);
            }
        for (const EdNote& c : add)
            if (findNote(c.t, c.col, 0.5) < 0) notes_.push_back(c);
        sortNotes();
        dirty_ = true;
        flash(TextFormat("Copied %d notes from section %d", int(add.size()), from));
    }
    // JS Engine style spam tools: a note on every snap of the section.
    void fillSection(int pattern, int side) {
        snapshot();
        const int s = sectionAt(cur_);
        const double b0 = sectionBeatStart(s), b1 = sectionBeatStart(s + 1);
        std::mt19937 r(unsigned(s * 7919 + snapIdx_));
        int k = 0;
        for (double b = b0; b < b1 - 1e-6; b += snapBeats(), ++k) {
            const double t = msAtBeat(b);
            int lane = 0;
            if (pattern == 0) lane = fillLane_ % keys_;                       // one lane
            else if (pattern == 1) lane = k % keys_;                          // stairs
            else if (pattern == 2) lane = (keys_ - 1) - (k % keys_);          // stairs down
            else if (pattern == 3) lane = int(r() % unsigned(keys_));         // random
            else lane = (k % 2) ? keys_ - 1 - (k / 2) % keys_ : (k / 2) % keys_;  // zigzag
            for (int sd = 0; sd < 2; ++sd) {
                if (side != 2 && side != sd) continue;
                const int col = sd * keys_ + lane;
                if (findNote(t, col, 0.5) >= 0) continue;
                EdNote n;
                n.t = t;
                n.col = col;
                n.type = noteTypes_[size_t(typeIdx_)];
                notes_.push_back(n);
            }
        }
        sortNotes();
        dirty_ = true;
        flash(TextFormat("Filled section %d", s));
    }
    void setKeys(int k) {
        k = clampv(k, 1, 9);
        if (k == keys_) return;
        snapshot();
        std::vector<EdNote> kept;
        int dropped = 0;
        for (EdNote n : notes_) {
            const int side = n.col >= keys_ ? 1 : 0, lane = n.col % keys_;
            if (lane >= k) { ++dropped; continue; }
            n.col = side * k + lane;
            kept.push_back(n);
        }
        notes_.swap(kept);
        keys_ = k;
        dirty_ = true;
        flash(dropped ? TextFormat("%d keys (%d notes past the last lane removed)", k, dropped) : TextFormat("%d keys", k));
    }

    // ------------------------------------------------------------- layout --
    // The event lane and both sides share the space between the panels.
    float laneW() const { return std::min(48.0f, 590.0f / float(2 * keys_ + 1)); }
    float gridX() const {
        const float lw = laneW(), total = float(2 * keys_ + 1) * lw + 10;
        return 250 + (610 - total) * 0.5f + lw + 10;
    }
    float eventX() const { return gridX() - laneW() - 10; }
    float lineY() const { return 220.0f; }
    float pxPerMs() const { return 0.45f * zoom_; }
    double snappedTimeAtY(float y) {
        const double raw = cur_ + (y - lineY()) / pxPerMs();
        return msAtBeat(snapBeat(beatAt(std::max(0.0, raw))));
    }
    bool mouseCell(int& col, double& t) {
        const Vector2 m = ui_.mouse;
        const float x0 = gridX();
        if (m.x < x0 || m.x >= x0 + laneW() * 2 * keys_ || m.y < 40 || m.y > gfx::H) return false;
        col = int((m.x - x0) / laneW());
        t = snappedTimeAtY(m.y);
        return t >= 0;
    }
    bool mouseEventCell(double& t) {
        const Vector2 m = ui_.mouse;
        if (m.x < eventX() || m.x >= eventX() + laneW() || m.y < 40 || m.y > gfx::H) return false;
        t = snappedTimeAtY(m.y);
        return t >= 0;
    }

    void handleGridMouse() {
        const Input& in = app().input;
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);
        const bool ctrl = in.isDown(KEY_LEFT_CONTROL) || in.isDown(KEY_RIGHT_CONTROL);
        int col = 0;
        double t = 0;
        const bool inGrid = mouseCell(col, t);
        double et = 0;
        const bool inEvents = mouseEventCell(et);
        const double tol = std::max(4.0, 60000.0 / bpmAt(t) * snapBeats() * 0.45);
        // Event lane: click selects or adds, right click removes.
        if (inEvents && ui_.click) {
            const long hit = findEvent(et, tol);
            if (hit >= 0) {
                selEvent_ = hit;
            } else {
                snapshot();
                psych::ChartEvent e;
                e.timeMs = et;
                e.name = eventNames_.empty() ? std::string("Hey!") : eventNames_[size_t(eventIdx_)];
                e.v1 = eventV1_;
                e.v2 = eventV2_;
                events_.push_back(e);
                sortEvents();
                selEvent_ = findEvent(et, 0.01);
                dirty_ = true;
            }
            tab_ = 3;
            return;
        }
        if (inEvents && ui_.rclick) {
            const long hit = findEvent(et, tol);
            if (hit >= 0) {
                snapshot();
                events_.erase(events_.begin() + hit);
                selEvent_ = -1;
                dirty_ = true;
            }
            return;
        }
        // Box selection: SHIFT + drag on the grid.
        if (ui_.click && inGrid && shift) {
            boxing_ = true;
            boxA_ = ui_.mouse;
            return;
        }
        if (boxing_) {
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                boxing_ = false;
                const float ya = std::min(boxA_.y, ui_.mouse.y), yb = std::max(boxA_.y, ui_.mouse.y);
                const float xa = std::min(boxA_.x, ui_.mouse.x), xb = std::max(boxA_.x, ui_.mouse.x);
                const double ta = cur_ + (ya - lineY()) / pxPerMs(), tb = cur_ + (yb - lineY()) / pxPerMs();
                const int ca = int((xa - gridX()) / laneW()), cb = int((xb - gridX()) / laneW());
                if (!ctrl) clearSelection();
                for (EdNote& n : notes_)
                    if (n.t >= ta && n.t <= tb && n.col >= ca && n.col <= cb) n.sel = true;
                flash(TextFormat("%d notes selected", selectedCount()));
            }
            return;
        }
        if (ui_.click && inGrid) {
            const long hit = findNote(t, col, tol);
            if (hit >= 0) {
                EdNote& n = notes_[size_t(hit)];
                if (ctrl) {
                    n.sel = !n.sel;
                } else if (n.sel && selectedCount() == 1) {  // second click on a selected note removes it
                    snapshot();
                    notes_.erase(notes_.begin() + hit);
                    dirty_ = true;
                } else {
                    clearSelection();
                    n.sel = true;
                }
            } else {
                snapshot();
                clearSelection();
                EdNote n;
                n.t = t;
                n.col = col;
                n.type = noteTypes_[size_t(typeIdx_)];
                n.sel = true;
                notes_.push_back(n);
                if (ezMode_) ezPlaceSpam(n);  // JS Engine's EZ Spam Mode
                sortNotes();
                dragging_ = true;
                dirty_ = true;
            }
            tab_ = tab_ == 3 ? 2 : tab_;
        }
        if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            // Drag down to stretch the new note into a hold.
            if (EdNote* n = firstSelected()) {
                const double end = snappedTimeAtY(ui_.mouse.y);
                n->len = std::max(0.0, end - n->t);
            }
        }
        if (in.mouseReleased(MOUSE_BUTTON_LEFT)) dragging_ = false;
        if (ui_.rclick && inGrid) {
            const long hit = findNote(t, col, tol);
            if (hit >= 0) {
                snapshot();
                notes_.erase(notes_.begin() + hit);
                dirty_ = true;
            }
        }
    }

    // ----------------------------------------------------------- playback --
    void togglePlay() {
        if (playing_) { stopPlay(); return; }
        App& a = app();
        a.mixer.setTracks(inst_, voiceP_, voiceO_);
        a.mixer.setRate(playRate_);
        applyVolumes();
        a.mixer.play(cur_ / 1000.0);
        playing_ = true;
        lastT_ = cur_;
    }
    void stopPlay() {
        if (!playing_) return;
        app().mixer.pause();
        playing_ = false;
    }
    void applyVolumes() {
        Mixer& m = app().mixer;
        m.setTrackVolume(Mixer::kInst, float(instVol_));
        m.setTrackVolume(Mixer::kVoicePlayer, muteVoices_ ? 0.0f : float(voiceVol_));
        m.setTrackVolume(Mixer::kVoiceOpponent, muteVoices_ ? 0.0f : float(voiceVol_));
    }
    void tickSounds(double from, double to) {
        if (to <= from || to - from > 500) return;
        auto lo = std::upper_bound(notes_.begin(), notes_.end(), from, [](double v, const EdNote& n) { return v < n.t; });
        int count = 0;
        for (auto it = lo; it != notes_.end() && it->t <= to && count < 4; ++it) {
            const bool player = it->col >= keys_;
            if (player ? !hitPlayer_ : !hitOpponent_) continue;
            sfxPlay(Sfx::Hit, 0.45f);  // the same tick for both sides
            ++count;
        }
        if (metronome_ && std::floor(beatAt(to)) != std::floor(beatAt(from)))
            sfxPlay(Sfx::Scroll, 0.5f, int(std::floor(beatAt(to))) % 4 == 0 ? 1.4f : 1.0f);
    }

    // ------------------------------------------------------------- saving --
    bool eventsInChart() const { return eventsFile_.empty() || !eventsSeparate_; }

    std::string eventsJson() const {
        std::string o = "[";
        bool firstEv = true;
        for (size_t i = 0; i < events_.size();) {
            const double t = events_[i].timeMs;
            o += std::string(firstEv ? "" : ",") + "\n    [" + num(t) + ", [";
            bool firstSub = true;
            for (; i < events_.size() && events_[i].timeMs == t; ++i) {
                o += std::string(firstSub ? "" : ", ") + "[" + jsonStr(events_[i].name) + ", " + jsonStr(events_[i].v1) + ", " +
                     jsonStr(events_[i].v2) + "]";
                firstSub = false;
            }
            o += "]]";
            firstEv = false;
        }
        return o + (events_.empty() ? "]" : "\n  ]");
    }

    std::string buildJson(bool withEvents) {
        // Sections up to the last note or event (at least one).
        int last = 0;
        if (!notes_.empty()) last = sectionAt(notes_.back().t);
        if (!events_.empty()) last = std::max(last, sectionAt(events_.back().timeMs));
        std::string o;
        long long total = 0;
        for (const EdNote& n : notes_) total += n.stack;
        o.reserve(size_t(total) * 28 + 4096);
        o += "{\"song\": {\n  \"song\": " + jsonStr(title_) + ",\n  \"bpm\": " + num(sections_.empty() ? 120 : sections_[0].bpm) +
             ",\n  \"speed\": " + num(speed_) + ",\n  \"needsVoices\": " + (needsVoices_ ? "true" : "false") +
             ",\n  \"player1\": " + jsonStr(player_) + ",\n  \"player2\": " + jsonStr(opponent_);
        if (!gf_.empty()) o += ",\n  \"gfVersion\": " + jsonStr(gf_);
        if (!stage_.empty()) o += ",\n  \"stage\": " + jsonStr(stage_);
        if (keys_ != 4) o += ",\n  \"keyCount\": " + std::to_string(keys_) + ",\n  \"mania\": " + std::to_string(keys_ - 1);
        o += ",\n  \"events\": " + (withEvents ? eventsJson() : std::string("[]"));
        o += ",\n  \"notes\": [";
        size_t ni = 0;
        double prevBpm = sections_.empty() ? 120 : sections_[0].bpm;
        for (int s = 0; s <= last; ++s) {
            const EdSection sec = section(s);
            const double a = sectionStart(s), b = sectionStart(s + 1);
            o += std::string(s ? "," : "") + "\n    {\"sectionBeats\": " + num(sec.beats) + ", \"lengthInSteps\": " +
                 std::to_string(int(std::lround(sec.beats * 4))) + ", \"mustHitSection\": " + (sec.mustHit ? "true" : "false");
            if (std::fabs(sec.bpm - prevBpm) > 1e-9) o += ", \"changeBPM\": true, \"bpm\": " + num(sec.bpm);
            else o += ", \"changeBPM\": false, \"bpm\": " + num(sec.bpm);
            prevBpm = sec.bpm;
            o += std::string(", \"altAnim\": ") + (sec.altAnim ? "true" : "false");
            o += std::string(", \"gfSection\": ") + (sec.gf ? "true" : "false");
            o += ", \"sectionNotes\": [";
            bool first = true;
            while (ni < notes_.size() && (notes_[ni].t < b - 0.5 || s == last)) {
                const EdNote& n = notes_[ni++];
                if (n.t < a - 0.5 && s != 0) continue;
                // noteData is relative to the section's side: 0..K-1 is the side it focuses.
                const bool player = n.col >= keys_;
                const int lane = player ? n.col - keys_ : n.col;
                const int data = (player == sec.mustHit) ? lane : lane + keys_;
                std::string one = "[" + num(n.t) + ", " + std::to_string(data) + ", " + num(n.len);
                if (!n.type.empty()) one += ", " + jsonStr(n.type);
                one += "]";
                for (int k = 0; k < std::max(1, n.stack); ++k) {  // stacked notes stay stacked
                    o += std::string(first ? "" : ", ") + one;
                    first = false;
                }
            }
            o += "]}";
        }
        o += "\n  ]\n}}\n";
        return o;
    }

    // Where this chart is written. Mod and custom charts are saved in place
    // (the original is kept once as .bak); generated charts get a new file.
    // A chart file written in Psych's format (an FL Studio project, a
    // PulseForge chart or a V-Slice chart is not overwritten).
    static bool psychJsonFile(const std::string& path) {
        std::string file;
        int pattern = 0;
        if (!endsWithI(path, ".json") || flpChartPath(path, file, pattern)) return false;
        std::string head;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            head.resize(4096);
            head.resize(std::fread(head.data(), 1, head.size(), f));
            std::fclose(f);
        }
        return head.find("\"pulseforge-") == std::string::npos;
    }

    std::string savePath() {
        const SongEntry& e = *entry_;
        const std::string key0 = songPathName(e.builtin ? title_ + " (edit)" : e.songKey);
        for (size_t i = 0; i < e.diffs.size(); ++i)
            if (e.diffs[i] == diff_ && i < e.chartPaths.size() && !e.chartPaths[i].empty() && e.metaPath.empty()) {
                if (psychJsonFile(e.chartPaths[i])) return e.chartPaths[i];
                // FL Studio / PulseForge: a Psych chart next to the original.
                std::string file = e.chartPaths[i];
                int pattern = 0;
                flpChartPath(e.chartPaths[i], file, pattern);
                std::string name = key0;
                const std::string d = toLower(diff_);
                if (d.rfind("normal", 0) != 0) name += "-" + songPathName(diff_);
                return pathStr(toPath(file).parent_path() / toPath(name + ".json"));
            }
        const std::string key = songPathName(e.builtin ? title_ + " (edit)" : e.songKey);
        const std::string file = key + (equalsI(diff_, "Normal") ? "" : "-" + toLower(diff_)) + ".json";
        if (e.builtin) return app().base + "songs/" + key + "/" + file;
        if (e.autoChart && !e.inst.empty()) {
            const fs::path audio = toPath(e.inst);
            const fs::path dir = audio.parent_path();
            // A loose audio file in songs/ gets its own folder.
            if (pathStr(dir) + "/" == app().base + "songs/" || toLower(pathStr(dir.filename())) == "songs")
                return pathStr(dir / toPath(key) / toPath(file));
            return pathStr(dir / toPath(file));
        }
        return pathStr(toPath(e.dataDir) / toPath(file));
    }

    static bool writeText(const fs::path& p, const std::string& text) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        return bool(out);
    }

    bool save(bool playtest) {
        App& a = app();
        const std::string path = savePath();
        std::error_code ec;
        const fs::path p = toPath(path);
        fs::create_directories(p.parent_path(), ec);
        if (fs::exists(p, ec) && !backedUp_) {
            const fs::path bak = toPath(path + ".bak");
            if (!fs::exists(bak, ec)) fs::copy_file(p, bak, ec);
            backedUp_ = true;
        }
        // Audio for charts that had none on disk.
        if (entry_->builtin) {
            if (inst_) writeWav(pathStr(p.parent_path() / "Inst.wav"), *inst_);
            if (voiceP_) writeWav(pathStr(p.parent_path() / "Voices-Player.wav"), *voiceP_);
            if (voiceO_) writeWav(pathStr(p.parent_path() / "Voices-Opponent.wav"), *voiceO_);
        } else if (entry_->autoChart && !entry_->inst.empty()) {
            const fs::path audio = toPath(entry_->inst);
            if (audio.parent_path() != p.parent_path()) {
                const fs::path dst = p.parent_path() / toPath("Inst" + pathStr(audio.extension()));
                if (!fs::exists(dst, ec)) fs::copy_file(audio, dst, ec);
            }
        }
        if (!writeText(p, buildJson(eventsInChart()))) {
            flash("Could not save " + path);
            return false;
        }
        if (!eventsInChart()) {
            const fs::path ep = toPath(eventsFile_);
            if (!fs::exists(toPath(eventsFile_ + ".bak"), ec)) fs::copy_file(ep, toPath(eventsFile_ + ".bak"), ec);
            writeText(ep, "{\"song\": {\n  \"events\": " + eventsJson() + ",\n  \"notes\": []\n}}\n");
        }
        dirty_ = false;
        long long total = 0;
        for (const EdNote& n : notes_) total += n.stack;
        flash("Saved " + pathStr(p.filename()) + " (" + fmtThousands(total) + " notes, " + std::to_string(events_.size()) + " events)");
        // New files: rescan so Freeplay (and the playtest) see the chart.
        std::string id = songId_;
        if (entry_->builtin || entry_->autoChart) {
            a.library.scan(a.base);
            for (const SongEntry& s : a.library.songs())
                for (const std::string& cp : s.chartPaths)
                    if (!cp.empty() && fs::equivalent(toPath(cp), p, ec)) id = s.id;
            entry_ = a.library.find(id);
            if (!entry_) return false;
            songId_ = id;
            for (size_t i = 0; i < entry_->chartPaths.size(); ++i)
                if (fs::equivalent(toPath(entry_->chartPaths[i]), p, ec)) diff_ = entry_->diffs[i];
        }
        if (playtest) {
            stopPlay();
            PlayRequest r;
            r.songId = songId_;
            r.diff = diff_;
            a.change(makePlay(r));
            return true;
        }
        return false;
    }

    void flash(const std::string& s) {
        flash_ = s;
        flashAt_ = app().now;
    }

    // ------------------------------------------------------------ drawing --
    void drawGrid() {
        const float lw = laneW(), x0 = gridX(), ly = lineY(), pp = pxPerMs(), ex = eventX();
        const int cols = 2 * keys_;
        const float gw = lw * cols;
        const double tTop = cur_ - (ly - 40) / pp, tBot = cur_ + (gfx::H - ly) / pp;
        // Section backgrounds: the camera's side is lighter, gf sections tinted.
        const int s0 = sectionAt(std::max(0.0, tTop)), s1 = sectionAt(std::max(0.0, tBot));
        for (int s = s0; s <= s1; ++s) {
            const float ya = std::max(40.0f, ly + float((sectionStart(s) - cur_) * pp));
            const float yb = std::min(float(gfx::H), ly + float((sectionStart(s + 1) - cur_) * pp));
            if (yb <= ya) continue;
            const EdSection& sec = section(s);
            const Color on = sec.gf ? Color{54, 40, 52, 255} : Color{44, 34, 66, 255}, off{28, 22, 40, 255};
            DrawRectangleRec({x0, ya, lw * keys_, yb - ya}, sec.mustHit ? off : on);
            DrawRectangleRec({x0 + lw * keys_, ya, lw * keys_, yb - ya}, sec.mustHit ? on : off);
            DrawRectangleRec({ex, ya, lw, yb - ya}, Color{24, 18, 34, 255});
        }
        // Beat / snap lines
        const double b0 = std::floor(beatAt(std::max(0.0, tTop))), b1 = std::ceil(beatAt(std::max(0.0, tBot)));
        for (double b = b0; b <= b1; b += snapBeats()) {
            const float y = ly + float((msAtBeat(b) - cur_) * pp);
            if (y < 40 || y > gfx::H) continue;
            const bool beat = std::fabs(b - std::round(b)) < 1e-6;
            DrawRectangleRec({ex, y - 0.5f, x0 + gw - ex, 1.0f},
                             beat ? Color{110, 90, 150, 140} : Color{80, 66, 110, 70});
        }
        for (int s = s0; s <= s1 + 1; ++s) {  // section starts, numbered
            const float y = ly + float((sectionStart(s) - cur_) * pp);
            if (y < 40 || y > gfx::H) continue;
            DrawRectangleRec({ex, y - 1.5f, x0 + gw - ex, 3.0f}, Color{170, 140, 220, 200});
            gfx::text(FontId::Ui, std::to_string(s), ex - 8, y - 10, 18, gfx::fade(WHITE, 0.6f), Align::Right);
        }
        for (int c = 0; c <= cols; ++c)
            DrawRectangleRec({x0 + c * lw - (c == keys_ ? 1.5f : 0.5f), 40, c == keys_ ? 3.0f : 1.0f, gfx::H - 40.0f},
                             c == keys_ ? Color{200, 170, 255, 200} : Color{70, 58, 96, 160});
        DrawRectangleRec({ex + lw, 40, 1, gfx::H - 40.0f}, Color{70, 58, 96, 160});
        // Events
        for (size_t i = 0; i < events_.size(); ++i) {
            const psych::ChartEvent& e = events_[i];
            const float y = ly + float((e.timeMs - cur_) * pp);
            if (y < 30 || y > gfx::H + 20) continue;
            const bool sel = long(i) == selEvent_;
            DrawRectangleRounded({ex + 4, y - lw * 0.32f, lw - 8, lw * 0.64f}, 0.3f, 6, sel ? kAccent : Color{150, 120, 220, 255});
            gfx::text(FontId::Ui, "E", ex + lw * 0.5f, y - 10, 18, Color{20, 12, 36, 255}, Align::Center);
            if (sel || (i + 1 < events_.size() && events_[i + 1].timeMs == e.timeMs && i + 1 == size_t(selEvent_))) {
                gfx::text(FontId::Ui, e.name, ex - 8, y + 6, 16, gfx::fade(kAccent, 0.9f), Align::Right);
            }
        }
        // Notes in view (sustains first)
        auto lo = std::lower_bound(notes_.begin(), notes_.end(), tTop - 60000.0, [](const EdNote& n, double v) { return n.t < v; });
        float lastY[18];
        std::fill(std::begin(lastY), std::end(lastY), -1e9f);
        for (int pass = 0; pass < 2; ++pass) {
            for (auto it = lo; it != notes_.end() && it->t <= tBot; ++it) {
                const EdNote& n = *it;
                const float yEnd = ly + float((n.t + n.len - cur_) * pp);
                if (yEnd < 30 && n.len <= 0) continue;
                const int lane = n.col % keys_;
                const int dir = gfx::laneDir(keys_, lane);
                const float cx = x0 + (n.col + 0.5f) * lw;
                const float y = ly + float((n.t - cur_) * pp);
                if (pass == 0) {
                    if (n.len > 0) gfx::drawSustain(dir == gfx::kCenter ? gfx::kUp : dir, cx, y, yEnd, lw * 0.32f, 0.85f, true);
                    continue;
                }
                if (y < 30 || y > gfx::H + 30) continue;
                // Spam charts: one note per pixel row is enough to see them.
                if (std::fabs(y - lastY[n.col]) < 1.0f && !n.sel) continue;
                lastY[n.col] = y;
                const bool mine = n.type.find("Hurt") != std::string::npos;
                if (mine) DrawCircleV({cx, y}, lw * 0.32f, Color{220, 40, 60, 255});
                else gfx::drawNote(dir, {cx, y}, lw * 0.82f, 1.0f);
                if (!n.type.empty() && !mine) DrawCircleV({cx + lw * 0.3f, y - lw * 0.3f}, 5, gfx::rgb(0xFFD54A));
                if (n.stack > 1)
                    gfx::text(FontId::Ui, "x" + std::to_string(n.stack), cx + lw * 0.42f, y + 2, 14, kAccent, Align::Right, 1.0f);
                if (n.sel) DrawRectangleLinesEx({cx - lw * 0.48f, y - lw * 0.48f, lw * 0.96f, lw * 0.96f}, 2, gfx::rgb(0xFFFFFF));
            }
        }
        // Mouse cursor cell / selection box
        int col = 0;
        double t = 0;
        if (boxing_) {
            const Rectangle r{std::min(boxA_.x, ui_.mouse.x), std::min(boxA_.y, ui_.mouse.y), std::fabs(ui_.mouse.x - boxA_.x),
                              std::fabs(ui_.mouse.y - boxA_.y)};
            DrawRectangleRec(r, Color{255, 255, 255, 30});
            DrawRectangleLinesEx(r, 1.5f, Color{255, 255, 255, 160});
        } else if (mouseCell(col, t)) {
            const float y = ly + float((t - cur_) * pp);
            DrawRectangleRec({x0 + col * lw + 2, y - lw * 0.45f, lw - 4, lw * 0.9f}, Color{255, 255, 255, 40});
        } else if (mouseEventCell(t)) {
            const float y = ly + float((t - cur_) * pp);
            DrawRectangleRec({ex + 2, y - lw * 0.45f, lw - 4, lw * 0.9f}, Color{255, 255, 255, 40});
        }
        // Strum line and receptors
        DrawRectangleRec({ex - 6, ly - 2, x0 + gw - ex + 12, 4}, Color{255, 220, 120, 220});
        for (int c = 0; c < cols; ++c) {
            const int dir = gfx::laneDir(keys_, c % keys_);
            gfx::drawReceptor(dir, {x0 + (c + 0.5f) * lw, ly}, lw * 0.82f, Color{200, 190, 230, 120});
        }
        // Top bar
        DrawRectangleRec({0, 0, gfx::W, 40}, Color{8, 5, 14, 235});
        gfx::text(FontId::Ui, "EVENTS", ex + lw * 0.5f, 10, 16, gfx::fade(WHITE, 0.6f), Align::Center);
        gfx::text(FontId::Ui, "OPPONENT", x0 + lw * keys_ * 0.5f, 8, 22, gfx::fade(WHITE, 0.75f), Align::Center);
        gfx::text(FontId::Ui, "PLAYER", x0 + lw * keys_ * 1.5f, 8, 22, gfx::fade(WHITE, 0.75f), Align::Center);
    }

    void drawInfo() {
        const float x = 18;
        float y = 56;
        auto line = [&](const std::string& s, Color c = kInk, float size = 18) {
            gfx::text(FontId::Ui, s, x, y, size, c);
            y += size + 5;
        };
        DrawRectangleRounded({8, 48, 232, 330}, 0.04f, 6, kPanel);
        line(title_, kAccent, 22);
        line(diff_ + (dirty_ ? "  (unsaved)" : ""), gfx::fade(WHITE, 0.7f));
        const int sec = sectionAt(cur_);
        const double beat = beatAt(cur_);
        line("Time  " + fmtTime(cur_ / 1000.0) + TextFormat(".%03d", int(std::fmod(std::max(0.0, cur_), 1000.0))));
        line(TextFormat("Beat %.2f  Step %d", beat, int(std::floor(beat * 4))));
        line(TextFormat("Section %d  BPM %.2f", sec, bpmAt(cur_)));
        line(TextFormat("Snap 1/%d  Zoom %.2fx", kSnaps[snapIdx_], zoom_));
        long long total = 0;
        for (const EdNote& n : notes_) total += n.stack;
        line("Notes " + fmtThousands(total));
        line(TextFormat("Events %d", int(events_.size())));
        if (const int sc = selectedCount()) line(TextFormat("Selected %d", sc), gfx::rgb(0x9AE6FF));
        y += 6;
        static const char* help[] = {"SPACE play", "Wheel/W/S move", "A/D section (SHIFT: 4)", "LEFT/RIGHT snap",
                                     "Click add/select", "SHIFT+drag select", "Ctrl+C/X/V copy, cut",
                                     "DEL delete", "M mirror", "Ctrl+Z/Y undo", "ENTER save+play"};
        for (const char* h : help) line(h, gfx::fade(WHITE, 0.55f), 15);
    }

    // The tabbed panel. Immediate mode: drawing and clicks happen together.
    void drawPanel() {
        if (ui_.click) ui_.focus.clear();  // a click anywhere ends typing; a text field takes it back
        DrawRectangleRounded({kPanelX - 8, 46, gfx::W - kPanelX, gfx::H - 54}, 0.03f, 6, kPanel);
        static const char* tabs[] = {"Song", "Section", "Note", "Events", "Charting", "EZ Spam"};
        const float tw = (gfx::W - kPanelX - 16) / 6.0f;
        for (int i = 0; i < 6; ++i)
            if (ui_.button({kPanelX + i * tw, 52, tw - 4, 30}, tabs[i], tab_ == i)) tab_ = i;
        const float x = kPanelX, w = gfx::W - kPanelX - 18;
        float y = 96;
        auto row = [&](float h = 30) {
            const Rectangle r{x, y, w, h};
            y += h + 8;
            return r;
        };
        auto half = [&](int i) { return Rectangle{x + i * (w * 0.5f + 4), y, w * 0.5f - 4, 30}; };
        auto heading = [&](const std::string& s) {
            gfx::text(FontId::Ui, s, x, y, 18, kAccent);
            y += 26;
        };
        switch (tab_) {
            case 0: {  // Song
                if (ui_.text("title", row(), "Song", title_)) dirty_ = true;
                if (ui_.text("p1", row(), "Player", player_)) dirty_ = true;
                if (ui_.text("p2", row(), "Opponent", opponent_)) dirty_ = true;
                if (ui_.text("gf", row(), "Girlfriend", gf_)) dirty_ = true;
                if (ui_.text("stage", row(), "Stage", stage_)) dirty_ = true;
                double bpm = sections_.empty() ? 120 : sections_[0].bpm;
                if (ui_.stepper(row(), "BPM", bpm, 1, 10, 2000, "%.2f")) setSectionBpm(0, bpm);
                if (ui_.stepper(row(), "Scroll speed", speed_, 0.1, 0.1, 10, "%.1f")) dirty_ = true;
                double k = keys_;
                if (ui_.stepper(row(), "Keys", k, 1, 1, 9, "%.0f")) setKeys(int(k));
                if (ui_.check(row(), "Needs voices", needsVoices_)) dirty_ = true;
                if (!eventsFile_.empty() && ui_.check(row(), "Events in events.json", eventsSeparate_)) dirty_ = true;
                y += 6;
                if (ui_.button(half(0), "Save (F2)")) save(false);
                if (ui_.button(half(1), "Save and play")) {
                    if (save(true)) return;
                }
                y += 38;
                if (ui_.button(half(0), "Clear notes")) {
                    snapshot();
                    notes_.clear();
                    dirty_ = true;
                }
                if (ui_.button(half(1), "Clear events")) {
                    snapshot();
                    events_.clear();
                    selEvent_ = -1;
                    dirty_ = true;
                }
                y += 38;
                gfx::text(FontId::Ui, "FL Studio (SNIFF): drop a .flp or .fsc here", x, y, 15, kDim);
                y += 20;
                gfx::text(FontId::Ui, "C3-D#3 player, C4-D#4 opponent, F3/F#3 camera", x, y, 15, kDim);
                y += 24;
                break;
            }
            case 1: {  // Section
                const int s = sectionAt(cur_);
                EdSection& sec = section(s);
                heading(TextFormat("Section %d", s));
                bool must = sec.mustHit, gf = sec.gf, alt = sec.altAnim;
                if (ui_.check(row(), "Must hit section (camera on the player)", must)) { snapshot(); section(s).mustHit = must; dirty_ = true; }
                if (ui_.check(row(), "GF section (girlfriend sings)", gf)) { snapshot(); section(s).gf = gf; dirty_ = true; }
                if (ui_.check(row(), "Alt animation (opponent)", alt)) { snapshot(); section(s).altAnim = alt; dirty_ = true; }
                double beats = section(s).beats;
                if (ui_.stepper(row(), "Beats", beats, 0.25, 0.25, 64, "%.2f")) {
                    snapshot();
                    section(s).beats = beats;
                    rebuildStarts();
                    dirty_ = true;
                }
                // Time signature: 7/16 is 7 sixteenth notes, 1.75 beats (Psych's sectionBeats).
                {
                    static const std::vector<std::string> dens = {"/2", "/4", "/8", "/16", "/32"};
                    static const int denVal[] = {2, 4, 8, 16, 32};
                    int di = 1;
                    double num = section(s).beats;
                    for (int k : {1, 2, 3, 4, 0}) {  // 4/4 rather than 2/2, 7/16 for 1.75 beats
                        const double n = section(s).beats * denVal[k] / 4.0;
                        if (std::fabs(n - std::round(n)) < 1e-6) { di = k; num = std::round(n); break; }
                    }
                    const Rectangle r = row();
                    gfx::text(FontId::Ui, "Time signature", r.x, r.y + 6, 18, kDim);
                    double numEdit = num;
                    int denEdit = di;
                    const bool c1 = ui_.stepper({r.x + 130, r.y, 150, r.height}, "", numEdit, 1, 1, 64, "%.0f");
                    const bool c2 = ui_.choice({r.x + 286, r.y, r.width - 286, r.height}, "", dens, denEdit, 0);
                    if (c1 || c2) {
                        snapshot();
                        section(s).beats = std::max(0.125, std::round(numEdit) * 4.0 / denVal[denEdit]);
                        rebuildStarts();
                        dirty_ = true;
                    }
                }
                if (ui_.button(row(), "Same length for every section after this")) {
                    snapshot();
                    for (int k = s + 1; k < int(sections_.size()); ++k) sections_[size_t(k)].beats = section(s).beats;
                    rebuildStarts();
                    dirty_ = true;
                }
                double sbpm = section(s).bpm;
                if (ui_.stepper(row(), "Section BPM", sbpm, 1, 10, 2000, "%.2f")) setSectionBpm(s, sbpm);
                y += 4;
                {
                    const float tw3 = (w - 8) / 3.0f;
                    if (ui_.button({x, y, tw3 - 2, 30}, "Copy (Ctrl+C)")) copySelectionOrSection();
                    if (ui_.button({x + tw3 + 2, y, tw3 - 2, 30}, "Cut (Ctrl+X)")) cutSelectionOrSection();
                    if (ui_.button({x + 2 * tw3 + 4, y, tw3 - 2, 30}, "Paste (Ctrl+V)")) paste();
                    y += 38;
                }
                if (ui_.button(half(0), "Clear notes")) clearSection(true, false);
                if (ui_.button(half(1), "Clear events")) clearSection(false, true);
                y += 38;
                if (ui_.button(half(0), "Swap sides")) swapSides();
                if (ui_.button(half(1), "Mirror lanes")) mirror();
                y += 38;
                if (ui_.stepper(row(), "Sections back", copyBack_, 1, 1, 64, "%.0f")) {}
                if (ui_.button(row(), "Copy notes from that section")) copyLastSection(int(copyBack_));
                if (ui_.button(row(), "Select all notes of the section (Ctrl+A)")) selectSection();
                break;
            }
            case 2: {  // Note
                EdNote* n = firstSelected();
                const int count = selectedCount();
                heading(count == 0 ? std::string("No note selected") : count == 1 ? std::string("Selected note")
                                                                                   : TextFormat("%d notes selected", count));
                if (ui_.choice(row(), "Type", noteTypes_, typeIdx_))
                    applyToSelection([&](EdNote& e) { e.type = noteTypes_[size_t(typeIdx_)]; });
                if (n) {
                    double t = n->t;
                    if (ui_.stepper(row(), "Time (ms)", t, 60000.0 / bpmAt(n->t) * snapBeats(), 0, 1e9, "%.1f")) {
                        const double d = t - n->t;
                        applyToSelection([&](EdNote& e) { e.t = std::max(0.0, e.t + d); });
                    }
                    double len = n->len;
                    if (ui_.stepper(row(), "Hold (ms)", len, 60000.0 / bpmAt(n->t) * snapBeats(), 0, 1e7, "%.1f"))
                        applyToSelection([&](EdNote& e) { e.len = len; });
                    double st = n->stack;
                    if (ui_.stepper(row(), "Stacked notes", st, 1, 1, 65535, "%.0f"))
                        applyToSelection([&](EdNote& e) { e.stack = int(st); });
                    if (ui_.button(half(0), "Delete")) deleteSelection();
                    if (ui_.button(half(1), "Unselect")) clearSelection();
                    y += 38;
                }
                y += 6;
                heading("Spam tools");
                ui_.stepper(row(), "Stack amount", stackAmount_, 1, 1, 65535, "%.0f");
                if (ui_.button(row(), "Stack the selected notes")) {
                    const int k = int(stackAmount_);
                    applyToSelection([&](EdNote& e) { e.stack = k; });
                }
                static const std::vector<std::string> patterns = {"One lane", "Stairs up", "Stairs down", "Random", "Zigzag"};
                static const std::vector<std::string> sides = {"Opponent", "Player", "Both"};
                ui_.choice(row(), "Pattern", patterns, fillPattern_);
                if (fillPattern_ == 0) {
                    double lane = fillLane_;
                    if (ui_.stepper(row(), "Lane", lane, 1, 0, keys_ - 1, "%.0f")) fillLane_ = int(lane);
                }
                ui_.choice(row(), "Side", sides, fillSide_);
                if (ui_.button(row(), "Fill the section (one note per snap)")) fillSection(fillPattern_, fillSide_);
                break;
            }
            case 3: {  // Events
                psych::ChartEvent* e = selEvent_ >= 0 && selEvent_ < long(events_.size()) ? &events_[size_t(selEvent_)] : nullptr;
                if (e) {
                    // Events sharing this time: step through them.
                    int first = int(selEvent_), lastI = int(selEvent_);
                    while (first > 0 && events_[size_t(first - 1)].timeMs == e->timeMs) --first;
                    while (lastI + 1 < int(events_.size()) && events_[size_t(lastI + 1)].timeMs == e->timeMs) ++lastI;
                    heading(TextFormat("Event %d of %d at %.0f ms", int(selEvent_) - first + 1, lastI - first + 1, e->timeMs));
                    if (ui_.button(half(0), "< previous")) selEvent_ = selEvent_ > first ? selEvent_ - 1 : lastI;
                    if (ui_.button(half(1), "next >")) selEvent_ = selEvent_ < lastI ? selEvent_ + 1 : first;
                    y += 38;
                    int idx = int(std::find(eventNames_.begin(), eventNames_.end(), e->name) - eventNames_.begin());
                    if (idx >= int(eventNames_.size())) {
                        eventNames_.push_back(e->name);
                        idx = int(eventNames_.size()) - 1;
                    }
                    if (ui_.dropdown("evname", row(), "Event", eventNames_, idx, 64)) {
                        snapshot();
                        e->name = eventNames_[size_t(idx)];
                        eventIdx_ = idx;
                        dirty_ = true;
                    }
                    if (ui_.text("ev1", row(), "Value 1", e->v1)) dirty_ = true;
                    if (ui_.text("ev2", row(), "Value 2", e->v2)) dirty_ = true;
                    drawEventHelp(e->name, y);
                    if (ui_.button(half(0), "Add here")) {
                        snapshot();
                        psych::ChartEvent c = *e;
                        events_.insert(events_.begin() + selEvent_ + 1, c);
                        ++selEvent_;
                        dirty_ = true;
                    }
                    if (ui_.button(half(1), "Remove")) {
                        snapshot();
                        events_.erase(events_.begin() + selEvent_);
                        selEvent_ = -1;
                        dirty_ = true;
                    }
                    y += 38;
                } else {
                    heading("New events");
                    ui_.dropdown("evnew", row(), "Event", eventNames_, eventIdx_, 64);
                    ui_.text("nv1", row(), "Value 1", eventV1_);
                    ui_.text("nv2", row(), "Value 2", eventV2_);
                    if (!eventNames_.empty()) drawEventHelp(eventNames_[size_t(eventIdx_)], y);
                    if (ui_.button(row(), "Add at the line")) {
                        snapshot();
                        psych::ChartEvent c;
                        c.timeMs = msAtBeat(snapBeat(beatAt(cur_)));
                        c.name = eventNames_.empty() ? std::string("Hey!") : eventNames_[size_t(eventIdx_)];
                        c.v1 = eventV1_;
                        c.v2 = eventV2_;
                        events_.push_back(c);
                        sortEvents();
                        selEvent_ = findEvent(c.timeMs, 0.01);
                        dirty_ = true;
                    }
                    gfx::text(FontId::Ui, "Click the EVENTS lane to add or select one;", x, y, 16, kDim);
                    gfx::text(FontId::Ui, "right click removes it.", x, y + 20, 16, kDim);
                }
                break;
            }
            case 4: {  // Charting
                if (ui_.stepper(row(), "Playback rate", playRate_, 0.05, 0.25, 3, "%.2fx") && playing_) app().mixer.setRate(playRate_);
                if (ui_.stepper(row(), "Music volume", instVol_, 0.1, 0, 1, "%.1f")) applyVolumes();
                if (ui_.stepper(row(), "Voices volume", voiceVol_, 0.1, 0, 1, "%.1f")) applyVolumes();
                if (ui_.check(row(), "Mute voices", muteVoices_)) applyVolumes();
                ui_.check(row(), "Hit sounds: player", hitPlayer_);
                ui_.check(row(), "Hit sounds: opponent", hitOpponent_);
                ui_.check(row(), "Metronome", metronome_);
                double snap = snapIdx_;
                if (ui_.stepper(row(), "Snap (1/N)", snap, 1, 0, double(std::size(kSnaps) - 1), "%.0f")) snapIdx_ = int(snap);
                gfx::text(FontId::Ui, TextFormat("= 1/%d of a bar", kSnaps[snapIdx_]), x, y, 16, kDim);
                y += 24;
                double z = zoom_;
                if (ui_.stepper(row(), "Zoom", z, 0.25, 0.25, 8, "%.2fx")) zoom_ = float(z);
                break;
            }
            case 5: {  // EZ Spam: JS Engine's "Note Spamming" tab
                heading("Note Spamming (as in JS Engine)");
                ui_.check(row(), "Enable EZ Spam Mode", ezMode_);
                auto twoButtons = [&](const char* a, const char* b, double& v, double lo, double hi) {
                    if (ui_.button(half(0), a)) v = clampv(v * 2, lo, hi);
                    if (ui_.button(half(1), b)) v = clampv(v / 2, lo, hi);
                    y += 38;
                };
                ui_.stepper(row(), "Spam Count", ezCount_, 1, 0, 999999, "%.4g");
                twoButtons("x2 Amount", "x0.5 Amount", ezCount_, 0, 999999);
                ui_.stepper(row(), "Spam Multiplier", ezMult_, 1, 0, 999999, "%.4g");
                twoButtons("x2 SM", "x0.5 SM", ezMult_, 0, 999999);
                ui_.stepper(row(), "Spam Scroll Amount", ezScroll_, 1, -9999, 9999, "%.0f");
                ui_.stepper(row(), "Stretch Amount", ezStretch_, 1, 0, 8192, "%.4g");
                twoButtons("x2 SH", "x0.5 SH", ezStretch_, 0, 8192);
                if (ui_.button(row(), "Stretch Notes")) ezStretchNotes();
                ui_.stepper(row(), "Steps to Shift By", ezShift_, 1, -8192, 8192, "%.4g");
                if (ui_.button(row(), "Shift Notes")) ezShiftNotes();
                ui_.stepper(row(), "Amount of Duplicates", ezDupes_, 1, 0, 32, "%.0f");
                if (ui_.button(row(), "Duplicate Notes")) ezDuplicateNotes();
                const double placed = std::max(1.0, std::floor(ezCount_ * ezMult_));
                gfx::text(FontId::Ui,
                          ezMode_ ? TextFormat("Each note placed becomes %s notes, %.4g per step.", fmtThousands((long long)placed).c_str(), ezMult_)
                                  : "Placing a note places Spam Count x Spam Multiplier notes when on.",
                          x, y, 16, kDim);
                y += 22;
                break;
            }
            default: break;
        }
        ui_.overlays();
    }

    // ------------------------------------------------- FL Studio (SNIFF) --
    // The notes of an FL Studio pattern replace the chart's (undo brings them
    // back): the pattern named like this difficulty, else the first with notes
    // in SNIFF's lanes. Scores (.fsc) have no tempo: the chart's is kept.
    void importFlp(const std::string& path) {
        FlpProject p;
        std::string err;
        if (!readFlpFile(path, p, err)) { flash("FL Studio: " + err); return; }
        int pattern = -1;
        for (const FlpPattern& pat : p.patterns)
            if (equalsI(pat.name, diff_)) pattern = pat.number;
        Chart c;
        if (!flpToChart(p, pattern, c, err, p.bpm > 0 ? 0.0 : bpmAt(0))) { flash("FL Studio: " + err); return; }
        snapshot();
        keys_ = 4;
        sections_.clear();
        for (const SectionInfo& si : c.sections) {
            EdSection s;
            s.mustHit = si.mustHit;
            s.altAnim = si.alt;
            s.beats = si.beats;
            s.bpm = si.bpm;
            sections_.push_back(s);
        }
        notes_.clear();
        for (const Note& n : c.notes) {
            EdNote e;
            e.t = n.time;
            e.col = n.side ? keys_ + n.lane : n.lane;
            e.len = n.length;
            if (n.side == 1 && (n.flags & kFlagAlt)) e.type = "Alt Animation";
            e.stack = std::max<int>(1, n.stack);
            notes_.push_back(e);
        }
        sortNotes();
        rebuildStarts();
        dirty_ = true;
        flash(TextFormat("Imported %s notes from %s (%s, %.0f BPM)", fmtThousands((long long)c.notes.size()).c_str(),
                         pathStr(toPath(path).filename()).c_str(), flpPatternLabel(p, pattern < 0 ? flpChartPatterns(p, false).front() : pattern).c_str(),
                         c.bpmAt(0)));
    }

    // ---------------------------------------------------------- EZ Spam --
    // JS Engine's "Note Spamming" tools, on the section under the line.
    // One step at the section's BPM, as JS Engine's 15000 / BPM.
    double sectionStepMs(int s) { return 15000.0 / std::max(1.0, section(s).bpm); }
    void ezStretchNotes() {
        snapshot();
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1);
        for (EdNote& n : notes_) {
            if (n.t < a - 0.5 || n.t >= b - 0.5) continue;
            if (n.len > 0) n.len *= ezStretch_;
            n.t = std::max(a, a + (n.t - a) * ezStretch_);
        }
        sortNotes();
        dirty_ = true;
        flash(TextFormat("Stretched the section's notes x%.4g", ezStretch_));
    }
    void ezShiftNotes() {
        snapshot();
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1), by = ezShift_ * sectionStepMs(s);
        for (EdNote& n : notes_)
            if (n.t >= a - 0.5 && n.t < b - 0.5) n.t = std::max(0.0, n.t + by);
        sortNotes();
        dirty_ = true;
        flash(TextFormat("Shifted the section's notes %.4g steps", ezShift_));
    }
    void ezDuplicateNotes() {
        snapshot();
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1), by = ezShift_ * sectionStepMs(s);
        std::vector<EdNote> src;
        for (const EdNote& n : notes_)
            if (n.t >= a - 0.5 && n.t < b - 0.5) src.push_back(n);
        // Each copy moves "Steps to Shift By" further, as in JS Engine.
        for (int d = 1; d <= int(ezDupes_); ++d)
            for (EdNote n : src) {
                n.t = std::max(0.0, n.t + by * d);
                n.sel = false;
                notes_.push_back(n);
            }
        sortNotes();
        dirty_ = true;
        flash(TextFormat("%s notes added", fmtThousands((long long)(src.size() * size_t(ezDupes_))).c_str()));
    }
    // EZ Spam Mode: a note placed at (t, col) brings Spam Count x Spam
    // Multiplier - 1 more, each 1/Multiplier step later and Scroll Amount
    // lanes over (wrapping round both sides' lanes).
    void ezPlaceSpam(const EdNote& first) {
        const long long more = (long long)std::floor(ezCount_ * ezMult_) - 1;
        if (more <= 0 || ezMult_ <= 0) return;
        const long long cap = std::min<long long>(more, 5000000);  // a click must not freeze the editor
        const double gap = sectionStepMs(sectionAt(first.t)) / ezMult_;
        const int cols = keys_ * 2, shift = int(std::floor(ezScroll_));
        EdNote n = first;
        n.sel = false;
        n.len = 0;
        notes_.reserve(notes_.size() + size_t(cap));
        for (long long k = 0; k < cap; ++k) {
            n.t += gap;
            n.col = ((n.col + shift) % cols + cols) % cols;
            notes_.push_back(n);
        }
        sortNotes();
        if (cap < more) flash("EZ Spam: stopped at 5,000,000 notes for one click");
    }

    void drawEventHelp(const std::string& name, float& y) {
        std::string help;
        for (const EventInfo& e : kEvents)
            if (name == e.name) help = e.help;
        if (help.empty())
            if (auto it = eventHelp_.find(name); it != eventHelp_.end()) help = it->second;
        if (help.empty()) help = "A custom event: its script decides what the values do.";
        // Word wrap to the panel.
        const float maxW = gfx::W - kPanelX - 24;
        std::string lineText, word;
        auto flushLine = [&]() {
            gfx::text(FontId::Ui, lineText, kPanelX, y, 16, kDim);
            y += 20;
            lineText.clear();
        };
        for (size_t i = 0; i <= help.size(); ++i) {
            const char c = i < help.size() ? help[i] : ' ';
            if (c == ' ' || c == '\n') {
                const std::string cand = lineText.empty() ? word : lineText + " " + word;
                if (gfx::measure(FontId::Ui, cand, 16).x > maxW && !lineText.empty()) {
                    flushLine();
                    lineText = word;
                } else {
                    lineText = cand;
                }
                word.clear();
                if (c == '\n') flushLine();
            } else {
                word += c;
            }
        }
        if (!lineText.empty()) flushLine();
        y += 8;
    }

    // ------------------------------------------------------------- state --
    std::string songId_, diff_, error_, title_, player_, opponent_, gf_, stage_, flash_;
    const SongEntry* entry_ = nullptr;
    std::future<Prepared> job_;
    bool loaded_ = false, playing_ = false, dirty_ = false, dragging_ = false, backedUp_ = false, typing_ = false;
    bool needsVoices_ = true, eventsSeparate_ = true, boxing_ = false;
    bool hitPlayer_ = true, hitOpponent_ = true, metronome_ = false, muteVoices_ = false;
    int keys_ = 4, snapIdx_ = 3, typeIdx_ = 0, tab_ = 2, eventIdx_ = 0, fillPattern_ = 1, fillSide_ = 1, fillLane_ = 0;
    // EZ Spam (JS Engine's Note Spamming tab)
    bool ezMode_ = false;
    double ezCount_ = 1, ezMult_ = 1, ezScroll_ = 0, ezStretch_ = 1, ezShift_ = 1, ezDupes_ = 1;
    long selEvent_ = -1;
    double cur_ = 0, lastT_ = 0, speed_ = 1, flashAt_ = -10, escAt_ = -10;
    double playRate_ = 1, instVol_ = 1, voiceVol_ = 1, stackAmount_ = 10, copyBack_ = 1;
    float zoom_ = 1.0f;
    Vector2 boxA_{};
    Ui ui_;
    std::vector<EdNote> notes_, clipboard_;
    std::vector<psych::ChartEvent> clipEvents_;
    std::vector<EdSection> sections_;
    std::vector<double> starts_, beatStarts_;
    std::vector<psych::ChartEvent> events_;
    std::vector<std::string> noteTypes_, eventNames_;
    std::map<std::string, std::string> eventHelp_;
    std::string eventV1_, eventV2_, eventsFile_;
    std::vector<UndoStep> undo_, redo_;
    PcmPtr inst_, voiceP_, voiceO_;
};

std::unique_ptr<State> makeChartEditor(const std::string& songId, const std::string& diff) {
    return std::make_unique<ChartEditorState>(songId, diff);
}

}  // namespace gh
