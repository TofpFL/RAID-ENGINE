#include "flp_import.hpp"

#include <algorithm>
#include <cctype>
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
    kPlaylist = 0xE9,   // arrangement clips: packed 32/60-byte records
};

constexpr size_t kNoteSize = 24;

bool isSpecial(uint32_t pitch) { return pitch == 53 || pitch == 54 || pitch == 56 || pitch == 57 || pitch == 58; }

// A channel named "Opp", "Opponent" or "Player" - optionally with a 1-50
// index like "Opp3" or "Player 12" - charts by channel instead of the FPC
// pitch windows; singing and atonal instruments sharing the prefix all
// land on the one side.
bool flpSideName(const std::string& nm, int& side) {
    std::string low;
    low.reserve(nm.size());
    for (char c : nm) low += char(std::tolower((unsigned char)c));
    static const char* kBase[] = {"opponent", "player", "opp"};
    static const int kSide[] = {0, 1, 0};
    for (size_t b = 0; b < 3; ++b) {
        const std::string base = kBase[b];
        if (low == base) {
            side = kSide[b];
            return true;
        }
        if (low.size() <= base.size() || low.compare(0, base.size(), base) != 0) continue;
        std::string rest = low.substr(base.size());
        const size_t a = rest.find_first_not_of(' '), z = rest.find_last_not_of(' ');
        if (a == std::string::npos) continue;
        rest = rest.substr(a, z - a + 1);
        if (rest.empty() ||
            !std::all_of(rest.begin(), rest.end(), [](char c) { return std::isdigit((unsigned char)c); }))
            continue;
        const int idx = std::atoi(rest.c_str());
        if (idx >= 1 && idx <= 50) {
            side = kSide[b];
            return true;
        }
    }
    return false;
}

// A channel named "Cam Events" (or CamEvents / Cam / Camera) carries
// the camera and section markers instead of notes.
bool flpCamName(const std::string& nm) {
    std::string low;
    low.reserve(nm.size());
    for (char c : nm) low += char(std::tolower((unsigned char)c));
    const size_t a = low.find_first_not_of(' '), z = low.find_last_not_of(' ');
    if (a == std::string::npos) return false;
    low = low.substr(a, z - a + 1);
    return low == "cam events" || low == "camevents" || low == "cam" || low == "camera";
}

struct FlpNamedChannels {
    std::vector<uint32_t> opp, player, cam;  // sorted, unique
};

FlpNamedChannels flpNamedChannels(const FlpProject& p) {
    FlpNamedChannels r;
    for (const auto& [ch, names] : p.channelNames)
        for (const std::string& nm : names) {
            int side = -1;
            if (flpSideName(nm, side)) (side == 0 ? r.opp : r.player).push_back(ch);
            if (flpCamName(nm)) r.cam.push_back(ch);
        }
    auto dedupe = [](std::vector<uint32_t>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    dedupe(r.opp);
    dedupe(r.player);
    dedupe(r.cam);
    return r;
}

// 0 = opponent channel, 1 = player channel, -1 = not a chart-side channel.
int flpNamedSide(const FlpNamedChannels& n, uint32_t ch) {
    if (std::binary_search(n.opp.begin(), n.opp.end(), ch)) return 0;
    if (std::binary_search(n.player.begin(), n.player.end(), ch)) return 1;
    return -1;
}

// True when the channel is a "Cam Events" instrument.
bool flpCamSide(const FlpNamedChannels& n, uint32_t ch) {
    return std::binary_search(n.cam.begin(), n.cam.end(), ch);
}

// A pattern charts as ONE side: every key on the piano roll maps onto the
// four lanes (pitch % 4, C-anchored) and the pattern's side comes from its
// content - named channels vote their channel's side, chart-channel notes
// vote by register (below middle C player, middle C and up opponent).
// Ties go to the player.
int flpPatternSide(const FlpProject& p, const FlpPattern& pat) {
    const FlpNamedChannels named = flpNamedChannels(p);
    uint64_t playerVotes = 0, oppVotes = 0;
    for (const FlpNote& n : pat.notes) {
        if (isSpecial(n.pitch)) continue;
        const int ns = flpNamedSide(named, n.channel);
        if (ns >= 0) {
            (ns == 1 ? playerVotes : oppVotes)++;
        } else if (n.channel == p.chartChannel) {
            (n.pitch < 60 ? playerVotes : oppVotes)++;
        }
    }
    return playerVotes >= oppVotes ? 1 : 0;
}

}  // namespace

bool readFlp(std::string_view bytes, FlpProject& out, std::string& err) {
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    if (n < 22 || std::memcmp(b, "FLhd", 4) != 0) { err = "not an FL Studio file"; return false; }
    const uint32_t headLen = rd32(b + 4);
    const int ppqn = headLen >= 6 ? std::max<int>(1, rd16(b + 12)) : 96;
    // FL 2025+ writes event 0xAC as a byte where the classic table says
    // dword; the header's channel count tells the two framings apart.
    const int wantChannels = headLen >= 6 ? int(rd16(b + 10)) : -1;
    size_t i = 8 + size_t(headLen);
    if (i + 8 > n || std::memcmp(b + i, "FLdt", 4) != 0) { err = "FL Studio file without data"; return false; }
    const size_t end = std::min(n, i + 8 + size_t(rd32(b + i + 4)));
    const size_t dataStart = i + 8;

    // One pass over the event stream; returns the channel-block count so the
    // caller can tell a desynced framing from a good one.
    auto parsePass = [&](bool acByte) -> size_t {
        out = FlpProject();
        out.ppqn = ppqn;
        i = dataStart;
        int prevId = -1;
        uint32_t prevValue = 0, lastChannel = 0;
        size_t chanEvents = 0;
        bool fpcFound = false, chanCtx = false;
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
            if (id < 64 || (acByte && id == 0xAC)) {
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
                case kGenChannel: lastChannel = value; chanCtx = true; ++chanEvents; break;
                case kPatStart: chanCtx = false; break;
                case kTempo: out.bpm = value / 1000.0; break;
                case kOldTempo: if (out.bpm <= 0) out.bpm = value; break;
                case 17: out.timeSigNum = int(value); break;   // time signature
                case 18: out.timeSigDen = int(value); break;   // numerator / denominator
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
                case kPlaylist: {
                // Records are 60 bytes in FL 21+ files, 32 before; a blob
                // divisible by 60 uses the new size (PyFLP's rule).
                const size_t rec = len % 60 == 0 && len >= 60 ? 60 : 32;
                for (size_t k = 0; k + rec <= len; k += rec) {
                    const unsigned char* r = data + k;
                    const int32_t pos = int32_t(rd32(r));
                    const int32_t patBase = int16_t(rd16(r + 4));
                    const int32_t itemIdx = int16_t(rd16(r + 6));
                    const int32_t clipLen = int32_t(rd32(r + 8));
                    const int32_t trackRvidx = int16_t(rd16(r + 12));
                    const int pattern = itemIdx - patBase;
                    if (pattern <= 0 || pos < 0 || clipLen <= 0) continue;  // channel/audio clips
                    FlpPlaylistItem it;
                    it.pattern = pattern;
                    it.track = trackRvidx > 0 ? 500 - trackRvidx : 0;
                    it.startTick = uint32_t(pos);
                    it.lengthTicks = uint32_t(clipLen);
                    out.playlist.push_back(it);
                }
                break;
            }            case kNoteData: {
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
            // Text events inside a channel's definition block are its names
            // (channel name, plugin name); pattern names never reach here.
            if (chanCtx && data && id >= 0xC0 && id <= 0xCF) {
                std::string nm = flString(data, len);
                if (!nm.empty()) {
                    auto& v = out.channelNames[lastChannel];
                    if (v.empty() || v.back() != nm) v.push_back(nm);
                }
            }
            prevId = id;
            prevValue = value;
        }
        std::sort(out.patterns.begin(), out.patterns.end(), [](const FlpPattern& a, const FlpPattern& c) { return a.number < c.number; });
        return chanEvents;
    };

    const size_t classic = parsePass(false);
    if (wantChannels < 0 || classic == size_t(wantChannels)) return true;
    // Classic framing desynced: FL 2025+ writes 0xAC as a byte event.
    const size_t quirky = parsePass(true);
    if (wantChannels < 0 || quirky == size_t(wantChannels)) return true;
    parsePass(false);  // neither framing validated: keep the classic result
    return true;
}

bool readFlpFile(const std::string& path, FlpProject& out, std::string& err) {
    std::string bytes;
    if (!readTextFile(path, bytes)) { err = "cannot read " + path; return false; }
    return readFlp(bytes, out, err);
}

std::vector<int> flpChartPatterns(const FlpProject& p, bool strict) {
    const auto named = flpNamedChannels(p);
    std::vector<int> out;
    for (const FlpPattern& pat : p.patterns) {
        size_t onChannel = 0, sniffKeys = 0, lanes = 0;
        for (const FlpNote& n : pat.notes) {
            const bool onNamed = flpNamedSide(named, n.channel) >= 0;
            if (!onNamed && n.channel != p.chartChannel) continue;
            ++onChannel;
            ++sniffKeys;  // every key on a chart channel charts now
            ++lanes;
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

double flpBpmScale(const FlpProject& p) {
    // The signature's denominator names the beat: x/16 -> a 16th-note beat
    // (tempo x4), x/8 -> an 8th (x2), x/4 -> a quarter (x1). The numerator
    // only counts beats per bar, so it does not enter the scale.
    return p.timeSigDen > 0 ? p.timeSigDen / 4.0 : 1.0;
}

bool flpToChart(const FlpProject& p, int pattern, Chart& out, std::string& err, double bpmIn) {
    out = Chart();
    if (pattern < 0) {
        std::vector<int> list = flpChartPatterns(p, true);
        if (list.empty()) list = flpChartPatterns(p, false);  // asked for this file: any notes in the lanes
        if (list.empty()) { err = "no chart notes"; return false; }
        pattern = list.front();
    }
    const FlpPattern* pat = nullptr;
    for (const FlpPattern& q : p.patterns)
        if (q.number == pattern) pat = &q;
    if (!pat) { err = "no pattern " + std::to_string(pattern); return false; }
    const int pside = flpPatternSide(p, *pat);
    const double bpm = (bpmIn > 0 ? bpmIn : p.bpm > 0 ? p.bpm : 120.0) * flpBpmScale(p);
    const double msPerTick = 60000.0 / (bpm * p.ppqn);

    const auto named = flpNamedChannels(p);
    std::vector<FlpNote> notes;
    for (const FlpNote& n : pat->notes)
        if (flpNamedSide(named, n.channel) >= 0 || n.channel == p.chartChannel ||
            flpCamSide(named, n.channel)) notes.push_back(n);
    // By time; at the same time the section markers come first (SNIFF).
    std::stable_sort(notes.begin(), notes.end(), [&](const FlpNote& a, const FlpNote& b) {
        if (a.time != b.time) return a.time < b.time;
        const bool sa = flpCamSide(named, a.channel) && isSpecial(a.pitch);
        const bool sb = flpCamSide(named, b.channel) && isSpecial(b.pitch);
        return sa && !sb;
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
        if (flpCamSide(named, n.channel)) {
            // Cam Events: the pitch picks the marker; never a chart note.
            switch (n.pitch) {
                case 53: mustHit = sec.mustHit = true; break;   // camera on the player
                case 54: mustHit = sec.mustHit = false; break;  // camera on the opponent
                case 57: sec.altOpp = true; break;
                case 58: sec.altPlayer = true; break;
                default: break;  // tempo changes and anything else: ignored
            }
            continue;
        }
        const int nside = flpNamedSide(named, n.channel);
        // Named channels keep their channel's side; the chart channel takes
        // the pattern's classified side. Every key charts: lane = pitch % 4.
        const int side = nside >= 0 ? nside : pside;
        const uint8_t lane = uint8_t(n.pitch & 3);
        Note note;
        note.time = n.time * msPerTick;
        note.side = side;
        note.lane = lane;
        if (n.velocity < 0x40 || n.duration >= uint32_t(p.ppqn))
            note.length = std::max(0.0, (double(n.duration) - p.ppqn / 4.0) * msPerTick);
        pending.push_back({note, n.time / sectionTicks});
    }
    if (pending.empty()) { err = "no chart notes"; return false; }
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
