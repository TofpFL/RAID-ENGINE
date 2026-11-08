// Song library: the built-in songs plus any FNF-style songs found in the
// `songs/` and `mods/` folders next to the executable.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio.hpp"
#include "chart.hpp"
#include "composer.hpp"

namespace gh {

struct SongEntry {
    std::string id;        // stable key for scores
    std::string title;
    std::string subtitle;  // blurb or source pack
    bool builtin = false;
    const SongDef* def = nullptr;
    std::vector<std::string> diffs;
    std::vector<std::string> chartPaths;  // parallel to diffs (custom songs)
    std::string metaPath;                 // V-Slice metadata
    std::string inst, voices, voicesP, voicesO;
    uint32_t color = 0xB48CDC;
    std::string opponent;  // built-in character id, "" for custom
};

struct Prepared {
    bool ok = false;
    std::string error;
    Chart chart;
    PcmPtr inst, voiceP, voiceO;
};

class Library {
public:
    void scan(const std::string& baseDir);
    const std::vector<SongEntry>& songs() const { return songs_; }
    const SongEntry* find(const std::string& id) const;
    int customCount() const { return customCount_; }
    const std::string& report() const { return report_; }

    // Heavy work (decoding, composing, parsing). Thread-safe.
    Prepared prepare(const SongEntry& e, const std::string& diff, bool mirror);
    PcmPtr preview(const SongEntry& e);  // instrumental only, for menus

    // Menu music (composed once).
    PcmPtr menuMusic(double* loopSec);

private:
    struct BuiltinAudio {
        std::string id;
        std::shared_ptr<Composition> comp;
        PcmPtr inst, vp, vo;
    };
    BuiltinAudio builtinAudio(const SongDef& def);
    void scanSongFolder(const std::string& dir, const std::string& audioDir, const std::string& source);

    std::vector<SongEntry> songs_;
    int customCount_ = 0;
    std::string report_;
    std::mutex mtx_;
    BuiltinAudio cache_;
    std::mutex customMtx_;
    std::string customCacheKey_, customInstKey_;
    PcmPtr customInst_, customVoices_, customVp_, customVo_;
    std::mutex menuMtx_;
    PcmPtr menu_;
    double menuLoop_ = 0;
};

}  // namespace gh
