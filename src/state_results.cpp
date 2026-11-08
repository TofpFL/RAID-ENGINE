// End-of-song results.
#include <cmath>

#include "app.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "raylib.h"

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {
const Color kGold{255, 214, 120, 255};
const Color kIvory{246, 236, 214, 255};

Color rankColor(const std::string& r) {
    if (r == "S+" || r == "S") return Color{255, 214, 100, 255};
    if (r == "A") return Color{120, 230, 150, 255};
    if (r == "B") return Color{120, 190, 255, 255};
    if (r == "C") return Color{190, 150, 255, 255};
    return Color{255, 90, 100, 255};
}
const Color kRatingCols[4] = {{255, 215, 100, 255}, {120, 220, 255, 255}, {205, 205, 220, 255}, {205, 120, 150, 255}};
}  // namespace

class ResultsState : public State {
public:
    explicit ResultsState(ResultData r) : r_(std::move(r)) {}
    void enter() override {
        t0_ = app().now;
        sfxPlay(r_.rank == "S" || r_.rank == "S+" ? Sfx::Unlock : Sfx::Confirm);
        double sum = 0, sq = 0;
        for (const auto& e : r_.errors) { sum += e.err; sq += double(e.err) * e.err; }
        const double n = double(std::max<size_t>(1, r_.errors.size()));
        mean_ = sum / n;
        sd_ = std::sqrt(std::max(0.0, sq / n - mean_ * mean_));
    }
    void update(double) override {
        App& a = app();
        if (a.autoRun.enabled && a.autoRun.quitAfter < 0 && a.now - t0_ > 2.5) a.quit = true;
        if (a.now - t0_ < 0.6) return;
        if (!r_.replayPath.empty() && a.input.pressed(KEY_R)) {
            sfxPlay(Sfx::Confirm);
            PlayRequest n;
            n.songId = r_.req.songId;
            n.diff = r_.req.diff;
            n.replayPath = r_.replayPath;
            a.change(makePlay(n));
            return;
        }
        if (a.input.accept() || a.input.back()) {
            sfxPlay(Sfx::Confirm);
            if (r_.req.story && !r_.req.storyQueue.empty()) {
                PlayRequest n = r_.req;
                n.songId = n.storyQueue.front();
                n.storyQueue.erase(n.storyQueue.begin());
                a.change(makePlay(n));
            } else {
                a.change(r_.req.story ? makeStory() : makeFreeplay());
            }
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float t = float(now - t0_);
        gfx::beginCanvas();
        drawMenuBackground(now, r_.color, menuPulse());
        // rank
        const float pop = easeOutBack(saturate(t * 2.0f));
        const Color rc = rankColor(r_.rank);
        gfx::glowText(FontId::Title, r_.rank, 250, 120 + (1 - pop) * 60, 210 * std::max(0.2f, pop), fade(rc, saturate(t * 3)), rc);
        if (!r_.clear.empty()) gfx::text(FontId::Title, r_.clear, 250, 380, 34, kGold, Align::Center, 2.0f);
        if (r_.newBest && std::fmod(t * 2.0f, 2.0f) < 1.5f)
            gfx::text(FontId::Ui, "NEW BEST!", 250, 430, 36, WHITE, Align::Center, 2.0f);
        if (r_.botplay) gfx::text(FontId::Ui, "Botplay - not saved", 250, 470, 24, fade(kIvory, 0.7f), Align::Center);
        else if (r_.practice) gfx::text(FontId::Ui, "Practice - not saved", 250, 470, 24, fade(kIvory, 0.7f), Align::Center);
        // stats
        const float x = 520;
        gfx::text(FontId::Title, r_.title, x, 50, 44, kGold, Align::Left, 2.0f);
        gfx::text(FontId::Ui, r_.diff + (r_.rate != 1.0 ? std::string(TextFormat("   %.2fx", r_.rate)) : std::string()), x, 104, 28,
                  kIvory, Align::Left, 1.5f);
        const float shown = saturate(t * 1.5f);
        gfx::text(FontId::Ui, "SCORE", x, 150, 22, fade(kGold, 0.8f));
        gfx::text(FontId::Ui, fmtThousands((long long)(double(r_.score) * shown)), x, 170, 56, WHITE, Align::Left, 2.0f);
        gfx::text(FontId::Ui, "ACCURACY", x + 380, 150, 22, fade(kGold, 0.8f));
        gfx::text(FontId::Ui, TextFormat("%.2f%%", r_.acc * 100.0 * shown), x + 380, 170, 56, WHITE, Align::Left, 2.0f);
        static const char* names[4] = {"DIVINE", "GRACE", "MORTAL", "SINFUL"};
        for (int i = 0; i < 4; ++i) {
            gfx::text(FontId::Ui, names[i], x, 250.0f + i * 38, 30, kRatingCols[i], Align::Left, 1.5f);
            gfx::text(FontId::Ui, std::to_string(r_.counts[size_t(i)]), x + 220, 250.0f + i * 38, 30, WHITE, Align::Right, 1.5f);
        }
        gfx::text(FontId::Ui, "MISSES", x + 380, 250, 30, Color{255, 90, 100, 255}, Align::Left, 1.5f);
        gfx::text(FontId::Ui, std::to_string(r_.misses), x + 640, 250, 30, WHITE, Align::Right, 1.5f);
        gfx::text(FontId::Ui, "MAX COMBO", x + 380, 288, 30, kGold, Align::Left, 1.5f);
        gfx::text(FontId::Ui, std::to_string(r_.maxCombo), x + 640, 288, 30, WHITE, Align::Right, 1.5f);
        gfx::text(FontId::Ui, "TIMING", x + 380, 326, 30, kIvory, Align::Left, 1.5f);
        gfx::text(FontId::Ui, TextFormat("%+.1f / %.1f ms", mean_, sd_), x + 640, 326, 30, WHITE, Align::Right, 1.5f);
        // timing graph
        const Rectangle g{x, 420, 680, 190};
        DrawRectangleRounded({g.x - 8, g.y - 8, g.width + 16, g.height + 16}, 0.06f, 8, Color{8, 4, 14, 200});
        const float cy = g.y + g.height * 0.5f;
        for (int k = -3; k <= 3; ++k) {
            const float yy = cy - float(k) * 45.0f / 166.0f * g.height * 0.5f;
            DrawLineEx({g.x, yy}, {g.x + g.width, yy}, k == 0 ? 2.0f : 1.0f, fade(WHITE, k == 0 ? 0.35f : 0.12f));
        }
        const size_t n = r_.errors.size();
        const size_t stride = std::max<size_t>(1, n / 5000);
        const size_t upto = size_t(double(n) * saturate(t * 1.2f));
        for (size_t i = 0; i < upto; i += stride) {
            const auto& e = r_.errors[i];
            const float px = g.x + float(e.t / r_.lengthMs) * g.width;
            const float py = cy + clampv(e.err, -166.0f, 166.0f) / 166.0f * g.height * 0.5f;
            const float ae = std::fabs(e.err);
            const int ri = ae <= 45 ? 0 : ae <= 90 ? 1 : ae <= 135 ? 2 : 3;
            DrawCircleV({px, py}, 2.2f, kRatingCols[ri]);
        }
        gfx::text(FontId::Ui, "early", g.x + 6, g.y + 2, 18, fade(kIvory, 0.6f));
        gfx::text(FontId::Ui, "late", g.x + 6, g.y + g.height - 20, 18, fade(kIvory, 0.6f));
        if (r_.req.story && r_.req.storyQueue.empty())
            gfx::text(FontId::Title, "WEEK COMPLETE  -  " + fmtThousands(r_.req.storyScore), 250, 540, 28, kGold, Align::Center, 2.0f);
        DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
        const bool next = r_.req.story && !r_.req.storyQueue.empty();
        std::string hint = next ? "ENTER next song" : "ENTER continue";
        if (!r_.replayPath.empty()) hint += "      R watch replay";
        if (r_.replay) hint = "REPLAY      " + hint;
        gfx::text(FontId::Ui, hint, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
        gfx::endCanvas();
    }

private:
    ResultData r_;
    double t0_ = 0, mean_ = 0, sd_ = 0;
};

std::unique_ptr<State> makeResults(const ResultData& r) { return std::make_unique<ResultsState>(r); }

}  // namespace gh
