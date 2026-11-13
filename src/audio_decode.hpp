// Song audio decoded while it plays.
//
// The S2X engine reads a chart while the song plays instead of all of it
// first; this does the same for audio, the slow part here (the charts load
// at hundreds of MB per second). A file is opened, its length read from the
// header and the song starts at once: a background thread decodes it in
// blocks, first where the song plays, then the rest. Tracks too long to keep
// whole (S2X's discarded chunks) keep only the minutes around the play
// position, so an 8-hour song needs a few MB.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace gh {

struct Pcm;

struct PcmStream {
    static constexpr size_t kBlockFrames = 16384;            // ~0.37 s
    static constexpr size_t kWholeBytes = size_t(192) << 20;  // longer tracks keep a window
    static constexpr size_t kBehindBlocks = 81;               // window: ~30 s behind the play position
    static constexpr size_t kAheadBlocks = 162;               // and ~60 s ahead
    static constexpr size_t kHoldBlocks = 16;                 // ~6 s kept at holdPos

    std::string path;
    int channels = 2;     // 1 or 2
    size_t total = 0;     // frames at 44.1 kHz
    size_t blocks = 0;
    bool windowed = false;
    std::unique_ptr<std::atomic<int16_t*>[]> block;  // null: not decoded (yet, or any more)
    mutable std::atomic<size_t> playPos{0};          // where the mixer plays (frames)
    // A second place the mixer will jump to (the start of a loop, as in the
    // Freeplay preview): its first blocks stay decoded and in memory too.
    mutable std::atomic<size_t> holdPos{~size_t(0)};
    std::atomic<size_t> inMemory{0};                 // blocks held
    std::atomic<bool> finished{false};               // whole track decoded, or the decoder stopped

    PcmStream() = default;
    PcmStream(const PcmStream&) = delete;
    PcmStream& operator=(const PcmStream&) = delete;
    ~PcmStream();
    // seq_cst: pairs with the eviction's exchange and the readers' counter.
    const int16_t* get(size_t b) const { return block[b].load(std::memory_order_seq_cst); }
    size_t framesIn(size_t b) const { return b + 1 < blocks ? kBlockFrames : total - b * kBlockFrames; }
    // Every frame in [from, to) is in memory now.
    bool ready(size_t from, size_t to) const;
};

// Opens an Ogg Vorbis, MP3 or WAV file and returns at once: the length is
// known, the samples arrive in the background, starting at `startFraction`
// of the track (where playing starts). Null when the file is not one of
// those formats (use loadAudioFile then). Tracks over `wholeBytes` keep a
// window around the play position.
std::shared_ptr<Pcm> openAudioStream(const std::string& path, double startFraction = 0.0,
                                     size_t wholeBytes = PcmStream::kWholeBytes);

// The whole file decoded by the same decoders (16-bit, 44.1 kHz). False with
// an empty `err` when the format is not Ogg Vorbis, MP3 or WAV; false with
// `err` set when decoding fails or the track is over `maxBytes`.
bool decodeAudioFile(const std::string& path, Pcm& out, std::string& err, size_t maxBytes);

// A 16-bit WAV file of the track (a streamed track is decoded again from its file).
bool writeWavFile(const std::string& path, const Pcm& p);

// The mixer brackets each audio callback with these: a block left behind by
// the play position is freed only once no callback can be reading it.
void audioReadBegin();
void audioReadEnd();
// Decoder threads end (game exit); allowed again by startAudioDecoders().
void stopAudioDecoders();
void startAudioDecoders();
int liveAudioDecoders();

}  // namespace gh
