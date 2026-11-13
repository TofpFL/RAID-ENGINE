// Video playback for cutscenes (startVideo) and video sprites
// (makeVideoSprite). MPEG-1 files (.mpg) play directly through pl_mpeg; other
// formats (.mp4, .webm, .mov...) are converted once to MPEG-1 with ffmpeg
// when it is installed (on the PATH or next to the game), into cache/videos/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "raylib.h"
#include "scene.hpp"

typedef struct plm_t plm_t;

namespace gh {

class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    bool open(const std::string& mpgPath, bool loop, bool withAudio = true);
    void update(double dt);  // decodes up to the new time and uploads the frame
    bool ended() const;
    const Texture2D& texture() const { return tex_; }
    int width() const { return w_; }
    int height() const { return h_; }
    double duration() const;
    void setVolume(float v);
    void close();

    // pl_mpeg callbacks
    void onVideo(void* frame);
    void onAudio(const float* interleaved, unsigned frames);

private:
    plm_t* plm_ = nullptr;
    Texture2D tex_{};
    std::vector<uint8_t> rgba_;
    bool frameDirty_ = false;
    int w_ = 0, h_ = 0;
    AudioStream stream_{};
    bool audio_ = false;
    std::vector<float> pending_;  // decoded samples waiting for the device (stereo)
    float volume_ = 1.0f;
};

// Finds videos/<name> in the mod. `playable` is an .mpg to open now; while a
// conversion runs, `converting` is true (ask again later); `error` says why
// the video cannot play.
struct VideoSource {
    std::string playable, error;
    bool converting = false;
};
VideoSource findVideo(const scene::Files& files, const std::string& name, const std::string& baseDir);

}  // namespace gh
