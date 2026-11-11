#include "flp_import.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common.hpp"

namespace gh {

namespace {

uint16_t rd16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// FL Studio 12 and later write names in UTF-16; older versions in ASCII.
// Latin text in UTF-16 has a zero in nearly every other byte.
std::string flString(const unsigned char* p, size_t n) {
    size_t odd = 0, oddZero = 0;
    for (size_t i = 1; i < n; i += 2) {
        ++odd;
        oddZero += p[i] == 0;
    }
    const bool wide = n >= 4 && n % 2 == 0 && oddZero * 4 >= odd * 3;
    std::string out;
    if (wide) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            const unsigned c = unsigned(p[i]) | unsigned(p[i + 1]) << 8;
            if (c == 0) break;
            if (c < 0x80) out += char(c);
            else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 63)); }
            else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        }
    } else {
        for (size_t i = 0; i < n && p[i]; ++i) out += char(p[i]);
    }
    return trim(out);
}

enum : uint8_t {
    kGenChannel = 0x40,  // word: a channel of the rack starts
    kPatStart = 0x41,    // word: the pattern the next events belong to
    kOldTempo = 0x42,    // word: tempo of old versions
    kTempo = 0x9C,       // dword: tempo x 1000
    kPatName = 0xC1,
    kTitle = 0xC2,
    kPlugName = 0xC9,
    kNoteData = 0xE0,
};

constexpr size_t kNoteSize = 24;

bool isSpecial(uint32_t pitch) { return pitch == 53 || pitch == 54 || pitch == 56 || pitch == 57 || pitch == 58; }
bool isChartPitch(uint32_t pitch) { return (pitch >= 48 && pitch <= 51) || (pitch >= 60 && pitch <= 63); }

}  // namespace

bool readFlp(std::string_view bytes, FlpProject& out, std::string& err) {
    out = FlpProject();
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    if (n < 22 || std::memcmp(b, "FLhd", 4) != 0) { err = "not an FL Studio file"; return false; }
    const uint32_t headLen = rd32(b + 4);
    if (headLen >= 6) out.ppqn = std::max<int>(1, rd16(b + 12));
    size_t i = 8 + size_t(headLen);
    if (i + 8 > n || std::memcmp(b + i, "FLdt", 4) != 0) { err = "FL Studio file without data"; return false; }
    const size_t end = std::min(n, i + 8 + size_t(rd32(b + i + 4)));
    i += 8;
    int prevId = -1;
    uint32_t prevValue = 0, lastChannel = 0;
    bool fpcFound = false;
    auto pattern = [&](int number) -> FlpPattern& {
        for (FlpPattern& p : out.patterns)
            if (p.number == number) return p;
        out.patterns.push_back(FlpPattern{number, {}, {}});
        return out.patterns.back();
    };
    while (i < end) {
        const uint8_t id = b[i++];
        uint32_t value = 0;
        const unsigned char* data = nullptr;
        size_t len = 0;
        if (id < 64) {
            if (i + 1 > end) break;
            value = b[i];
            i += 1;
        } else if (id < 128) {
            if (i + 2 > end) break;
            value = rd16(b + i);
            i += 2;
        } else if (id < 192) {
            if (i + 4 > end) break;
            value = rd32(b + i);
            i += 4;
        } else {
            size_t shift = 0;
            for (;;) {  // length: 7 bits per byte, high bit = more
                if (i >= end) break;
                const uint8_t c = b[i++];
                len |= size_t(c & 0x7F) << shift;
                shift += 7;
                if (!(c & 0x80) || shift > 28) break;
            }
            if (i + len > end) break;
            data = b + i;
            i += len;
        }
        switch (id) {
            case kGenChannel: lastChannel = value; break;
            case kTempo: out.bpm = value / 1000.0; break;
            case kOldTempo: if (out.bpm <= 0) out.bpm = value; break;
            case kTitle: out.title = flString(data, len); break;
            case kPlugName:
                if (!fpcFound && flString(data, len) == "FPC") {
                    out.chartChannel = int(lastChannel);
                    fpcFound = true;
                }
                break;
            case kPatName:
                if (prevId == kPatStart) pattern(int(prevValue)).name = flString(data, len);
                break;
            case kNoteData: {
                FlpPattern& p = pattern(prevId == kPatStart ? int(prevValue) : 0);
                for (size_t k = 0; k + kNoteSize <= len; k += kNoteSize) {
                    const unsigned char* r = data + k;
                    FlpNote note;
                    note.time = rd32(r);
                    note.channel = rd16(r + 6);
                    note.duration = rd32(r + 8);
                    note.pitch = rd32(r + 12);
                    note.flags = r[19];
                    note.velocity = r[21];
                    p.notes.push_back(note);
                }
                break;
            }
            default: break;
        }
        prevId = id;
        prevValue = value;
    }
    std::sort(out.patterns.begin(), out.patterns.end(), [](const FlpPattern& a, const FlpPattern& c) { return a.number < c.number; });
    return true;
}

bool readFlpFile(const std::string& path, FlpProject& out, std::string& err) {
    std::string bytes;
    if (!readTextFile(path, bytes)) { err = "cannot read " + path; return false; }
    return readFlp(bytes, out, err);
}

std::vector<int> flpChartPatterns(const FlpProject& p, bool strict) {
    std::vector<int> out;
    for (const FlpPattern& pat : p.patterns) {
        size_t onChannel = 0, sniffKeys = 0, lanes = 0;
        for (const FlpNote& n : pat.notes) {
            if (n.channel != p.chartChannel) continue;
            ++onChannel;
            sniffKeys += isChartPitch(n.pitch) || isSpecial(n.pitch);
            lanes += isChartPitch(n.pitch);
        }
        if (lanes > 0 && (!strict || sniffKeys * 10 >= onChannel * 9)) out.push_back(pat.number);
    }
    return out;
}

std::string flpPatternLabel(const FlpProject& p, int number) {
    for (const FlpPattern& pat : p.patterns)
        if (pat.number == number && !pat.name.empty()) return prettyName(pat.name);
    return "Pattern " + std::to_string(number);
}

bool flpToChart(const FlpProject& p, int pattern, Chart& out, std::string& err, double bpmIn) {
    out = Chart();
    if (pattern < 0) {
        std::vector<int> list = flpChartPatterns(p, true);
        if (list.empty()) list = flpChartPatterns(p, false);  // asked for this file: any notes in the lanes
        if (list.empty()) { err = "no notes in the chart lanes (C3-D#3, C4-D#4)"; return false; }
        pattern = list.front();
    }
    const FlpPattern* pat = nullptr;
    for (const FlpPattern& q : p.patterns)
        if (q.number == pattern) pat = &q;
    if (!pat) { err = "no pattern " + std::to_string(pattern); return false; }
    const double bpm = bpmIn > 0 ? bpmIn : p.bpm > 0 ? p.bpm : 120.0;
    const double msPerTick = 60000.0 / (bpm * p.ppqn);

    std::vector<FlpNote> notes;
    for (const FlpNote& n : pat->notes)
        if (n.channel == p.chartChannel) notes.push_back(n);
    // By time; at the same time the section markers come first (SNIFF).
    std::stable_sort(notes.begin(), notes.end(), [](const FlpNote& a, const FlpNote& b) {
        if (a.time != b.time) return a.time < b.time;
        return isSpecial(a.pitch) && !isSpecial(b.pitch);
    });

    const uint32_t sectionTicks = uint32_t(p.ppqn) * 4;
    struct Sec { bool mustHit = true, altOpp = false, altPlayer = false; };
    std::vector<Sec> secs;
    bool mustHit = true;
    auto secAt = [&](uint32_t t) -> Sec& {
        const size_t s = t / sectionTicks;
        while (secs.size() <= s) {
            Sec next;
            next.mustHit = mustHit;
            secs.push_back(next);
        }
        return secs[s];
    };
    struct Pending { Note n; size_t sec; };
    std::vector<Pending> pending;
    for (const FlpNote& n : notes) {
        Sec& sec = secAt(n.time);
        switch (n.pitch) {
            case 53: mustHit = sec.mustHit = true; continue;   // camera on the player
            case 54: mustHit = sec.mustHit = false; continue;  // camera on the opponent
            case 57: sec.altOpp = true; continue;
            case 58: sec.altPlayer = true; continue;
            case 56: continue;  // a tempo change: SNIFF asks for the new tempo, the chart keeps one
            default: break;
        }
        if (!isChartPitch(n.pitch)) continue;
        Note note;
        note.time = n.time * msPerTick;
        note.side = n.pitch <= 51 ? 1 : 0;
        note.lane = uint8_t(n.pitch <= 51 ? n.pitch - 48 : n.pitch - 60);
        if (n.velocity < 0x40 || n.duration >= uint32_t(p.ppqn))
            note.length = std::max(0.0, (double(n.duration) - p.ppqn / 4.0) * msPerTick);
        pending.push_back({note, n.time / sectionTicks});
    }
    if (pending.empty()) { err = "no notes in the chart lanes (C3-D#3, C4-D#4)"; return false; }
    for (Pending& pn : pending) {
        const Sec& sec = secs[pn.sec];
        if ((pn.n.side == 0 && sec.altOpp) || (pn.n.side == 1 && sec.altPlayer)) pn.n.flags |= kFlagAlt;
        out.notes.push_back(pn.n);
    }
    const double sectionMs = 4 * 60000.0 / bpm;
    for (size_t s = 0; s < secs.size(); ++s) {
        out.sections.push_back({double(s) * sectionMs, secs[s].mustHit, false, secs[s].altOpp, 4, bpm});
        const uint8_t side = secs[s].mustHit ? 1 : 0;
        if (out.focus.empty() || out.focus.back().side != side) out.focus.push_back({double(s) * sectionMs, side});
    }
    out.title = p.title;
    out.keys = 4;
    out.speed = clampv(bpm / 50.0, 0.5, 6.0);  // SNIFF's default scroll speed
    out.tempo.push_back({0, bpm, 0});
    out.rawNoteCount = out.notes.size();
    out.finalize(bpm);
    for (Note& n : out.notes)
        if (n.length > 0 && n.length < out.stepMs(n.time) * 0.999) n.length = 0;
    return true;
}

bool flpChartPath(const std::string& chartPath, std::string& file, int& pattern) {
    file = chartPath;
    pattern = -1;
    const size_t hash = chartPath.rfind('#');
    std::string base = hash == std::string::npos ? chartPath : chartPath.substr(0, hash);
    if (!endsWithI(base, ".flp") && !endsWithI(base, ".fsc")) return false;
    if (hash != std::string::npos) {
        const std::string num = chartPath.substr(hash + 1);
        if (num.empty() || !std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
            base = chartPath;  // a '#' in the file name itself
            if (!endsWithI(base, ".flp") && !endsWithI(base, ".fsc")) return false;
        } else {
            pattern = std::atoi(num.c_str());
        }
    }
    file = base;
    return true;
}

}  // namespace gh
