#include "video.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "common.hpp"
#include "process.hpp"


#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

namespace gh {

namespace fs = std::filesystem;

namespace {
fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

FILE* openRead(const std::string& path) {
#if defined(_WIN32)
    return _wfopen(toPath(path).wstring().c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

void videoCb(plm_t*, plm_frame_t* frame, void* user) { static_cast<VideoPlayer*>(user)->onVideo(frame); }
void audioCb(plm_t*, plm_samples_t* s, void* user) { static_cast<VideoPlayer*>(user)->onAudio(s->interleaved, s->count); }

constexpr int kAudioBuffer = 2048;  // frames per device buffer
}  // namespace

VideoPlayer::~VideoPlayer() { close(); }

bool VideoPlayer::open(const std::string& path, bool loop, bool withAudio) {
    close();
    FILE* f = openRead(path);
    if (!f) return false;
    plm_ = plm_create_with_file(f, 1);
    if (!plm_) return false;
    if (!plm_probe(plm_, 5000 * 1024) || plm_get_num_video_streams(plm_) < 1) {
        close();
        return false;
    }
    w_ = plm_get_width(plm_);
    h_ = plm_get_height(plm_);
    if (w_ <= 0 || h_ <= 0) {
        close();
        return false;
    }
    plm_set_loop(plm_, loop);
    plm_set_video_decode_callback(plm_, videoCb, this);
    // plm_frame_to_rgba writes only R, G and B: alpha stays opaque.
    rgba_.assign(size_t(w_) * size_t(h_) * 4, 255);
    Image img{rgba_.data(), w_, h_, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    tex_ = LoadTextureFromImage(img);
    SetTextureFilter(tex_, TEXTURE_FILTER_BILINEAR);
    const int rate = plm_get_samplerate(plm_);
    audio_ = withAudio && IsAudioDeviceReady() && plm_get_num_audio_streams(plm_) > 0 && rate > 0;
    if (audio_) {
        plm_set_audio_enabled(plm_, 1);
        plm_set_audio_stream(plm_, 0);
        plm_set_audio_decode_callback(plm_, audioCb, this);
        plm_set_audio_lead_time(plm_, double(kAudioBuffer) / rate);
        SetAudioStreamBufferSizeDefault(kAudioBuffer);
        stream_ = LoadAudioStream(unsigned(rate), 32, 2);
        SetAudioStreamBufferSizeDefault(0);
        SetAudioStreamVolume(stream_, volume_);
        PlayAudioStream(stream_);
    } else {
        plm_set_audio_enabled(plm_, 0);
    }
    return true;
}

void VideoPlayer::close() {
    if (audio_ && IsAudioStreamValid(stream_)) {
        StopAudioStream(stream_);
        UnloadAudioStream(stream_);
    }
    audio_ = false;
    stream_ = AudioStream{};
    if (tex_.id && IsWindowReady()) UnloadTexture(tex_);
    tex_ = Texture2D{};
    if (plm_) plm_destroy(plm_);
    plm_ = nullptr;
    pending_.clear();
}

void VideoPlayer::onVideo(void* frame) {
    plm_frame_to_rgba(static_cast<plm_frame_t*>(frame), rgba_.data(), w_ * 4);
    frameDirty_ = true;
}

void VideoPlayer::onAudio(const float* interleaved, unsigned frames) {
    pending_.insert(pending_.end(), interleaved, interleaved + size_t(frames) * 2);
}

void VideoPlayer::update(double dt) {
    if (!plm_) return;
    // Long frames (loading hitches) would decode a burst of frames: cap them.
    plm_decode(plm_, std::min(dt, 0.1));
    if (frameDirty_ && tex_.id) {
        UpdateTexture(tex_, rgba_.data());
        frameDirty_ = false;
    }
    if (audio_) {
        while (IsAudioStreamProcessed(stream_)) {
            std::vector<float> chunk(size_t(kAudioBuffer) * 2, 0.0f);
            const size_t n = std::min(chunk.size(), pending_.size());
            std::copy(pending_.begin(), pending_.begin() + std::ptrdiff_t(n), chunk.begin());
            pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(n));
            UpdateAudioStream(stream_, chunk.data(), kAudioBuffer);
        }
        if (pending_.size() > size_t(kAudioBuffer) * 16) pending_.erase(pending_.begin(), pending_.end() - kAudioBuffer * 4);
    }
}

bool VideoPlayer::ended() const { return !plm_ || plm_has_ended(plm_); }
double VideoPlayer::duration() const { return plm_ ? plm_get_duration(plm_) : 0.0; }

void VideoPlayer::setVolume(float v) {
    volume_ = v;
    if (audio_) SetAudioStreamVolume(stream_, v);
}

// ------------------------------------------------------------- conversion --

namespace {

struct Job {
    bool running = true, ok = false;
};
std::mutex g_jobsMtx;
std::map<std::string, std::shared_ptr<Job>> g_jobs;  // by output path

std::string findFfmpeg(const std::string& baseDir) {
    static std::string cached;
    static bool probed = false;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (!probed) cached = findProgram(baseDir, "ffmpeg", "-version");
    probed = true;
    return cached;
}

}  // namespace

VideoSource findVideo(const scene::Files& files, const std::string& nameIn, const std::string& baseDir) {
    VideoSource out;
    std::string name = nameIn;
    for (const char* ext : {".mp4", ".webm", ".mpg", ".mpeg", ".mov", ".avi", ".mkv"})
        if (endsWithI(name, ext)) name.resize(name.size() - std::string_view(ext).size());
    for (const char* ext : {".mpg", ".mpeg"})
        if (std::string f = files.find("videos/" + name + ext); !f.empty()) {
            out.playable = f;
            return out;
        }
    std::string src;
    for (const char* ext : {".mp4", ".webm", ".mov", ".avi", ".mkv"})
        if (src.empty()) src = files.find("videos/" + name + ext);
    if (src.empty()) {
        out.error = "video not found: " + nameIn;
        return out;
    }
    // Cache key: source path, size and time, so an edited video converts again.
    std::error_code ec;
    const auto size = fs::file_size(toPath(src), ec);
    // (libc++ counts file times in __int128: narrow it before formatting.)
    const long long time = (long long)fs::last_write_time(toPath(src), ec).time_since_epoch().count();
    const size_t key = std::hash<std::string>{}(src + "|" + std::to_string((unsigned long long)size) + "|" + std::to_string(time));
    const fs::path cacheDir = toPath(baseDir) / "cache" / "videos";
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
    const std::string dest = pathStr(cacheDir / (std::string(hex) + ".mpg"));
    {
        std::lock_guard<std::mutex> lk(g_jobsMtx);
        auto it = g_jobs.find(dest);
        if (it != g_jobs.end()) {
            if (it->second->running) { out.converting = true; return out; }
            if (!it->second->ok) { out.error = "could not convert " + nameIn + " with ffmpeg"; return out; }
            out.playable = dest;
            return out;
        }
    }
    if (fs::exists(toPath(dest), ec)) {
        out.playable = dest;
        return out;
    }
    const std::string ffmpeg = findFfmpeg(baseDir);
    if (ffmpeg.empty()) {
        out.error = "video " + nameIn + " needs ffmpeg to play (install it or put ffmpeg next to the game)";
        return out;
    }
    fs::create_directories(cacheDir, ec);
    auto job = std::make_shared<Job>();
    {
        std::lock_guard<std::mutex> lk(g_jobsMtx);
        g_jobs[dest] = job;
    }
    std::thread([ffmpeg, src, dest, job] {
        const std::string part = dest + ".part";
        const int rc = runProcess(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-i", src, "-vf",
                                           "scale='trunc(min(1280,iw)/2)*2':-2", "-r", "30", "-c:v", "mpeg1video", "-q:v",
                                           "4", "-bf", "0", "-c:a", "mp2", "-b:a", "192k", "-ar", "44100", "-ac", "2", "-f",
                                           "mpeg", part});
        std::error_code e2;
        bool ok = rc == 0 && fs::exists(toPath(part), e2) && fs::file_size(toPath(part), e2) > 0;
        if (ok) fs::rename(toPath(part), toPath(dest), e2);
        ok = ok && !e2;
        if (!ok) fs::remove(toPath(part), e2);
        std::lock_guard<std::mutex> lk(g_jobsMtx);
        job->ok = ok;
        job->running = false;
    }).detach();
    out.converting = true;
    return out;
}

}  // namespace gh
