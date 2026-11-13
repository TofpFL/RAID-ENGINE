// Title, main menu, story, freeplay and credits screens.
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "characters.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "platform.hpp"
#include "raylib.h"

#ifndef GH_VERSION
#define GH_VERSION "dev"  // set by CMake from project(VERSION)
#endif

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {

const Color kGold{255, 214, 120, 255};
const Color kIvory{246, 236, 214, 255};

Color diffColor(const std::string& d) {
    const std::string l = toLower(d);
    if (l == "easy") return Color{120, 220, 140, 255};
    if (l == "normal") return Color{255, 214, 100, 255};
    if (l == "hard") return Color{255, 120, 90, 255};
    if (l == "infernal") return Color{255, 50, 70, 255};
    return Color{190, 160, 255, 255};
}

void hints(const char* s) {
    if (app().touchMode) return;  // the on-screen buttons speak for themselves
    DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
    gfx::text(FontId::Ui, s, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
}

double lastBeatTime() {
    const double b = app().menuBeat();
    const double frac = b - std::floor(b);
    return app().now - frac * 60.0 / app().menuBpm();
}

std::vector<std::string> wrap(const std::string& s, size_t width) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush = [&] {
        if (cur.size() + word.size() + (cur.empty() ? 0 : 1) > width && !cur.empty()) {
            lines.push_back(cur);
            cur.clear();
        }
        if (!word.empty()) cur += (cur.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush();
        else word += c;
    }
    flush();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Background jobs that must not block the UI (or the exit) while composing.
std::atomic<int> g_jobs{0};
struct PcmJob {
    struct Slot {
        std::mutex m;
        bool ready = false;
        PcmPtr pcm;
    };
    std::shared_ptr<Slot> slot;
    template <class F> void start(F fn) {
        slot = std::make_shared<Slot>();
        auto s = slot;
        ++g_jobs;
        std::thread([s, fn] {
            PcmPtr r = fn();
            {
                std::lock_guard<std::mutex> lk(s->m);
                s->pcm = r;
                s->ready = true;
            }
            --g_jobs;
        }).detach();
    }
    bool running() const { return slot != nullptr; }
    bool ready() const {
        if (!slot) return false;
        std::lock_guard<std::mutex> lk(slot->m);
        return slot->ready;
    }
    PcmPtr take() {
        PcmPtr r;
        if (slot) {
            std::lock_guard<std::mutex> lk(slot->m);
            r = slot->pcm;
        }
        slot.reset();
        return r;
    }
};

}  // namespace

void waitForBackgroundJobs() {
    for (int i = 0; i < 600 && g_jobs.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// ------------------------------------------------------------------ Title --

class TitleState : public State {
public:
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.tapCode = KEY_ENTER;
        return t;
    }
    void update(double) override {
        App& a = app();
        flash_ = std::max(0.0f, flash_ - float(a.dt) * 1.8f);
        if (!leaving_ && (a.input.accept() || a.input.firstPressed() != 0) && !a.input.pressed(KEY_F11)) {
            leaving_ = true;
            leaveAt_ = a.now;
            flash_ = 1.0f;
            sfxPlay(Sfx::Confirm);
        }
        if (leaving_ && a.now - leaveAt_ > 0.75) a.change(makeMainMenu());
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float pulse = menuPulse();
        gfx::beginCanvas();
        drawMenuBackground(now, 0xF4C76B, pulse);
        CharacterState p;
        p.id = CharId::Principality;
        p.beatAt = lastBeatTime();
        drawCharacter(p, {230, 690}, 0.9f, now, false);
        CharacterState h;
        h.id = CharId::Herald;
        h.beatAt = p.beatAt;
        drawCharacter(h, {1060, 690}, 0.95f, now, true);
        const float s = 1.0f + 0.025f * pulse;
        gfx::glowText(FontId::Title, "GOD'S HAND", gfx::W / 2, 150 - 6 * s, 104 * s, kGold, Color{255, 190, 80, 255});
        gfx::text(FontId::Title, "of Principality and Demise", gfx::W / 2, 286, 38, kIvory, Align::Center, 2.0f);
        const float blink = leaving_ ? (std::fmod(float(now) * 12.0f, 2.0f) < 1.0f ? 1.0f : 0.2f)
                                     : 0.55f + 0.45f * std::sin(float(now) * 3.5f);
        gfx::text(FontId::Ui, a.touchMode ? "TAP TO BEGIN" : "PRESS ENTER TO BEGIN", gfx::W / 2, 470, 40, fade(WHITE, blink),
                  Align::Center, 2.0f);
        gfx::text(FontId::Ui, "A rhythm duel  -  v" GH_VERSION, gfx::W / 2, 680, 22, fade(kIvory, 0.7f), Align::Center, 1.0f);
        if (!a.mixer.audioOk())
            gfx::text(FontId::Ui, "No audio device found - the game will run silently", gfx::W / 2, 520, 22,
                      Color{255, 160, 140, 255}, Align::Center, 1.5f);
        if (flash_ > 0) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(WHITE, flash_ * 0.8f));
        gfx::endCanvas();
    }

private:
    bool leaving_ = false;
    double leaveAt_ = 0;
    float flash_ = 0;
};

// -------------------------------------------------------------- Main menu --

class MainMenuState : public State {
public:
    void enter() override { anim_ = float(app().mainSel); }
    void update(double dt) override {
        App& a = app();
        int& sel = a.mainSel;
        anim_ = approach(anim_, float(sel), 14.0f, float(dt));
        if (chosen_ >= 0) {
            if (a.now - chosenAt_ > 0.55) {
                switch (chosen_) {
                    case 0: a.change(makeStory()); break;
                    case 1: a.change(makeFreeplay()); break;
                    case 2: a.change(makeOptions()); break;
                    case 3: a.change(makeCredits()); break;
                    default: a.quit = true; break;
                }
            }
            return;
        }
        if (a.input.up()) { sel = (sel + 4) % 5; sfxPlay(Sfx::Scroll); }
        if (a.input.down()) { sel = (sel + 1) % 5; sfxPlay(Sfx::Scroll); }
        if (a.input.accept()) {
            chosen_ = sel;
            chosenAt_ = a.now;
            sfxPlay(sel == 4 ? Sfx::Cancel : Sfx::Confirm);
        }
        if (a.input.back()) { sfxPlay(Sfx::Cancel); a.change(makeTitle()); }
        // 7: the editors (chart, character, stage, events), as Psych's Master Editor Menu.
        if (a.input.pressed(KEY_SEVEN) || a.input.pressed(KEY_KP_7)) {
            sfxPlay(Sfx::Confirm);
            a.change(makeEditorMenu());
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float pulse = menuPulse();
        static const char* items[5] = {"STORY MODE", "FREEPLAY", "OPTIONS", "CREDITS", "QUIT"};
        gfx::beginCanvas();
        drawMenuBackground(now, 0x8C6CFF, pulse);
        CharacterState h;
        h.id = CharId::Herald;
        h.beatAt = lastBeatTime();
        gfx::glow({1080, 500}, 230 + 20 * pulse, fade(kGold, 0.3f));
        drawCharacter(h, {1080, 640}, 0.85f, now, true);
        gfx::text(FontId::Title, "GOD'S HAND", 60, 40, 44, kGold, Align::Left, 2.0f);
        for (int i = 0; i < 5; ++i) {
            const bool on = i == a.mainSel;
            const float y = 165.0f + i * 92.0f - (on ? 6.0f : 0.0f);
            float size = on ? 66.0f + 4.0f * pulse : 50.0f;
            Color c = on ? kGold : fade(kIvory, 0.55f);
            if (chosen_ >= 0 && on && std::fmod(float(now - chosenAt_) * 10.0f, 2.0f) < 1.0f) c = WHITE;
            if (chosen_ >= 0 && !on) c = fade(c, std::max(0.0f, 1.0f - float(now - chosenAt_) * 3.0f));
            if (on) gfx::glowText(FontId::Title, items[i], gfx::W / 2, y, size, c, Color{255, 180, 80, 255});
            else gfx::text(FontId::Title, items[i], gfx::W / 2, y + 6, size, c, Align::Center, 2.0f);
            if (on) {
                const float w = gfx::measure(FontId::Title, items[i], size).x * 0.5f + 40 + 6 * pulse;
                gfx::drawNote(gfx::kRight, {gfx::W / 2 - w, y + size * 0.55f}, 44);
                gfx::drawNote(gfx::kLeft, {gfx::W / 2 + w, y + size * 0.55f}, 44);
            }
        }
        hints("UP/DOWN select   ENTER confirm   7 editors   ESC back   F11 fullscreen");
        gfx::text(FontId::Ui, a.library.report(), 16, gfx::H - 70, 20, fade(kIvory, 0.6f));
        gfx::endCanvas();
    }

private:
    float anim_ = 0;
    int chosen_ = -1;
    double chosenAt_ = 0;
};

// ------------------------------------------------------------------ Story --

class StoryState : public State {
public:
    void update(double) override {
        App& a = app();
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        int& d = a.storyDiff;
        d = clampv(d, 0, 2);
        if (a.input.left()) { d = (d + 2) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.right()) { d = (d + 1) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.back()) { sfxPlay(Sfx::Cancel); a.change(makeMainMenu()); }
        if (a.input.accept() && !go_) {
            go_ = true;
            sfxPlay(Sfx::Confirm);
            PlayRequest r;
            r.songId = "builtin:principality";
            r.diff = diffs[d];
            r.story = true;
            r.storyQueue = {"builtin:godshand", "builtin:demise"};
            a.change(makePlay(r));
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        gfx::beginCanvas();
        drawMenuBackground(now, 0xFF8A5C, menuPulse());
        gfx::gradientRect({0, 70, gfx::W, 330}, Color{10, 6, 20, 220}, Color{40, 18, 40, 220});
        const CharId ids[3] = {CharId::Principality, CharId::Hand, CharId::Demise};
        const double bt = lastBeatTime();
        for (int i = 0; i < 3; ++i) {
            CharacterState c;
            c.id = ids[i];
            c.beatAt = bt;
            const float x = 330.0f + i * 310.0f;
            drawCharacter(c, {x, 370}, i == 2 ? 0.62f : 0.68f, now, false);
            gfx::text(FontId::Ui, charDisplayName(ids[i]), x, 372, 24, charColor(ids[i]), Align::Center, 1.5f);
        }
        gfx::text(FontId::Title, "WEEK I", 60, 14, 40, kGold, Align::Left, 2.0f);
        gfx::text(FontId::Title, "THE ASCENSION", gfx::W - 60, 14, 40, kIvory, Align::Right, 2.0f);
        gfx::text(FontId::Ui, "TRACKS", 120, 440, 28, kGold);
        const char* tracks[3] = {"Principality", "Godshand", "Demise"};
        for (int i = 0; i < 3; ++i) gfx::text(FontId::Ui, tracks[i], 120, 478.0f + i * 40, 34, kIvory, Align::Left, 1.5f);
        const std::string d = diffs[clampv(a.storyDiff, 0, 2)];
        const Color dc = diffColor(d);
        const float bob = std::sin(float(now) * 4.0f) * 4.0f;
        gfx::drawNote(gfx::kLeft, {760 - bob, 500}, 54);
        gfx::drawNote(gfx::kRight, {1160 + bob, 500}, 54);
        gfx::glowText(FontId::Title, toLower(d) == "hard" ? "HARD" : (d == "Easy" ? "EASY" : "NORMAL"), 960, 470, 56, dc, dc);
        const ScoreEntry* best = a.scores.get(Scores::key("week:ascension", d));
        gfx::text(FontId::Ui, best ? ("WEEK SCORE: " + fmtThousands(best->score)) : std::string("WEEK SCORE: -"), 960, 560,
                  28, kIvory, Align::Center, 1.5f);
        hints("LEFT/RIGHT difficulty   ENTER begin the ascension   ESC back");
        gfx::endCanvas();
    }

private:
    bool go_ = false;
};

// --------------------------------------------------------------- Freeplay --

class FreeplayState : public State {
public:
    void enter() override {
        rebuild();
        anim_ = float(app().freeplaySel);
        selChangedAt_ = app().now;
        bg_ = currentColor();
    }
    bool wantsMenuMusic() const override { return !previewPlaying_; }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        if (!modsOpen_) t.keys = {{"MODIFIERS", KEY_TAB}, {"MOD PACKS", KEY_M}, {"CHART EDITOR", KEY_SEVEN}, {"RESCAN", KEY_R}};
        if (!modsOpen_ && platform::kAndroid) t.keys.insert(t.keys.begin() + 2, {"IMPORT ZIP", KEY_I});
        return t;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        int& sel = a.freeplaySel;
        const int n = int(list_.size());
        anim_ = approach(anim_, float(sel), 12.0f, float(dt));
        const Color target = currentColor();
        bg_ = gfx::mix(bg_, target, saturate(float(dt) * 4.0f));

        if (modsOpen_) {
            Modifiers& m = a.mods;
            if (in.up()) { modSel_ = (modSel_ + kModRows - 1) % kModRows; sfxPlay(Sfx::Scroll); }
            if (in.down()) { modSel_ = (modSel_ + 1) % kModRows; sfxPlay(Sfx::Scroll); }
            const int dir = in.left() ? -1 : (in.right() ? 1 : 0);
            if (dir || in.accept()) {
                sfxPlay(Sfx::Scroll);
                if (modSel_ == 0) m.rate = clampv(std::round((m.rate + 0.05 * (dir ? dir : 1)) * 20.0) / 20.0, 0.5, 2.0);
                if (modSel_ == 1) m.botplay = !m.botplay;
                if (modSel_ == 2) m.practice = !m.practice;
                if (modSel_ == 3) m.mirror = !m.mirror;
                if (modSel_ == 4) {
                    m.scrollType = (m.scrollType + (dir ? dir : 1) + 3) % 3;
                    m.scrollValue = m.scrollType == 2 ? 2.5 : 1.0;
                }
                if (modSel_ == 5 && m.scrollType != 0) {
                    const double step = m.scrollType == 1 ? 0.05 : 0.1;
                    m.scrollValue = std::round((m.scrollValue + step * (dir ? dir : 1)) / step) * step;
                    m.scrollValue = m.scrollType == 1 ? clampv(m.scrollValue, 0.25, 5.0) : clampv(m.scrollValue, 0.5, 10.0);
                }
            }
            if (in.back() || in.pressed(KEY_TAB)) modsOpen_ = false;
            return;
        }
        if (n > 0) {
            if (in.up()) { sel = (sel + n - 1) % n; changed(); }
            if (in.down()) { sel = (sel + 1) % n; changed(); }
            const int nd = int(list_[size_t(sel)]->diffs.size());
            if (nd > 0) {
                if (in.left()) { a.freeplayDiff = (diffIndex() + nd - 1) % nd; sfxPlay(Sfx::Scroll); }
                if (in.right()) { a.freeplayDiff = (diffIndex() + 1) % nd; sfxPlay(Sfx::Scroll); }
            }
        }
        if (in.pressed(KEY_TAB)) { modsOpen_ = true; sfxPlay(Sfx::Scroll); }
        // Android: mod zips picked in the system file browser are copied to mods/.
        if (platform::kAndroid && in.pressed(KEY_I) && !importing_) {
            importing_ = platform::pickModZips();
            note(importing_ ? "Pick one or more mod .zip files" : "The file picker is not available");
        }
        bool rescan = in.pressed(KEY_R);
        if (importing_) {
            int count = 0;
            const platform::Import st = platform::importStatus(&count);
            if (st == platform::Import::Done || st == platform::Import::Nothing) {
                importing_ = false;
                platform::resetImport();
                if (st == platform::Import::Done) {
                    rescan = true;
                    note(TextFormat("Imported %d mod zip(s)", count));
                } else {
                    note("Nothing imported (mods must be .zip files)");
                }
            } else if (st == platform::Import::Copying) {
                note("Copying...");
            }
        }
        if (rescan) {
            const std::string id = n > 0 ? list_[size_t(sel)]->id : "";
            a.library.scan(a.base, installProgress);
            rebuild();
            for (int i = 0; i < int(list_.size()); ++i)
                if (list_[size_t(i)]->id == id) sel = i;
            sfxPlay(Sfx::Confirm);
        }
        if (in.back()) { sfxPlay(Sfx::Cancel); a.change(makeMainMenu()); return; }
        if (in.pressed(KEY_M)) { sfxPlay(Sfx::Confirm); a.change(makeModPacks()); return; }
        if ((in.pressed(KEY_SEVEN) || in.pressed(KEY_KP_7)) && n > 0) {  // chart editor, as in Psych
            const SongEntry* e = list_[size_t(sel)];
            if (!e->diffs.empty()) {
                sfxPlay(Sfx::Confirm);
                a.change(makeChartEditor(e->id, e->diffs[size_t(diffIndex())]));
                return;
            }
        }
        if (in.accept() && n > 0) {
            const SongEntry* e = list_[size_t(sel)];
            if (!e->diffs.empty()) {
                sfxPlay(Sfx::Confirm);
                PlayRequest r;
                r.songId = e->id;
                r.diff = e->diffs[size_t(diffIndex())];
                a.change(makePlay(r));
                return;
            }
        }
        // song preview
        if (n > 0) {
            const SongEntry* e = list_[size_t(sel)];
            if (!job_.running() && previewId_ != e->id && a.now - selChangedAt_ > 0.45) {
                pendingId_ = e->id;
                const SongEntry copy = *e;
                job_.start([copy] { return app().library.preview(copy); });
            }
            if (job_.ready()) {
                PcmPtr pcm = job_.take();
                if (pcm && pendingId_ == e->id) {
                    a.stopMenuMusic();
                    a.mixer.setTracks(pcm, nullptr, nullptr);
                    const double len = double(pcm->frames()) / Mixer::kRate;
                    const double start = e->builtin ? len * 0.38 : len * 0.3;
                    a.mixer.setLoop(true, start, len);
                    a.mixer.setRate(1.0);
                    a.mixer.play(start);
                    previewPlaying_ = true;
                }
                previewId_ = pendingId_;
            }
        }
    }

    void draw() override {
        App& a = app();
        const double now = a.now;
        gfx::beginCanvas();
        const unsigned hex = (unsigned(bg_.r) << 16) | (unsigned(bg_.g) << 8) | bg_.b;
        drawMenuBackground(now, hex, previewPlaying_ ? 0.0f : menuPulse());
        const int n = int(list_.size());
        for (int i = std::max(0, int(anim_) - 7); i < std::min(n, int(anim_) + 8); ++i) {
            const float d = float(i) - anim_;
            const float y = 330 + d * 76;
            const float x = 150 + d * 22;
            const bool on = i == a.freeplaySel;
            const float alpha = std::max(0.0f, 1.0f - std::fabs(d) * 0.2f);
            const SongEntry* e = list_[size_t(i)];
            const CharId cid = e->builtin ? charFromName(e->opponent) : CharId::Shade;
            drawIcon(cid, gfx::rgb(e->color), {x - 50, y + 26}, on ? 70.0f : 56.0f, false, now);
            gfx::text(FontId::Ui, e->title, x, y, on ? 52.0f : 42.0f, fade(on ? WHITE : kIvory, alpha), Align::Left, 2.0f);
            if (on && !e->builtin)
                gfx::text(FontId::Ui, e->subtitle, x + 4, y + 50, 20, fade(kGold, 0.8f), Align::Left, 1.0f);
        }
        // info panel
        DrawRectangleRounded({830, 70, 410, 560}, 0.06f, 12, Color{8, 6, 16, 190});
        if (n > 0) {
            const SongEntry* e = list_[size_t(a.freeplaySel)];
            const CharId cid = e->builtin ? charFromName(e->opponent) : CharId::Shade;
            drawIcon(cid, gfx::rgb(e->color), {1035, 175}, 150, false, now);
            gfx::text(FontId::Title, e->title, 1035, 262, e->title.size() > 16 ? 26.0f : 34.0f, kGold, Align::Center, 2.0f);
            float y = 312;
            for (const std::string& l : wrap(e->builtin ? e->subtitle : ("From: " + e->subtitle), 36)) {
                gfx::text(FontId::Ui, l, 1035, y, 22, kIvory, Align::Center);
                y += 24;
            }
            if (!e->diffs.empty()) {
                const std::string d = e->diffs[size_t(diffIndex())];
                const Color dc = diffColor(d);
                const float bob = std::sin(float(now) * 4.0f) * 3.0f;
                gfx::drawNote(gfx::kLeft, {880 - bob, 430}, 40);
                gfx::drawNote(gfx::kRight, {1190 + bob, 430}, 40);
                gfx::text(FontId::Title, toLower(d) == "infernal" ? "INFERNAL" : d, 1035, 410, 34, dc, Align::Center, 2.0f);
                const ScoreEntry* best = a.scores.get(Scores::key(e->id, d));
                const std::string bs = best ? fmtThousands(best->score) + "  -  " + TextFormat("%.2f%%", best->acc * 100.0) +
                                                  "  -  " + best->rank
                                            : "No record yet";
                gfx::text(FontId::Ui, "BEST", 1035, 466, 20, fade(kGold, 0.8f), Align::Center);
                gfx::text(FontId::Ui, bs, 1035, 488, 26, WHITE, Align::Center, 1.5f);
                gfx::text(FontId::Ui, TextFormat("Difficulty %d / %d", diffIndex() + 1, int(e->diffs.size())), 1035, 524, 20,
                          fade(kIvory, 0.6f), Align::Center);
            }
            const Modifiers& m = a.mods;
            std::string ms = TextFormat("Rate %.2fx", m.rate);
            if (m.botplay) ms += "  Botplay";
            if (m.practice) ms += "  Practice";
            if (m.mirror) ms += "  Opponent";
            if (m.scrollType == 1) ms += TextFormat("  Scroll x%.2f", m.scrollValue);
            if (m.scrollType == 2) ms += TextFormat("  Scroll %.1f", m.scrollValue);
            gfx::text(FontId::Ui, ms, 1035, 586, 22,
                      (m.rate != 1.0 || m.botplay || m.practice || m.mirror || m.scrollType != 0) ? kGold : fade(kIvory, 0.5f), Align::Center);
        }
        if (a.library.customCount() == 0) {
            const std::string tip = a.touchMode
                ? "Tip: get mods in MOD PACKS, or copy them to " + platform::displayPath(a.base + "mods") + " and tap RESCAN"
                : std::string("Tip: drop FNF / Psych Engine songs into the 'songs' or 'mods' folder, then press R");
            gfx::text(FontId::Ui, tip, 40, 640, 20, fade(kIvory, 0.65f));
        }
        if (!note_.empty() && a.now - noteAt_ < 4.0)
            gfx::text(FontId::Ui, note_, 40, 600, 24, fade(kGold, float(std::min(1.0, 4.0 - (a.now - noteAt_)))), Align::Left, 1.5f);
        if (modsOpen_) drawMods();
        hints("ENTER play   LEFT/RIGHT difficulty   TAB modifiers   7 chart editor   M mod packs   R rescan   ESC back");
        gfx::endCanvas();
    }

private:
    void rebuild() {
        list_.clear();
        for (const SongEntry& e : app().library.songs()) list_.push_back(&e);
        app().freeplaySel = clampv(app().freeplaySel, 0, std::max(0, int(list_.size()) - 1));
    }
    void changed() {
        sfxPlay(Sfx::Scroll);
        selChangedAt_ = app().now;
    }
    void note(const std::string& s) {
        note_ = s;
        noteAt_ = app().now;
    }
    // Unpacking big zips takes a while: keep the screen alive.
    static void installProgress(const std::string& zip, double f) {
        BeginDrawing();
        ClearBackground(Color{12, 8, 20, 255});
        const char* t1 = TextFormat("Installing %s", zip.c_str());
        const int w = GetScreenWidth(), h = GetScreenHeight();
        DrawText(t1, (w - MeasureText(t1, 24)) / 2, h / 2 - 30, 24, RAYWHITE);
        DrawRectangle(w / 4, h / 2 + 10, w / 2, 10, Color{60, 50, 80, 255});
        DrawRectangle(w / 4, h / 2 + 10, int(w / 2 * f), 10, Color{200, 160, 255, 255});
        EndDrawing();
        PollInputEvents();
        SwapScreenBuffer();
    }
    int diffIndex() const {
        if (list_.empty()) return 0;
        const int nd = int(list_[size_t(app().freeplaySel)]->diffs.size());
        return nd > 0 ? clampv(app().freeplayDiff, 0, nd - 1) : 0;
    }
    Color currentColor() const {
        if (list_.empty()) return kGold;
        return gfx::rgb(list_[size_t(app().freeplaySel)]->color);
    }
    void drawMods() {
        const Modifiers& m = app().mods;
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 150});
        DrawRectangleRounded({370, 140, 540, 430}, 0.08f, 12, Color{16, 10, 30, 245});
        gfx::text(FontId::Title, "MODIFIERS", 640, 158, 38, kGold, Align::Center, 2.0f);
        static const char* scrollTypes[3] = {"the chart's", "multiplied", "constant"};
        const std::string rows[kModRows] = {
            TextFormat("Playback rate   %.2fx", m.rate), std::string("Botplay   ") + (m.botplay ? "ON" : "OFF"),
            std::string("Practice (no death)   ") + (m.practice ? "ON" : "OFF"),
            std::string("Play as the opponent   ") + (m.mirror ? "ON" : "OFF"), std::string("Scroll speed   ") + scrollTypes[m.scrollType],
            m.scrollType == 0 ? std::string("Scroll value   -")
                              : std::string(TextFormat(m.scrollType == 1 ? "Scroll value   x%.2f" : "Scroll value   %.1f", m.scrollValue))};
        for (int i = 0; i < kModRows; ++i)
            gfx::text(FontId::Ui, rows[i], 640, 222.0f + i * 50, 30, i == modSel_ ? kGold : (i == 5 && m.scrollType == 0 ? fade(kIvory, 0.4f) : kIvory),
                      Align::Center, 1.5f);
        gfx::text(FontId::Ui, "Scores are only saved at 1.00x or faster, without botplay or practice", 640, 532, 18,
                  fade(kIvory, 0.6f), Align::Center);
    }

    std::vector<const SongEntry*> list_;
    float anim_ = 0;
    Color bg_ = kGold;
    double selChangedAt_ = 0;
    PcmJob job_;
    std::string pendingId_, previewId_;
    bool previewPlaying_ = false;
    bool modsOpen_ = false;
    static constexpr int kModRows = 6;
    int modSel_ = 0;
    bool importing_ = false;
    std::string note_;
    double noteAt_ = 0;
};

// ---------------------------------------------------------------- Credits --

class CreditsState : public State {
public:
    void enter() override { t0_ = app().now; }
    void update(double) override {
        if (app().input.back() || app().input.accept()) {
            sfxPlay(Sfx::Cancel);
            app().change(makeMainMenu());
        }
    }
    void draw() override {
        static const char* lines[] = {
            "#GOD'S HAND",
            "of Principality and Demise",
            "",
            "@A RHYTHM DUEL",
            "Created for deatuamae67-crypto",
            "",
            "@ENGINE, MUSIC, ART AND CHARTS",
            "Written from scratch in C++20",
            "Every song is synthesized and every character is drawn in code",
            "Built with Claude Code",
            "",
            "@LIBRARIES",
            "raylib 5.5 by Ramon Santamaria (zlib license)",
            "",
            "@FONTS",
            "Cinzel Decorative by Natanael Gama (SIL Open Font License)",
            "Rajdhani by Indian Type Foundry (SIL Open Font License)",
            "",
            "@THE CAST",
            "The Herald - a mortal voice",
            "The Principality - keeper of the first gate",
            "The Hand - author of the world",
            "Demise - the end of all principalities",
            "",
            "Thank you for playing.",
        };
        const double now = app().now;
        gfx::beginCanvas();
        drawMenuBackground(now, 0x6C8CFF, menuPulse());
        float y = 640 - float(now - t0_) * 42.0f;
        const float total = float(sizeof(lines) / sizeof(lines[0])) * 46 + 700;
        while (y < -total + 640) y += total;
        for (const char* l : lines) {
            if (l[0] == '#') gfx::glowText(FontId::Title, l + 1, gfx::W / 2, y, 64, kGold, Color{255, 180, 80, 255});
            else if (l[0] == '@') gfx::text(FontId::Title, l + 1, gfx::W / 2, y + 10, 30, kGold, Align::Center, 2.0f);
            else gfx::text(FontId::Ui, l, gfx::W / 2, y + 10, 28, kIvory, Align::Center, 1.5f);
            y += l[0] == '#' ? 80 : 46;
        }
        hints("ESC back");
        gfx::endCanvas();
    }

private:
    double t0_ = 0;
};

std::unique_ptr<State> makeTitle() { return std::make_unique<TitleState>(); }
std::unique_ptr<State> makeMainMenu() { return std::make_unique<MainMenuState>(); }
std::unique_ptr<State> makeStory() { return std::make_unique<StoryState>(); }
std::unique_ptr<State> makeFreeplay() { return std::make_unique<FreeplayState>(); }
std::unique_ptr<State> makeCredits() { return std::make_unique<CreditsState>(); }

}  // namespace gh
