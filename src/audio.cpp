#include "audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common.hpp"
#include "platform.hpp"
#include "raylib.h"
#include "synth.hpp"

namespace gh {

// ------------------------------------------------------------- decoding --

bool loadAudioFile(const std::string& path, Pcm& out, std::string& err) {
    Wave w = LoadWave(path.c_str());
    if (w.data == nullptr || w.frameCount == 0) {
        err = "could not decode audio: " + path;
        if (w.data) UnloadWave(w);
        return false;
    }
    WaveFormat(&w, Mixer::kRate, 16, 2);
    out.channels = 2;
    const int16_t* p = static_cast<const int16_t*>(w.data);
    out.data.assign(p, p + size_t(w.frameCount) * 2);
    UnloadWave(w);
    return true;
}

// ---------------------------------------------------------------- mixer --

static Mixer* g_mixer = nullptr;
static std::atomic<unsigned> g_seq{0};

static void streamCallback(void* buffer, unsigned int frames) {
    if (g_mixer) g_mixer->audioCallback(static_cast<float*>(buffer), frames);
    else std::memset(buffer, 0, size_t(frames) * 2 * sizeof(float));
}

bool Mixer::init() {
    g_mixer = this;
    for (auto& v : trackVol_) v.store(1.0f);
    audioOk_ = false;
    if (!IsAudioDeviceReady()) return false;
    auto* s = new AudioStream(LoadAudioStream(kRate, 32, 2));
    if (!IsAudioStreamValid(*s)) {
        delete s;
        return false;
    }
    SetAudioStreamCallback(*s, streamCallback);
    PlayAudioStream(*s);
    stream_ = s;
    audioOk_ = true;
    return true;
}

void Mixer::shutdown() {
    playing_ = false;
    if (stream_) {
        auto* s = static_cast<AudioStream*>(stream_);
        StopAudioStream(*s);
        UnloadAudioStream(*s);
        delete s;
        stream_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& t : tracks_) t.reset();
    }
    g_mixer = nullptr;
}

void Mixer::setTracks(PcmPtr inst, PcmPtr vp, PcmPtr vo) {
    playing_ = false;
    paused_ = false;
    std::lock_guard<std::mutex> lk(mtx_);
    tracks_[kInst] = std::move(inst);
    tracks_[kVoicePlayer] = std::move(vp);
    tracks_[kVoiceOpponent] = std::move(vo);
    pos_ = 0.0;
    loop_ = false;
    for (int i = 0; i < kTrackCount; ++i) { trackVol_[i] = 1.0f; curVol_[i] = 1.0f; }
    fadeGain_ = 1.0f;
    fadeTarget_ = 1.0f;
    fadeStep_ = 0.0f;
    haveLast_ = false;
}

void Mixer::play(double fromSec) {
    pos_ = fromSec * kRate;
    fbStart_ = nowSec();
    fbStartPos_ = fromSec;
    paused_ = false;
    haveLast_ = false;
    playing_ = true;
}

void Mixer::pause() {
    if (!playing_) return;
    if (!audioOk_) fbStartPos_ = fbStartPos_ + (nowSec() - fbStart_) * rate_.load();
    playing_ = false;
    paused_ = true;
}

void Mixer::resume() {
    if (!paused_) return;
    fbStart_ = nowSec();
    paused_ = false;
    haveLast_ = false;
    playing_ = true;
}

void Mixer::stop() {
    playing_ = false;
    paused_ = false;
    pos_ = 0.0;
    fbStartPos_ = 0;
}

void Mixer::seek(double sec) {
    pos_ = sec * kRate;
    fbStart_ = nowSec();
    fbStartPos_ = sec;
    haveLast_ = false;
}

void Mixer::setLoop(bool loop, double startSec, double endSec) {
    if (endSec < 0) endSec = lengthSec();
    loopStart_ = startSec * kRate;
    loopEnd_ = endSec * kRate;
    loop_ = loop && endSec > startSec;
}

void Mixer::setRate(double r) {
    r = clampv(r, 0.25, 4.0);
    if (!audioOk_ && playing_) {
        const double now = nowSec();
        fbStartPos_ = fbStartPos_ + (now - fbStart_) * rate_.load();
        fbStart_ = now;
    }
    rate_ = r;
    haveLast_ = false;
}

void Mixer::setTrackVolume(int track, float v) {
    if (track >= 0 && track < kTrackCount) trackVol_[track] = v;
}

void Mixer::fadeTo(float v, float seconds) {
    fadeTarget_ = v;
    fadeStep_ = seconds <= 0 ? 1.0f : 1.0f / (seconds * kRate);
    if (!audioOk_) fadeGain_ = v;
}

double Mixer::lengthSec() const {
    // tracks_ is only replaced on the main thread, so reading here is safe.
    const PcmPtr& t = tracks_[kInst];
    return t ? double(t->frames()) / kRate : 0.0;
}

double Mixer::rawTime() const {
    if (!audioOk_) {
        if (!playing_) return fbStartPos_;
        return fbStartPos_ + (nowSec() - fbStart_) * rate_.load();
    }
    return pos_.load() / kRate;
}

bool Mixer::ended() const {
    const double len = lengthSec();
    return !loop_ && len > 0 && rawTime() >= len;
}

double Mixer::estimate(double now) const {
    if (!audioOk_) {
        if (!playing_) return fbStartPos_;
        return fbStartPos_ + (now - fbStart_) * rate_.load();
    }
    double pos, t;
    unsigned n;
    for (;;) {
        const unsigned s0 = g_seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        pos = pos_.load(std::memory_order_relaxed);
        t = cbTime_.load(std::memory_order_relaxed);
        n = cbFrames_.load(std::memory_order_relaxed);
        if (g_seq.load(std::memory_order_acquire) == s0) break;
    }
    const double end = pos / kRate;
    if (!playing_) return end;
    double est = end - double(n) * rate_.load() / kRate + (now - t) * rate_.load();
    return std::min(est, end);
}

double Mixer::time(double now) {
    const double m = estimate(now);
    if (!playing_ || !haveLast_) {
        haveLast_ = playing_.load();
        lastValue_ = m;
        lastNow_ = now;
        return m;
    }
    const double dt = std::max(0.0, now - lastNow_);
    const double pred = lastValue_ + dt * rate_.load();
    const double diff = m - pred;
    double v;
    if (std::fabs(diff) > 0.035) {
        v = m;  // hard resync (seek, loop, hiccup)
    } else {
        v = pred + diff * std::min(1.0, dt * 6.0);
        v = std::max(v, lastValue_);
    }
    lastValue_ = v;
    lastNow_ = now;
    return v;
}

static inline float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.0f);
    return x < 0 ? -y : y;
}

void Mixer::audioCallback(float* out, unsigned frames) {
    const double now = nowSec();
    auto publish = [&](double pos) {
        g_seq.fetch_add(1, std::memory_order_acq_rel);
        pos_.store(pos, std::memory_order_relaxed);
        cbTime_.store(now, std::memory_order_relaxed);
        cbFrames_.store(frames, std::memory_order_relaxed);
        g_seq.fetch_add(1, std::memory_order_acq_rel);
    };
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    // In the background (Android) the song holds its place instead of playing on.
    if (!playing_ || !lk.owns_lock() || platform::inBackground()) {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
        publish(pos_.load());
        return;
    }
    double pos = pos_.load();
    const double rate = rate_.load();
    const bool loop = loop_.load();
    const double ls = loopStart_.load(), le = loopEnd_.load();
    const float vol = volume_.load();
    const float fTarget = fadeTarget_.load(), fStep = fadeStep_.load();
    float tv[kTrackCount];
    const Pcm* tr[kTrackCount];
    for (int t = 0; t < kTrackCount; ++t) {
        tv[t] = trackVol_[t].load();
        tr[t] = tracks_[t].get();
    }
    for (unsigned f = 0; f < frames; ++f) {
        float l = 0, r = 0;
        for (int t = 0; t < kTrackCount; ++t) curVol_[t] += (tv[t] - curVol_[t]) * 0.004f;
        if (pos >= 0) {
            const size_t i = size_t(pos);
            const float fr = float(pos - double(i));
            for (int t = 0; t < kTrackCount; ++t) {
                const Pcm* p = tr[t];
                if (!p || curVol_[t] < 1e-4f) continue;
                const size_t n = p->frames();
                if (i >= n) continue;
                const int16_t* d = p->data.data();
                const float g = curVol_[t] * (1.0f / 32768.0f);
                if (p->channels == 2) {
                    const size_t j = (i + 1 < n) ? i + 1 : i;
                    l += (d[2 * i] + (d[2 * j] - d[2 * i]) * fr) * g;
                    r += (d[2 * i + 1] + (d[2 * j + 1] - d[2 * i + 1]) * fr) * g;
                } else {
                    const size_t j = (i + 1 < n) ? i + 1 : i;
                    const float s = (d[i] + (d[j] - d[i]) * fr) * g;
                    l += s;
                    r += s;
                }
            }
        }
        if (fadeGain_ < fTarget) fadeGain_ = std::min(fTarget, fadeGain_ + fStep);
        else if (fadeGain_ > fTarget) fadeGain_ = std::max(fTarget, fadeGain_ - fStep);
        const float g = vol * fadeGain_;
        out[2 * f] = softClip(l * g);
        out[2 * f + 1] = softClip(r * g);
        pos += rate;
        if (loop && le > ls && pos >= le) pos -= (le - ls);
    }
    publish(pos);
}

// ------------------------------------------------------------------ sfx --

namespace {
constexpr int kAliases = 6;
struct SfxSlot {
    Sound base{};
    Sound alias[kAliases]{};
    int next = 0;
    bool ok = false;
};
SfxSlot g_sfx[int(Sfx::kCount)];
float g_sfxVol = 0.7f;

void loadSlot(Sfx id, const synth::Stereo& s) {
    std::vector<int16_t> mono(s.size());
    float peak = 1e-6f;
    for (size_t i = 0; i < s.size(); ++i) peak = std::max(peak, std::fabs(s.l[i] + s.r[i]) * 0.5f);
    const float g = std::min(4.0f, 0.8f / peak);
    for (size_t i = 0; i < s.size(); ++i)
        mono[i] = int16_t(std::clamp((s.l[i] + s.r[i]) * 0.5f * g, -1.0f, 1.0f) * 32767.0f);
    Wave w{};
    w.frameCount = unsigned(mono.size());
    w.sampleRate = synth::SR;
    w.sampleSize = 16;
    w.channels = 1;
    w.data = mono.data();
    SfxSlot& slot = g_sfx[int(id)];
    slot.base = LoadSoundFromWave(w);
    slot.ok = IsSoundValid(slot.base);
    if (!slot.ok) return;
    for (auto& a : slot.alias) a = LoadSoundAlias(slot.base);
}
}  // namespace

void sfxInit() {
    if (!IsAudioDeviceReady()) return;
    using namespace synth;
    auto buf = [](double sec) { return Stereo(size_t(sec * SR)); };
    {
        Stereo s = buf(0.12);
        bell(s, nullptr, 0, 96, 0.5f, 0);
        loadSlot(Sfx::Scroll, s);
    }
    {
        Stereo s = buf(1.2);
        bell(s, nullptr, 0, 76, 0.8f, -0.2f);
        bell(s, nullptr, 0.05, 79, 0.7f, 0.2f);
        bell(s, nullptr, 0.1, 83, 0.7f, 0);
        bell(s, nullptr, 0.15, 88, 0.6f, 0);
        loadSlot(Sfx::Confirm, s);
    }
    {
        Stereo s = buf(0.35);
        pluck(s, nullptr, 0, 0.08, 69, 0.9f, 0, 1);
        pluck(s, nullptr, 0.08, 0.12, 62, 0.9f, 0, 1);
        loadSlot(Sfx::Cancel, s);
    }
    {
        Stereo s = buf(0.08);
        for (size_t i = 0; i < s.size(); ++i) {
            const double t = double(i) / SR;
            const float v = float(std::sin(2 * kPi * 2100 * t) * std::exp(-t * 90.0));
            s.l[i] = s.r[i] = v;
        }
        loadSlot(Sfx::Hit, s);
    }
    {
        Stereo s = buf(0.35);
        tom(s, nullptr, 0, 95, 0.9f);
        snare(s, nullptr, 0.0, 0.25f);
        loadSlot(Sfx::Miss, s);
    }
    {
        Stereo s = buf(0.3);
        for (size_t i = 0; i < s.size(); ++i) {
            const double t = double(i) / SR;
            const float sq = std::fmod(t * 110.0, 1.0) < 0.5 ? 1.0f : -1.0f;
            const float v = std::tanh(sq * 2.0f + float(std::sin(2 * kPi * 233 * t))) * float(std::exp(-t * 9.0));
            s.l[i] = s.r[i] = v * 0.6f;
        }
        loadSlot(Sfx::Mine, s);
    }
    const double cnt[3] = {69, 71, 74};
    for (int k = 0; k < 3; ++k) {
        Stereo s = buf(1.0);
        bell(s, nullptr, 0, cnt[k], 0.9f, 0);
        bell(s, nullptr, 0, cnt[k] + 12, 0.4f, 0);
        loadSlot(Sfx(int(Sfx::Count3) + k), s);
    }
    {
        Stereo s = buf(2.0);
        for (int n : {62, 66, 69, 74, 78}) bell(s, nullptr, 0, n, 0.7f, 0);
        crash(s, nullptr, 0, 0.6f);
        loadSlot(Sfx::CountGo, s);
    }
    {
        Stereo s = buf(3.0);
        impact(s, nullptr, 0, 1.0f);
        padNote(s, nullptr, 0.1, 2.0, 38, 1.0f, 3, 0);
        padNote(s, nullptr, 0.1, 2.0, 39, 0.8f, 3, 0);
        loadSlot(Sfx::Death, s);
    }
    {
        Stereo s = buf(1.6);
        const int notes[6] = {74, 78, 81, 86, 90, 93};
        for (int k = 0; k < 6; ++k) bell(s, nullptr, k * 0.06, notes[k], 0.6f, (k % 2) ? 0.4f : -0.4f);
        loadSlot(Sfx::Unlock, s);
    }
}

void sfxShutdown() {
    for (SfxSlot& s : g_sfx) {
        if (!s.ok) continue;
        for (auto& a : s.alias) UnloadSoundAlias(a);
        UnloadSound(s.base);
        s.ok = false;
    }
}

void sfxSetVolume(float v) { g_sfxVol = v; }

void sfxPlay(Sfx id, float volume, float pitch) {
    SfxSlot& s = g_sfx[int(id)];
    if (!s.ok) return;
    Sound& snd = s.alias[s.next];
    s.next = (s.next + 1) % kAliases;
    SetSoundVolume(snd, std::clamp(volume * g_sfxVol, 0.0f, 1.0f));
    SetSoundPitch(snd, pitch);
    PlaySound(snd);
}

}  // namespace gh
