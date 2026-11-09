#include "synth.hpp"

#include <algorithm>
#include <cmath>

namespace gh::synth {

namespace {
constexpr double kTwoPi = 6.283185307179586;

inline size_t startIndex(double t) { return t <= 0 ? 0 : size_t(t * SR + 0.5); }
inline size_t lengthFor(const Stereo& out, size_t i0, double dur) {
    size_t n = size_t(dur * SR);
    if (i0 >= out.size()) return 0;
    return std::min(n, out.size() - i0);
}
inline float polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return float(t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return float(t * t + t + t + 1.0); }
    return 0.0f;
}
inline float sawBL(double ph, double dt) { return float(2.0 * ph - 1.0) - polyblep(ph, dt); }
inline float pulseBL(double ph, double dt, double w) {
    double p2 = ph + (1.0 - w);
    if (p2 >= 1.0) p2 -= 1.0;
    return (ph < w ? 1.0f : -1.0f) + polyblep(ph, dt) - polyblep(p2, dt);
}
inline void panGains(float pan, float& gl, float& gr) {  // pan -1..1, equal power
    float a = (pan + 1.0f) * 0.25f * float(3.14159265358979);
    gl = std::cos(a) * 1.41421356f;
    gr = std::sin(a) * 1.41421356f;
}
inline float lerpTo(float a, float b, float t) { return a + (b - a) * t; }
inline void addSend(Mono* send, size_t i, float v) {
    if (send && i < send->size()) (*send)[i] += v;
}

struct Formant { float f[3]; };
const Formant kVowels[5] = {
    {{730, 1090, 2440}},  // a
    {{530, 1840, 2480}},  // e
    {{290, 2290, 3010}},  // i
    {{570, 840, 2410}},   // o
    {{300, 870, 2240}},   // u
};
}  // namespace

void Stereo::mixIn(const Stereo& o, float g) {
    const size_t n = std::min(size(), o.size());
    for (size_t i = 0; i < n; ++i) { l[i] += o.l[i] * g; r[i] += o.r[i] * g; }
}

double midiHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

void SVF::set(float cutoff, float q) {
    cutoff = std::clamp(cutoff, 10.0f, float(SR) * 0.45f);
    const float g = std::tan(3.14159265f * cutoff / float(SR));
    k = 1.0f / std::max(0.05f, q);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

float SVF::process(float x, float* low, float* high) {
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    if (low) *low = v2;
    if (high) *high = x - k * v1 - v2;
    return v1 * k;
}

// ------------------------------------------------------------------ drums --

void kick(Stereo& out, double t, float vel, float punch) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, 0.5);
    Noise nz(uint32_t(i0 * 2654435761u + 7));
    double ph = 0;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        const double f = 44.0 + (80.0 + 90.0 * punch) * std::exp(-tt * 36.0);
        ph += f / SR;
        float s = float(std::sin(kTwoPi * ph) * std::exp(-tt * (8.5 - 2.5 * punch)));
        s += nz.next() * float(std::exp(-tt * 380.0)) * 0.22f;
        const float v = std::tanh(s * (1.3f + punch)) * vel * 0.85f;
        out.l[i0 + k] += v;
        out.r[i0 + k] += v;
    }
}

void snare(Stereo& out, Mono* send, double t, float vel) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, 0.4);
    Noise nz(uint32_t(i0 * 747796405u + 3));
    SVF bp; bp.set(1900.0f, 0.75f);
    SVF hp; hp.set(5200.0f, 0.7f);
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        const float tone = float(std::sin(kTwoPi * 186.0 * tt) * std::exp(-tt * 24.0) * 0.55 +
                                 std::sin(kTwoPi * 334.0 * tt) * std::exp(-tt * 32.0) * 0.25);
        const float w = nz.next();
        float h = 0;
        const float b = bp.process(w);
        hp.process(w, nullptr, &h);
        const float noise = (b * 0.9f + h * 0.35f) * float(std::exp(-tt * 15.0));
        const float v = (tone + noise) * vel * 0.6f;
        out.l[i0 + k] += v;
        out.r[i0 + k] += v;
        addSend(send, i0 + k, v * 0.35f);
    }
}

void clap(Stereo& out, Mono* send, double t, float vel) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, 0.45);
    Noise nz(uint32_t(i0 * 2891336453u + 11));
    SVF bp; bp.set(1250.0f, 1.1f);
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        double env = 0;
        for (int b = 0; b < 3; ++b) {
            double d = tt - b * 0.0105;
            if (d >= 0) env += std::exp(-d * 190.0);
        }
        if (tt > 0.028) env += std::exp(-(tt - 0.028) * 13.0) * 0.8;
        const float v = bp.process(nz.next()) * float(env) * vel * 0.75f;
        out.l[i0 + k] += v;
        out.r[i0 + k] += v;
        addSend(send, i0 + k, v * 0.45f);
    }
}

void hat(Stereo& out, double t, float vel, bool open, float pan) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, open ? 0.38 : 0.09);
    Noise nz(uint32_t(i0 * 1103515245u + 12345));
    SVF hp; hp.set(7800.0f, 0.8f);
    float gl, gr; panGains(pan, gl, gr);
    const double decay = open ? 10.0 : 58.0;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        float h = 0;
        hp.process(nz.next(), nullptr, &h);
        const float v = h * float(std::exp(-tt * decay)) * vel * 0.32f;
        out.l[i0 + k] += v * gl;
        out.r[i0 + k] += v * gr;
    }
}

void crash(Stereo& out, Mono* send, double t, float vel) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, 2.4);
    Noise nl(uint32_t(i0 + 101)), nr(uint32_t(i0 + 977));
    SVF hl, hr; hl.set(4800.0f, 0.6f); hr.set(5100.0f, 0.6f);
    const double partials[6] = {541, 803, 1197, 1711, 2483, 3119};
    double ph[6] = {0, 0.1, 0.2, 0.3, 0.4, 0.5};
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        float metal = 0;
        for (int p = 0; p < 6; ++p) {
            ph[p] += partials[p] / SR;
            if (ph[p] >= 1.0) ph[p] -= 1.0;
            metal += ph[p] < 0.5 ? 0.08f : -0.08f;
        }
        float a = 0, b = 0;
        hl.process(nl.next() + metal, nullptr, &a);
        hr.process(nr.next() + metal, nullptr, &b);
        const float env = float(std::exp(-tt * 2.1) * (1.0 - std::exp(-tt * 900.0)));
        out.l[i0 + k] += a * env * vel * 0.33f;
        out.r[i0 + k] += b * env * vel * 0.33f;
        addSend(send, i0 + k, (a + b) * env * vel * 0.1f);
    }
}

void tom(Stereo& out, Mono* send, double t, double hz, float vel) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, 0.45);
    double ph = 0;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        ph += hz * (1.0 + 0.7 * std::exp(-tt * 30.0)) / SR;
        const float v = float(std::sin(kTwoPi * ph) * std::exp(-tt * 7.5)) * vel * 0.6f;
        out.l[i0 + k] += v;
        out.r[i0 + k] += v;
        addSend(send, i0 + k, v * 0.3f);
    }
}

void riser(Stereo& out, double t, double dur, float vel) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, dur);
    Noise nl(uint32_t(i0 + 5)), nr(uint32_t(i0 + 55));
    SVF bl, br;
    for (size_t k = 0; k < n; ++k) {
        const double x = double(k) / double(std::max<size_t>(1, n));
        if ((k & 31) == 0) {
            const float fc = float(300.0 * std::pow(9000.0 / 300.0, x));
            bl.set(fc, 2.0f);
            br.set(fc * 1.03f, 2.0f);
        }
        const float env = float(x * x) * vel * 0.28f;
        out.l[i0 + k] += bl.process(nl.next()) * env;
        out.r[i0 + k] += br.process(nr.next()) * env;
    }
}

void impact(Stereo& out, Mono* send, double t, float vel) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, 2.6);
    Noise nz(uint32_t(i0 + 31337));
    SVF lp; lp.set(900.0f, 0.7f);
    double ph = 0;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        ph += (28.0 + 40.0 * std::exp(-tt * 3.0)) / SR;
        float low = 0;
        lp.process(nz.next(), &low);
        float s = float(std::sin(kTwoPi * ph) * std::exp(-tt * 1.3)) + low * float(std::exp(-tt * 7.0)) * 1.2f;
        s = std::tanh(s * 1.8f) * vel * 0.8f;
        out.l[i0 + k] += s;
        out.r[i0 + k] += s;
        addSend(send, i0 + k, s * 0.4f);
    }
}

// ------------------------------------------------------ pitched instruments --

void bass(Stereo& out, double t, double dur, double midi, float vel, int style) {
    const double rel = 0.06;
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, dur + rel);
    const double f = midiHz(midi);
    double p1 = 0, p2 = 0.37, sub = 0;
    SVF lp;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        if ((k & 15) == 0) {
            float fc;
            switch (style) {
                case 0: fc = 380.0f; break;
                case 1: fc = 520.0f + 500.0f * float(std::exp(-tt * 8.0)); break;
                case 2: fc = 320.0f + 2200.0f * float(std::exp(-tt * 11.0)); break;
                default: fc = 420.0f + 1500.0f * float(std::exp(-tt * 7.0)); break;
            }
            lp.set(fc, style >= 2 ? 1.4f : 0.8f);
        }
        const double dt = f / SR;
        p1 += dt; if (p1 >= 1) p1 -= 1;
        p2 += dt * 1.004; if (p2 >= 1) p2 -= 1;
        sub += dt * 0.5; if (sub >= 1) sub -= 1;
        float src;
        if (style == 0) src = float(std::sin(kTwoPi * p1)) * 0.8f + float(std::sin(kTwoPi * sub)) * 0.5f;
        else if (style == 2) src = sawBL(p1, dt) * 0.6f + pulseBL(p2, dt, 0.5) * 0.4f;
        else src = sawBL(p1, dt) * 0.55f + sawBL(p2, dt) * 0.45f;
        float low = 0;
        lp.process(src, &low);
        float s = style == 0 ? src * 0.6f : low;
        s += float(std::sin(kTwoPi * sub)) * (style == 3 ? 0.45f : 0.25f);
        if (style >= 2) s = std::tanh(s * (style == 3 ? 2.6f : 1.6f)) * 0.7f;
        float env = float(std::min(1.0, tt / 0.004));
        if (tt > dur) env *= float(std::max(0.0, 1.0 - (tt - dur) / rel));
        const float v = s * env * vel * 0.42f;
        out.l[i0 + k] += v;
        out.r[i0 + k] += v;
    }
}

void padNote(Stereo& out, Mono* send, double t, double dur, double midi, float vel, int style, float pan) {
    const double att = style == 3 ? 0.08 : 0.35, rel = 0.9;
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, dur + rel);
    const double f = midiHz(midi);
    const double det = std::pow(2.0, (style == 3 ? 5.0 : 9.0) / 1200.0);
    double pa = 0.11, pb = 0.63, pc = 0.29;
    SVF la, lb, f1a, f2a, f1b, f2b;
    float gl, gr; panGains(pan, gl, gr);
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        if ((k & 31) == 0) {
            const float lfo = float(std::sin(kTwoPi * 0.18 * (t + tt)));
            const float fc = (style == 3 ? 1500.0f : 1900.0f) + 500.0f * lfo;
            la.set(fc, 0.7f);
            lb.set(fc * 1.07f, 0.7f);
            if (style == 1) {  // choir-like formants "ah" -> "oh"
                const float m = 0.5f + 0.5f * lfo;
                f1a.set(lerpTo(730.0f, 570.0f, m), 6.0f); f2a.set(lerpTo(1090.0f, 840.0f, m), 8.0f);
                f1b.set(lerpTo(730.0f, 570.0f, 1 - m), 6.0f); f2b.set(lerpTo(1090.0f, 840.0f, 1 - m), 8.0f);
            }
        }
        const double dt = f / SR;
        pa += dt / det; if (pa >= 1) pa -= 1;
        pb += dt * det; if (pb >= 1) pb -= 1;
        pc += dt * 2.0; if (pc >= 1) pc -= 1;
        float a, b;
        if (style == 3) {  // dark organ
            a = pulseBL(pa, dt / det, 0.5) * 0.6f + pulseBL(pc, dt * 2, 0.5) * 0.25f;
            b = pulseBL(pb, dt * det, 0.5) * 0.6f + pulseBL(pc, dt * 2, 0.5) * 0.25f;
        } else {
            a = sawBL(pa, dt / det);
            b = sawBL(pb, dt * det);
        }
        float lowA = 0, lowB = 0;
        la.process(a, &lowA);
        lb.process(b, &lowB);
        if (style == 1) {
            lowA = lowA * 0.35f + f1a.process(a) * 1.1f + f2a.process(a) * 0.6f;
            lowB = lowB * 0.35f + f1b.process(b) * 1.1f + f2b.process(b) * 0.6f;
        }
        float env = float(std::min(1.0, tt / att));
        if (tt > dur) env *= float(std::max(0.0, 1.0 - (tt - dur) / rel));
        const float g = env * vel * 0.085f;
        const float vl = (lowA * 0.8f + lowB * 0.2f) * g * gl;
        const float vr = (lowA * 0.2f + lowB * 0.8f) * g * gr;
        out.l[i0 + k] += vl;
        out.r[i0 + k] += vr;
        addSend(send, i0 + k, (vl + vr) * 0.5f);
    }
}

void bell(Stereo& out, Mono* send, double t, double midi, float vel, float pan) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, 3.0);
    const double f = midiHz(midi);
    float gl, gr; panGains(pan, gl, gr);
    double pc = 0, pm = 0;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        pm += f * 3.5 / SR;
        const double idx = 2.4 * std::exp(-tt * 4.5) + 0.3;
        pc += f / SR;
        const float s = float(std::sin(kTwoPi * pc + idx * std::sin(kTwoPi * pm)) * std::exp(-tt * 1.7) *
                              std::min(1.0, tt / 0.002));
        const float v = s * vel * 0.16f;
        out.l[i0 + k] += v * gl;
        out.r[i0 + k] += v * gr;
        addSend(send, i0 + k, v * 0.6f);
    }
}

void pluck(Stereo& out, Mono* send, double t, double dur, double midi, float vel, float pan, int style) {
    const size_t i0 = startIndex(t), n = lengthFor(out, i0, std::min(dur + 0.35, 0.9));
    const double f = midiHz(midi);
    float gl, gr; panGains(pan, gl, gr);
    double p = 0, p2 = 0.5;
    SVF lp;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        if ((k & 15) == 0) lp.set(250.0f + 5200.0f * float(std::exp(-tt * 15.0)), 1.2f);
        const double dt = f / SR;
        p += dt; if (p >= 1) p -= 1;
        p2 += dt * 1.006; if (p2 >= 1) p2 -= 1;
        const float src = style == 2 ? pulseBL(p, dt, 0.3) * 0.6f + sawBL(p2, dt) * 0.4f : sawBL(p, dt) * 0.7f + sawBL(p2, dt) * 0.3f;
        float low = 0;
        lp.process(src, &low);
        const float env = float(std::exp(-tt * 6.5) * std::min(1.0, tt / 0.002));
        const float v = low * env * vel * 0.2f;
        out.l[i0 + k] += v * gl;
        out.r[i0 + k] += v * gr;
        addSend(send, i0 + k, v * 0.4f);
    }
}

// ------------------------------------------------------------------- voice --

void voiceNote(Mono& out, Mono* send, double t, double dur, double midi, double prevMidi, int vowelA, int vowelB,
               const VoiceStyle& st, uint32_t seed) {
    const double att = 0.012, rel = 0.07;
    const size_t i0 = startIndex(t);
    if (i0 >= out.size()) return;
    const size_t n = std::min(size_t((dur + rel) * SR), out.size() - i0);
    if (n == 0) return;
    Noise nz(seed * 2654435761u + 1);
    SVF f1, f2, f3, cons;
    cons.set(3200.0f, 0.8f);
    const int uni = std::clamp(st.unison, 1, 4);
    double ph[4] = {0.0, 0.31, 0.67, 0.13};
    double sub = 0, up = 0;
    const bool glide = prevMidi > 0 && std::fabs(prevMidi - midi) <= 7.0;
    const Formant& A = kVowels[std::clamp(vowelA, 0, 4)];
    const Formant& B = kVowels[std::clamp(vowelB, 0, 4)];
    const float dn = st.drive > 1.0f ? std::tanh(st.drive) : 1.0f;
    for (size_t k = 0; k < n; ++k) {
        const double tt = double(k) / SR;
        double m = midi;
        if (glide) m += (prevMidi - midi) * std::exp(-tt / 0.028);
        const double vd = st.vibDepth * std::clamp((tt - 0.12) / 0.2, 0.0, 1.0);
        m += vd * std::sin(kTwoPi * st.vibHz * tt);
        if ((k & 31) == 0) {
            const float x = float(std::clamp(tt / std::max(0.05, dur), 0.0, 1.0));
            const float sh = st.formantShift;
            f1.set((A.f[0] + (B.f[0] - A.f[0]) * x) * sh, 7.0f);
            f2.set((A.f[1] + (B.f[1] - A.f[1]) * x) * sh, 9.0f);
            f3.set((A.f[2] + (B.f[2] - A.f[2]) * x) * sh, 12.0f);
        }
        const double f = midiHz(m);
        float src = 0;
        for (int u = 0; u < uni; ++u) {
            const double c = uni == 1 ? 0.0 : (double(u) / (uni - 1) - 0.5) * 2.0 * st.detuneCents;
            const double dt = f * std::pow(2.0, c / 1200.0) / SR;
            ph[u] += dt; if (ph[u] >= 1) ph[u] -= 1;
            src += st.pulse > 0 ? sawBL(ph[u], dt) * (1 - st.pulse) + pulseBL(ph[u], dt, 0.22) * st.pulse : sawBL(ph[u], dt);
        }
        src /= float(uni);
        sub += f * 0.5 / SR; if (sub >= 1) sub -= 1;
        up += f * 2.0 / SR; if (up >= 1) up -= 1;
        src += sawBL(sub, f * 0.5 / SR) * st.subOctave;
        src += nz.next() * st.breath;
        float y = f1.process(src) * 1.0f + f2.process(src) * 0.55f + f3.process(src) * 0.22f;
        y += float(std::sin(kTwoPi * up)) * st.upOctave * 0.3f;
        // consonant burst at the start of each syllable
        if (tt < 0.03) y += cons.process(nz.next()) * st.consonant * float(std::exp(-tt * 140.0));
        if (st.drive > 1.0f) y = std::tanh(y * st.drive) / dn;
        float env = float(std::min(1.0, tt / att));
        if (tt > 0.15) env *= 0.88f;
        if (tt > dur) env *= float(std::max(0.0, 1.0 - (tt - dur) / rel));
        const float v = y * env * st.gain;
        out[i0 + k] += v;
        addSend(send, i0 + k, v * st.send);
    }
}

// ----------------------------------------------------------------- effects --

namespace {
struct Comb {
    std::vector<float> buf;
    size_t idx = 0;
    float store = 0, fb = 0.84f, d1 = 0.2f, d2 = 0.8f;
    float run(float in) {
        const float o = buf[idx];
        store = o * d2 + store * d1;
        buf[idx] = in + store * fb;
        if (++idx >= buf.size()) idx = 0;
        return o;
    }
};
struct Allpass {
    std::vector<float> buf;
    size_t idx = 0;
    float run(float in) {
        const float b = buf[idx];
        buf[idx] = in + b * 0.5f;
        if (++idx >= buf.size()) idx = 0;
        return b - in;
    }
};
}  // namespace

void reverb(const Mono& send, float* outL, float* outR, size_t outN, float room, float damp, float wet, float width) {
    static const int combT[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int apT[4] = {556, 441, 341, 225};
    Comb cl[8], cr[8];
    Allpass al[4], ar[4];
    const float fb = room * 0.28f + 0.7f, d = damp * 0.4f;
    for (int i = 0; i < 8; ++i) {
        cl[i].buf.assign(size_t(combT[i]), 0.0f);
        cr[i].buf.assign(size_t(combT[i] + 23), 0.0f);
        cl[i].fb = cr[i].fb = fb;
        cl[i].d1 = cr[i].d1 = d;
        cl[i].d2 = cr[i].d2 = 1.0f - d;
    }
    for (int i = 0; i < 4; ++i) {
        al[i].buf.assign(size_t(apT[i]), 0.0f);
        ar[i].buf.assign(size_t(apT[i] + 23), 0.0f);
    }
    const float w1 = wet * (width * 0.5f + 0.5f), w2 = wet * ((1.0f - width) * 0.5f);
    const size_t n = std::min(send.size(), outN);
    const bool mono = (outL == outR);
    for (size_t i = 0; i < n; ++i) {
        const float in = send[i] * 0.03f;
        float l = 0, r = 0;
        for (int c = 0; c < 8; ++c) { l += cl[c].run(in); r += cr[c].run(in); }
        for (int a = 0; a < 4; ++a) { l = al[a].run(l); r = ar[a].run(r); }
        if (mono) {
            outL[i] += (l + r) * 0.5f * wet;
        } else {
            outL[i] += l * w1 + r * w2;
            outR[i] += r * w1 + l * w2;
        }
    }
}

void pingPong(Stereo& io, double delaySec, float feedback, float mix) {
    const size_t d = std::max<size_t>(1, size_t(delaySec * SR));
    std::vector<float> bl(d, 0.0f), br(d, 0.0f);
    size_t idx = 0;
    for (size_t i = 0; i < io.size(); ++i) {
        const float dl = bl[idx], dr = br[idx];
        bl[idx] = (io.l[i] + io.r[i]) * 0.5f + dr * feedback;
        br[idx] = dl * feedback;
        io.l[i] += dl * mix;
        io.r[i] += dr * mix;
        if (++idx >= d) idx = 0;
    }
}

std::vector<int16_t> toPcm16(const Stereo& s, float peakTarget, float maxGain) {
    float peak = 1e-6f;
    for (size_t i = 0; i < s.size(); ++i) peak = std::max(peak, std::max(std::fabs(s.l[i]), std::fabs(s.r[i])));
    const float g = std::min(maxGain, peakTarget / peak);
    std::vector<int16_t> out(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        out[2 * i] = int16_t(std::clamp(s.l[i] * g, -1.0f, 1.0f) * 32767.0f);
        out[2 * i + 1] = int16_t(std::clamp(s.r[i] * g, -1.0f, 1.0f) * 32767.0f);
    }
    return out;
}

std::vector<int16_t> monoToPcm16(const Mono& s, float peakTarget) {
    float peak = 1e-6f;
    for (float v : s) peak = std::max(peak, std::fabs(v));
    const float g = peakTarget / peak;
    std::vector<int16_t> out(s.size());
    for (size_t i = 0; i < s.size(); ++i) out[i] = int16_t(std::clamp(s[i] * g, -1.0f, 1.0f) * 32767.0f);
    return out;
}

}  // namespace gh::synth
