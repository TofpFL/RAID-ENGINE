// The duel itself.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <future>

#include "app.hpp"
#include "characters.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "raylib.h"
#include "stage.hpp"

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {
const Color kGold{255, 214, 120, 255};
const Color kIvory{246, 236, 214, 255};
const Color kMissColor{255, 70, 80, 255};

Color ratingColor(Rating r) {
    switch (r) {
        case kDivine: return Color{255, 215, 100, 255};
        case kGrace: return Color{120, 220, 255, 255};
        case kMortal: return Color{205, 205, 220, 255};
        default: return Color{205, 120, 150, 255};
    }
}

int singDir(int keys, int lane) {
    const int d = gfx::laneDir(keys, lane);
    return d == gfx::kCenter ? 2 : d;
}
}  // namespace

class PlayState : public State {
public:
    explicit PlayState(PlayRequest r) : req_(std::move(r)) {}
    bool wantsMenuMusic() const override { return false; }

    void enter() override {
        App& a = app();
        a.stopMenuMusic();
        a.mixer.clear();
        entry_ = a.library.find(req_.songId);
        if (!entry_) {
            phase_ = Phase::Error;
            error_ = "Song not found: " + req_.songId;
            return;
        }
        rate_ = req_.story ? 1.0 : a.mods.rate;
        const bool mirror = !req_.story && a.mods.mirror;
        const SongEntry copy = *entry_;
        const std::string diff = req_.diff;
        job_ = std::async(std::launch::async, [copy, diff, mirror] { return app().library.prepare(copy, diff, mirror); });
        loadStart_ = a.now;
    }

    void update(double dt) override {
        App& a = app();
        const double now = a.now;
        if (phase_ == Phase::Loading) {
            if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                prep_ = job_.get();
                if (!prep_.ok) {
                    phase_ = Phase::Error;
                    error_ = prep_.error;
                } else {
                    setup();
                }
            }
            return;
        }
        if (phase_ == Phase::Error) {
            if (a.input.accept() || a.input.back()) exitToMenu();
            return;
        }
        stage_.update(float(dt), now);
        fx_.update(float(dt));
        if (phase_ == Phase::Paused) { updatePause(); return; }
        if (phase_ == Phase::Dead) { updateDead(); return; }

        songMs_ = a.mixer.time(now) * 1000.0 - a.settings.offsetMs * rate_;
        handleInput();
        if (phase_ != Phase::Playing) return;
        session_.update(songMs_);
        handleEvents();

        const int beat = int(std::floor(chart_.beatAt(songMs_)));
        if (beat != lastBeat_) {
            for (int b = std::max(lastBeat_ + 1, beat - 3); b <= beat; ++b) onBeat(b);
            lastBeat_ = beat;
        }
        for (int side = 0; side < 2; ++side)
            for (int l = 0; l < chart_.keys; ++l)
                if (session_.isHolding(side, l)) {
                    confirm_[side][l] = std::max(confirm_[side][l], 0.7f);
                    CharacterState& c = charOf(side);
                    c.singUntil = std::max(c.singUntil, now + 0.12);
                }
        const int focus = chart_.focusAt(songMs_);
        const bool focusLeft = (focus == 1) == playerLeft_;
        camFocus_.x = approach(camFocus_.x, focusLeft ? 540.0f : 740.0f, 2.4f, float(dt));
        camFocus_.y = approach(camFocus_.y, 370.0f, 2.4f, float(dt));
        camZoom_ = approach(camZoom_, 1.0f, 4.0f, float(dt));
        hudZoom_ = approach(hudZoom_, 1.0f, 5.0f, float(dt));
        beatPulse_ = std::max(0.0f, beatPulse_ - float(dt) * 4.0f);
        minePulse_ = std::max(0.0f, minePulse_ - float(dt) * 3.0f);
        for (auto& side : confirm_)
            for (float& c : side) c = std::max(0.0f, c - float(dt) * 5.0f);
        healthShown_ = approach(healthShown_, float(session_.health), 10.0f, float(dt));

        if (session_.dead) {
            phase_ = Phase::Dead;
            deadAt_ = now;
            deadSel_ = 0;
            a.mixer.fadeTo(0.0f, 0.5f);
            sfxPlay(Sfx::Death);
            charOf(1).miss(1, now);
            charOf(1).dead = true;
            return;
        }
        const double audioEnd = prep_.inst ? a.mixer.lengthSec() * 1000.0 : 0.0;
        const double endAt = std::max(chart_.lengthMs + 800.0 * rate_, std::min(audioEnd, chart_.lengthMs + 8000.0 * rate_));
        if ((songMs_ >= endAt && session_.allResolved()) || songMs_ > chart_.lengthMs + 20000.0) finish();
    }

    void draw() override {
        App& a = app();
        const double now = a.now;
        if (phase_ == Phase::Loading || phase_ == Phase::Error) {
            drawLoading();
            return;
        }
        gfx::beginCanvas(0.94f * camZoom_, camFocus_);
        stage_.drawBack(now);
        const bool demise = left_.id == CharId::Demise;
        drawCharacter(left_, {330, 600}, demise ? 0.95f : 1.0f, now, false);
        drawCharacter(right_, {950, 612}, 1.0f, now, true);
        stage_.drawFront(now);
        gfx::endCanvas();

        gfx::beginCanvas();
        stage_.drawOverlay();
        if (minePulse_ > 0) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(Color{200, 0, 30, 255}, minePulse_ * 0.35f));
        gfx::endCanvas();

        gfx::beginCanvas(hudZoom_);
        drawLanes();
        fx_.draw(true);
        drawHud();
        gfx::endCanvas();

        gfx::beginCanvas();
        if (phase_ == Phase::Paused) drawPause();
        if (phase_ == Phase::Dead) drawDead();
        gfx::endCanvas();
    }

private:
    enum class Phase { Loading, Playing, Paused, Dead, Error };

    CharacterState& charOf(int side) { return (side == 1) == playerLeft_ ? left_ : right_; }

    void setup() {
        App& a = app();
        chart_ = std::move(prep_.chart);
        cfg_.ghostTapping = a.settings.ghostTapping;
        cfg_.botplay = !req_.story && a.mods.botplay;
        cfg_.practice = !req_.story && a.mods.practice;
        cfg_.rate = rate_;
        playerLeft_ = !req_.story && a.mods.mirror;
        const CharId opp = entry_->builtin ? charFromName(entry_->opponent) : CharId::Shade;
        left_ = CharacterState{};
        left_.id = opp;
        left_.tint = gfx::rgb(entry_->color);
        right_ = CharacterState{};
        right_.id = CharId::Herald;
        leftColor_ = opp == CharId::Shade ? gfx::rgb(entry_->color) : charColor(opp);
        rightColor_ = charColor(CharId::Herald);
        stage_.init(entry_->builtin ? entry_->def->stage : 3, gfx::rgb(entry_->color), a.settings.lowDetail, a.settings.flashing);
        keyLane_.assign(Input::kMaxCodes, -1);
        const auto lk = a.settings.laneKeys(chart_.keys);
        for (int l = 0; l < int(lk.size()); ++l)
            for (int code : lk[size_t(l)])
                if (code > 0 && code < Input::kMaxCodes) keyLane_[size_t(code)] = l;
        a.mixer.setTracks(prep_.inst, prep_.voiceP, prep_.voiceO);
        a.mixer.setRate(rate_);
        restart();
    }

    void restart() {
        App& a = app();
        session_.init(chart_, cfg_);
        popups_.clear();
        fx_.clear();
        for (auto& s : confirm_)
            for (float& c : s) c = 0;
        for (int& h : heldCount_) h = 0;
        leadMs_ = 60000.0 / chart_.bpmAt(0) * 5.0;
        lastBeat_ = int(std::floor(chart_.beatAt(-leadMs_))) - 1;
        for (int t = 0; t < Mixer::kTrackCount; ++t) a.mixer.setTrackVolume(t, 1.0f);
        a.mixer.fadeTo(1.0f, 0.0f);
        a.mixer.play(-leadMs_ / 1000.0);
        songMs_ = -leadMs_;
        healthShown_ = 1.0f;
        camFocus_ = {640, 370};
        camZoom_ = hudZoom_ = 1.0f;
        left_.singUntil = right_.singUntil = -10;
        left_.dead = right_.dead = false;
        finished_ = false;
        phase_ = Phase::Playing;
        a.input.clear();
    }

    void handleInput() {
        App& a = app();
        for (const InputEvent& e : a.input.events()) {
            const bool pauseKey = e.code == KEY_ESCAPE || e.code == kPadBase + GAMEPAD_BUTTON_MIDDLE_RIGHT ||
                                  (e.code == KEY_ENTER && !a.input.isDown(KEY_LEFT_ALT) && !a.input.isDown(KEY_RIGHT_ALT));
            if (e.down && pauseKey && keyLane_[size_t(e.code)] < 0) {
                pause();
                return;
            }
            if (e.code <= 0 || e.code >= Input::kMaxCodes) continue;
            const int lane = keyLane_[size_t(e.code)];
            if (lane < 0) continue;
            const double t = std::min(songMs_, songMs_ - (a.now - e.t) * 1000.0 * rate_);
            if (e.down) {
                if (heldCount_[lane]++ == 0) session_.press(lane, t);
            } else if (heldCount_[lane] > 0 && --heldCount_[lane] == 0) {
                session_.release(lane, t);
            }
        }
    }

    void popup(const std::string& text, Color c, const std::string& timing) {
        const double now = app().now;
        if (!popups_.empty() && now - popups_.back().t0 < 0.03) {  // spam: update instead of stacking
            Popup& p = popups_.back();
            if (p.text == "MISS" && text != "MISS") return;
            p.text = text;
            p.timing = timing;
            p.c = c;
            p.combo = session_.combo;
            return;
        }
        if (popups_.size() >= 4) popups_.erase(popups_.begin());
        popups_.push_back({text, timing, c, now, session_.combo});
    }

    void splash(int side, int lane) {
        const Color c = gfx::dirColor(gfx::laneDir(chart_.keys, lane));
        const Vector2 p{laneX(side, lane), strumY()};
        gfx::Particle ring{};
        ring.p = p;
        ring.kind = 2;
        ring.c = c;
        ring.size = noteSize() * 0.75f;
        ring.life = ring.maxLife = 0.3f;
        fx_.add(ring);
        for (int i = 0; i < 8; ++i) {
            gfx::Particle s{};
            const float ang = gfx::randf(0, 6.2831853f), sp = gfx::randf(260, 520);
            s.p = p;
            s.v = {std::cos(ang) * sp, std::sin(ang) * sp};
            s.kind = 1;
            s.c = gfx::brighten(c, 0.3f);
            s.size = 3.5f;
            s.life = s.maxLife = gfx::randf(0.2f, 0.35f);
            fx_.add(s);
        }
    }

    void handleEvents() {
        App& a = app();
        const double now = a.now;
        for (const GameEvent& ev : session_.events) {
            const int dir = singDir(chart_.keys, ev.lane);
            switch (ev.type) {
                case GameEvent::PlayerHit: {
                    confirm_[1][ev.lane] = 1.0f;
                    if (!(ev.flags & kFlagNoAnim)) charOf(1).sing(dir, now, ev.length / 1000.0 / rate_);
                    a.mixer.setTrackVolume(Mixer::kVoicePlayer, 1.0f);
                    const RatingInfo& ri = ratingInfo(ev.rating);
                    popup(ri.name, ratingColor(ev.rating),
                          a.settings.showTimings && !cfg_.botplay ? TextFormat("%+.0f ms", ev.errMs) : "");
                    if (ev.rating == kDivine && a.settings.noteSplashes && now - lastSplash_[ev.lane] > 0.06) {
                        splash(1, ev.lane);
                        lastSplash_[ev.lane] = now;
                    }
                    if (a.settings.hitsoundVol > 0 && now - lastHitsound_ > 0.02) {
                        sfxPlay(Sfx::Hit, a.settings.hitsoundVol / std::max(0.05f, a.settings.sfxVol));
                        lastHitsound_ = now;
                    }
                    break;
                }
                case GameEvent::PlayerMiss:
                case GameEvent::HoldDrop:
                case GameEvent::GhostMiss:
                    charOf(1).miss(dir, now);
                    a.mixer.setTrackVolume(Mixer::kVoicePlayer, 0.0f);
                    if (now - lastMissSfx_ > 0.05) {
                        sfxPlay(Sfx::Miss, 0.55f, gfx::randf(0.9f, 1.1f));
                        lastMissSfx_ = now;
                    }
                    popup("MISS", kMissColor, "");
                    break;
                case GameEvent::PlayerMine:
                    charOf(1).miss(dir, now);
                    sfxPlay(Sfx::Mine);
                    minePulse_ = 1.0f;
                    popup("CURSED", kMissColor, "");
                    break;
                case GameEvent::OpponentHit:
                    confirm_[0][ev.lane] = 1.0f;
                    if (!(ev.flags & kFlagNoAnim)) charOf(0).sing(dir, now, ev.length / 1000.0 / rate_);
                    break;
                default: break;
            }
        }
        session_.events.clear();
    }

    void onBeat(int b) {
        App& a = app();
        if (b >= -4 && b <= -1) sfxPlay(Sfx(int(Sfx::Count3) + (b + 4)));
        left_.beat(a.now);
        right_.beat(a.now);
        stage_.onBeat(b);
        beatPulse_ = 1.0f;
        if (b >= 0 && b % 4 == 0 && a.settings.camZoom) {
            camZoom_ += 0.015f;
            hudZoom_ += 0.03f;
        }
    }

    void pause() {
        App& a = app();
        phase_ = Phase::Paused;
        pauseSel_ = 0;
        a.mixer.pause();
        sfxPlay(Sfx::Scroll);
    }

    void resume() {
        App& a = app();
        a.mixer.resume();
        phase_ = Phase::Playing;
        a.input.clear();
        const auto lk = a.settings.laneKeys(chart_.keys);
        for (int l = 0; l < chart_.keys; ++l) {
            if (heldCount_[l] <= 0) continue;
            bool down = false;
            for (int code : lk[size_t(l)]) down |= a.input.isDown(code);
            if (!down) {
                heldCount_[l] = 0;
                session_.release(l, songMs_);
            }
        }
    }

    void updatePause() {
        App& a = app();
        if (a.input.up()) { pauseSel_ = (pauseSel_ + 2) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.down()) { pauseSel_ = (pauseSel_ + 1) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.back()) { resume(); return; }
        if (a.input.accept()) {
            sfxPlay(Sfx::Confirm);
            if (pauseSel_ == 0) resume();
            else if (pauseSel_ == 1) restart();
            else exitToMenu();
        }
    }

    void updateDead() {
        App& a = app();
        if (a.now - deadAt_ < 0.8) return;
        if (a.input.accept()) { sfxPlay(Sfx::Confirm); restart(); }
        else if (a.input.back()) exitToMenu();
    }

    void exitToMenu() {
        App& a = app();
        a.mixer.stop();
        a.mixer.clear();
        a.change(req_.story ? makeStory() : makeFreeplay());
    }

    void finish() {
        if (finished_) return;
        finished_ = true;
        App& a = app();
        ResultData r;
        r.req = req_;
        r.title = chart_.title.empty() ? entry_->title : chart_.title;
        r.diff = req_.diff;
        r.score = session_.score;
        r.acc = session_.accuracy();
        r.misses = session_.misses;
        r.maxCombo = session_.maxCombo;
        r.counts = session_.counts;
        r.rank = session_.rank();
        r.clear = session_.clearLabel();
        r.errors = session_.hitErrors;
        r.lengthMs = std::max(1.0, chart_.lengthMs);
        r.botplay = cfg_.botplay;
        r.practice = cfg_.practice;
        r.rate = rate_;
        r.color = entry_->color;
        const bool saveable = !cfg_.botplay && !cfg_.practice && rate_ >= 1.0;
        if (saveable) {
            ScoreEntry e{r.score, r.acc, r.misses, r.rank, r.clear};
            r.newBest = a.scores.submit(Scores::key(entry_->id + (playerLeft_ ? "#opponent" : ""), req_.diff), e);
        }
        if (req_.story) {
            r.req.storyScore += r.score;
            r.req.storyMisses += r.misses;
            r.req.storyAccSum += r.acc;
            r.req.storySongs += 1;
            if (r.req.storyQueue.empty()) {
                ScoreEntry w{r.req.storyScore, r.req.storyAccSum / r.req.storySongs, r.req.storyMisses, "", ""};
                a.scores.submit(Scores::key("week:ascension", req_.diff), w);
            }
        }
        if (a.autoRun.enabled)
            std::printf("[auto] %s (%s): score %lld, accuracy %.2f%%, misses %d, max combo %d, notes %zu\n", r.title.c_str(),
                        r.diff.c_str(), r.score, r.acc * 100.0, r.misses, r.maxCombo, session_.totalPlayerNotes);
        a.change(makeResults(r));
    }

    // ------------------------------------------------------------ layout --
    float laneW() const {
        const int k = chart_.keys;
        return std::min(112.0f, 500.0f / float(k));
    }
    float noteSize() const { return laneW() * 0.94f; }
    float strumY() const { return app().settings.downscroll ? gfx::H - 112.0f : 112.0f; }
    float laneX(int side, int lane) const {
        const int k = chart_.keys;
        const float w = laneW();
        if (app().settings.middlescroll) {
            if (side == 1) return 640.0f + (float(lane) - (k - 1) * 0.5f) * w;
            const int half = (k + 1) / 2;
            if (lane < half) return 90.0f + lane * w * 0.8f;
            return gfx::W - 90.0f - float(k - 1 - lane) * w * 0.8f;
        }
        const bool left = (side == 1) == playerLeft_;
        return (left ? 320.0f : 960.0f) + (float(lane) - (k - 1) * 0.5f) * w;
    }

    void drawMine(Vector2 c, float size, float alpha) {
        const float t = float(app().now);
        gfx::glow(c, size * 0.7f, fade(Color{255, 30, 60, 255}, 0.5f * alpha));
        DrawPoly(c, 8, size * 0.36f, t * 90.0f, fade(Color{150, 20, 40, 255}, alpha));
        DrawCircleV(c, size * 0.27f, fade(Color{30, 6, 12, 255}, alpha));
        DrawRing(c, size * 0.24f, size * 0.29f, 0, 360, 32, fade(Color{255, 60, 80, 255}, alpha));
        const float r = size * 0.12f;
        DrawLineEx({c.x - r, c.y - r}, {c.x + r, c.y + r}, 3, fade(Color{255, 90, 100, 255}, alpha));
        DrawLineEx({c.x - r, c.y + r}, {c.x + r, c.y - r}, 3, fade(Color{255, 90, 100, 255}, alpha));
    }

    void drawLanes() {
        App& a = app();
        const Settings& s = a.settings;
        const int K = chart_.keys;
        const float ns = noteSize();
        const float sy = strumY();
        const float dirSign = s.downscroll ? -1.0f : 1.0f;
        const double speed = s.scrollSpeed > 0.001f ? s.scrollSpeed : chart_.speed;
        const double pxPerMs = 0.45 * speed / rate_;
        const double ahead = (gfx::H + ns) / pxPerMs, behind = (ns + 40.0) / pxPerMs;
        for (int side = 0; side < 2; ++side) {
            const bool mid = s.middlescroll && side == 0;
            const float alphaSide = mid ? 0.35f : 1.0f;
            const float sc = mid ? 0.8f : 1.0f;
            for (int lane = 0; lane < K; ++lane) {
                const float x = laneX(side, lane);
                const int dir = gfx::laneDir(K, lane);
                const Color col = gfx::dirColor(dir);
                float conf = confirm_[side][lane];
                if (side == 0 && !s.opponentStrums) conf = 0;
                const bool pressed = side == 1 && heldCount_[lane] > 0 && !cfg_.botplay;
                if (conf > 0.01f) {
                    gfx::glow({x, sy}, ns * 0.95f * conf, fade(col, 0.6f * alphaSide));
                    gfx::drawNote(dir, {x, sy}, ns * sc, alphaSide, 1.0f + 0.06f * conf);
                } else if (pressed) {
                    gfx::drawReceptor(dir, {x, sy}, ns * sc, gfx::mix(WHITE, col, 0.6f), 0.9f);
                } else {
                    gfx::drawReceptor(dir, {x, sy}, ns * sc, fade(WHITE, alphaSide));
                }
                const std::vector<LaneNote>& v = session_.lane(side, lane);
                if (v.empty()) continue;
                const double tStart = songMs_ - behind - session_.maxSustain(side, lane);
                const double tEnd = songMs_ + ahead;
                auto lo = std::lower_bound(v.begin(), v.end(), tStart, [](const LaneNote& n, double t) { return n.t < t; });
                auto hi = std::upper_bound(lo, v.end(), tEnd, [](double t, const LaneNote& n) { return t < n.t; });
                for (auto it = lo; it != hi; ++it) {
                    const LaneNote& n = *it;
                    if (n.len <= 0) continue;
                    if (n.state == kHeldDone || n.state == kHit || n.state == kMineHit || n.state == kMinePassed) continue;
                    const double endT = n.t + n.len;
                    if (endT < songMs_ - behind) continue;
                    const bool active = n.state == kHolding;
                    const double startT = active ? std::max(n.t, songMs_) : n.t;
                    const float y0 = active ? sy : sy + dirSign * float((startT - songMs_) * pxPerMs);
                    const float y1 = sy + dirSign * float((endT - songMs_) * pxPerMs);
                    const float al = (n.state == kMissed || n.state == kDropped ? 0.35f : 1.0f) * alphaSide;
                    gfx::drawSustain(dir, x, y0, y1, ns * 0.3f * sc, al, true);
                }
                // Level of detail: notes closer than ~15% of a note overlap into a
                // solid wall anyway, so only one of them is drawn.
                const float minGap = std::max(2.0f, ns * 0.15f);
                float lastY = 1e9f;
                for (auto it = hi; it != lo;) {
                    --it;
                    const LaneNote& n = *it;
                    if (n.state != kPending && n.state != kMissed) continue;
                    if (n.state == kMissed && n.t < songMs_ - behind) continue;
                    const float y = sy + dirSign * float((n.t - songMs_) * pxPerMs);
                    if (std::fabs(y - lastY) < minGap && n.len <= 0) continue;
                    lastY = y;
                    const float al = (n.state == kMissed ? 0.35f : 1.0f) * alphaSide;
                    if (n.kind == kMine) drawMine({x, y}, ns * sc, al);
                    else gfx::drawNote(dir, {x, y}, ns * sc, al);
                }
            }
        }
    }

    void drawHud() {
        App& a = app();
        const Settings& s = a.settings;
        const double now = a.now;
        // health bar
        const float by = s.downscroll ? 82.0f : 640.0f, bx = 340, bw = 600, bh = 18;
        const float hp = healthShown_ / 2.0f;
        const float leftFrac = playerLeft_ ? hp : 1.0f - hp;
        const float split = bx + bw * leftFrac;
        DrawRectangleRounded({bx - 5, by - 5, bw + 10, bh + 10}, 0.6f, 8, Color{8, 4, 14, 230});
        DrawRectangleRec({bx, by, split - bx, bh}, leftColor_);
        DrawRectangleRec({split, by, bx + bw - split, bh}, rightColor_);
        DrawRectangleRec({bx, by, bw, bh * 0.35f}, Color{255, 255, 255, 40});
        const bool playerLosing = session_.health < 0.4, oppLosing = session_.health > 1.6;
        const float isz = 86.0f * (1.0f + 0.12f * beatPulse_);
        drawIcon(left_.id, left_.tint, {split - 36, by + bh * 0.5f}, isz, playerLeft_ ? playerLosing : oppLosing, now);
        drawIcon(right_.id, right_.tint, {split + 36, by + bh * 0.5f}, isz, playerLeft_ ? oppLosing : playerLosing, now);
        // score line
        std::string line = "Score " + fmtThousands(session_.score) + "   |   Misses " + std::to_string(session_.misses) +
                           "   |   " + TextFormat("%.2f%%", session_.accuracy() * 100.0) + " (" + session_.rank() + ")";
        const std::string cl = session_.clearLabel();
        if (!cl.empty()) line += "   |   " + cl;
        gfx::text(FontId::Ui, line, gfx::W / 2, by + 36, 24, WHITE, Align::Center, 2.0f);
        if (cfg_.botplay)
            gfx::text(FontId::Title, "BOTPLAY", gfx::W / 2, s.downscroll ? 560 : 150, 40,
                      fade(kGold, 0.55f + 0.35f * std::sin(float(now) * 3.0f)), Align::Center, 2.0f);
        if (cfg_.practice) gfx::text(FontId::Ui, "PRACTICE MODE", gfx::W / 2, by + 62, 20, fade(kGold, 0.8f), Align::Center, 1.5f);
        if (rate_ != 1.0) gfx::text(FontId::Ui, TextFormat("%.2fx", rate_), bx + bw + 60, by - 4, 24, kGold, Align::Left, 1.5f);

        // song progress
        const float ty = s.downscroll ? gfx::H - 26.0f : 12.0f;
        const float prog = float(clampv(songMs_ / std::max(1.0, chart_.lengthMs), 0.0, 1.0));
        DrawRectangleRounded({440, ty, 400, 14}, 0.8f, 8, Color{8, 4, 14, 200});
        DrawRectangleRounded({442, ty + 2, 396 * prog, 10}, 0.8f, 8, gfx::mix(leftColor_, WHITE, 0.3f));
        const double remain = std::max(0.0, (chart_.lengthMs - songMs_) / 1000.0 / rate_);
        gfx::text(FontId::Ui, chart_.title + "  -  " + req_.diff + "  -  " + fmtTime(remain), gfx::W / 2,
                  s.downscroll ? ty - 26 : ty + 14, 20, fade(WHITE, 0.85f), Align::Center, 1.5f);

        // rating popups
        const float px = s.middlescroll ? 1030.0f : gfx::W / 2;
        const float py = s.downscroll ? 400.0f : 300.0f;
        if (!popups_.empty()) {
            const Popup& p = popups_.back();
            const float age = float(now - p.t0);
            if (age <= 0.7f) {
            const float pop = 1.0f + 0.25f * std::exp(-age * 20.0f);
            const float al = age < 0.4f ? 1.0f : 1.0f - (age - 0.4f) / 0.3f;
            const float y = py - age * 26.0f;
            gfx::text(FontId::Title, p.text, px, y, 46 * pop, fade(p.c, al), Align::Center, 2.5f);
            if (p.combo >= 3 && p.text != "MISS")
                gfx::text(FontId::Ui, "x" + std::to_string(p.combo), px, y + 52, 34 * pop, fade(WHITE, al), Align::Center, 2.0f);
            if (!p.timing.empty()) gfx::text(FontId::Ui, p.timing, px, y - 26, 22, fade(kIvory, al * 0.9f), Align::Center, 1.5f);
            }
        }
        // countdown
        if (songMs_ < 0) {
            const double bf = chart_.beatAt(songMs_);
            const int b = int(std::floor(bf));
            const float frac = float(bf - b);
            static const char* words[4] = {"III", "II", "I", "ASCEND!"};
            if (b >= -4 && b <= -1) {
                const float sc = 1.0f + 0.35f * std::pow(1.0f - frac, 3.0f);
                gfx::glowText(FontId::Title, words[b + 4], gfx::W / 2, 300 - 40 * sc, 96 * sc, fade(kGold, 1.0f - frac),
                              Color{255, 170, 60, 255});
            }
        }
    }

    void drawPause() {
        App& a = app();
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{6, 4, 12, 190});
        gfx::glowText(FontId::Title, "PAUSED", gfx::W / 2, 140, 80, kGold, Color{255, 170, 60, 255});
        gfx::text(FontId::Ui, chart_.title + "  -  " + req_.diff, gfx::W / 2, 250, 30, kIvory, Align::Center, 1.5f);
        static const char* items[3] = {"Resume", "Restart", "Exit to menu"};
        for (int i = 0; i < 3; ++i) {
            const bool on = i == pauseSel_;
            gfx::text(FontId::Title, items[i], gfx::W / 2, 340.0f + i * 70, on ? 48.0f : 40.0f, on ? kGold : fade(kIvory, 0.6f),
                      Align::Center, 2.0f);
        }
        (void)a;
    }

    void drawDead() {
        App& a = app();
        const float t = float(a.now - deadAt_);
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(Color{20, 0, 6, 255}, std::min(0.85f, t * 1.2f)));
        const float al = saturate(t * 1.5f - 0.3f);
        gfx::glowText(FontId::Title, "YOU HAVE FALLEN", gfx::W / 2, 220, 72, fade(Color{255, 80, 90, 255}, al),
                      Color{255, 40, 60, 255});
        gfx::text(FontId::Ui, "ENTER rise again      ESC leave", gfx::W / 2, 360, 34, fade(kIvory, al), Align::Center, 2.0f);
    }

    void drawLoading() {
        App& a = app();
        const double now = a.now;
        gfx::beginCanvas();
        drawMenuBackground(now, entry_ ? entry_->color : 0x8C6CFF, 0);
        if (phase_ == Phase::Error) {
            gfx::text(FontId::Title, "THE HEAVENS ARE SILENT", gfx::W / 2, 230, 50, kMissColor, Align::Center, 2.0f);
            gfx::text(FontId::Ui, error_, gfx::W / 2, 330, 28, kIvory, Align::Center, 1.5f);
            gfx::text(FontId::Ui, "ENTER return", gfx::W / 2, 420, 26, fade(kIvory, 0.7f), Align::Center);
        } else {
            for (int i = 0; i < 4; ++i) {
                const float ang = float(now) * 2.5f + i * 1.5707963f;
                gfx::drawNote(i, {640 + std::cos(ang) * 70, 300 + std::sin(ang) * 70}, 56);
            }
            const std::string title = entry_ ? entry_->title : "";
            gfx::text(FontId::Title, "Summoning " + title + "...", gfx::W / 2, 420, 40, kGold, Align::Center, 2.0f);
            static const char* tips[] = {"Hold the note until its trail ends.", "Calibrate your audio offset in Options.",
                                         "Botplay and playback rate live under TAB in Freeplay.",
                                         "Your own FNF / Psych charts go in the songs or mods folder."};
            const int ti = int((now - loadStart_) / 3.0) % 4;
            gfx::text(FontId::Ui, tips[ti], gfx::W / 2, 480, 26, fade(kIvory, 0.8f), Align::Center);
        }
        gfx::endCanvas();
    }

    struct Popup {
        std::string text, timing;
        Color c;
        double t0;
        int combo;
    };

    PlayRequest req_;
    const SongEntry* entry_ = nullptr;
    Phase phase_ = Phase::Loading;
    std::future<Prepared> job_;
    Prepared prep_;
    std::string error_;
    Chart chart_;
    Session session_;
    SessionConfig cfg_;
    Stage stage_;
    CharacterState left_, right_;
    Color leftColor_{}, rightColor_{};
    bool playerLeft_ = false;
    double songMs_ = -1e9, leadMs_ = 0, rate_ = 1.0;
    int lastBeat_ = -100000;
    float camZoom_ = 1, hudZoom_ = 1, beatPulse_ = 0, minePulse_ = 0, healthShown_ = 1;
    Vector2 camFocus_{640, 370};
    float confirm_[2][9] = {};
    int heldCount_[9] = {};
    std::vector<int> keyLane_ = std::vector<int>(Input::kMaxCodes, -1);
    std::vector<Popup> popups_;
    gfx::Particles fx_;
    int pauseSel_ = 0, deadSel_ = 0;
    double deadAt_ = 0, loadStart_ = 0, lastHitsound_ = 0, lastMissSfx_ = 0;
    double lastSplash_[9] = {};
    bool finished_ = false;
};

std::unique_ptr<State> makePlay(const PlayRequest& req) { return std::make_unique<PlayState>(req); }

}  // namespace gh
