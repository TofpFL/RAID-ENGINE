// Small offline synthesizer used to generate every built-in song and sound
// effect at load time (no audio files are shipped with the game).
#pragma once

#include <cstdint>
#include <vector>

namespace gh::synth {

constexpr int SR = 44100;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
    size_t size() const { return l.size(); }
    void add(size_t i, float vl, float vr) {
        if (i < l.size()) { l[i] += vl; r[i] += vr; }
    }
    void mixIn(const Stereo& o, float g);
};
using Mono = std::vector<float>;

double midiHz(double midi);

// Andrew Simper's trapezoidal state-variable filter.
struct SVF {
    float a1 = 0, a2 = 0, a3 = 0, k = 1, ic1 = 0, ic2 = 0;
    void set(float cutoff, float q);
    // Returns band-pass (normalised to unity peak); low/high via out params.
    float process(float x, float* low = nullptr, float* high = nullptr);
};

struct Noise {
    uint32_t s = 0x12345678u;
    explicit Noise(uint32_t seed = 0x12345678u) : s(seed ? seed : 1u) {}
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return float(int32_t(s)) * (1.0f / 2147483648.0f); }
};

// ---- drums & effects (t = start time in seconds)
void kick(Stereo& out, double t, float vel, float punch);
void snare(Stereo& out, Mono* send, double t, float vel);
void clap(Stereo& out, Mono* send, double t, float vel);
void hat(Stereo& out, double t, float vel, bool open, float pan);
void crash(Stereo& out, Mono* send, double t, float vel);
void tom(Stereo& out, Mono* send, double t, double hz, float vel);
void riser(Stereo& out, double t, double dur, float vel);
void impact(Stereo& out, Mono* send, double t, float vel);

// ---- pitched instruments
void bass(Stereo& out, double t, double dur, double midi, float vel, int style);
void padNote(Stereo& out, Mono* send, double t, double dur, double midi, float vel, int style, float pan);
void bell(Stereo& out, Mono* send, double t, double midi, float vel, float pan);
void pluck(Stereo& out, Mono* send, double t, double dur, double midi, float vel, float pan, int style);

// ---- singing voice
struct VoiceStyle {
    float vibHz = 5.5f, vibDepth = 0.25f;  // semitones
    float formantShift = 1.0f;
    float drive = 1.0f;
    float consonant = 0.35f;
    float gain = 0.5f;
    float send = 0.3f;
    int unison = 1;
    float detuneCents = 0;
    float subOctave = 0, upOctave = 0;
    float pulse = 0;  // 0 = saw, 1 = narrow pulse (robotic)
    float breath = 0;
    int vowels[4] = {0, 1, 3, 0};
};
// Voices are rendered mono (centre) so a muted voice stem is cheap to keep.
void voiceNote(Mono& out, Mono* send, double t, double dur, double midi, double prevMidi, int vowelA, int vowelB,
               const VoiceStyle& st, uint32_t seed);

// Freeverb-style reverb: reads a mono send and adds the wet signal to outL/outR
// (pass the same pointer twice for a mono destination).
void reverb(const Mono& send, float* outL, float* outR, size_t n, float room, float damp, float wet, float width);
void pingPong(Stereo& io, double delaySec, float feedback, float mix);

// Peak-normalise to int16 interleaved stereo.
std::vector<int16_t> toPcm16(const Stereo& s, float peakTarget, float maxGain = 8.0f);
// Mono float -> int16 mono.
std::vector<int16_t> monoToPcm16(const Mono& s, float peakTarget);

}  // namespace gh::synth
