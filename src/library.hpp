// Song library: the built-in songs plus any FNF-style songs found in the
// `songs/` and `mods/` folders next to the executable.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio.hpp"
#include "chart.hpp"
#include "composer.hpp"
#include "song_scan.hpp"

namespace gh {

struct Prepared {
    bool ok = false;
    std::string error;
    Chart chart;
    PcmPtr inst, voiceP, voiceO;
};

class Library {
public:
    // progress: shown while .zip mods unpack (they can take a while).
    void scan(const std::string& baseDir, const std::function<void(const std::string&, double)>& progress = {});
    const std::vector<SongEntry>& songs() const { return songs_; }
    const SongEntry* find(const std::string& id) const;
    int customCount() const { return customCount_; }
    const std::string& report() const { return report_; }
    const std::string& installReport() const { return installReport_; }
    // Every folder of the enabled mods that holds assets (images/, characters/,
    // stages/, sounds/, music/, videos/, shaders/, fonts/), its own mod's
    // first: a song looks there for what its own folders do not have, since
    // mods reuse each other's assets.
    std::vector<std::string> sharedAssetRootsFor(const SongEntry& e) const { return sharedRootsFor(assets_, e); }
    // The editors: every mod's asset folders by mod (its folder in mods/), and all of them.
    const std::map<std::string, std::vector<std::string>>& packRoots() const { return assets_.packRoots; }
    const std::vector<std::string>& allAssetRoots() const { return assets_.sharedRoots; }

    // Heavy work (decoding, composing, parsing). Thread-safe.
    // extraKeys = false: every chart gets 4 keys (Options, Extra keys).
    Prepared prepare(const SongEntry& e, const std::string& diff, bool mirror, bool extraKeys = true);
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

    std::vector<SongEntry> songs_;
    SongScan assets_;  // the asset folders of the scan (its songs moved to songs_)
    int customCount_ = 0;
    std::string report_, installReport_;
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
