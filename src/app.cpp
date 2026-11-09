#include "app.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "common.hpp"
#include "gfx.hpp"
#include "raylib.h"

namespace gh {

static App g_app;
App& app() { return g_app; }

static std::vector<double> parseList(const char* s) {
    std::vector<double> v;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ',' || *p == 0) {
            if (!cur.empty()) v.push_back(std::atof(cur.c_str()));
            cur.clear();
            if (*p == 0) break;
        } else {
            cur += *p;
        }
    }
    return v;
}

void App::change(std::unique_ptr<State> next) {
    if (next_) return;  // a transition is already running
    next_ = std::move(next);
    fadeDir_ = 1;
}

void App::startMenuMusic() {
    if (menuMusic_ || !menuPcm_) return;
    mixer.setTracks(menuPcm_, nullptr, nullptr);
    mixer.setRate(1.0);
    mixer.setLoop(true, 0, menuLoop_);
    mixer.play(0);
    menuMusic_ = true;
}

void App::stopMenuMusic() {
    if (!menuMusic_) return;
    mixer.stop();
    mixer.clear();
    menuMusic_ = false;
}

double App::menuBeat() {
    if (menuMusic_) return mixer.time(now) * menuBpm_ / 60.0;
    return (now - startTime) * menuBpm_ / 60.0;
}

void App::applyVolumes() {
    SetMasterVolume(settings.masterVol);
    mixer.setVolume(settings.musicVol);
    sfxSetVolume(settings.sfxVol);
}

void App::saveSettings() { settings.save(base + "settings.cfg"); }

void App::toggleFullscreen() {
    ToggleBorderlessWindowed();
    settings.fullscreen = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
}

int App::run(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(a, "--autoplay")) {
            autoRun.enabled = true;
            autoRun.song = next();
            if (i + 1 < argc && argv[i + 1][0] != '-') autoRun.diff = next();
        } else if (!std::strcmp(a, "--human")) autoRun.botplay = false;
        else if (!std::strcmp(a, "--state")) { autoRun.enabled = true; autoRun.state = next(); }
        else if (!std::strcmp(a, "--shots")) autoRun.shots = parseList(next());
        else if (!std::strcmp(a, "--shot-prefix")) autoRun.shotPrefix = next();
        else if (!std::strcmp(a, "--quit-after")) autoRun.quitAfter = std::atof(next());
        else if (!std::strcmp(a, "--rate")) mods.rate = clampv(std::atof(next()), 0.5, 2.0);
        else if (!std::strcmp(a, "--mirror")) mods.mirror = true;
        else if (!std::strcmp(a, "--base")) base = next();
        else if (!std::strcmp(a, "--editor")) {
            autoRun.enabled = true;
            autoRun.editorSong = next();
            if (i + 1 < argc && argv[i + 1][0] != '-') autoRun.diff = next();
        } else if (!std::strcmp(a, "--editor-export")) autoRun.editorExport = next();
        else if (!std::strcmp(a, "--list-songs")) listSongs = true;
    }
    if (listSongs) {
        // Prints what the song library finds (no window): --list-songs [--base dir]
        if (base.empty()) base = std::string(GetWorkingDirectory()) + "/";
        if (base.back() != '/') base += '/';
        SetTraceLogLevel(LOG_WARNING);
        library.scan(base);
        for (const SongEntry& e : library.songs()) {
            if (e.builtin) continue;
            std::printf("%s\n  title: %s | %s%s\n  audio: %s\n", e.id.c_str(), e.title.c_str(), e.subtitle.c_str(),
                        e.autoChart ? " | AutoChart" : "", e.inst.empty() ? "(none)" : e.inst.c_str());
            for (size_t i = 0; i < e.diffs.size(); ++i)
                std::printf("  [%s] %s\n", e.diffs[i].c_str(), i < e.chartPaths.size() ? e.chartPaths[i].c_str() : "");
            if (e.assetRoots.size() > 1)
                for (const std::string& r : e.assetRoots) std::printf("  root %s\n", r.c_str());
        }
        std::printf("%s\n%s", library.report().c_str(), library.installReport().c_str());
        return 0;
    }

    if (base.empty()) base = GetApplicationDirectory();
    if (!FileExists((base + "assets/fonts/Rajdhani-Bold.ttf").c_str())) {
        std::string cwd = std::string(GetWorkingDirectory()) + "/";
        if (FileExists((cwd + "assets/fonts/Rajdhani-Bold.ttf").c_str())) base = cwd;
    }
    settings.load(base + "settings.cfg");
    scores.load(base + "scores.cfg");

    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT;
    if (settings.vsync) flags |= FLAG_VSYNC_HINT;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 720, "God's Hand of Principality and Demise");
    SetExitKey(KEY_NULL);
    SetWindowMinSize(640, 360);
    if (settings.fullscreen && !autoRun.enabled) ToggleBorderlessWindowed();

    InitAudioDevice();
    mixer.init();
    sfxInit();
    applyVolumes();
    gfx::init(base);
    library.scan(base, [](const std::string& zip, double f) {
        // Unpacking a big mod pack takes a while: keep the window alive.
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
    });

    menuBpm_ = menuTheme().bpm;
    menuJob_ = std::async(std::launch::async, [this] { return library.menuMusic(&menuLoop_); });

    startTime = now = nowSec();
    if (autoRun.enabled && !autoRun.song.empty()) {
        PlayRequest r;
        r.songId = autoRun.song.find(':') == std::string::npos ? "builtin:" + autoRun.song : autoRun.song;
        r.diff = autoRun.diff;
        mods.botplay = autoRun.botplay;
        state_ = makePlay(r);
    } else if (!autoRun.editorSong.empty()) state_ = makeChartEditor(autoRun.editorSong, autoRun.diff);
    else if (autoRun.state == "menu") state_ = makeMainMenu();
    else if (autoRun.state == "freeplay") state_ = makeFreeplay();
    else if (autoRun.state == "story") state_ = makeStory();
    else if (autoRun.state == "options") state_ = makeOptions();
    else if (autoRun.state == "credits") state_ = makeCredits();
    else if (autoRun.state == "calibration") state_ = makeCalibration();
    else state_ = makeTitle();
    state_->enter();

    double last = nowSec(), nextFrame = last;
    while (!quit && !WindowShouldClose()) {
        PollInputEvents();
        now = nowSec();
        input.poll(now);
        const int cap = settings.vsync ? 0 : settings.fpsCap;
        if (cap > 0 && now < nextFrame) {
            const double wait = nextFrame - now;
            if (wait > 0.0012) std::this_thread::sleep_for(std::chrono::microseconds(500));
            else std::this_thread::yield();
            continue;
        }
        if (cap > 0) {
            nextFrame += 1.0 / cap;
            if (now - nextFrame > 0.1) nextFrame = now;
        }
        dt = std::min(0.1, now - last);
        last = now;
        if (dt > 0) fps = fps * 0.95f + float(1.0 / dt) * 0.05f;
        frame();
    }

    state_.reset();
    next_.reset();
    waitForBackgroundJobs();
    mixer.shutdown();
    sfxShutdown();
    gfx::shutdown();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

void App::frame() {
    input.beginFrame(dt);
    if (input.pressed(KEY_F11) || (input.pressed(KEY_ENTER) && (input.isDown(KEY_LEFT_ALT) || input.isDown(KEY_RIGHT_ALT))))
        toggleFullscreen();

    if (!menuPcm_ && menuJob_.valid() &&
        menuJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        menuPcm_ = menuJob_.get();
    if (state_->wantsMenuMusic() && !fadeDir_) startMenuMusic();

    if (fadeDir_ == 0) state_->update(dt);
    if (fadeDir_ > 0) {
        fade_ += float(dt / 0.22);
        if (fade_ >= 1.0f) {
            fade_ = 1.0f;
            state_ = std::move(next_);
            input.clear();
            state_->enter();
            fadeDir_ = -1;
        }
    } else if (fadeDir_ < 0) {
        state_->update(dt);
        fade_ -= float(dt / 0.22);
        if (fade_ <= 0.0f) { fade_ = 0; fadeDir_ = 0; }
    }

    BeginDrawing();
    ClearBackground(BLACK);
    state_->draw();
    gfx::beginCanvas();
    if (fade_ > 0) DrawRectangleRec({-10, -10, gfx::W + 20, gfx::H + 20}, gfx::fade(BLACK, fade_));
    if (settings.showFps) gfx::text(gfx::FontId::Ui, TextFormat("%d FPS", int(fps + 0.5f)), 8, 4, 20, WHITE, gfx::Align::Left, 1.5f);
    gfx::endCanvas();
    gfx::drawLetterbox();
    EndDrawing();

    if (autoRun.enabled) {
        const double t = now - startTime;
        if (shotIndex_ < autoRun.shots.size() && t >= autoRun.shots[shotIndex_]) {
            TakeScreenshot(TextFormat("%s_%02d.png", autoRun.shotPrefix.c_str(), int(shotIndex_)));
            std::printf("[auto] screenshot %zu at %.2fs\n", shotIndex_, t);
            ++shotIndex_;
        }
        if (autoRun.quitAfter > 0 && t >= autoRun.quitAfter) quit = true;
    }
    SwapScreenBuffer();
}

// ------------------------------------------------------- shared visuals --

float menuPulse() {
    const double b = app().menuBeat();
    const double frac = b - std::floor(b);
    return float(std::exp(-frac * 6.0));
}

void drawMenuBackground(double now, unsigned tintHex, float pulse) {
    const Color tint = gfx::rgb(tintHex);
    gfx::gradientRect({-20, -20, gfx::W + 40, gfx::H + 40}, gfx::mix(Color{14, 10, 34, 255}, tint, 0.18f),
                      gfx::mix(Color{40, 20, 60, 255}, tint, 0.35f));
    const float t = float(now);
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < 12; ++i) {
        const float a = t * 0.04f + i * 6.2831853f / 12.0f;
        const Vector2 c{640, 820};
        gfx::tri(c, {c.x + std::cos(a - 0.06f) * 1400, c.y + std::sin(a - 0.06f) * 1400},
                 {c.x + std::cos(a + 0.06f) * 1400, c.y + std::sin(a + 0.06f) * 1400}, gfx::fade(tint, 0.05f));
    }
    EndBlendMode();
    gfx::glow({640, 760}, 520 + 40 * pulse, gfx::fade(tint, 0.35f + 0.15f * pulse));
    Rng rng(77);
    for (int i = 0; i < 90; ++i) {
        const float x = float(rng.uniform() * gfx::W);
        const float sp = float(10 + rng.uniform() * 30);
        const float y = std::fmod(float(rng.uniform() * gfx::H) - t * sp + gfx::H * 4, gfx::H + 40) - 20;
        DrawCircleV({x, y}, float(1 + rng.uniform() * 2), gfx::fade(WHITE, float(0.2 + 0.5 * rng.uniform())));
    }
}

}  // namespace gh
