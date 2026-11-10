#include "chart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "chart_stream.hpp"
#include "common.hpp"
#include "json.hpp"

namespace gh {

bool readTextFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return false; }
    out.resize(size_t(sz));
    size_t rd = sz > 0 ? std::fread(out.data(), 1, size_t(sz), f) : 0;
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

// Key count of a legacy/Psych chart. Only an explicit count makes it an
// extra-key chart: Psych and JS Engine play every other chart with 4 keys
// (lane = noteData % 4), whatever values its notes use.
static int keysFromMetadata(const json::Document& doc, const json::Value& s, const std::array<uint64_t, 64>& dataCount) {
    int keys = int(json::Document::num(doc.get(s, "keyCount"), 0));  // Leather Engine
    if (keys >= 1 && keys <= 9) return keys;
    uint64_t total = 0;
    for (uint64_t c : dataCount) total += c;
    auto usedFrom = [&](int d) {
        uint64_t n = 0;
        for (size_t i = size_t(std::max(0, d)); i < dataCount.size(); ++i) n += dataCount[i];
        return n;
    };
    // "mania": Dave & Bambi / Kade multi-key forks (in "options"): 0=4K 1=6K 2=7K 3=9K.
    // Psych Extra Keys puts it on the song with keys = mania + 1.
    static const int dnb[] = {4, 6, 7, 9};
    const json::Value* opts = doc.get(s, "options");
    const double om = opts ? json::Document::num(doc.get(*opts, "mania"), -1) : -1;
    const double sm = json::Document::num(doc.get(s, "mania"), -1);
    if (om >= 0 && om <= 3) return dnb[int(om)];
    if (sm >= 0 && sm <= 8) {
        const int ek = int(sm) + 1;
        // Same number, two meanings: the notes tell them apart. A few stray
        // values past Extra Keys' lanes do not make it a Kade chart.
        if (sm <= 3 && usedFrom(2 * ek) * 100 > total && usedFrom(2 * ek) > 8) return dnb[int(sm)];
        return ek;
    }
    return 4;
}

static bool parsePsych(const json::Document& doc, Chart& out, std::string& err, const StreamedNotes* sn = nullptr) {
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
    if (!sections || sections->type != Type::Array) { err = "chart has no 'notes' section array"; return false; }

    // Pass 1: how often each noteData value is used.
    std::array<uint64_t, 64> dataCount{};
    if (sn) dataCount = sn->dataCount;
    for (size_t i = 0; i < doc.size(*sections); ++i) {
        const json::Value* sv = doc.get(doc.at(*sections, i), "sectionNotes");
        if (!sv || sv->type != Type::Array) continue;
        for (size_t j = 0; j < doc.size(*sv); ++j) {
            const json::Value& n = doc.at(*sv, j);
            if (n.type != Type::Array || doc.size(n) < 2) continue;
            double d = json::Document::num(&doc.at(n, 1), -1);
            if (d >= 0 && d < 64) ++dataCount[size_t(d)];
        }
    }
    out.keys = keysFromMetadata(doc, s, dataCount);
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
    for (size_t i = 0; i < doc.size(*sections); ++i) {
        const json::Value& sec = doc.at(*sections, i);
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
        out.sections.push_back({secStart, must, gfSec, altSec});
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

bool parseChart(std::string_view text, const std::string& difficulty, Chart& out, std::string& err,
                std::string_view vsliceMetadata) {
    out = Chart();
    out.difficulty = difficulty;
    json::Document doc;
    std::string e;
    if (!doc.parse(text, &e)) { err = "invalid JSON: " + e; return false; }
    const json::Value* notes = doc.get(doc.root(), "notes");
    const bool ok = (!vsliceMetadata.empty() || (notes && notes->type == json::Type::Object))
                        ? parseVSlice(doc, vsliceMetadata, difficulty, out, err)
                        : parsePsych(doc, out, err);
    if (ok) psych::parseEvents(doc, out.events);
    return ok;
}

bool parseChartStreamed(std::string_view skeleton, const StreamedNotes& notes, const std::string& difficulty, Chart& out,
                        std::string& err) {
    out = Chart();
    out.difficulty = difficulty;
    json::Document doc;
    std::string e;
    if (!doc.parse(skeleton, &e)) { err = "invalid JSON: " + e; return false; }
    const bool ok = parsePsych(doc, out, err, &notes);
    if (ok) psych::parseEvents(doc, out.events);
    return ok;
}

}  // namespace gh
