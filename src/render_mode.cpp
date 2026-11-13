#include "render_mode.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

#include "rlgl.h"

namespace gh {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::atomic<int> g_busy{0};
std::mutex g_msgMtx;
std::string g_message;

void setMessage(const std::string& m) {
    std::lock_guard<std::mutex> lk(g_msgMtx);
    g_message = m;
}

void removeFile(const std::string& p) {
    std::error_code ec;
    fs::remove(u8path(p), ec);
}

bool moveFile(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(u8path(from), u8path(to), ec);
    if (!ec) return true;
    fs::copy_file(u8path(from), u8path(to), fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    removeFile(from);
    return true;
}

void put32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

}  // namespace

const std::vector<RenderEncoder>& renderEncoders() {
    static const std::vector<RenderEncoder> list = {
        {"H.264 (x264)", "mp4"}, {"H.265 (x265)", "mp4"}, {"VP9 (WebM)", "webm"}, {"H.264 NVIDIA", "mp4"},
        {"H.265 NVIDIA", "mp4"}, {"H.264 AMD", "mp4"},    {"H.264 Intel", "mp4"},
    };
    return list;
}

std::vector<std::string> renderVideoArgs(const RenderOptions& o, const std::string& out) {
    const std::string br = std::to_string(std::max(1, o.bitrateMbps)) + "M";
    std::vector<std::string> a = {"-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s",
                                  std::to_string(o.width) + "x" + std::to_string(o.height), "-r", std::to_string(o.fps), "-i", "-",
                                  "-vf", "vflip"};
    switch (o.encoder) {
        case 1: a.insert(a.end(), {"-c:v", "libx265", "-preset", "fast", "-b:v", br, "-pix_fmt", "yuv420p", "-tag:v", "hvc1"}); break;
        case 2:
            a.insert(a.end(), {"-c:v", "libvpx-vp9", "-b:v", br, "-deadline", "realtime", "-cpu-used", "8", "-row-mt", "1",
                               "-pix_fmt", "yuv420p"});
            break;
        case 3: a.insert(a.end(), {"-c:v", "h264_nvenc", "-preset", "p4", "-b:v", br, "-pix_fmt", "yuv420p"}); break;
        case 4: a.insert(a.end(), {"-c:v", "hevc_nvenc", "-preset", "p4", "-b:v", br, "-pix_fmt", "yuv420p", "-tag:v", "hvc1"}); break;
        case 5: a.insert(a.end(), {"-c:v", "h264_amf", "-b:v", br, "-pix_fmt", "yuv420p"}); break;
        case 6: a.insert(a.end(), {"-c:v", "h264_qsv", "-b:v", br, "-pix_fmt", "nv12"}); break;
        default:
            a.insert(a.end(), {"-c:v", "libx264", "-preset", "veryfast", "-b:v", br, "-maxrate", br, "-bufsize",
                               std::to_string(std::max(1, o.bitrateMbps) * 2) + "M", "-pix_fmt", "yuv420p"});
            break;
    }
    a.push_back(out);
    return a;
}

std::vector<std::string> renderMuxArgs(const RenderOptions& o, const std::string& video, const std::string& wav,
                                       const std::string& out) {
    const bool webm = o.encoder == 2;
    std::vector<std::string> a = {"-hide_banner", "-loglevel", "error", "-y", "-i", video, "-i", wav, "-map", "0:v", "-map", "1:a",
                                  "-c:v", "copy"};
    if (webm) a.insert(a.end(), {"-c:a", "libopus", "-b:a", "160k"});
    else a.insert(a.end(), {"-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart"});
    a.insert(a.end(), {"-shortest", out});
    return a;
}

VideoRenderer::~VideoRenderer() {
    std::string err;
    if (active_) finish(err);
}

bool VideoRenderer::start(const std::string& ffmpeg, const std::string& outBase, const RenderOptions& o, std::string& err) {
    if (active_) finish(err);
    err.clear();
    opt_ = o;
    opt_.width = std::max(16, o.width & ~1);  // the encoders want even sizes
    opt_.height = std::max(16, o.height & ~1);
    opt_.fps = std::clamp(o.fps, 1, 1000);
    if (opt_.encoder < 0 || opt_.encoder >= int(renderEncoders().size())) opt_.encoder = 0;
    const std::string ext = renderEncoders()[size_t(opt_.encoder)].ext;
    ffmpeg_ = ffmpeg;
    outBase_ = outBase;
    final_ = outBase + "." + ext;
    videoTmp_ = outBase + ".tmp." + ext;
    wavTmp_ = outBase + ".tmp.wav";
    log_ = outBase + ".log";
    std::error_code ec;
    fs::create_directories(u8path(outBase).parent_path(), ec);
    rt_ = LoadRenderTexture(opt_.width, opt_.height);
    if (!rt_.id) {
        err = "could not make a " + std::to_string(opt_.width) + "x" + std::to_string(opt_.height) + " picture to record";
        return false;
    }
    SetTextureFilter(rt_.texture, TEXTURE_FILTER_BILINEAR);
    if (!proc_.start(ffmpeg, renderVideoArgs(opt_, videoTmp_), log_)) {
        UnloadRenderTexture(rt_);
        rt_ = {};
        err = "could not start ffmpeg";
        return false;
    }
    if (opt_.audio) {
        wav_ = std::fopen(wavTmp_.c_str(), "wb");
        if (wav_) {
            std::fwrite("RIFF", 1, 4, wav_);
            put32(wav_, 36);  // sizes written at the end
            std::fwrite("WAVEfmt ", 1, 8, wav_);
            put32(wav_, 16);
            put16(wav_, 1);
            put16(wav_, 2);
            put32(wav_, 44100);
            put32(wav_, 44100 * 4);
            put16(wav_, 4);
            put16(wav_, 16);
            std::fwrite("data", 1, 4, wav_);
            put32(wav_, 0);
        }
    }
    frames_ = 0;
    audioFrames_ = 0;
    active_ = true;
    return true;
}

unsigned renderAudioFrames(long long frame, int fps) {
    const uint64_t f = uint64_t(std::max(1, fps));
    const uint64_t a = uint64_t(frame) * 44100 / f, b = uint64_t(frame + 1) * 44100 / f;
    return unsigned(b - a);
}

unsigned VideoRenderer::audioFramesFor(long long frame) const { return renderAudioFrames(frame, opt_.fps); }

bool VideoRenderer::captureFrame() {
    if (!active_ || !rt_.id) return false;
    void* px = rlReadTexturePixels(rt_.texture.id, rt_.texture.width, rt_.texture.height, rt_.texture.format);
    if (!px) return false;
    const bool ok = proc_.write(px, size_t(rt_.texture.width) * size_t(rt_.texture.height) * 4);
    MemFree(px);
    if (ok) ++frames_;
    return ok;
}

void VideoRenderer::addAudio(const float* stereo, unsigned frames) {
    if (!active_ || !wav_ || frames == 0) return;
    pcm_.resize(size_t(frames) * 2);
    for (size_t i = 0; i < pcm_.size(); ++i) pcm_[i] = int16_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
    std::fwrite(pcm_.data(), 2, pcm_.size(), wav_);
    audioFrames_ += frames;
}

void VideoRenderer::closeWav() {
    if (!wav_) return;
    const uint64_t bytes = std::min<uint64_t>(audioFrames_ * 4, 0xFFFFFF00ull);
    std::fseek(wav_, 4, SEEK_SET);
    put32(wav_, uint32_t(36 + bytes));
    std::fseek(wav_, 40, SEEK_SET);
    put32(wav_, uint32_t(bytes));
    std::fclose(wav_);
    wav_ = nullptr;
}

std::string VideoRenderer::finish(std::string& err) {
    err.clear();
    if (!active_) return "";
    active_ = false;
    const int rc = proc_.finish();  // ffmpeg encodes what is left and ends
    if (rt_.id) {
        UnloadRenderTexture(rt_);
        rt_ = {};
    }
    closeWav();
    if (rc != 0 || frames_ == 0) {
        err = frames_ == 0 ? "nothing was recorded" : "ffmpeg could not make the video (its messages: " + log_ + ")";
        removeFile(videoTmp_);
        removeFile(wavTmp_);
        return "";
    }
    removeFile(log_);
    if (!opt_.audio || audioFrames_ == 0) {
        removeFile(wavTmp_);
        if (!moveFile(videoTmp_, final_)) {
            err = "could not write " + final_;
            return "";
        }
        setMessage("Video saved: " + final_);
        return final_;
    }
    // The audio goes in by copying the video as it is: a few seconds, in the background.
    ++g_busy;
    std::thread([ffmpeg = ffmpeg_, opt = opt_, video = videoTmp_, wav = wavTmp_, out = final_] {
        if (runProcess(ffmpeg, renderMuxArgs(opt, video, wav, out)) == 0) {
            removeFile(video);
            removeFile(wav);
            setMessage("Video saved: " + out);
        } else {
            removeFile(out);
            moveFile(video, out);
            setMessage("Video saved without its audio (ffmpeg could not add it; the audio is in " + wav + "): " + out);
        }
        --g_busy;
    }).detach();
    setMessage("Adding the audio to " + final_ + "...");
    return final_;
}

bool VideoRenderer::busy() { return g_busy.load() > 0; }

void VideoRenderer::waitIdle(double maxSeconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(maxSeconds);
    while (busy() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

std::string VideoRenderer::takeMessage() {
    std::lock_guard<std::mutex> lk(g_msgMtx);
    std::string m;
    m.swap(g_message);
    return m;
}

}  // namespace gh
