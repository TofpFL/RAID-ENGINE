// RAID Engine: .raid binary chart format â€” writer, streaming reader,
// pattern-matching encoder. See raid.hpp for the format overview.
//
// Op encoding inside chunk/pattern payloads (all LEB128 varints, signed
// values zigzagged):
//
//   op 0  literal, absolute (chunk level):
//         u32 count
//         timeUs:   first i64 zigzag, then count-1 u64 deltas (non-decreasing)
//         lengthUs: count x u64
//         pitch:    count x zigzag of pitch+1 (-1 -> 0)
//         lane:     count x u64
//         attr:     count x u64  (side | kind<<1 | flags<<2)
//         type:     count x u64
//         stack:    count x u64
//
//   op 1  literal, relative (pattern level): same columns, but the first
//         relTimeUs is the note's offset from the pattern start (the rest
//         are deltas), pitch is pitchRel (pitch - pattern pitch origin,
//         zigzag; meaningful only when the pattern's hasPitch flag is set)
//         and lane is laneRel (zigzag).
//
//   op 2  pattern instance:
//         u32 patternId, u32 repeatCount,
//         i64 anchorUs         (zigzag; chunk level: absolute timeUs; inside a
//                              pattern: offset from the parent pattern start)
//         i64 pitchOriginDelta (zigzag; added to the current pitch base,
//                              which is 0 at chunk level -> absolute pitch
//                              of the pattern's first note)
//         i64 laneOriginDelta  (zigzag; same for lane)
//         The child pattern repeats repeatCount times, each copy shifted by
//         the child pattern's spanUs.
//
//   op 3  pattern group ("play these tables together"):
//         u32 entryCount (1..4096), then per entry:
//           u32 patternId, u32 repeatCount, i64 anchorUs, i64 pitchDelta,
//           i64 laneDelta   (same meaning as op 2)
//           i64 winLoUs, i64 winHiUs  (zigzag; absolute song microseconds â€”
//                              only events with timeUs in [winLoUs, winHiUs)
//                              are emitted; INT64_MIN/INT64_MAX = unbounded)
//         Every entry expands independently (its own repeats and window)
//         and the results are merged by timeUs, so several pattern tables
//         can sound at once â€” the playlist-overlap case. Entries' windows
//         are absolute, so they stay correct under repeat shifts and
//         nesting.
//
// Pattern section payload: u32 count, then per pattern
// {u64 spanUs, u8 flags (bit0 hasPitch), u32 opsLen, ops[opsLen]} where ops
// is a u32 opCount followed by ops, then u32 crc32 of the preceding bytes.
// Chunk payloads are opCount + ops + u64 expandedCount; the chunk index
// entry carries the crc32 of the whole decompressed payload.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "miniz.h"
#include "raid.hpp"
#include "raid_chart.hpp"
#include "flp_import.hpp"

namespace gh::raid {

namespace {

// ------------------------------------------------------------ byte I/O ----

struct Buf {
    std::vector<char> b;
    void u8(uint32_t v) { b.push_back(char(v & 0xFF)); }
    void u16(uint32_t v) { u8(v); u8(v >> 8); }
    void u32(uint32_t v) { u16(v); u16(v >> 16); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void i64(int64_t v) { u64(uint64_t(v)); }
    void var(uint64_t v) {
        while (v >= 0x80) { u8(uint32_t(v) | 0x80); v >>= 7; }
        u8(uint32_t(v));
    }
    void zig(int64_t v) { var((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }
    void raw(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        b.insert(b.end(), c, c + n);
    }
    void padTo(size_t n) { while (b.size() < n) b.push_back(0); }
};

struct Cur {
    const char* p = nullptr;
    const char* end = nullptr;
    bool ok = true;
    uint8_t u8() {
        if (p >= end) { ok = false; return 0; }
        return uint8_t(*p++);
    }
    uint16_t u16() { return uint16_t(u8() | (u8() << 8)); }
    uint32_t u32() { return uint32_t(u16()) | (uint32_t(u16()) << 16); }
    uint64_t u64() { return uint64_t(u32()) | (uint64_t(u32()) << 32); }
    int64_t i64() { return int64_t(u64()); }
    uint64_t var() {
        uint64_t v = 0;
        int s = 0;
        for (;;) {
            uint8_t c = u8();
            if (!ok) return 0;
            v |= uint64_t(c & 0x7F) << s;
            if (!(c & 0x80)) return v;
            s += 7;
            if (s > 63) { ok = false; return 0; }
        }
    }
    int64_t zig() {
        uint64_t v = var();
        return int64_t((v >> 1) ^ uint64_t(0) - (v & 1));
    }
    bool skip(size_t n) {
        if (size_t(end - p) < n) { ok = false; return false; }
        p += n;
        return true;
    }
};

// ------------------------------------------------------- compression -------

bool deflate(const char* src, size_t n, int level, std::vector<char>& out) {
    mz_ulong cap = mz_compressBound(mz_ulong(n));
    out.resize(cap);
    mz_ulong dst = cap;
    if (mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &dst,
                     reinterpret_cast<const unsigned char*>(src), mz_ulong(n), level) != MZ_OK)
        return false;
    out.resize(dst);
    return true;
}

bool inflate(const char* src, size_t n, size_t rawSize, std::vector<char>& out) {
    out.resize(rawSize);
    mz_ulong dst = mz_ulong(rawSize);
    if (mz_uncompress(reinterpret_cast<unsigned char*>(out.data()), &dst,
                      reinterpret_cast<const unsigned char*>(src), mz_ulong(n)) != MZ_OK ||
        dst != rawSize)
        return false;
    return true;
}

uint32_t crcOf(const void* p, size_t n) {
    return uint32_t(mz_crc32(0, reinterpret_cast<const unsigned char*>(p), n));
}

uint32_t getU32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// ------------------------------------------------------------ op codec ----

constexpr uint8_t kOpLiteralAbs = 0;
constexpr uint8_t kOpLiteralRel = 1;
constexpr uint8_t kOpPattern = 2;
constexpr uint8_t kOpGroup = 3;
constexpr int kMaxDepth = 8;
// kMaxChunkExpand lives in raid.hpp so the encoder can enforce it too.

void writeAttr(Buf& b, const Event& e) {
    b.var(uint64_t(e.side) | (uint64_t(e.kind) << 1) | (uint64_t(e.flags) << 2));
}

void writeLiteralAbs(Buf& b, const Event* e, size_t n, size_t startIdx) {
    b.u8(kOpLiteralAbs);
    b.u32(uint32_t(n));
    b.zig(e[startIdx].timeUs);
    for (size_t k = 1; k < n; ++k)
        b.var(uint64_t(e[startIdx + k].timeUs - e[startIdx + k - 1].timeUs));
    for (size_t k = 0; k < n; ++k) b.var(e[startIdx + k].lengthUs);
    for (size_t k = 0; k < n; ++k) b.zig(int64_t(e[startIdx + k].pitch) + 1);
    for (size_t k = 0; k < n; ++k) b.var(e[startIdx + k].lane);
    for (size_t k = 0; k < n; ++k) writeAttr(b, e[startIdx + k]);
    for (size_t k = 0; k < n; ++k) b.var(e[startIdx + k].type);
    for (size_t k = 0; k < n; ++k) b.var(e[startIdx + k].stack);
}

// Pattern-local literal: e[0..n) are consecutive events of one pattern run.
// t0/pitch0/lane0 are the run's first event (the pattern's canonical origin).
void writeLiteralRel(Buf& b, const Event* e, size_t n, int64_t t0,
                     int32_t pitch0, int32_t lane0) {
    b.u8(kOpLiteralRel);
    b.u32(uint32_t(n));
    b.var(uint64_t(e[0].timeUs - t0));
    for (size_t k = 1; k < n; ++k)
        b.var(uint64_t(e[k].timeUs - e[k - 1].timeUs));
    for (size_t k = 0; k < n; ++k) b.var(e[k].lengthUs);
    for (size_t k = 0; k < n; ++k) b.zig(int64_t(e[k].pitch) - pitch0);
    for (size_t k = 0; k < n; ++k) b.zig(int64_t(e[k].lane) - lane0);
    for (size_t k = 0; k < n; ++k) writeAttr(b, e[k]);
    for (size_t k = 0; k < n; ++k) b.var(e[k].type);
    for (size_t k = 0; k < n; ++k) b.var(e[k].stack);
}

void writeInst(Buf& b, uint32_t patternId, uint32_t repeatCount, int64_t anchorUs,
               int64_t pitchDelta, int64_t laneDelta) {
    b.u8(kOpPattern);
    b.u32(patternId);
    b.u32(repeatCount);
    b.zig(anchorUs);
    b.zig(pitchDelta);
    b.zig(laneDelta);
}

struct GroupEntry {
    uint32_t patternId = 0, repeatCount = 1;
    int64_t anchorUs = 0, pitchDelta = 0, laneDelta = 0;
    int64_t winLoUs = INT64_MIN, winHiUs = INT64_MAX;
};

void writeGroupOp(Buf& b, const GroupEntry* e, size_t n) {
    b.u8(kOpGroup);
    b.u32(uint32_t(n));
    for (size_t i = 0; i < n; ++i) {
        b.u32(e[i].patternId);
        b.u32(e[i].repeatCount);
        b.zig(e[i].anchorUs);
        b.zig(e[i].pitchDelta);
        b.zig(e[i].laneDelta);
        b.zig(e[i].winLoUs);
        b.zig(e[i].winHiUs);
    }
}

struct Ctx {
    int64_t timeBase = 0, pitchBase = 0, laneBase = 0;
    // Absolute-time clip for group-op entries: literal events outside
    // [clipLo, clipHi) are dropped during expansion.
    int64_t clipLo = INT64_MIN, clipHi = INT64_MAX;
};

struct Env {
    const std::vector<char>* blob = nullptr;   // pattern section payload
    const std::vector<PatternRec>* pats = nullptr;
    // patHasInst[i]: pattern i's op stream contains pattern ops (it is not
    // a leaf). Only used by the op-log walker; emitOps ignores it.
    const std::vector<uint8_t>* hasInst = nullptr;
};
// (blob->size() bounds every PatternRec.off + len)

struct EmitOut {
    std::vector<Event>* out;
    uint64_t produced = 0;
    uint64_t limit = kMaxChunkExpand;
};

bool emitOps(Cur& cur, const Env& env, const PatternRec* pat, const Ctx& ctx,
             int depth, EmitOut& eo, std::string& err);

// Adds a non-negative delta to a time base without signed overflow.
bool addTime(int64_t& t, uint64_t d, std::string& err) {
    if (d > uint64_t(INT64_MAX)) { err = "time delta out of range"; return false; }
    if (t >= 0 && d > uint64_t(INT64_MAX - t)) { err = "time overflow"; return false; }
    t += int64_t(d);
    return true;
}

// Fields decoded from varints/zigzags are bounded so accumulated pattern
// transforms cannot overflow int64 even with deep nesting.
constexpr int64_t kFieldBound = int64_t(1) << 40;

bool emitLiteral(Cur& cur, bool relative, const PatternRec* pat, const Ctx& ctx,
                 EmitOut& eo, std::string& err) {
    uint32_t n = uint32_t(cur.u32());
    if (!cur.ok || n == 0 || n > (1u << 24)) { err = "bad literal count"; return false; }
    std::vector<Event> tmp((size_t)n);  // extra parens: not a function decl
    // time column
    int64_t t = relative ? ctx.timeBase : 0;
    if (relative) {
        if (!addTime(t, cur.var(), err)) return false;
    } else {
        t = cur.zig();
    }
    if (!cur.ok) { err = "truncated literal"; return false; }
    // Deltas are unsigned, so times are non-decreasing within a literal: one
    // check on the first time bounds the whole run.
    if (t < 0) { err = "negative event time"; return false; }
    tmp[0].timeUs = t;
    for (uint32_t k = 1; k < n; ++k) {
        uint64_t d = cur.var();
        if (!cur.ok) { err = "truncated literal"; return false; }
        if (!addTime(t, d, err)) return false;
        tmp[k].timeUs = t;
    }
    // Group-op windows clip by absolute time; count what survives before
    // spending work on the property columns.
    size_t kept = 0;
    for (uint32_t k = 0; k < n; ++k)
        kept += tmp[k].timeUs >= ctx.clipLo && tmp[k].timeUs < ctx.clipHi;
    if (eo.produced + kept > eo.limit) { err = "chunk expands beyond its declared size"; return false; }
    // length column
    for (uint32_t k = 0; k < n; ++k) {
        uint64_t v = cur.var();
        if (!cur.ok || v > 0xFFFFFFFFull) { err = "truncated literal"; return false; }
        tmp[k].lengthUs = uint32_t(v);
    }
    // pitch column
    for (uint32_t k = 0; k < n; ++k) {
        int64_t p = cur.zig();
        if (!cur.ok || p < -kFieldBound || p > kFieldBound) { err = "pitch out of range"; return false; }
        int64_t pv = relative ? (pat && pat->hasPitch ? ctx.pitchBase + p : -1) : p - 1;
        if (pv < -1 || pv > 0x0FFFFFFF) { err = "pitch out of range"; return false; }
        tmp[k].pitch = int32_t(pv);
    }
    // lane column
    for (uint32_t k = 0; k < n; ++k) {
        int64_t l = relative ? cur.zig() : int64_t(cur.var());
        if (!cur.ok || l < -kFieldBound || l > kFieldBound) { err = "lane out of range"; return false; }
        int64_t lane = relative ? ctx.laneBase + l : l;
        if (lane < 0 || lane > 255) { err = "lane out of range"; return false; }
        tmp[k].lane = uint8_t(lane);
    }
    // attr / type / stack columns
    for (uint32_t k = 0; k < n; ++k) {
        uint64_t a = cur.var();
        if (!cur.ok || a > 0x3FF) { err = "bad note attr"; return false; }
        tmp[k].side = uint8_t(a & 1);
        tmp[k].kind = uint8_t((a >> 1) & 1);
        tmp[k].flags = uint8_t((a >> 2) & 0xFF);
    }
    for (uint32_t k = 0; k < n; ++k) {
        uint64_t v = cur.var();
        if (!cur.ok || v > 0xFFFF) { err = "bad note type"; return false; }
        tmp[k].type = uint16_t(v);
    }
    for (uint32_t k = 0; k < n; ++k) {
        uint64_t v = cur.var();
        if (!cur.ok || v > 0xFFFF) { err = "bad note stack"; return false; }
        tmp[k].stack = uint16_t(v);
        if (tmp[k].stack == 0) tmp[k].stack = 1;
    }
    for (uint32_t k = 0; k < n; ++k) {
        if (tmp[k].timeUs < ctx.clipLo || tmp[k].timeUs >= ctx.clipHi) continue;
        eo.out->push_back(tmp[k]);
    }
    eo.produced += kept;
    return true;
}

bool emitOps(Cur& cur, const Env& env, const PatternRec* pat, const Ctx& ctx,
             int depth, EmitOut& eo, std::string& err) {
    if (depth > kMaxDepth) { err = "pattern nesting too deep"; return false; }
    uint32_t opCount = uint32_t(cur.u32());
    if (!cur.ok || opCount > (1u << 22)) { err = "bad op count"; return false; }
    for (uint32_t o = 0; o < opCount; ++o) {
        uint8_t kind = cur.u8();
        if (!cur.ok) { err = "truncated op"; return false; }
        if (kind == kOpLiteralAbs) {
            if (!emitLiteral(cur, false, nullptr, ctx, eo, err)) return false;
        } else if (kind == kOpLiteralRel) {
            if (!pat) { err = "relative literal outside a pattern"; return false; }
            if (!emitLiteral(cur, true, pat, ctx, eo, err)) return false;
        } else if (kind == kOpPattern) {
            uint32_t pid = uint32_t(cur.u32());
            uint32_t reps = uint32_t(cur.u32());
            int64_t anchor = cur.zig();
            int64_t pitchDelta = cur.zig();
            int64_t laneDelta = cur.zig();
            if (!cur.ok) { err = "truncated pattern op"; return false; }
            if (!env.pats || pid >= env.pats->size()) { err = "bad pattern id"; return false; }
            const PatternRec& child = (*env.pats)[pid];
            if (child.len < 4 || child.off > env.blob->size() ||
                child.len > env.blob->size() - child.off ||
                child.spanUs > (1ull << 50) ||
                (child.spanUs && uint64_t(reps) > (1ull << 50) / child.spanUs) ||
                anchor < -(int64_t(1) << 50) || anchor > (int64_t(1) << 50) ||
                pitchDelta < -kFieldBound || pitchDelta > kFieldBound ||
                laneDelta < -kFieldBound || laneDelta > kFieldBound) {
                err = "pattern transform out of range";
                return false;
            }
            if (reps > eo.limit) { err = "pattern repeat count too large"; return false; }
            for (uint32_t r = 0; r < reps; ++r) {
                Ctx sub{ctx.timeBase + anchor + int64_t(r) * int64_t(child.spanUs),
                        ctx.pitchBase + pitchDelta, ctx.laneBase + laneDelta,
                        ctx.clipLo, ctx.clipHi};
                Cur cc{env.blob->data() + child.off, env.blob->data() + child.off + child.len};
                if (!emitOps(cc, env, &child, sub, depth + 1, eo, err)) return false;
            }
        } else if (kind == kOpGroup) {
            uint32_t nEnt = uint32_t(cur.u32());
            if (!cur.ok || nEnt == 0 || nEnt > 4096) { err = "bad group count"; return false; }
            struct GEnt {
                const PatternRec* child;
                uint32_t reps;
                int64_t anchor, pd, ld, lo, hi;
            };
            std::vector<GEnt> ents;
            ents.reserve(nEnt);
            for (uint32_t i = 0; i < nEnt; ++i) {
                const uint32_t pid = uint32_t(cur.u32());
                const uint32_t reps = uint32_t(cur.u32());
                const int64_t anchor = cur.zig(), pd = cur.zig(), ld = cur.zig();
                const int64_t lo = cur.zig(), hi = cur.zig();
                if (!cur.ok) { err = "truncated group op"; return false; }
                if (!env.pats || pid >= env.pats->size()) { err = "bad pattern id"; return false; }
                const PatternRec& child = (*env.pats)[pid];
                if (child.len < 4 || child.off > env.blob->size() ||
                    child.len > env.blob->size() - child.off ||
                    child.spanUs > (1ull << 50) ||
                    (child.spanUs && uint64_t(reps) > (1ull << 50) / child.spanUs) ||
                    anchor < -(int64_t(1) << 50) || anchor > (int64_t(1) << 50) ||
                    pd < -kFieldBound || pd > kFieldBound ||
                    ld < -kFieldBound || ld > kFieldBound ||
                    lo > hi) {
                    err = "pattern transform out of range";
                    return false;
                }
                if (reps > eo.limit) { err = "pattern repeat count too large"; return false; }
                ents.push_back({&child, reps, anchor, pd, ld, lo, hi});
            }
            // Expand each entry independently (entries may overlap â€” that is
            // the point of the op), then k-way merge the sorted runs into
            // the output. No merged copy: peak memory stays one chunk's
            // worth of events, same as a literal run.
            const uint64_t budget = eo.limit - eo.produced;
            std::vector<std::vector<Event>> parts(ents.size());
            uint64_t total = 0;
            for (size_t i = 0; i < ents.size(); ++i) {
                const GEnt& ge = ents[i];
                EmitOut sub{&parts[i], 0, budget};
                for (uint32_t r = 0; r < ge.reps; ++r) {
                    Ctx sctx{ctx.timeBase + ge.anchor + int64_t(r) * int64_t(ge.child->spanUs),
                             ctx.pitchBase + ge.pd, ctx.laneBase + ge.ld, ge.lo, ge.hi};
                    Cur cc{env.blob->data() + ge.child->off,
                           env.blob->data() + ge.child->off + ge.child->len};
                    if (!emitOps(cc, env, ge.child, sctx, depth + 1, sub, err)) return false;
                }
                total += sub.produced;
                if (total > budget) { err = "chunk expands beyond its declared size"; return false; }
            }
            std::vector<size_t> pos(parts.size(), 0);
            for (uint64_t k = 0; k < total; ++k) {
                size_t best = parts.size();
                int64_t bt = 0;
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (pos[i] < parts[i].size() &&
                        (best == parts.size() || parts[i][pos[i]].timeUs < bt)) {
                        best = i;
                        bt = parts[i][pos[i]].timeUs;
                    }
                }
                eo.out->push_back(parts[best][pos[best]++]);
            }
            eo.produced += total;
        } else {
            err = "unknown op kind";
            return false;
        }
    }
    return cur.ok;
}

// -------------------------------------------------- op-log walker ---------
// Mirrors emitOps but records segments instead of events. Only LEAF ops
// produce segments: literal runs always, pattern instances only when the
// pattern's own op stream contains no further pattern ops. Patterns that
// merely wrap other patterns are traversed (per repeat, with the same
// time/pitch/lane transforms emitOps applies) but not logged, so the
// resulting segment stream is a flat "what is sounding" timeline.

// Skips a literal op's column data after its u32 count: the time column
// (n varints) plus 6 property columns of n varints each.
bool skipLiteralBody(Cur& cur, uint32_t n, std::string& err) {
    for (uint32_t k = 0; k < n; ++k) {
        (void)cur.var();
        if (!cur.ok) { err = "truncated literal"; return false; }
    }
    for (int col = 0; col < 6; ++col)
        for (uint32_t k = 0; k < n; ++k) {
            (void)cur.var();
            if (!cur.ok) { err = "truncated literal"; return false; }
        }
    return true;
}

// Scans one pattern definition's top-level ops (cur at its opCount) and
// reports whether any op is a pattern instance. Used to flag non-leaf
// patterns for the op-log walker. `found` is only meaningful on success.
bool opsContainInst(Cur& cur, bool& found, std::string& err) {
    found = false;
    uint32_t opCount = uint32_t(cur.u32());
    if (!cur.ok || opCount > (1u << 22)) { err = "bad op count"; return false; }
    for (uint32_t o = 0; o < opCount; ++o) {
        uint8_t kind = cur.u8();
        if (!cur.ok) { err = "truncated op"; return false; }
        if (kind == kOpLiteralAbs || kind == kOpLiteralRel) {
            uint32_t n = uint32_t(cur.u32());
            if (!cur.ok || n == 0 || n > (1u << 24)) { err = "bad literal count"; return false; }
            if (!skipLiteralBody(cur, n, err)) return false;
        } else if (kind == kOpPattern || kind == kOpGroup) {
            found = true;
            return true;
        } else {
            err = "unknown op kind";
            return false;
        }
    }
    return cur.ok;
}

struct WalkOut {
    std::vector<Reader::OpSegment>* segs;
    uint32_t chunk = 0;
    uint64_t produced = 0;   // expanded events accounted for (== emitOps' eo.produced)
    uint64_t limit = 0;      // chunk's declared expanded count
    uint64_t visits = 0;     // op visits, capped against hostile nesting
};

bool walkLiteral(Cur& cur, bool relative, const Ctx& ctx, WalkOut& wo,
                 bool emit, std::string& err) {
    uint32_t n = uint32_t(cur.u32());
    if (!cur.ok || n == 0 || n > (1u << 24)) { err = "bad literal count"; return false; }
    int64_t t = relative ? ctx.timeBase : 0;
    if (relative) {
        if (!addTime(t, cur.var(), err)) return false;
    } else {
        t = cur.zig();
    }
    if (!cur.ok) { err = "truncated literal"; return false; }
    if (t < 0) { err = "negative event time"; return false; }
    // Group-op windows clip by absolute time; times are non-decreasing so
    // the surviving run is contiguous â€” track its bounds and count.
    uint32_t kept = 0;
    int64_t kFirst = 0, kLast = 0;
    auto note = [&](int64_t tt) {
        if (tt >= ctx.clipLo && tt < ctx.clipHi) {
            if (kept == 0) kFirst = tt;
            kLast = tt;
            ++kept;
        }
    };
    note(t);
    for (uint32_t k = 1; k < n; ++k) {
        uint64_t d = cur.var();
        if (!cur.ok) { err = "truncated literal"; return false; }
        if (!addTime(t, d, err)) return false;
        note(t);
    }
    if (wo.produced + kept > wo.limit) { err = "chunk expands beyond its declared size"; return false; }
    // Property columns (length, pitch, lane, attr, type, stack): n varints
    // each â€” zigzag and varint consume identically, values unneeded here.
    for (int col = 0; col < 6; ++col)
        for (uint32_t k = 0; k < n; ++k) {
            (void)cur.var();
            if (!cur.ok) { err = "truncated literal"; return false; }
        }
    if (emit && kept) {
        Reader::OpSegment s;
        s.startUs = kFirst; s.endUs = kLast; s.chunk = wo.chunk;
        s.kind = 0; s.count = kept;
        wo.segs->push_back(s);
    }
    wo.produced += kept;
    return true;
}

// `emit` is false while walking inside a leaf pattern instance: events are
// still counted (produced/visits bounds apply) but no segments are
// recorded, so a pattern's own literal content doesn't double-cover the
// pattern segment that represents it.
bool walkOps(Cur& cur, const Env& env, const Ctx& ctx, int depth,
             WalkOut& wo, bool emit, std::string& err) {
    if (depth > kMaxDepth) { err = "pattern nesting too deep"; return false; }
    uint32_t opCount = uint32_t(cur.u32());
    if (!cur.ok || opCount > (1u << 22)) { err = "bad op count"; return false; }
    for (uint32_t o = 0; o < opCount; ++o) {
        if (++wo.visits > (1ull << 25)) { err = "op log too large"; return false; }
        uint8_t kind = cur.u8();
        if (!cur.ok) { err = "truncated op"; return false; }
        if (kind == kOpLiteralAbs) {
            if (!walkLiteral(cur, false, ctx, wo, emit, err)) return false;
        } else if (kind == kOpLiteralRel) {
            if (depth == 0) { err = "relative literal outside a pattern"; return false; }
            if (!walkLiteral(cur, true, ctx, wo, emit, err)) return false;
        } else if (kind == kOpPattern) {
            uint32_t pid = uint32_t(cur.u32());
            uint32_t reps = uint32_t(cur.u32());
            int64_t anchor = cur.zig();
            int64_t pitchDelta = cur.zig();
            int64_t laneDelta = cur.zig();
            if (!cur.ok) { err = "truncated pattern op"; return false; }
            if (!env.pats || pid >= env.pats->size()) { err = "bad pattern id"; return false; }
            const PatternRec& child = (*env.pats)[pid];
            if (child.len < 4 || child.off > env.blob->size() ||
                child.len > env.blob->size() - child.off ||
                child.spanUs > (1ull << 50) ||
                (child.spanUs && uint64_t(reps) > (1ull << 50) / child.spanUs) ||
                anchor < -(int64_t(1) << 50) || anchor > (int64_t(1) << 50) ||
                pitchDelta < -kFieldBound || pitchDelta > kFieldBound ||
                laneDelta < -kFieldBound || laneDelta > kFieldBound) {
                err = "pattern transform out of range";
                return false;
            }
            if (reps > wo.limit) { err = "pattern repeat count too large"; return false; }
            const bool leaf = !env.hasInst || pid >= env.hasInst->size() ||
                              (*env.hasInst)[pid] == 0;
            if (leaf && emit) {
                const int64_t startUs = ctx.timeBase + anchor;
                int64_t endUs = startUs + int64_t(reps) * int64_t(child.spanUs);
                if (endUs < startUs) endUs = INT64_MAX;
                Reader::OpSegment s;
                s.startUs = startUs; s.endUs = endUs; s.chunk = wo.chunk;
                s.kind = 2; s.count = pid; s.aux = reps;
                s.pitchDelta = int32_t(pitchDelta); s.laneDelta = int32_t(laneDelta);
                wo.segs->push_back(s);
            }
            // Leaf instances are walked with emit=false: their expanded
            // events still count toward produced/visits, but their literal
            // content stays hidden behind the pattern segment.
            const bool childEmit = emit && !leaf;
            for (uint32_t r = 0; r < reps; ++r) {
                Ctx sub{ctx.timeBase + anchor + int64_t(r) * int64_t(child.spanUs),
                        ctx.pitchBase + pitchDelta, ctx.laneBase + laneDelta,
                        ctx.clipLo, ctx.clipHi};
                Cur cc{env.blob->data() + child.off,
                       env.blob->data() + child.off + child.len};
                if (!walkOps(cc, env, sub, depth + 1, wo, childEmit, err)) return false;
            }
        } else if (kind == kOpGroup) {
            uint32_t nEnt = uint32_t(cur.u32());
            if (!cur.ok || nEnt == 0 || nEnt > 4096) { err = "bad group count"; return false; }
            for (uint32_t i = 0; i < nEnt; ++i) {
                const uint32_t pid = uint32_t(cur.u32());
                const uint32_t reps = uint32_t(cur.u32());
                const int64_t anchor = cur.zig(), pd = cur.zig(), ld = cur.zig();
                const int64_t lo = cur.zig(), hi = cur.zig();
                if (!cur.ok) { err = "truncated group op"; return false; }
                if (!env.pats || pid >= env.pats->size()) { err = "bad pattern id"; return false; }
                const PatternRec& child = (*env.pats)[pid];
                if (child.len < 4 || child.off > env.blob->size() ||
                    child.len > env.blob->size() - child.off ||
                    child.spanUs > (1ull << 50) ||
                    (child.spanUs && uint64_t(reps) > (1ull << 50) / child.spanUs) ||
                    anchor < -(int64_t(1) << 50) || anchor > (int64_t(1) << 50) ||
                    pd < -kFieldBound || pd > kFieldBound ||
                    ld < -kFieldBound || ld > kFieldBound ||
                    lo > hi) {
                    err = "pattern transform out of range";
                    return false;
                }
                if (reps > wo.limit) { err = "pattern repeat count too large"; return false; }
                const bool leaf = !env.hasInst || pid >= env.hasInst->size() ||
                                  (*env.hasInst)[pid] == 0;
                if (leaf && emit) {
                    const int64_t startUs = ctx.timeBase + anchor;
                    int64_t endUs = startUs + int64_t(reps) * int64_t(child.spanUs);
                    if (endUs < startUs) endUs = INT64_MAX;
                    // Clip the logged span to the entry's event window.
                    Reader::OpSegment s;
                    s.startUs = std::max(startUs, lo);
                    s.endUs = std::min(endUs, hi);
                    if (s.endUs < s.startUs) { s.startUs = s.endUs = startUs; }
                    s.chunk = wo.chunk;
                    s.kind = 2; s.count = pid; s.aux = reps;
                    s.pitchDelta = int32_t(pd); s.laneDelta = int32_t(ld);
                    wo.segs->push_back(s);
                }
                const bool childEmit = emit && !leaf;
                for (uint32_t r = 0; r < reps; ++r) {
                    Ctx sub{ctx.timeBase + anchor + int64_t(r) * int64_t(child.spanUs),
                            ctx.pitchBase + pd, ctx.laneBase + ld, lo, hi};
                    Cur cc{env.blob->data() + child.off,
                           env.blob->data() + child.off + child.len};
                    if (!walkOps(cc, env, sub, depth + 1, wo, childEmit, err)) return false;
                }
            }
        } else {
            err = "unknown op kind";
            return false;
        }
    }
    return cur.ok;
}

// ------------------------------------------------------- pattern match ----

struct Sig {
    int64_t dtUs;
    int32_t pitchDelta, laneDelta;
    uint32_t lenUs;
    uint8_t side, kind, flags;
    uint16_t type, stack;
    bool operator==(const Sig& o) const {
        return dtUs == o.dtUs && pitchDelta == o.pitchDelta && laneDelta == o.laneDelta &&
               lenUs == o.lenUs && side == o.side && kind == o.kind && flags == o.flags &&
               type == o.type && stack == o.stack;
    }
};

inline Sig sigAt(const std::vector<Event>& e, size_t i) {  // i >= 1
    const Event& a = e[i - 1];
    const Event& b = e[i];
    Sig s;
    s.dtUs = b.timeUs - a.timeUs;
    s.pitchDelta = (a.pitch >= 0 && b.pitch >= 0) ? b.pitch - a.pitch : 0;
    s.laneDelta = int32_t(b.lane) - int32_t(a.lane);
    s.lenUs = b.lengthUs;
    s.side = b.side; s.kind = b.kind; s.flags = b.flags;
    s.type = b.type; s.stack = b.stack;
    return s;
}

inline bool firstEq(const Event& a, const Event& b) {
    return a.lengthUs == b.lengthUs && a.side == b.side && a.kind == b.kind &&
           a.flags == b.flags && a.type == b.type && a.stack == b.stack;
}

// Longest run length L (capped) whose internal step signatures match between
// i and j, with equal first-note fields. Pitch/lane transforms are uniform
// shifts, so rel patterns are what matter.
size_t matchLen(const std::vector<Event>& e, size_t i, size_t j, size_t maxLen) {
    if (i == j || !firstEq(e[i], e[j])) return 0;
    size_t l = 1;
    while (l < maxLen && i + l < e.size() && j + l < e.size() &&
           sigAt(e, i + l) == sigAt(e, j + l))
        ++l;
    return l;
}

// 1 = every pitch present (>= 0), 0 = every pitch absent (-1), -1 = mixed
// (cannot be a pattern: pitch information would be lost).
int pitchMode(const std::vector<Event>& e, size_t i, size_t n) {
    bool any = false, none = false;
    for (size_t k = 0; k < n; ++k) {
        if (e[i + k].pitch >= 0) any = true; else none = true;
        if (any && none) return -1;
    }
    return any ? 1 : 0;
}

uint64_t hashRun(const std::vector<Event>& e, size_t i, size_t L, int64_t spanUs) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(uint64_t(spanUs));
    mix(uint64_t(pitchMode(e, i, L) + 1));
    const Event& f = e[i];
    mix(f.lengthUs);
    mix(uint64_t(f.side) | (uint64_t(f.kind) << 1) | (uint64_t(f.flags) << 2));
    mix(f.type); mix(f.stack);
    for (size_t k = 1; k < L; ++k) {
        Sig s = sigAt(e, i + k);
        mix(uint64_t(s.dtUs));
        mix(uint64_t(uint32_t(s.pitchDelta)));
        mix(uint64_t(uint32_t(s.laneDelta)));
        mix(s.lenUs);
        mix(uint64_t(s.side) | (uint64_t(s.kind) << 1) | (uint64_t(s.flags) << 2) |
             (uint64_t(s.type) << 10) | (uint64_t(s.stack) << 26));
    }
    return h;
}

// Hash of just the first `count` step signatures at position i (for the
// candidate index).
uint64_t hashSigs(const std::vector<Event>& e, size_t i, int count) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (int k = 1; k <= count && i + size_t(k) < e.size(); ++k) {
        Sig s = sigAt(e, i + size_t(k));
        mix(uint64_t(s.dtUs));
        mix(uint64_t(uint32_t(s.pitchDelta)));
        mix(uint64_t(uint32_t(s.laneDelta)));
        mix(s.lenUs);
        mix(uint64_t(s.side) | (uint64_t(s.kind) << 1) | (uint64_t(s.flags) << 2) |
             (uint64_t(s.type) << 10) | (uint64_t(s.stack) << 26));
    }
    return h;
}

struct Plan {
    size_t evStart = 0;
    uint32_t evCount = 0;    // expanded events covered
    int32_t patId = -1;      // >= 0: pattern instance
    uint32_t repCount = 0;   // pattern instances of patLen events each
    uint32_t patLen = 0;
};

struct Tmpl {
    size_t start = 0;
    uint32_t len = 0;
    int64_t spanUs = 0;
    bool hasPitch = false;
    int64_t lastRelUs = 0;
};

struct ChunkOut {
    Buf payload;  // u32 opCount + ops + u64 expandedCount (sealed)
    uint64_t firstEvent = 0;
    int64_t firstTimeUs = 0, lastTimeUs = 0;
    uint32_t expanded = 0, opCount = 0;
};

// Compresses chunk payloads + meta + pattern section and writes the whole
// .raid file (header, index, sections, chunks). Shared by writeRaid and the
// FL Studio playlist encoder.
bool writeRaidDone(const std::string& path, const std::vector<char>& patSection,
                   std::vector<ChunkOut>& chunks, uint64_t logicalN,
                   uint64_t literalEvents, uint64_t patternCount,
                   std::string_view metaJson, const EncodeOptions& opt,
                   Info* statsOut, std::string& err) {
    chunks.erase(std::remove_if(chunks.begin(), chunks.end(),
                                [](const ChunkOut& c) { return c.opCount == 0; }),
                 chunks.end());
    // ---- compress --------------------------------------------------------
    struct ChunkFile {
        std::vector<char> blob;
        uint64_t rawSize = 0;
        uint32_t crc = 0;
        bool compressed = false;
    };
    std::vector<ChunkFile> cf(chunks.size());
    uint64_t storedBytes = 0, rawBytes = 0;
    for (size_t k = 0; k < chunks.size(); ++k) {
        const ChunkOut& c = chunks[k];
        rawBytes += c.payload.b.size();
        cf[k].rawSize = c.payload.b.size();
        cf[k].crc = crcOf(c.payload.b.data(), c.payload.b.size());
        std::vector<char> comp;
        if (deflate(c.payload.b.data(), c.payload.b.size(), opt.compressLevel, comp) &&
            comp.size() < c.payload.b.size()) {
            cf[k].blob = std::move(comp);
            cf[k].compressed = true;
        } else {
            cf[k].blob = c.payload.b;
        }
        storedBytes += cf[k].blob.size();
    }

    // meta payload = metaJson + crc trailer
    std::vector<char> metaPayload(metaJson.begin(), metaJson.end());
    {
        Buf t;
        t.u32(crcOf(metaPayload.data(), metaPayload.size()));
        metaPayload.insert(metaPayload.end(), t.b.begin(), t.b.end());
    }
    std::vector<char> metaStored;
    uint64_t metaComp = 0, metaRaw = metaPayload.size();
    {
        std::vector<char> comp;
        if (deflate(metaPayload.data(), metaPayload.size(), opt.compressLevel, comp) &&
            comp.size() < metaPayload.size()) {
            metaComp = comp.size();
            metaStored = std::move(comp);
        } else {
            metaStored = metaPayload;
        }
    }
    std::vector<char> patStored;
    uint64_t patComp = 0, patRaw = patSection.size();
    {
        std::vector<char> comp;
        if (deflate(patSection.data(), patSection.size(), opt.compressLevel, comp) &&
            comp.size() < patSection.size()) {
            patComp = comp.size();
            patStored = std::move(comp);
        } else {
            patStored = patSection;
        }
    }

    // ---- write -----------------------------------------------------------
    const uint64_t indexOff = kHeaderSize;
    const uint64_t indexSize = chunks.size() * kIndexEntrySize;
    const uint64_t metaOffAbs = indexOff + indexSize;
    const uint64_t patOffAbs = metaOffAbs + metaStored.size();
    uint64_t chunkOffAbs = patOffAbs + patStored.size();

    Buf hdr;
    hdr.raw(kMagic, 4);
    hdr.u32(kVersion);
    hdr.u32(0);                     // flags
    hdr.u32(0);                     // reserved (keyCount lives in meta)
    hdr.u32(0);                     // reserved
    hdr.u64(logicalN);              // logicalEventCount (expanded)
    hdr.u64(chunks.size());
    hdr.u64(patternCount);
    hdr.u64(indexOff);
    hdr.u64(indexSize);
    hdr.u64(metaOffAbs);
    hdr.u64(metaComp);
    hdr.u64(metaRaw);
    hdr.u64(patOffAbs);
    hdr.u64(patComp);
    hdr.u64(patRaw);
    hdr.u32(0);                     // crc placeholder at byte 108
    hdr.u32(0);                     // reserved
    hdr.padTo(kHeaderSize);
    uint32_t hcrc = crcOf(hdr.b.data(), kHeaderSize);
    hdr.b[108] = char(hcrc); hdr.b[109] = char(hcrc >> 8);
    hdr.b[110] = char(hcrc >> 16); hdr.b[111] = char(hcrc >> 24);

    Buf idx;
    uint64_t coff = chunkOffAbs;
    for (size_t k = 0; k < chunks.size(); ++k) {
        const ChunkOut& c = chunks[k];
        idx.u64(c.firstEvent);
        idx.i64(c.firstTimeUs);
        idx.i64(c.lastTimeUs);
        idx.u64(coff);
        idx.u32(uint32_t(cf[k].blob.size()));
        idx.u32(uint32_t(cf[k].rawSize));
        idx.u32(c.expanded);
        idx.u32(c.opCount);
        idx.u32(cf[k].crc);
        idx.u32(cf[k].compressed ? 1u : 0u);
        coff += cf[k].blob.size();
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    bool ok = std::fwrite(hdr.b.data(), 1, hdr.b.size(), f) == hdr.b.size() &&
              std::fwrite(idx.b.data(), 1, idx.b.size(), f) == idx.b.size() &&
              std::fwrite(metaStored.data(), 1, metaStored.size(), f) == metaStored.size() &&
              std::fwrite(patStored.data(), 1, patStored.size(), f) == patStored.size();
    for (size_t k = 0; ok && k < cf.size(); ++k)
        ok = std::fwrite(cf[k].blob.data(), 1, cf[k].blob.size(), f) == cf[k].blob.size();
    std::fclose(f);
    if (!ok) { err = "write failed: " + path; return false; }

    if (statsOut) {
        statsOut->version = kVersion;
        statsOut->flags = 0;
        statsOut->keyCount = 0;
        statsOut->logicalEvents = logicalN;
        statsOut->chunkCount = chunks.size();
        statsOut->patternCount = patternCount;
        statsOut->literalEvents = literalEvents;
        statsOut->storedBytes = storedBytes;
        statsOut->rawBytes = rawBytes;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------ public API ---

bool isRaidFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char m[4] = {};
    const bool ok = std::fread(m, 1, 4, f) == 4 && std::memcmp(m, kMagic, 4) == 0;
    std::fclose(f);
    return ok;
}

bool writeRaid(const std::string& path, const std::vector<Event>& events,
               std::string_view metaJson, const EncodeOptions& opt,
               Info* statsOut, std::string& err) {
    err.clear();
    // Options must stay inside what the decoder accepts, or the file would be
    // written but unreadable.
    if (opt.chunkEvents == 0 || opt.chunkEvents > uint32_t(kMaxChunkExpand) ||
        opt.minPattern < 2 || opt.maxPattern < opt.minPattern ||
        opt.maxPattern > uint32_t(kMaxChunkExpand) ||
        opt.compressLevel < 1 || opt.compressLevel > 10) {
        err = "invalid encode options";
        return false;
    }
    for (size_t i = 0; i < events.size(); ++i) {
        const Event& e = events[i];
        // Every field is validated against the decoder's representable range;
        // anything outside it would decode differently or be rejected.
        if (e.timeUs < 0 || (i && e.timeUs < events[i - 1].timeUs)) {
            err = "events are not sorted by non-negative time";
            return false;
        }
        if (e.lane > 255 || e.side > 1 || e.kind > 1 || e.stack > 0xFFFF ||
            e.type > 0xFFFF ||
            (e.pitch != -1 && (e.pitch < 0 || e.pitch > 0x0FFFFFFF))) {
            err = "event field out of range at index " + std::to_string(i);
            return false;
        }
    }
    const size_t n = events.size();

    // ---- pattern detection -------------------------------------------
    std::vector<Plan> plans;
    std::vector<Tmpl> tmpls;
    std::unordered_map<uint64_t, std::vector<uint32_t>> tmplIndex;

    auto tmplEq = [&](const Tmpl& t, size_t i, size_t L, int64_t spanUs) {
        if (t.len != L || t.spanUs != spanUs) return false;
        int pm = pitchMode(events, i, L);
        if (pm < 0 || (pm == 1) != t.hasPitch) return false;
        if (!firstEq(events[t.start], events[i])) return false;
        for (size_t k = 1; k < L; ++k)
            if (!(sigAt(events, t.start + k) == sigAt(events, i + k))) return false;
        // sigs give equal deltas; verify absolute rels for pitch/lane.
        for (size_t k = 0; k < L; ++k) {
            if (pm == 1 &&
                events[t.start + k].pitch - events[t.start].pitch !=
                    events[i + k].pitch - events[i].pitch)
                return false;
            if (int32_t(events[t.start + k].lane) - int32_t(events[t.start].lane) !=
                int32_t(events[i + k].lane) - int32_t(events[i].lane))
                return false;
        }
        return true;
    };

    auto internPattern = [&](size_t i, size_t L, int64_t spanUs) -> uint32_t {
        uint64_t h = hashRun(events, i, L, spanUs);
        auto& vec = tmplIndex[h];
        for (uint32_t id : vec)
            if (tmplEq(tmpls[id], i, L, spanUs)) return id;
        uint32_t id = uint32_t(tmpls.size());
        tmpls.push_back({i, uint32_t(L), spanUs, pitchMode(events, i, L) == 1,
                         events[i + L - 1].timeUs - events[i].timeUs});
        vec.push_back(id);
        return id;
    };

    const bool detect = opt.patterns && n >= opt.minPattern * 2 &&
                        (opt.maxDetectEvents == 0 || n <= opt.maxDetectEvents);
    const uint32_t H = 6;  // signature window for candidate lookup
    std::unordered_map<uint64_t, std::vector<uint32_t>> heads;

    size_t litStart = 0;
    size_t i = 0;
    while (i < n) {
        size_t bestL = 0;
        if (detect && i + opt.minPattern <= n) {
            uint64_t h = hashSigs(events, i, int(H));
            auto it = heads.find(h);
            if (it != heads.end()) {
                const auto& cand = it->second;
                size_t checked = 0;
                for (auto rit = cand.rbegin(); rit != cand.rend() && checked < 24;
                     ++rit, ++checked) {
                    size_t j = *rit;
                    if (j >= i) continue;
                    size_t maxL = std::min<size_t>({opt.maxPattern, n - i, n - j});
                    if (maxL < opt.minPattern) continue;
                    size_t L = matchLen(events, i, j, maxL);
                    if (L >= opt.minPattern && pitchMode(events, i, L) >= 0 &&
                        pitchMode(events, j, L) >= 0 && L > bestL)
                        bestL = L;
                }
            }
        }
        if (detect && i + 1 + H < n) {
            uint64_t hh = hashSigs(events, i, int(H));
            auto& vec = heads[hh];
            if (vec.size() < 64 || vec.back() != uint32_t(i)) vec.push_back(uint32_t(i));
        }
        if (bestL >= opt.minPattern) {
            // repeat extension: identical blocks spaced by a constant span
            int64_t span = 0;
            uint32_t reps = 1;
            if (i + bestL < n) span = events[i + bestL].timeUs - events[i].timeUs;
            while (i + (size_t(reps) + 1) * bestL <= n) {
                size_t b = i + size_t(reps) * bestL;
                if (reps == 1) {
                    span = events[b].timeUs - events[i].timeUs;
                } else if (events[b].timeUs - events[b - bestL].timeUs != span) {
                    break;
                }
                if (!firstEq(events[i], events[b])) break;
                int pm = pitchMode(events, i, bestL);
                if (pm < 0 || pitchMode(events, b, bestL) != pm) break;
                bool ok = true;
                for (size_t k = 1; k < bestL; ++k)
                    if (!(sigAt(events, i + k) == sigAt(events, b + k))) { ok = false; break; }
                // Repeats share one pitch/lane origin, so pitch and lane must
                // be identical (not just equally shaped) in every block;
                // transposed copies become separate instantiations instead.
                for (size_t k = 0; ok && k < bestL; ++k)
                    if (events[b + k].pitch != events[i + k].pitch ||
                        events[b + k].lane != events[i + k].lane) { ok = false; break; }
                if (!ok) break;
                ++reps;
            }
            if (litStart < i) plans.push_back({litStart, uint32_t(i - litStart), -1, 0, 0});
            int64_t lastRel = events[i + bestL - 1].timeUs - events[i].timeUs;
            int64_t patSpan = reps > 1 ? span : lastRel;
            uint32_t pid = internPattern(i, bestL, patSpan);
            plans.push_back({i, uint32_t(bestL) * reps, int32_t(pid), reps, uint32_t(bestL)});
            i += bestL * reps;
            litStart = i;
            continue;
        }
        ++i;
    }
    if (litStart < n) plans.push_back({litStart, uint32_t(n - litStart), -1, 0, 0});

    // ---- pattern table --------------------------------------------------
    std::vector<PatternRec> patterns(tmpls.size());
    std::vector<char> patSection;
    {
        Buf pb;
        pb.u32(uint32_t(tmpls.size()));
        for (size_t id = 0; id < tmpls.size(); ++id) {
            const Tmpl& t = tmpls[id];
            Buf stream;
            stream.u32(1);  // one op: the literal-relative run
            Buf op;
            writeLiteralRel(op, &events[t.start], t.len, events[t.start].timeUs,
                            t.hasPitch ? events[t.start].pitch : 0,
                            events[t.start].lane);
            stream.raw(op.b.data(), op.b.size());
            pb.u64(uint64_t(t.spanUs));
            pb.u8(t.hasPitch ? 1 : 0);
            pb.u32(uint32_t(stream.b.size()));
            patterns[id].spanUs = uint64_t(t.spanUs);
            patterns[id].hasPitch = t.hasPitch;
            patterns[id].off = uint32_t(pb.b.size());
            patterns[id].len = uint32_t(stream.b.size());
            pb.raw(stream.b.data(), stream.b.size());
        }
        uint32_t crc = crcOf(pb.b.data(), pb.b.size());
        pb.u32(crc);
        patSection = std::move(pb.b);
    }

    // ---- chunk packing --------------------------------------------------
    std::vector<ChunkOut> chunks;
    chunks.push_back(ChunkOut{});
    uint64_t expandedTotal = 0;
    uint64_t literalEvents = 0;

    auto sealChunk = [&]() {
        ChunkOut& c = chunks.back();
        if (c.opCount == 0) return;
        // payload = u32 opCount + ops + u64 expandedCount
        Buf final;
        final.u32(c.opCount);
        final.raw(c.payload.b.data(), c.payload.b.size());
        final.u64(c.expanded);
        c.payload = std::move(final);
        chunks.push_back(ChunkOut{});
    };

    for (const Plan& pl : plans) {
        if (pl.patId < 0) {
            size_t pos = pl.evStart;
            size_t left = pl.evCount;
            while (left) {
                ChunkOut& c = chunks.back();
                size_t room = opt.chunkEvents - std::min<uint64_t>(c.expanded, opt.chunkEvents);
                if (room == 0) { sealChunk(); continue; }
                size_t take = std::min<size_t>(room, left);
                if (c.opCount == 0) {
                    c.firstEvent = expandedTotal;
                    c.firstTimeUs = events[pos].timeUs;
                }
                writeLiteralAbs(c.payload, events.data(), take, pos);
                c.opCount++;
                c.expanded += uint32_t(take);
                c.lastTimeUs = events[pos + take - 1].timeUs;
                expandedTotal += take;
                literalEvents += take;
                pos += take;
                left -= take;
                if (c.expanded >= opt.chunkEvents) sealChunk();
            }
        } else {
            const Tmpl& t = tmpls[size_t(pl.patId)];
            uint32_t perInst = pl.patLen;
            uint32_t repsLeft = pl.repCount;
            int64_t anchor = events[pl.evStart].timeUs;
            while (repsLeft) {
                ChunkOut& c = chunks.back();
                size_t room = opt.chunkEvents - std::min<uint64_t>(c.expanded, opt.chunkEvents);
                uint32_t fit = uint32_t(room / perInst);
                uint32_t use = std::min(fit, repsLeft);
                if (use == 0) {
                    if (c.expanded != 0) { sealChunk(); continue; }
                    use = 1;  // a single instance may exceed the target size
                }
                if (c.opCount == 0) {
                    c.firstEvent = expandedTotal;
                    c.firstTimeUs = anchor;
                }
                // Origins come from THIS run (pl.evStart), not the deduped
                // template: pitch/lane rels are equal by construction, the
                // absolute origins differ per instantiation.
                writeInst(c.payload, uint32_t(pl.patId), use, anchor,
                          t.hasPitch ? events[pl.evStart].pitch : 0,
                          events[pl.evStart].lane);
                c.opCount++;
                c.expanded += use * perInst;
                c.lastTimeUs = anchor + int64_t(use - 1) * t.spanUs + t.lastRelUs;
                expandedTotal += uint64_t(use) * perInst;
                anchor += int64_t(use) * t.spanUs;
                repsLeft -= use;
                if (c.expanded >= opt.chunkEvents) sealChunk();
            }
        }
    }
    sealChunk();
    if (!chunks.empty() && chunks.back().opCount == 0) chunks.pop_back();

    if (expandedTotal != n) {
        err = "internal encoder error: expansion mismatch";
        return false;
    }

    return writeRaidDone(path, patSection, chunks, n, literalEvents,
                         patterns.size(), metaJson, opt, statsOut, err);
}

// ------------------------------------------------------- FL Studio ------

// Largest event count one pattern-instance op may expand to. Chunks reject
// expansions beyond kMaxChunkExpand, so FL patterns are cut into blocks well
// under that bound; each block becomes its own pattern-table definition.
constexpr uint32_t kFlpBlockEvents = 3000000;

bool writeRaidFlp(const std::string& path, const FlpProject& project,
                  const EncodeOptions& opt, Info* statsOut, std::string& err) {
    err.clear();
    if (opt.chunkEvents == 0 || opt.chunkEvents > uint32_t(kMaxChunkExpand) ||
        opt.compressLevel < 1 || opt.compressLevel > 10) {
        err = "invalid encode options";
        return false;
    }
    const double bpm = (project.bpm > 0 ? project.bpm : 120.0) * flpBpmScale(project);
    const double usPerTick = 60000000.0 / (bpm * double(project.ppqn));

    // ---- chart events per FL pattern (pattern-local microseconds) --------
    std::unordered_map<int, std::vector<Event>> patEvents;
    auto eventsFor = [&](int num) -> const std::vector<Event>* {
        auto it = patEvents.find(num);
        if (it != patEvents.end()) return it->second.empty() ? nullptr : &it->second;
        Chart c;
        std::string cerr;
        if (!flpToChart(project, num, c, cerr)) {
            patEvents.emplace(num, std::vector<Event>{});
            return nullptr;
        }
        auto r = patEvents.emplace(num, chartEvents(c));
        return r.first->second.empty() ? nullptr : &r.first->second;
    };

    // ---- clips on the timeline -------------------------------------------
    // A playlist clip shows a WINDOW of its pattern: FL trims clips at
    // record +24 (start offset) so the visible content is pattern-local
    // [offUs, offUs+lenUs), not always [0, len). Clips longer than the
    // pattern's loop period play it cyclically: cycle k is the pattern
    // rotated by (offUs mod period), i.e. pattern note t sounds at clip-local
    // (t - off) mod period + k*period.
    struct Seg {
        int pattern = 0;
        int64_t baseUs = 0;     // absolute time where local loUs maps
        int64_t loUs = 0;       // window start in segment-local pattern time
        int64_t hiUs = 0;       // window end, exclusive
        int track = 0;
        int64_t rotShift = -1;  // >=0: rotated cycle, shift = offUs mod period
    };
    // Loop period per pattern in microseconds: the declared length when
    // the file carries it (event 164), else the content extent rounded up
    // to a whole bar - FL's effective repeat unit for playlist clips.
    const uint64_t barTicks = project.timeSigDen > 0
        ? uint64_t(project.timeSigNum) * uint64_t(project.ppqn) * 4 / uint64_t(project.timeSigDen)
        : 0;
    std::unordered_map<int, uint64_t> patPeriodUs;
    for (const FlpPattern& fp : project.patterns) {
        uint64_t ticks = fp.lengthTicks;
        if (!ticks) {
            uint64_t maxEnd = 0;
            for (const FlpNote& n : fp.notes)
                maxEnd = std::max<uint64_t>(maxEnd, uint64_t(n.time) + uint64_t(n.duration));
            if (maxEnd && barTicks)
                ticks = (maxEnd + barTicks - 1) / barTicks * barTicks;
        }
        if (ticks)
            patPeriodUs[fp.number] = uint64_t(std::llround(double(ticks) * usPerTick));
    }

    std::vector<Seg> segs;
    for (const FlpPlaylistItem& it : project.playlist) {
        if (it.muted || it.pattern <= 0 || it.startTick < 0) continue;
        const int64_t startUs = int64_t(std::llround(it.startTick * usPerTick));
        const uint64_t lenUs = uint64_t(std::llround(it.lengthTicks * usPerTick));
        const int64_t offUs = int64_t(std::llround(double(it.offsetTicks) * usPerTick));
        // A clip longer than its pattern loops in FL Studio: each cycle is a
        // segment playing the pattern rotated by the clip's offset (defs are
        // shared per rotation+window, so the pattern data is still stored once).
        const auto perIt = patPeriodUs.find(it.pattern);
        const uint64_t periodUs = perIt != patPeriodUs.end() ? perIt->second : 0;
        if (periodUs > 0 && lenUs > periodUs) {
            int64_t sMod = offUs % int64_t(periodUs);
            if (sMod < 0) sMod += int64_t(periodUs);
            const int64_t rot = sMod > 0 ? sMod : -1;
            int64_t t = startUs;
            uint64_t rem = lenUs;
            while (rem > 0) {
                const uint64_t w = std::min(rem, periodUs);
                // rot >= 0: rotated cycle, window [0, w) of the rotated events.
                // rot < 0 with lo=0: the unrotated first period (sMod == 0).
                segs.push_back({it.pattern, t, 0, int64_t(w), it.track, rot});
                t += int64_t(periodUs);
                rem -= w;
            }
        } else {
            segs.push_back({it.pattern, startUs, offUs, offUs + int64_t(lenUs), it.track, -1});
        }
    }
    if (segs.empty() && project.playlist.empty()) {
        // No usable arrangement: lay every pattern that charts notes end to end.
        std::vector<const FlpPattern*> order;
        for (const FlpPattern& p : project.patterns)
            if (eventsFor(p.number)) order.push_back(&p);
        std::sort(order.begin(), order.end(),
                  [](const FlpPattern* a, const FlpPattern* b) { return a->number < b->number; });
        int64_t tUs = 0;
        for (const FlpPattern* p : order) {
            const std::vector<Event>& ev = *eventsFor(p->number);
            const int64_t len = int64_t(ev.back().timeUs) + 1;  // content end
            segs.push_back({p->number, tUs, 0, len, 0, -1});
            tUs += len;
        }
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
        if (a.baseUs != b.baseUs) return a.baseUs < b.baseUs;
        if (a.track != b.track) return a.track < b.track;
        return a.pattern < b.pattern;
    });

    // ---- pattern-table definitions ---------------------------------------
    // One def per (pattern, effective window): the pattern's chart events
    // inside the clip's time window, cut into <= kFlpBlockEvents blocks.
    // Clips sharing a pattern and effective length share the same defs.
    struct Def {
        Buf ops;                 // u32 opCount(=1) + one literal-rel op
        uint64_t spanUs = 0;     // last event time - first event time
        int64_t originUs = 0;    // pattern-local time of the first event
        int32_t originLane = 0;  // lane of the first event (instance lane origin)
        uint32_t count = 0;      // expanded events per instance
        uint64_t sideP = 0, sideO = 0;
        const std::vector<Event>* src = nullptr;  // pattern events this block covers
        size_t srcBegin = 0, srcEnd = 0;          // index range inside *src
    };
    std::vector<Def> defs;
    // Defs are shared per (pattern, rotation, window): clips with the same
    // trim window point at the same table entries.
    std::map<std::tuple<int, int64_t, int64_t, int64_t>, std::vector<uint32_t>> winDefs;

    // Rotated views for looping clips: pattern note t -> (t - shift) mod
    // period, re-sorted. std::map nodes keep the vectors' addresses stable
    // for Def::src pointers. shift 0 returns the unrotated events.
    std::map<std::pair<int, int64_t>, std::vector<Event>> rotCache;
    auto rotSrc = [&](int pattern, int64_t shift) -> const std::vector<Event>* {
        const auto perIt = patPeriodUs.find(pattern);
        const int64_t period = perIt != patPeriodUs.end() ? int64_t(perIt->second) : 0;
        if (shift <= 0 || period <= 0) return eventsFor(pattern);
        const auto key = std::make_pair(pattern, shift);
        auto it = rotCache.find(key);
        if (it != rotCache.end()) return it->second.empty() ? nullptr : &it->second;
        const std::vector<Event>* ev = eventsFor(pattern);
        if (!ev) {
            rotCache.emplace(key, std::vector<Event>{});
            return nullptr;
        }
        std::vector<Event> r;
        r.reserve(ev->size());
        for (const Event& e : *ev) {
            Event e2 = e;
            int64_t t = (e.timeUs - shift) % period;
            if (t < 0) t += period;
            e2.timeUs = t;
            r.push_back(e2);
        }
        std::stable_sort(r.begin(), r.end(),
                         [](const Event& a, const Event& b) { return a.timeUs < b.timeUs; });
        auto res = rotCache.emplace(key, std::move(r));
        return res.first->second.empty() ? nullptr : &res.first->second;
    };

    // Index range of events with timeUs in [lo, hi); events are time-sorted.
    auto windowRange = [](const std::vector<Event>& ev, int64_t lo, int64_t hi,
                          size_t& n0, size_t& n1) {
        const auto cmp = [](const Event& e, int64_t t) { return e.timeUs < t; };
        n0 = size_t(std::lower_bound(ev.begin(), ev.end(), lo, cmp) - ev.begin());
        n1 = size_t(std::lower_bound(ev.begin(), ev.end(), hi, cmp) - ev.begin());
    };

    auto defsFor = [&](const Seg& s) -> const std::vector<uint32_t>* {
        const std::vector<Event>* ev =
            s.rotShift >= 0 ? rotSrc(s.pattern, s.rotShift) : eventsFor(s.pattern);
        if (!ev) return nullptr;
        size_t n0 = 0, n1 = 0;
        windowRange(*ev, s.loUs, s.hiUs, n0, n1);
        if (n1 <= n0) return nullptr;
        const auto key = std::make_tuple(s.pattern, s.rotShift, s.loUs, s.hiUs);
        auto it = winDefs.find(key);
        if (it != winDefs.end()) return &it->second;
        std::vector<uint32_t> ids;
        for (size_t b = n0; b < n1; b += kFlpBlockEvents) {
            const size_t e = std::min(b + size_t(kFlpBlockEvents), n1);
            Def d;
            d.count = uint32_t(e - b);
            d.src = ev;
            d.srcBegin = b;
            d.srcEnd = e;
            d.originUs = ev->at(b).timeUs;
            d.originLane = ev->at(b).lane;
            d.spanUs = uint64_t(ev->at(e - 1).timeUs - ev->at(b).timeUs);
            d.ops.u32(1);
            Buf op;
            writeLiteralRel(op, ev->data() + b, e - b, ev->at(b).timeUs, 0, ev->at(b).lane);
            d.ops.raw(op.b.data(), op.b.size());
            for (size_t k = b; k < e; ++k) {
                if (ev->at(k).side == 1) ++d.sideP; else ++d.sideO;
            }
            ids.push_back(uint32_t(defs.size()));
            defs.push_back(std::move(d));
        }
        return &winDefs.emplace(key, std::move(ids)).first->second;
    };

    // Pre-build defs in segment order so pattern ids follow the arrangement.
    for (const Seg& s : segs) defsFor(s);

    // ---- pattern section --------------------------------------------------
    Buf pb;
    pb.u32(uint32_t(defs.size()));
    for (const Def& d : defs) {
        pb.u64(d.spanUs);
        pb.u8(0);  // hasPitch: chart events carry no pitch transforms
        pb.u32(uint32_t(d.ops.b.size()));
        pb.raw(d.ops.b.data(), d.ops.b.size());
    }
    {
        Buf t;
        t.u32(crcOf(pb.b.data(), pb.b.size()));
        pb.raw(t.b.data(), t.b.size());
    }
    std::vector<char> patSection = std::move(pb.b);

    // ---- chunks ------------------------------------------------------------
    // Resolve each clip's defs and last event time; clips with no chart notes
    // drop out. Chunk index entries require non-overlapping, non-decreasing
    // time ranges, so clips are grouped into maximal sets whose event spans
    // overlap: single-clip groups keep pattern-instance ops, overlapping
    // groups merge into one time-sorted literal run (events interleave exactly
    // like the arrangement plays them).
    std::vector<const std::vector<uint32_t>*> segIds(segs.size(), nullptr);
    std::vector<int64_t> segEndUs(segs.size(), 0);
    std::vector<size_t> active;
    for (size_t i = 0; i < segs.size(); ++i) {
        const Seg& s = segs[i];
        const std::vector<uint32_t>* ids = defsFor(s);
        if (!ids || ids->empty()) continue;
        segIds[i] = ids;
        // Absolute end = base - lo + last local event time of each block.
        int64_t end = s.baseUs - s.loUs;
        for (uint32_t id : *ids) {
            const Def& d = defs[id];
            end = std::max(end, s.baseUs - s.loUs + d.originUs + int64_t(d.spanUs));
        }
        segEndUs[i] = end;
        active.push_back(i);
    }

    std::vector<ChunkOut> chunks;
    chunks.push_back(ChunkOut{});
    uint64_t expandedTotal = 0, literalEvents = 0;
    uint64_t playerNotes = 0, opponentNotes = 0;
    int64_t maxEndUs = 0;

    auto sealChunk = [&]() {
        ChunkOut& c = chunks.back();
        if (c.opCount == 0) return;
        Buf final;
        final.u32(c.opCount);
        final.raw(c.payload.b.data(), c.payload.b.size());
        final.u64(c.expanded);
        c.payload = std::move(final);
        chunks.push_back(ChunkOut{});
    };

    auto emitInstance = [&](const Seg& s, uint32_t id) {
        const Def& d = defs[id];
        const int64_t anchor = s.baseUs - s.loUs + d.originUs;
        for (;;) {
            ChunkOut& ch = chunks.back();
            const size_t room = opt.chunkEvents - std::min<uint64_t>(ch.expanded, opt.chunkEvents);
            if (size_t(d.count) <= room || ch.expanded == 0) {
                if (ch.opCount == 0) {
                    ch.firstEvent = expandedTotal;
                    ch.firstTimeUs = anchor;
                }
                writeInst(ch.payload, id, 1, anchor, 0, d.originLane);
                ch.opCount++;
                ch.expanded += d.count;
                ch.lastTimeUs = anchor + int64_t(d.spanUs);
                expandedTotal += d.count;
                literalEvents += d.count;
                playerNotes += d.sideP;
                opponentNotes += d.sideO;
                maxEndUs = std::max(maxEndUs, ch.lastTimeUs);
                return;
            }
            sealChunk();
        }
    };

    size_t g = 0;
    while (g < active.size()) {
        size_t h = g + 1;
        int64_t gEnd = segEndUs[active[g]];
        while (h < active.size() && segs[active[h]].baseUs < gEnd) {
            gEnd = std::max(gEnd, segEndUs[active[h]]);
            ++h;
        }
        if (h - g == 1) {
            const size_t ai = active[g];
            for (uint32_t id : *segIds[ai]) emitInstance(segs[ai], id);
        } else {
            // Overlapping segments: merge their in-window events by absolute
            // time and store them as literal runs.
            std::vector<Event> merged;
            for (size_t x = g; x < h; ++x) {
                const Seg& s = segs[active[x]];
                const std::vector<Event>* ev =
                    s.rotShift >= 0 ? rotSrc(s.pattern, s.rotShift) : eventsFor(s.pattern);
                if (!ev) continue;
                const int64_t shift = s.baseUs - s.loUs;  // seg-local -> absolute
                size_t n0 = 0, n1 = 0;
                windowRange(*ev, s.loUs, s.hiUs, n0, n1);
                merged.reserve(merged.size() + (n1 - n0));
                for (size_t k = n0; k < n1; ++k) {
                    Event e2 = ev->at(k);
                    e2.timeUs += shift;
                    merged.push_back(e2);
                }
            }
            std::stable_sort(merged.begin(), merged.end(),
                             [](const Event& a, const Event& b) { return a.timeUs < b.timeUs; });
            // The merged timeline still decides chunk windows (index time
            // ranges must not overlap), but instead of literal runs each
            // window becomes an op-3 group: every pattern table with events
            // inside it plays at once. Patterns stay separate defs â€” no
            // merged duplicates in the file.
            size_t pos = 0;
            while (pos < merged.size()) {
                ChunkOut& ch = chunks.back();
                const size_t room = opt.chunkEvents - std::min<uint64_t>(ch.expanded, opt.chunkEvents);
                if (room == 0) { sealChunk(); continue; }
                size_t take = std::min(room, merged.size() - pos);
                // A time window cannot split events sharing one timestamp:
                // grow the take to cover the whole same-time run.
                while (pos + take < merged.size() &&
                       merged[pos + take].timeUs == merged[pos + take - 1].timeUs)
                    ++take;
                const int64_t winLo = merged[pos].timeUs;
                const int64_t winHi = pos + take < merged.size()
                                          ? merged[pos + take].timeUs
                                          : INT64_MAX;
                // Every def with at least one event in [winLo, winHi).
                std::vector<GroupEntry> ents;
                for (size_t x = g; x < h; ++x) {
                    const size_t ai = active[x];
                    const Seg& s = segs[ai];
                    const int64_t shift = s.baseUs - s.loUs;
                    for (uint32_t id : *segIds[ai]) {
                        const Def& d = defs[id];
                        const int64_t loLocal = winLo - shift;
                        const int64_t hiLocal = winHi - shift;
                        const std::vector<Event>& ev = *d.src;
                        const auto lb = std::lower_bound(
                            ev.begin() + std::ptrdiff_t(d.srcBegin),
                            ev.begin() + std::ptrdiff_t(d.srcEnd), loLocal,
                            [](const Event& e, int64_t t) { return e.timeUs < t; });
                        const auto ub = std::lower_bound(
                            ev.begin() + std::ptrdiff_t(d.srcBegin),
                            ev.begin() + std::ptrdiff_t(d.srcEnd), hiLocal,
                            [](const Event& e, int64_t t) { return e.timeUs < t; });
                        if (lb == ub) continue;
                        GroupEntry ge;
                        ge.patternId = id;
                        ge.anchorUs = shift + d.originUs;
                        ge.laneDelta = d.originLane;
                        ge.winLoUs = winLo;
                        ge.winHiUs = winHi;
                        ents.push_back(ge);
                    }
                }
                if (ents.empty()) { pos += take; continue; }  // cannot happen
                if (ch.opCount == 0) {
                    ch.firstEvent = expandedTotal;
                    ch.firstTimeUs = winLo;
                }
                writeGroupOp(ch.payload, ents.data(), ents.size());
                ch.opCount++;
                ch.expanded += uint32_t(take);
                ch.lastTimeUs = merged[pos + take - 1].timeUs;
                for (size_t k = pos; k < pos + take; ++k)
                    if (merged[k].side == 1) ++playerNotes; else ++opponentNotes;
                expandedTotal += take;
                literalEvents += take;
                maxEndUs = std::max(maxEndUs, ch.lastTimeUs);
                pos += take;
                if (ch.expanded >= opt.chunkEvents) sealChunk();
            }
        }
        g = h;
    }
    sealChunk();

    if (expandedTotal == 0) {
        err = "no chart notes in the project";
        return false;
    }

    // ---- meta -------------------------------------------------------------
    Chart meta;
    meta.title = project.title;
    meta.keys = 4;
    meta.speed = std::min(6.0, std::max(0.5, bpm / 50.0));
    meta.tempo.push_back({0.0, bpm, 0.0});
    meta.focus.push_back({0.0, 1});
    const double songMs = double(maxEndUs) / 1000.0;
    const double sectionMs = 4.0 * 60000.0 / bpm;
    for (double t = 0; t < songMs; t += sectionMs)
        meta.sections.push_back({t, true, false, false, 4.0, bpm});
    meta.lengthMs = songMs;
    meta.playerNotes = playerNotes;
    meta.opponentNotes = opponentNotes;
    meta.rawNoteCount = expandedTotal;
    const std::string mj = chartMetaJson(meta);

    return writeRaidDone(path, patSection, chunks, expandedTotal, literalEvents,
                         defs.size(), mj, opt, statsOut, err);
}

// ------------------------------------------------------------ reading -----

namespace {

// Counting semaphore with a resizable permit count (decodes in flight).
struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    int total = 1, inUse = 0;
    explicit Semaphore(int n) : total(n) {}
    void setTotal(int n) {
        std::lock_guard<std::mutex> lk(m);
        total = n;
        if (n > inUse) cv.notify_all();
    }
    void acquire() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return inUse < total; });
        ++inUse;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m);
            --inUse;
        }
        cv.notify_one();
    }
};

struct DecodeGuard {
    Semaphore& s;
    explicit DecodeGuard(Semaphore& sem) : s(sem) { s.acquire(); }
    ~DecodeGuard() { s.release(); }
    DecodeGuard(const DecodeGuard&) = delete;
};

}  // namespace

struct Reader::Impl {
    FILE* f_ = nullptr;
    std::string path_;
    Info info_;
    std::vector<ChunkEnt> chunks_;
    // meta / pattern section locations (lazy)
    uint64_t metaOff_ = 0, metaStored_ = 0, metaRaw_ = 0;
    uint64_t patOff_ = 0, patStored_ = 0, patRaw_ = 0;

    std::atomic<bool> patternsLoaded_{false};
    std::mutex patMutex_;
    std::vector<char> patternBlob_;
    std::vector<PatternRec> patterns_;
    std::vector<uint8_t> patHasInst_;  // pattern ops contain nested instances

    // FILE* has one shared seek position: every seek+read is serialized here.
    // Decompression and pattern expansion run outside this lock.
    std::mutex ioMutex_;
    std::mutex cacheMutex_;
    std::vector<Cached> cache_;
    uint64_t cacheStamp_ = 0;
    uint64_t cacheEvents_ = 0, cacheCapEvents_ = 0;

    Semaphore decSem_;
    int decTotal_ = 2;
    uint64_t memBudget_ = 128ull << 20;   // default 128 MiB
    uint64_t maxChunkEvents_ = 0;

    Impl() : decSem_(2) { recomputeLimits(); }
    ~Impl() { if (f_) std::fclose(f_); }

    void recomputeLimits() {
        // A single chunk decode may materialize at most this many events; the
        // LRU cache as a whole may hold at most memBudget_ worth of events.
        maxChunkEvents_ = std::max<uint64_t>(1, std::min<uint64_t>(
            kMaxChunkExpand, memBudget_ / sizeof(Event)));
        cacheCapEvents_ = std::max<uint64_t>(1, memBudget_ / sizeof(Event));
        decSem_.setTotal(decTotal_);
    }

    // seek+read under ioMutex_; inflate happens in the caller, unlocked.
    bool readRaw(uint64_t off, uint64_t size, std::vector<char>& out,
                 std::string& err) {
        out.resize(size);
        if (size == 0) return true;
        std::lock_guard<std::mutex> lk(ioMutex_);
#if defined(_WIN32)
        if (_fseeki64(f_, (long long)off, SEEK_SET) != 0) { err = "cannot seek"; return false; }
#else
        if (fseeko(f_, off_t(off), SEEK_SET) != 0) { err = "cannot seek"; return false; }
#endif
        if (std::fread(out.data(), 1, size, f_) != size) {
            err = "cannot read section";
            return false;
        }
        return true;
    }

    bool readSection(uint64_t off, uint64_t storedSize, uint64_t rawSize,
                     bool compressed, std::vector<char>& out, std::string& err) {
        out.clear();
        if (!f_ || rawSize > (512ull << 20) || (compressed && storedSize > (512ull << 20))) {
            err = "section too large";
            return false;
        }
        if (compressed) {
            std::vector<char> src;
            if (!readRaw(off, storedSize, src, err)) return false;
            if (!inflate(src.data(), src.size(), size_t(rawSize), out)) {
                err = "section decompression failed";
                return false;
            }
        } else {
            if (!readRaw(off, rawSize, out, err)) return false;
        }
        return true;
    }

    bool readHeader(std::string& err) {
        unsigned char h[kHeaderSize];
        if (std::fread(h, 1, kHeaderSize, f_) != kHeaderSize) { err = "short raid header"; return false; }
        if (std::memcmp(h, kMagic, 4) != 0) { err = "not a raid file"; return false; }
        Cur c{reinterpret_cast<const char*>(h), reinterpret_cast<const char*>(h) + kHeaderSize};
        c.skip(4);
        info_.version = uint32_t(c.u32());
        info_.flags = uint32_t(c.u32());
        c.u32();  // reserved
        c.u32();  // reserved
        info_.logicalEvents = c.u64();
        info_.chunkCount = c.u64();
        info_.patternCount = c.u64();
        uint64_t indexOff = c.u64(), indexSize = c.u64();
        metaOff_ = c.u64(); metaStored_ = c.u64(); metaRaw_ = c.u64();
        patOff_ = c.u64(); patStored_ = c.u64(); patRaw_ = c.u64();
        uint32_t storedCrc = uint32_t(c.u32());
        c.u32();
        if (!c.ok) { err = "bad raid header"; return false; }
        if (info_.version != kVersion) {
            err = "unsupported raid version " + std::to_string(info_.version);
            return false;
        }
        unsigned char hz[kHeaderSize];
        std::memcpy(hz, h, kHeaderSize);
        hz[108] = hz[109] = hz[110] = hz[111] = 0;
        if (crcOf(hz, kHeaderSize) != storedCrc) { err = "raid header checksum mismatch"; return false; }
        if (info_.chunkCount > (64ull << 20) || info_.patternCount > (1ull << 24)) {
            err = "absurd section count";
            return false;
        }
        if (indexSize != info_.chunkCount * kIndexEntrySize) { err = "bad chunk index size"; return false; }
        // The index must fit inside the file.
#if defined(_WIN32)
        if (_fseeki64(f_, 0, SEEK_END)) { err = "cannot seek"; return false; }
        const uint64_t fileSize = uint64_t(_ftelli64(f_));
#else
        if (fseeko(f_, 0, SEEK_END)) { err = "cannot seek"; return false; }
        const uint64_t fileSize = uint64_t(ftello(f_));
#endif
        if (indexOff > fileSize || indexSize > fileSize - indexOff ||
            metaOff_ > fileSize || patOff_ > fileSize) {
            err = "raid sections outside file";
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(ioMutex_);
#if defined(_WIN32)
            if (_fseeki64(f_, (long long)indexOff, SEEK_SET)) { err = "cannot seek"; return false; }
#else
            if (fseeko(f_, off_t(indexOff), SEEK_SET)) { err = "cannot seek"; return false; }
#endif
        }
        std::vector<char> idx(indexSize);
        {
            std::lock_guard<std::mutex> lk(ioMutex_);
            if (indexSize && std::fread(idx.data(), 1, indexSize, f_) != indexSize) {
                err = "cannot read chunk index";
                return false;
            }
        }
        Cur ic{idx.data(), idx.data() + idx.size()};
        chunks_.resize(size_t(info_.chunkCount));
        for (auto& e : chunks_) {
            e.firstEvent = ic.u64();
            e.firstTimeUs = ic.i64();
            e.lastTimeUs = ic.i64();
            e.offset = ic.u64();
            e.storedSize = uint32_t(ic.u32());
            e.rawSize = uint32_t(ic.u32());
            e.expanded = uint32_t(ic.u32());
            e.opCount = uint32_t(ic.u32());
            e.crc = uint32_t(ic.u32());
            e.flags = uint32_t(ic.u32());
            if (!ic.ok) { err = "truncated chunk index"; return false; }
        }
        // The index must describe a real chunk sequence: every chunk expands to
        // a bounded number of events, times are sane, offsets point inside the
        // file, and chunk time ranges are non-decreasing (range() binary-
        // searches and early-outs on that invariant).
        int64_t prevLast = INT64_MIN;
        for (const ChunkEnt& e : chunks_) {
            if (e.expanded == 0 || e.expanded > uint32_t(kMaxChunkExpand) ||
                e.lastTimeUs < e.firstTimeUs || e.firstTimeUs < prevLast ||
                e.offset > fileSize || e.firstTimeUs < 0) {
                err = "bad chunk index entry";
                return false;
            }
            prevLast = e.lastTimeUs;
        }
        return true;
    }

    // Parses the pattern table. Caller must hold patMutex_. The loaded flag
    // is only published after this returns, so concurrent decodes never see
    // a half-filled patterns_/patHasInst_.
    bool loadPatternsImpl(std::string& err) {
        if (patRaw_ == 0) { patterns_.clear(); patHasInst_.clear(); return true; }
        if (!readSection(patOff_, patStored_, patRaw_, patStored_ != 0, patternBlob_, err))
            return false;
        const size_t n = patternBlob_.size();
        if (n < 8) { err = "bad pattern section"; return false; }
        uint32_t want = getU32(reinterpret_cast<const unsigned char*>(patternBlob_.data() + n - 4));
        if (crcOf(patternBlob_.data(), n - 4) != want) { err = "pattern checksum mismatch"; return false; }
        Cur c{patternBlob_.data(), patternBlob_.data() + n - 4};
        uint32_t count = uint32_t(c.u32());
        if (!c.ok || count != info_.patternCount || count > (1u << 24)) {
            err = "bad pattern count";
            return false;
        }
        patterns_.clear();
        patterns_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            PatternRec p;
            p.spanUs = uint64_t(c.u64());
            p.hasPitch = (c.u8() & 1) != 0;
            uint32_t opsLen = uint32_t(c.u32());
            if (!c.ok || opsLen < 4 || opsLen > n ||
                size_t(c.p - patternBlob_.data()) + opsLen > n - 4) {
                err = "bad pattern record";
                return false;
            }
            p.off = uint32_t(c.p - patternBlob_.data());
            p.len = opsLen;
            c.skip(opsLen);
            patterns_.push_back(p);
        }
        if (!c.ok || c.p != patternBlob_.data() + n - 4) { err = "trailing pattern data"; return false; }
        // Flag patterns whose op stream nests further instances; the op-log
        // walker uses this to emit only leaf segments. A pattern whose ops
        // fail to parse is flagged non-leaf â€” decoding it errors properly.
        patHasInst_.assign(patterns_.size(), 0);
        for (size_t i = 0; i < patterns_.size(); ++i) {
            const PatternRec& p = patterns_[i];
            Cur pc{patternBlob_.data() + p.off, patternBlob_.data() + p.off + p.len};
            bool found = false;
            std::string scanErr;
            if (!opsContainInst(pc, found, scanErr))
                patHasInst_[i] = 1;
            else
                patHasInst_[i] = found ? 1 : 0;
        }
        return true;
    }

    bool loadPatterns(std::string& err) {
        if (patternsLoaded_.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lk(patMutex_);
        if (patternsLoaded_.load(std::memory_order_relaxed)) return true;
        // Done once, win or fail: a failed parse stays failed rather than
        // being retried by every later decode.
        const bool ok = loadPatternsImpl(err);
        patternsLoaded_.store(true, std::memory_order_release);
        return ok;
    }

    // Decodes one chunk's expanded events, appending to out. Serialized by
    // decSem_ so at most decTotal_ decodes (and their allocations) are live.
    bool decodeChunk(const ChunkEnt& e, std::vector<Event>& out, std::string& err) {
        if (e.expanded > maxChunkEvents_) {
            err = "chunk expands beyond the memory budget";
            return false;
        }
        DecodeGuard guard(decSem_);
        std::vector<char> payload;
        const bool compressed = (e.flags & 1) != 0;
        if (!readSection(e.offset, compressed ? e.storedSize : 0, e.rawSize, compressed, payload, err))
            return false;
        if (payload.size() < 12 || crcOf(payload.data(), payload.size()) != e.crc) {
            err = "chunk checksum mismatch";
            return false;
        }
        if (!loadPatterns(err)) return false;

        const size_t before = out.size();
        Cur cur{payload.data(), payload.data() + payload.size()};
        Env env{&patternBlob_, &patterns_};
        Ctx ctx{};
        EmitOut eo{&out, 0, e.expanded};
        if (!emitOps(cur, env, nullptr, ctx, 0, eo, err)) {
            out.resize(before);
            return false;
        }
        // trailer: u64 expandedCount
        uint64_t declared = cur.u64();
        if (!cur.ok || cur.p != cur.end || declared != eo.produced || eo.produced != e.expanded) {
            out.resize(before);
            err = "chunk event count mismatch";
            return false;
        }
        return true;
    }

    // Walks a chunk's op stream without materializing events, recording
    // leaf segments (literals and innermost pattern instances) via walkOps.
    // Pattern identity only survives here â€” expanded events lose it.
    bool chunkOps(const ChunkEnt& e, size_t chunkIdx, std::vector<Reader::OpSegment>& out,
                  std::string& err) {
        DecodeGuard guard(decSem_);
        std::vector<char> payload;
        const bool compressed = (e.flags & 1) != 0;
        if (!readSection(e.offset, compressed ? e.storedSize : 0, e.rawSize, compressed, payload, err))
            return false;
        if (payload.size() < 12 || crcOf(payload.data(), payload.size()) != e.crc) {
            err = "chunk checksum mismatch";
            return false;
        }
        if (!loadPatterns(err)) return false;

        Cur cur{payload.data(), payload.data() + payload.size()};
        Env env{&patternBlob_, &patterns_, &patHasInst_};
        Ctx ctx{};
        WalkOut wo{&out, uint32_t(chunkIdx), 0, e.expanded, 0};
        if (!walkOps(cur, env, ctx, 0, wo, true, err)) return false;
        // trailer: u64 expandedCount â€” same validation as decodeChunk.
        uint64_t declared = cur.u64();
        if (!cur.ok || cur.p != cur.end || declared != wo.produced ||
            wo.produced != e.expanded) {
            err = "chunk event count mismatch";
            return false;
        }
        return true;
    }

    // LRU-cached chunk decode; safe to call from any thread.
    bool chunkEvents(size_t i, std::vector<Event>& out, std::string& err) {
        if (i >= chunks_.size()) { err = "chunk index out of range"; return false; }
        {
            std::lock_guard<std::mutex> lk(cacheMutex_);
            for (auto& c : cache_)
                if (c.index == i) {
                    out.insert(out.end(), c.events.begin(), c.events.end());
                    c.stamp = ++cacheStamp_;
                    return true;
                }
        }
        Cached c;
        c.index = i;
        c.stamp = ++cacheStamp_;
        if (!decodeChunk(chunks_[i], c.events, err)) return false;
        out.insert(out.end(), c.events.begin(), c.events.end());
        {
            std::lock_guard<std::mutex> lk(cacheMutex_);
            // Another thread may have cached this chunk while we decoded.
            for (auto& e : cache_)
                if (e.index == i) return true;
            // Bound the cache by event count, not just entry count: evict LRU
            // until the new chunk fits the memory budget.
            while (!cache_.empty() &&
                   (cache_.size() >= 8 || cacheEvents_ + c.events.size() > cacheCapEvents_)) {
                auto oldest = std::min_element(cache_.begin(), cache_.end(),
                                               [](const Cached& a, const Cached& b) { return a.stamp < b.stamp; });
                cacheEvents_ -= oldest->events.size();
                *oldest = std::move(cache_.back());
                cache_.pop_back();
            }
            cacheEvents_ += c.events.size();
            cache_.push_back(std::move(c));
        }
        return true;
    }
};

// ------------------------------------------------------------ public API ---

bool Reader::open(const std::string& path, std::string& err) {
    close();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    auto p = std::make_shared<Impl>();
    p->decTotal_ = decTotal_;
    p->memBudget_ = memBudget_;
    p->recomputeLimits();
    p->f_ = f;
    p->path_ = path;
    if (!p->readHeader(err)) { std::fclose(f); return false; }
    impl_ = std::move(p);
    return true;
}

void Reader::close() {
    impl_.reset();
}

Reader::~Reader() { close(); }

const Info& Reader::info() const {
    static const Info kEmpty;
    return impl_ ? impl_->info_ : kEmpty;
}

const std::string& Reader::path() const {
    static const std::string kEmpty;
    return impl_ ? impl_->path_ : kEmpty;
}

bool Reader::isOpen() const { return impl_ && impl_->f_ != nullptr; }

void Reader::setMaxConcurrentDecodes(int n) {
    decTotal_ = std::max(1, n);
    if (impl_) { impl_->decTotal_ = decTotal_; impl_->decSem_.setTotal(decTotal_); }
}

void Reader::setMemoryBudgetBytes(uint64_t bytes) {
    memBudget_ = bytes;
    if (impl_) { impl_->memBudget_ = bytes; impl_->recomputeLimits(); }
}

uint64_t Reader::memoryBudgetBytes() const {
    return impl_ ? impl_->memBudget_ : memBudget_;
}

std::string Reader::metaJson() {
    if (!impl_ || impl_->metaRaw_ == 0) return "";
    Impl& im = *impl_;
    std::vector<char> blob;
    std::string err;
    if (!im.readSection(im.metaOff_, im.metaStored_, im.metaRaw_, im.metaStored_ != 0, blob, err))
        return "";
    if (blob.size() < 4) return "";
    uint32_t want = getU32(reinterpret_cast<const unsigned char*>(blob.data() + blob.size() - 4));
    if (crcOf(blob.data(), blob.size() - 4) != want) return "";
    return std::string(blob.data(), blob.size() - 4);
}

int64_t Reader::chunkAtOrBefore(int64_t timeUs) const {
    if (!impl_ || impl_->chunks_.empty()) return -1;
    const auto& chunks = impl_->chunks_;
    size_t lo = 0, hi = chunks.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (chunks[mid].firstTimeUs <= timeUs) lo = mid + 1; else hi = mid;
    }
    return int64_t(lo) - 1;
}

bool Reader::chunkEvents(size_t i, std::vector<Event>& out, std::string& err) {
    if (!impl_) { err = "not open"; return false; }
    return impl_->chunkEvents(i, out, err);
}

bool Reader::range(int64_t startUs, int64_t endUs, std::vector<Event>& out,
                   std::string& err, std::vector<uint64_t>* indices) {
    if (!impl_) { err = "not open"; return false; }
    Impl& im = *impl_;
    if (endUs <= startUs || im.chunks_.empty()) return true;
    const auto& chunks = im.chunks_;
    // first chunk whose lastTimeUs >= startUs (lastTimeUs is non-decreasing,
    // enforced by readHeader)
    size_t lo = 0, hi = chunks.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (chunks[mid].lastTimeUs >= startUs) hi = mid; else lo = mid + 1;
    }
    std::vector<Event> buf;
    for (size_t i = lo; i < chunks.size() && chunks[i].firstTimeUs < endUs; ++i) {
        buf.clear();
        if (!im.chunkEvents(i, buf, err)) return false;
        // buf holds the chunk's expanded events in order: position k has the
        // global index chunks[i].firstEvent + k.
        for (size_t k = 0; k < buf.size(); ++k) {
            const Event& e = buf[k];
            if (e.timeUs >= endUs) break;
            if (e.timeUs >= startUs) {
                out.push_back(e);
                if (indices) indices->push_back(chunks[i].firstEvent + k);
            }
        }
    }
    return true;
}

bool Reader::opLog(int64_t startUs, int64_t endUs, std::vector<OpSegment>& out,
                   std::string& err) {
    if (!impl_) { err = "not open"; return false; }
    Impl& im = *impl_;
    if (endUs <= startUs || im.chunks_.empty()) return true;
    const auto& chunks = im.chunks_;
    size_t lo = 0, hi = chunks.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (chunks[mid].lastTimeUs >= startUs) hi = mid; else lo = mid + 1;
    }
    for (size_t i = lo; i < chunks.size() && chunks[i].firstTimeUs < endUs; ++i)
        if (!im.chunkOps(chunks[i], i, out, err)) return false;
    return true;
}

bool Reader::chunkTimeRange(size_t i, int64_t& firstUs, int64_t& lastUs) const {
    if (!impl_ || i >= impl_->chunks_.size()) return false;
    firstUs = impl_->chunks_[i].firstTimeUs;
    lastUs = impl_->chunks_[i].lastTimeUs;
    return true;
}

int64_t Reader::durationUs() const {
    if (!impl_ || impl_->chunks_.empty()) return 0;
    return impl_->chunks_.back().lastTimeUs;
}

std::future<bool> Reader::chunkEventsAsync(size_t i, std::vector<Event>* out) {
    // The worker holds its own reference to the Impl, so destroying the
    // Reader cannot free the file state mid-decode.
    std::shared_ptr<Impl> im = impl_;
    if (!im || !out) return std::async(std::launch::deferred, [] { return false; });
    return std::async(std::launch::async, [im, i, out]() {
        std::string err;
        return im->chunkEvents(i, *out, err);
    });
}

// -------------------------------------------------------- pattern log ----

bool PatternLogger::open(Reader& rd, const std::string& path, std::string& err) {
    close();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open pattern log file"; return false; }
    rd_ = &rd;
    f_ = f;
    path_ = path;
    winStartUs_ = 0;
    prevHorizonUs_ = -1;
    written_.clear();
    prevPatId_ = -1;
    instances_ = changes_ = 0;
    std::fprintf(f_, "# RAID pattern log\n");
    return true;
}

void PatternLogger::close() {
    if (f_) {
        std::string err;
        (void)tick(INT64_MAX, err);  // flush any tail the playhead never reached
        std::fprintf(f_, "# end: %llu pattern instances, %llu pattern->pattern changes\n",
                     (unsigned long long)instances_, (unsigned long long)changes_);
        std::fclose(f_);
        f_ = nullptr;
    }
    rd_ = nullptr;
    written_.clear();
}

bool PatternLogger::tick(int64_t horizonUs, std::string& err) {
    if (!f_) return true;
    if (horizonUs <= prevHorizonUs_) return true;
    std::vector<Reader::OpSegment> segs;
    // Only chunks overlapping the new stretch are walked; the boundary
    // chunk is re-read but its already-logged segments are skipped by the
    // per-chunk ordinal in written_.
    if (!rd_->opLog(winStartUs_, horizonUs, segs, err)) return false;
    uint32_t lastChunk = UINT32_MAX, pos = 0;
    for (const Reader::OpSegment& s : segs) {
        // Segments are emitted in time order; the first one starting at
        // or past the horizon means the rest of the log is still future.
        if (s.startUs >= horizonUs) break;
        if (s.chunk != lastChunk) { lastChunk = s.chunk; pos = 0; }
        uint32_t& w = written_[s.chunk];
        ++pos;
        if (pos <= w) continue;
        w = pos;
        if (s.kind == 2) {
            ++instances_;
            const bool change = prevPatId_ >= 0 && int64_t(s.count) != prevPatId_;
            if (change) {
                ++changes_;
                std::fprintf(f_, ">>> pattern change #%lld -> #%u\n",
                             (long long)prevPatId_, s.count);
            }
            prevPatId_ = int64_t(s.count);
            std::fprintf(f_,
                         "pat#%-6u x%-4u chunk %-3u [%10.1f - %10.1f ms]  pitch%+d lane%+d\n",
                         s.count, s.aux, s.chunk, double(s.startUs) / 1000.0,
                         double(s.endUs) / 1000.0, s.pitchDelta, s.laneDelta);
        } else {
            prevPatId_ = -1;  // literals break pattern-to-pattern adjacency
            std::fprintf(f_, "lit      chunk %-3u [%10.1f - %10.1f ms]  %u events\n",
                         s.chunk, double(s.startUs) / 1000.0,
                         double(s.endUs) / 1000.0, s.count);
        }
    }
    winStartUs_ = horizonUs;
    prevHorizonUs_ = horizonUs;
    return true;
}

}  // namespace gh::raid
