#include "library.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "common.hpp"
#include "json.hpp"

namespace gh {

namespace fs = std::filesystem;

static fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

static std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static std::vector<fs::path> listDir(const fs::path& dir, bool wantDirs) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool isDir = it->is_directory(e2);
        if (isDir == wantDirs) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool isAudioExt(const std::string& lowerName) {
    return endsWithI(lowerName, ".ogg") || endsWithI(lowerName, ".mp3") || endsWithI(lowerName, ".wav") ||
           endsWithI(lowerName, ".qoa");
}

static uint32_t colorFor(const std::string& s) {
    static const uint32_t palette[] = {0xB48CDC, 0x8CC8DC, 0xDCA08C, 0x8CDCA0, 0xDC8CB4, 0xC8DC8C, 0x8CA0DC, 0xDCC88C};
    uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
    return palette[h % 8];
}

static int diffOrder(const std::string& d) {
    const std::string l = toLower(d);
    if (l == "easy") return 0;
    if (l == "normal") return 1;
    if (l == "hard") return 2;
    return 3;
}

void Library::scan(const std::string& baseDir) {
    songs_.clear();
    for (const SongDef& def : builtinSongs()) {
        SongEntry e;
        e.id = "builtin:" + def.id;
        e.title = def.title;
        e.subtitle = def.blurb;
        e.builtin = true;
        e.def = &def;
        e.diffs = def.diffs;
        e.color = def.color;
        e.opponent = def.opponent;
        songs_.push_back(e);
    }
    const size_t firstCustom = songs_.size();
    const fs::path base = toPath(baseDir);

    for (const fs::path& d : listDir(base / "songs", true)) scanSongFolder(pathStr(d), pathStr(d), "songs", pathStr(d) + "/");

    for (const fs::path& mod : listDir(base / "mods", true)) {
        const std::string modName = pathStr(mod.filename());
        const fs::path songsRoot = mod / "songs";
        auto audioFor = [&](const std::string& name) {
            for (const fs::path& s : listDir(songsRoot, true)) {
                const std::string n = pathStr(s.filename());
                if (equalsI(n, name) || songPathName(n) == songPathName(name)) return pathStr(s);
            }
            return std::string();
        };
        for (const fs::path& s : listDir(mod / "data", true)) {
            const std::string name = pathStr(s.filename());
            if (equalsI(name, "songs")) {  // V-Slice layout: data/songs/<song>/
                for (const fs::path& s2 : listDir(s, true)) {
                    const std::string n2 = pathStr(s2.filename());
                    scanSongFolder(pathStr(s2), audioFor(n2), modName, pathStr(mod) + "/");
                }
            } else {
                scanSongFolder(pathStr(s), audioFor(name), modName, pathStr(mod) + "/");
            }
        }
    }
    std::sort(songs_.begin() + std::ptrdiff_t(firstCustom), songs_.end(),
              [](const SongEntry& a, const SongEntry& b) { return toLower(a.title) < toLower(b.title); });
    customCount_ = int(songs_.size() - firstCustom);
    report_ = std::to_string(customCount_) + " custom song" + (customCount_ == 1 ? "" : "s") + " found";
}

void Library::scanSongFolder(const std::string& dirS, const std::string& audioDirS, const std::string& source,
                             const std::string& modRoot) {
    const fs::path dir = toPath(dirS);
    const std::string folder = pathStr(dir.filename());
    const std::string key = songPathName(folder);
    std::vector<fs::path> charts;
    fs::path meta, vsliceChart;
    for (const fs::path& f : listDir(dir, false)) {
        const std::string n = toLower(pathStr(f.filename()));
        if (!endsWithI(n, ".json")) continue;
        if (n.find("-metadata") != std::string::npos) { meta = f; continue; }
        if (n.find("events") != std::string::npos) continue;
        if (endsWithI(n, "-chart.json")) vsliceChart = f;
        charts.push_back(f);
    }
    if (charts.empty()) return;

    SongEntry e;
    e.title = prettyName(folder);
    e.subtitle = source;
    e.id = "custom:" + source + "/" + key;
    e.color = colorFor(e.id);
    e.modRoot = modRoot;
    e.dataDir = dirS + "/";
    e.songKey = folder;

    if (!vsliceChart.empty() && !meta.empty()) {
        std::string metaText;
        if (readTextFile(pathStr(meta), metaText)) {
            json::Document d;
            if (d.parse(metaText)) {
                const json::Value& m = d.root();
                std::string t(d.strOr(d.get(m, "songName"), ""));
                if (!t.empty()) e.title = t;
                if (const json::Value* pd = d.get(m, "playData"))
                    if (const json::Value* df = d.get(*pd, "difficulties"); df && df->type == json::Type::Array)
                        for (size_t i = 0; i < d.size(*df); ++i) {
                            e.diffs.push_back(prettyName(d.str(d.at(*df, i))));
                            e.chartPaths.push_back(pathStr(vsliceChart));
                        }
            }
        }
        if (e.diffs.empty()) { e.diffs = {"Normal"}; e.chartPaths = {pathStr(vsliceChart)}; }
        e.metaPath = pathStr(meta);
    } else {
        std::vector<std::pair<std::string, std::string>> found;
        for (const fs::path& c : charts) {
            std::string stem = pathStr(c.stem());
            const std::string ls = toLower(stem);
            std::string diff;
            if (ls == key || songPathName(stem) == key) diff = "Normal";
            else if (ls.rfind(key + "-", 0) == 0) diff = prettyName(stem.substr(key.size() + 1));
            else diff = prettyName(stem);
            for (auto& f : found)
                if (equalsI(f.first, diff)) diff += " (2)";
            found.push_back({diff, pathStr(c)});
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
            const int oa = diffOrder(a.first), ob = diffOrder(b.first);
            return oa != ob ? oa < ob : a.first < b.first;
        });
        for (auto& f : found) { e.diffs.push_back(f.first); e.chartPaths.push_back(f.second); }
    }

    if (!audioDirS.empty()) {
        for (const fs::path& f : listDir(toPath(audioDirS), false)) {
            const std::string n = toLower(pathStr(f.filename()));
            if (!isAudioExt(n)) continue;
            const std::string stem = toLower(pathStr(f.stem()));
            const std::string p = pathStr(f);
            if (stem == "inst") e.inst = p;
            else if (stem == "voices") e.voices = p;
            else if (stem.rfind("voices-", 0) == 0 || stem.rfind("voices_", 0) == 0) {
                const std::string suf = stem.substr(7);
                if (suf == "player" || suf == "bf" || suf == "boyfriend" || suf == "playable") e.voicesP = p;
                else if (suf == "opponent" || suf == "dad" || suf == "opp" || suf == "enemy") e.voicesO = p;
                else if (e.voicesO.empty()) e.voicesO = p;
                else if (e.voicesP.empty()) e.voicesP = p;
            } else if (e.inst.empty() && stem.rfind("inst", 0) == 0) {
                e.inst = p;  // e.g. Inst-erect.ogg when no plain Inst exists
            }
        }
    }
    songs_.push_back(e);
}

const SongEntry* Library::find(const std::string& id) const {
    for (const SongEntry& e : songs_)
        if (e.id == id) return &e;
    return nullptr;
}

Library::BuiltinAudio Library::builtinAudio(const SongDef& def) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (cache_.id == def.id && cache_.comp) return cache_;
    BuiltinAudio a;
    a.id = def.id;
    a.comp = compose(def, true);
    auto inst = std::make_shared<Pcm>();
    inst->channels = 2;
    inst->data = std::move(a.comp->inst);
    auto vp = std::make_shared<Pcm>();
    vp->channels = 1;
    vp->data = std::move(a.comp->voiceP);
    auto vo = std::make_shared<Pcm>();
    vo->channels = 1;
    vo->data = std::move(a.comp->voiceO);
    a.inst = inst;
    a.vp = vp;
    a.vo = vo;
    cache_ = a;
    return a;
}

static PcmPtr loadOrNull(const std::string& path) {
    if (path.empty()) return nullptr;
    auto p = std::make_shared<Pcm>();
    std::string err;
    if (!loadAudioFile(path, *p, err)) return nullptr;
    return p;
}

Prepared Library::prepare(const SongEntry& e, const std::string& diff, bool mirror) {
    Prepared p;
    if (e.builtin && e.def) {
        BuiltinAudio a = builtinAudio(*e.def);
        buildChart(*e.def, *a.comp, diff, p.chart);
        p.inst = a.inst;
        p.voiceP = a.vp;
        p.voiceO = a.vo;
    } else {
        size_t idx = 0;
        for (size_t i = 0; i < e.diffs.size(); ++i)
            if (e.diffs[i] == diff) idx = i;
        if (idx >= e.chartPaths.size()) { p.error = "no chart for difficulty " + diff; return p; }
        std::string text, meta;
        if (!readTextFile(e.chartPaths[idx], text)) { p.error = "cannot read " + e.chartPaths[idx]; return p; }
        if (!e.metaPath.empty()) readTextFile(e.metaPath, meta);
        std::string err;
        if (!parseChart(text, diff, p.chart, err, meta)) { p.error = err; return p; }
        if (p.chart.title.empty()) p.chart.title = e.title;
        p.chart.difficulty = diff;
        // Psych keeps events in a separate events.json next to the charts.
        for (const fs::path& f : listDir(toPath(e.dataDir), false)) {
            if (!equalsI(pathStr(f.filename()), "events.json")) continue;
            std::string evText;
            if (readTextFile(pathStr(f), evText)) {
                psych::parseEvents(evText, p.chart.events);
                std::stable_sort(p.chart.events.begin(), p.chart.events.end(),
                                 [](const psych::ChartEvent& a, const psych::ChartEvent& b) { return a.timeMs < b.timeMs; });
            }
        }
        {
            std::lock_guard<std::mutex> lk(customMtx_);
            if (customCacheKey_ != e.id) {
                customCacheKey_ = e.id;
                if (customInstKey_ != e.id) customInst_ = loadOrNull(e.inst);
                customInstKey_ = e.id;
                customVoices_ = loadOrNull(e.voices);
                customVp_ = loadOrNull(e.voicesP);
                customVo_ = loadOrNull(e.voicesO);
            }
            p.inst = customInst_;
            if (customVp_ || customVo_) {
                p.voiceP = customVp_;
                p.voiceO = customVo_;
            } else {
                p.voiceP = customVoices_;  // one shared vocal track: muted on any miss
            }
        }
    }
    if (mirror) {
        p.chart.mirror();
        if (p.voiceO) std::swap(p.voiceP, p.voiceO);
    }
    if (p.chart.notes.empty()) { p.error = "chart has no notes"; return p; }
    p.ok = true;
    return p;
}

PcmPtr Library::preview(const SongEntry& e) {
    if (e.builtin && e.def) return builtinAudio(*e.def).inst;
    std::lock_guard<std::mutex> lk(customMtx_);
    if (customInstKey_ != e.id) {
        // Only the instrumental is needed for a preview; prepare() loads voices.
        customInstKey_ = e.id;
        customCacheKey_.clear();
        customInst_ = loadOrNull(e.inst);
    }
    return customInst_;
}

PcmPtr Library::menuMusic(double* loopSec) {
    std::lock_guard<std::mutex> lk(menuMtx_);
    if (!menu_) {
        auto comp = compose(menuTheme(), true);
        auto p = std::make_shared<Pcm>();
        p->channels = 2;
        p->data = std::move(comp->inst);
        menu_ = p;
        menuLoop_ = comp->loopSec;
    }
    if (loopSec) *loopSec = menuLoop_;
    return menu_;
}

}  // namespace gh
