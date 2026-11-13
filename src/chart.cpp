#include "chart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

#include "chart_stream.hpp"
#include "common.hpp"
#include "json.hpp"

namespace gh {

bool readTextFile(const std::string& path, std::string& out) {
#if defined(_WIN32)
    // Paths are UTF-8; _wfopen needs UTF-16 (video.cpp's toPath does the same).
    const std::wstring w = std::filesystem::path(std::u8string(path.begin(), path.end())).wstring();
    FILE* f = _wfopen(w.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
#if defined(_WIN32)
    // ftell's long is 32-bit: files past 2 GiB need the 64-bit variants.
    if (_fseeki64(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftello(f);
    std::fseek(f, 0, SEEK_SET);
#endif
    if (sz < 0) { std::fclose(f); return false; }
    out.resize(size_t(sz));
    size_t rd = 0;
    while (rd < size_t(sz)) {
        const size_t got = sz > 0 ? std::fread(out.data() + rd, 1, size_t(sz) - rd, f) : 0;
        if (got == 0) break;
        rd += got;
    }
    std::fclose(f);
    out.resize(rd);
    return true;
}

// ---------------------------------------------------------------- timing ---

static size_t tempoIndexAtMs(const std::vector<TempoPoint>& t, double ms) {
    size_t lo = 0, hi = t.size();
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].timeMs <= ms) lo = mid; else hi = mid;
    }
    return lo;
}

double Chart::bpmAt(double ms) const {
    if (tempo.empty()) return 120.0;
    return tempo[tempoIndexAtMs(tempo, ms)].bpm;
}

double Chart::beatAt(double ms) const {
    if (tempo.empty()) return ms * 120.0 / 60000.0;
    const TempoPoint& p = tempo[tempoIndexAtMs(tempo, ms)];
    return p.beat + (ms - p.timeMs) * p.bpm / 60000.0;
}

double Chart::msAtBeat(double beat) const {
    if (tempo.empty()) return beat * 60000.0 / 120.0;
    size_t lo = 0, hi = tempo.size();
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (tempo[mid].beat <= beat) lo = mid; else hi = mid;
    }
    const TempoPoint& p = tempo[lo];
    return p.timeMs + (beat - p.beat) * 60000.0 / p.bpm;
}

int Chart::focusAt(double ms) const {
    if (focus.empty()) return 0;
    size_t lo = 0, hi = focus.size();
    if (focus[0].timeMs > ms) return focus[0].side;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (focus[mid].timeMs <= ms) lo = mid; else hi = mid;
    }
    return focus[lo].side;
}

int Chart::sectionAt(double ms) const {
    if (sections.empty() || ms < sections[0].timeMs) return -1;
    size_t lo = 0, hi = sections.size();
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (sections[mid].timeMs <= ms) lo = mid; else hi = mid;
    }
    return int(lo);
}

void Chart::finalize(double baseBpm) {
    if (!(baseBpm > 0)) baseBpm = 120;
    if (tempo.empty()) tempo.push_back({0, baseBpm, 0});
    std::stable_sort(tempo.begin(), tempo.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.timeMs < b.timeMs; });
    if (tempo[0].timeMs > 0) tempo.insert(tempo.begin(), TempoPoint{0, tempo[0].bpm, 0});
    tempo[0].beat = tempo[0].timeMs * tempo[0].bpm / 60000.0;
    for (size_t i = 1; i < tempo.size(); ++i) {
        if (!(tempo[i].bpm > 0)) tempo[i].bpm = tempo[i - 1].bpm;
        tempo[i].beat = tempo[i - 1].beat + (tempo[i].timeMs - tempo[i - 1].timeMs) * tempo[i - 1].bpm / 60000.0;
    }

    keys = clampv(keys, 1, 9);
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n) {
        return !std::isfinite(n.time) || n.lane >= keys || n.side > 1;
    }), notes.end());
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        if (a.time != b.time) return a.time < b.time;
        if (a.side != b.side) return a.side < b.side;
        return a.lane < b.lane;
    });

    // Stacked notes (same side+lane within 1 ms) become one note that counts
    // as all of them: one press hits the whole stack, and the score, combo and
    // note counts include every note of the file.
    std::vector<long long> lastKept(size_t(2 * keys), -1);
    std::vector<Note> clean;
    clean.reserve(notes.size());
    for (const Note& n : notes) {
        long long& lk = lastKept[size_t(n.side * keys + n.lane)];
        if (lk >= 0 && std::fabs(clean[size_t(lk)].time - n.time) < 1.0 &&
            uint32_t(clean[size_t(lk)].stack) + n.stack <= kMaxStack) {
            Note& k = clean[size_t(lk)];
            k.length = std::max(k.length, n.length);
            if (n.kind == kNormal) k.kind = kNormal;
            k.stack = uint16_t(k.stack + n.stack);
            continue;
        }
        lk = (long long)clean.size();
        clean.push_back(n);
    }
    notes.swap(clean);

    lengthMs = 0;
    playerNotes = opponentNotes = 0;
    for (const Note& n : notes) {
        lengthMs = std::max(lengthMs, n.time + std::max(0.0, n.length));
        (n.side ? playerNotes : opponentNotes) += n.stack;
    }

    std::stable_sort(focus.begin(), focus.end(), [](const FocusPoint& a, const FocusPoint& b) { return a.timeMs < b.timeMs; });
    if (focus.empty() && !notes.empty()) {
        // Derive camera focus per bar from which side has more notes.
        double barMs = 4 * 60000.0 / tempo[0].bpm;
        size_t i = 0;
        for (double t = 0; t <= lengthMs; t += barMs) {
            int cnt[2] = {0, 0};
            while (i < notes.size() && notes[i].time < t + barMs) cnt[notes[i++].side]++;
            if (cnt[0] + cnt[1] == 0) continue;
            uint8_t side = cnt[1] >= cnt[0] ? 1 : 0;
            if (focus.empty() || focus.back().side != side) focus.push_back({t, side});
        }
    }
}

uint16_t Chart::typeIndex(const std::string& name) {
    if (name.empty()) return 0;
    for (size_t i = 0; i < noteTypes.size(); ++i)
        if (noteTypes[i] == name) return uint16_t(i);
    if (noteTypes.size() >= 65535) return 0;
    noteTypes.push_back(name);
    return uint16_t(noteTypes.size() - 1);
}

void Chart::mirror() {
    for (Note& n : notes) n.side ^= 1;
    for (FocusPoint& f : focus) f.side ^= 1;
    std::swap(player, opponent);
    std::swap(playerNotes, opponentNotes);
}

// --------------------------------------------------------------- parsing ---

static void applyNoteType(std::string_view type, Note& n, Chart& c) {
    if (type.empty()) return;
    n.type = c.typeIndex(std::string(type));
    std::string t = toLower(type);
    static const char* mines[] = {"hurt", "kill", "death", "mine", "poison", "danger", "avoid"};
    for (const char* m : mines)
        if (t.find(m) != std::string::npos) { n.kind = kMine; return; }
    if (t.find("no anim") != std::string::npos) n.flags |= kFlagNoAnim | kFlagNoMissAnim;
    if (t.find("alt") != std::string::npos) n.flags |= kFlagAlt;
    if (t == "gf sing" || t == "gf") n.flags |= kFlagGF;
    if (t == "hey!" || t == "hey") n.flags |= kFlagHey;
}

// How badly a key count fits the noteData values a chart uses (0 = fits).
// Values past both sides cost the most; a side that has notes but leaves
// some of its lanes empty costs that many lanes times its share of notes
// (with too few keys, one side's lanes spill into the other's first lanes;
// with too many, the last lanes of a side stay empty).
static double keyMisfit(const std::array<uint64_t, 64>& dataCount, uint64_t total, int keys) {
    if (total == 0) return 0;
    uint64_t past = 0;
    for (size_t d = size_t(2 * keys); d < dataCount.size(); ++d) past += dataCount[d];
    double cost = 50.0 * double(past) / double(total);
    for (int side = 0; side < 2; ++side) {
        uint64_t n = 0;
        int empty = 0;
        for (int l = 0; l < keys; ++l) {
            const uint64_t c = dataCount[size_t(side * keys + l)];
            n += c;
            empty += c == 0;
        }
        cost += double(empty) * double(n) / double(total);
    }
    return cost;
}

// Key count of a legacy/Psych chart. Only an explicit count makes it an
// extra-key chart: Psych and JS Engine play every other chart with 4 keys
// (lane = noteData % 4), whatever values its notes use.
static int keysFromMetadata(const json::Document& doc, const json::Value& s, const std::array<uint64_t, 64>& dataCount,
                            bool extraKeys) {
    if (!extraKeys) return 4;  // the player turned extra keys off
    int keys = int(json::Document::num(doc.get(s, "keyCount"), 0));  // Leather Engine
    if (keys >= 1 && keys <= 9) return keys;
    // "mania" numbers key counts differently from fork to fork: Dave & Bambi /
    // Kade / Andromeda forks ("options": 0=4K 1=6K 2=7K 3=9K), Psych Extra Keys
    // (keys = mania + 1) and Shaggy-style multi-key (4, 6, 9, 5, 7, 8, 1, 2, 3).
    // Psych and JS Engine ignore all of them and play 4 keys, so charts made
    // there often keep a "mania" their notes do not use (Eons: options.mania 3,
    // notes on lanes 0-3 only). Every reading, 4 keys included, is checked
    // against the notes and the best fit wins.
    static const int dnb[] = {4, 6, 7, 9};
    static const int shaggy[] = {4, 6, 9, 5, 7, 8, 1, 2, 3};
    const json::Value* opts = doc.get(s, "options");
    const double om = opts ? json::Document::num(doc.get(*opts, "mania"), -1) : -1;
    const double sm = json::Document::num(doc.get(s, "mania"), -1);
    const bool songMania = sm >= 0 && sm <= 8, optMania = om >= 0 && om <= 3;
    if (!songMania && !optMania) return 4;
    // On a tie the first candidate wins: a song-level "mania" (Extra Keys,
    // Kade, Shaggy) before 4 keys; 4 keys before an "options" one.
    std::vector<int> candidates;
    if (songMania) {
        const int m = int(sm);
        candidates = {m + 1, m <= 3 ? dnb[m] : 0, shaggy[m]};
    }
    candidates.push_back(4);
    if (optMania) candidates.push_back(dnb[int(om)]);
    uint64_t total = 0;
    for (uint64_t c : dataCount) total += c;
    int best = 0;
    double bestCost = 0;
    for (int k : candidates) {
        if (k < 1 || k > 9) continue;
        const double cost = keyMisfit(dataCount, total, k);
        if (best == 0 || cost < bestCost - 1e-9) {
            best = k;
            bestCost = cost;
        }
    }
    return best;
}

static bool parsePsych(const json::Document& doc, Chart& out, std::string& err, const StreamedNotes* sn = nullptr,
                       bool extraKeys = true) {
    using json::Type;
    const json::Value& root = doc.root();
    const json::Value* songV = doc.get(root, "song");
    const json::Value& s = (songV && songV->type == Type::Object) ? *songV : root;

    out.title = std::string(doc.strOr(doc.get(s, "song"), doc.strOr(doc.get(s, "songName"), "")));
    double bpm = json::Document::num(doc.get(s, "bpm"), 100.0);
    if (!(bpm > 0) || !std::isfinite(bpm)) bpm = 100.0;
    out.speed = clampv(json::Document::num(doc.get(s, "speed"), 1.0), 0.1, 20.0);
    out.player = std::string(doc.strOr(doc.get(s, "player1"), ""));
    out.opponent = std::string(doc.strOr(doc.get(s, "player2"), ""));
    out.gf = std::string(doc.strOr(doc.get(s, "gfVersion"), doc.strOr(doc.get(s, "player3"), doc.strOr(doc.get(s, "gf"), ""))));
    out.stage = std::string(doc.strOr(doc.get(s, "stage"), ""));
    const bool v1 = doc.strOr(doc.get(s, "format"), "").find("psych_v1") != std::string_view::npos;

    const json::Value* sections = doc.get(s, "notes");
    // A lone section ({"mustHitSection", "sectionNotes"}: a fragment recovered
    // from a bigger chart) plays as a chart of one section.
    const bool lone = (!sections || sections->type != Type::Array) && doc.get(s, "sectionNotes");
    if (!lone && (!sections || sections->type != Type::Array)) { err = "chart has no 'notes' section array"; return false; }
    const size_t sectionCount = lone ? 1 : doc.size(*sections);
    auto sectionAt = [&](size_t i) -> const json::Value& { return lone ? s : doc.at(*sections, i); };

    // Pass 1: how often each noteData value is used.
    std::array<uint64_t, 64> dataCount{};
    if (sn) dataCount = sn->dataCount;
    for (size_t i = 0; i < sectionCount; ++i) {
        const json::Value* sv = doc.get(sectionAt(i), "sectionNotes");
        if (!sv || sv->type != Type::Array) continue;
        for (size_t j = 0; j < doc.size(*sv); ++j) {
            const json::Value& n = doc.at(*sv, j);
            if (n.type != Type::Array || doc.size(n) < 2) continue;
            double d = json::Document::num(&doc.at(n, 1), -1);
            if (d >= 0 && d < 64) ++dataCount[size_t(d)];
        }
    }
    out.keys = keysFromMetadata(doc, s, dataCount, extraKeys);
    const int keys = out.keys;

    double secStart = 0, curBpm = bpm;
    out.tempo.push_back({0, bpm, 0});
    {
        uint64_t total = 0;
        for (uint64_t c : dataCount) total += c;
        out.notes.reserve(sn ? sn->notes.size() : size_t(total));
    }
    std::vector<uint16_t> streamTypes;  // StreamedNotes type index -> chart type index
    if (sn) {
        streamTypes.push_back(0);
        for (const std::string& t : sn->types) {
            Note probe;
            applyNoteType(t, probe, out);
            streamTypes.push_back(probe.type);
        }
    }
    // Values past the two sides fold back like Psych/JS Engine: lane = data % keys,
    // and anything from `keys` on belongs to the other side.
    auto addNote = [&](double t, int di, double l, bool must, double stepMs, bool altSec, bool gfSec,
                       const std::string_view typeName, bool hasTypeName, bool altBool, uint16_t streamType,
                       uint16_t stack = 1) {
        if (!std::isfinite(t) || di < 0 || di >= 64) return;
        Note n;
        n.time = t;
        n.length = (std::isfinite(l) && l >= stepMs * 0.999) ? l : 0.0;
        n.lane = uint8_t(di % keys);
        n.stack = std::max<uint16_t>(1, stack);
        if (v1) n.side = di < keys ? 1 : 0;
        else n.side = ((di < keys) ? must : !must) ? 1 : 0;
        if (hasTypeName) applyNoteType(typeName, n, out);
        else if (streamType) {
            const std::string& tn = out.noteTypes[streamTypes[streamType]];
            applyNoteType(tn, n, out);
        }
        if (altBool) n.flags |= kFlagAlt;
        if (altSec && n.side == 0) n.flags |= kFlagAlt;
        if (gfSec && di < keys) n.flags |= kFlagGF;
        out.notes.push_back(n);
    };
    for (size_t i = 0; i < sectionCount; ++i) {
        const json::Value& sec = sectionAt(i);
        if (sec.type != Type::Object) continue;
        const bool must = json::Document::boolean(doc.get(sec, "mustHitSection"), true);
        if (json::Document::boolean(doc.get(sec, "changeBPM"), false)) {
            double nb = json::Document::num(doc.get(sec, "bpm"), curBpm);
            if (nb > 0 && std::isfinite(nb) && std::fabs(nb - curBpm) > 1e-9) {
                curBpm = nb;
                out.tempo.push_back({secStart, nb, 0});
            }
        }
        double beats = json::Document::num(doc.get(sec, "sectionBeats"), -1);
        if (!(beats > 0) || !std::isfinite(beats)) {
            beats = json::Document::num(doc.get(sec, "lengthInSteps"), 16) / 4.0;
            if (!(beats > 0) || !std::isfinite(beats)) beats = 4;
        }
        const uint8_t fside = must ? 1 : 0;
        if (out.focus.empty() || out.focus.back().side != fside) out.focus.push_back({secStart, fside});
        const bool altSec = json::Document::boolean(doc.get(sec, "altAnim"), false);
        const bool gfSec = json::Document::boolean(doc.get(sec, "gfSection"), false);
        out.sections.push_back({secStart, must, gfSec, altSec, beats, curBpm});
        const double stepMs = 15000.0 / curBpm;

        const json::Value* sv = doc.get(sec, "sectionNotes");
        if (sv && sv->type == Type::Array) {
            for (size_t j = 0; j < doc.size(*sv); ++j) {
                const json::Value& nv = doc.at(*sv, j);
                if (sn && nv.type == Type::Object) {
                    const json::Value* mk = doc.get(nv, "__gh_stream");
                    const double k = json::Document::num(mk, -1);
                    if (k >= 0 && size_t(k) + 1 < sn->rangeStart.size()) {
                        for (uint32_t q = sn->rangeStart[size_t(k)]; q < sn->rangeStart[size_t(k) + 1]; ++q) {
                            const StreamedNote& r = sn->notes[q];
                            addNote(r.time, r.noteData(), r.length, must, stepMs, altSec, gfSec, {}, false, r.alt(), r.type,
                                    r.stack);
                        }
                    }
                    continue;
                }
                if (nv.type != Type::Array || doc.size(nv) < 2) continue;
                double t = json::Document::num(&doc.at(nv, 0), std::numeric_limits<double>::quiet_NaN());
                double d = json::Document::num(&doc.at(nv, 1), -1);
                if (!std::isfinite(t) || !(d >= 0)) continue;  // d < 0: legacy event note
                double l = doc.size(nv) > 2 ? json::Document::num(&doc.at(nv, 2), 0) : 0;
                std::string_view tn;
                bool hasTn = false, altB = false;
                if (doc.size(nv) > 3) {
                    const json::Value& tv = doc.at(nv, 3);
                    if (tv.type == Type::String) { tn = doc.str(tv); hasTn = true; }
                    else if (tv.type == Type::Bool && tv.b) altB = true;
                }
                addNote(t, int(d), l, must, stepMs, altSec, gfSec, tn, hasTn, altB, 0);
            }
        }
        secStart += beats * 60000.0 / curBpm;
    }
    out.rawNoteCount = sn ? sn->rawCount : out.notes.size();
    out.finalize(bpm);
    return true;
}

static bool parseVSlice(const json::Document& doc, std::string_view metaText, const std::string& diff, Chart& out,
                        std::string& err) {
    using json::Type;
    const json::Value& root = doc.root();
    double bpm = 100.0;
    if (!metaText.empty()) {
        json::Document meta;
        std::string e;
        if (meta.parse(metaText, &e)) {
            const json::Value& m = meta.root();
            out.title = std::string(meta.strOr(meta.get(m, "songName"), ""));
            if (const json::Value* tc = meta.get(m, "timeChanges"); tc && tc->type == Type::Array) {
                for (size_t i = 0; i < meta.size(*tc); ++i) {
                    const json::Value& c = meta.at(*tc, i);
                    double t = json::Document::num(meta.get(c, "t"), 0);
                    double b = json::Document::num(meta.get(c, "bpm"), 0);
                    if (b > 0) {
                        if (i == 0) bpm = b;
                        out.tempo.push_back({std::max(0.0, t), b, 0});
                    }
                }
            }
            if (const json::Value* pd = meta.get(m, "playData")) {
                if (const json::Value* ch = meta.get(*pd, "characters")) {
                    out.player = std::string(meta.strOr(meta.get(*ch, "player"), ""));
                    out.opponent = std::string(meta.strOr(meta.get(*ch, "opponent"), ""));
                    out.gf = std::string(meta.strOr(meta.get(*ch, "girlfriend"), ""));
                }
                out.stage = std::string(meta.strOr(meta.get(*pd, "stage"), ""));
            }
        }
    }
    const json::Value* notesObj = doc.get(root, "notes");
    if (!notesObj || notesObj->type != Type::Object || doc.size(*notesObj) == 0) {
        err = "V-Slice chart has no notes";
        return false;
    }
    const json::Value* arr = nullptr;
    for (size_t i = 0; i < doc.size(*notesObj); ++i)
        if (equalsI(doc.keyAt(*notesObj, i), diff)) arr = &doc.valAt(*notesObj, i);
    if (!arr) arr = &doc.valAt(*notesObj, 0);
    if (arr->type != Type::Array) { err = "V-Slice difficulty is not an array"; return false; }

    out.speed = 1.0;
    if (const json::Value* ss = doc.get(root, "scrollSpeed")) {
        if (ss->type == Type::Number) out.speed = ss->num;
        else if (ss->type == Type::Object) {
            double sp = -1;
            for (size_t i = 0; i < doc.size(*ss); ++i)
                if (equalsI(doc.keyAt(*ss, i), diff)) sp = json::Document::num(&doc.valAt(*ss, i), -1);
            if (sp <= 0) sp = json::Document::num(doc.get(*ss, "default"), -1);
            if (sp <= 0 && doc.size(*ss) > 0) sp = json::Document::num(&doc.valAt(*ss, 0), 1.0);
            if (sp > 0) out.speed = sp;
        }
    }
    out.speed = clampv(out.speed, 0.1, 20.0);
    out.keys = 4;
    out.notes.reserve(doc.size(*arr));
    for (size_t i = 0; i < doc.size(*arr); ++i) {
        const json::Value& nv = doc.at(*arr, i);
        if (nv.type != Type::Object) continue;
        double t = json::Document::num(doc.get(nv, "t"), std::numeric_limits<double>::quiet_NaN());
        double d = json::Document::num(doc.get(nv, "d"), -1);
        if (!std::isfinite(t) || !(d >= 0) || d >= 8) continue;
        Note n;
        n.time = t;
        double l = json::Document::num(doc.get(nv, "l"), 0);
        n.length = (std::isfinite(l) && l > 0) ? l : 0;
        n.lane = uint8_t(int(d) % 4);
        n.side = int(d) < 4 ? 1 : 0;
        if (const json::Value* k = doc.get(nv, "k"); k && k->type == Type::String) applyNoteType(doc.str(*k), n, out);
        out.notes.push_back(n);
    }
    if (const json::Value* ev = doc.get(root, "events"); ev && ev->type == Type::Array) {
        for (size_t i = 0; i < doc.size(*ev); ++i) {
            const json::Value& e = doc.at(*ev, i);
            if (doc.strOr(doc.get(e, "e"), "") != "FocusCamera") continue;
            const json::Value* v = doc.get(e, "v");
            int ch = 0;
            if (v && v->type == Type::Object) ch = int(json::Document::num(doc.get(*v, "char"), 0));
            else ch = int(json::Document::num(v, 0));
            out.focus.push_back({json::Document::num(doc.get(e, "t"), 0), uint8_t(ch == 0 ? 1 : 0)});
        }
    }
    // Drop sustains shorter than a step, like the original game.
    if (out.tempo.empty()) out.tempo.push_back({0, bpm, 0});
    out.rawNoteCount = out.notes.size();
    out.finalize(bpm);
    for (Note& n : out.notes)
        if (n.length > 0 && n.length < out.stepMs(n.time) * 0.999) n.length = 0;
    return true;
}

// ------------------------------------------------------------ PulseForge ---
// "format": "pulseforge-chart" (exported charts) or "pulseforge-editor-chart"
// (the chart editor's projects and autosaves): notes with their time in ms,
// lane and owner, a list of tempos, and the scene in "song" or "metadata".

static bool isPulseForge(const json::Document& doc) {
    const std::string_view f = doc.strOr(doc.get(doc.root(), "format"), "");
    return f == "pulseforge-chart" || f == "pulseforge-editor-chart";
}

static std::string valueText(const json::Document& d, const json::Value* v) {
    if (!v) return "";
    if (v->type == json::Type::String) return std::string(d.str(*v));
    if (v->type == json::Type::Bool) return v->b ? "true" : "false";
    if (v->type == json::Type::Number) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.10g", v->num);
        return buf;
    }
    return "";
}

static int pulseForgeOwner(std::string_view o) {
    if (o == "opponent" || o == "dad" || o == "enemy") return 1;
    if (o == "girlfriend" || o == "gf") return 2;
    return 0;
}

// Events as PulseForge reads them: {"time"/"t", "name", "value1", "value2"}
// or "values", grouped {"time", "events": [[name, v1, v2]...]}, or Psych's
// [time, [[name, v1, v2]...]].
static void readPulseForgeEvents(const json::Document& d, const json::Value& arr, std::vector<psych::ChartEvent>& out) {
    using json::Type;
    auto triples = [&](double t, const json::Value& list) {
        for (size_t j = 0; j < d.size(list); ++j) {
            const json::Value& x = d.at(list, j);
            if (x.type != Type::Array || d.size(x) == 0) continue;
            psych::ChartEvent ev;
            ev.timeMs = t;
            ev.name = valueText(d, &d.at(x, 0));
            if (d.size(x) > 1) ev.v1 = valueText(d, &d.at(x, 1));
            if (d.size(x) > 2) ev.v2 = valueText(d, &d.at(x, 2));
            if (!ev.name.empty()) out.push_back(std::move(ev));
        }
    };
    for (size_t i = 0; i < d.size(arr); ++i) {
        const json::Value& e = d.at(arr, i);
        if (e.type == Type::Array) {
            if (d.size(e) >= 2 && d.at(e, 1).type == Type::Array) triples(json::Document::num(&d.at(e, 0), 0), d.at(e, 1));
            continue;
        }
        if (e.type != Type::Object) continue;
        const double t = json::Document::num(d.get(e, "time"), json::Document::num(d.get(e, "t"), json::Document::num(d.get(e, "timeMs"), 0)));
        if (const json::Value* list = d.get(e, "events"); list && list->type == Type::Array) {
            triples(t, *list);
            continue;
        }
        psych::ChartEvent ev;
        ev.timeMs = t;
        ev.name = valueText(d, d.get(e, "name"));
        if (ev.name.empty()) ev.name = valueText(d, d.get(e, "event"));
        if (ev.name.empty()) ev.name = valueText(d, d.get(e, "e"));
        ev.v1 = valueText(d, d.get(e, "value1"));
        ev.v2 = valueText(d, d.get(e, "value2"));
        const json::Value* vals = d.get(e, "values");
        if (!vals) vals = d.get(e, "v");
        if (vals && vals->type == Type::Array) {
            if (d.size(*vals) > 0) ev.v1 = valueText(d, &d.at(*vals, 0));
            if (d.size(*vals) > 1) ev.v2 = valueText(d, &d.at(*vals, 1));
        } else if (vals && ev.v1.empty()) {
            ev.v1 = valueText(d, vals);
        }
        if (!ev.name.empty()) out.push_back(std::move(ev));
    }
    std::stable_sort(out.begin(), out.end(), [](const psych::ChartEvent& a, const psych::ChartEvent& b) { return a.timeMs < b.timeMs; });
}

static bool parsePulseForge(const json::Document& doc, Chart& out, std::string& err, const StreamedNotes* sn, bool extraKeys) {
    using json::Type;
    const json::Value& root = doc.root();
    const json::Value* meta = doc.get(root, "metadata");
    if (!meta || meta->type != Type::Object) meta = doc.get(root, "song");
    const json::Value& m = meta && meta->type == Type::Object ? *meta : root;
    out.title = std::string(doc.strOr(doc.get(m, "title"), ""));
    out.player = std::string(doc.strOr(doc.get(m, "player"), ""));
    out.opponent = std::string(doc.strOr(doc.get(m, "opponent"), ""));
    out.gf = std::string(doc.strOr(doc.get(m, "girlfriend"), ""));
    out.stage = std::string(doc.strOr(doc.get(m, "stage"), ""));
    const int keys = int(json::Document::num(doc.get(root, "keyCount"), json::Document::num(doc.get(m, "keyCount"), 4)));
    out.keys = extraKeys && keys >= 1 && keys <= 9 ? keys : 4;
    const double speed = json::Document::num(doc.get(root, "scrollSpeed"), json::Document::num(doc.get(m, "scrollSpeed"), 1.0));
    out.speed = clampv(speed > 0 && std::isfinite(speed) ? speed : 1.0, 0.1, 20.0);
    double bpm = 0;
    if (const json::Value* tv = doc.get(root, "tempos"); tv && tv->type == Type::Array)
        for (size_t i = 0; i < doc.size(*tv); ++i) {
            const json::Value& t = doc.at(*tv, i);
            const double b = json::Document::num(doc.get(t, "bpm"), 0), at = json::Document::num(doc.get(t, "time"), 0);
            if (!(b > 0) || !std::isfinite(b) || !std::isfinite(at)) continue;
            if (out.tempo.empty()) bpm = b;
            out.tempo.push_back({std::max(0.0, at), b, 0});
        }
    if (!(bpm > 0)) bpm = 120;

    const json::Value* notes = doc.get(root, "notes");
    if (!notes || notes->type != Type::Array) { err = "PulseForge chart has no notes"; return false; }
    std::vector<uint16_t> streamTypes{0};  // StreamedNotes type index -> chart type index
    if (sn)
        for (const std::string& t : sn->types) {
            Note probe;
            applyNoteType(t, probe, out);
            streamTypes.push_back(probe.type);
        }
    auto add = [&](double t, double len, int lane, int owner, uint16_t stack) -> Note* {
        if (!std::isfinite(t) || lane < 0) return nullptr;
        Note n;
        n.time = t;
        n.length = std::isfinite(len) && len > 0 ? len : 0;
        n.lane = uint8_t(lane % out.keys);
        n.side = owner == 0 ? 1 : 0;
        n.stack = std::max<uint16_t>(1, stack);
        if (owner == 2) n.flags |= kFlagGF;
        out.notes.push_back(n);
        return &out.notes.back();
    };
    uint64_t raw = 0;
    for (size_t i = 0; i < doc.size(*notes); ++i) {
        const json::Value& nv = doc.at(*notes, i);
        if (nv.type != Type::Object) continue;
        if (sn) {
            const double k = json::Document::num(doc.get(nv, "__gh_stream"), -1);
            if (k >= 0 && size_t(k) + 1 < sn->rangeStart.size()) {
                for (uint32_t q = sn->rangeStart[size_t(k)]; q < sn->rangeStart[size_t(k) + 1]; ++q) {
                    const StreamedNote& r = sn->notes[q];
                    if (Note* n = add(r.time, r.length, r.noteData() % 20, r.noteData() / 20, r.stack); n && r.type) {
                        const uint16_t ct = r.type < streamTypes.size() ? streamTypes[r.type] : 0;
                        const std::string typeName = ct ? out.noteTypes[ct] : std::string();
                        if (ct) applyNoteType(typeName, *n, out);
                    }
                }
                continue;
            }
        }
        ++raw;
        const std::string_view kind = doc.strOr(doc.get(nv, "kind"), "");
        Note* n = add(json::Document::num(doc.get(nv, "time"), std::numeric_limits<double>::quiet_NaN()),
                      json::Document::num(doc.get(nv, "duration"), 0), int(json::Document::num(doc.get(nv, "lane"), -1)),
                      pulseForgeOwner(doc.strOr(doc.get(nv, "owner"), "player")), 1);
        if (n && !kind.empty() && kind != "normal") applyNoteType(kind, *n, out);
    }
    out.rawNoteCount = sn ? sn->rawCount : raw;
    if (const json::Value* ev = doc.get(root, "events"); ev && ev->type == Type::Array) readPulseForgeEvents(doc, *ev, out.events);
    out.finalize(bpm);
    // Sustains shorter than a step are taps, as in the other formats.
    for (Note& n : out.notes)
        if (n.length > 0 && n.length < out.stepMs(n.time) * 0.999) n.length = 0;
    return true;
}

bool parseChart(std::string_view text, const std::string& difficulty, Chart& out, std::string& err,
                std::string_view vsliceMetadata, bool extraKeys) {
    out = Chart();
    out.difficulty = difficulty;
    json::Document doc;
    std::string e;
    if (!doc.parse(text, &e)) { err = "invalid JSON: " + e; return false; }
    if (isPulseForge(doc)) return parsePulseForge(doc, out, err, nullptr, extraKeys);
    const json::Value* notes = doc.get(doc.root(), "notes");
    const bool ok = (!vsliceMetadata.empty() || (notes && notes->type == json::Type::Object))
                        ? parseVSlice(doc, vsliceMetadata, difficulty, out, err)
                        : parsePsych(doc, out, err, nullptr, extraKeys);
    if (ok) psych::parseEvents(doc, out.events);
    return ok;
}

bool parseChartStreamed(std::string_view skeleton, const StreamedNotes& notes, const std::string& difficulty, Chart& out,
                        std::string& err, bool extraKeys) {
    out = Chart();
    out.difficulty = difficulty;
    json::Document doc;
    std::string e;
    if (!doc.parse(skeleton, &e)) { err = "invalid JSON: " + e; return false; }
    if (isPulseForge(doc)) return parsePulseForge(doc, out, err, &notes, extraKeys);
    const bool ok = parsePsych(doc, out, err, &notes, extraKeys);
    if (ok) psych::parseEvents(doc, out.events);
    return ok;
}

}  // namespace gh
