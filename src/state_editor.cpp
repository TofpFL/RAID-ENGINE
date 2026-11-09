// Chart editor (Freeplay: 7). A Psych-style grid: opponent lanes on the left,
// player lanes on the right, time flowing down past a fixed line. Saves
// legacy/Psych 0.x JSON that this game and Psych Engine both read.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>

#include "app.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "raylib.h"

namespace gh {

namespace {
namespace fs = std::filesystem;
using gfx::Align;
using gfx::FontId;

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

const char* kNoteTypes[] = {"", "Alt Animation", "Hurt Note", "GF Sing", "No Animation", "Hey!"};
const int kSnaps[] = {4, 8, 12, 16, 24, 32, 48, 64, 96, 192};

struct EdNote {
    double t = 0;
    int col = 0;  // 0..K-1 opponent, K..2K-1 player
    double len = 0;
    std::string type;
};

struct EdSection {
    bool mustHit = true, altAnim = false, gf = false;
    double bpm = 120;
};

bool writeWav(const std::string& path, const Pcm& p) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t frames = uint32_t(p.frames()), ch = uint32_t(p.channels), bytes = frames * ch * 2;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(uint16_t(ch)); w32(Mixer::kRate); w32(Mixer::kRate * ch * 2); w16(uint16_t(ch * 2)); w16(16);
    std::fwrite("data", 1, 4, f);
    w32(bytes);
    std::fwrite(p.data.data(), 2, p.data.size(), f);
    std::fclose(f);
    return true;
}

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c >= 0x20) o += c;
    }
    return o + "\"";
}

std::string num(double v) {
    char b[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.1f", v);
    else std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos && s.find('e') == std::string::npos) {
        while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    }
    return s;
}

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
            std::ofstream(toPath(a.autoRun.editorExport), std::ios::binary) << buildJson();
            a.quit = true;
            return;
        }
        (void)dt;
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

        // Playback
        if (in.pressed(KEY_SPACE)) togglePlay();
        if (playing_) {
            const double t = a.mixer.time(a.now) * 1000.0;
            tickHitsounds(lastT_, t);
            lastT_ = t;
            cur_ = t;
            if (a.mixer.ended() || cur_ > songLength() + 2000) stopPlay();
        }

        // Navigation
        const float wheel = GetMouseWheelMove();
        if (wheel != 0 || in.pressed(KEY_UP) || in.pressed(KEY_DOWN) || in.pressed(KEY_W) || in.pressed(KEY_S)) {
            if (playing_) stopPlay();
            int steps = wheel != 0 ? -int(std::round(wheel)) : (in.pressed(KEY_UP) || in.pressed(KEY_W) ? -1 : 1);
            if (shift) steps *= 4;
            const double b = snapBeat(beatAt(cur_)) + steps * snapBeats();
            cur_ = std::max(0.0, msAtBeat(b));
        }
        if (in.pressed(KEY_A) || in.pressed(KEY_PAGE_UP) || in.pressed(KEY_D) || in.pressed(KEY_PAGE_DOWN)) {
            if (playing_) stopPlay();
            const int s = sectionAt(cur_) + ((in.pressed(KEY_A) || in.pressed(KEY_PAGE_UP)) ? -1 : 1);
            cur_ = sectionStart(std::max(0, s));
        }
        if (in.pressed(KEY_HOME)) cur_ = 0;
        if (in.pressed(KEY_END)) cur_ = songLength();
        if (in.pressed(KEY_LEFT)) snapIdx_ = std::max(0, snapIdx_ - 1);
        if (in.pressed(KEY_RIGHT)) snapIdx_ = std::min(int(std::size(kSnaps)) - 1, snapIdx_ + 1);
        if (in.pressed(KEY_Z) && !ctrl) zoom_ = std::max(0.25f, zoom_ / 1.25f);
        if (in.pressed(KEY_X)) zoom_ = std::min(8.0f, zoom_ * 1.25f);

        // Editing
        if (ctrl && in.pressed(KEY_Z)) undo();
        if (ctrl && in.pressed(KEY_Y)) redo();
        if (ctrl && in.pressed(KEY_S)) save(false);
        if (in.pressed(KEY_TAB)) {
            snapshot();
            EdSection& s = section(sectionAt(cur_));
            s.mustHit = !s.mustHit;
            dirty_ = true;
        }
        if (in.pressed(KEY_T)) {
            typeIdx_ = (typeIdx_ + 1) % int(std::size(kNoteTypes));
            if (EdNote* n = selected()) { snapshot(); n->type = kNoteTypes[typeIdx_]; dirty_ = true; }
        }
        if (in.pressed(KEY_Q) || in.pressed(KEY_E)) {
            if (EdNote* n = selected()) {
                snapshot();
                const double stepMs = 60000.0 / bpmAt(n->t) * snapBeats();
                n->len = std::max(0.0, n->len + (in.pressed(KEY_E) ? stepMs : -stepMs));
                dirty_ = true;
            }
        }
        if (in.pressed(KEY_LEFT_BRACKET) || in.pressed(KEY_RIGHT_BRACKET)) {
            snapshot();
            const int s = sectionAt(cur_);
            const double step = shift ? 0.1 : 1.0;
            const double nb = clampv(section(s).bpm + (in.pressed(KEY_RIGHT_BRACKET) ? step : -step), 10.0, 1000.0);
            // A BPM change carries on to the following sections that shared the old value.
            const double old = section(s).bpm;
            for (size_t i = size_t(s); i < sections_.size() && std::fabs(sections_[i].bpm - old) < 1e-9; ++i) sections_[i].bpm = nb;
            dirty_ = true;
        }
        if (in.pressed(KEY_MINUS)) { speed_ = std::max(0.1, std::round((speed_ - 0.1) * 10) / 10); dirty_ = true; }
        if (in.pressed(KEY_EQUAL)) { speed_ = std::min(10.0, std::round((speed_ + 0.1) * 10) / 10); dirty_ = true; }
        if (ctrl && in.pressed(KEY_C)) copySection();
        if (ctrl && in.pressed(KEY_V)) pasteSection();
        if (in.pressed(KEY_DELETE)) clearSection();
        if (in.pressed(KEY_ENTER) || in.pressed(KEY_KP_ENTER)) {
            if (save(true)) return;
        }
        handleMouse();
        if (in.back()) {
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
        drawPanels();
        if (a.now - flashAt_ < 3.0)
            gfx::text(FontId::Ui, flash_, gfx::W / 2, gfx::H - 40, 24,
                      gfx::fade(gfx::rgb(0xFFE08A), float(std::min(1.0, 3.0 - (a.now - flashAt_)))), Align::Center, 1.5f);
        gfx::endCanvas();
    }

private:
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
        // Sections of 4 beats with the BPM in force at their start.
        const double audioMs = inst_ ? double(inst_->frames()) * 1000.0 / Mixer::kRate : 0.0;
        const double endMs = std::max({c.lengthMs + 4000.0, audioMs, 8000.0});
        double t = 0;
        while (t < endMs && sections_.size() < 200000) {
            EdSection s;
            s.bpm = c.bpmAt(t + 0.01);
            s.mustHit = c.focusAt(t + 1) == 1;
            sections_.push_back(s);
            t += 4 * 60000.0 / s.bpm;
        }
        notes_.reserve(c.notes.size());
        for (const Note& n : c.notes) {
            EdNote e;
            e.t = n.time;
            e.col = n.side ? keys_ + n.lane : n.lane;
            e.len = n.length;
            e.type = n.type < c.noteTypes.size() ? c.noteTypes[n.type] : "";
            if (e.type.empty() && n.kind == kMine) e.type = "Hurt Note";
            notes_.push_back(e);
        }
        sortNotes();
        events_ = c.events;
        rebuildStarts();
        loaded_ = true;
        App& a = app();
        a.mixer.setTracks(inst_, voiceP_, voiceO_);
        a.mixer.setRate(1.0);
        if (entry_->builtin) flash("Built-in song: saving makes an editable copy in songs/");
        else if (entry_->autoChart) flash("AutoChart draft: save to keep it as a real chart");
    }

    // ------------------------------------------------------------ timing --
    void rebuildStarts() {
        starts_.resize(sections_.size() + 1);
        starts_[0] = 0;
        for (size_t i = 0; i < sections_.size(); ++i) starts_[i + 1] = starts_[i] + 4 * 60000.0 / sections_[i].bpm;
    }
    EdSection& section(int i) {
        while (int(sections_.size()) <= i) {
            EdSection s = sections_.empty() ? EdSection{} : sections_.back();
            sections_.push_back(s);
        }
        return sections_[size_t(std::max(0, i))];
    }
    int sectionAt(double ms) {
        if (starts_.size() != sections_.size() + 1) rebuildStarts();
        if (ms < 0) return 0;
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), ms);
        int i = int(it - starts_.begin()) - 1;
        if (i >= int(sections_.size())) {
            // Past the end: extend with the last tempo.
            const double last = sections_.empty() ? 120 : sections_.back().bpm;
            i = int(sections_.size()) + int((ms - starts_.back()) / (4 * 60000.0 / last));
        }
        return std::max(0, i);
    }
    double sectionStart(int i) {
        if (starts_.size() != sections_.size() + 1) rebuildStarts();
        if (i < int(starts_.size())) return starts_[size_t(i)];
        const double last = sections_.empty() ? 120 : sections_.back().bpm;
        return starts_.back() + (i - int(sections_.size())) * 4 * 60000.0 / last;
    }
    double bpmAt(double ms) {
        const int s = sectionAt(ms);
        return s < int(sections_.size()) ? sections_[size_t(s)].bpm : (sections_.empty() ? 120 : sections_.back().bpm);
    }
    double beatAt(double ms) {
        const int s = sectionAt(ms);
        return s * 4.0 + (ms - sectionStart(s)) * bpmAt(ms) / 60000.0;
    }
    double msAtBeat(double beat) {
        if (beat <= 0) return beat * 60000.0 / bpmAt(0);
        const int s = int(std::floor(beat / 4.0));
        const double bpm = s < int(sections_.size()) ? sections_[size_t(s)].bpm : (sections_.empty() ? 120 : sections_.back().bpm);
        return sectionStart(s) + (beat - s * 4.0) * 60000.0 / bpm;
    }
    double snapBeats() const { return 4.0 / kSnaps[snapIdx_]; }
    double snapBeat(double b) const { return std::round(b / snapBeats()) * snapBeats(); }
    double songLength() {
        double l = inst_ ? double(inst_->frames()) * 1000.0 / Mixer::kRate : 0.0;
        if (!notes_.empty()) l = std::max(l, notes_.back().t + notes_.back().len);
        return l;
    }

    // ------------------------------------------------------------ editing --
    void sortNotes() {
        std::stable_sort(notes_.begin(), notes_.end(), [](const EdNote& a, const EdNote& b) {
            return a.t != b.t ? a.t < b.t : a.col < b.col;
        });
    }
    void snapshot() {
        undo_.push_back({notes_, sections_, speed_});
        const size_t cap = notes_.size() > 200000 ? 4 : 60;
        while (undo_.size() > cap) undo_.erase(undo_.begin());
        redo_.clear();
    }
    void undo() {
        if (undo_.empty()) return;
        redo_.push_back({notes_, sections_, speed_});
        notes_ = std::move(undo_.back().notes);
        sections_ = std::move(undo_.back().sections);
        speed_ = undo_.back().speed;
        undo_.pop_back();
        rebuildStarts();
        selected_ = -1;
        dirty_ = true;
        flash("Undo");
    }
    void redo() {
        if (redo_.empty()) return;
        undo_.push_back({notes_, sections_, speed_});
        notes_ = std::move(redo_.back().notes);
        sections_ = std::move(redo_.back().sections);
        speed_ = redo_.back().speed;
        redo_.pop_back();
        rebuildStarts();
        selected_ = -1;
        dirty_ = true;
        flash("Redo");
    }
    EdNote* selected() { return selected_ >= 0 && selected_ < long(notes_.size()) ? &notes_[size_t(selected_)] : nullptr; }

    long findNote(double t, int col, double tolMs) {
        auto lo = std::lower_bound(notes_.begin(), notes_.end(), t - tolMs, [](const EdNote& n, double v) { return n.t < v; });
        for (auto it = lo; it != notes_.end() && it->t <= t + tolMs; ++it)
            if (it->col == col) return long(it - notes_.begin());
        return -1;
    }

    void copySection() {
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1);
        clipboard_.clear();
        for (const EdNote& n : notes_)
            if (n.t >= a - 0.5 && n.t < b - 0.5) {
                EdNote c = n;
                c.t -= a;
                clipboard_.push_back(c);
            }
        flash(TextFormat("Copied %d notes", int(clipboard_.size())));
    }
    void pasteSection() {
        if (clipboard_.empty()) return;
        snapshot();
        const double a = sectionStart(sectionAt(cur_));
        for (EdNote c : clipboard_) {
            c.t += a;
            if (findNote(c.t, c.col, 0.5) < 0) notes_.push_back(c);
        }
        sortNotes();
        dirty_ = true;
        flash(TextFormat("Pasted %d notes", int(clipboard_.size())));
    }
    void clearSection() {
        snapshot();
        const int s = sectionAt(cur_);
        const double a = sectionStart(s), b = sectionStart(s + 1);
        notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const EdNote& n) { return n.t >= a - 0.5 && n.t < b - 0.5; }),
                     notes_.end());
        selected_ = -1;
        dirty_ = true;
    }

    // ------------------------------------------------------------- layout --
    float laneW() const { return keys_ > 6 ? 40.0f : 52.0f; }
    float gridX() const { return gfx::W * 0.5f - laneW() * keys_; }
    float lineY() const { return 220.0f; }
    float pxPerMs() const { return 0.45f * zoom_; }
    Vector2 mouse() const {
        const float sw = float(GetScreenWidth()), sh = float(GetScreenHeight());
        const float s = std::min(sw / gfx::W, sh / gfx::H);
        const Vector2 m = GetMousePosition();
        return {(m.x - (sw - gfx::W * s) * 0.5f) / s, (m.y - (sh - gfx::H * s) * 0.5f) / s};
    }
    bool mouseCell(int& col, double& t) {
        const Vector2 m = mouse();
        const float x0 = gridX();
        if (m.x < x0 || m.x >= x0 + laneW() * 2 * keys_ || m.y < 40 || m.y > gfx::H) return false;
        col = int((m.x - x0) / laneW());
        const double raw = cur_ + (m.y - lineY()) / pxPerMs();
        t = msAtBeat(snapBeat(beatAt(std::max(0.0, raw))));
        return t >= 0;
    }

    void handleMouse() {
        int col = 0;
        double t = 0;
        const bool inGrid = mouseCell(col, t);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && inGrid) {
            const long hit = findNote(t, col, 60000.0 / bpmAt(t) * snapBeats() * 0.45);
            snapshot();
            if (hit >= 0) {
                if (selected_ == hit) {  // second click on a selected note removes it
                    notes_.erase(notes_.begin() + hit);
                    selected_ = -1;
                } else {
                    selected_ = hit;
                }
            } else {
                EdNote n;
                n.t = t;
                n.col = col;
                n.type = kNoteTypes[typeIdx_];
                notes_.push_back(n);
                sortNotes();
                selected_ = findNote(t, col, 0.01);
                dragging_ = true;
            }
            dirty_ = true;
        }
        if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            // Drag down to stretch the new note into a hold.
            if (EdNote* n = selected()) {
                const double raw = cur_ + (mouse().y - lineY()) / pxPerMs();
                const double end = msAtBeat(snapBeat(beatAt(std::max(0.0, raw))));
                n->len = std::max(0.0, end - n->t);
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging_ = false;
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && inGrid) {
            const long hit = findNote(t, col, 60000.0 / bpmAt(t) * snapBeats() * 0.45);
            if (hit >= 0) {
                snapshot();
                notes_.erase(notes_.begin() + hit);
                selected_ = -1;
                dirty_ = true;
            }
        }
    }

    // ----------------------------------------------------------- playback --
    void togglePlay() {
        if (playing_) { stopPlay(); return; }
        App& a = app();
        a.mixer.setTracks(inst_, voiceP_, voiceO_);
        a.mixer.play(cur_ / 1000.0);
        playing_ = true;
        lastT_ = cur_;
    }
    void stopPlay() {
        if (!playing_) return;
        app().mixer.pause();
        playing_ = false;
    }
    void tickHitsounds(double from, double to) {
        if (to <= from || to - from > 500) return;
        auto lo = std::upper_bound(notes_.begin(), notes_.end(), from, [](double v, const EdNote& n) { return v < n.t; });
        int count = 0;
        for (auto it = lo; it != notes_.end() && it->t <= to && count < 4; ++it, ++count)
            sfxPlay(it->col >= keys_ ? Sfx::Hit : Sfx::Scroll, it->col >= keys_ ? 0.45f : 0.25f);
    }

    // ------------------------------------------------------------- saving --
    std::string buildJson() {
        // Sections up to the last note (at least one).
        int last = 0;
        if (!notes_.empty()) last = sectionAt(notes_.back().t);
        std::string o;
        o.reserve(notes_.size() * 28 + 4096);
        o += "{\"song\": {\n  \"song\": " + jsonStr(title_) + ",\n  \"bpm\": " + num(sections_.empty() ? 120 : sections_[0].bpm) +
             ",\n  \"speed\": " + num(speed_) + ",\n  \"needsVoices\": " + (voiceP_ || voiceO_ ? "true" : "false") +
             ",\n  \"player1\": " + jsonStr(player_) + ",\n  \"player2\": " + jsonStr(opponent_);
        if (!gf_.empty()) o += ",\n  \"gfVersion\": " + jsonStr(gf_);
        if (!stage_.empty()) o += ",\n  \"stage\": " + jsonStr(stage_);
        if (keys_ != 4) o += ",\n  \"keyCount\": " + std::to_string(keys_) + ",\n  \"mania\": " + std::to_string(keys_ - 1);
        o += ",\n  \"events\": [";
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
        o += events_.empty() ? "]" : "\n  ]";
        o += ",\n  \"notes\": [";
        size_t ni = 0;
        double prevBpm = sections_.empty() ? 120 : sections_[0].bpm;
        for (int s = 0; s <= last; ++s) {
            const EdSection& sec = section(s);
            const double a = sectionStart(s), b = sectionStart(s + 1);
            o += std::string(s ? "," : "") + "\n    {\"sectionBeats\": 4, \"lengthInSteps\": 16, \"mustHitSection\": " +
                 (sec.mustHit ? "true" : "false");
            if (std::fabs(sec.bpm - prevBpm) > 1e-9) o += ", \"changeBPM\": true, \"bpm\": " + num(sec.bpm);
            else o += ", \"changeBPM\": false, \"bpm\": " + num(sec.bpm);
            prevBpm = sec.bpm;
            if (sec.altAnim) o += ", \"altAnim\": true";
            if (sec.gf) o += ", \"gfSection\": true";
            o += ", \"sectionNotes\": [";
            bool first = true;
            while (ni < notes_.size() && (notes_[ni].t < b - 0.5 || s == last)) {
                const EdNote& n = notes_[ni++];
                if (n.t < a - 0.5 && s != 0) continue;
                // noteData is relative to the section's side: 0..K-1 is the side it focuses.
                const bool player = n.col >= keys_;
                const int lane = player ? n.col - keys_ : n.col;
                const int data = (player == sec.mustHit) ? lane : lane + keys_;
                o += std::string(first ? "" : ", ") + "[" + num(n.t) + ", " + std::to_string(data) + ", " + num(n.len);
                if (!n.type.empty()) o += ", " + jsonStr(n.type);
                o += "]";
                first = false;
            }
            o += "]}";
        }
        o += "\n  ]\n}}\n";
        return o;
    }

    // Where this chart is written. Mod and custom charts are saved in place
    // (the original is kept once as .bak); generated charts get a new file.
    std::string savePath() {
        const SongEntry& e = *entry_;
        for (size_t i = 0; i < e.diffs.size(); ++i)
            if (e.diffs[i] == diff_ && i < e.chartPaths.size() && !e.chartPaths[i].empty() && e.metaPath.empty())
                return e.chartPaths[i];
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
        const std::string text = buildJson();
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(text.data(), std::streamsize(text.size()));
            if (!out) {
                flash("Could not save " + path);
                return false;
            }
        }
        dirty_ = false;
        flash("Saved " + pathStr(p.filename()) + TextFormat(" (%d notes)", int(notes_.size())));
        // New files: rescan so Freeplay (and the playtest) see the chart.
        std::string id = songId_;
        if (entry_->builtin || entry_->autoChart || !fs::equivalent(p, toPath(savePath()), ec)) {
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
        const float lw = laneW(), x0 = gridX(), ly = lineY(), pp = pxPerMs();
        const int cols = 2 * keys_;
        const float gw = lw * cols;
        const double tTop = cur_ - (ly - 40) / pp, tBot = cur_ + (gfx::H - ly) / pp;
        // Section backgrounds: the camera's side is lighter.
        const int s0 = sectionAt(std::max(0.0, tTop)), s1 = sectionAt(std::max(0.0, tBot));
        for (int s = s0; s <= s1; ++s) {
            const float ya = ly + float((sectionStart(s) - cur_) * pp), yb = ly + float((sectionStart(s + 1) - cur_) * pp);
            const bool must = section(s).mustHit;
            DrawRectangleRec({x0, std::max(40.0f, ya), lw * keys_, std::min(float(gfx::H), yb) - std::max(40.0f, ya)},
                             must ? Color{28, 22, 40, 255} : Color{44, 34, 66, 255});
            DrawRectangleRec({x0 + lw * keys_, std::max(40.0f, ya), lw * keys_, std::min(float(gfx::H), yb) - std::max(40.0f, ya)},
                             must ? Color{44, 34, 66, 255} : Color{28, 22, 40, 255});
        }
        // Beat / snap lines
        const double b0 = std::floor(beatAt(std::max(0.0, tTop))), b1 = std::ceil(beatAt(std::max(0.0, tBot)));
        for (double b = b0; b <= b1; b += snapBeats()) {
            const float y = ly + float((msAtBeat(b) - cur_) * pp);
            if (y < 40 || y > gfx::H) continue;
            const bool beat = std::fabs(b - std::round(b)) < 1e-6;
            const bool sec = beat && int(std::round(b)) % 4 == 0;
            DrawRectangleRec({x0, y - (sec ? 1.5f : 0.5f), gw, sec ? 3.0f : 1.0f},
                             sec ? Color{170, 140, 220, 200} : beat ? Color{110, 90, 150, 140} : Color{80, 66, 110, 70});
            if (sec) gfx::text(FontId::Ui, std::to_string(int(std::round(b)) / 4), x0 - 10, y - 10, 18, gfx::fade(WHITE, 0.6f), Align::Right);
        }
        for (int c = 0; c <= cols; ++c)
            DrawRectangleRec({x0 + c * lw - (c == keys_ ? 1.5f : 0.5f), 40, c == keys_ ? 3.0f : 1.0f, gfx::H - 40.0f},
                             c == keys_ ? Color{200, 170, 255, 200} : Color{70, 58, 96, 160});
        // Notes in view (sustains first)
        auto lo = std::lower_bound(notes_.begin(), notes_.end(), tTop - 60000.0, [](const EdNote& n, double v) { return n.t < v; });
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
                const bool sel = selected_ >= 0 && &n == &notes_[size_t(selected_)];
                const bool mine = n.type.find("Hurt") != std::string::npos;
                if (mine) DrawCircleV({cx, y}, lw * 0.32f, Color{220, 40, 60, 255});
                else gfx::drawNote(dir, {cx, y}, lw * 0.82f, 1.0f);
                if (!n.type.empty() && !mine) DrawCircleV({cx + lw * 0.3f, y - lw * 0.3f}, 5, gfx::rgb(0xFFD54A));
                if (sel) DrawRectangleLinesEx({cx - lw * 0.48f, y - lw * 0.48f, lw * 0.96f, lw * 0.96f}, 2, gfx::rgb(0xFFFFFF));
            }
        }
        // Mouse cursor cell
        int col = 0;
        double t = 0;
        if (mouseCell(col, t)) {
            const float y = ly + float((t - cur_) * pp);
            DrawRectangleRec({x0 + col * lw + 2, y - lw * 0.45f, lw - 4, lw * 0.9f}, Color{255, 255, 255, 40});
        }
        // Strum line and receptors
        DrawRectangleRec({x0 - 6, ly - 2, gw + 12, 4}, Color{255, 220, 120, 220});
        for (int c = 0; c < cols; ++c) {
            const int dir = gfx::laneDir(keys_, c % keys_);
            gfx::drawReceptor(dir, {x0 + (c + 0.5f) * lw, ly}, lw * 0.82f, Color{200, 190, 230, 120});
        }
        // Top bar
        DrawRectangleRec({0, 0, gfx::W, 40}, Color{8, 5, 14, 235});
        gfx::text(FontId::Ui, "OPPONENT", x0 + lw * keys_ * 0.5f, 8, 22, gfx::fade(WHITE, 0.75f), Align::Center);
        gfx::text(FontId::Ui, "PLAYER", x0 + lw * keys_ * 1.5f, 8, 22, gfx::fade(WHITE, 0.75f), Align::Center);
    }

    void drawPanels() {
        const float x = 18;
        float y = 56;
        auto line = [&](const std::string& s, Color c = Color{230, 225, 245, 255}, float size = 20) {
            gfx::text(FontId::Ui, s, x, y, size, c);
            y += size + 6;
        };
        DrawRectangleRounded({8, 48, 300, 430}, 0.04f, 6, Color{10, 6, 18, 220});
        line(title_, gfx::rgb(0xFFE08A), 24);
        line(diff_ + (dirty_ ? "   (unsaved)" : ""), gfx::fade(WHITE, 0.7f));
        const int sec = sectionAt(cur_);
        const double beat = beatAt(cur_);
        line("Time  " + fmtTime(cur_ / 1000.0) + TextFormat(".%03d", int(std::fmod(std::max(0.0, cur_), 1000.0))));
        line(TextFormat("Beat  %.2f   Step  %d", beat, int(std::floor(beat * 4))));
        line(TextFormat("Section  %d  (camera: %s)", sec, section(sec).mustHit ? "player" : "opponent"));
        line(TextFormat("BPM  %.2f", bpmAt(cur_)));
        line(TextFormat("Scroll speed  %.1f", speed_));
        line(TextFormat("Snap  1/%d", kSnaps[snapIdx_]));
        line(TextFormat("Keys  %d   Zoom  %.2fx", keys_, zoom_));
        line(std::string("Note type  ") + (kNoteTypes[typeIdx_][0] ? kNoteTypes[typeIdx_] : "normal"));
        line(TextFormat("Notes  %s", fmtThousands((long long)notes_.size()).c_str()));
        if (const EdNote* n = selected())
            line(TextFormat("Selected  %.0f ms, hold %.0f ms", n->t, n->len), gfx::rgb(0x9AE6FF), 18);
        // Help
        const float hx = gfx::W - 300;
        float hy = 56;
        DrawRectangleRounded({hx - 10, 48, 300, 560}, 0.04f, 6, Color{10, 6, 18, 220});
        static const char* help[] = {
            "SPACE  play / pause",       "Wheel, W/S  move by snap",    "A/D  previous / next section",
            "LEFT/RIGHT  snap",          "Click  add / select note",    "Click selected  delete",
            "Drag down  hold length",    "Right click  delete",         "Q/E  shorter / longer hold",
            "T  note type",              "TAB  camera side of section", "[ ]  BPM (SHIFT: 0.1)",
            "- =  scroll speed",         "Z/X  zoom",                   "Ctrl+C / Ctrl+V  copy section",
            "DEL  clear section",        "Ctrl+Z / Ctrl+Y  undo, redo", "Ctrl+S  save",
            "ENTER  save and play",      "ESC  back"};
        for (const char* h : help) {
            gfx::text(FontId::Ui, h, hx, hy, 18, gfx::fade(WHITE, 0.75f));
            hy += 26;
        }
    }

    // ------------------------------------------------------------- state --
    struct UndoStep {
        std::vector<EdNote> notes;
        std::vector<EdSection> sections;
        double speed = 1;
    };

    std::string songId_, diff_, error_, title_, player_, opponent_, gf_, stage_, flash_;
    const SongEntry* entry_ = nullptr;
    std::future<Prepared> job_;
    bool loaded_ = false, playing_ = false, dirty_ = false, dragging_ = false, backedUp_ = false;
    int keys_ = 4, snapIdx_ = 3, typeIdx_ = 0;
    long selected_ = -1;
    double cur_ = 0, lastT_ = 0, speed_ = 1, flashAt_ = -10, escAt_ = -10;
    float zoom_ = 1.0f;
    std::vector<EdNote> notes_, clipboard_;
    std::vector<EdSection> sections_;
    std::vector<double> starts_;
    std::vector<psych::ChartEvent> events_;
    std::vector<UndoStep> undo_, redo_;
    PcmPtr inst_, voiceP_, voiceO_;
};

std::unique_ptr<State> makeChartEditor(const std::string& songId, const std::string& diff) {
    return std::make_unique<ChartEditorState>(songId, diff);
}

}  // namespace gh
