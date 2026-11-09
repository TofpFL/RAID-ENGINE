#include "library.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>

#include "autochart.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "json.hpp"
#include "mod_install.hpp"

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

void Library::scan(const std::string& baseDir, const std::function<void(const std::string&, double)>& progress) {
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
    installReport_.clear();
    installModZips(pathStr(base / "mods"), installReport_, progress);

    // Loose audio files in songs/: playable through AutoChart.
    for (const fs::path& f : listDir(base / "songs", false)) {
        const std::string n = pathStr(f.filename());
        if (!isAudioExt(toLower(n))) continue;
        SongEntry e;
        e.title = prettyName(pathStr(f.stem()));
        e.subtitle = "songs - AutoChart";
        e.id = "custom:songs/" + songPathName(pathStr(f.stem()));
        e.color = colorFor(e.id);
        e.inst = pathStr(f);
        e.dataDir = pathStr(base / "songs") + "/";
        e.songKey = pathStr(f.stem());
        e.autoChart = true;
        e.diffs = {"Easy", "Normal", "Hard"};
        e.chartPaths = {"", "", ""};
        songs_.push_back(e);
    }

    for (const fs::path& d : listDir(base / "songs", true)) scanSongFolder(pathStr(d), pathStr(d), "songs", pathStr(d) + "/");

    scanMods(pathStr(base / "mods"));
    std::sort(songs_.begin() + std::ptrdiff_t(firstCustom), songs_.end(),
              [](const SongEntry& a, const SongEntry& b) { return toLower(a.title) < toLower(b.title); });
    customCount_ = int(songs_.size() - firstCustom);
    report_ = std::to_string(customCount_) + " custom song" + (customCount_ == 1 ? "" : "s") + " found";
}

// ------------------------------------------------------------- mods -----

namespace {

// Letters and digits only, lower case: "Ntuzongere_Kugaruka" == "ntuzongere-kugaruka".
std::string fuzzyKey(std::string_view s) {
    std::string r;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) r += char(std::tolower((unsigned char)c));
    return r;
}

fs::path childDirI(const fs::path& dir, std::string_view name) {
    for (const fs::path& d : listDir(dir, true))
        if (equalsI(pathStr(d.filename()), name)) return d;
    return {};
}

bool hasChartJson(const fs::path& dir) {
    for (const fs::path& f : listDir(dir, false)) {
        const std::string n = toLower(pathStr(f.filename()));
        if (endsWithI(n, ".json") && n.find("events") == std::string::npos && n.find("-metadata") == std::string::npos) return true;
    }
    return false;
}

bool hasAudio(const fs::path& dir) {
    for (const fs::path& f : listDir(dir, false))
        if (isAudioExt(toLower(pathStr(f.filename())))) return true;
    return false;
}

// Folders that never hold playable charts.
bool skipFolder(const std::string& name) {
    static const char* names[] = {"projects", "autosaves", "musical-conversions", "source-duplicates", "manifest",
                                  "images", "characters", "stages", "scripts", "custom_events", "custom_notetypes",
                                  "shaders", "videos", "sounds", "music", "fonts", "weeks", "patterns", "replays"};
    const std::string l = toLower(name);
    if (l.empty() || l[0] == '.') return true;
    for (const char* n : names)
        if (l == n) return true;
    return false;
}

// A content root holds data/<song>/ charts or songs/<song>/ audio (a Psych mod
// folder, or the assets/ folder of a full game build).
bool isContentRoot(const fs::path& d) {
    const fs::path data = childDirI(d, "data");
    if (!data.empty())
        for (const fs::path& s : listDir(data, true))
            if (hasChartJson(s) || equalsI(pathStr(s.filename()), "songs")) return true;
    const fs::path songs = childDirI(d, "songs");
    if (!songs.empty())
        for (const fs::path& s : listDir(songs, true))
            if (hasAudio(s) || hasChartJson(s)) return true;
    return false;
}

void findRoots(const fs::path& d, int depth, std::vector<fs::path>& roots, std::vector<fs::path>& loose) {
    if (isContentRoot(d)) {
        roots.push_back(d);
        return;
    }
    loose.push_back(d);
    if (depth >= 4) return;
    for (const fs::path& s : listDir(d, true))
        if (!skipFolder(pathStr(s.filename()))) findRoots(s, depth + 1, roots, loose);
}

// Is this .json a chart? Small files are parsed; huge ones are judged by
// their first and last bytes.
bool looksLikeChart(const fs::path& f, ChartPeek& pk) {
    const std::string n = toLower(pathStr(f.filename()));
    if (!endsWithI(n, ".json") || n == "mod.json" || n == "pack.json" || n.find("events") != std::string::npos ||
        n.find("-metadata") != std::string::npos || n.rfind("autochart-", 0) == 0 || n.find("report") != std::string::npos ||
        n.find(".pfchart.") != std::string::npos || n.find(".pfm.") != std::string::npos)
        return false;
    const std::string p = pathStr(f);
    pk = peekChart(p);
    if (fileSize(p) < (4u << 20)) {
        std::string text;
        if (!readTextFile(p, text)) return false;
        json::Document d;
        if (!d.parse(text)) return false;
        const json::Value* songV = d.get(d.root(), "song");
        const json::Value& sv = songV && songV->type == json::Type::Object ? *songV : d.root();
        const json::Value* notes = d.get(sv, "notes");
        if (!notes) return false;
        if (notes->type == json::Type::Object) return true;  // V-Slice
        if (notes->type != json::Type::Array) return false;
        if (d.size(*notes) == 0) return true;
        return d.get(d.at(*notes, 0), "sectionNotes") != nullptr || d.get(d.at(*notes, 0), "mustHitSection") != nullptr;
    }
    return pk.isChart;
}

// "unknown-black-hole-hard-1" -> "unknown-black-hole"
std::string stripVariant(std::string stem) {
    static const char* diffs[] = {"easy", "normal", "hard", "insane", "erect", "nightmare", "extreme", "expert", "hell",
                                  "accurate", "uncompressed", "compressed", "unfinished", "backup"};
    bool changed = true;
    while (changed && !stem.empty()) {
        changed = false;
        size_t dash = stem.find_last_of("-_ ");
        if (dash == std::string::npos) break;
        const std::string tail = toLower(stem.substr(dash + 1));
        bool strip = !tail.empty() && std::all_of(tail.begin(), tail.end(), [](char c) { return std::isdigit((unsigned char)c); });
        for (const char* d : diffs) strip |= tail == d;
        if (strip && dash > 0) { stem.resize(dash); changed = true; }
    }
    return stem;
}

struct AudioSet { std::string inst, voices, voicesP, voicesO; };

AudioSet audioIn(const fs::path& dir, std::string_view preferKey = {}) {
    AudioSet a;
    std::vector<fs::path> files;
    for (const fs::path& f : listDir(dir, false))
        if (isAudioExt(toLower(pathStr(f.filename())))) files.push_back(f);
    for (const fs::path& f : files) {
        const std::string stem = toLower(pathStr(f.stem()));
        const std::string p = pathStr(f);
        if (stem == "inst") a.inst = p;
        else if (stem == "voices") a.voices = p;
        else if (stem.rfind("voices-", 0) == 0 || stem.rfind("voices_", 0) == 0) {
            const std::string suf = stem.substr(7);
            if (suf == "player" || suf == "bf" || suf == "boyfriend" || suf == "playable") a.voicesP = p;
            else if (suf == "opponent" || suf == "dad" || suf == "opp" || suf == "enemy") a.voicesO = p;
            else if (a.voicesO.empty()) a.voicesO = p;
            else if (a.voicesP.empty()) a.voicesP = p;
        }
    }
    if (a.inst.empty())
        for (const fs::path& f : files)
            if (toLower(pathStr(f.stem())).rfind("inst", 0) == 0) { a.inst = pathStr(f); break; }
    if (a.inst.empty() && !preferKey.empty())
        for (const fs::path& f : files) {
            const std::string k = fuzzyKey(pathStr(f.stem()));
            if (k.find(preferKey) != std::string::npos || (!k.empty() && std::string(preferKey).find(k) == 0)) {
                a.inst = pathStr(f);
                break;
            }
        }
    return a;
}

std::string modDisplayName(const fs::path& modDir) {
    std::string text;
    if (readTextFile(pathStr(modDir / "mod.json"), text) || readTextFile(pathStr(modDir / "pack.json"), text)) {
        json::Document d;
        if (d.parse(text)) {
            std::string n(d.strOr(d.get(d.root(), "name"), ""));
            if (!n.empty()) return n;
        }
    }
    return pathStr(modDir.filename());
}

std::vector<std::string> assetRootsOf(const fs::path& root) {
    std::vector<std::string> out{pathStr(root) + "/"};
    std::vector<fs::path> extra;
    for (const fs::path& c : listDir(root, true)) {
        const std::string n = toLower(pathStr(c.filename()));
        if (n == "data" || n == "songs" || n == "images" || n == "characters" || n == "stages") continue;
        if (!childDirI(c, "images").empty() || !childDirI(c, "sounds").empty() || !childDirI(c, "music").empty())
            extra.push_back(c);
    }
    // shared/preload first, then the week libraries.
    std::stable_sort(extra.begin(), extra.end(), [](const fs::path& a, const fs::path& b) {
        auto rank = [](const fs::path& p) {
            const std::string n = toLower(pathStr(p.filename()));
            return n == "shared" ? 0 : n == "preload" ? 1 : 2;
        };
        return rank(a) < rank(b);
    });
    for (const fs::path& c : extra) out.push_back(pathStr(c) + "/");
    return out;
}

std::set<std::string> readDisabledMods(const fs::path& modsDir) {
    std::set<std::string> off;
    std::string text;
    if (!readTextFile(pathStr(modsDir / "modsList.txt"), text)) return off;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = trim(std::string_view(text).substr(p, e - p));
        p = e + 1;
        const size_t bar = line.rfind('|');
        if (bar != std::string::npos && trim(line.substr(bar + 1)) == "0") off.insert(toLower(line.substr(0, bar)));
    }
    return off;
}

}  // namespace

void Library::scanMods(const std::string& modsDirS) {
    const fs::path modsDir = toPath(modsDirS);
    const std::set<std::string> disabled = readDisabledMods(modsDir);
    struct ModScan {
        fs::path dir;
        std::string folder, display;
        std::vector<fs::path> roots, loose;
    };
    std::vector<ModScan> mods;
    for (const fs::path& mod : listDir(modsDir, true)) {
        ModScan m;
        m.dir = mod;
        m.folder = pathStr(mod.filename());
        if (m.folder.empty() || m.folder[0] == '.' || disabled.count(toLower(m.folder))) continue;
        m.display = modDisplayName(mod);
        findRoots(mod, 0, m.roots, m.loose);
        mods.push_back(std::move(m));
    }

    // Audio folders of every mod, by fuzzy song name (for loose charts).
    std::map<std::string, std::vector<std::pair<size_t, fs::path>>> audioIndex;
    for (size_t mi = 0; mi < mods.size(); ++mi)
        for (const fs::path& r : mods[mi].roots)
            if (const fs::path songs = childDirI(r, "songs"); !songs.empty())
                for (const fs::path& s : listDir(songs, true))
                    if (hasAudio(s)) audioIndex[fuzzyKey(pathStr(s.filename()))].push_back({mi, s});

    for (size_t mi = 0; mi < mods.size(); ++mi) {
        const ModScan& m = mods[mi];
        const std::string source = m.display;
        // ---- content roots (Psych mods, full game builds)
        std::set<std::string> charted;  // fuzzy keys with charts in this mod
        std::vector<std::pair<std::string, fs::path>> audioOnly;
        for (size_t ri = 0; ri < m.roots.size(); ++ri) {
            const fs::path& root = m.roots[ri];
            const std::vector<std::string> assetRoots = assetRootsOf(root);
            const fs::path data = childDirI(root, "data");
            const fs::path songs = childDirI(root, "songs");
            auto audioFor = [&](const std::string& name) -> fs::path {
                for (const fs::path& s : listDir(songs, true))
                    if (fuzzyKey(pathStr(s.filename())) == fuzzyKey(name)) return s;
                return {};
            };
            std::set<std::string> done;
            const size_t before = songs_.size();
            for (const fs::path& s : listDir(data, true)) {
                const std::string name = pathStr(s.filename());
                if (equalsI(name, "songs")) {  // V-Slice layout: data/songs/<song>/
                    for (const fs::path& s2 : listDir(s, true)) {
                        const std::string n2 = pathStr(s2.filename());
                        scanSongFolder(pathStr(s2), pathStr(audioFor(n2)), source, pathStr(root) + "/");
                        done.insert(fuzzyKey(n2));
                    }
                    continue;
                }
                if (!hasChartJson(s)) continue;
                const fs::path audio = audioFor(name);
                scanSongFolder(pathStr(s), pathStr(audio), source, pathStr(root) + "/");
                // Charts kept in songs/<song>/ too: offered as extra difficulties.
                if (!audio.empty() && hasChartJson(audio) && songs_.size() > before && songs_.back().songKey == name) {
                    SongEntry& e = songs_.back();
                    for (const fs::path& f : listDir(audio, false)) {
                        ChartPeek pk;
                        if (!looksLikeChart(f, pk)) continue;
                        std::string label = prettyName(pathStr(f.stem()));
                        for (const std::string& d : e.diffs)
                            if (equalsI(d, label)) label += " (songs)";
                        e.diffs.push_back(label);
                        e.chartPaths.push_back(pathStr(f));
                    }
                }
                done.insert(fuzzyKey(name));
            }
            for (const fs::path& s : listDir(songs, true)) {
                const std::string name = pathStr(s.filename());
                if (done.count(fuzzyKey(name))) continue;
                if (hasChartJson(s)) {
                    scanSongFolder(pathStr(s), pathStr(s), source, pathStr(root) + "/");
                    done.insert(fuzzyKey(name));
                } else if (hasAudio(s)) {
                    audioOnly.push_back({name, s});
                }
            }
            for (size_t i = before; i < songs_.size(); ++i) {
                SongEntry& e = songs_[i];
                e.assetRoots = assetRoots;
                // Ids follow the folder (display names can repeat between mods).
                e.id = "custom:" + m.folder + "/" + songPathName(e.songKey) + (e.autoChart ? "~auto" : "");
                if (ri > 0) e.id += "~" + std::to_string(ri);
                if (!e.autoChart) charted.insert(fuzzyKey(e.songKey));
                if (!e.autoChart && e.inst.empty()) {
                    // No audio in this build: the same song from another folder or mod.
                    auto it = audioIndex.find(fuzzyKey(e.songKey));
                    if (it != audioIndex.end()) {
                        const fs::path* best = &it->second.front().second;
                        for (auto& c : it->second)
                            if (c.first == mi) { best = &c.second; break; }
                        const AudioSet a = audioIn(*best);
                        e.inst = a.inst;
                        e.voices = a.voices;
                        e.voicesP = a.voicesP;
                        e.voicesO = a.voicesO;
                    }
                    if (e.inst.empty()) {
                        e.noAudio = true;
                        e.subtitle += " - no audio";
                    }
                }
            }
        }

        // ---- loose charts anywhere else in the mod
        struct Group {
            std::string title, key;
            fs::path dir;
            std::vector<std::pair<std::string, std::string>> charts;  // label, path
        };
        std::vector<Group> groups;
        for (const fs::path& d : m.loose) {
            for (const fs::path& f : listDir(d, false)) {
                ChartPeek pk;
                if (!looksLikeChart(f, pk)) continue;
                const std::string stem = pathStr(f.stem());
                std::string title = trim(pk.song);
                std::string key = fuzzyKey(title);
                if (key.empty()) {
                    title = prettyName(stripVariant(stem));
                    key = fuzzyKey(title);
                }
                if (key.empty()) key = fuzzyKey(stem);
                Group* g = nullptr;
                for (Group& x : groups)
                    if (x.key == key) g = &x;
                if (!g) {
                    groups.push_back({title.empty() ? prettyName(stem) : title, key, d, {}});
                    g = &groups.back();
                }
                // Label: the file name without the song name.
                std::string label = stem;
                const std::string sk = fuzzyKey(stem);
                if (sk.rfind(key, 0) == 0) {
                    size_t cut = 0, matched = 0;
                    while (cut < stem.size() && matched < key.size()) {
                        if (std::isalnum((unsigned char)stem[cut])) ++matched;
                        ++cut;
                    }
                    label = stem.substr(cut);
                    while (!label.empty() && !std::isalnum((unsigned char)label[0])) label.erase(0, 1);
                }
                label = label.empty() ? "Normal" : prettyName(label);
                for (auto& c : g->charts)
                    if (equalsI(c.first, label)) label += " (" + std::to_string(g->charts.size() + 1) + ")";
                g->charts.push_back({label, pathStr(f)});
            }
        }
        for (Group& g : groups) {
            std::stable_sort(g.charts.begin(), g.charts.end(), [](const auto& a, const auto& b) {
                const int oa = diffOrder(a.first.substr(0, a.first.find(' '))), ob = diffOrder(b.first.substr(0, b.first.find(' ')));
                return oa != ob ? oa < ob : a.first < b.first;
            });
            SongEntry e;
            e.title = g.title;
            e.subtitle = source;
            e.id = "custom:" + m.folder + "/" + songPathName(g.title);
            for (const SongEntry& o : songs_)
                if (o.id == e.id) e.id += "~" + fuzzyKey(pathStr(g.dir.filename()));
            e.color = colorFor(e.id);
            e.modRoot = pathStr(g.dir) + "/";
            e.dataDir = pathStr(g.dir) + "/";
            e.songKey = songPathName(g.title);
            e.assetRoots = {e.modRoot};
            for (auto& c : g.charts) { e.diffs.push_back(c.first); e.chartPaths.push_back(c.second); }
            // Audio: next to the charts, else any mod's songs/<song>/ (this mod first).
            AudioSet a;
            size_t groupsInDir = 0;
            for (const Group& o : groups) groupsInDir += o.dir == g.dir;
            {
                AudioSet here = audioIn(g.dir, g.key);
                const bool named = !here.inst.empty() && fuzzyKey(toPath(here.inst).stem().string()).find(g.key) != std::string::npos;
                const bool instName = !here.inst.empty() && toLower(pathStr(toPath(here.inst).stem())).rfind("inst", 0) == 0;
                if (named || (instName && groupsInDir == 1)) a = here;
            }
            if (a.inst.empty()) {
                auto it = audioIndex.find(g.key);
                if (it != audioIndex.end()) {
                    const fs::path* best = &it->second.front().second;
                    for (auto& c : it->second)
                        if (c.first == mi) { best = &c.second; break; }
                    a = audioIn(*best);
                }
            }
            e.inst = a.inst;
            e.voices = a.voices;
            e.voicesP = a.voicesP;
            e.voicesO = a.voicesO;
            if (e.inst.empty()) {
                e.noAudio = true;
                e.subtitle += " - no audio";
            }
            charted.insert(g.key);
            songs_.push_back(e);
        }

        // ---- audio without a chart anywhere in the mod: AutoChart
        for (auto& [name, dir] : audioOnly) {
            if (charted.count(fuzzyKey(name))) continue;
            scanSongFolder(pathStr(dir), pathStr(dir), source, pathStr(dir.parent_path().parent_path()) + "/");
            if (!songs_.empty() && songs_.back().songKey == name) {
                songs_.back().assetRoots = assetRootsOf(dir.parent_path().parent_path());
                songs_.back().id = "custom:" + m.folder + "/" + songPathName(name) + "~auto";
            }
            charted.insert(fuzzyKey(name));
        }
    }
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
    SongEntry e;
    e.title = prettyName(folder);
    e.subtitle = source;
    e.id = "custom:" + source + "/" + key;
    e.color = colorFor(e.id);
    e.modRoot = modRoot;
    e.dataDir = dirS + "/";
    e.songKey = folder;

    if (charts.empty()) {
        // No chart at all: offer AutoChart when the folder has audio.
        e.autoChart = true;
        e.subtitle = source + " - AutoChart";
        e.diffs = {"Easy", "Normal", "Hard"};
        e.chartPaths = {"", "", ""};
    } else if (!vsliceChart.empty() && !meta.empty()) {
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
    if (e.autoChart && e.inst.empty()) {
        // Any single audio file will do as the instrumental.
        for (const fs::path& f : listDir(toPath(audioDirS.empty() ? dirS : audioDirS), false))
            if (isAudioExt(toLower(pathStr(f.filename())))) { e.inst = pathStr(f); break; }
        if (e.inst.empty()) return;
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
    } else if (e.autoChart) {
        int d = 1;
        for (size_t i = 0; i < e.diffs.size(); ++i)
            if (e.diffs[i] == diff) d = int(i);
        {
            std::lock_guard<std::mutex> lk(customMtx_);
            if (customInstKey_ != e.id || !customInst_) {
                customInstKey_ = e.id;
                customInst_ = loadOrNull(e.inst);
            }
            customCacheKey_.clear();
            p.inst = customInst_;
        }
        if (!p.inst) { p.error = "cannot decode " + e.inst; return p; }
        AutoChartInfo info;
        if (!autoChart(p.inst->data.data(), p.inst->frames(), p.inst->channels, Mixer::kRate, d, p.chart, &info)) {
            p.error = "AutoChart found no rhythm in this audio";
            return p;
        }
        p.chart.title = e.title;
        p.chart.difficulty = diff;
    } else {
        size_t idx = 0;
        for (size_t i = 0; i < e.diffs.size(); ++i)
            if (e.diffs[i] == diff) idx = i;
        if (idx >= e.chartPaths.size()) { p.error = "no chart for difficulty " + diff; return p; }
        std::string meta;
        if (!e.metaPath.empty()) readTextFile(e.metaPath, meta);
        std::string err;
        if (!loadChartFile(e.chartPaths[idx], diff, p.chart, err, meta)) { p.error = err; return p; }
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
