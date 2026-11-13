// Song audio decoded while it plays (audio_decode.cpp): the streamed samples
// match the file, resampling, decoding where the song plays first, and long
// tracks keeping only a window in memory. Built when raylib (whose decoders
// these are) is part of the build.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "audio.hpp"
#include "audio_decode.hpp"
#include "process.hpp"
#include "render_mode.hpp"

using namespace gh;
namespace fs = std::filesystem;

extern int g_fail, g_pass;
#define ACHECK(cond)                                                             \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

namespace {

int16_t sampleL(size_t k) { return int16_t(int((k * 7) % 65536) - 32768); }
int16_t sampleR(size_t k) { return int16_t(int((k * 13 + 5) % 65536) - 32768); }
int16_t sampleM(size_t k) { return int16_t(int((k * 3) % 20000) - 10000); }

bool writeTestWav(const std::string& path, unsigned rate, unsigned ch, size_t frames, bool stereoPattern) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t bytes = uint32_t(frames * ch * 2);
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(uint16_t(ch)); w32(rate); w32(rate * ch * 2); w16(uint16_t(ch * 2)); w16(16);
    std::fwrite("data", 1, 4, f);
    w32(bytes);
    std::vector<int16_t> d(frames * ch);
    for (size_t k = 0; k < frames; ++k) {
        if (stereoPattern) {
            d[k * ch] = sampleL(k);
            d[k * ch + 1] = sampleR(k);
        } else {
            d[k * ch] = sampleM(k);
        }
    }
    std::fwrite(d.data(), 2, d.size(), f);
    return std::fclose(f) == 0;
}

// Waits until block b of the stream is decoded (or 5 s pass).
bool waitBlock(const PcmStream& s, size_t b) {
    for (int i = 0; i < 1000; ++i) {
        if (s.get(b)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool waitFinished(const PcmStream& s) {
    for (int i = 0; i < 1000 && !s.finished.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return s.finished.load();
}

void testStreamExact(const std::string& dir) {
    // Stereo at 44.1 kHz: every streamed frame is the file's.
    const std::string path = dir + "/stereo.wav";
    const size_t frames = 100000;
    ACHECK(writeTestWav(path, 44100, 2, frames, true));
    auto pcm = openAudioStream(path, 0.5);
    ACHECK(pcm && pcm->stream);
    if (!pcm || !pcm->stream) return;
    ACHECK(pcm->frames() == frames);
    ACHECK(pcm->channels == 2);
    const PcmStream& s = *pcm->stream;
    // Decoding starts where playing will (half way), then does the rest.
    ACHECK(waitFinished(s));
    bool same = true;
    for (size_t k = 0; k < frames && same; ++k) {
        const size_t b = k / PcmStream::kBlockFrames, o = k % PcmStream::kBlockFrames;
        const int16_t* p = s.get(b);
        same = p && p[o * 2] == sampleL(k) && p[o * 2 + 1] == sampleR(k);
    }
    ACHECK(same);
    ACHECK(s.ready(0, frames));

    // The whole-file decoder gives the same samples.
    Pcm whole;
    std::string err;
    ACHECK(decodeAudioFile(path, whole, err, size_t(1) << 30));
    ACHECK(whole.frames() == frames && whole.channels == 2 && !whole.stream);
    ACHECK(whole.data.size() == frames * 2 && whole.data[2 * 777] == sampleL(777) && whole.data[2 * 777 + 1] == sampleR(777));
    ACHECK(!decodeAudioFile(path, whole, err, 1000) && !err.empty());  // over the limit: refused, with a reason

    // A streamed track written back as WAV is decoded again from its file.
    const std::string back = dir + "/back.wav";
    ACHECK(writeWavFile(back, *pcm));
    Pcm again;
    ACHECK(decodeAudioFile(back, again, err, size_t(1) << 30));
    ACHECK(again.data == whole.data);
}

void testResample(const std::string& dir) {
    // Mono at 22.05 kHz: twice the frames at 44.1 kHz, linearly interpolated.
    const std::string path = dir + "/mono22.wav";
    const size_t frames = 50000;
    ACHECK(writeTestWav(path, 22050, 1, frames, false));
    auto pcm = openAudioStream(path);
    ACHECK(pcm && pcm->stream);
    if (!pcm || !pcm->stream) return;
    ACHECK(pcm->frames() == frames * 2);
    ACHECK(pcm->channels == 1);
    ACHECK(waitFinished(*pcm->stream));
    bool same = true;
    for (size_t o = 0; o + 2 < frames * 2 && same; ++o) {
        const size_t i = o / 2;
        const int a = sampleM(i), b = sampleM(i + 1);
        const int want = (o % 2) ? a + ((b - a) * 22050) / 44100 : a;
        const int16_t* p = pcm->stream->get(o / PcmStream::kBlockFrames);
        same = p && p[o % PcmStream::kBlockFrames] == want;
        if (!same) std::printf("  resample differs at %zu: %d vs %d\n", o, p ? p[o % PcmStream::kBlockFrames] : -1, want);
    }
    ACHECK(same);
}

void testWindow(const std::string& dir) {
    // A track too long to keep whole: decoded around the play position only.
    const std::string path = dir + "/long.wav";
    const size_t frames = 6000000;  // 367 blocks
    ACHECK(writeTestWav(path, 44100, 1, frames, false));
    auto pcm = openAudioStream(path, 0.0, 1 << 20);
    ACHECK(pcm && pcm->stream && pcm->stream->windowed);
    if (!pcm || !pcm->stream) return;
    PcmStream& s = *pcm->stream;
    const size_t B = PcmStream::kBlockFrames;
    ACHECK(waitBlock(s, PcmStream::kAheadBlocks - 1));  // the window ahead of the start fills...
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ACHECK(!s.get(PcmStream::kAheadBlocks + 5));         // ...and stops there
    ACHECK(s.inMemory.load() <= PcmStream::kAheadBlocks + 1);

    // A jump far ahead (a seek): that part comes next.
    s.playPos.store(300 * B + 123);
    ACHECK(waitBlock(s, 300));
    ACHECK(waitBlock(s, s.blocks - 1));
    const int16_t* p = s.get(300);
    ACHECK(p && p[0] == sampleM(300 * B) && p[B - 1] == sampleM(301 * B - 1));
    const int16_t* last = s.get(s.blocks - 1);
    const size_t lastFrames = s.framesIn(s.blocks - 1);
    ACHECK(last && lastFrames == frames - (s.blocks - 1) * B && last[lastFrames - 1] == sampleM(frames - 1));

    // Back to the middle: past the memory budget, the blocks far from the
    // play position (the start, the end) go away.
    s.playPos.store(170 * B);
    ACHECK(waitBlock(s, 299));
    bool evicted = false;
    for (int i = 0; i < 400 && !evicted; ++i) {
        evicted = !s.get(0) && !s.get(s.blocks - 1);
        if (!evicted) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ACHECK(evicted);
    ACHECK(s.get(170) && s.get(170 - PcmStream::kBehindBlocks + 1));  // the window stays
    ACHECK(s.inMemory.load() <= PcmStream::kBehindBlocks + PcmStream::kAheadBlocks + 16);

    // Back to the start (a restart): decoded again.
    s.playPos.store(0);
    ACHECK(waitBlock(s, 0));
    p = s.get(0);
    ACHECK(p && p[5] == sampleM(5));
}

void testLoopHold(const std::string& dir) {
    // A long track looping (the Freeplay preview): the loop's start stays in
    // memory while the window moves on, so the jump back never waits.
    const std::string path = dir + "/long.wav";  // written by testWindow
    auto pcm = openAudioStream(path, 0.0, 1 << 20);
    ACHECK(pcm && pcm->stream && pcm->stream->windowed);
    if (!pcm || !pcm->stream) return;
    PcmStream& s = *pcm->stream;
    const size_t B = PcmStream::kBlockFrames;
    s.holdPos.store(2 * B + 100);
    s.playPos.store(300 * B);
    ACHECK(waitBlock(s, 300) && waitBlock(s, s.blocks - 1));
    ACHECK(waitBlock(s, 2) && waitBlock(s, 2 + PcmStream::kHoldBlocks - 1));
    s.playPos.store(170 * B);  // more memory needed: the far blocks go, the held ones stay
    ACHECK(waitBlock(s, 299));
    bool evicted = false;
    for (int i = 0; i < 400 && !evicted; ++i) {
        evicted = !s.get(0) && !s.get(2 + PcmStream::kHoldBlocks);
        if (!evicted) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ACHECK(evicted);
    ACHECK(s.get(2) && s.get(2 + PcmStream::kHoldBlocks - 1));
    const int16_t* p = s.get(2);
    ACHECK(p && p[100] == sampleM(2 * B + 100));
}

void testNotAudio(const std::string& dir) {
    const std::string path = dir + "/notes.txt";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fputs("not audio at all", f);
        std::fclose(f);
    }
    ACHECK(!openAudioStream(path));
    Pcm p;
    std::string err;
    ACHECK(!decodeAudioFile(path, p, err, size_t(1) << 30) && err.empty());  // not ours: the caller tries raylib
    ACHECK(!openAudioStream(dir + "/missing.ogg"));
}

void testRenderMode(const std::string& dir) {
    // A second of video gets exactly a second of audio, whatever the frame rate.
    for (int fps : {24, 30, 60, 120, 144, 240}) {
        uint64_t sum = 0;
        unsigned lo = ~0u, hi = 0;
        for (long long k = 0; k < fps * 3; ++k) {
            const unsigned n = renderAudioFrames(k, fps);
            sum += n;
            lo = std::min(lo, n);
            hi = std::max(hi, n);
        }
        ACHECK(sum == 3 * 44100);
        ACHECK(hi - lo <= 1);
    }
    RenderOptions o;
    o.width = 1920;
    o.height = 1080;
    o.fps = 60;
    o.bitrateMbps = 20;
    std::vector<std::string> a = renderVideoArgs(o, "out.mp4");
    auto has = [&](const std::string& x) { return std::find(a.begin(), a.end(), x) != a.end(); };
    ACHECK(has("1920x1080") && has("libx264") && has("20M") && has("vflip") && a.back() == "out.mp4");
    o.encoder = 2;
    a = renderVideoArgs(o, "out.webm");
    ACHECK(has("libvpx-vp9"));
    a = renderMuxArgs(o, "v.webm", "a.wav", "out.webm");
    ACHECK(has("libopus") && has("-shortest") && a.back() == "out.webm");
    ACHECK(renderEncoders().size() == 7 && std::string(renderEncoders()[2].ext) == "webm");
#if !defined(_WIN32)
    // What is written goes to the program's standard input (here cat, into a file).
    const std::string log = dir + "/pipe.txt";
    PipeProcess p;
    ACHECK(p.start("cat", {}, log));
    ACHECK(p.write("frames go here", 14));
    ACHECK(p.finish() == 0);
    FILE* f = std::fopen(log.c_str(), "rb");
    char buf[32] = {};
    if (f) {
        buf[std::fread(buf, 1, sizeof buf - 1, f)] = 0;
        std::fclose(f);
    }
    ACHECK(std::string(buf) == "frames go here");
    ACHECK(!p.write("x", 1));  // after finish nothing goes anywhere
#endif
}

}  // namespace

void runAudioTests() {
    const fs::path dir = fs::temp_directory_path() / "gh_audio_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string d = dir.string();
    startAudioDecoders();
    testStreamExact(d);
    testResample(d);
    testWindow(d);
    testLoopHold(d);
    testNotAudio(d);
    testRenderMode(d);
    stopAudioDecoders();
    ACHECK(liveAudioDecoders() == 0);
    fs::remove_all(dir, ec);
}
