#include "autochart.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "common.hpp"

namespace gh {

namespace {

constexpr int kN = 1024, kHop = 256, kRate = 22050;

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(kPi) / float(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    const size_t k = size_t(clampv(p, 0.0, 1.0) * double(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

}  // namespace

bool autoChart(const int16_t* samples, size_t frames, int channels, int sampleRate, int difficulty, Chart& out,
               AutoChartInfo* info) {
    if (!samples || frames < size_t(sampleRate) * 4 || channels < 1 || sampleRate < 8000) return false;
    difficulty = clampv(difficulty, 0, 2);

    // Mono at ~22 kHz.
    const double step = double(sampleRate) / kRate;
    const size_t n = size_t(double(frames) / step);
    std::vector<float> mono(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t f = std::min(frames - 1, size_t(double(i) * step));
        float s = 0;
        for (int c = 0; c < channels; ++c) s += float(samples[f * size_t(channels) + size_t(c)]);
        mono[i] = s / (32768.0f * float(channels));
    }

    // Spectral flux per band, energy and spectral centroid per frame.
    const size_t nf = n > size_t(kN) ? (n - size_t(kN)) / kHop + 1 : 0;
    if (nf < 64) return false;
    std::vector<float> window(kN);
    for (int i = 0; i < kN; ++i) window[size_t(i)] = 0.5f - 0.5f * std::cos(2.0f * float(kPi) * float(i) / float(kN - 1));
    std::vector<double> env(nf, 0), energy(nf, 0), centroid(nf, 0);
    std::vector<float> prev(kN / 2, 0), mag(kN / 2);
    std::vector<std::complex<float>> buf(kN);
    const double binHz = double(kRate) / kN;
    for (size_t f = 0; f < nf; ++f) {
        for (int i = 0; i < kN; ++i) buf[size_t(i)] = {mono[f * kHop + size_t(i)] * window[size_t(i)], 0.0f};
        fft(buf);
        double flux = 0, e = 0, cw = 0, cs = 0;
        for (int k = 1; k < kN / 2; ++k) {
            const float m = std::log1p(100.0f * std::abs(buf[size_t(k)]));
            mag[size_t(k)] = m;
            const double hz = k * binHz;
            const double w = hz < 150 ? 1.6 : hz < 2500 ? 1.0 : 0.7;  // kicks count a bit more
            flux += w * std::max(0.0f, m - prev[size_t(k)]);
            e += m;
            cw += m * hz;
            cs += m;
        }
        prev.swap(mag);
        env[f] = flux;
        energy[f] = e;
        centroid[f] = cs > 0 ? cw / cs : 0;
    }
    const double hopMs = 1000.0 * kHop / kRate;

    // Normalise the onset envelope (remove slow trend).
    std::vector<double> o(nf, 0);
    {
        const int w = 24;
        double acc = 0;
        for (size_t i = 0; i < nf; ++i) {
            acc += env[i];
            if (i >= size_t(2 * w + 1)) acc -= env[i - 2 * w - 1];
            const size_t c = i >= size_t(w) ? i - w : 0;
            const double mean = acc / double(std::min(i + 1, size_t(2 * w + 1)));
            o[c] = std::max(0.0, env[c] - mean);
        }
    }

    // Tempo: autocorrelation over 70-200 BPM with a gentle preference for ~130.
    double bestLag = 0, bestScore = -1;
    const double minLag = 60000.0 / 200.0 / hopMs, maxLag = 60000.0 / 70.0 / hopMs;
    std::vector<double> ac(size_t(maxLag) + 3, 0);
    for (size_t lag = size_t(minLag); lag <= size_t(maxLag) + 1 && lag < nf; ++lag) {
        double s = 0;
        for (size_t i = lag; i < nf; ++i) s += o[i] * o[i - lag];
        ac[lag] = s / double(nf - lag);
    }
    for (size_t lag = size_t(minLag) + 1; lag + 1 < ac.size() && lag <= size_t(maxLag); ++lag) {
        const double bpm = 60000.0 / (double(lag) * hopMs);
        const double pref = std::exp(-0.5 * std::pow(std::log2(bpm / 130.0) / 0.6, 2.0));
        // Reward lags whose double is also strong (true beat periods repeat).
        const double sc = (ac[lag] + 0.5 * (2 * lag < ac.size() ? ac[2 * lag] : 0)) * (0.6 + 0.4 * pref);
        if (sc > bestScore) { bestScore = sc; bestLag = double(lag); }
    }
    if (bestLag <= 0) return false;
    {  // parabolic refinement
        const size_t l = size_t(bestLag);
        if (l > 0 && l + 1 < ac.size()) {
            const double a = ac[l - 1], b = ac[l], c = ac[l + 1], den = a - 2 * b + c;
            if (std::fabs(den) > 1e-12) bestLag += clampv(0.5 * (a - c) / den, -0.5, 0.5);
        }
    }
    double bpm = 60000.0 / (bestLag * hopMs);
    while (bpm < 85) bpm *= 2;
    while (bpm > 210) bpm /= 2;
    bpm = std::round(bpm * 10.0) / 10.0;
    double beatMs = 60000.0 / bpm;

    // Beat phase: the offset whose grid collects the most onset energy.
    double bestPhase = 0, bestPhaseScore = -1;
    for (double ph = 0; ph < beatMs; ph += hopMs * 0.5) {
        double s = 0;
        for (double t = ph; t < double(nf) * hopMs; t += beatMs) {
            const size_t i = size_t(t / hopMs);
            if (i < nf) s += o[i];
        }
        if (s > bestPhaseScore) { bestPhaseScore = s; bestPhase = ph; }
    }

    // Peak picking.
    struct Onset { double t, strength, centroid, energy; size_t frame; };
    std::vector<Onset> onsets;
    for (size_t i = 3; i + 3 < nf; ++i) {
        double mean = 0;
        int cnt = 0;
        for (size_t j = i >= 10 ? i - 10 : 0; j <= std::min(nf - 1, i + 10); ++j, ++cnt) mean += o[j];
        mean /= std::max(1, cnt);
        bool peak = o[i] > 0;
        for (size_t j = i - 3; j <= i + 3 && peak; ++j)
            if (j != i && o[j] > o[i]) peak = false;
        // +29 ms: the flux peaks before the attack is heard (measured on clicks).
        if (peak && o[i] > mean * 1.35 + 1e-6) onsets.push_back({double(i) * hopMs + hopMs * 0.5 + 29.0, o[i], centroid[i], energy[i], i});
    }
    if (onsets.size() < 8) return false;

    // Refine tempo and phase with a least-squares fit of strong onsets to the
    // eighth-note grid; a 0.1 % tempo error would drift ~0.1 s over a song.
    {
        std::vector<double> st;
        for (const Onset& on : onsets) st.push_back(on.strength);
        const double cut = percentile(st, 0.4);
        double phase = bestPhase + 29.0;
        // Fit a growing span of the song so early drift cannot mis-assign
        // later onsets to the wrong grid slot.
        const double spans[] = {15000, 30000, 60000, 120000, 1e12, 1e12};
        for (double span : spans) {
            const double slotE = beatMs / 2;
            double sk = 0, st2 = 0, skk = 0, skt = 0, cnt = 0;
            for (const Onset& on : onsets) {
                if (on.strength < cut || on.t > onsets.front().t + span) continue;
                const double k = std::round((on.t - phase) / slotE);
                if (std::fabs(on.t - (phase + k * slotE)) > slotE * 0.25) continue;
                sk += k; st2 += on.t; skk += k * k; skt += k * on.t; cnt += 1;
            }
            const double den = cnt * skk - sk * sk;
            if (cnt < 16 || std::fabs(den) < 1e-9) continue;
            const double b = (cnt * skt - sk * st2) / den, a = (st2 - b * sk) / cnt;
            if (!(b > 0) || std::fabs(2 * b / beatMs - 1.0) > 0.02) continue;
            beatMs = 2 * b;
            phase = a;
        }
        while (phase < 0) phase += beatMs;
        bestPhase = std::fmod(phase, beatMs);
        bpm = 60000.0 / beatMs;
    }

    // Snap to the grid for the difficulty and keep the strongest per slot.
    const int div = difficulty == 0 ? 1 : difficulty == 1 ? 2 : 4;  // per beat
    const double slot = beatMs / div;
    std::vector<double> strengths;
    for (const Onset& on : onsets) strengths.push_back(on.strength);
    const double minStrength = percentile(strengths, difficulty == 0 ? 0.55 : difficulty == 1 ? 0.35 : 0.2);
    std::vector<Onset> picked;
    long long lastSlot = -1000000;
    for (const Onset& on : onsets) {
        if (on.strength < minStrength) continue;
        const long long k = std::llround((on.t - bestPhase) / slot);
        const double snapped = bestPhase + double(k) * slot;
        if (std::fabs(snapped - on.t) > slot * 0.45 || snapped < 0) continue;
        if (k == lastSlot) {
            if (on.strength > picked.back().strength) { picked.back() = on; picked.back().t = snapped; }
            continue;
        }
        Onset p = on;
        p.t = snapped;
        picked.push_back(p);
        lastSlot = k;
    }
    if (picked.size() < 8) return false;

    // Lanes from pitch (spectral centroid), avoiding fast jacks.
    std::vector<double> cents;
    for (const Onset& p : picked) cents.push_back(p.centroid);
    const double q1 = percentile(cents, 0.25), q2 = percentile(cents, 0.5), q3 = percentile(cents, 0.75);
    const double strongCut = percentile(strengths, 0.93);
    out = Chart();
    out.keys = 4;
    out.speed = difficulty == 0 ? 1.7 : difficulty == 1 ? 2.1 : 2.5;
    out.tempo.push_back({0, bpm, 0});
    Rng rng(uint64_t(frames) * 2654435761ull + uint64_t(difficulty));
    int prevLane = -1;
    double prevT = -1e9;
    const double blockMs = beatMs * 16;  // four bars per singer
    for (size_t i = 0; i < picked.size(); ++i) {
        const Onset& p = picked[i];
        int lane = p.centroid < q1 ? 0 : p.centroid < q2 ? 1 : p.centroid < q3 ? 2 : 3;
        if (lane == prevLane && p.t - prevT < beatMs * 0.75) lane = (lane + (rng.chance(0.5) ? 1 : 3)) % 4;
        const int block = int(std::floor((p.t - bestPhase) / blockMs));
        Note nt;
        nt.time = p.t;
        nt.lane = uint8_t(lane);
        nt.side = uint8_t(block % 2 == 0 ? 0 : 1);
        // Sustains where the sound keeps ringing before the next note.
        const double gap = i + 1 < picked.size() ? picked[i + 1].t - p.t : beatMs * 2;
        if (gap >= beatMs * (difficulty == 0 ? 1.5 : 1.0)) {
            const size_t end = std::min(nf - 1, p.frame + size_t(beatMs * 0.5 / hopMs));
            double hold = 0;
            for (size_t j = p.frame; j <= end; ++j) hold += energy[j];
            hold /= double(end - p.frame + 1);
            if (hold > p.energy * 0.82) nt.length = std::floor((gap - beatMs * 0.25) / (beatMs / 4)) * (beatMs / 4);
        }
        out.notes.push_back(nt);
        if (difficulty == 2 && p.strength >= strongCut && gap >= slot) {
            Note chord = nt;
            chord.lane = uint8_t((lane + 2) % 4);
            chord.length = 0;
            out.notes.push_back(chord);
        }
        prevLane = lane;
        prevT = p.t;
    }
    for (double t = bestPhase; t < double(nf) * hopMs; t += blockMs)
        out.focus.push_back({t, uint8_t(int(std::floor((t - bestPhase) / blockMs + 0.5)) % 2 == 0 ? 0 : 1)});
    out.finalize(bpm);
    // Align beat numbers with the detected downbeat.
    const double b = out.beatAt(bestPhase);
    const double frac = b - std::round(b);
    for (TempoPoint& tp : out.tempo) tp.beat -= frac;
    if (info) {
        info->bpm = bpm;
        info->firstBeatMs = bestPhase;
        info->onsets = onsets.size();
    }
    return !out.notes.empty();
}

}  // namespace gh
