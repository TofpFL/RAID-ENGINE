#include "song_scan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <system_error>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "json.hpp"

namespace gh {

namespace fs = std::filesystem;

namespace {

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::vector<fs::path> listDir(const fs::path& dir, bool wantDirs) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool isDir = it->is_directory(e2);
        if (isDir == wantDirs) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool seekTo(FILE* f, uint64_t pos) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)pos, SEEK_SET) == 0;
#else
    return fseeko(f, off_t(pos), SEEK_SET) == 0;
#endif
}

bool isAudioExt(const std::string& lowerName) {
    return endsWithI(lowerName, ".ogg") || endsWithI(lowerName, ".mp3") || endsWithI(lowerName, ".wav") ||
           endsWithI(lowerName, ".qoa");
}

uint32_t colorFor(const std::string& s) {
    static const uint32_t palette[] = {0xB48CDC, 0x8CC8DC, 0xDCA08C, 0x8CDCA0, 0xDC8CB4, 0xC8DC8C, 0x8CA0DC, 0xDCC88C};
    uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
    return palette[h % 8];
}

int diffOrder(const std::string& d) {
    const std::string l = toLower(d);
    if (l == "easy") return 0;
    if (l == "normal") return 1;
    if (l == "hard") return 2;
    return 3;
}

// Letters and digits only, lower case: "Ntuzongere_Kugaruka" == "ntuzongere-kugaruka".
std::string fuzzyKey(std::string_view s) {
    std::string r;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) r += char(std::tolower((unsigned char)c));
    return r;
}

// Like fuzzyKey, but keeps non-Latin letters ("生𤯨 V2" -> "生𤯨v2").
std::string groupKey(std::string_view s) {
    std::string r;
    for (char c : s)
        if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80) r += char(std::tolower((unsigned char)c));
    return r;
}

void appendUnique(std::vector<std::string>& to, const std::vector<std::string>& more) {
    for (const std::string& s : more)
        if (std::find(to.begin(), to.end(), s) == to.end()) to.push_back(s);
}

fs::path childDirI(const fs::path& dir, std::string_view name) {
    for (const fs::path& d : listDir(dir, true))
        if (equalsI(pathStr(d.filename()), name)) return d;
    return {};
}

constexpr int kMaxDepth = 8;  // folders below the mod folder that are searched

// Folders that never hold playable charts.
bool skipFolder(const std::string& name) {
    static const char* names[] = {"musical-conversions", "source-duplicates", "manifest", "images", "characters",
                                  "stages", "scripts", "custom_events", "custom_notetypes", "shaders", "videos",
                                  "sounds", "music", "fonts", "weeks", "patterns", "replays", "dialogue", "obj"};
    const std::string l = toLower(name);
    if (l.empty() || l[0] == '.') return true;
    for (const char* n : names)
        if (l == n) return true;
    return false;
}

// Folders of data/ that hold no song: data/characters/, data/stages/...
bool notSongFolder(const std::string& name) {
    static const char* names[] = {"characters", "stages", "weeks", "dialogue", "scripts", "custom_events",
                                  "custom_notetypes", "notetypes", "events", "shaders", "images", "ui"};
    const std::string l = toLower(name);
    if (l.empty() || l[0] == '.') return true;
    for (const char* n : names)
        if (l == n) return true;
    return false;
}

// ---- telling charts from the other .json files of a mod

bool chartName(const std::string& lowerName) {
    return endsWithI(lowerName, ".json") && lowerName != "mod.json" && lowerName != "pack.json" &&
           lowerName.find("events") == std::string::npos && lowerName.find("-metadata") == std::string::npos &&
           lowerName.rfind("autochart-", 0) != 0 && lowerName.find(".pfm.") == std::string::npos;
}

// Every chart format has one of these keys ("bpm" or "song" alone are also
// in title screen and freeplay files).
bool hasChartKey(std::string_view s) {
    static const char* keys[] = {"\"notes\"", "\"sectionNotes\"", "\"scrollSpeed\"", "\"mustHitSection\""};
    for (const char* k : keys)
        if (s.find(k) != std::string_view::npos) return true;
    return false;
}

// A chart, not a character, stage, week, dialogue or title screen file. The
// first bytes tell for nearly every chart; one that starts with a long list
// of events is looked at further in and at its end.
bool chartContent(const fs::path& f) {
    const std::string p = pathStr(f);
    FILE* fp = std::fopen(p.c_str(), "rb");
    if (!fp) return false;
    std::string head(4096, '\0');
    head.resize(std::fread(head.data(), 1, head.size(), fp));
    bool found = hasChartKey(head);
    if (!found && head.size() == 4096) {
        constexpr size_t kWin = 96 * 1024;
        std::string more(kWin, '\0');
        more.resize(std::fread(more.data(), 1, more.size(), fp));
        found = hasChartKey(more);
        const uint64_t size = fileSize(p);
        if (!found && size > 4096 + kWin && seekTo(fp, size - kWin)) {
            more.assign(kWin, '\0');
            more.resize(std::fread(more.data(), 1, more.size(), fp));
            found = hasChartKey(more);
        }
    }
    std::fclose(fp);
    return found;
}

bool hasChartJson(const fs::path& dir) {
    for (const fs::path& f : listDir(dir, false))
        if (chartName(toLower(pathStr(f.filename()))) && chartContent(f)) return true;
    return false;
}

bool hasAudio(const fs::path& dir) {
    for (const fs::path& f : listDir(dir, false))
        if (isAudioExt(toLower(pathStr(f.filename())))) return true;
    return false;
}

// A content root holds data/<song>/ charts or songs/<song>/ audio (a Psych mod
// folder, or the assets/ folder of a full game build).
bool isContentRoot(const fs::path& d) {
    const fs::path data = childDirI(d, "data");
    for (const fs::path& s : listDir(data, true)) {
        const std::string n = pathStr(s.filename());
        if (equalsI(n, "songs") || (!notSongFolder(n) && hasChartJson(s))) return true;
    }
    for (const fs::path& s : listDir(childDirI(d, "songs"), true))
        if (hasAudio(s) || hasChartJson(s)) return true;
    return false;
}

// Folders of a mod holding assets that other mods may use: any folder with
// images/, characters/, stages/, sounds/, music/, videos/, shaders/ or fonts/
// (a Psych mod, a game build's assets/shared/, a week library...).
void collectAssetRoots(const fs::path& d, int depth, std::vector<std::string>& out) {
    static const char* kinds[] = {"images", "characters", "stages", "sounds", "music", "videos", "shaders", "fonts"};
    const std::vector<fs::path> kids = listDir(d, true);
    bool has = false;
    for (const fs::path& k : kids) {
        const std::string n = toLower(pathStr(k.filename()));
        for (const char* kind : kinds)
            if (n == kind) has = true;
    }
    if (has) out.push_back(pathStr(d) + "/");
    if (depth >= 6) return;
    for (const fs::path& k : kids) {
        const std::string n = toLower(pathStr(k.filename()));
        if (skipFolder(n) || n == "data" || n == "songs") continue;
        collectAssetRoots(k, depth + 1, out);
    }
}

// A folder searched for loose charts and audio, and the content root it sits
// in (whose assets its songs use), if any.
struct LooseDir {
    fs::path dir, root;
};
// A content root, and for one found inside another, that one.
struct Root {
    fs::path dir, outer;
};

void findRoots(const fs::path& d, int depth, std::vector<std::pair<fs::path, int>>& roots, std::vector<LooseDir>& loose) {
    if (isContentRoot(d)) {
        roots.push_back({d, depth});
        return;
    }
    loose.push_back({d, {}});
    if (depth >= 6) return;  // e.g. <mod>/export/release/windows/bin/assets/ of a source tree
    for (const fs::path& s : listDir(d, true))
        if (!skipFolder(pathStr(s.filename()))) findRoots(s, depth + 1, roots, loose);
}

void findInner(const fs::path& root, int depth, std::vector<Root>& inner, std::vector<LooseDir>& loose);

void innerAt(const fs::path& d, int depth, const fs::path& outer, std::vector<Root>& inner, std::vector<LooseDir>& loose) {
    if (isContentRoot(d)) {
        inner.push_back({d, outer});
        findInner(d, depth, inner, loose);
        return;
    }
    loose.push_back({d, outer});
    if (depth >= kMaxDepth) return;
    for (const fs::path& s : listDir(d, true))
        if (!skipFolder(pathStr(s.filename()))) innerAt(s, depth + 1, outer, inner, loose);
}

// What a content root holds besides its own data/ and songs/: asset
// libraries with charts of their own (a build's assets/preload/ and
// assets/shared/ next to assets/songs/), the mods/<mod>/ folders inside a
// build's mods/, chart folders such as charts/<song>/.
void findInner(const fs::path& root, int depth, std::vector<Root>& inner, std::vector<LooseDir>& loose) {
    if (depth >= kMaxDepth) return;
    for (const fs::path& c : listDir(root, true)) {
        const std::string n = pathStr(c.filename());
        if (skipFolder(n) || equalsI(n, "data") || equalsI(n, "songs")) continue;
        innerAt(c, depth + 1, root, inner, loose);
    }
}

// Is this .json a chart? Small files are parsed; huge ones are judged by
// their first and last bytes.
bool looksLikeChart(const fs::path& f, ChartPeek& pk) {
    const std::string n = toLower(pathStr(f.filename()));
    if (!chartName(n) || n.find("report") != std::string::npos) return false;
    const std::string p = pathStr(f);
    pk = peekChart(p);
    if (fileSize(p) < (4u << 20)) {
        std::string text;
        if (!readTextFile(p, text)) return false;
        json::Document d;
        if (!d.parse(text)) return false;
        const json::Value* notes = nullptr;
        if (pk.pulseForge) {  // PulseForge: a list of note objects, empty in a new project
            notes = d.get(d.root(), "notes");
            return notes && notes->type == json::Type::Array && d.size(*notes) > 0;
        }
        const json::Value* songV = d.get(d.root(), "song");
        const json::Value& sv = songV && songV->type == json::Type::Object ? *songV : d.root();
        // A loose file becomes a song of its own, so one without a single
        // note (a recovered header with only events) is left out.
        auto filled = [&](const json::Value* arr) { return arr && arr->type == json::Type::Array && d.size(*arr) > 0; };
        notes = d.get(sv, "notes");
        if (!notes) return filled(d.get(sv, "sectionNotes"));  // a lone section
        if (notes->type == json::Type::Object) {               // V-Slice: notes per difficulty
            for (size_t i = 0; i < d.size(*notes); ++i)
                if (filled(&d.valAt(*notes, i))) return true;
            return false;
        }
        if (notes->type != json::Type::Array || d.size(*notes) == 0) return false;
        if (!d.get(d.at(*notes, 0), "sectionNotes") && !d.get(d.at(*notes, 0), "mustHitSection")) return false;
        for (size_t i = 0; i < d.size(*notes); ++i)
            if (filled(d.get(d.at(*notes, i), "sectionNotes"))) return true;
        return false;
    }
    return pk.isChart;
}

// Size and a hash of the first and last 64 KB: identical copies of a chart
// in two folders are offered once.
std::string fileSignature(const fs::path& f) {
    const std::string p = pathStr(f);
    const uint64_t size = fileSize(p);
    uint64_t h = 1469598103934665603ull;
    if (FILE* fp = std::fopen(p.c_str(), "rb")) {
        std::vector<char> buf(65536);
        size_t n = std::fread(buf.data(), 1, buf.size(), fp);
        for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(buf[i])) * 1099511628211ull;
        if (size > 2 * buf.size() && seekTo(fp, size - buf.size())) {
            n = std::fread(buf.data(), 1, buf.size(), fp);
            for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(buf[i])) * 1099511628211ull;
        }
        std::fclose(fp);
    }
    return std::to_string(size) + ":" + std::to_string(h);
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

// "Traumatism_G-Side_Inst" -> "Traumatism_G-Side" (role 1: instrumental),
// "Song (Vocals)" -> "Song" (role 2: vocals); anything else is a full mix (0).
std::string audioBase(const std::string& stem, int& role) {
    role = 0;
    std::string s = stem;
    while (!s.empty() && (s.back() == ' ' || s.back() == ')')) s.pop_back();
    const size_t sep = s.find_last_of("-_ (");
    const std::string tail = toLower(sep == std::string::npos ? s : s.substr(sep + 1));
    if (tail == "inst" || tail == "instrumental") role = 1;
    else if (tail == "voices" || tail == "vocals" || tail == "vocal" || tail == "voice" || tail == "vox") role = 2;
    if (role == 0) return trim(stem);
    s = sep == std::string::npos ? std::string() : s.substr(0, sep);
    while (!s.empty() && (s.back() == ' ' || s.back() == '-' || s.back() == '_' || s.back() == '(')) s.pop_back();
    return trim(s);
}

bool genericAudioName(const std::string& base) {
    static const char* names[] = {"inst", "instrumental", "voices", "vocals", "audio", "song", "music", "track", "untitled"};
    const std::string k = fuzzyKey(base);
    if (groupKey(base).empty()) return true;
    for (const char* n : names)
        if (k == n) return true;
    return false;
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

void scanSongFolder(std::vector<SongEntry>& songs, const std::string& dirS, const std::string& audioDirS,
                    const std::string& source, const std::string& modRoot) {
    const fs::path dir = toPath(dirS);
    const std::string folder = pathStr(dir.filename());
    const std::string key = songPathName(folder);
    std::vector<fs::path> charts;
    fs::path meta, vsliceChart;
    for (const fs::path& f : listDir(dir, false)) {
        const std::string n = toLower(pathStr(f.filename()));
        if (!endsWithI(n, ".json")) continue;
        if (n.find("-metadata") != std::string::npos) { meta = f; continue; }
        if (!chartName(n) || !chartContent(f)) continue;  // dialogue.json and the like
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
    songs.push_back(e);
}

uint32_t le32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t be32(const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }

}  // namespace

// ------------------------------------------------------------ audio length

double audioSeconds(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    const uint64_t size = fileSize(path);
    std::vector<unsigned char> h(8192);
    h.resize(std::fread(h.data(), 1, h.size(), f));
    const size_t n = h.size();
    double secs = -1;
    if (n >= 12 && !std::memcmp(h.data(), "RIFF", 4) && !std::memcmp(h.data() + 8, "WAVE", 4)) {
        uint32_t byteRate = 0;
        for (size_t o = 12; o + 8 <= n;) {
            const uint32_t len = le32(&h[o + 4]);
            if (!std::memcmp(&h[o], "fmt ", 4) && o + 20 <= n) byteRate = le32(&h[o + 16]);
            if (!std::memcmp(&h[o], "data", 4)) {
                const uint64_t data = std::min<uint64_t>(len, size > o + 8 ? size - (o + 8) : 0);
                if (byteRate) secs = double(data) / byteRate;
                break;
            }
            o += 8 + size_t(len) + (len & 1);
        }
    } else if (n >= 4 && !std::memcmp(h.data(), "OggS", 4)) {
        uint32_t rate = 0;
        for (size_t o = 0; o + 16 <= n && !rate; ++o) {
            if (!std::memcmp(&h[o], "\x01vorbis", 7)) rate = le32(&h[o + 12]);
            else if (!std::memcmp(&h[o], "OpusHead", 8)) rate = 48000;  // Opus positions count 48 kHz samples
        }
        const uint64_t back = std::min<uint64_t>(size, 65536);
        std::vector<unsigned char> t(size_t(back), 0);
        if (rate && back >= 14 && seekTo(f, size - back)) {
            t.resize(std::fread(t.data(), 1, t.size(), f));
            for (size_t o = t.size() >= 14 ? t.size() - 14 : 0; o-- > 0;) {
                if (std::memcmp(&t[o], "OggS", 4)) continue;
                const uint64_t g = uint64_t(le32(&t[o + 6])) | uint64_t(le32(&t[o + 10])) << 32;
                if (g != ~0ull) secs = double(g) / rate;
                break;
            }
        }
    } else if (n >= 26 && !std::memcmp(h.data(), "fLaC", 4)) {
        const unsigned char* s = &h[8];  // STREAMINFO follows its 4-byte block header
        const uint32_t rate = uint32_t(s[10]) << 12 | uint32_t(s[11]) << 4 | s[12] >> 4;
        const uint64_t total = uint64_t(s[13] & 0x0F) << 32 | uint64_t(be32(&s[14]));
        if (rate) secs = double(total) / rate;
    } else if (n >= 16 && !std::memcmp(h.data(), "qoaf", 4)) {
        const uint32_t samples = be32(&h[4]);
        const uint32_t rate = uint32_t(h[9]) << 16 | uint32_t(h[10]) << 8 | h[11];
        if (rate) secs = double(samples) / rate;
    } else {
        // MP3: the bitrate of the first frame (after any ID3v2 tag) over the file size.
        uint64_t tag = 0;
        if (n >= 10 && !std::memcmp(h.data(), "ID3", 3))
            tag = 10 + (uint64_t(h[6] & 0x7F) << 21 | uint64_t(h[7] & 0x7F) << 14 | uint64_t(h[8] & 0x7F) << 7 | (h[9] & 0x7F));
        std::vector<unsigned char> fr(4096);
        if (tag < size && seekTo(f, tag)) {
            fr.resize(std::fread(fr.data(), 1, fr.size(), f));
            static const int v1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
            static const int v2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
            for (size_t o = 0; o + 4 <= fr.size(); ++o) {
                if (fr[o] != 0xFF || (fr[o + 1] & 0xE0) != 0xE0) continue;
                const int version = (fr[o + 1] >> 3) & 3, layer = (fr[o + 1] >> 1) & 3, bi = fr[o + 2] >> 4;
                if (version == 1 || layer != 1 || bi == 0 || bi == 15) continue;  // Layer III frames only
                const int kbps = version == 3 ? v1[bi] : v2[bi];
                if (kbps) secs = double(size - tag - o) * 8.0 / (kbps * 1000.0);
                break;
            }
        }
    }
    std::fclose(f);
    return std::isfinite(secs) ? secs : -1;
}

// ------------------------------------------------------------- songs/ ----

void scanSongsDir(const std::string& songsDirS, SongScan& out) {
    const fs::path songsDir = toPath(songsDirS);
    // Loose audio files in songs/: playable through AutoChart.
    for (const fs::path& f : listDir(songsDir, false)) {
        const std::string n = pathStr(f.filename());
        if (!isAudioExt(toLower(n))) continue;
        SongEntry e;
        e.title = prettyName(pathStr(f.stem()));
        e.subtitle = "songs - AutoChart";
        e.id = "custom:songs/" + songPathName(pathStr(f.stem()));
        e.color = colorFor(e.id);
        e.inst = pathStr(f);
        e.dataDir = songsDirS + (songsDirS.empty() || songsDirS.back() == '/' ? "" : "/");
        e.songKey = pathStr(f.stem());
        e.autoChart = true;
        e.diffs = {"Easy", "Normal", "Hard"};
        e.chartPaths = {"", "", ""};
        out.songs.push_back(e);
    }
    for (const fs::path& d : listDir(songsDir, true)) scanSongFolder(out.songs, pathStr(d), pathStr(d), "songs", pathStr(d) + "/");
}

// --------------------------------------------------------------- mods ----

void scanModsDir(const std::string& modsDirS, SongScan& out) {
    const fs::path modsDir = toPath(modsDirS);
    const std::set<std::string> disabled = readDisabledMods(modsDir);
    struct ModScan {
        fs::path dir;
        std::string folder, display;
        std::vector<Root> roots;
        std::vector<LooseDir> loose;
    };
    std::vector<ModScan> mods;
    for (const fs::path& mod : listDir(modsDir, true)) {
        ModScan m;
        m.dir = mod;
        m.folder = pathStr(mod.filename());
        if (m.folder.empty() || m.folder[0] == '.' || disabled.count(toLower(m.folder))) continue;
        m.display = modDisplayName(mod);
        std::vector<std::pair<fs::path, int>> top;
        findRoots(mod, 0, top, m.loose);
        for (const auto& r : top) m.roots.push_back({r.first, {}});
        // Then what those roots hold besides data/ and songs/. They come after
        // the roots found first, so the ids of the songs found there stay put.
        for (const auto& r : top) findInner(r.first, r.second, m.roots, m.loose);
        // Charts kept straight in data/ rather than in data/<song>/.
        for (size_t i = 0; i < m.roots.size(); ++i)
            if (const fs::path data = childDirI(m.roots[i].dir, "data"); !data.empty()) m.loose.push_back({data, m.roots[i].dir});
        std::vector<std::string>& packRoots = out.packRoots[m.folder];
        collectAssetRoots(mod, 0, packRoots);
        out.sharedRoots.insert(out.sharedRoots.end(), packRoots.begin(), packRoots.end());
        mods.push_back(std::move(m));
    }

    // Audio folders of every mod, by fuzzy song name (for charts without audio).
    std::map<std::string, std::vector<std::pair<size_t, fs::path>>> audioIndex;
    for (size_t mi = 0; mi < mods.size(); ++mi)
        for (const Root& r : mods[mi].roots)
            for (const fs::path& s : listDir(childDirI(r.dir, "songs"), true))
                if (hasAudio(s)) audioIndex[fuzzyKey(pathStr(s.filename()))].push_back({mi, s});
    auto indexedAudio = [&](const std::string& key, size_t mi, AudioSet& a) {
        auto it = audioIndex.find(key);
        if (it == audioIndex.end()) return;
        const fs::path* best = &it->second.front().second;
        for (auto& c : it->second)
            if (c.first == mi) { best = &c.second; break; }
        a = audioIn(*best);
    };

    std::vector<SongEntry>& songs = out.songs;
    // Ids are unique: two song folders with the same name (data/x and
    // data/charts/x) keep apart.
    std::set<std::string> taken;
    for (const SongEntry& e : songs) taken.insert(e.id);
    auto claim = [&](std::string id) {
        if (taken.count(id))
            for (int n = 2;; ++n)
                if (!taken.count(id + "~" + std::to_string(n))) {
                    id += "~" + std::to_string(n);
                    break;
                }
        taken.insert(id);
        return id;
    };
    for (size_t mi = 0; mi < mods.size(); ++mi) {
        const ModScan& m = mods[mi];
        const std::string source = m.display;
        const size_t modStart = songs.size();
        // ---- content roots (Psych mods, full game builds)
        std::set<std::string> charted;  // fuzzy keys with charts in this mod
        std::vector<std::pair<std::string, fs::path>> audioOnly;
        for (size_t ri = 0; ri < m.roots.size(); ++ri) {
            const fs::path& root = m.roots[ri].dir;
            std::vector<std::string> assetRoots = assetRootsOf(root);
            if (!m.roots[ri].outer.empty()) appendUnique(assetRoots, assetRootsOf(m.roots[ri].outer));
            const fs::path data = childDirI(root, "data");
            const fs::path songsDir = childDirI(root, "songs");
            auto audioFor = [&](const std::string& name) -> fs::path {
                for (const fs::path& s : listDir(songsDir, true))
                    if (fuzzyKey(pathStr(s.filename())) == fuzzyKey(name)) return s;
                return {};
            };
            std::set<std::string> done;
            const size_t before = songs.size();
            auto songFolder = [&](const fs::path& s) {
                const std::string name = pathStr(s.filename());
                const fs::path audio = audioFor(name);
                const size_t n0 = songs.size();
                scanSongFolder(songs, pathStr(s), pathStr(audio), source, pathStr(root) + "/");
                // Charts kept in songs/<song>/ too: offered as extra difficulties.
                if (!audio.empty() && hasChartJson(audio) && songs.size() > n0 && songs.back().songKey == name) {
                    SongEntry& e = songs.back();
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
            };
            for (const fs::path& s : listDir(data, true)) {
                const std::string name = pathStr(s.filename());
                if (equalsI(name, "songs")) {  // V-Slice layout: data/songs/<song>/
                    for (const fs::path& s2 : listDir(s, true)) {
                        const std::string n2 = pathStr(s2.filename());
                        scanSongFolder(songs, pathStr(s2), pathStr(audioFor(n2)), source, pathStr(root) + "/");
                        done.insert(fuzzyKey(n2));
                    }
                    continue;
                }
                if (notSongFolder(name)) continue;
                if (hasChartJson(s)) {
                    songFolder(s);
                    continue;
                }
                // A folder of song folders: data/charts/<song>/.
                for (const fs::path& s2 : listDir(s, true))
                    if (!notSongFolder(pathStr(s2.filename())) && hasChartJson(s2)) songFolder(s2);
            }
            for (const fs::path& s : listDir(songsDir, true)) {
                const std::string name = pathStr(s.filename());
                if (done.count(fuzzyKey(name))) continue;
                if (hasChartJson(s)) {
                    scanSongFolder(songs, pathStr(s), pathStr(s), source, pathStr(root) + "/");
                    done.insert(fuzzyKey(name));
                } else if (hasAudio(s)) {
                    audioOnly.push_back({name, s});
                }
            }
            for (size_t i = before; i < songs.size(); ++i) {
                SongEntry& e = songs[i];
                e.assetRoots = assetRoots;
                e.pack = m.folder;
                // Ids follow the folder (display names can repeat between mods).
                std::string id = "custom:" + m.folder + "/" + songPathName(e.songKey) + (e.autoChart ? "~auto" : "");
                if (ri > 0) id += "~" + std::to_string(ri);
                e.id = claim(id);
                if (!e.autoChart) charted.insert(fuzzyKey(e.songKey));
                if (!e.autoChart && e.inst.empty()) {
                    // No audio in this build: the same song from another folder or mod.
                    AudioSet a;
                    indexedAudio(fuzzyKey(e.songKey), mi, a);
                    e.inst = a.inst;
                    e.voices = a.voices;
                    e.voicesP = a.voicesP;
                    e.voicesO = a.voicesO;
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
            fs::path dir, root;
            std::vector<std::pair<std::string, std::string>> charts;  // label, path
            std::set<std::string> signatures;
        };
        std::vector<Group> groups;
        for (const LooseDir& ld : m.loose) {
            for (const fs::path& f : listDir(ld.dir, false)) {
                ChartPeek pk;
                if (!looksLikeChart(f, pk)) continue;
                std::string stem = pathStr(f.stem());
                // PulseForge editor files: <name>.pfchart.json, <name>.autosave.pfchart.json
                const char* editorFile = nullptr;
                if (endsWithI(stem, ".pfchart")) {
                    stem.resize(stem.size() - 8);
                    editorFile = " (project)";
                }
                if (endsWithI(stem, ".autosave")) {
                    stem.resize(stem.size() - 9);
                    editorFile = " (autosave)";
                }
                std::string title = trim(pk.song);
                std::string key = fuzzyKey(title);
                if (key.empty()) {
                    title = prettyName(stripVariant(stem));
                    key = fuzzyKey(title);
                }
                if (key.empty()) key = groupKey(title.empty() ? stem : title);
                Group* g = nullptr;
                for (Group& x : groups)
                    if (x.key == key) g = &x;
                if (!g) {
                    groups.push_back({title.empty() ? prettyName(stem) : title, key, ld.dir, ld.root, {}, {}});
                    g = &groups.back();
                }
                if (!g->signatures.insert(fileSignature(f)).second) continue;  // the same chart in two folders
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
                if (pk.pulseForge && !pk.difficulty.empty() && sk.rfind(key, 0) != 0) label = pk.difficulty;
                label = label.empty() ? (pk.difficulty.empty() ? "Normal" : prettyName(pk.difficulty)) : prettyName(label);
                if (editorFile) label += editorFile;
                auto labelTaken = [&](const std::string& l) {
                    for (auto& c : g->charts)
                        if (equalsI(c.first, l)) return true;
                    return false;
                };
                if (labelTaken(label))
                    for (int n = 2;; ++n)
                        if (!labelTaken(label + " (" + std::to_string(n) + ")")) {
                            label += " (" + std::to_string(n) + ")";
                            break;
                        }
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
            std::string id = "custom:" + m.folder + "/" + songPathName(g.title);
            if (taken.count(id)) id += "~" + fuzzyKey(pathStr(g.dir.filename()));
            e.id = claim(id);
            e.color = colorFor(e.id);
            e.modRoot = pathStr(g.root.empty() ? g.dir : g.root) + "/";
            e.dataDir = pathStr(g.dir) + "/";
            e.songKey = songPathName(g.title);
            e.assetRoots = {pathStr(g.dir) + "/"};
            if (!g.root.empty()) appendUnique(e.assetRoots, assetRootsOf(g.root));
            e.pack = m.folder;
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
            if (a.inst.empty()) indexedAudio(g.key, mi, a);
            e.inst = a.inst;
            e.voices = a.voices;
            e.voicesP = a.voicesP;
            e.voicesO = a.voicesO;
            if (e.inst.empty()) {
                e.noAudio = true;
                e.subtitle += " - no audio";
            }
            charted.insert(g.key);
            songs.push_back(e);
        }

        // ---- copies: a song whose charts are all in another entry of the same
        // song (a game build next to its source, a chart copied to two
        // folders) is offered once, by the entry that has them all.
        {
            const size_t n = songs.size() - modStart;
            std::vector<std::set<std::string>> sigs(n);
            std::map<std::string, std::vector<size_t>> bySong;
            for (size_t i = 0; i < n; ++i) {
                const SongEntry& e = songs[modStart + i];
                if (e.autoChart) continue;
                for (const std::string& p : e.chartPaths)
                    if (!p.empty()) sigs[i].insert(fileSignature(toPath(p)));
                bySong[groupKey(e.songKey)].push_back(i);
            }
            std::vector<bool> drop(n, false);
            for (auto& [key, list] : bySong)
                for (size_t a : list)
                    for (size_t b : list) {
                        if (a == b || drop[b] || sigs[a].empty()) continue;
                        const bool inside = std::includes(sigs[b].begin(), sigs[b].end(), sigs[a].begin(), sigs[a].end());
                        if (inside && (sigs[a].size() < sigs[b].size() || a > b)) {
                            drop[a] = true;
                            break;
                        }
                    }
            for (size_t i = n; i-- > 0;)
                if (drop[i]) songs.erase(songs.begin() + std::ptrdiff_t(modStart + i));
        }

        // ---- audio without a chart anywhere in the mod: AutoChart
        for (auto& [name, dir] : audioOnly) {
            if (charted.count(fuzzyKey(name))) continue;
            scanSongFolder(songs, pathStr(dir), pathStr(dir), source, pathStr(dir.parent_path().parent_path()) + "/");
            if (!songs.empty() && songs.back().songKey == name) {
                SongEntry& e = songs.back();
                e.assetRoots = assetRootsOf(dir.parent_path().parent_path());
                e.pack = m.folder;
                e.id = claim("custom:" + m.folder + "/" + songPathName(name) + "~auto");
            }
            charted.insert(fuzzyKey(name));
        }

        // ---- loose audio files that no song uses (a pack of songs without
        // charts): AutoChart. Shorter than 30 s is a sound effect, not a song.
        std::set<std::string> used;
        for (size_t i = modStart; i < songs.size(); ++i)
            for (const std::string* p : {&songs[i].inst, &songs[i].voices, &songs[i].voicesP, &songs[i].voicesO})
                if (!p->empty()) used.insert(*p);
        for (const LooseDir& ld : m.loose) {
            struct Track { std::string base, key, inst, mix, voices; };
            std::vector<Track> tracks;
            for (const fs::path& f : listDir(ld.dir, false)) {
                if (!isAudioExt(toLower(pathStr(f.filename())))) continue;
                const std::string p = pathStr(f);
                if (used.count(p)) continue;
                int role = 0;
                std::string base = audioBase(pathStr(f.stem()), role);
                if (genericAudioName(base)) base = pathStr(ld.dir.filename());
                const std::string key = groupKey(base);
                Track* t = nullptr;
                for (Track& x : tracks)
                    if (x.key == key) t = &x;
                if (!t) {
                    tracks.push_back({base, key, {}, {}, {}});
                    t = &tracks.back();
                }
                std::string& slot = role == 1 ? t->inst : role == 2 ? t->voices : t->mix;
                if (slot.empty()) slot = p;
            }
            for (const Track& t : tracks) {
                const std::string inst = !t.inst.empty() ? t.inst : t.mix;
                if (inst.empty()) continue;  // vocals alone
                const std::string fk = fuzzyKey(t.base);
                if (!fk.empty() && charted.count(fk)) continue;
                const double secs = audioSeconds(inst);
                if (secs >= 0 ? secs < 30 : fileSize(inst) < 400000) continue;
                SongEntry e;
                e.title = prettyName(t.base);
                e.subtitle = source + " - AutoChart";
                e.id = claim("custom:" + m.folder + "/" + songPathName(e.title) + "~auto");
                e.color = colorFor(e.id);
                e.modRoot = pathStr(ld.root.empty() ? ld.dir : ld.root) + "/";
                e.dataDir = pathStr(ld.dir) + "/";
                e.songKey = songPathName(e.title);
                e.assetRoots = {pathStr(ld.dir) + "/"};
                if (!ld.root.empty()) appendUnique(e.assetRoots, assetRootsOf(ld.root));
                e.pack = m.folder;
                e.inst = inst;
                e.voices = t.voices;  // "Song.ogg" next to "Song (Vocals).ogg" is the instrumental
                e.autoChart = true;
                e.diffs = {"Easy", "Normal", "Hard"};
                e.chartPaths = {"", "", ""};
                if (!fk.empty()) charted.insert(fk);
                songs.push_back(e);
            }
        }

        // The same song twice in a mod (a source tree and its build, two
        // versions): the subtitle says which folder each one comes from.
        std::map<std::string, int> titles;
        for (size_t i = modStart; i < songs.size(); ++i) ++titles[toLower(songs[i].title)];
        for (size_t i = modStart; i < songs.size(); ++i) {
            SongEntry& e = songs[i];
            if (titles[toLower(e.title)] < 2) continue;
            const fs::path rel = toPath(e.modRoot).lexically_relative(m.dir);
            std::string where = rel.empty() ? std::string() : pathStr(rel);
            while (!where.empty() && (where.back() == '/' || where.back() == '.')) where.pop_back();
            if (!where.empty()) e.subtitle += " - " + where;
        }
    }
}

std::vector<std::string> sharedRootsFor(const SongScan& scan, const SongEntry& e) {
    std::vector<std::string> out;
    if (auto it = scan.packRoots.find(e.pack); it != scan.packRoots.end()) out = it->second;
    out.insert(out.end(), scan.sharedRoots.begin(), scan.sharedRoots.end());
    return out;  // Files::setSharedRoots drops repeats and the song's own roots
}

}  // namespace gh
