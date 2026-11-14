// The Visualizer Bar — see vizbar.hpp. Layout (1280x720 reference, scales
// with the window): a ~54 px strip at the top or bottom edge, 1 px frame
// lines just inside a near-black bed, the atom icon in a ~96 px left margin,
// and kBands spectrum bars filling the rest.
#include "vizbar.hpp"

#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "raylib.h"

namespace gh {

namespace {

constexpr float kBarH = 54.0f;
constexpr float kAtomW = 96.0f;
constexpr float kPadX = 14.0f;

void fft(std::complex<float>* a, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(kPi) / float(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

}  // namespace

void VizBar::reset() {
    std::fill(std::begin(band_), std::end(band_), 0.0f);
    atomSpin_ = 0;
}

void VizBar::update(double now, float dt) {
    (void)now;
    atomSpin_ += dt * 0.55f;  // slow precession of the orbit planes

    float pcm[kFftN];
    const int got = app().mixer.tapAudio(pcm, kFftN);
    if (!windowReady_) {
        for (int i = 0; i < kFftN; ++i) window_[i] = 0.5f - 0.5f * std::cos(2.0f * float(kPi) * float(i) / float(kFftN - 1));
        windowReady_ = true;
    }
    float target[kBands] = {};
    if (got >= kFftN / 2) {
        const int half = got > kFftN ? kFftN : got;
        // Newest kFftN samples: tapAudio returns the newest `got` ending at
        // the write head; take the last kFftN of them, zero-padded left.
        const int use = std::min(kFftN, half);
        for (int i = 0; i < kFftN; ++i)
            fftBuf_[i] = {i < kFftN - use ? 0.0f : pcm[got - use + (i - (kFftN - use))] * window_[i], 0.0f};
        fft(fftBuf_, kFftN);
        // Log-spaced bands, ~40 Hz .. 16 kHz (Nyquist is 22.05 kHz).
        constexpr double kRate = 44100.0, kBinHz = kRate / kFftN;
        constexpr double fLo = 40.0, fHi = 16000.0;
        for (int b = 0; b < kBands; ++b) {
            const double f0 = fLo * std::pow(fHi / fLo, double(b) / kBands);
            const double f1 = fLo * std::pow(fHi / fLo, double(b + 1) / kBands);
            const int k0 = std::max(1, int(f0 / kBinHz));
            const int k1 = std::min(kFftN / 2 - 1, int(f1 / kBinHz) + 1);
            double m = 0;
            for (int k = k0; k <= k1; ++k) m += std::abs(fftBuf_[k]);
            m /= std::max(1, k1 - k0 + 1);
            // perceptual-ish scale: quiet floor, ~-46 dB .. 0 dB window
            target[b] = float(std::clamp((std::log10(m * 90.0 + 1e-4) + 2.6) / 2.6, 0.0, 1.0));
        }
    }
    const float atk = std::min(1.0f, dt * 22.0f), rel = std::min(1.0f, dt * 7.0f);
    for (int b = 0; b < kBands; ++b) {
        const float k = target[b] > band_[b] ? atk : rel;
        band_[b] += (target[b] - band_[b]) * k;
    }
}

void VizBar::draw() {
    const Settings& s = app().settings;
    if (s.vizBar == 0) return;
    const float W = float(gfx::W), H = float(gfx::H);
    // Receptors live at the top in upscroll and the bottom in downscroll: if
    // the chosen edge would cover them, the strip moves to the other edge.
    bool bottom = s.vizBar == 2;
    if (bottom == s.downscroll) bottom = !bottom;
    const float y = bottom ? H - kBarH : 0.0f;

    DrawRectangleRec({0, y, W, kBarH}, Color{4, 6, 12, 215});
    const Color line{214, 220, 232, 110};
    DrawRectangleRec({0, y + 1, W, 1}, line);
    DrawRectangleRec({0, y + kBarH - 2, W, 1}, line);

    // Atom icon: nucleus + three precessing orbit ellipses with electrons.
    const Vector2 c{kAtomW * 0.5f, y + kBarH * 0.5f};
    const Color orbit{168, 188, 212, 120};
    const Color nuc{226, 234, 246, 220};
    for (int i = 0; i < 3; ++i) {
        const float tilt = atomSpin_ + float(i) * 1.0472f;  // 60 deg apart
        // ellipse in its own tilted plane: parametrize then shear-rotate
        constexpr int kSegs = 48;
        Vector2 prev{};
        for (int k = 0; k <= kSegs; ++k) {
            const float t = float(k) / float(kSegs) * 2.0f * float(kPi);
            const float ex = 27.0f * std::cos(t), ey = 11.0f * std::sin(t);
            const float ct = std::cos(tilt), st = std::sin(tilt);
            const Vector2 p{c.x + ex * ct - ey * st * 0.35f, c.y + (ex * st + ey * ct) * 0.55f};
            if (k) DrawLineV(prev, p, orbit);
            prev = p;
        }
        // electron riding the orbit
        const float et = float(app().now) * (1.7f + 0.35f * float(i)) + float(i) * 2.1f;
        const float ex = 27.0f * std::cos(et), ey = 11.0f * std::sin(et);
        const float ct = std::cos(tilt), st = std::sin(tilt);
        const Vector2 e{c.x + ex * ct - ey * st * 0.35f, c.y + (ex * st + ey * ct) * 0.55f};
        DrawCircleV(e, 2.6f, nuc);
    }
    DrawCircleV(c, 3.2f, nuc);
    DrawCircleLines(int(c.x), int(c.y), 6, gfx::fade(nuc, 0.35f));

    // Spectrum bars: log-ish spacing already baked into band_; thin bars
    // rising from the bottom frame line.
    const float x0 = kAtomW + 10.0f, x1 = W - kPadX;
    const float slot = (x1 - x0) / float(kBands);
    const float bw = std::max(1.0f, slot * 0.55f);
    const Color barCol{186, 202, 224, 190};
    const float base = y + kBarH - 4.0f;
    const float maxH = kBarH - 12.0f;
    for (int b = 0; b < kBands; ++b) {
        const float h = band_[b] * band_[b] * maxH;  // gamma: keep quiet parts quiet
        if (h < 1.0f) continue;
        const float bx = x0 + slot * (float(b) + 0.5f) - bw * 0.5f;
        DrawRectangleRec({bx, base - h, bw, h}, barCol);
    }
}

}  // namespace gh
