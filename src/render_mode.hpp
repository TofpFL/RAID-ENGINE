// Rendering mode, as in JS Engine, H-Slice and S2X: a song plays by itself
// with its time cut loose from real time (every frame is one 1/fps step), and
// every frame goes to ffmpeg, so the video is smooth however slowly the
// computer draws it. The game's mixer mixes the song's audio alongside, one
// frame's worth at a time, and it is put into the video at the end.
// Desktop only: Android has no ffmpeg to run.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "process.hpp"
#include "raylib.h"

namespace gh {

struct RenderOptions {
    int width = 1280, height = 720;
    int fps = 60;
    int encoder = 0;  // index into renderEncoders()
    int bitrateMbps = 20;
    bool audio = true;
};

// The encoders on offer: the label in the options and the container they use.
struct RenderEncoder {
    const char* label;
    const char* ext;  // "mp4" or "webm"
};
const std::vector<RenderEncoder>& renderEncoders();

// Audio frames (44.1 kHz) that go with video frame `frame` at `fps`: a second
// of video always gets exactly 44100.
unsigned renderAudioFrames(long long frame, int fps);

// The ffmpeg arguments that encode raw RGBA frames read from standard input
// (flipped: render textures are stored bottom row first) into `out`.
std::vector<std::string> renderVideoArgs(const RenderOptions& o, const std::string& out);
// The arguments that put `wav` into the finished video (`video` -> `out`).
std::vector<std::string> renderMuxArgs(const RenderOptions& o, const std::string& video, const std::string& wav,
                                       const std::string& out);

class VideoRenderer {
public:
    VideoRenderer() = default;
    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;
    ~VideoRenderer();

    // ffmpeg: its path; outBase: the video's path without an extension.
    bool start(const std::string& ffmpeg, const std::string& outBase, const RenderOptions& o, std::string& err);
    bool active() const { return active_; }
    double frameTime() const { return 1.0 / opt_.fps; }
    long long frames() const { return frames_; }
    const RenderOptions& options() const { return opt_; }
    const RenderTexture2D& target() const { return rt_; }
    // Audio frames (44.1 kHz) that go with video frame `frame`.
    unsigned audioFramesFor(long long frame) const;
    // The frame drawn into target() goes to ffmpeg; false once ffmpeg stopped.
    bool captureFrame();
    void addAudio(const float* stereo, unsigned frames);
    // Ends the video. Its audio is added in the background: the message from
    // takeMessage() says when the file is ready. Returns the final path, or ""
    // with the reason in `err`.
    std::string finish(std::string& err);

    static bool busy();                      // a video is still being finished
    static void waitIdle(double maxSeconds);  // game exit
    static std::string takeMessage();         // "Video saved: ..." (once)

private:
    bool active_ = false;
    RenderOptions opt_;
    std::string ffmpeg_, outBase_, videoTmp_, wavTmp_, final_, log_;
    PipeProcess proc_;
    RenderTexture2D rt_{};
    long long frames_ = 0;
    FILE* wav_ = nullptr;
    uint64_t audioFrames_ = 0;
    std::vector<int16_t> pcm_;
    void closeWav();
};

}  // namespace gh
