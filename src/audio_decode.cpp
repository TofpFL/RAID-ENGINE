#include "audio_decode.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "audio.hpp"

// The decoders compiled into raylib (raudio.c): only their declarations here.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "dr_mp3.h"
#include "dr_wav.h"

namespace gh {

namespace {

constexpr uint64_t kRate = 44100;

// ------------------------------------------------------------- sources --
// A decoder of one file: interleaved 16-bit frames at the file's own rate.
// The files are opened here and handed to the decoders: inside raylib,
// fopen() on Android only looks in the APK, and the mods are outside it.

FILE* openRead(const std::string& path) { return std::fopen(path.c_str(), "rb"); }

bool seekFile(void* f, int offset, bool fromCurrent) {
    return std::fseek(static_cast<FILE*>(f), long(offset), fromCurrent ? SEEK_CUR : SEEK_SET) == 0;
}

size_t readFile(void* f, void* out, size_t n) { return std::fread(out, 1, n, static_cast<FILE*>(f)); }

struct Source {
    unsigned rate = 0, channels = 0;
    uint64_t frames = 0;
    virtual ~Source() = default;
    virtual size_t read(int16_t* out, size_t n) = 0;
    virtual bool seek(uint64_t frame) = 0;
};

struct OggSource final : Source {
    stb_vorbis* v = nullptr;
    ~OggSource() override {
        if (v) stb_vorbis_close(v);
    }
    bool open(const std::string& path) {
        FILE* f = openRead(path);
        if (!f) return false;
        int e = 0;
        v = stb_vorbis_open_file(f, 1, &e, nullptr);  // closes f when done (or on failure)
        if (!v) return false;
        const stb_vorbis_info info = stb_vorbis_get_info(v);
        rate = info.sample_rate;
        channels = unsigned(std::max(0, info.channels));
        frames = stb_vorbis_stream_length_in_samples(v);
        return rate > 0 && channels > 0 && frames > 0;
    }
    size_t read(int16_t* out, size_t n) override {
        size_t got = 0;
        while (got < n) {
            const size_t want = std::min<size_t>(n - got, size_t(1) << 20);
            const int k = stb_vorbis_get_samples_short_interleaved(v, int(channels), out + got * channels,
                                                                   int(want * channels));
            if (k <= 0) break;
            got += size_t(k);
        }
        return got;
    }
    bool seek(uint64_t f) override { return f <= 0xFFFFFFFFull && stb_vorbis_seek(v, unsigned(f)) != 0; }
};

struct Mp3Source final : Source {
    drmp3 m{};
    FILE* f = nullptr;
    bool ok = false;
    ~Mp3Source() override {
        if (ok) drmp3_uninit(&m);
        if (f) std::fclose(f);
    }
    static size_t onRead(void* u, void* out, size_t n) { return readFile(u, out, n); }
    static drmp3_bool32 onSeek(void* u, int offset, drmp3_seek_origin o) {
        return seekFile(u, offset, o == drmp3_seek_origin_current) ? 1 : 0;
    }
    bool open(const std::string& path) {
        f = openRead(path);
        if (!f || !drmp3_init(&m, onRead, onSeek, f, nullptr)) return false;
        ok = true;
        rate = m.sampleRate;
        channels = m.channels;
        frames = drmp3_get_pcm_frame_count(&m);  // walks the frame headers
        drmp3_seek_to_pcm_frame(&m, 0);
        return rate > 0 && channels > 0 && frames > 0;
    }
    size_t read(int16_t* out, size_t n) override { return size_t(drmp3_read_pcm_frames_s16(&m, n, out)); }
    bool seek(uint64_t f) override { return drmp3_seek_to_pcm_frame(&m, f) != 0; }
};

struct WavSource final : Source {
    drwav w{};
    FILE* f = nullptr;
    bool ok = false;
    ~WavSource() override {
        if (ok) drwav_uninit(&w);
        if (f) std::fclose(f);
    }
    static size_t onRead(void* u, void* out, size_t n) { return readFile(u, out, n); }
    static drwav_bool32 onSeek(void* u, int offset, drwav_seek_origin o) {
        return seekFile(u, offset, o == drwav_seek_origin_current) ? 1 : 0;
    }
    bool open(const std::string& path) {
        f = openRead(path);
        if (!f || !drwav_init(&w, onRead, onSeek, f, nullptr)) return false;
        ok = true;
        rate = w.sampleRate;
        channels = w.channels;
        frames = w.totalPCMFrameCount;
        return rate > 0 && channels > 0 && frames > 0;
    }
    size_t read(int16_t* out, size_t n) override { return size_t(drwav_read_pcm_frames_s16(&w, n, out)); }
    bool seek(uint64_t f) override { return drwav_seek_to_pcm_frame(&w, f) != 0; }
};

template <class T> std::unique_ptr<Source> tryOpen(const std::string& path) {
    auto s = std::make_unique<T>();
    if (!s->open(path)) return nullptr;
    return s;
}

bool endsWithNoCase(const std::string& s, const char* ext) {
    const size_t n = std::strlen(ext);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != ext[i]) return false;
    return true;
}

// By the file's first bytes (mods name files anything): Ogg Vorbis, WAV, MP3.
std::unique_ptr<Source> openSource(const std::string& path) {
    unsigned char h[12] = {};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    const size_t n = std::fread(h, 1, sizeof h, f);
    std::fclose(f);
    if (n < 4) return nullptr;
    if (!std::memcmp(h, "OggS", 4)) return tryOpen<OggSource>(path);  // Opus fails here: raylib cannot either
    if (!std::memcmp(h, "RIFF", 4) || !std::memcmp(h, "RIFX", 4) || !std::memcmp(h, "RF64", 4) || !std::memcmp(h, "riff", 4))
        return tryOpen<WavSource>(path);
    if (!std::memcmp(h, "ID3", 3) || (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) || endsWithNoCase(path, ".mp3"))
        return tryOpen<Mp3Source>(path);
    return nullptr;
}

// ----------------------------------------------------------- converter --
// Frames of a source at 44.1 kHz (linear interpolation, as the game's mixer
// does), mono kept mono and anything wider taken as its first two channels.

class Converter {
public:
    explicit Converter(std::unique_ptr<Source> s) : src_(std::move(s)) {
        inCh_ = src_->channels;
        outCh_ = inCh_ == 1 ? 1 : 2;
        total_ = src_->rate == kRate ? src_->frames : (src_->frames * kRate + src_->rate - 1) / src_->rate;
        buf_.resize(kChunk * inCh_);
    }
    int channels() const { return int(outCh_); }
    uint64_t total() const { return total_; }

    // Output frames [first, first + count): past the end of the file, silence.
    void decode(uint64_t first, size_t count, int16_t* dst) {
        if (src_->rate == kRate) direct(first, count, dst);
        else resample(first, count, dst);
    }

private:
    static constexpr size_t kChunk = 8192;

    void put(const int16_t* in, size_t frames, int16_t* out) const {
        if (inCh_ == outCh_) {
            std::memcpy(out, in, frames * outCh_ * sizeof(int16_t));
            return;
        }
        for (size_t f = 0; f < frames; ++f) {
            out[2 * f] = in[f * inCh_];
            out[2 * f + 1] = in[f * inCh_ + 1];
        }
    }

    void direct(uint64_t first, size_t count, int16_t* dst) {
        if (pos_ != first) {
            bufLen_ = 0;
            if (first >= src_->frames || !src_->seek(first)) {
                std::memset(dst, 0, count * outCh_ * sizeof(int16_t));
                pos_ = ~0ull;
                return;
            }
            pos_ = first;
        }
        while (count > 0) {
            const size_t n = std::min(count, kChunk);
            const size_t got = src_->read(buf_.data(), n);
            put(buf_.data(), got, dst);
            dst += got * outCh_;
            count -= got;
            pos_ += got;
            if (got < n) {  // the file ends early
                std::memset(dst, 0, count * outCh_ * sizeof(int16_t));
                pos_ = ~0ull;
                return;
            }
        }
    }

    // Makes input frames i and i + 1 (when the file has it) readable in buf_:
    // reading on continues the file (keeping frame i), anything else seeks.
    bool load(uint64_t i) {
        if (i >= src_->frames) return false;
        const uint64_t need = std::min(i + 2, src_->frames), end = bufStart_ + bufLen_;
        if (i >= bufStart_ && need <= end) return true;
        if (pos_ == end && i >= bufStart_ && i <= end && end - i < kChunk) {
            const size_t keep = size_t(end - i);
            std::memmove(buf_.data(), buf_.data() + (i - bufStart_) * inCh_, keep * inCh_ * sizeof(int16_t));
            bufStart_ = i;
            bufLen_ = keep;
        } else if (pos_ == end && i > end && i - end < kChunk / 2) {
            bufStart_ = end;  // a few frames ahead: read through them
            bufLen_ = 0;
        } else {
            if (!src_->seek(i)) return false;
            pos_ = bufStart_ = i;
            bufLen_ = 0;
        }
        const size_t got = src_->read(buf_.data() + bufLen_ * inCh_, kChunk - bufLen_);
        pos_ += got;
        bufLen_ += got;
        return i >= bufStart_ && i < bufStart_ + bufLen_;
    }

    void resample(uint64_t first, size_t count, int16_t* dst) {
        const uint64_t rate = src_->rate;
        for (size_t k = 0; k < count; ++k) {
            const uint64_t num = (first + k) * rate;
            const uint64_t i = num / kRate;
            const int64_t fr = int64_t(num % kRate);
            if (!load(i)) {
                std::memset(dst + k * outCh_, 0, (count - k) * outCh_ * sizeof(int16_t));
                return;
            }
            const int16_t* a = buf_.data() + (i - bufStart_) * inCh_;
            const int16_t* b = i + 1 < bufStart_ + bufLen_ ? a + inCh_ : a;
            for (unsigned c = 0; c < outCh_; ++c)
                dst[k * outCh_ + c] = int16_t(a[c] + ((int64_t(b[c]) - a[c]) * fr) / int64_t(kRate));
        }
    }

    std::unique_ptr<Source> src_;
    unsigned inCh_ = 2, outCh_ = 2;
    uint64_t total_ = 0;
    std::vector<int16_t> buf_;
    uint64_t bufStart_ = 0, pos_ = 0;  // buf_ holds input frames [bufStart_, bufStart_ + bufLen_); pos_: the source's next
    size_t bufLen_ = 0;
};

// -------------------------------------------------------------- threads --

std::atomic<int> g_live{0};
std::atomic<bool> g_stop{false};
std::atomic<unsigned> g_readers{0};  // odd while an audio callback reads blocks

// Waits for the audio callback running now (if any) to end.
void waitForReaders() {
    const unsigned s = g_readers.load(std::memory_order_seq_cst);
    if (!(s & 1u)) return;
    for (int i = 0; i < 5000 && g_readers.load(std::memory_order_seq_cst) == s; ++i)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
}

// The blocks kept at holdPos ([first, last) or an empty range).
void holdRange(const PcmStream& s, size_t& first, size_t& last) {
    const size_t h = s.holdPos.load(std::memory_order_relaxed);
    first = last = 0;
    if (h == ~size_t(0) || h >= s.total) return;
    first = h / PcmStream::kBlockFrames;
    last = std::min(s.blocks, first + PcmStream::kHoldBlocks);
}

// A long track keeps the blocks around the play position (and at holdPos):
// the others go.
void evictFar(PcmStream& s, size_t pb, size_t end, std::vector<int16_t*>& doomed) {
    if (s.inMemory.load(std::memory_order_relaxed) <
        PcmStream::kBehindBlocks + PcmStream::kAheadBlocks + PcmStream::kHoldBlocks + 16)
        return;
    const size_t lo = pb > PcmStream::kBehindBlocks ? pb - PcmStream::kBehindBlocks : 0;
    size_t hFirst, hLast;
    holdRange(s, hFirst, hLast);
    for (size_t b = 0; b < s.blocks; ++b) {
        if (b >= lo && b < end) {
            b = end - 1;
            continue;
        }
        if (b >= hFirst && b < hLast) continue;
        if (int16_t* p = s.block[b].exchange(nullptr, std::memory_order_seq_cst)) doomed.push_back(p);
    }
    if (doomed.empty()) return;
    waitForReaders();
    for (int16_t* p : doomed) delete[] p;
    s.inMemory.fetch_sub(doomed.size(), std::memory_order_relaxed);
    doomed.clear();
}

void decodeLoop(std::weak_ptr<PcmStream> weak, std::unique_ptr<Converter> conv) {
    size_t next = ~size_t(0);
    std::vector<int16_t*> doomed;
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::shared_ptr<PcmStream> s = weak.lock();
        if (!s) break;  // nobody plays this track any more
        constexpr size_t B = PcmStream::kBlockFrames;
        const size_t pb = std::min(s->blocks - 1, s->playPos.load(std::memory_order_relaxed) / B);
        const size_t end = s->windowed ? std::min(s->blocks, pb + PcmStream::kAheadBlocks) : s->blocks;
        // What the mixer needs first: the block being played, then on from there.
        size_t target = ~size_t(0);
        if (!s->get(pb)) target = pb;
        else if (next > pb && next < end && !s->get(next)) target = next;
        else {
            for (size_t b = pb + 1; b < end; ++b)
                if (!s->get(b)) { target = b; break; }
            if (target == ~size_t(0) && !s->windowed)
                for (size_t b = 0; b < pb; ++b)
                    if (!s->get(b)) { target = b; break; }
            if (target == ~size_t(0) && s->windowed) {  // where a loop jumps back to
                size_t hFirst, hLast;
                holdRange(*s, hFirst, hLast);
                for (size_t b = hFirst; b < hLast; ++b)
                    if (!s->get(b)) { target = b; break; }
            }
        }
        if (target == ~size_t(0)) {
            if (!s->windowed) {
                s->finished.store(true);
                break;  // all of it in memory
            }
            s.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));  // window full: wait for the song to move on
            continue;
        }
        if (s->windowed) evictFar(*s, pb, end, doomed);
        const size_t count = s->framesIn(target);
        int16_t* buf = new int16_t[count * size_t(s->channels)];
        conv->decode(uint64_t(target) * B, count, buf);
        s->block[target].store(buf, std::memory_order_release);
        s->inMemory.fetch_add(1, std::memory_order_relaxed);
        next = target + 1;
    }
    if (auto s = weak.lock()) s->finished.store(true);
    g_live.fetch_sub(1);
}

}  // namespace

// ------------------------------------------------------------------ API --

PcmStream::~PcmStream() {
    if (!block) return;
    for (size_t b = 0; b < blocks; ++b) delete[] block[b].load();
}

bool PcmStream::ready(size_t from, size_t to) const {
    to = std::min(to, total);
    if (from >= to) return true;
    for (size_t b = from / kBlockFrames; b <= (to - 1) / kBlockFrames; ++b)
        if (!get(b)) return false;
    return true;
}

std::shared_ptr<Pcm> openAudioStream(const std::string& path, double startFraction, size_t wholeBytes) {
    auto src = openSource(path);
    if (!src) return nullptr;
    auto conv = std::make_unique<Converter>(std::move(src));
    const uint64_t total = conv->total();
    if (total == 0 || total > (uint64_t(1) << 40)) return nullptr;
    auto s = std::make_shared<PcmStream>();
    s->path = path;
    s->channels = conv->channels();
    s->total = size_t(total);
    s->blocks = (s->total + PcmStream::kBlockFrames - 1) / PcmStream::kBlockFrames;
    s->block.reset(new std::atomic<int16_t*>[s->blocks]);
    for (size_t b = 0; b < s->blocks; ++b) s->block[b].store(nullptr);
    s->windowed = uint64_t(total) * uint64_t(s->channels) * 2 > wholeBytes;
    s->playPos.store(size_t(std::clamp(startFraction, 0.0, 1.0) * double(s->total)));
    auto pcm = std::make_shared<Pcm>();
    pcm->channels = s->channels;
    pcm->stream = s;
    pcm->streamFrames = s->total;
    g_live.fetch_add(1);
    std::thread(decodeLoop, std::weak_ptr<PcmStream>(s), std::move(conv)).detach();
    return pcm;
}

bool decodeAudioFile(const std::string& path, Pcm& out, std::string& err, size_t maxBytes) {
    err.clear();
    auto src = openSource(path);
    if (!src) return false;
    Converter conv(std::move(src));
    const uint64_t bytes = conv.total() * uint64_t(conv.channels()) * 2;
    if (bytes > maxBytes) {
        err = "audio too long to load whole (" + std::to_string(bytes >> 20) + " MB): " + path;
        return false;
    }
    out.stream.reset();
    out.streamFrames = 0;
    out.channels = conv.channels();
    out.data.resize(size_t(conv.total()) * size_t(out.channels));
    conv.decode(0, size_t(conv.total()), out.data.data());
    return true;
}

bool writeWavFile(const std::string& path, const Pcm& p) {
    const uint64_t frames = p.frames(), ch = uint64_t(std::max(1, p.channels));
    const uint64_t bytes = frames * ch * 2;
    if (bytes > 0xFFFFFF00ull) return false;  // WAV sizes are 32-bit
    std::unique_ptr<Converter> conv;
    if (p.stream) {
        auto src = openSource(p.stream->path);
        if (!src) return false;
        conv = std::make_unique<Converter>(std::move(src));
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(uint32_t(36 + bytes));
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(uint16_t(ch));
    w32(uint32_t(kRate));
    w32(uint32_t(kRate * ch * 2));
    w16(uint16_t(ch * 2));
    w16(16);
    std::fwrite("data", 1, 4, f);
    w32(uint32_t(bytes));
    bool ok = true;
    if (conv) {
        std::vector<int16_t> buf(size_t(PcmStream::kBlockFrames * ch));
        for (uint64_t at = 0; at < frames && ok; at += PcmStream::kBlockFrames) {
            const size_t n = size_t(std::min<uint64_t>(PcmStream::kBlockFrames, frames - at));
            conv->decode(at, n, buf.data());
            ok = std::fwrite(buf.data(), 2, n * ch, f) == n * ch;
        }
    } else {
        ok = std::fwrite(p.data.data(), 2, p.data.size(), f) == p.data.size();
    }
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

void audioReadBegin() { g_readers.fetch_add(1, std::memory_order_seq_cst); }
void audioReadEnd() { g_readers.fetch_add(1, std::memory_order_seq_cst); }

void stopAudioDecoders() {
    g_stop.store(true);
    for (int i = 0; i < 400 && g_live.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void startAudioDecoders() { g_stop.store(false); }

int liveAudioDecoders() { return g_live.load(); }

}  // namespace gh
