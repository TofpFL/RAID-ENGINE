// Mod packs screen (Freeplay: M): downloads the packs published on the
// game's "mods" release (with the system's curl; on Android through Java)
// and installs them.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "platform.hpp"
#include "process.hpp"
#include "raylib.h"

namespace gh {

namespace {
namespace fs = std::filesystem;
using gfx::Align;
using gfx::FontId;

constexpr const char* kReleaseUrl =
    "https://github.com/deatuamae67-crypto/Gods-Hand-of-Principality-And-Demise/releases/download/mods/";

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

struct Pack {
    std::string folder;
    std::vector<std::string> zips;
};

// Shared with the download thread.
struct Shared {
    std::mutex m;
    std::string status;
    std::string currentFile;  // .part being written
    bool busy = false, manifestOk = false, done = false;
    std::vector<Pack> packs;
};

uint64_t fileSizeOf(const std::string& p) {
    std::error_code ec;
    const auto s = fs::file_size(toPath(p), ec);
    return ec ? 0 : uint64_t(s);
}

}  // namespace

class ModPacksState : public State {
public:
    void enter() override {
        App& a = app();
        if (!platform::kAndroid) curl_ = findProgram(a.base, "curl", "--version");
        if (!canDownload()) {
            sh_->status = "curl was not found. Download the packs from the game's Releases page (tag 'mods') and put the zips in mods/.";
            return;
        }
        startJob([this, base = a.base] { fetchManifest(base); });
    }

    void update(double) override {
        App& a = app();
        Input& in = a.input;
        bool busy, done;
        {
            std::lock_guard<std::mutex> lk(sh_->m);
            busy = sh_->busy;
            done = sh_->done;
            sh_->done = false;
        }
        if (done) {
            // Install what was downloaded (the library unpacks zips in mods/).
            a.library.scan(a.base, [](const std::string& zip, double f) {
                BeginDrawing();
                ClearBackground(Color{12, 8, 20, 255});
                const char* t1 = TextFormat("Installing %s", zip.c_str());
                const int w = GetScreenWidth(), h = GetScreenHeight();
                DrawText(t1, (w - MeasureText(t1, 24)) / 2, h / 2 - 30, 24, RAYWHITE);
                DrawRectangle(w / 4, h / 2 + 10, int(w / 2 * f), 10, Color{200, 160, 255, 255});
                EndDrawing();
                PollInputEvents();
                SwapScreenBuffer();
            });
            std::lock_guard<std::mutex> lk(sh_->m);
            sh_->status += "  Installed: " + a.library.report() + ".";
        }
        const int n = int(packs().size()) + 1;  // + "all"
        if (!busy) {
            if (in.up()) { sel_ = (sel_ + n - 1) % n; sfxPlay(Sfx::Scroll); }
            if (in.down()) { sel_ = (sel_ + 1) % n; sfxPlay(Sfx::Scroll); }
            if (in.accept() && canDownload() && n > 1) {
                std::vector<Pack> todo;
                if (sel_ == 0) {
                    for (const Pack& p : packs())
                        if (!installed(p)) todo.push_back(p);
                } else {
                    todo.push_back(packs()[size_t(sel_ - 1)]);
                }
                if (!todo.empty()) {
                    sfxPlay(Sfx::Confirm);
                    startJob([this, todo, base = a.base] { download(base, todo); });
                }
            }
        }
        if (in.back() && !busy) {
            sfxPlay(Sfx::Cancel);
            a.change(makeFreeplay());
        }
    }

    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x5A3C8C, menuPulse());
        gfx::text(FontId::Title, "MOD PACKS", gfx::W / 2, 40, 54, gfx::rgb(0xFFE08A), Align::Center, 3);
        std::string status, current;
        bool busy;
        {
            std::lock_guard<std::mutex> lk(sh_->m);
            status = sh_->status;
            current = sh_->currentFile;
            busy = sh_->busy;
        }
        const auto list = packs();
        float y = 130;
        auto row = [&](int i, const std::string& label, const std::string& right) {
            const bool on = i == sel_;
            if (on) DrawRectangleRounded({200, y - 6, 880, 36}, 0.3f, 6, Color{255, 255, 255, 30});
            gfx::text(FontId::Ui, label, 220, y, 26, on ? gfx::rgb(0xFFFFFF) : gfx::fade(WHITE, 0.7f));
            gfx::text(FontId::Ui, right, 1060, y, 22, gfx::fade(WHITE, 0.6f), Align::Right);
            y += 38;
        };
        if (!list.empty()) {
            int missing = 0;
            for (const Pack& p : list) missing += !installed(p);
            row(0, "Download every missing pack", std::to_string(missing) + " missing");
            const int first = std::max(1, std::min(sel_ - 6, int(list.size()) - 11));
            for (int i = first; i < int(list.size()) + 1 && y < 600; ++i) {
                const Pack& p = list[size_t(i - 1)];
                row(i, prettyName(p.folder), installed(p) ? "installed" : (p.zips.size() > 1 ? std::to_string(p.zips.size()) + " parts" : ""));
            }
        }
        if (busy && !current.empty())
            status += TextFormat("  (%.1f MB)", double(fileSizeOf(current)) / 1e6);
        gfx::text(FontId::Ui, status, gfx::W / 2, 620, 20, gfx::fade(WHITE, 0.8f), Align::Center);
        const char* help = busy ? "Working... please wait" : a.touchMode ? "OK download" : "UP/DOWN select   ENTER download   ESC back";
        gfx::text(FontId::Ui, help, gfx::W / 2, 670, 20, gfx::fade(WHITE, 0.55f), Align::Center);
        gfx::endCanvas();
    }

    ~ModPacksState() override {
        if (worker_.joinable()) worker_.join();
    }

private:
    std::vector<Pack> packs() {
        std::lock_guard<std::mutex> lk(sh_->m);
        return sh_->packs;
    }
    bool canDownload() const { return platform::kAndroid || !curl_.empty(); }
    bool installed(const Pack& p) const {
        std::error_code ec;
        return fs::is_directory(toPath(app().base + "mods/" + p.folder), ec);
    }
    void startJob(std::function<void()> fn) {
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> lk(sh_->m);
            sh_->busy = true;
        }
        worker_ = std::thread([this, fn] {
            fn();
            std::lock_guard<std::mutex> lk(sh_->m);
            sh_->busy = false;
            sh_->currentFile.clear();
        });
    }
    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(sh_->m);
        sh_->status = s;
    }
    bool fetch(const std::string& url, const std::string& dest) {
        {
            std::lock_guard<std::mutex> lk(sh_->m);
            sh_->currentFile = dest + ".part";
        }
        std::error_code ec;
        fs::create_directories(toPath(dest).parent_path(), ec);
        bool ok;
        if (platform::kAndroid) {
            std::string err;
            ok = false;
            for (int attempt = 0; attempt < 3 && !ok; ++attempt) ok = platform::download(url, dest + ".part", err);
        } else {
            ok = runProcess(curl_, {"-L", "--fail", "--silent", "--show-error", "--retry", "3", "-o", dest + ".part", url}) == 0;
        }
        if (!ok) {
            fs::remove(toPath(dest + ".part"), ec);
            return false;
        }
        fs::rename(toPath(dest + ".part"), toPath(dest), ec);
        return !ec;
    }
    void fetchManifest(const std::string& base) {
        setStatus("Fetching the list of mod packs...");
        const std::string dest = base + "cache/mod-packs.txt";
        if (!fetch(std::string(kReleaseUrl) + "mod-packs.txt", dest)) {
            setStatus("Could not reach the Releases page (no internet?).");
            return;
        }
        std::string text;
        if (FILE* f = std::fopen(dest.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        std::vector<Pack> list;
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            const std::string line = text.substr(p, e - p);
            p = e + 1;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            Pack pk;
            pk.folder = line.substr(0, tab);
            size_t q = tab + 1;
            while (q < line.size()) {
                size_t sp = line.find(' ', q);
                if (sp == std::string::npos) sp = line.size();
                const std::string z = trim(line.substr(q, sp - q));
                if (!z.empty() && z.find('/') == std::string::npos && z.find('\\') == std::string::npos) pk.zips.push_back(z);
                q = sp + 1;
            }
            if (!pk.folder.empty() && !pk.zips.empty() && pk.folder.find("..") == std::string::npos) list.push_back(pk);
        }
        std::lock_guard<std::mutex> lk(sh_->m);
        sh_->packs = list;
        sh_->manifestOk = true;
        sh_->status = std::to_string(list.size()) + " packs available.";
    }
    void download(const std::string& base, const std::vector<Pack>& todo) {
        int ok = 0, failed = 0;
        for (const Pack& p : todo) {
            for (const std::string& z : p.zips) {
                setStatus("Downloading " + z + "...");
                if (fetch(std::string(kReleaseUrl) + z, base + "mods/" + z)) ++ok;
                else ++failed;
            }
        }
        std::lock_guard<std::mutex> lk(sh_->m);
        sh_->status = std::to_string(ok) + " file(s) downloaded" + (failed ? ", " + std::to_string(failed) + " failed." : ".");
        sh_->done = ok > 0;
    }

    std::shared_ptr<Shared> sh_ = std::make_shared<Shared>();
    std::thread worker_;
    std::string curl_;
    int sel_ = 0;
};

std::unique_ptr<State> makeModPacks() { return std::make_unique<ModPacksState>(); }

}  // namespace gh
