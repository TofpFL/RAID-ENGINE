// The duel: game flow, input, judgement events, classic visuals and the HUD.
// Psych-mode scene and scripting live in play_psych.cpp.
#include "state_play.hpp"
#include "psych_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio_decode.hpp"
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

// Psych Engine's rating names and full-combo labels (its interface style).
std::string psychRatingName(double acc, long long judged) {
    if (judged == 0) return "?";
    static const std::pair<double, const char*> names[] = {{0.2, "You Suck!"}, {0.4, "Shit"}, {0.5, "Bad"}, {0.6, "Bruh"},
                                                           {0.69, "Meh"}, {0.7, "Nice"}, {0.8, "Good"}, {0.9, "Great"},
                                                           {1.0, "Sick!"}};
    if (acc >= 1.0) return "Perfect!!";
    for (const auto& [limit, name] : names)
        if (acc < limit) return name;
    return "Sick!";
}

std::string psychFcLabel(const Session& s) {
    if (s.misses > 0) return s.misses < 10 ? "SDCB" : "Clear";
    if (s.counts[kMortal] + s.counts[kSinful] > 0) return "FC";
    if (s.counts[kGrace] > 0) return "GFC";
    return "SFC";
}
}  // namespace

PlayState::PlayState(PlayRequest r) : req_(std::move(r)) {}

PlayState::~PlayState() {
    stopRendering();
    if (uiFromMod_) app().psychUiForSong({});  // the interface as it was
    stopPauseMusic();
    scripts_.reset();
    releaseAudio();
    if (hudRt_.id && IsWindowReady()) UnloadRenderTexture(hudRt_);
    if (camRt_.id && IsWindowReady()) UnloadRenderTexture(camRt_);
}

void PlayState::enter() {
    App& a = app();
    a.stopMenuMusic();
    a.mixer.clear();
    if (a.blueballedSong != req_.songId) {
        a.blueballedSong = req_.songId;
        a.blueballed = 0;
    }
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
    const bool extraKeys = app().settings.extraKeys;
    job_ = std::async(std::launch::async,
                      [copy, diff, mirror, extraKeys] { return app().library.prepare(copy, diff, mirror, extraKeys); });
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
    // Psych's Auto Pause: the song pauses when the window loses focus (Android
    // pauses on its own when the app goes to the background).
    if (a.settings.autoPause && phase_ == Phase::Playing && !rendering_ && !a.autoRun.enabled && !platform::kAndroid &&
        IsWindowReady() && !IsWindowFocused()) {
        if (!(scripts_ && scripts_->callStops("onPause"))) {
            pause();
            return;
        }
    }
    if (psych_ && updateVideos(dt)) return;  // a cutscene holds the game
    if (psych_ && substate_.active) {
        const std::string name = substate_.name;
        if (scripts_) scripts_->call("onCustomSubstateUpdate", {scene::Value(name), scene::Value(dt)});
        if (substate_.active && substate_.pausesGame) {
            scene_->update(dt, chart_.stepMs(std::max(0.0, songMs_)), false);  // tweens on its objects keep going
            if (scripts_) {
                scripts_->dispatchCompletions();
                scripts_->call("onCustomSubstateUpdatePost", {scene::Value(name), scene::Value(dt)});
            }
            return;
        }
        if (scripts_) scripts_->call("onCustomSubstateUpdatePost", {scene::Value(name), scene::Value(dt)});
    }
    if (popupHold_) {  // "Popup" event: the song waits for OK
        // While recording nobody presses OK: the popup stays two seconds.
        const bool recordedLongEnough =
            rendering_ && std::any_of(notices_.begin(), notices_.end(), [&](const Notice& n) { return n.kind == 2 && now - n.t0 > 2.0; });
        if (a.input.accept() || a.input.back() || recordedLongEnough) {
            popupHold_ = false;
            notices_.erase(std::remove_if(notices_.begin(), notices_.end(), [](const Notice& n) { return n.kind == 2; }),
                           notices_.end());
            a.mixer.resume();
        }
        return;
    }
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

    // The audio offset is for the player's ears; a recording is in sync by itself.
    songMs_ = a.mixer.time(now) * 1000.0 - (rendering_ ? 0.0 : a.settings.offsetMs * rate_);
    if (rendering_) {
        const int at = int(std::max(0.0, songMs_) / 1000.0), len = int(chart_.lengthMs / 1000.0);
        a.renderLabel = TextFormat("%d:%02d / %d:%02d", at / 60, at % 60, len / 60, len % 60);
    }
    handleInput();
    if (phase_ != Phase::Playing) return;
    if (!songStarted_ && songMs_ >= 0) {
        songStarted_ = true;
        songStartAt_ = now;
        if (scripts_) scripts_->call("onSongStart");
    }
    session_.update(songMs_);
    handleNoteEvents();
    processChartEvents();
    adaptNoteGap();
    if (a.settings.noteCounter > 0) {  // notes per second of each side
        npsWindow_.push_back({now, session_.judged(), session_.opponentHits});
        while (npsWindow_.size() > 1 && now - npsWindow_.front().t > 1.0) npsWindow_.pop_front();
        nps_ = session_.judged() - npsWindow_.front().player;
        npsOpp_ = session_.opponentHits - npsWindow_.front().opponent;
        maxNps_ = std::max(maxNps_, nps_);
        maxNpsOpp_ = std::max(maxNpsOpp_, npsOpp_);
    }

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
    if (!chart_.sections.empty()) {  // the chart's own sections (any length)
        const int sec = chart_.sectionAt(songMs_);
        if (sec > lastSection_) onSection(sec);
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
        // Charts with sections move the camera at each section (onSection).
        if (psych_ && chart_.sections.empty() && !scene_->camForced) psychFocus(focus);
    }
    if (!psych_) {
        const bool focusLeft = (focus == 1) == playerLeft_;
        camFocus_.x = approach(camFocus_.x, focusLeft ? 540.0f : 740.0f, 2.4f, float(dt));
        camFocus_.y = approach(camFocus_.y, 370.0f, 2.4f, float(dt));
        if (zoomingOn_) {  // zoom events stay until the first note is hit
            camZoom_ = approach(camZoom_, 1.0f, 4.0f, float(dt));
            hudZoom_ = approach(hudZoom_, 1.0f, 5.0f, float(dt));
        }
    } else {
        psychHoldRefresh();
        psychUpdate(dt);
        if (phase_ != Phase::Playing) return;
    }
    updateJsEngine(dt);
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
    SceneRenderer::smoothScaling = a.settings.antialiasing;
    if (phase_ == Phase::Loading || phase_ == Phase::Error) {
        drawLoading();
        return;
    }
    if (psych_) {
        gfx::setViewWidth(gameW_);
        psychDraw();
        gfx::beginCanvas();
        drawNotices();
        drawBotEnergy();
        gfx::endCanvas();
        if (gameW_ < gfx::W) {
            gfx::drawLetterbox();  // the part of the picture past FlxG.width is not shown
            gfx::setViewWidth(gfx::W);
        }
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
        drawUnderNotes();
        drawLanes();
        fx_.draw(true);
        drawHud();
        gfx::endCanvas();
        gfx::beginCanvas();
        drawNotices();
        drawBotEnergy();
        gfx::endCanvas();
    }

    gfx::beginCanvas();
    if (phase_ == Phase::Paused) drawPause();
    if (phase_ == Phase::Dead) drawDead();
    drawDebugLog();
    if (debugOverlay_) drawDebugOverlay();
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
    // Psych's hit windows, Safe Frames (frames of 1/60 s) and Rating Offset.
    cfg_.windows[kDivine] = a.settings.sickWindow;
    cfg_.windows[kGrace] = a.settings.goodWindow;
    cfg_.windows[kMortal] = a.settings.badWindow;
    cfg_.windows[kSinful] = a.settings.safeFrames / 60.0 * 1000.0;
    cfg_.ratingOffset = a.settings.ratingOffset;
    playerLeft_ = !req_.story && a.mods.mirror;
    if (replaying_) {
        cfg_.botplay = false;
        cfg_.practice = true;  // a replay never ends early
        cfg_.ghostTapping = playback_.ghostTapping;
        for (int k = 0; k < kRatingCount; ++k) cfg_.windows[k] = playback_.windows[k];  // judged as it was played
        cfg_.ratingOffset = playback_.ratingOffset;
        playerLeft_ = playback_.mirror;
    }
    // Rendering mode: the song plays by itself and is recorded from here on.
    if (a.wantsRender() && !replaying_) {
        rendering_ = a.startRender(entry_->title, req_.diff);
        if (rendering_) cfg_.botplay = true;
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
    for (int l = 0; l < chart_.keys && l < 9; ++l) keyLane_[size_t(kTouchLane + l)] = l;  // on-screen lanes

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
                float x = baseX + 50 + 112.0f * float(lane) + (side == 1 ? gameW_ * 0.5f : 0.0f);
                if (s.middlescroll && side == 0) {
                    x += 310;
                    if (lane > 1) x += gameW_ * 0.5f + 25;
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
                st.x = 42.0f + 50 + 112.0f * float(lane) + (side == 0 ? gameW_ * 0.5f : 0.0f);
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
    const App& a = app();
    const Settings& s = a.settings;
    double base = s.scrollSpeed > 0.001f ? s.scrollSpeed : chart_.speed;
    if (a.mods.scrollType == 1) base *= a.mods.scrollValue;  // Freeplay modifiers
    else if (a.mods.scrollType == 2) base = a.mods.scrollValue;
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
    lastSection_ = chart_.sections.empty() ? -100000 : 0;  // Psych: sectionHit from section 1 on
    lastFocus_ = -1;
    eventIdx_ = 0;
    for (int t = 0; t < Mixer::kTrackCount; ++t) a.mixer.setTrackVolume(t, 1.0f);
    a.mixer.fadeTo(1.0f, 0.0f);
    songMs_ = -leadMs_;
    healthShown_ = 1.0f;
    camFocus_ = {640, 370};
    camZoom_ = hudZoom_ = 1.0f;
    zoomingOn_ = false;
    npsWindow_.clear();
    nps_ = maxNps_ = npsOpp_ = maxNpsOpp_ = 0;
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
    platform::log("song " + req_.songId + " " + req_.diff);
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
        const bool pauseKey = e.code == KEY_ESCAPE || e.code == KEY_BACK || e.code == kPadBase + GAMEPAD_BUTTON_MIDDLE_RIGHT ||
                              (e.code == KEY_ENTER && !a.input.isDown(KEY_LEFT_ALT) && !a.input.isDown(KEY_RIGHT_ALT));
        if (e.down && pauseKey && (e.code <= 0 || e.code >= Input::kMaxCodes || keyLane_[size_t(e.code)] < 0)) {
            if (rendering_) {  // stops the recording: the video keeps what was recorded
                stopRendering();
                exitToMenu();
                return;
            }
            if (scripts_ && scripts_->callStops("onPause")) continue;
            pause();
            return;
        }
        // F3: the debug overlay (unless F3 is a lane).
        if (e.down && e.code == KEY_F3 && !keyReserved(e.code)) {
            debugOverlay_ = !debugOverlay_;
            continue;
        }
        // 7: this song in the chart editor, as in Psych (unless 7 is a lane).
        if (e.down && (e.code == KEY_SEVEN || e.code == KEY_KP_7) && !keyReserved(e.code) && !replaying_ && !cutscene_) {
            openChartEditor();
            return;
        }
        // R: Psych's Reset button, an instant loss (unless R is a lane, Options:
        // Disable reset button, practice mode, a replay or before the song starts).
        if (e.down && e.code == KEY_R && !keyReserved(e.code) && !a.settings.noReset && !replaying_ && !cfg_.practice && !rendering_ &&
            songMs_ >= 0 && !cutscene_) {
            session_.setHealth(0);
            session_.dead = true;
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
    if (a.settings.touchControls && !a.touchMode && !replaying_) handleTouch();
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

// Touchscreens (the on-screen buttons): the whole screen is split into the
// player's lanes, with a PAUSE button in the corner.
TouchLayout PlayState::touchLayout() const {
    TouchLayout t;
    if (phase_ != Phase::Playing || (substate_.active && substate_.pausesGame)) return t;  // menus: arrows, OK, BACK
    t.pad = false;
    t.keysLeft = true;
    if (cutscene_) {
        t.keys = {{"SKIP", KEY_ENTER}};
        return t;
    }
    if (popupHold_) {  // a "Popup" window: tap anywhere to close it
        t.tapCode = KEY_ENTER;
        return t;
    }
    t.keys = {{"PAUSE", KEY_ESCAPE}};
    if (botEnergyOn_ && !cfg_.botplay) t.keys.push_back({"BOT", KEY_LEFT_CONTROL});
    if (!replaying_ && !cfg_.botplay) t.lanes = chart_.keys;
    return t;
}

// Back from the background: wait in the pause menu instead of throwing notes at the player.
void PlayState::onResume() {
    if (phase_ == Phase::Playing && !cutscene_) pause();
}

// Touch controls option (desktop): each player lane is a full-height column around its strum.
void PlayState::handleTouch() {
    bool want[9] = {};
    const int K = chart_.keys;
    const float half = std::min(112.0f, 500.0f / float(K)) * 0.5f + 4.0f;
    for (const TouchPoint& tp : touchPoints()) {  // fingers, or the mouse while clicked
        const float x = gfx::toCanvas(tp.pos).x;
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
    App& a = app();
    const double now = a.now;
    int rating = -1;
    for (int k = 0; k < kRatingCount; ++k)
        if (text == ratingInfo(Rating(k)).name) rating = k;
    if (!popups_.empty() && now - popups_.back().t0 < 0.03) {  // spam: update instead of stacking
        Popup& p = popups_.back();
        if (p.text == "MISS" && text != "MISS") return;
        p.text = text;
        p.timing = timing;
        p.c = c;
        p.combo = session_.combo;
        p.rating = rating;
        return;
    }
    // Combo stacking off (Psych's option): the new one replaces the others.
    if (!a.settings.comboStacking) popups_.clear();
    if (popups_.size() >= 16) popups_.erase(popups_.begin());
    Popup p{text, timing, c, now, session_.combo};
    p.rating = rating;
    p.seed = uint32_t(GetRandomValue(1, 0x7FFFFFFF));
    const double bpm = chart_.bpmAt(std::max(0.0, songMs_));
    p.beat = float(60.0 / std::max(1.0, bpm));
    popups_.push_back(p);
}

void PlayState::splash(int side, int lane) {
    const Color c = gfx::dirColor(gfx::laneDir(chart_.keys, lane));
    const Vector2 p = strumCenter(side, lane);
    gfx::Particle ring{};
    ring.p = p;
    ring.kind = 2;
    ring.c = c;
    ring.c.a = (unsigned char)(255 * clampv(app().settings.splashAlpha, 0.0f, 1.0f));  // Options: note splash opacity
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
        s.c.a = ring.c.a;
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
        if (ev.type == GameEvent::PlayerHit || ev.type == GameEvent::OpponentHit) zoomingOn_ = true;
        switch (ev.type) {
            case GameEvent::PlayerHit: {
                confirm_[1][ev.lane] = 1.0f;
                if (!(ev.flags & kFlagNoAnim)) charOf(1).sing(dir, now, ev.length / 1000.0 / rate_);
                a.mixer.setTrackVolume(Mixer::kVoicePlayer, 1.0f);
                const RatingInfo& ri = ratingInfo(ev.rating);
                if (!cfg_.botplay && (!cfg_.practice || replaying_)) scoreBopAt_ = now;  // Psych: the score text zooms
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
                scoreDirty_ = true;
                scoreDirtyMiss_ = false;
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
                scoreDirty_ = scoreDirtyMiss_ = true;
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
    // Psych's updateScore: scoreTxt's text, once a frame however many notes were judged.
    if (psych_ && scoreDirty_) {
        updateScoreText(scoreDirtyMiss_);
        scoreDirty_ = false;
    }
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
    if (b >= 0 && b % 4 == 0 && a.settings.camZoom && zoomingOn_) {
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
    if (chart_.sections.empty() && s >= 0 && s % 16 == 0) onSection(s / 16);
}

void PlayState::onSection(int s) {
    if (s == lastSection_) return;
    lastSection_ = s;
    const SectionInfo* info =
        s >= 0 && s < int(chart_.sections.size()) ? &chart_.sections[size_t(s)] : nullptr;
    // Psych sectionHit: the camera goes to the section's singer unless a
    // script or "Camera Follow Pos" holds it.
    if (psych_ && info && !scene_->camForced) moveCameraSection(s);
    if (!scripts_) return;
    scripts_->setGlobal("curSection", double(s));
    scripts_->setGlobal("mustHitSection", info ? info->mustHit : chart_.focusAt(songMs_ + 1) == 1);
    scripts_->setGlobal("gfSection", info && info->gf);
    scripts_->setGlobal("altAnim", info && info->alt);
    scripts_->call("onSectionHit");
}

// ------------------------------------------------------------- pause/death --

void PlayState::pause() {
    App& a = app();
    const long presses = std::count_if(record_.events.begin(), record_.events.end(), [](const ReplayEvent& e) { return e.down; });
    platform::log(TextFormat("paused at %.1f s: %ld presses, %lld hits, %lld misses", songMs_ / 1000.0, presses,
                             session_.judged(), session_.misses));
    phase_ = Phase::Paused;
    pauseSel_ = 0;
    pauseScroll_ = 0;
    pausedAt_ = a.now;
    a.mixer.pause();
    // Psych's pause music (Options: pause screen song), from a random point, fading in.
    const PsychUi& ui = psychUi();
    if (const std::string song = ui.active() ? ui.pauseMusic(a.settings.pauseMusic) : std::string(); !song.empty()) {
        stopPauseMusic();
        pauseMusic_ = LoadMusicStream(song.c_str());
        if (pauseMusic_.stream.buffer) {
            pauseMusic_.looping = true;
            pauseMusicVol_ = 0;
            SetMusicVolume(pauseMusic_, 0);
            PlayMusicStream(pauseMusic_);
            const float len = GetMusicTimeLength(pauseMusic_);
            if (len > 1) SeekMusicStream(pauseMusic_, float(GetRandomValue(0, int(len * 0.5f))));
        }
    }
    for (auto& kv : sounds_)
        if (kv.second.started && !kv.second.paused) PauseMusicStream(kv.second.mus);
    sfxPlay(Sfx::Scroll);
}

void PlayState::stopPauseMusic() {
    if (!pauseMusic_.stream.buffer) return;
    if (IsAudioDeviceReady()) {
        StopMusicStream(pauseMusic_);
        UnloadMusicStream(pauseMusic_);
    }
    pauseMusic_ = Music{};
}

void PlayState::resume() {
    App& a = app();
    stopPauseMusic();
    platform::log("resumed");
    a.mixer.resume();
    for (auto& kv : sounds_)
        if (kv.second.started && !kv.second.paused) ResumeMusicStream(kv.second.mus);
    phase_ = Phase::Playing;
    a.input.clear();
    const auto lk = a.settings.laneKeys(chart_.keys);
    for (int l = 0; l < chart_.keys; ++l) {
        if (heldCount_[l] <= 0) continue;
        bool down = a.input.isDown(kTouchLane + l);
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
    const int n = kPauseItems;
    pauseScroll_ = approach(pauseScroll_, float(pauseSel_), 12.0f, float(a.dt));
    if (pauseMusic_.stream.buffer) {
        pauseMusicVol_ = std::min(0.5f, pauseMusicVol_ + 0.01f * float(a.dt));  // as slowly as Psych's
        SetMusicVolume(pauseMusic_, std::max(pauseMusicVol_, 0.05f));
        UpdateMusicStream(pauseMusic_);
    }
    if (a.input.up()) { pauseSel_ = (pauseSel_ + n - 1) % n; sfxPlay(Sfx::Scroll); }
    if (a.input.down()) { pauseSel_ = (pauseSel_ + 1) % n; sfxPlay(Sfx::Scroll); }
    if (a.input.back()) { resume(); return; }
    if (a.input.accept()) {
        sfxPlay(Sfx::Confirm);
        if (pauseSel_ == 0) resume();
        else if (pauseSel_ == 1) restartSong();
        else if (pauseSel_ == 2) openOptions();
        else exitToMenu();
    }
}

void PlayState::die() {
    App& a = app();
    ++a.blueballed;
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

void PlayState::stopRendering() {
    if (!rendering_) return;
    rendering_ = false;
    app().stopRender();
}

void PlayState::exitToMenu() {
    App& a = app();
    stopRendering();
    if (scripts_) scripts_->call("onDestroy");
    a.mixer.stop();
    a.mixer.clear();
    a.change(req_.story ? makeStory() : makeFreeplay());
}

void PlayState::openChartEditor() {
    App& a = app();
    stopRendering();
    if (scripts_) scripts_->call("onDestroy");
    a.mixer.stop();
    a.mixer.clear();
    sfxPlay(Sfx::Confirm);
    a.change(makeChartEditor(req_.songId, req_.diff));
}

// Options from the pause menu: leaving them starts this song again, as in Psych.
void PlayState::openOptions() {
    App& a = app();
    stopRendering();
    if (scripts_) scripts_->call("onDestroy");
    a.mixer.stop();
    a.mixer.clear();
    a.optionsReturn = std::make_unique<PlayRequest>(req_);
    a.change(makeOptions());
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
    stopRendering();  // the video ends with the song
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
    r.totalNotes = (long long)session_.totalPlayerNotes;
    r.opponentHits = session_.opponentHits;
    r.opponentNotes = (long long)session_.totalOpponentNotes;
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
        for (int k = 0; k < kRatingCount; ++k) record_.windows[k] = cfg_.windows[k];
        record_.ratingOffset = cfg_.ratingOffset;
        record_.score = r.score;
        record_.acc = r.acc;
        record_.misses = r.misses;
        const std::string dir = a.userDir + "replays/";
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
        std::printf("[auto] %s (%s): score %lld, accuracy %.2f%%, misses %lld, max combo %lld, notes %llu%s\n", r.title.c_str(),
                    r.diff.c_str(), r.score, r.acc * 100.0, r.misses, r.maxCombo, (unsigned long long)session_.totalPlayerNotes,
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

void PlayState::adaptNoteGap() {
    App& a = app();
    if (a.settings.noteDetail != 0) {
        noteGap_ = 1.0f;
        return;
    }
    drawnSum_ += drawnNotes_;
    candidateSum_ += candidateNotes_;
    gapTime_ += a.dt;
    ++gapFrames_;
    if (gapTime_ < 0.5) return;
    const double fps = gapFrames_ / gapTime_;
    const double drawn = drawnSum_ / gapFrames_, cand = candidateSum_ / gapFrames_;
    drawnSum_ = candidateSum_ = gapTime_ = 0;
    gapFrames_ = 0;
    // Smooth play needs about 60 frames per second (a lower frame limit asks for less).
    double target = 60;
    if (!a.settings.vsync && a.settings.fpsCap > 0) target = std::min(target, double(a.settings.fpsCap));
    int k = 0;  // noteGap_ = 2^k
    while (k < 5 && float(1 << k) < noteGap_) ++k;
    if (fps < target * 0.85 && drawn > 600 && k < 5) {
        gapTooMuch_[k] = cand;  // this many notes were too many with this gap
        noteGap_ = float(1 << (k + 1));
    } else if (k > 0 && cand < gapTooMuch_[k - 1] * 0.6) {
        noteGap_ = float(1 << (k - 1));  // clearly fewer notes now: try the finer gap again
    }
}

void PlayState::drawLanes() {
    App& a = app();
    const Settings& s = a.settings;
    const PsychUi& ui = psychUi();
    const int K = chart_.keys;
    const float ns = noteSize();
    const double pxPerMs = 0.45 * scrollSpeed() / rate_;
    const double reach = (gfx::H * 1.5 + ns) / pxPerMs;
    drawnNotes_ = candidateNotes_ = 0;
    for (int side = 0; side < 2; ++side) {
        if (side == 0 && !s.opponentNotes) continue;  // Options: opponent notes (Psych hides their receptors and notes)
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
                const int sstate = conf > 0.01f ? 2 : pressed ? 1 : 0;
                if (sstate) ui.beginDir(dir);  // idle receptors stay grey
                const bool drawn = ui.strum(dir, c, size, sstate, alphaStrum, st.angle, a.now);
                if (sstate) ui.endDir();
                if (drawn) {
                    // the Psych Engine interface drew it
                } else if (conf > 0.01f) {
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
                ui.beginDir(dir);  // this lane's colours (Psych colour-template skins)
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
                    if (!ui.hold(dir, c.x + (nv ? nv->offsetX : 0.0f), y0, y1, size * 0.3f, al))
                        gfx::drawSustain(dir, c.x + (nv ? nv->offsetX : 0.0f), y0, y1, size * 0.3f, al * 0.85f + 0.15f * (al > 0), true);
                }
                // Notes less than a pixel apart look the same drawn once, so a
                // lane never draws more notes than it has pixels (spam charts
                // stay smooth); with Note detail "Adaptive" the gap grows
                // while the frame rate is too low. Every note is still played
                // and counted.
                const float minGap = noteGap_;
                float lastY = 1e9f, lastY1 = 1e9f;
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
                    if (std::fabs(y - lastY1) >= 1.0f || n.len > 0) {
                        ++candidateNotes_;  // what every pixel would draw
                        lastY1 = y;
                    }
                    if (std::fabs(y - lastY) < minGap && n.len <= 0) continue;
                    lastY = y;
                    ++drawnNotes_;
                    const float al = (n.state == kMissed ? 0.35f : 1.0f) * alphaStrum * (nv ? nv->alpha : 1.0f);
                    if (al <= 0.002f) continue;
                    const Vector2 p{c.x + (nv ? nv->offsetX : 0.0f), y + (nv ? nv->offsetY : 0.0f)};
                    if (n.kind == kMine) drawMine(p, size, al, st.angle);
                    else if (!ui.note(dir, p, size, al, st.angle)) gfx::drawNote(dir, p, size, al, 1.0f, st.angle);
                }
                ui.endDir();
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

// ------------------------------------------------------- Psych HUD objects --

scene::Sprite* PlayState::hudObject(const char* name) const {
    if (!psych_ || !scene_) return nullptr;
    auto it = scene_->hud.find(name);
    return it == scene_->hud.end() ? nullptr : it->second.get();
}

Vector2 PlayState::hudMoved(const char* name) const {
    const scene::Sprite* o = hudObject(name);
    auto it = hudHome_.find(name);
    if (!o || it == hudHome_.end()) return {0, 0};
    return {o->x - it->second.x, o->y - it->second.y};
}

// Psych 0.7.3's places: healthBar.png centred with its top at 89% of the
// height (11% in downscroll), the score text 40 pixels under it (36 in 0.6),
// the time text 19 pixels down (FlxG.height - 44 in downscroll) with the time
// bar a quarter of its height lower, BOTPLAY 55 under the time bar (78 over
// it in downscroll).
PlayState::PsychHudLayout PlayState::psychHudLayout() const {
    const Settings& s = app().settings;
    const PsychUi& ui = psychUi();
    PsychHudLayout L;
    const Vector2 hb = ui.healthBarSize();
    const bool picture = hb.x > 0;
    const Vector2 size = picture ? hb : Vector2{601, 19};
    L.bar = {(gfx::W - size.x) * 0.5f, gfx::H * (s.downscroll ? 0.11f : 0.89f), size.x, size.y};
    L.inset = picture ? ui.barInset() : 3.0f;
    L.scoreY = L.bar.y + (picture && !ui.barOnTop() ? 36.0f : 40.0f);
    L.timeTxtY = s.downscroll ? gfx::H - 44.0f : 19.0f;
    L.timeBarY = L.timeTxtY + (ui.lineHeight(32) + 4) * 0.25f;
    L.botY = s.downscroll ? L.timeBarY - 78 : L.timeBarY + 55;
    L.timeBarSize = ui.timeBarSize();
    return L;
}

void PlayState::initHudObjects() {
    const Settings& s = app().settings;
    const PsychHudLayout L = psychHudLayout();
    auto put = [&](const char* name, float x, float y, float w, float h) {
        scene::Sprite* o = hudObject(name);
        if (!o) return;
        o->x = x;
        o->y = y;
        o->width = o->frameW = w;
        o->height = o->frameH = h;
        o->centerOrigin();
        hudHome_[name] = {x, y};
    };
    put("healthBar", L.bar.x, L.bar.y, L.bar.width, L.bar.height);
    put("healthBarBG", L.bar.x, L.bar.y, L.bar.width, L.bar.height);
    put("iconP1", 0, L.bar.y - 75, 150, 150);
    put("iconP2", 0, L.bar.y - 75, 150, 150);
    put("scoreTxt", 0, L.scoreY, gfx::W, psychUi().lineHeight(20) * 2 + 4);
    put("timeBar", (gfx::W - L.timeBarSize.x) * 0.5f, L.timeBarY, L.timeBarSize.x, L.timeBarSize.y);
    put("timeBarBG", (gfx::W - L.timeBarSize.x) * 0.5f, L.timeBarY, L.timeBarSize.x, L.timeBarSize.y);
    put("timeTxt", 42 + gfx::W * 0.5f - 248, L.timeTxtY, 400, psychUi().lineHeight(32) + 4);
    put("botplayTxt", 400, L.botY, gfx::W - 800, psychUi().lineHeight(32) + 4);
    // What Psych's options do to them (Hide HUD, health bar opacity, time bar type).
    for (const char* n : {"healthBar", "iconP1", "iconP2"})
        if (scene::Sprite* o = hudObject(n)) {
            o->visible = !s.hideHud;
            o->alpha = s.healthBarAlpha;
        }
    if (scene::Sprite* o = hudObject("healthBarBG")) o->visible = !s.hideHud;
    if (scene::Sprite* o = hudObject("scoreTxt")) o->visible = !s.hideHud;
    if (scene::Sprite* o = hudObject("iconP1")) o->flipX = true;
    for (const char* n : {"timeBar", "timeTxt"})
        if (scene::Sprite* o = hudObject(n)) {
            o->visible = s.timeBar != 3;
            o->alpha = 0;  // until the song starts
        }
    if (scene::Sprite* o = hudObject("timeBarBG")) o->visible = s.timeBar != 3;
    if (scene::Sprite* o = hudObject("timeTxt"); o && s.timeBar == 2)
        o->text.text = timeEngineText_ = songNameShown_.empty() ? chart_.title : songNameShown_;
    if (scene::Sprite* o = hudObject("botplayTxt")) {
        o->visible = cfg_.botplay || replaying_;
        o->text.text = botEngineText_ = replaying_ ? "REPLAY" : "BOTPLAY";
    }
    updateHudObjects(0);
    updateScoreText(false);
}

// What Psych's PlayState.update does to the HUD objects before onUpdatePost.
void PlayState::updateHudObjects(double dt) {
    const Settings& s = app().settings;
    const double now = app().now;
    // updateIconsScale: back to 1 (exp(-9 t)); updateIconsPosition: beside the
    // bar's split (health 50% = the middle).
    scene::Sprite* p1 = hudObject("iconP1");
    scene::Sprite* p2 = hudObject("iconP2");
    const float k = float(std::exp(-dt * 9.0 * rate_));
    for (scene::Sprite* ic : {p1, p2})
        if (ic) ic->scaleX = ic->scaleY = 1.0f + (ic->scaleX - 1.0f) * k;
    if (const scene::Sprite* bar = hudObject("healthBar")) {
        const float inset = psychUi().healthBarSize().x > 0 ? psychUi().barInset() : 3.0f;
        const float hp = healthShown_ / 2.0f, leftFrac = playerLeft_ ? hp : 1.0f - hp;
        const float center = bar->x + inset + (bar->width - 2 * inset) * leftFrac;
        if (p1) p1->x = iconHomeX_[1] = center + (150 * p1->scaleX - 150) * 0.5f - 26;
        if (p2) p2->x = iconHomeX_[0] = center - 150 * p2->scaleX * 0.5f - 52;
    }
    // doScoreBop: 1.075, back to 1 in 0.2 s.
    if (scene::Sprite* st = hudObject("scoreTxt"); st && s.scoreZoom) {
        const double t = now - scoreBopAt_;
        if (t >= 0 && t < 0.2) {
            st->scaleX = st->scaleY = float(1.075 - 0.075 * t / 0.2);
            scoreBopping_ = true;
        } else if (scoreBopping_) {
            st->scaleX = st->scaleY = 1;
            scoreBopping_ = false;
        }
    }
    // The time bar and text fade in for 0.5 s when the song starts (circOut);
    // the time text is written every frame.
    if (songStartAt_ >= 0 && !timeFadeDone_) {
        const float t = std::min(1.0f, float(now - songStartAt_) / 0.5f);
        const float in = std::sqrt(std::max(0.0f, 1.0f - (t - 1.0f) * (t - 1.0f)));
        for (const char* n : {"timeBar", "timeTxt"})
            if (scene::Sprite* o = hudObject(n)) o->alpha = in;
        timeFadeDone_ = t >= 1.0f;
    }
    if (scene::Sprite* tt = hudObject("timeTxt"); tt && s.timeBar != 2) {
        const double cur = std::max(0.0, songMs_);
        const double calc = s.timeBar == 1 ? cur : std::max(0.0, shownLengthMs() - cur);
        tt->text.text = timeEngineText_ = fmtTime(std::floor(calc / 1000.0));
    }
    // BOTPLAY: fading out and back in over a second, then a second fully shown.
    if (scene::Sprite* bt = hudObject("botplayTxt"); bt && bt->visible) {
        botplaySine_ += 180.0 * dt;
        bt->alpha = std::clamp(1.0f - float(std::sin(kPi * botplaySine_ / 180.0)), 0.0f, 1.0f);
    }
}

std::string PlayState::psychScoreLine() const {
    // Psych 0.7.3's updateScore: "Score: 1200 | Misses: 0 | Rating: Sick! (98.5%) - SFC".
    const long long judged = session_.judged() + session_.misses;
    std::string line = "Score: " + std::to_string(session_.score) + " | Misses: " + std::to_string(session_.misses) + " | Rating: " +
                       (ratingNameOverride_.empty() ? psychRatingName(session_.accuracy(), judged) : ratingNameOverride_);
    if (judged > 0) {
        // CoolUtil.floorDecimal(percent, 2), written as Haxe writes a Float.
        std::string pct = TextFormat("%.2f", std::floor(session_.accuracy() * 10000.0) / 100.0);
        while (!pct.empty() && pct.back() == '0') pct.pop_back();
        if (!pct.empty() && pct.back() == '.') pct.pop_back();
        line += " (" + pct + "%) - " + (ratingFcOverride_.empty() ? psychFcLabel(session_) : ratingFcOverride_);
    }
    return line;
}

void PlayState::updateScoreText(bool miss) {
    scene::Sprite* st = hudObject("scoreTxt");
    if (!st) return;
    // On spam charts the scripts hear it once a frame at most (Options: Lua note callbacks).
    const bool calls = scripts_ && app().settings.luaNoteCallbacks;
    if (calls && scripts_->callStops("preUpdateScore", {scene::Value(miss)})) return;
    st->text.text = scoreEngineText_ = psychScoreLine();
    if (calls) scripts_->call("onUpdateScore", {scene::Value(miss)});
}

// Draws what `draw` draws turned by `angle` degrees and scaled around `c`, as
// Flixel turns and scales a sprite around its middle.
template <class F>
void turned(Vector2 c, float angle, float sx, float sy, F&& draw) {
    const bool t = angle != 0 || sx != 1 || sy != 1;
    if (t) {
        rlDrawRenderBatchActive();
        rlPushMatrix();
        rlTranslatef(c.x, c.y, 0);
        if (angle != 0) rlRotatef(angle, 0, 0, 1);
        rlScalef(sx, sy, 1);
        rlTranslatef(-c.x, -c.y, 0);
    }
    draw();
    if (t) {
        rlDrawRenderBatchActive();
        rlPopMatrix();
    }
}

// The HUD. With the Psych Engine interface it is laid out as Psych 0.7.3's
// PlayState does it (psychHudLayout); in Psych songs each piece is drawn where
// its object is (scripts move, turn, scale and fade them). The game's own look
// otherwise, moved as far as scripts moved the objects.
void PlayState::drawHud() {
    App& a = app();
    const Settings& s = a.settings;
    const double now = a.now;
    const PsychUi& ui = psychUi();
    const bool look = ui.active();             // the Psych Engine interface
    const bool hideOwn = s.hideHud && !psych_;  // in Psych songs the objects carry Hide HUD
    const bool psychText = look && ui.font();
    // health bar
    const Vector2 hbs = ui.healthBarSize();
    const bool psychBar = hbs.x > 0;
    const Rectangle home = psychBar ? Rectangle{(gfx::W - hbs.x) * 0.5f, gfx::H * (s.downscroll ? 0.11f : 0.89f), hbs.x, hbs.y}
                                    : Rectangle{335, s.downscroll ? 77.0f : 635.0f, 610, 28};
    Vector2 barMoved = hudMoved("healthBar");
    if (barMoved.x == 0 && barMoved.y == 0) barMoved = hudMoved("healthBarBG");  // Psych 0.6 scripts move the picture
    const Rectangle frame{home.x + barMoved.x, home.y + barMoved.y, home.width, home.height};
    const float inset = psychBar ? ui.barInset() : 5.0f;
    const float bx = frame.x + inset, by = frame.y + inset, bw = frame.width - 2 * inset, bh = frame.height - 2 * inset;
    const float hp = healthShown_ / 2.0f;
    const float leftFrac = playerLeft_ ? hp : 1.0f - hp;
    const float split = bx + bw * leftFrac;  // Psych's barCenter
    // Health bar opacity: Psych songs start healthBar and the icons at that alpha.
    const float barOpacity = psych_ ? 1.0f : s.healthBarAlpha;
    const scene::Sprite* barObj = hudObject("healthBar");
    float barAlpha = 1, bgAlpha = 1;
    if (!hideOwn && hudShown("healthBar", &barAlpha)) {
        if (!hudShown("healthBarBG", &bgAlpha)) bgAlpha = 0;  // Psych 0.6 scripts fade the picture on its own
        barAlpha *= barOpacity;
        if (barAlpha > 0.002f)
            turned({frame.x + frame.width * 0.5f, frame.y + frame.height * 0.5f}, barObj ? barObj->angle : 0.0f, barObj ? barObj->scaleX : 1.0f,
                   barObj ? barObj->scaleY : 1.0f, [&] {
                       if (ui.healthBar(frame, leftColor_, rightColor_, leftFrac, barAlpha, barAlpha * bgAlpha)) return;
                       DrawRectangleRounded(frame, 0.6f, 8, fade(Color{8, 4, 14, 230}, barAlpha * bgAlpha));
                       DrawRectangleRec({bx, by, split - bx, bh}, fade(leftColor_, barAlpha));
                       DrawRectangleRec({split, by, bx + bw - split, bh}, fade(rightColor_, barAlpha));
                       DrawRectangleRec({bx, by, bw, bh * 0.35f}, Color{255, 255, 255, (unsigned char)(40 * barAlpha)});
                   });
    }
    const bool playerLosing = session_.health < 0.4, oppLosing = session_.health > 1.6;
    // Psych: 150-pixel icons centred on the bar's top edge, beside the split,
    // growing to 1.2 on each beat and easing back (exp(-9 t)). In Psych songs
    // the icon objects carry that (updateHudObjects).
    float bop = 1;
    if (look && songStarted_ && !psych_) {
        const double beat = std::floor(chart_.beatAt(songMs_));
        if (beat >= 1) bop = 1.0f + 0.2f * float(std::exp(-0.009 * std::max(0.0, songMs_ - chart_.msAtBeat(beat))));
    }
    const float isz = 86.0f * (1.0f + 0.12f * beatPulse_);
    for (int i = 0; i < 2 && !hideOwn; ++i) {  // 0 = left (opponent side), 1 = right
        const char* name = i == 0 ? "iconP2" : "iconP1";
        float ia = 1;
        if (!hudShown(name, &ia)) continue;
        ia *= barOpacity;
        if (ia <= 0.002f) continue;
        const scene::Sprite* io = hudObject(name);
        Vector2 pos;
        float scale = bop;
        if (io && look) {  // where the object is: its middle is 75 pixels in, whatever its scale
            pos = {io->x + 75, io->y + 75};
            scale = io->scaleX;
        } else {
            pos = look ? Vector2{i == 0 ? split - 75.0f * bop + 23.0f : split + 75.0f * bop - 26.0f, frame.y}
                       : Vector2{split + (i == 0 ? -36.0f : 36.0f), by + bh * 0.5f};
            if (io) {
                pos.x += io->x - iconHomeX_[i] - barMoved.x;
                pos.y += io->y - hudHome_[name].y - barMoved.y;
            }
        }
        const float angle = io ? io->angle : 0.0f;
        const bool flip = io ? io->flipX : i == 1;
        const bool losing = i == 0 ? (playerLeft_ ? playerLosing : oppLosing) : (playerLeft_ ? oppLosing : playerLosing);
        if (psych_ && hasPsychIcon_[i] && iconTex_[i].id) {
            const int frames = std::max(1, iconFrames_[i]);
            const float fw = float(iconTex_[i].width) / float(frames), fh = float(iconTex_[i].height);
            const int fi = losing && frames > 1 ? 1 : 0;
            const float dw = look ? fw * scale : isz * 1.6f, dh = dw * fh / fw;
            Rectangle src{fw * float(fi), 0, flip ? -fw : fw, fh};
            DrawTexturePro(iconTex_[i], src, {pos.x, pos.y, dw, dh}, {dw * 0.5f, dh * 0.5f}, angle, fade(WHITE, ia));
        } else {
            const CharacterState& cs = i == 0 ? left_ : right_;
            turned(pos, angle, 1, 1, [&] { drawIcon(cs.id, cs.tint, pos, look ? 115.0f * scale : isz, losing, now); });
        }
    }
    // score line (Psych: 40 pixels under the bar's top; 36 in 0.6)
    float scoreAlpha = 1;
    const bool scoreShown = !hideOwn && hudShown("scoreTxt", &scoreAlpha);
    const scene::Sprite* so = hudObject("scoreTxt");
    const Vector2 scoreMoved = hudMoved("scoreTxt");
    const float scoreTop = home.y + (psychBar && !ui.barOnTop() ? 36.0f : 40.0f) + scoreMoved.y;
    const float scoreX = gfx::W * 0.5f + scoreMoved.x;
    if (scoreShown) {
        std::string line = "Score " + fmtThousands(session_.score) + "   |   Misses " + fmtThousands(session_.misses) +
                           "   |   " + TextFormat("%.2f%%", session_.accuracy() * 100.0) + " (" +
                           (ratingNameOverride_.empty() ? session_.rank() : ratingNameOverride_) + ")";
        const std::string cl = ratingFcOverride_.empty() ? session_.clearLabel() : ratingFcOverride_;
        if (!cl.empty()) line += "   |   " + cl;
        if (look) line = so ? scoreEngineText_ : psychScoreLine();  // Psych Engine's own score line
        if (so && !so->text.text.empty() && so->text.text != scoreEngineText_) line = so->text.text;  // a script wrote it
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        // Long numbers (spam charts) shrink the line to fit instead of running off screen.
        constexpr float kMaxLineW = gfx::W - 40.0f;
        turned({scoreX, scoreTop + 12}, so ? so->angle : 0.0f, 1, 1, [&] {
            if (psychText) {
                // vcr.ttf 20 with a black outline; with "Score text zoom on hit" it
                // grows to 1.075 on each note you hit and eases back in 0.2 s, around
                // the middle of its box (two lines high: Psych ends it with a line break).
                float size = 20;
                const float w = ui.textWidth(line, size);
                if (w > kMaxLineW) size *= kMaxLineW / w;
                float zoom = 1;
                if (so) zoom = so->scaleX;
                else if (s.scoreZoom && now - scoreBopAt_ < 0.2) zoom = 1.075f - 0.075f * float((now - scoreBopAt_) / 0.2);
                const float mid = scoreTop + ui.lineHeight(size) + 2;
                ui.text(line, scoreX, mid + (scoreTop + 2 - mid) * zoom - 2, size * zoom, fade(WHITE, scoreAlpha), 0, 1.25f * zoom);
            } else {
                float size = 24;
                const float w = gfx::measure(FontId::Ui, line, size).x;
                if (w > kMaxLineW) size *= kMaxLineW / w;
                gfx::text(FontId::Ui, line, scoreX, home.y + 5 + 36 + scoreMoved.y, size, fade(WHITE, scoreAlpha), Align::Center, 2.0f);
            }
        });
    }
    // Note counter (Options, Note counter): notes hit so far out of the chart's
    // total, for your side, or both sides added up the way H-Slice shows them
    // ("opponent + yours = both"); it follows the score line.
    if (s.noteCounter > 0 && scoreShown) {
        std::string line;
        if (s.noteCounter == 2) {
            const long long opp = session_.opponentHits, mine = session_.judged();
            line = "Notes " + fmtThousands(opp) + " + " + fmtThousands(mine) + " = " + fmtThousands(opp + mine) + " / " +
                   fmtThousands((long long)(session_.totalPlayerNotes + session_.totalOpponentNotes));
        } else {
            line = "Notes " + fmtThousands(session_.judged()) + " / " + fmtThousands((long long)session_.totalPlayerNotes);
        }
        if (s.noteCounter == 2)  // each side on its own: opponent / yours
            line += "   |   NPS " + fmtThousands(npsOpp_) + " / " + fmtThousands(nps_) + " (max " + fmtThousands(maxNpsOpp_) + " / " +
                    fmtThousands(maxNps_) + ")";
        else
            line += "   |   NPS " + fmtThousands(nps_) + " (max " + fmtThousands(maxNps_) + ")";
        if (cfg_.practice && !replaying_) line += "   |   PRACTICE MODE";
        if (psychText) {
            float size = 16;
            const float w = ui.textWidth(line, size);
            if (w > gfx::W - 40.0f) size *= (gfx::W - 40.0f) / w;
            ui.text(line, scoreX, scoreTop + 22, size, fade(WHITE, scoreAlpha), 0, 1.0f);
        } else {
            float size = 18;
            const float w = gfx::measure(FontId::Ui, line, size).x;
            if (w > gfx::W - 40.0f) size *= (gfx::W - 40.0f) / w;
            gfx::text(FontId::Ui, line, scoreX, home.y + 5 + 60 + scoreMoved.y, size, fade(kIvory, 0.9f * scoreAlpha), Align::Center, 1.5f);
        }
    }
    // Psych's time text: vcr.ttf 32 in a 400-pixel box at x 434 (STRUM_X +
    // FlxG.width / 2 - 248), 19 pixels down (FlxG.height - 44 in downscroll);
    // the time bar a quarter of the text's height under its top.
    const PsychHudLayout L = psychHudLayout();
    if (const scene::Sprite* bo = hudObject("botplayTxt")) {
        // Psych songs: the object (BOTPLAY, or what a script wrote and showed).
        float botAlpha = 1;
        const bool engineText = bo->text.text == botEngineText_;
        if (hudShown("botplayTxt", &botAlpha) && (s.botplayText || !engineText) && !bo->text.text.empty()) {
            const Vector2 c{bo->x + bo->width * 0.5f, bo->y};
            turned({c.x, c.y + 16}, bo->angle, bo->scaleX, bo->scaleY, [&] {
                if (!(psychText && ui.text(bo->text.text, c.x, c.y, 32, fade(WHITE, botAlpha), 0, 1.25f)))
                    gfx::text(FontId::Title, bo->text.text, c.x, c.y + 66, 40, fade(kGold, botAlpha), Align::Center, 2.0f);
            });
        }
    } else if ((cfg_.botplay || replaying_) && s.botplayText) {
        const char* label = replaying_ ? "REPLAY" : "BOTPLAY";
        // Psych: under the time bar (above it in downscroll), fading out and
        // back in over a second, then a second fully shown.
        const float al = std::clamp(1.0f - float(std::sin(kPi * (now - loadStart_))), 0.0f, 1.0f);
        if (!(psychText && ui.text(label, gfx::W / 2, L.botY, 32, fade(WHITE, al), 0, 1.25f)))
            gfx::text(FontId::Title, label, gfx::W / 2, s.downscroll ? 560 : 150, 40, fade(kGold, 0.55f + 0.35f * std::sin(float(now) * 3.0f)),
                      Align::Center, 2.0f);
    }
    if (s.touchControls && !a.touchMode && !replaying_) {
        const float half = std::min(112.0f, 500.0f / float(chart_.keys)) * 0.5f;
        for (int l = 0; l < chart_.keys; ++l) {
            const Vector2 c = strumCenter(1, l);
            const Color col = gfx::dirColor(gfx::laneDir(chart_.keys, l));
            DrawRectangleRec({c.x - half, gfx::H * 0.55f, half * 2 - 4, gfx::H * 0.45f}, fade(col, touchDown_[l] ? 0.28f : 0.09f));
        }
    }
    if (cfg_.practice && !replaying_ && !(s.noteCounter > 0 && scoreShown))
        gfx::text(FontId::Ui, "PRACTICE MODE", gfx::W / 2, home.y + 67, 20, fade(kGold, 0.8f), Align::Center, 1.5f);
    if (rate_ != 1.0) gfx::text(FontId::Ui, TextFormat("%.2fx", rate_), bx + bw + 60, by - 4, 24, kGold, Align::Left, 1.5f);

    // song progress (Options: time left, time elapsed, song name or off)
    float tbAlpha = 1, ttAlpha = 1, tbgAlpha = 1;
    const bool tbShown = hudShown("timeBar", &tbAlpha), ttShown = hudShown("timeTxt", &ttAlpha);
    if (!hudShown("timeBarBG", &tbgAlpha)) tbgAlpha = 0;
    const scene::Sprite* tbo = hudObject("timeBar");
    const scene::Sprite* tto = hudObject("timeTxt");
    if ((psych_ || s.timeBar != 3) && (tbShown || ttShown)) {
        const float prog = float(clampv(songMs_ / std::max(1.0, shownLengthMs()), 0.0, 1.0));
        // Psych counts the song's own time (a faster rate does not shorten it);
        // the game's own look counts the real time left.
        const double div = look ? 1.0 : rate_;
        const double remain = std::max(0.0, (shownLengthMs() - songMs_) / 1000.0 / div);
        const double elapsed = std::max(0.0, songMs_ / 1000.0 / div);
        const std::string name = songNameShown_.empty() ? chart_.title : songNameShown_;
        const bool songName = s.timeBar == 2;
        std::string timeText = songName ? name : fmtTime(s.timeBar == 1 ? elapsed : remain);
        if (tto && !tto->text.text.empty() && (look || tto->text.text != timeEngineText_)) timeText = tto->text.text;
        const Vector2 tbMoved = hudMoved("timeBar"), ttMoved = hudMoved("timeTxt");
        const bool custom = timeBarBack_.a || timeBarFill_.a;  // setTimeBarColors()
        if (!custom && look) {
            // Psych: hidden until the song starts, then fading in for 0.5 s
            // (circOut); in Psych songs the objects carry that.
            float in = 1;
            if (!psych_) {
                const float t = songStartAt_ < 0 ? 0.0f : std::min(1.0f, float(now - songStartAt_) / 0.5f);
                in = std::sqrt(std::max(0.0f, 1.0f - (t - 1.0f) * (t - 1.0f)));
            }
            const Vector2 tbs = L.timeBarSize;
            const Rectangle tframe{(gfx::W - tbs.x) * 0.5f + tbMoved.x, L.timeBarY + tbMoved.y, tbs.x, tbs.y};
            if (tbShown)
                turned({tframe.x + tframe.width * 0.5f, tframe.y + tframe.height * 0.5f}, tbo ? tbo->angle : 0.0f, tbo ? tbo->scaleX : 1.0f,
                       tbo ? tbo->scaleY : 1.0f, [&] { ui.timeBar(tframe, prog, in * tbAlpha, in * tbAlpha * tbgAlpha); });
            const float tx = 42 + gfx::W / 2 - 248 + 200 + ttMoved.x, ty = (songName ? L.timeTxtY + 3 : L.timeTxtY) + ttMoved.y;
            if (ttShown)
                turned({tx, ty + 16}, tto ? tto->angle : 0.0f, tto ? tto->scaleX : 1.0f, tto ? tto->scaleY : 1.0f, [&] {
                    if (!(psychText && ui.text(timeText, tx, ty, songName ? 24.0f : 32.0f, fade(WHITE, in * ttAlpha), 0, 2.0f)))
                        gfx::text(FontId::Ui, timeText, tx, ty, 28, fade(WHITE, in * ttAlpha), Align::Center, 2.0f);
                });
        } else {
            const float ty = (s.downscroll ? gfx::H - 26.0f : 12.0f) + tbMoved.y;
            if (tbShown) {
                DrawRectangleRounded({440 + tbMoved.x, ty, 400, 14}, 0.8f, 8, fade(timeBarBack_.a ? timeBarBack_ : Color{8, 4, 14, 200}, tbAlpha));
                DrawRectangleRounded({442 + tbMoved.x, ty + 2, 396 * prog, 10}, 0.8f, 8,
                                     fade(timeBarFill_.a ? timeBarFill_ : gfx::mix(leftColor_, WHITE, 0.3f), tbAlpha));
            }
            std::string label = name + "  -  " + req_.diff;
            if (s.timeBar != 2) label += "  -  " + fmtTime(s.timeBar == 1 ? elapsed : remain);
            if (tto && !tto->text.text.empty() && tto->text.text != timeEngineText_) label = tto->text.text;
            if (ttShown)
                gfx::text(FontId::Ui, label, gfx::W / 2 + ttMoved.x, (s.downscroll ? gfx::H - 52.0f : 26.0f) + ttMoved.y, 20,
                          fade(WHITE, 0.85f * ttAlpha), Align::Center, 1.5f);
        }
    }
}

// Rating popups and the countdown: under the notes, as Psych adds its
// comboGroup and countdown sprites before the notes.
void PlayState::drawUnderNotes() {
    App& a = app();
    const Settings& s = a.settings;
    const double now = a.now;
    const PsychUi& ui = psychUi();
    float comboAlpha = 1;
    if (!popups_.empty() && s.hitPopups && !s.hideHud && hudShown("comboGroup", &comboAlpha)) {  // Options: hit popups
        if (ui.active() && ui.ratingSize(0).x > 0) {
            // Psych's popUpScore: the rating at FlxG.width * 0.35 - 40, 60 pixels
            // over the middle, at 70%; the combo digits (three at least) at 50%,
            // 43 pixels apart, 80 pixels under the middle. Each falls with its own
            // random speed and fades out in 0.2 s: the rating a beat later, the
            // digits two beats later. Misses show nothing.
            for (const Popup& p : popups_) {
                if (p.rating < 0) continue;
                const float tau = float((now - p.t0) * rate_);
                if (tau > 2.0f * p.beat + 0.2f) continue;
                uint32_t r = p.seed;
                auto rnd = [&r] {
                    r ^= r << 13;
                    r ^= r >> 17;
                    r ^= r << 5;
                    return float(r & 0xFFFFFF) / float(0x1000000);
                };
                auto irange = [&](int lo, int hi) { return float(lo + int(rnd() * float(hi - lo + 1))); };
                const Vector2 rs = ui.ratingSize(std::min(p.rating, 3));
                if (rs.x <= 0) continue;
                const float rk = std::floor(rs.x * 0.7f) / rs.x;
                const float rvx = -irange(0, 10), rvy = -irange(140, 175);
                // Psych's comboOffset (Options: rating / combo position): x right, y up.
                const float* off = s.comboOffset;
                const float rx = gfx::W * 0.35f - 40.0f + off[0] + rvx * tau;
                const float ry = (gfx::H - rs.y) * 0.5f - 60.0f - off[1] + rvy * tau + 0.5f * 550.0f * tau * tau;
                const float ra = tau < p.beat ? 1.0f : std::max(0.0f, 1.0f - (tau - p.beat) / 0.2f);
                if (ra > 0.002f) ui.ratingAt(std::min(p.rating, 3), rx, ry, rk, ra * comboAlpha);
                std::string digits = std::to_string(std::max(0LL, p.combo));
                while (digits.size() < 3) digits.insert(digits.begin(), '0');
                const float da = tau < 2.0f * p.beat ? 1.0f : std::max(0.0f, 1.0f - (tau - 2.0f * p.beat) / 0.2f);
                for (size_t i = 0; i < digits.size(); ++i) {
                    const int d = digits[i] - '0';
                    const float ay = irange(200, 300), vy = -irange(140, 160), vx = rnd() * 10.0f - 5.0f;
                    const Vector2 ns = ui.numSize(d);
                    if (ns.x <= 0 || da <= 0.002f) continue;
                    const float nk = std::floor(ns.x * 0.5f) / ns.x;
                    const float x = gfx::W * 0.35f + 43.0f * float(i) - 90.0f + off[2] + vx * tau;
                    const float y = (gfx::H - ns.y) * 0.5f + 80.0f - off[3] + vy * tau + 0.5f * ay * tau * tau;
                    ui.numAt(d, x, y, nk, da * comboAlpha);
                }
                if (!p.timing.empty() && ra > 0.002f)
                    ui.text(p.timing, rx + rs.x * rk * 0.5f, ry - 22.0f, 20, fade(WHITE, ra * comboAlpha), 0, 1.25f);
            }
        } else {
            const float px = (s.middlescroll ? 1030.0f : gfx::W / 2) + s.comboOffset[0];
            const float py = (s.downscroll ? 400.0f : 300.0f) - s.comboOffset[1];
            const Popup& p = popups_.back();
            const float age = float(now - p.t0);
            if (age <= 0.7f) {
                const float pop = 1.0f + 0.25f * std::exp(-age * 20.0f);
                const float al = (age < 0.4f ? 1.0f : 1.0f - (age - 0.4f) / 0.3f) * comboAlpha;
                const float y = py - age * 26.0f;
                gfx::text(FontId::Title, p.text, px, y, 46 * pop, fade(p.c, al), Align::Center, 2.5f);
                if (p.combo >= 3 && p.text != "MISS")
                    gfx::text(FontId::Ui, "x" + std::to_string(p.combo), px, y + 52, 34 * pop, fade(WHITE, al), Align::Center, 2.0f);
                if (!p.timing.empty()) gfx::text(FontId::Ui, p.timing, px, y - 26, 22, fade(kIvory, al * 0.9f), Align::Center, 1.5f);
            }
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
            if (ui.active()) {
                // Psych: nothing on "three", then ready, set, go in the middle of
                // the screen, each fading out over a beat (cubeInOut).
                const float e = frac <= 0.5f ? 4.0f * frac * frac * frac : 1.0f - 4.0f * std::pow(1.0f - frac, 3.0f);
                if (b > -4) ui.countdown(b + 3, {gfx::W / 2, gfx::H / 2}, 1.0f - e);
            } else {
                gfx::glowText(FontId::Title, psych_ ? psychWords[b + 4] : words[b + 4], gfx::W / 2, 300 - 40 * sc, 96 * sc,
                              fade(kGold, 1.0f - frac), Color{255, 170, 60, 255});
            }
        }
    }
}

void PlayState::drawPause() {
    if (psychUi().active() && psychUi().font()) {
        drawPsychPause();
        return;
    }
    DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{6, 4, 12, 190});
    gfx::glowText(FontId::Title, "PAUSED", gfx::W / 2, 140, 80, kGold, Color{255, 170, 60, 255});
    gfx::text(FontId::Ui, chart_.title + "  -  " + req_.diff, gfx::W / 2, 250, 30, kIvory, Align::Center, 1.5f);
    static const char* items[kPauseItems] = {"Resume", "Restart", "Options", "Exit to menu"};
    for (int i = 0; i < kPauseItems; ++i) {
        const bool on = i == pauseSel_;
        gfx::text(FontId::Title, items[i], gfx::W / 2, 340.0f + i * 70, on ? 48.0f : 40.0f, on ? kGold : fade(kIvory, 0.6f),
                  Align::Center, 2.0f);
    }
}

// Psych's PauseSubState: the screen darkens to 60%, the song, difficulty and
// deaths slide in at the top right, and the items are Alphabet's bold letters
// in a slanted list that glides to the selection.
void PlayState::drawPsychPause() {
    App& a = app();
    const PsychUi& ui = psychUi();
    const float t = float(a.now - pausedAt_);
    auto quartInOut = [](float x) { return x <= 0.5f ? 8.0f * x * x * x * x : 1.0f - 8.0f * std::pow(1.0f - x, 4.0f); };
    DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.6f * quartInOut(saturate(t / 0.4f))));
    std::string diff = req_.diff;
    for (char& c : diff) c = char(std::toupper((unsigned char)c));
    // Psych: the song, the difficulty and the deaths at the top right, each
    // sliding 5 pixels down while it fades in (0.4 s, quartInOut, 0.3, 0.5 and
    // 0.7 s after pausing); PRACTICE MODE shown at once under them.
    const std::string info[3] = {songNameShown_.empty() ? chart_.title : songNameShown_, diff, "Blueballed: " + std::to_string(a.blueballed)};
    for (int i = 0; i < 3; ++i) {
        const float k = quartInOut(saturate((t - 0.3f - 0.2f * float(i)) / 0.4f));
        ui.text(info[i], gfx::W - 20, 15.0f + 32.0f * float(i) + 5.0f * k, 32, fade(WHITE, k), 1, 0);
    }
    if (cfg_.practice) ui.text("PRACTICE MODE", gfx::W - 20, 15.0f + 101.0f, 32, WHITE, 1, 0);
    if (cfg_.botplay) ui.text("BOTPLAY", gfx::W - 20, 15.0f + (cfg_.practice ? 133.0f : 101.0f), 32, WHITE, 1, 0);
    static const char* items[kPauseItems] = {"Resume", "Restart Song", "Options", "Exit to menu"};
    for (int i = 0; i < kPauseItems; ++i) {
        const float target = float(i) - pauseScroll_;
        const float x = 90 + target * 20, y = 320 + target * 156;
        if (y < -100 || y > gfx::H + 40) continue;
        const float al = i == pauseSel_ ? 1.0f : 0.6f;
        if (ui.alphabet(items[i], x, y, 70, al) < 0) ui.text(items[i], x, y, 64, fade(WHITE, al), -1, 2.0f);
    }
}

void PlayState::drawDead() {
    App& a = app();
    const float t = float(a.now - deadAt_);
    DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(Color{20, 0, 6, 255}, std::min(0.85f, t * 1.2f)));
    const float al = saturate(t * 1.5f - 0.3f);
    gfx::glowText(FontId::Title, "YOU HAVE FALLEN", gfx::W / 2, 220, 72, fade(Color{255, 80, 90, 255}, al),
                  Color{255, 40, 60, 255});
    gfx::text(FontId::Ui, a.touchMode ? "OK rise again      BACK leave" : "ENTER rise again      ESC leave", gfx::W / 2, 360, 34,
              fade(kIvory, al), Align::Center, 2.0f);
}

void PlayState::drawDebugOverlay() {
    App& a = app();
    std::vector<std::pair<std::string, std::string>> rows;
    auto num = [](double v) {
        std::string s = TextFormat("%.0f", v);
        for (int i = int(s.size()) - 3; i > (s[0] == '-' ? 1 : 0); i -= 3) s.insert(size_t(i), ",");
        return s;
    };
    rows.push_back({"CHART", chart_.title + " - " + chart_.difficulty + TextFormat("  |  %dK  |  speed %.2f  |  BPM %.1f", chart_.keys,
                                                                                    scrollSpeed(), chart_.bpmAt(std::max(0.0, songMs_)))});
    rows.push_back({"", "notes P " + num(double(chart_.playerNotes)) + "  O " + num(double(chart_.opponentNotes)) + "  (in the file " +
                            num(double(chart_.rawNoteCount)) + ")  sections " + num(double(chart_.sections.size()))});
    rows.push_back({"CLOCK", TextFormat("%.3f s / %.1f s  |  beat %.2f  |  step %d  |  section %d  |  rate %.2fx", songMs_ / 1000.0,
                                        chart_.lengthMs / 1000.0, chart_.beatAt(songMs_), lastStep_, lastSection_, rate_)});
    auto track = [](const char* name, const PcmPtr& p) -> std::string {
        if (!p) return "";
        if (!p->stream) return std::string(name) + TextFormat(" whole %.0f MB", double(p->data.size()) * 2 / 1048576.0);
        const PcmStream& s = *p->stream;
        const double mb = double(s.inMemory.load()) * PcmStream::kBlockFrames * s.channels * 2 / 1048576.0;
        return std::string(name) + TextFormat(" %s %zu/%zu blocks (%.0f MB)", s.windowed ? "window" : "streamed", s.inMemory.load(), s.blocks, mb);
    };
    std::string audio = track("inst", prep_.inst);
    for (const auto& [n, p] : {std::pair<const char*, const PcmPtr*>{"voices", &prep_.voiceP}, {"opponent", &prep_.voiceO}}) {
        const std::string t = track(n, *p);
        if (!t.empty()) audio += (audio.empty() ? "" : "  |  ") + t;
    }
    rows.push_back({"AUDIO", (audio.empty() ? std::string("none") : audio) + (a.mixer.waitingForAudio() ? "  |  WAITING" : "")});
    rows.push_back({"NOTES", "drawn " + num(drawnNotes_) + " of " + num(candidateNotes_) +
                                 TextFormat("  |  gap %.0f px (%s)", noteGap_, a.settings.noteDetail ? "Full" : "Adaptive")});
    rows.push_back({"", "judged " + num(double(session_.judged())) + "  combo " + num(double(session_.combo)) + "  misses " +
                            num(double(session_.misses)) + "  opponent " + num(double(session_.opponentHits)) + "  NPS " +
                            num(double(nps_))});
    rows.push_back({"FRAME", TextFormat("%d fps  |  update %.2f ms  |  draw %.2f ms  |  worst %.1f ms", int(a.fps + 0.5f), a.updateMs,
                                        a.drawMs, a.worstFrameMs)});
    if (scripts_) rows.push_back({"SCRIPTS", TextFormat("%zu loaded  |  note callbacks %s", scripts_->count(),
                                                         a.settings.luaNoteCallbacks ? "on" : "off")});
    const float x = 14, w = 900, lh = 19;
    const float y0 = gfx::H - 16 - lh * float(rows.size());
    DrawRectangleRec({x - 8, y0 - 8, w, lh * float(rows.size()) + 12}, fade(BLACK, 0.72f));
    for (size_t i = 0; i < rows.size(); ++i) {
        const float y = y0 + lh * float(i);
        if (!rows[i].first.empty()) gfx::text(FontId::Ui, rows[i].first, x, y, 16, Color{255, 210, 90, 255}, Align::Left, 1.0f);
        gfx::text(FontId::Ui, rows[i].second, x + 74, y, 16, WHITE, Align::Left, 1.0f);
    }
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
        gfx::text(FontId::Ui, a.touchMode ? "OK return" : "ENTER return", gfx::W / 2, 420, 26, fade(kIvory, 0.7f), Align::Center);
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
