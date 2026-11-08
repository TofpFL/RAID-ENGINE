// The duel: game flow, input, judgement events, classic visuals and the HUD.
// Psych-mode scene and scripting live in play_psych.cpp.
#include "state_play.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "common.hpp"
#include "rlgl.h"

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

Color rgbColor(uint32_t rgb) { return Color{(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb, 255}; }
}  // namespace

PlayState::PlayState(PlayRequest r) : req_(std::move(r)) {}

PlayState::~PlayState() {
    scripts_.reset();
    releaseAudio();
    if (hudRt_.id && IsWindowReady()) UnloadRenderTexture(hudRt_);
}

void PlayState::enter() {
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
    bool mirror = !req_.story && a.mods.mirror;
    if (!req_.replayPath.empty()) {
        if (!playback_.load(req_.replayPath)) {
            phase_ = Phase::Error;
            error_ = "Cannot read replay " + req_.replayPath;
            return;
        }
        replaying_ = true;
        rate_ = playback_.rate;
        mirror = playback_.mirror;
    }
    const SongEntry copy = *entry_;
    const std::string diff = req_.diff;
    job_ = std::async(std::launch::async, [copy, diff, mirror] { return app().library.prepare(copy, diff, mirror); });
    loadStart_ = a.now;
}

void PlayState::update(double dt) {
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
    for (auto it = debug_.begin(); it != debug_.end();)
        it = now - it->t > 8.0 ? debug_.erase(it) : it + 1;
    stage_.update(float(dt), now);
    fx_.update(float(dt));
    if (phase_ == Phase::Paused) { updatePause(); return; }
    if (phase_ == Phase::Dead) { updateDead(); return; }
    if (phase_ == Phase::Waiting) {
        // A script stopped the countdown (cutscene, dialogue): keep scripts
        // running, start anyway if nothing resumes it.
        if (psych_) psychUpdate(dt);
        if (a.input.back()) { exitToMenu(); return; }
        if (now - waitStart_ > 6.0 || a.input.accept()) beginCountdown();
        return;
    }
    if (phase_ == Phase::Ending) {
        if (psych_) psychUpdate(dt);
        if (now - endWaitStart_ > 8.0 || a.input.accept()) { endRequested_ = true; finish(); }
        return;
    }

    songMs_ = a.mixer.time(now) * 1000.0 - a.settings.offsetMs * rate_;
    handleInput();
    if (phase_ != Phase::Playing) return;
    if (!songStarted_ && songMs_ >= 0) {
        songStarted_ = true;
        if (scripts_) scripts_->call("onSongStart");
    }
    session_.update(songMs_);
    handleNoteEvents();
    processChartEvents();

    // Psych order: onBeatHit, then onStepHit, then onSectionHit.
    const double beatF = chart_.beatAt(songMs_);
    const int beat = int(std::floor(beatF));
    if (beat != lastBeat_) {
        for (int b = std::max(lastBeat_ + 1, beat - 3); b <= beat; ++b) onBeat(b);
        lastBeat_ = beat;
    }
    const int step = int(std::floor(beatF * 4.0));
    if (step != lastStep_) {
        for (int s = std::max(lastStep_ + 1, step - 8); s <= step; ++s) onStep(s);
        lastStep_ = step;
    }
    for (int side = 0; side < 2; ++side)
        for (int l = 0; l < chart_.keys; ++l)
            if (session_.isHolding(side, l)) {
                confirm_[side][l] = std::max(confirm_[side][l], 0.7f);
                CharacterState& c = charOf(side);
                c.singUntil = std::max(c.singUntil, now + 0.12);
            }
    const int focus = chart_.focusAt(songMs_);
    if (focus != lastFocus_) {
        lastFocus_ = focus;
        if (psych_) psychFocus(focus);
    }
    if (!psych_) {
        const bool focusLeft = (focus == 1) == playerLeft_;
        camFocus_.x = approach(camFocus_.x, focusLeft ? 540.0f : 740.0f, 2.4f, float(dt));
        camFocus_.y = approach(camFocus_.y, 370.0f, 2.4f, float(dt));
        camZoom_ = approach(camZoom_, 1.0f, 4.0f, float(dt));
        hudZoom_ = approach(hudZoom_, 1.0f, 5.0f, float(dt));
    } else {
        psychHoldRefresh();
        psychUpdate(dt);
        if (phase_ != Phase::Playing) return;
    }
    beatPulse_ = std::max(0.0f, beatPulse_ - float(dt) * 4.0f);
    minePulse_ = std::max(0.0f, minePulse_ - float(dt) * 3.0f);
    for (auto& side : confirm_)
        for (float& c : side) c = std::max(0.0f, c - float(dt) * 5.0f);
    healthShown_ = approach(healthShown_, float(session_.health), 10.0f, float(dt));
    if (speedTweenDur_ > 0) {
        speedTweenT_ = std::min(speedTweenDur_, speedTweenT_ + dt);
        speedMult_ = speedTweenFrom_ + (speedTweenTo_ - speedTweenFrom_) * (speedTweenT_ / speedTweenDur_);
        if (speedTweenT_ >= speedTweenDur_) speedTweenDur_ = 0;
    }

    if (session_.dead) {
        if (scripts_ && scripts_->callStops("onGameOver")) {
            session_.dead = false;
            session_.setHealth(0.001);
        } else {
            die();
            return;
        }
    }
    const double audioEnd = prep_.inst ? a.mixer.lengthSec() * 1000.0 : 0.0;
    const double endAt = std::max(chart_.lengthMs + 800.0 * rate_, std::min(audioEnd, chart_.lengthMs + 8000.0 * rate_));
    if ((songMs_ >= endAt && session_.allResolved()) || songMs_ > chart_.lengthMs + 20000.0) finish();
}

void PlayState::draw() {
    App& a = app();
    const double now = a.now;
    if (phase_ == Phase::Loading || phase_ == Phase::Error) {
        drawLoading();
        return;
    }
    if (psych_) {
        psychDraw();
    } else {
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
    }

    gfx::beginCanvas();
    if (phase_ == Phase::Paused) drawPause();
    if (phase_ == Phase::Dead) drawDead();
    drawDebugLog();
    gfx::endCanvas();
}

// ------------------------------------------------------------------- setup --

void PlayState::setup() {
    App& a = app();
    chart_ = std::move(prep_.chart);
    cfg_.ghostTapping = a.settings.ghostTapping;
    cfg_.botplay = !req_.story && a.mods.botplay;
    cfg_.practice = !req_.story && a.mods.practice;
    cfg_.rate = rate_;
    playerLeft_ = !req_.story && a.mods.mirror;
    if (replaying_) {
        cfg_.botplay = false;
        cfg_.practice = true;  // a replay never ends early
        cfg_.ghostTapping = playback_.ghostTapping;
        playerLeft_ = playback_.mirror;
    }
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

    // Script note ids: every note in chart order.
    noteOrder_.clear();
    {
        Session probe;
        probe.init(chart_, cfg_);
        std::vector<NoteRef> refs(chart_.notes.size(), NoteRef{0, 0, 0});
        for (int side = 0; side < 2; ++side)
            for (int l = 0; l < chart_.keys; ++l) {
                const auto& v = probe.lane(side, l);
                for (size_t i = 0; i < v.size(); ++i)
                    if (v[i].id < refs.size()) refs[v[i].id] = NoteRef{uint8_t(side), uint8_t(l), uint32_t(i)};
            }
        noteOrder_ = std::move(refs);
    }

    psych_ = !entry_->builtin && setupPsych();
    if (!psych_) layoutStrums();  // Psych mode laid them out before its scripts ran
    if (psych_) {
        leftColor_ = rgbColor(barColor_[0]);
        rightColor_ = rgbColor(barColor_[1]);
    }
    a.mixer.setTracks(prep_.inst, prep_.voiceP, prep_.voiceO);
    a.mixer.setRate(rate_);
    restart();
}

void PlayState::layoutStrums() {
    App& a = app();
    const Settings& s = a.settings;
    const int K = chart_.keys;
    scene::Scene& sc = *scene_;
    sc.keys = K;
    sc.strums.assign(size_t(2 * K), scene::Strum{});
    const float w = std::min(112.0f, 500.0f / float(K));
    for (int side = 0; side < 2; ++side)
        for (int lane = 0; lane < K; ++lane) {
            scene::Strum& st = strum(side, lane);
            st.downScroll = s.downscroll;
            float cx, cy = s.downscroll ? gfx::H - 112.0f : 112.0f;
            if (psych_ && K == 4) {
                // Psych Engine's own strum positions, so scripts line up.
                const float baseX = s.middlescroll ? -278.0f : 42.0f;
                float x = baseX + 50 + 112.0f * float(lane) + (side == 1 ? gfx::W * 0.5f : 0.0f);
                if (s.middlescroll && side == 0) {
                    x += 310;
                    if (lane > 1) x += gfx::W * 0.5f + 25;
                }
                st.x = x;
                st.y = s.downscroll ? gfx::H - 150.0f : 50.0f;
                if (s.middlescroll && side == 0) st.alpha = 0.35f;
            } else {
                if (s.middlescroll) {
                    if (side == 1) cx = 640.0f + (float(lane) - (K - 1) * 0.5f) * w;
                    else {
                        const int half = (K + 1) / 2;
                        cx = lane < half ? 90.0f + lane * w * 0.8f : gfx::W - 90.0f - float(K - 1 - lane) * w * 0.8f;
                        st.alpha = 0.35f;
                        st.scaleX = st.scaleY = 0.7f * 0.8f;
                    }
                } else {
                    const bool left = (side == 1) == playerLeft_;
                    cx = (left ? 320.0f : 960.0f) + (float(lane) - (K - 1) * 0.5f) * w;
                }
                st.x = cx - 56.0f;
                st.y = cy - 56.0f;
            }
            if (psych_ && playerLeft_ && K == 4 && !s.middlescroll) {
                // Playing as the opponent: the player's lanes are on the left.
                st.x = 42.0f + 50 + 112.0f * float(lane) + (side == 0 ? gfx::W * 0.5f : 0.0f);
            }
            st.defX = st.x;
            st.defY = st.y;
        }
}

scene::Strum& PlayState::strum(int side, int lane) { return scene_->strums[size_t(side == 0 ? lane : chart_.keys + lane)]; }
const scene::Strum& PlayState::strum(int side, int lane) const {
    return scene_->strums[size_t(side == 0 ? lane : chart_.keys + lane)];
}

Vector2 PlayState::strumCenter(int side, int lane) const {
    const scene::Strum& s = strum(side, lane);
    return {s.x + 56.0f, s.y + 56.0f};
}

float PlayState::noteSize() const { return std::min(112.0f, 500.0f / float(chart_.keys)) * 0.94f; }

double PlayState::scrollSpeed() const {
    const Settings& s = app().settings;
    const double base = s.scrollSpeed > 0.001f ? s.scrollSpeed : chart_.speed;
    return base * speedMult_;
}

void PlayState::restart() {
    App& a = app();
    session_.init(chart_, cfg_);
    popups_.clear();
    fx_.clear();
    for (auto& s : confirm_)
        for (float& c : s) c = 0;
    for (int& h : heldCount_) h = 0;
    for (bool& t : touchDown_) t = false;
    record_.events.clear();
    replayIdx_ = 0;
    leadMs_ = 60000.0 / chart_.bpmAt(0) * 5.0;
    lastBeat_ = int(std::floor(chart_.beatAt(-leadMs_))) - 1;
    lastStep_ = lastBeat_ * 4 + 3;
    lastSection_ = -100000;
    lastFocus_ = -1;
    eventIdx_ = 0;
    for (int t = 0; t < Mixer::kTrackCount; ++t) a.mixer.setTrackVolume(t, 1.0f);
    a.mixer.fadeTo(1.0f, 0.0f);
    songMs_ = -leadMs_;
    healthShown_ = 1.0f;
    camFocus_ = {640, 370};
    camZoom_ = hudZoom_ = 1.0f;
    speedMult_ = 1.0;
    speedTweenDur_ = 0;
    left_.singUntil = right_.singUntil = -10;
    left_.dead = right_.dead = false;
    finished_ = songStarted_ = countdownStarted_ = endRequested_ = false;
    a.input.clear();
    if (psych_ && scripts_ && scripts_->callStops("onStartCountdown")) {
        a.mixer.stop();
        phase_ = Phase::Waiting;
        waitStart_ = a.now;
        return;
    }
    beginCountdown();
}

void PlayState::beginCountdown() {
    App& a = app();
    if (countdownStarted_ && phase_ == Phase::Playing) return;
    countdownStarted_ = true;
    a.mixer.play(-leadMs_ / 1000.0);
    songMs_ = -leadMs_;
    phase_ = Phase::Playing;
    if (scripts_) {
        scripts_->setGlobal("startedCountdown", true);
        scripts_->call("onCountdownStarted");
    }
}

// ------------------------------------------------------------------- input --

void PlayState::handleInput() {
    App& a = app();
    for (const InputEvent& e : a.input.events()) {
        const bool pauseKey = e.code == KEY_ESCAPE || e.code == kPadBase + GAMEPAD_BUTTON_MIDDLE_RIGHT ||
                              (e.code == KEY_ENTER && !a.input.isDown(KEY_LEFT_ALT) && !a.input.isDown(KEY_RIGHT_ALT));
        if (e.down && pauseKey && (e.code <= 0 || e.code >= Input::kMaxCodes || keyLane_[size_t(e.code)] < 0)) {
            if (scripts_ && scripts_->callStops("onPause")) continue;
            pause();
            return;
        }
        if (e.code <= 0 || e.code >= Input::kMaxCodes || replaying_) continue;
        const int lane = keyLane_[size_t(e.code)];
        if (lane < 0) continue;
        const double t = std::min(songMs_, songMs_ - (a.now - e.t) * 1000.0 * rate_);
        if (e.down) {
            if (heldCount_[lane]++ == 0) laneDown(lane, t);
        } else if (heldCount_[lane] > 0 && --heldCount_[lane] == 0) {
            laneUp(lane, t);
        }
    }
    if (a.settings.touchControls && !replaying_) handleTouch();
    // Replays feed the recorded presses at their song times.
    while (replaying_ && replayIdx_ < playback_.events.size() && playback_.events[replayIdx_].t <= songMs_) {
        const ReplayEvent& ev = playback_.events[replayIdx_++];
        if (ev.lane >= chart_.keys) continue;
        if (ev.down) { heldCount_[ev.lane] = 1; laneDown(ev.lane, ev.t); }
        else { heldCount_[ev.lane] = 0; laneUp(ev.lane, ev.t); }
    }
}

void PlayState::laneDown(int lane, double t) {
    session_.press(lane, t);
    if (!replaying_ && !cfg_.botplay) record_.events.push_back({t, uint8_t(lane), true});
    if (scripts_) scripts_->call("onKeyPress", {scene::Value(double(lane))});
}

void PlayState::laneUp(int lane, double t) {
    session_.release(lane, t);
    if (!replaying_ && !cfg_.botplay) record_.events.push_back({t, uint8_t(lane), false});
    if (scripts_) scripts_->call("onKeyRelease", {scene::Value(double(lane))});
}

// Touchscreens: each player lane is a full-height column around its strum.
void PlayState::handleTouch() {
    const float sw = float(GetScreenWidth()), sh = float(GetScreenHeight());
    const float sc = std::min(sw / gfx::W, sh / gfx::H);
    const float ox = (sw - gfx::W * sc) * 0.5f;
    bool want[9] = {};
    const int K = chart_.keys;
    const float half = std::min(112.0f, 500.0f / float(K)) * 0.5f + 4.0f;
    for (int i = 0; i < GetTouchPointCount(); ++i) {
        const float x = (GetTouchPosition(i).x - ox) / sc;
        int best = -1;
        float bestD = 1e9f;
        for (int l = 0; l < K; ++l) {
            const float d = std::fabs(x - strumCenter(1, l).x);
            if (d < bestD) { bestD = d; best = l; }
        }
        if (best >= 0 && bestD <= half * 1.6f) want[best] = true;
    }
    for (int l = 0; l < K; ++l) {
        if (want[l] && !touchDown_[l]) {
            touchDown_[l] = true;
            if (heldCount_[l]++ == 0) laneDown(l, songMs_);
        } else if (!want[l] && touchDown_[l]) {
            touchDown_[l] = false;
            if (heldCount_[l] > 0 && --heldCount_[l] == 0) laneUp(l, songMs_);
        }
    }
}

void PlayState::popup(const std::string& text, Color c, const std::string& timing) {
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

void PlayState::splash(int side, int lane) {
    const Color c = gfx::dirColor(gfx::laneDir(chart_.keys, lane));
    const Vector2 p = strumCenter(side, lane);
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

void PlayState::handleNoteEvents() {
    App& a = app();
    const double now = a.now;
    std::vector<GameEvent> evs;
    evs.swap(session_.events);
    for (const GameEvent& ev : evs) {
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
                if (psych_) psychNoteHit(ev, false);
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
                if (psych_) psychMiss(ev, ev.type == GameEvent::GhostMiss);
                break;
            case GameEvent::PlayerMine:
                charOf(1).miss(dir, now);
                sfxPlay(Sfx::Mine);
                minePulse_ = 1.0f;
                popup("CURSED", kMissColor, "");
                if (psych_) psychMiss(ev, false);
                break;
            case GameEvent::OpponentHit:
                confirm_[0][ev.lane] = 1.0f;
                if (!(ev.flags & kFlagNoAnim)) charOf(0).sing(dir, now, ev.length / 1000.0 / rate_);
                if (psych_) psychNoteHit(ev, true);
                break;
            default: break;
        }
    }
    if (psych_ && !evs.empty()) updateStatGlobals();
}

void PlayState::processChartEvents() {
    while (eventIdx_ < chart_.events.size() && chart_.events[eventIdx_].timeMs <= songMs_) {
        const psych::ChartEvent e = chart_.events[eventIdx_++];
        handleBuiltinEvent(e);
        if (scripts_)
            scripts_->call("onEvent", {scene::Value(e.name), scene::Value(e.v1), scene::Value(e.v2), scene::Value(e.timeMs)});
    }
}

void PlayState::onBeat(int b) {
    App& a = app();
    if (b >= -4 && b <= -1) {
        sfxPlay(Sfx(int(Sfx::Count3) + (b + 4)));
        if (scripts_) scripts_->call("onCountdownTick", {scene::Value(double(b + 4))});
    }
    left_.beat(a.now);
    right_.beat(a.now);
    stage_.onBeat(b);
    beatPulse_ = 1.0f;
    if (psych_) {
        psychBeat(b);
        return;
    }
    if (b >= 0 && b % 4 == 0 && a.settings.camZoom) {
        camZoom_ += 0.015f;
        hudZoom_ += 0.03f;
    }
}

void PlayState::onStep(int s) {
    if (!scripts_) return;
    scripts_->setGlobal("curStep", double(s));
    scripts_->setGlobal("curDecStep", chart_.beatAt(songMs_) * 4.0);
    scripts_->call("onStepHit", {scene::Value(double(s))});
    if (scripts_->kadeMode) scripts_->call("stepHit", {scene::Value(double(s))});
    if (s >= 0 && s % 16 == 0) onSection(s / 16);
}

void PlayState::onSection(int s) {
    if (s == lastSection_) return;
    lastSection_ = s;
    if (!scripts_) return;
    scripts_->setGlobal("curSection", double(s));
    scripts_->setGlobal("mustHitSection", chart_.focusAt(songMs_ + 1) == 1);
    scripts_->call("onSectionHit");
}

// ------------------------------------------------------------- pause/death --

void PlayState::pause() {
    App& a = app();
    phase_ = Phase::Paused;
    pauseSel_ = 0;
    a.mixer.pause();
    for (auto& kv : sounds_)
        if (IsSoundPlaying(kv.second.snd)) PauseSound(kv.second.snd);
    sfxPlay(Sfx::Scroll);
}

void PlayState::resume() {
    App& a = app();
    a.mixer.resume();
    for (auto& kv : sounds_)
        if (kv.second.started && !kv.second.paused) ResumeSound(kv.second.snd);
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
    if (scripts_) scripts_->call("onResume");
}

void PlayState::updatePause() {
    App& a = app();
    if (a.input.up()) { pauseSel_ = (pauseSel_ + 2) % 3; sfxPlay(Sfx::Scroll); }
    if (a.input.down()) { pauseSel_ = (pauseSel_ + 1) % 3; sfxPlay(Sfx::Scroll); }
    if (a.input.back()) { resume(); return; }
    if (a.input.accept()) {
        sfxPlay(Sfx::Confirm);
        if (pauseSel_ == 0) resume();
        else if (pauseSel_ == 1) restartSong();
        else exitToMenu();
    }
}

void PlayState::die() {
    App& a = app();
    phase_ = Phase::Dead;
    deadAt_ = a.now;
    deadSel_ = 0;
    a.mixer.fadeTo(0.0f, 0.5f);
    sfxPlay(Sfx::Death);
    charOf(1).miss(1, a.now);
    charOf(1).dead = true;
    if (scripts_) scripts_->call("onGameOverStart");
}

void PlayState::updateDead() {
    App& a = app();
    if (a.now - deadAt_ < 0.8) return;
    if (a.input.accept()) { sfxPlay(Sfx::Confirm); restartSong(); }
    else if (a.input.back()) exitToMenu();
}

void PlayState::exitToMenu() {
    App& a = app();
    if (scripts_) scripts_->call("onDestroy");
    a.mixer.stop();
    a.mixer.clear();
    a.change(req_.story ? makeStory() : makeFreeplay());
}

void PlayState::finish() {
    if (finished_) return;
    App& a = app();
    if (scripts_ && !endRequested_) {
        if (scripts_->callStops("onEndSong")) {
            if (phase_ != Phase::Ending) {
                phase_ = Phase::Ending;
                endWaitStart_ = a.now;
            }
            return;
        }
    }
    finished_ = true;
    if (scripts_) scripts_->call("onDestroy");
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
    r.practice = cfg_.practice && !replaying_;
    r.replay = replaying_;
    r.rate = rate_;
    r.color = entry_->color;
    const bool saveable = !cfg_.botplay && !cfg_.practice && !replaying_ && rate_ >= 1.0;
    if (saveable) {
        ScoreEntry e{r.score, r.acc, r.misses, r.rank, r.clear};
        r.newBest = a.scores.submit(Scores::key(entry_->id + (playerLeft_ ? "#opponent" : ""), req_.diff), e);
    }
    if (!replaying_ && !cfg_.botplay && !record_.events.empty()) {
        record_.songId = entry_->id;
        record_.diff = req_.diff;
        record_.title = r.title;
        record_.rate = rate_;
        record_.mirror = playerLeft_;
        record_.ghostTapping = cfg_.ghostTapping;
        record_.keys = chart_.keys;
        record_.score = r.score;
        record_.acc = r.acc;
        record_.misses = r.misses;
        const std::string dir = a.base + "replays/";
        const std::string last = dir + replayFileName(entry_->id, req_.diff, "last");
        if (record_.save(last)) r.replayPath = last;
        if (r.newBest) record_.save(dir + replayFileName(entry_->id, req_.diff, "best"));
    } else if (replaying_) {
        r.replayPath = req_.replayPath;
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
        std::printf("[auto] %s (%s): score %lld, accuracy %.2f%%, misses %d, max combo %d, notes %zu%s\n", r.title.c_str(),
                    r.diff.c_str(), r.score, r.acc * 100.0, r.misses, r.maxCombo, session_.totalPlayerNotes,
                    psych_ ? " [psych scene]" : "");
    a.change(makeResults(r));
}

// ------------------------------------------------------------------- lanes --

void PlayState::drawMine(Vector2 c, float size, float alpha, float rot) {
    const float t = float(app().now);
    gfx::glow(c, size * 0.7f, fade(Color{255, 30, 60, 255}, 0.5f * alpha));
    DrawPoly(c, 8, size * 0.36f, t * 90.0f + rot, fade(Color{150, 20, 40, 255}, alpha));
    DrawCircleV(c, size * 0.27f, fade(Color{30, 6, 12, 255}, alpha));
    DrawRing(c, size * 0.24f, size * 0.29f, 0, 360, 32, fade(Color{255, 60, 80, 255}, alpha));
    const float r = size * 0.12f;
    DrawLineEx({c.x - r, c.y - r}, {c.x + r, c.y + r}, 3, fade(Color{255, 90, 100, 255}, alpha));
    DrawLineEx({c.x - r, c.y + r}, {c.x + r, c.y - r}, 3, fade(Color{255, 90, 100, 255}, alpha));
}

void PlayState::drawLanes() {
    App& a = app();
    const Settings& s = a.settings;
    const int K = chart_.keys;
    const float ns = noteSize();
    const double pxPerMs = 0.45 * scrollSpeed() / rate_;
    const double reach = (gfx::H * 1.5 + ns) / pxPerMs;
    for (int side = 0; side < 2; ++side) {
        for (int lane = 0; lane < K; ++lane) {
            const scene::Strum& st = strum(side, lane);
            const Vector2 c = strumCenter(side, lane);
            const float sc = st.scaleX / 0.7f;
            const float size = ns * std::fabs(sc);
            const float alphaStrum = st.alpha;
            const int dir = gfx::laneDir(K, lane);
            const Color col = gfx::dirColor(dir);
            const bool rotated = std::fabs(st.direction - 90.0f) > 0.01f;
            if (rotated) {
                rlDrawRenderBatchActive();
                rlPushMatrix();
                rlTranslatef(c.x, c.y, 0);
                rlRotatef(st.direction - 90.0f, 0, 0, 1);
                rlTranslatef(-c.x, -c.y, 0);
            }
            if (st.visible && alphaStrum > 0.001f) {
                float conf = confirm_[side][lane];
                if (side == 0 && !s.opponentStrums) conf = 0;
                const bool pressed = side == 1 && heldCount_[lane] > 0 && !cfg_.botplay;
                if (conf > 0.01f) {
                    gfx::glow(c, size * 0.95f * conf, fade(col, 0.6f * alphaStrum));
                    gfx::drawNote(dir, c, size, alphaStrum, 1.0f + 0.06f * conf, st.angle);
                } else if (pressed) {
                    gfx::drawReceptor(dir, c, size, fade(gfx::mix(WHITE, col, 0.6f), alphaStrum), 0.9f, st.angle);
                } else {
                    gfx::drawReceptor(dir, c, size, fade(WHITE, alphaStrum), 1.0f, st.angle);
                }
            }
            const float dirSign = st.downScroll ? -1.0f : 1.0f;
            const std::vector<LaneNote>& v = session_.lane(side, lane);
            if (!v.empty()) {
                const double tStart = songMs_ - reach * 0.5 - session_.maxSustain(side, lane);
                const double tEnd = songMs_ + reach;
                auto lo = std::lower_bound(v.begin(), v.end(), tStart, [](const LaneNote& n, double t) { return n.t < t; });
                auto hi = std::upper_bound(lo, v.end(), tEnd, [](double t, const LaneNote& n) { return t < n.t; });
                const bool anyVis = !noteVis_.empty();
                auto visOf = [&](const LaneNote& n) -> const NoteVis* {
                    if (!anyVis) return nullptr;
                    auto it = noteVis_.find(n.id);
                    return it == noteVis_.end() ? nullptr : &it->second;
                };
                const float offY = (gfx::H + size) * 0.5f;
                for (auto it = lo; it != hi; ++it) {
                    const LaneNote& n = *it;
                    if (n.len <= 0) continue;
                    if (n.state == kHeldDone || n.state == kHit || n.state == kMineHit || n.state == kMinePassed) continue;
                    const NoteVis* nv = visOf(n);
                    if (nv && !nv->visible) continue;
                    const double mult = nv ? nv->multSpeed : 1.0;
                    const double endT = n.t + n.len;
                    const bool active = n.state == kHolding;
                    const double startT = active ? std::max(n.t, songMs_) : n.t;
                    const float y0 = active ? c.y : c.y + dirSign * float((startT - songMs_) * pxPerMs * mult);
                    const float y1 = c.y + dirSign * float((endT - songMs_) * pxPerMs * mult);
                    if ((dirSign > 0 && y1 < c.y - offY) || (dirSign < 0 && y1 > c.y + offY)) continue;
                    const float al = (n.state == kMissed || n.state == kDropped ? 0.35f : 1.0f) * alphaStrum * (nv ? nv->alpha : 1.0f);
                    gfx::drawSustain(dir, c.x + (nv ? nv->offsetX : 0.0f), y0, y1, size * 0.3f, al * 0.85f + 0.15f * (al > 0), true);
                }
                // Level of detail: notes closer than ~15% of a note overlap into
                // a solid wall anyway, so only one of them is drawn.
                const float minGap = std::max(2.0f, ns * 0.15f);
                float lastY = 1e9f;
                for (auto it = hi; it != lo;) {
                    --it;
                    const LaneNote& n = *it;
                    if (n.state != kPending && n.state != kMissed) continue;
                    const NoteVis* nv = visOf(n);
                    if (nv && !nv->visible) continue;
                    const double mult = nv ? nv->multSpeed : 1.0;
                    const float y = c.y + dirSign * float((n.t - songMs_) * pxPerMs * mult);
                    if (n.state == kMissed && std::fabs(y - c.y) > 200) continue;
                    if (y < -size || y > gfx::H + size) {
                        if (!rotated) continue;
                    }
                    if (std::fabs(y - lastY) < minGap && n.len <= 0) continue;
                    lastY = y;
                    const float al = (n.state == kMissed ? 0.35f : 1.0f) * alphaStrum * (nv ? nv->alpha : 1.0f);
                    if (al <= 0.002f) continue;
                    const Vector2 p{c.x + (nv ? nv->offsetX : 0.0f), y + (nv ? nv->offsetY : 0.0f)};
                    if (n.kind == kMine) drawMine(p, size, al, st.angle);
                    else gfx::drawNote(dir, p, size, al, 1.0f, st.angle);
                }
            }
            if (rotated) {
                rlDrawRenderBatchActive();
                rlPopMatrix();
            }
        }
    }
}

bool PlayState::hudShown(const char* name, float* alpha) const {
    if (alpha) *alpha = 1.0f;
    if (!psych_) return true;
    auto it = scene_->hud.find(name);
    if (it == scene_->hud.end()) return true;
    if (alpha) *alpha = it->second->alpha;
    return it->second->visible && it->second->alpha > 0.002f;
}

void PlayState::drawHud() {
    App& a = app();
    const Settings& s = a.settings;
    const double now = a.now;
    // health bar
    const float by = s.downscroll ? 82.0f : 640.0f, bx = 340, bw = 600, bh = 18;
    const float hp = healthShown_ / 2.0f;
    const float leftFrac = playerLeft_ ? hp : 1.0f - hp;
    const float split = bx + bw * leftFrac;
    float barAlpha = 1;
    if (hudShown("healthBar", &barAlpha)) {
        DrawRectangleRounded({bx - 5, by - 5, bw + 10, bh + 10}, 0.6f, 8, fade(Color{8, 4, 14, 230}, barAlpha));
        DrawRectangleRec({bx, by, split - bx, bh}, fade(leftColor_, barAlpha));
        DrawRectangleRec({split, by, bx + bw - split, bh}, fade(rightColor_, barAlpha));
        DrawRectangleRec({bx, by, bw, bh * 0.35f}, Color{255, 255, 255, (unsigned char)(40 * barAlpha)});
    }
    const bool playerLosing = session_.health < 0.4, oppLosing = session_.health > 1.6;
    const float isz = 86.0f * (1.0f + 0.12f * beatPulse_);
    for (int i = 0; i < 2; ++i) {  // 0 = left (opponent side), 1 = right
        float ia = 1;
        if (!hudShown(i == 0 ? "iconP2" : "iconP1", &ia)) continue;
        const Vector2 pos{split + (i == 0 ? -36.0f : 36.0f), by + bh * 0.5f};
        const bool losing = i == 0 ? (playerLeft_ ? playerLosing : oppLosing) : (playerLeft_ ? oppLosing : playerLosing);
        if (psych_ && hasPsychIcon_[i] && iconTex_[i].id) {
            const int frames = std::max(1, iconFrames_[i]);
            const float fw = float(iconTex_[i].width) / float(frames), fh = float(iconTex_[i].height);
            const int fi = losing && frames > 1 ? 1 : 0;
            const float dsz = isz * 1.6f;
            Rectangle src{fw * float(fi), 0, i == 1 ? -fw : fw, fh};
            DrawTexturePro(iconTex_[i], src, {pos.x, pos.y, dsz, dsz * fh / fw}, {dsz * 0.5f, dsz * fh / fw * 0.5f}, 0,
                           fade(WHITE, ia));
        } else {
            const CharacterState& cs = i == 0 ? left_ : right_;
            drawIcon(cs.id, cs.tint, pos, isz, losing, now);
        }
    }
    // score line
    if (hudShown("scoreTxt")) {
        std::string line = "Score " + fmtThousands(session_.score) + "   |   Misses " + std::to_string(session_.misses) +
                           "   |   " + TextFormat("%.2f%%", session_.accuracy() * 100.0) + " (" +
                           (ratingNameOverride_.empty() ? session_.rank() : ratingNameOverride_) + ")";
        const std::string cl = ratingFcOverride_.empty() ? session_.clearLabel() : ratingFcOverride_;
        if (!cl.empty()) line += "   |   " + cl;
        if (psych_) {
            auto it = scene_->hud.find("scoreTxt");
            if (it != scene_->hud.end() && !it->second->text.text.empty()) line = it->second->text.text;
        }
        gfx::text(FontId::Ui, line, gfx::W / 2, by + 36, 24, WHITE, Align::Center, 2.0f);
    }
    if ((cfg_.botplay || replaying_) && hudShown("botplayTxt"))
        gfx::text(FontId::Title, replaying_ ? "REPLAY" : "BOTPLAY", gfx::W / 2, s.downscroll ? 560 : 150, 40,
                  fade(kGold, 0.55f + 0.35f * std::sin(float(now) * 3.0f)), Align::Center, 2.0f);
    if (s.touchControls && !replaying_) {
        const float half = std::min(112.0f, 500.0f / float(chart_.keys)) * 0.5f;
        for (int l = 0; l < chart_.keys; ++l) {
            const Vector2 c = strumCenter(1, l);
            const Color col = gfx::dirColor(gfx::laneDir(chart_.keys, l));
            DrawRectangleRec({c.x - half, gfx::H * 0.55f, half * 2 - 4, gfx::H * 0.45f}, fade(col, touchDown_[l] ? 0.28f : 0.09f));
        }
    }
    if (cfg_.practice && !replaying_) gfx::text(FontId::Ui, "PRACTICE MODE", gfx::W / 2, by + 62, 20, fade(kGold, 0.8f), Align::Center, 1.5f);
    if (rate_ != 1.0) gfx::text(FontId::Ui, TextFormat("%.2fx", rate_), bx + bw + 60, by - 4, 24, kGold, Align::Left, 1.5f);

    // song progress
    if (hudShown("timeBar")) {
        const float ty = s.downscroll ? gfx::H - 26.0f : 12.0f;
        const float prog = float(clampv(songMs_ / std::max(1.0, chart_.lengthMs), 0.0, 1.0));
        DrawRectangleRounded({440, ty, 400, 14}, 0.8f, 8, Color{8, 4, 14, 200});
        DrawRectangleRounded({442, ty + 2, 396 * prog, 10}, 0.8f, 8, gfx::mix(leftColor_, WHITE, 0.3f));
        const double remain = std::max(0.0, (chart_.lengthMs - songMs_) / 1000.0 / rate_);
        if (hudShown("timeTxt"))
            gfx::text(FontId::Ui, chart_.title + "  -  " + req_.diff + "  -  " + fmtTime(remain), gfx::W / 2,
                      s.downscroll ? ty - 26 : ty + 14, 20, fade(WHITE, 0.85f), Align::Center, 1.5f);
    }

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
    if (songMs_ < 0 && phase_ == Phase::Playing) {
        const double bf = chart_.beatAt(songMs_);
        const int b = int(std::floor(bf));
        const float frac = float(bf - b);
        static const char* words[4] = {"III", "II", "I", "ASCEND!"};
        static const char* psychWords[4] = {"3", "2", "1", "GO!"};
        if (b >= -4 && b <= -1) {
            const float sc = 1.0f + 0.35f * std::pow(1.0f - frac, 3.0f);
            gfx::glowText(FontId::Title, psych_ ? psychWords[b + 4] : words[b + 4], gfx::W / 2, 300 - 40 * sc, 96 * sc,
                          fade(kGold, 1.0f - frac), Color{255, 170, 60, 255});
        }
    }
}

void PlayState::drawPause() {
    DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{6, 4, 12, 190});
    gfx::glowText(FontId::Title, "PAUSED", gfx::W / 2, 140, 80, kGold, Color{255, 170, 60, 255});
    gfx::text(FontId::Ui, chart_.title + "  -  " + req_.diff, gfx::W / 2, 250, 30, kIvory, Align::Center, 1.5f);
    static const char* items[3] = {"Resume", "Restart", "Exit to menu"};
    for (int i = 0; i < 3; ++i) {
        const bool on = i == pauseSel_;
        gfx::text(FontId::Title, items[i], gfx::W / 2, 340.0f + i * 70, on ? 48.0f : 40.0f, on ? kGold : fade(kIvory, 0.6f),
                  Align::Center, 2.0f);
    }
}

void PlayState::drawDead() {
    App& a = app();
    const float t = float(a.now - deadAt_);
    DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(Color{20, 0, 6, 255}, std::min(0.85f, t * 1.2f)));
    const float al = saturate(t * 1.5f - 0.3f);
    gfx::glowText(FontId::Title, "YOU HAVE FALLEN", gfx::W / 2, 220, 72, fade(Color{255, 80, 90, 255}, al),
                  Color{255, 40, 60, 255});
    gfx::text(FontId::Ui, "ENTER rise again      ESC leave", gfx::W / 2, 360, 34, fade(kIvory, al), Align::Center, 2.0f);
}

void PlayState::drawDebugLog() {
    const double now = app().now;
    float y = 8;
    for (const DebugLine& d : debug_) {
        const float al = float(clampv(8.0 - (now - d.t), 0.0, 1.0));
        gfx::text(FontId::Ui, d.text, 10, y, 18, fade(d.c, al), Align::Left, 1.5f);
        y += 20;
    }
}

void PlayState::drawLoading() {
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
                                     "Psych Engine mods go in the mods folder, scripts and all."};
        const int ti = int((now - loadStart_) / 3.0) % 4;
        gfx::text(FontId::Ui, tips[ti], gfx::W / 2, 480, 26, fade(kIvory, 0.8f), Align::Center);
    }
    gfx::endCanvas();
}

std::unique_ptr<State> makePlay(const PlayRequest& req) { return std::make_unique<PlayState>(req); }

}  // namespace gh
