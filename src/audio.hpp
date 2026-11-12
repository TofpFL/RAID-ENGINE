// Audio: a small mixer that owns the song clock, plus generated sound effects.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gh {

struct PcmStream;  // audio_decode.hpp

// 16-bit PCM at 44.1 kHz, mono or interleaved stereo: all of it in `data`,
// or (a song file) decoded in the background into `stream` while it plays.
struct Pcm {
    std::vector<int16_t> data;
    int channels = 2;
    std::shared_ptr<PcmStream> stream;  // set: `data` stays empty
    size_t streamFrames = 0;            // the stream's length
    size_t frames() const { return stream ? streamFrames : (channels > 0 ? data.size() / size_t(channels) : 0); }
};
using PcmPtr = std::shared_ptr<const Pcm>;

// Decodes a whole ogg/mp3/wav (or qoa, through raylib) file to 44.1 kHz.
bool loadAudioFile(const std::string& path, Pcm& out, std::string& err);
// A song file: streamed (playable at once) when it is ogg/mp3/wav, else
// decoded whole. `startFraction`: where playing will start. Null on failure.
PcmPtr loadSongAudio(const std::string& path, double startFraction = 0.0);

// The mixer feeds one raylib AudioStream from a callback and counts the exact
// number of frames handed to the device; time() derives a smooth, monotonic
// song position from that counter. Without an audio device it falls back to a
// steady clock so the game stays playable (and testable) anyway.
class Mixer {
public:
    static constexpr int kRate = 44100;
    enum Track { kInst = 0, kVoicePlayer = 1, kVoiceOpponent = 2, kTrackCount = 3 };

    bool init();
    void shutdown();
    bool audioOk() const { return audioOk_; }

    void setTracks(PcmPtr inst, PcmPtr voicePlayer, PcmPtr voiceOpponent);
    void clear() { setTracks(nullptr, nullptr, nullptr); }
    void play(double fromSec);          // may be negative (silent lead-in)
    void pause();
    void resume();
    void stop();
    void seek(double sec);
    bool playing() const { return playing_.load(); }
    bool paused() const { return paused_; }
    void setLoop(bool loop, double startSec = 0, double endSec = -1);
    void setRate(double r);
    double rate() const { return rate_.load(); }
    void setTrackVolume(int track, float v);
    void setVolume(float v) { volume_.store(v); }  // music volume (master is raylib's)
    void fadeTo(float v, float seconds);           // smooth volume ramp

    double time(double now);  // smoothed position in seconds
    double rawTime() const;   // position of the last mixed frame
    double lengthSec() const;
    bool ended() const;
    // A streamed track has not decoded the part being played yet (after a
    // jump far ahead): the song clock waits for it.
    bool waitingForAudio() const { return stalled_.load(); }

    void audioCallback(float* out, unsigned frames);  // audio thread

private:
    double estimate(double now) const;

    std::mutex mtx_;  // guards track pointers
    PcmPtr tracks_[kTrackCount];
    std::atomic<double> pos_{0.0};        // frames
    std::atomic<double> cbTime_{0.0};     // steady time of last callback
    std::atomic<unsigned> cbFrames_{0};
    std::atomic<bool> playing_{false};
    std::atomic<double> rate_{1.0};
    std::atomic<float> trackVol_[kTrackCount];
    std::atomic<float> volume_{1.0f};
    std::atomic<float> fadeTarget_{1.0f};
    std::atomic<float> fadeStep_{0.0f};
    std::atomic<bool> loop_{false};
    std::atomic<bool> stalled_{false};
    void hintStreams(double sec);  // tells streamed tracks where playing goes on
    std::atomic<double> loopStart_{0}, loopEnd_{0};
    float curVol_[kTrackCount] = {1, 1, 1};
    float fadeGain_ = 1.0f;
    bool audioOk_ = false;
    bool paused_ = false;
    void* stream_ = nullptr;  // AudioStream*
    // fallback clock
    double fbStart_ = 0, fbStartPos_ = 0;
    // smoothing
    bool haveLast_ = false;
    double lastValue_ = 0, lastNow_ = 0;
};

// Procedurally generated sound effects.
enum class Sfx { Scroll, Confirm, Cancel, Hit, Miss, Mine, Count3, Count2, Count1, CountGo, Death, Unlock, kCount };
void sfxInit();
void sfxShutdown();
void sfxPlay(Sfx s, float volume = 1.0f, float pitch = 1.0f);
void sfxSetVolume(float v);
// Plays the sound in `path` (any audio file the game reads) for `id` from
// now on; an empty path goes back to the game's own sound.
bool sfxOverride(Sfx id, const std::string& path);
void sfxClearOverrides();

}  // namespace gh
