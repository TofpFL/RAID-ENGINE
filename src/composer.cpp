#include "composer.hpp"

#include <algorithm>
#include <cmath>
#include <future>

#include "common.hpp"
#include "synth.hpp"

namespace gh {

using namespace synth;

// ------------------------------------------------------------- song list --

static std::vector<SongDef> makeSongs() {
    using P = PartKind;
    std::vector<SongDef> v;
    {
        SongDef s;
        s.id = "vigil"; s.title = "Vigil"; s.opponent = "principality";
        s.blurb = "A gentle first hymn. Learn to answer the heavens.";
        s.bpm = 96; s.root = 50; s.scale = {0, 2, 4, 5, 7, 9, 11}; s.seed = 0x5161u; s.style = 0;
        s.progressions = {{0, 4, 5, 3}, {0, 3, 4, 4}, {5, 3, 0, 4}};
        s.parts = {{P::Intro, 4, 0}, {P::Call, 4, 1}, {P::Answer, 4, 1}, {P::Call, 4, 1}, {P::Answer, 4, 1},
                   {P::Break, 2, 0}, {P::Call, 4, 2}, {P::Answer, 4, 2}, {P::Duet, 4, 2}, {P::Outro, 4, 0}};
        s.diffs = {"Easy", "Normal", "Hard"};
        s.color = 0xF4C76B; s.stage = 0;
        v.push_back(s);
    }
    {
        SongDef s;
        s.id = "principality"; s.title = "Principality"; s.opponent = "principality";
        s.blurb = "The angel of the first gate tests your voice.";
        s.bpm = 118; s.root = 52; s.scale = {0, 2, 4, 6, 7, 9, 11}; s.seed = 0xA11E1u; s.style = 1;
        s.progressions = {{0, 1, 5, 4}, {0, 4, 5, 1}, {3, 4, 0, 0}};
        s.parts = {{P::Intro, 8, 1}, {P::Call, 8, 1}, {P::Answer, 8, 1}, {P::Call, 8, 2}, {P::Answer, 8, 2},
                   {P::Break, 4, 0}, {P::Call, 8, 3}, {P::Answer, 8, 3}, {P::Duet, 8, 3}, {P::Outro, 4, 1}};
        s.diffs = {"Easy", "Normal", "Hard"};
        s.color = 0xFFE6A8; s.stage = 0;
        v.push_back(s);
    }
    {
        SongDef s;
        s.id = "godshand"; s.title = "Godshand"; s.opponent = "hand";
        s.blurb = "The Hand that wrote the world keeps time.";
        s.bpm = 146; s.root = 50; s.scale = {0, 2, 3, 5, 7, 9, 10}; s.seed = 0x60D5u; s.style = 2;
        s.progressions = {{0, 6, 5, 6}, {0, 3, 6, 4}, {0, 5, 3, 4}};
        s.parts = {{P::Intro, 4, 1}, {P::Call, 8, 2}, {P::Answer, 8, 2}, {P::Duet, 8, 2}, {P::Break, 4, 1},
                   {P::Call, 8, 3}, {P::Answer, 8, 3}, {P::Call, 8, 3}, {P::Answer, 8, 3}, {P::Outro, 4, 1}};
        s.diffs = {"Easy", "Normal", "Hard", "Infernal"};
        s.color = 0x7FB8FF; s.stage = 1;
        v.push_back(s);
    }
    {
        SongDef s;
        s.id = "demise"; s.title = "Demise"; s.opponent = "demise";
        s.blurb = "The end of all principalities. Sing, or be unmade.";
        s.bpm = 176; s.root = 49; s.scale = {0, 1, 4, 5, 7, 8, 10}; s.seed = 0xDE315Eu; s.style = 3;
        s.progressions = {{0, 1, 0, 6}, {0, 5, 1, 0}, {0, 3, 1, 0}};
        s.oppOctave = -1;
        s.parts = {{P::Intro, 8, 1}, {P::Call, 8, 2}, {P::Answer, 8, 2}, {P::Call, 8, 3}, {P::Answer, 8, 3},
                   {P::Break, 4, 1}, {P::Duet, 16, 3}, {P::Call, 8, 3}, {P::Answer, 8, 3}, {P::Outro, 4, 1}};
        s.diffs = {"Easy", "Normal", "Hard", "Infernal"};
        s.color = 0xFF3B3B; s.stage = 2;
        v.push_back(s);
    }
    return v;
}

const std::vector<SongDef>& builtinSongs() {
    static const std::vector<SongDef> songs = makeSongs();
    return songs;
}

const SongDef* findBuiltin(const std::string& id) {
    for (const SongDef& s : builtinSongs())
        if (s.id == id) return &s;
    return nullptr;
}

const SongDef& menuTheme() {
    static const SongDef def = [] {
        SongDef s;
        s.id = "hymn"; s.title = "Hymn of the Gate";
        s.bpm = 100; s.root = 50; s.scale = {0, 2, 4, 6, 7, 9, 11}; s.seed = 0x4E11u; s.style = 1;
        s.progressions = {{0, 1, 5, 4}};
        s.parts = {{PartKind::Intro, 4, 0}, {PartKind::Call, 8, 2}, {PartKind::Break, 4, 1}};
        s.vocals = false;
        return s;
    }();
    return def;
}

int degToMidi(const SongDef& def, int deg) {
    const int n = int(def.scale.size());
    int oct = deg >= 0 ? deg / n : -((-deg + n - 1) / n);
    int d = deg - oct * n;
    return def.root + 12 * oct + def.scale[size_t(d)];
}

// ---------------------------------------------------------- composition --

namespace {

struct BarInfo {
    PartKind kind;
    int part = 0, intensity = 0, barInPart = 0, chord = 0, singer = -1, partBars = 0;
};

int mod7(int v) { return ((v % 7) + 7) % 7; }

bool isChordTone(int deg, int chord) {
    const int r = mod7(deg - chord);
    return r == 0 || r == 2 || r == 4;
}

int nearestChordTone(int deg, int chord) {
    for (int d = 0; d <= 3; ++d) {
        if (isChordTone(deg - d, chord)) return deg - d;
        if (isChordTone(deg + d, chord)) return deg + d;
    }
    return deg;
}

int reflectInto(int deg, int lo, int hi) {
    while (deg < lo || deg > hi) {
        if (deg < lo) deg = lo + (lo - deg);
        if (deg > hi) deg = hi - (deg - hi);
    }
    return deg;
}

struct Step { int step, len; };

std::vector<Step> genRhythm(Rng& rng, int steps, int intensity) {
    std::vector<Step> r;
    int pos = 0;
    while (pos < steps) {
        double x = rng.uniform();
        int dur;
        if (intensity <= 1) dur = x < 0.25 ? 2 : x < 0.75 ? 4 : x < 0.85 ? 6 : 8;
        else if (intensity == 2) dur = x < 0.06 ? 1 : x < 0.56 ? 2 : x < 0.68 ? 3 : x < 0.95 ? 4 : 6;
        else dur = x < 0.36 ? 1 : x < 0.8 ? 2 : x < 0.88 ? 3 : 4;
        const double restP = intensity >= 3 ? 0.06 : 0.12;
        if (pos > 0 && rng.chance(restP)) { pos += 2; continue; }
        if (dur == 1) {
            if (pos + 2 <= steps) { r.push_back({pos, 1}); r.push_back({pos + 1, 1}); pos += 2; }
            else pos = steps;
            continue;
        }
        dur = std::min(dur, steps - pos);
        r.push_back({pos, dur});
        pos += dur;
    }
    if (!r.empty()) r.back().len = std::max(r.back().len, std::min(8, steps - r.back().step));
    return r;
}

// One motif spanning `nBars` bars starting at `bar0`; beats relative to bar0.
std::vector<MelNote> genMotif(Rng& rng, const std::vector<BarInfo>& bars, int bar0, int nBars, int intensity, int lo,
                              int hi, int startDeg) {
    std::vector<MelNote> out;
    const auto rhythm = genRhythm(rng, nBars * 16, intensity);
    int deg = startDeg;
    bool first = true;
    for (const Step& s : rhythm) {
        const int bar = std::min(int(bars.size()) - 1, bar0 + s.step / 16);
        const int chord = bars[size_t(bar)].chord;
        if (first) {
            deg = nearestChordTone(deg, chord);
            first = false;
        } else {
            const double x = rng.uniform();
            const int move = x < 0.08 ? -3 : x < 0.22 ? -2 : x < 0.44 ? -1 : x < 0.56 ? 0 : x < 0.78 ? 1 : x < 0.92 ? 2 : 3;
            deg += move;
        }
        if (s.step % 8 == 0) deg = nearestChordTone(deg, chord);
        deg = reflectInto(deg, lo, hi);
        MelNote m;
        m.beat = s.step / 4.0;
        m.len = s.len / 4.0;
        m.deg = deg;
        m.intensity = intensity;
        out.push_back(m);
    }
    return out;
}

// Copy a motif to another position, adapting pitches to the target chords.
std::vector<MelNote> adaptMotif(const std::vector<MelNote>& motif, const std::vector<BarInfo>& bars, int srcBar0,
                                int dstBar0, int lo, int hi) {
    std::vector<MelNote> out;
    for (MelNote m : motif) {
        const int rel = int(m.beat / 4.0);
        const int sb = std::min(int(bars.size()) - 1, srcBar0 + rel);
        const int db = std::min(int(bars.size()) - 1, dstBar0 + rel);
        m.deg += bars[size_t(db)].chord - bars[size_t(sb)].chord;
        const int stepInBar = int(std::lround((m.beat - rel * 4.0) * 4.0));
        if (stepInBar % 8 == 0) m.deg = nearestChordTone(m.deg, bars[size_t(db)].chord);
        m.deg = reflectInto(m.deg, lo, hi);
        out.push_back(m);
    }
    return out;
}

void cadence(std::vector<MelNote>& notes, const std::vector<BarInfo>& bars, int lastBar, Rng& rng) {
    if (notes.empty()) return;
    MelNote& last = notes.back();
    const int chord = bars[size_t(std::min(int(bars.size()) - 1, lastBar))].chord;
    const int target = rng.chance(0.6) ? chord : chord + 2;
    int best = last.deg;
    for (int d = -3; d <= 3; ++d)
        if (mod7(last.deg + d - target) == 0) { best = last.deg + d; break; }
    last.deg = best;
    last.len = std::max(last.len, 1.0);
}

}  // namespace

static VoiceStyle styleFor(const std::string& who) {
    VoiceStyle v;
    if (who == "herald") {
        v.vibHz = 5.6f; v.vibDepth = 0.22f; v.formantShift = 1.12f; v.consonant = 0.4f; v.gain = 0.55f;
        v.send = 0.25f; v.breath = 0.02f;
        v.vowels[0] = 0; v.vowels[1] = 1; v.vowels[2] = 3; v.vowels[3] = 0;
    } else if (who == "principality") {
        v.vibHz = 5.0f; v.vibDepth = 0.18f; v.formantShift = 1.05f; v.unison = 3; v.detuneCents = 12;
        v.upOctave = 0.35f; v.breath = 0.04f; v.consonant = 0.15f; v.gain = 0.5f; v.send = 0.55f;
        v.vowels[0] = 0; v.vowels[1] = 3; v.vowels[2] = 0; v.vowels[3] = 4;
    } else if (who == "hand") {
        v.vibHz = 0; v.vibDepth = 0; v.pulse = 0.7f; v.consonant = 0.55f; v.drive = 1.5f; v.gain = 0.45f;
        v.send = 0.3f;
        v.vowels[0] = 1; v.vowels[1] = 2; v.vowels[2] = 1; v.vowels[3] = 0;
    } else {  // demise
        v.vibHz = 4.5f; v.vibDepth = 0.3f; v.formantShift = 0.82f; v.drive = 3.0f; v.subOctave = 0.45f;
        v.unison = 2; v.detuneCents = 8; v.consonant = 0.45f; v.gain = 0.42f; v.send = 0.35f;
        v.vowels[0] = 3; v.vowels[1] = 4; v.vowels[2] = 0; v.vowels[3] = 3;
    }
    return v;
}

std::shared_ptr<Composition> compose(const SongDef& def, bool renderAudio) {
    auto comp = std::make_shared<Composition>();
    Rng rng(def.seed);
    comp->bpm = def.bpm;
    const double beatSec = 60.0 / def.bpm;

    // ---- bar timeline
    std::vector<BarInfo> bars;
    for (size_t p = 0; p < def.parts.size(); ++p) {
        const Part& part = def.parts[p];
        const auto& prog = def.progressions[size_t(rng.next() % def.progressions.size())];
        for (int b = 0; b < part.bars; ++b) {
            BarInfo bi;
            bi.kind = part.kind;
            bi.part = int(p);
            bi.intensity = part.intensity;
            bi.barInPart = b;
            bi.partBars = part.bars;
            bi.chord = prog[size_t(b) % prog.size()];
            if (part.kind == PartKind::Call) bi.singer = 0;
            else if (part.kind == PartKind::Answer) bi.singer = 1;
            else if (part.kind == PartKind::Duet) bi.singer = b % 2;
            bars.push_back(bi);
        }
    }
    const int totalBars = int(bars.size());
    const double loopSec = totalBars * 4 * beatSec;
    comp->loopSec = loopSec;
    comp->lengthSec = loopSec + 3.0;

    // ---- camera focus
    int lastFocus = -1;
    for (int b = 0; b < totalBars; ++b) {
        int f;
        switch (bars[size_t(b)].kind) {
            case PartKind::Call: f = 0; break;
            case PartKind::Answer: f = 1; break;
            case PartKind::Duet: f = bars[size_t(b)].singer; break;
            case PartKind::Intro: f = 0; break;
            default: f = 1; break;
        }
        if (f != lastFocus) { comp->focus.push_back({b * 4.0, f}); lastFocus = f; }
    }

    // ---- melody
    const int lo = 5, hi = 14;
    std::vector<MelNote> lastCall;  // relative to the call part start
    int lastCallBar0 = 0, lastCallBars = 0;
    int bar0 = 0;
    for (size_t p = 0; p < def.parts.size(); ++p) {
        const Part& part = def.parts[p];
        const double base = bar0 * 4.0;
        if (part.kind == PartKind::Call) {
            std::vector<MelNote> rel;
            std::vector<MelNote> motifA = genMotif(rng, bars, bar0, 2, part.intensity, lo, hi, 9);
            std::vector<MelNote> motifB = genMotif(rng, bars, bar0, 2, part.intensity, lo, hi, 11);
            for (int ph = 0; ph * 4 < part.bars; ++ph) {
                const int pb = bar0 + ph * 4;
                const std::vector<MelNote>& m = (ph % 4 == 2) ? motifB : motifA;
                std::vector<MelNote> first = adaptMotif(m, bars, bar0, pb, lo, hi);
                std::vector<MelNote> second = adaptMotif(m, bars, bar0, pb + 2, lo, hi);
                if (ph % 2 == 1) {  // vary the tail of the answer half
                    std::vector<MelNote> tail = genMotif(rng, bars, pb + 3, 1, part.intensity, lo, hi,
                                                         second.empty() ? 9 : second.front().deg);
                    second.erase(std::remove_if(second.begin(), second.end(), [](const MelNote& n) { return n.beat >= 4.0; }),
                                 second.end());
                    for (MelNote t : tail) { t.beat += 4.0; second.push_back(t); }
                }
                cadence(second, bars, pb + 3, rng);
                bool firstNote = true;
                for (MelNote n : first) {
                    n.beat += ph * 16.0;
                    n.phraseStart = firstNote;
                    firstNote = false;
                    rel.push_back(n);
                }
                for (MelNote n : second) { n.beat += ph * 16.0 + 8.0; rel.push_back(n); }
            }
            for (MelNote n : rel) {
                n.side = 0;
                n.intensity = part.intensity;
                n.beat += base;
                comp->melody.push_back(n);
            }
            lastCall = rel;
            lastCallBar0 = bar0;
            lastCallBars = part.bars;
        } else if (part.kind == PartKind::Answer) {
            std::vector<MelNote> rel = lastCall;
            if (rel.empty()) rel = genMotif(rng, bars, bar0, part.bars, part.intensity, lo, hi, 9);
            const double partBeats = part.bars * 4.0;
            const double srcBeats = std::max(4.0, lastCallBars * 4.0);
            for (double off = 0; off < partBeats; off += srcBeats) {
                for (MelNote n : rel) {
                    if (n.beat + off >= partBeats) continue;
                    const int srcBar = lastCallBar0 + int(n.beat / 4.0);
                    const int dstBar = bar0 + int((n.beat + off) / 4.0);
                    n.deg += bars[size_t(std::min(totalBars - 1, dstBar))].chord -
                             bars[size_t(std::min(totalBars - 1, srcBar))].chord;
                    n.deg = reflectInto(n.deg, lo, hi);
                    n.side = 1;
                    n.intensity = part.intensity;
                    n.beat += base + off;
                    comp->melody.push_back(n);
                }
            }
        } else if (part.kind == PartKind::Duet) {
            for (int b = 0; b + 1 < part.bars; b += 2) {
                std::vector<MelNote> m = genMotif(rng, bars, bar0 + b, 1, part.intensity, lo, hi, 9 + (b % 4));
                cadence(m, bars, bar0 + b, rng);
                std::vector<MelNote> echo = adaptMotif(m, bars, bar0 + b, bar0 + b + 1, lo, hi);
                bool first = true;
                for (MelNote n : m) {
                    n.side = 0; n.phraseStart = first; first = false;
                    n.intensity = part.intensity;
                    n.beat += base + b * 4.0;
                    comp->melody.push_back(n);
                }
                first = true;
                for (MelNote n : echo) {
                    n.side = 1; n.phraseStart = first; first = false;
                    n.intensity = part.intensity;
                    n.beat += base + (b + 1) * 4.0;
                    comp->melody.push_back(n);
                }
            }
        }
        bar0 += part.bars;
    }
    if (!def.vocals) comp->melody.clear();
    std::stable_sort(comp->melody.begin(), comp->melody.end(), [](const MelNote& a, const MelNote& b) {
        return a.beat < b.beat || (a.beat == b.beat && a.side < b.side);
    });

    // ---- arpeggio (pluck in the instrumental; Infernal streams follow it)
    static const int arpPatterns[4][8] = {
        {0, 1, 2, 3, 2, 1, 0, 1}, {0, 2, 1, 3, 0, 2, 1, 3}, {3, 2, 1, 0, 1, 2, 3, 2}, {0, 1, 2, 1, 3, 2, 1, 2}};
    int arpPat = 0;
    for (int b = 0; b < totalBars; ++b) {
        const BarInfo& bi = bars[size_t(b)];
        if (bi.barInPart == 0) arpPat = int(rng.next() % 4);
        if (bi.intensity < 2 || bi.kind == PartKind::Break || bi.kind == PartKind::Outro) continue;
        const int tones[4] = {bi.chord, bi.chord + 2, bi.chord + 4, bi.chord + 7};
        for (int s = 0; s < 16; ++s) {
            MelNote a;
            a.beat = b * 4.0 + s * 0.25;
            a.len = 0.25;
            a.deg = 14 + tones[arpPatterns[arpPat][s % 8]];
            a.side = uint8_t(bi.singer < 0 ? 0 : bi.singer);
            a.intensity = bi.intensity;
            a.phraseStart = bi.singer < 0;  // marks "nobody sings" bars
            comp->arp.push_back(a);
        }
    }

    if (!renderAudio) return comp;

    // ---- audio rendering
    const size_t total = size_t((comp->lengthSec + 0.5) * SR);
    const int style = def.style;
    const SongDef* d = &def;
    const std::vector<BarInfo>* barsP = &bars;
    Composition* c = comp.get();

    auto renderInst = [=]() {
        Stereo out(total);
        Mono send(total, 0.0f);
        const std::vector<BarInfo>& B = *barsP;
        for (int b = 0; b < totalBars; ++b) {
            const BarInfo& bi = B[size_t(b)];
            const double t0 = b * 4 * beatSec;
            const bool last = bi.barInPart == bi.partBars - 1;
            const bool hasNext = size_t(bi.part + 1) < d->parts.size();
            const int nextInt = hasNext ? d->parts[size_t(bi.part + 1)].intensity : -1;
            const int I = bi.intensity;
            // pad: chord, voiced around the root octave
            for (int k = 0; k < 3; ++k) {
                const int deg = bi.chord + 2 * k + (k == 0 ? 0 : 0);
                padNote(out, &send, t0, 4 * beatSec, degToMidi(*d, deg), 0.9f, style, k == 0 ? 0.0f : (k == 1 ? -0.5f : 0.5f));
            }
            padNote(out, &send, t0, 4 * beatSec, degToMidi(*d, bi.chord - 7), 0.7f, style, 0.0f);
            // bells in calm sections
            if ((style <= 1 && (bi.kind == PartKind::Intro || bi.kind == PartKind::Break || I <= 1)) ||
                (style == 3 && bi.kind == PartKind::Break)) {
                for (int q = 0; q < 4; ++q) {
                    const int deg = 14 + bi.chord + (q % 3) * 2;
                    bell(out, &send, t0 + q * beatSec, degToMidi(*d, deg) + (style == 3 ? -12 : 0), 0.55f, q % 2 ? 0.4f : -0.4f);
                }
            }
            // bass
            const double bm = degToMidi(*d, bi.chord) - 12.0 - (style == 3 ? 12.0 : 0.0);
            if (bi.kind != PartKind::Break || I >= 1) {
                if (style == 0 || I == 0) {
                    bass(out, t0, 4 * beatSec * 0.95, bm, 0.8f, style);
                } else if (I == 1) {
                    bass(out, t0, 2 * beatSec * 0.9, bm, 0.8f, style);
                    bass(out, t0 + 2 * beatSec, 2 * beatSec * 0.9, bm, 0.75f, style);
                } else if (I == 2 || style == 1) {
                    for (int e = 0; e < 8; ++e)
                        bass(out, t0 + e * beatSec * 0.5, beatSec * 0.42, bm + ((e % 4 == 3) ? 12 : 0), e % 2 ? 0.7f : 0.85f, style);
                } else {
                    for (int s = 0; s < 16; ++s) {
                        if (style == 3 && (s % 4 == 1)) continue;
                        const double m = bm + ((style == 2 && s % 2 == 1) ? 12 : 0);
                        bass(out, t0 + s * beatSec * 0.25, beatSec * 0.2, m, s % 4 == 0 ? 0.9f : 0.7f, style);
                    }
                }
            }
            // drums
            if (I >= 1 && bi.kind != PartKind::Outro) {
                const bool four = style == 2 || (style == 3 && I >= 3);
                for (int s = 0; s < 16; ++s) {
                    const double t = t0 + s * beatSec * 0.25;
                    bool k = false, sn = false;
                    if (I == 1) k = (s == 0 || s == 8) && style != 0;
                    else if (four) k = s % 4 == 0 || (style == 3 && (s == 14 || s == 7));
                    else k = s == 0 || s == 8 || s == 10;
                    if (I >= 2) sn = s == 4 || s == 12;
                    if (k) kick(out, t, 0.95f, style >= 2 ? 1.0f : 0.5f);
                    if (sn) (style == 2 ? clap : snare)(out, &send, t, 0.8f);
                    if (sn && style == 2 && I >= 3) snare(out, &send, t, 0.35f);
                    // hats
                    if (I == 1 && s % 4 == 2) hat(out, t, 0.6f, false, 0.3f);
                    if (I == 2 && s % 2 == 0) hat(out, t, s % 4 == 2 ? 0.75f : 0.45f, s == 14 && b % 2 == 1, 0.3f);
                    if (I >= 3) hat(out, t, s % 4 == 2 ? 0.8f : (s % 2 ? 0.35f : 0.5f), s == 14, 0.25f);
                }
                if (style == 0 && I >= 1) kick(out, t0, 0.6f, 0.2f);
            }
            // fills, risers, crashes, impacts
            if (last && hasNext && I >= 1) {
                for (int s = 12; s < 16; ++s) {
                    const double t = t0 + s * beatSec * 0.25;
                    const float vel = 0.4f + 0.12f * float(s - 12);
                    if (style == 3) tom(out, &send, t, 140.0 - (s - 12) * 18.0, vel);
                    else snare(out, &send, t, vel * 0.8f);
                }
            }
            if (last && hasNext && nextInt > I) riser(out, t0, 4 * beatSec, 0.9f);
            if (bi.barInPart == 0 && I >= 2) crash(out, &send, t0, 0.8f);
            if (bi.barInPart == 0 && bi.kind == PartKind::Outro) crash(out, &send, t0, 0.7f);
            if (bi.barInPart == 0 && I >= 3 && style == 3) impact(out, &send, t0, 0.9f);
        }
        // final chord
        const double tEnd = totalBars * 4 * beatSec;
        if (d->vocals) {
            for (int k = 0; k < 3; ++k) padNote(out, &send, tEnd, 2.0, degToMidi(*d, 2 * k), 0.8f, style, 0.0f);
            bell(out, &send, tEnd, degToMidi(*d, 14), 0.7f, 0.0f);
        }
        // arpeggio with tempo-synced echoes
        for (const MelNote& a : c->arp) {
            const double t = a.beat * beatSec;
            const double m = degToMidi(*d, a.deg);
            const float pan = std::fmod(a.beat, 1.0) < 0.5 ? -0.25f : 0.25f;
            pluck(out, &send, t, beatSec * 0.25, m, 0.75f, pan, style);
            pluck(out, &send, t + beatSec * 0.75, beatSec * 0.25, m, 0.25f, -0.7f, style);
            pluck(out, &send, t + beatSec * 1.5, beatSec * 0.25, m, 0.1f, 0.7f, style);
        }
        reverb(send, out.l.data(), out.r.data(), out.size(), style == 3 ? 0.85f : 0.8f, 0.45f, 1.0f, 1.0f);
        if (!d->vocals && c->loopSec > 0) {
            // Fold the tail onto the start so the menu loop is seamless.
            const size_t L = size_t(c->loopSec * SR);
            for (size_t i = L; i < out.size(); ++i) {
                out.l[i - L] += out.l[i];
                out.r[i - L] += out.r[i];
            }
            out.l.resize(L);
            out.r.resize(L);
        }
        return toPcm16(out, 0.62f);
    };

    auto renderVoice = [=](int side) {
        Mono out(total, 0.0f);
        Mono send(total, 0.0f);
        const VoiceStyle vs = styleFor(side == 1 ? "herald" : d->opponent);
        const int octave = side == 1 ? 0 : d->oppOctave;
        double prevMidi = -1, prevEnd = -10;
        int idx = 0;
        for (const MelNote& m : c->melody) {
            if (m.side != side) continue;
            const double t = m.beat * beatSec;
            const double dur = std::max(0.06, m.len * beatSec - 0.03);
            const double midi = degToMidi(*d, m.deg) + 12.0 * octave;
            const int va = vs.vowels[idx % 4], vb = m.len >= 1.0 ? vs.vowels[(idx + 1) % 4] : va;
            voiceNote(out, &send, t, dur, midi, (t - prevEnd) < beatSec * 0.5 ? prevMidi : -1, va, vb, vs,
                      uint32_t(idx * 7919 + side));
            prevMidi = midi;
            prevEnd = t + dur;
            ++idx;
        }
        reverb(send, out.data(), out.data(), out.size(), 0.75f, 0.5f, 1.0f, 1.0f);
        return monoToPcm16(out, 0.55f);
    };

    if (def.vocals) {
        auto fInst = std::async(std::launch::async, renderInst);
        auto fVoices = std::async(std::launch::async, [&] {
            c->voiceO = renderVoice(0);
            c->voiceP = renderVoice(1);
        });
        c->inst = fInst.get();
        fVoices.get();
    } else {
        c->inst = renderInst();
    }
    return comp;
}

// ------------------------------------------------------------------ charts --

bool buildChart(const SongDef& def, const Composition& comp, const std::string& diff, Chart& out) {
    out = Chart();
    out.title = def.title;
    out.difficulty = diff;
    out.opponent = def.opponent;
    out.player = "herald";
    out.keys = 4;
    const std::string dl = toLower(diff);
    int level = 2;
    if (dl == "easy") level = 0;
    else if (dl == "normal") level = 1;
    else if (dl == "hard") level = 2;
    else if (dl == "infernal") level = 3;
    static const double speeds[4] = {1.6, 2.0, 2.4, 2.8};
    out.speed = speeds[level];
    const double beatMs = 60000.0 / def.bpm;
    auto lane = [](int deg) { return uint8_t(((deg % 4) + 4) % 4); };

    double lastKept[2] = {-100, -100};
    std::vector<std::vector<int>> melodySteps(2);  // 16th-note steps used by melody per side
    for (const MelNote& m : comp.melody) {
        const int step = int(std::lround(m.beat * 4.0));
        const int side = m.side;
        if (level == 0) {
            if (step % 4 != 0 && m.len < 1.0) continue;
            if (m.beat - lastKept[side] < 1.0) continue;
        } else if (level == 1) {
            if (step % 2 != 0) continue;
            if (m.beat - lastKept[side] < 0.5) continue;
        }
        lastKept[side] = m.beat;
        Note n;
        n.time = m.beat * beatMs;
        n.lane = lane(m.deg);
        n.side = uint8_t(side);
        const double minSus = level == 0 ? 1.0 : 0.75;
        n.length = m.len >= minSus ? (m.len - 0.25) * beatMs : 0.0;
        out.notes.push_back(n);
        melodySteps[size_t(side)].push_back(step);
        if (level >= 2 && m.phraseStart && m.intensity >= 2) {
            Note d2 = n;
            d2.lane = uint8_t((n.lane + 2) % 4);
            out.notes.push_back(d2);
        }
    }
    if (level >= 2) {
        // Hard adds quarter-note arpeggio hits in the loudest bars; Infernal
        // turns the whole arpeggio into 16th-note streams.
        for (auto& v : melodySteps) std::sort(v.begin(), v.end());
        for (const MelNote& a : comp.arp) {
            if (a.phraseStart) continue;
            const int step = int(std::lround(a.beat * 4.0));
            if (level == 2 && (a.intensity < 3 || step % 4 != 0)) continue;
            if (level == 3 && a.intensity < 2) continue;
            const auto& ms = melodySteps[a.side];
            if (std::binary_search(ms.begin(), ms.end(), step)) continue;
            Note n;
            n.time = a.beat * beatMs;
            n.lane = lane(a.deg);
            n.side = a.side;
            out.notes.push_back(n);
        }
    }
    for (const auto& f : comp.focus) out.focus.push_back({f.first * beatMs, uint8_t(f.second)});
    out.tempo.push_back({0, def.bpm, 0});
    out.finalize(def.bpm);
    // A sustain that overlaps the next note in its lane would be unplayable: trim it.
    std::vector<double> nextInLane(8, 1e18);
    for (size_t i = out.notes.size(); i-- > 0;) {
        Note& n = out.notes[i];
        double& nx = nextInLane[size_t(n.side * 4 + n.lane)];
        if (n.length > 0 && n.time + n.length > nx - beatMs * 0.25) n.length = std::max(0.0, nx - beatMs * 0.25 - n.time);
        if (n.length < beatMs * 0.25) n.length = 0;
        nx = n.time;
    }
    out.finalize(def.bpm);
    return true;
}

}  // namespace gh
