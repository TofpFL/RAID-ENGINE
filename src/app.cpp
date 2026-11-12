#include "app.hpp"
#include "psych_ui.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <typeinfo>

#include "chart_stream.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "platform.hpp"
#include "raylib.h"

namespace gh {

static App* g_app = nullptr;
App& app() { return *g_app; }

int App::start(int argc, char** argv) {
    delete g_app;
    g_app = new App();
    return g_app->run(argc, argv);
}

// Android log: which screen is open (the emulator test follows the game by it).
static void logScreen(const State& s) { platform::log(std::string("screen ") + typeid(s).name()); }

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
    platform::rememberVolume(effectiveVolume());
    if (!platform::inBackground()) SetMasterVolume(effectiveVolume());
    mixer.setVolume(settings.musicVol);
    sfxSetVolume(settings.sfxVol);
}

void App::saveSettings() { settings.save(base + "settings.cfg"); }

void App::volumeKeys() {
    if (platform::kAndroid) return;  // phones have their own volume buttons
    auto hit = [&](std::initializer_list<int> keys) {
        for (int k : keys)
            if (input.pressed(k) && !state_->keyReserved(k)) return true;
        return false;
    };
    int step = 0;
    bool mute = false;
    if (hit({KEY_EQUAL, KEY_KP_ADD})) step = 1;
    else if (hit({KEY_MINUS, KEY_KP_SUBTRACT})) step = -1;
    else if (hit({KEY_ZERO, KEY_KP_0})) mute = true;
    if (!step && !mute) return;
    if (mute) {
        settings.muted = !settings.muted;
    } else {
        settings.muted = false;
        settings.masterVol = clampv(float(std::round(settings.masterVol * 10.0f + float(step)) / 10.0f), 0.0f, 1.0f);
    }
    applyVolumes();
    saveSettings();
    volumeShownAt_ = now;
    if (!settings.muted) sfxPlay(Sfx::Scroll);
}

// Flixel's sound tray: ten bars at the top of the screen for a moment.
void App::drawVolumeTray() {
    const double t = now - volumeShownAt_;
    if (t < 0 || t > 1.8) return;
    const float slide = t < 1.5 ? 0.0f : float((t - 1.5) / 0.3);  // slides back up
    const float w = 200, h = 64, x = gfx::W / 2 - w / 2, y = 6 - slide * (h + 10);
    DrawRectangleRounded({x, y, w, h}, 0.2f, 6, Color{0, 0, 0, 200});
    const int level = settings.muted ? 0 : int(std::lround(settings.masterVol * 10));
    for (int i = 0; i < 10; ++i) {
        const float bh = 6 + i * 2.6f, bx = x + 22 + i * 16.5f, by = y + 38 - bh;
        DrawRectangleRec({bx, by, 12, bh}, i < level ? WHITE : Color{255, 255, 255, 60});
    }
    gfx::text(gfx::FontId::Ui, settings.muted ? "MUTED" : TextFormat("VOLUME %d%%", level * 10), gfx::W / 2, y + 42, 18,
              WHITE, gfx::Align::Center, 1.0f);
}

void App::applyPsychUi() {
    PsychUi& ui = psychUi();
    ui.unload();
    psychReport.clear();
    psychFolderInUse.clear();
    if (settings.psychUi) {
        // No folder chosen (or it is gone): the most complete Psych Engine /
        // JS Engine copy already on disk, such as an engine installed as a mod pack.
        std::string folder = settings.psychFolder;
        psychAutomatic = folder.empty() || !DirectoryExists(folder.c_str());
        if (psychAutomatic) {
            const std::vector<PsychFolder> found = findPsychFolders(base);
            folder = found.empty() ? std::string() : found.front().path;
            platform::log(found.empty() ? std::string("no Psych Engine copy found")
                                        : "Psych Engine interface from " + folder + " (" + std::to_string(found.size()) + " copies found)");
        }
        psychFolderInUse = folder;
        if (folder.empty()) psychReport = "no Psych Engine or JS Engine copy found among your mods";
        else if (ui.load(folder, psychReport) > 0) ui.setActive(true);
        else psychReport = "nothing of the Psych Engine interface found there";
    }
    ui.applySounds();
    // Menu music: Psych's freakyMenu while that interface is on, ours otherwise.
    const bool wantPsych = ui.active() && !ui.menuMusic().empty();
    if (wantPsych == psychMenuMusic_ && (menuPcm_ || menuJob_.valid())) return;
    stopMenuMusic();
    if (menuJob_.valid()) menuJob_.wait();
    menuJob_ = {};
    menuPcm_.reset();
    psychMenuMusic_ = false;
    if (wantPsych) {
        auto pcm = std::make_shared<Pcm>();
        std::string err;
        if (loadAudioFile(ui.menuMusic(), *pcm, err) && pcm->frames() > 0) {
            menuPcm_ = pcm;
            menuLoop_ = double(pcm->frames()) / Mixer::kRate;
            menuBpm_ = 102;  // freakyMenu
            psychMenuMusic_ = true;
            return;
        }
    }
    menuBpm_ = menuTheme().bpm;
    menuJob_ = std::async(std::launch::async, [this] { return library.menuMusic(&menuLoop_); });
}

void App::toggleFullscreen() {
    if (platform::kAndroid) return;
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
        else if (!std::strcmp(a, "--check-charts")) checkCharts = true;
        else if (!std::strcmp(a, "--touch")) touchMode = true;
        else if (!std::strcmp(a, "--perf")) perf = true;  // frame times on stdout every second
        else if (!std::strcmp(a, "--size")) std::sscanf(next(), "%dx%d", &winW_, &winH_);  // e.g. 2400x1080
    }
    if (listSongs) {
        // Prints what the song library finds (no window): --list-songs [--base dir]
        // [--check-charts: load every chart and print its keys and notes, or why it fails]
        if (base.empty()) base = std::string(GetWorkingDirectory()) + "/";
        if (base.back() != '/') base += '/';
        SetTraceLogLevel(LOG_WARNING);
        library.scan(base);
        for (const SongEntry& e : library.songs()) {
            if (e.builtin) continue;
            std::printf("%s\n  title: %s | %s%s\n  audio: %s\n", e.id.c_str(), e.title.c_str(), e.subtitle.c_str(),
                        e.autoChart ? " | AutoChart" : "", e.inst.empty() ? "(none)" : e.inst.c_str());
            if (!e.voices.empty() || !e.voicesP.empty() || !e.voicesO.empty())
                std::printf("  voices: %s\n", (e.voices.empty() ? e.voicesP + " " + e.voicesO : e.voices).c_str());
            std::string meta;
            if (checkCharts && !e.metaPath.empty()) readTextFile(e.metaPath, meta);
            for (size_t i = 0; i < e.diffs.size(); ++i) {
                const std::string path = i < e.chartPaths.size() ? e.chartPaths[i] : "";
                std::printf("  [%s] %s", e.diffs[i].c_str(), path.c_str());
                if (checkCharts && !path.empty()) {
                    Chart c;
                    std::string err;
                    if (loadChartFile(path, e.diffs[i], c, err, meta, 0, true))
                        std::printf("  -> ok %dK, %llu notes", c.keys, (unsigned long long)(c.playerNotes + c.opponentNotes));
                    else
                        std::printf("  -> ERROR %s", err.c_str());
                }
                std::printf("\n");
            }
            if (e.assetRoots.size() > 1)
                for (const std::string& r : e.assetRoots) std::printf("  root %s\n", r.c_str());
        }
        std::printf("%s\n%s", library.report().c_str(), library.installReport().c_str());
        return 0;
    }

    if (platform::kAndroid) {
        base = platform::dataDir();
        platform::prepareDataDir(base);
        touchMode = true;
    }
    if (base.empty()) base = GetApplicationDirectory();
    if (!FileExists((base + "assets/fonts/Rajdhani-Bold.ttf").c_str())) {
        std::string cwd = std::string(GetWorkingDirectory()) + "/";
        if (FileExists((cwd + "assets/fonts/Rajdhani-Bold.ttf").c_str())) base = cwd;
    }
    settings.load(base + "settings.cfg");
    scores.load(base + "scores.cfg");

    unsigned flags = platform::kAndroid ? 0u : (FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    if (settings.vsync) flags |= FLAG_VSYNC_HINT;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    // Android: the whole screen, whatever its size.
    InitWindow(platform::kAndroid ? 0 : std::max(320, winW_), platform::kAndroid ? 0 : std::max(180, winH_),
               "God's Hand of Principality and Demise");
    platform::afterWindowInit();
    platform::setVsync(settings.vsync);  // explicit both ways: some drivers default to on
    SetExitKey(KEY_NULL);
    if (!platform::kAndroid) {
        SetWindowMinSize(640, 360);
        if (settings.fullscreen && !autoRun.enabled) ToggleBorderlessWindowed();
    }

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
    if (settings.psychUi) applyPsychUi();

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
    else if (autoRun.state == "modpacks") state_ = makeModPacks();
    else state_ = makeTitle();
    state_->enter();
    logScreen(*state_);

    double last = nowSec(), nextFrame = last;
    while (!quit && !WindowShouldClose()) {
        PollInputEvents();
        now = nowSec();
        input.poll(now);
        if (touchMode) touch.update(input, now);
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
    psychUi().unload();
    sfxShutdown();
    gfx::shutdown();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

void App::frame() {
    input.beginFrame(dt);
    if (GetScreenWidth() != shownW_ || GetScreenHeight() != shownH_) {
        shownW_ = GetScreenWidth();
        shownH_ = GetScreenHeight();
        platform::log(TextFormat("screen size %dx%d", shownW_, shownH_));
    }
    if (platform::takeResumed()) {
        platform::log("back from the background");
        platform::setVsync(settings.vsync);  // a new surface starts with the system's default
        state_->onResume();
    }
    if (touchMode) touch.setLayout(input, state_->touchLayout(), now);
    if (input.pressed(KEY_F11) || (input.pressed(KEY_ENTER) && (input.isDown(KEY_LEFT_ALT) || input.isDown(KEY_RIGHT_ALT))))
        toggleFullscreen();
    volumeKeys();

    if (!menuPcm_ && menuJob_.valid() &&
        menuJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        menuPcm_ = menuJob_.get();
    if (state_->wantsMenuMusic() && !fadeDir_) startMenuMusic();

    const double tUpdate = nowSec();
    if (fadeDir_ == 0) state_->update(dt);
    if (fadeDir_ > 0) {
        fade_ += float(dt / 0.22);
        if (fade_ >= 1.0f) {
            fade_ = 1.0f;
            state_ = std::move(next_);
            input.clear();
            state_->enter();
            logScreen(*state_);
            fadeDir_ = -1;
        }
    } else if (fadeDir_ < 0) {
        state_->update(dt);
        fade_ -= float(dt / 0.22);
        if (fade_ <= 0.0f) { fade_ = 0; fadeDir_ = 0; }
    }

    const double tDraw = nowSec();
    BeginDrawing();
    ClearBackground(BLACK);
    state_->draw();
    gfx::beginCanvas();
    if (fade_ > 0) DrawRectangleRec({-10, -10, gfx::W + 20, gfx::H + 20}, gfx::fade(BLACK, fade_));
    if (settings.showFps) gfx::text(gfx::FontId::Ui, TextFormat("%d FPS", int(fps + 0.5f)), 8, 4, 20, WHITE, gfx::Align::Left, 1.5f);
    drawVolumeTray();
    gfx::endCanvas();
    gfx::drawLetterbox();
    if (touchMode) touch.draw();
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

    // Frame timing: update, draw (including the buffer swap), worst frame of the last second.
    const double tEnd = nowSec();
    const float upd = float((tDraw - tUpdate) * 1000.0), drw = float((tEnd - tDraw) * 1000.0);
    updateMs = updateMs * 0.9f + upd * 0.1f;
    drawMs = drawMs * 0.9f + drw * 0.1f;
    perfWorst_ = std::max(perfWorst_, float(dt * 1000.0));
    perfUpd_ += upd;
    perfDraw_ += drw;
    ++perfFrames_;
    if (now - perfSince_ >= 1.0) {
        worstFrameMs = perfWorst_;
        if (perf && perfFrames_ > 0)
            std::printf("[perf] %d fps, update %.2f ms, draw %.2f ms, worst frame %.1f ms\n", perfFrames_,
                        perfUpd_ / perfFrames_, perfDraw_ / perfFrames_, perfWorst_);
        perfSince_ = now;
        perfFrames_ = 0;
        perfUpd_ = perfDraw_ = perfWorst_ = 0;
    }
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
