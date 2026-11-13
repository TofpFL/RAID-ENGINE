// Chart <-> RAID bridge. See raid_chart.hpp.
#include <chrono>
#include <cmath>
#include <cstdio>

#include "json.hpp"
#include "raid_chart.hpp"

namespace gh::raid {

namespace {

std::string jsonEscape(std::string_view s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (uint8_t(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    r += buf;
                } else {
                    r += c;
                }
        }
    }
    return r;
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// Builds the meta JSON stored in the file. `stats` may be null while
// encoding (stats are only known after writeRaid); the caller patches them
// via Info afterwards — the stored stats block is best-effort.
std::string metaJson(const Chart& c, const Info* stats) {
    std::string j = "{\"raid\":1,\"title\":\"" + jsonEscape(c.title) +
                    "\",\"difficulty\":\"" + jsonEscape(c.difficulty) +
                    "\",\"opponent\":\"" + jsonEscape(c.opponent) +
                    "\",\"player\":\"" + jsonEscape(c.player) +
                    "\",\"gf\":\"" + jsonEscape(c.gf) +
                    "\",\"stage\":\"" + jsonEscape(c.stage) + "\",";
    j += "\"keys\":" + std::to_string(c.keys) + ",\"speed\":" + num(c.speed) + ",";
    j += "\"noteTypes\":[";
    for (size_t i = 0; i < c.noteTypes.size(); ++i) {
        if (i) j += ",";
        j += "\"" + jsonEscape(c.noteTypes[i]) + "\"";
    }
    j += "],\"tempo\":[";
    for (size_t i = 0; i < c.tempo.size(); ++i) {
        if (i) j += ",";
        j += "{\"timeMs\":" + num(c.tempo[i].timeMs) + ",\"bpm\":" + num(c.tempo[i].bpm) +
             ",\"beat\":" + num(c.tempo[i].beat) + "}";
    }
    j += "],\"focus\":[";
    for (size_t i = 0; i < c.focus.size(); ++i) {
        if (i) j += ",";
        j += "{\"timeMs\":" + num(c.focus[i].timeMs) + ",\"side\":" +
             std::to_string(c.focus[i].side) + "}";
    }
    j += "],\"sections\":[";
    for (size_t i = 0; i < c.sections.size(); ++i) {
        if (i) j += ",";
        const SectionInfo& s = c.sections[i];
        j += "{\"timeMs\":" + num(s.timeMs) +
             ",\"mustHit\":" + (s.mustHit ? "true" : "false") +
             ",\"gf\":" + (s.gf ? "true" : "false") +
             ",\"alt\":" + (s.alt ? "true" : "false") +
             ",\"beats\":" + num(s.beats) + ",\"bpm\":" + num(s.bpm) + "}";
    }
    j += "],\"lengthMs\":" + std::to_string(c.lengthMs) +
         ",\"playerNotes\":" + std::to_string(c.playerNotes) +
         ",\"opponentNotes\":" + std::to_string(c.opponentNotes) +
         ",\"stats\":{";
    if (stats) {
        j += "\"logicalEvents\":" + std::to_string(stats->logicalEvents) +
             ",\"chunks\":" + std::to_string(stats->chunkCount) +
             ",\"patterns\":" + std::to_string(stats->patternCount) +
             ",\"literalEvents\":" + std::to_string(stats->literalEvents) +
             ",\"storedBytes\":" + std::to_string(stats->storedBytes) +
             ",\"rawBytes\":" + std::to_string(stats->rawBytes) +
             ",\"codec\":\"deflate\",\"encoder\":\"godshand-raid/1\"";
    } else {
        j += "\"codec\":\"deflate\",\"encoder\":\"godshand-raid/1\"";
    }
    j += "}}";
    return j;
}

Note eventToNote(const Event& e) {
    Note n;
    n.time = double(e.timeUs) / 1000.0;
    n.length = double(e.lengthUs) / 1000.0;
    n.lane = e.lane;
    n.side = e.side;
    n.kind = e.kind == 1 ? kMine : kNormal;
    n.flags = e.flags;
    n.type = e.type;
    n.stack = e.stack ? e.stack : 1;
    return n;
}

}  // namespace

std::vector<Event> chartEvents(const Chart& c) {
    std::vector<Event> out;
    out.reserve(c.notes.size());
    for (const Note& n : c.notes) {
        Event e;
        e.timeUs = int64_t(std::llround(n.time * 1000.0));
        e.lengthUs = uint32_t(std::llround(std::max(0.0, n.length) * 1000.0));
        e.pitch = -1;
        e.lane = n.lane;
        e.side = n.side;
        e.kind = n.kind == kMine ? 1 : 0;
        e.flags = n.flags;
        e.type = n.type;
        e.stack = n.stack ? n.stack : 1;
        out.push_back(e);
    }
    return out;
}

bool saveChart(const std::string& path, const Chart& chart,
               const EncodeOptions& opt, Info* statsOut, std::string& err) {
    // Two passes would be needed to embed final stats in meta; instead write
    // once with best-effort stats, then rewrite meta is overkill — the stats
    // block is informational. We do a cheap pre-pass: encode once to learn
    // sizes? writeRaid already reports them; meta stats stay approximate.
    std::vector<Event> ev = chartEvents(chart);
    return writeRaid(path, ev, metaJson(chart, nullptr), opt, statsOut, err);
}

bool chartFromMeta(const std::string& meta, Chart& out, Info& info) {
    json::Document d;
    std::string err;
    if (!d.parse(meta, &err)) return false;
    const json::Value& r = d.root();
    if (d.get(r, "raid") && json::Document::num(d.get(r, "raid"), 0) < 1) return false;
    out.title = std::string(d.strOr(d.get(r, "title"), ""));
    out.difficulty = std::string(d.strOr(d.get(r, "difficulty"), ""));
    out.opponent = std::string(d.strOr(d.get(r, "opponent"), ""));
    out.player = std::string(d.strOr(d.get(r, "player"), ""));
    out.gf = std::string(d.strOr(d.get(r, "gf"), ""));
    out.stage = std::string(d.strOr(d.get(r, "stage"), ""));
    out.keys = int(json::Document::num(d.get(r, "keys"), 4));
    out.speed = json::Document::num(d.get(r, "speed"), 2.0);
    out.lengthMs = json::Document::num(d.get(r, "lengthMs"), 0);
    out.playerNotes = uint64_t(json::Document::num(d.get(r, "playerNotes"), 0));
    out.opponentNotes = uint64_t(json::Document::num(d.get(r, "opponentNotes"), 0));
    if (const json::Value* nt = d.get(r, "noteTypes"))
        for (size_t i = 0; i < d.size(*nt); ++i) out.noteTypes.push_back(std::string(d.str(d.at(*nt, i))));
    if (out.noteTypes.empty()) out.noteTypes.push_back("");
    if (const json::Value* tp = d.get(r, "tempo"))
        for (size_t i = 0; i < d.size(*tp); ++i) {
            const json::Value& t = d.at(*tp, i);
            out.tempo.push_back({json::Document::num(d.get(t, "timeMs"), 0),
                                 json::Document::num(d.get(t, "bpm"), 120),
                                 json::Document::num(d.get(t, "beat"), 0)});
        }
    if (const json::Value* fo = d.get(r, "focus"))
        for (size_t i = 0; i < d.size(*fo); ++i) {
            const json::Value& t = d.at(*fo, i);
            out.focus.push_back({json::Document::num(d.get(t, "timeMs"), 0),
                                 uint8_t(json::Document::num(d.get(t, "side"), 0))});
        }
    if (const json::Value* se = d.get(r, "sections"))
        for (size_t i = 0; i < d.size(*se); ++i) {
            const json::Value& s = d.at(*se, i);
            out.sections.push_back({json::Document::num(d.get(s, "timeMs"), 0),
                                    json::Document::boolean(d.get(s, "mustHit"), true),
                                    json::Document::boolean(d.get(s, "gf"), false),
                                    json::Document::boolean(d.get(s, "alt"), false),
                                    json::Document::num(d.get(s, "beats"), 4),
                                    json::Document::num(d.get(s, "bpm"), 0)});
        }
    return true;
}

bool loadChart(const std::string& path, Chart& out, std::string& err) {
    Reader r;
    if (!r.open(path, err)) return false;
    const Info& info = r.info();
    // Refuse charts that cannot be materialized; streaming callers use
    // Reader::range directly.
    if (info.logicalEvents > (400ull << 20)) {
        err = "raid chart has " + std::to_string(info.logicalEvents) +
              " events; too large to materialize (use the streaming reader)";
        return false;
    }
    std::string meta = r.metaJson();
    Info minfo;
    minfo.version = info.version;
    minfo.logicalEvents = info.logicalEvents;
    minfo.chunkCount = info.chunkCount;
    minfo.patternCount = info.patternCount;
    if (!meta.empty()) {
        Chart mc;
        if (!chartFromMeta(meta, mc, minfo)) { err = "bad raid metadata"; return false; }
        out = std::move(mc);
    } else {
        out = Chart{};
    }
    out.rawNoteCount = info.logicalEvents;
    out.notes.reserve(size_t(std::min<uint64_t>(info.logicalEvents, 16ull << 20)));
    std::vector<Event> buf;
    for (size_t i = 0; i < info.chunkCount; ++i) {
        buf.clear();
        if (!r.chunkEvents(i, buf, err)) return false;
        for (const Event& e : buf) out.notes.push_back(eventToNote(e));
    }
    // finalize re-sorts, recomputes tempo beats/stats and merges any stacked
    // duplicates a foreign file may contain; it is idempotent on our own
    // output because events are already stored merged.
    double bpm = out.tempo.empty() ? 120 : out.tempo.front().bpm;
    out.finalize(bpm > 0 ? bpm : 120);
    return true;
}

// ------------------------------------------------------- streaming --------

bool ChartStream::open(const std::string& path, Chart& meta, std::string& err) {
    close();
    if (!reader_.open(path, err)) return false;
    const Info& info = reader_.info();
    meta = Chart{};
    std::string mj = reader_.metaJson();
    if (!mj.empty()) {
        Info minfo;
        minfo.version = info.version;
        minfo.logicalEvents = info.logicalEvents;
        minfo.chunkCount = info.chunkCount;
        minfo.patternCount = info.patternCount;
        Chart mc;
        if (!chartFromMeta(mj, mc, minfo)) { err = "bad raid metadata"; return false; }
        meta = std::move(mc);
    }
    meta.rawNoteCount = info.logicalEvents;
    if (meta.lengthMs <= 0) meta.lengthMs = double(reader_.durationUs()) / 1000.0;
    if (meta.keys <= 0) meta.keys = 4;
    if (meta.noteTypes.empty()) meta.noteTypes.push_back("");
    chart_ = meta;
    return true;
}

void ChartStream::close() {
    harvestPrefetch();
    reader_.close();
    chart_ = Chart{};
}

bool ChartStream::pump(int64_t startUs, int64_t endUs, std::vector<Event>& out,
                       std::vector<uint64_t>& indices, std::string& err) {
    harvestPrefetch();
    return reader_.range(startUs, endUs, out, err, &indices);
}

void ChartStream::prefetch(int64_t startUs, int64_t endUs) {
    if (!reader_.isOpen() || endUs <= startUs) return;
    const Info& info = reader_.info();
    // Chunks overlapping [startUs, endUs): first chunk with lastTimeUs >=
    // startUs, through the last with firstTimeUs < endUs.
    size_t first = 0, last = size_t(info.chunkCount);
    // binary search on lastTimeUs via chunkAtOrBefore-style probing; the
    // reader does not expose per-chunk times for iteration, so walk the
    // index through chunkTimeRange (chunkCount is small enough that a
    // linear scan from the lower bound is fine — it is O(chunks in window)).
    // lower bound: first chunk whose lastTimeUs >= startUs
    {
        size_t lo = 0, hi = size_t(info.chunkCount);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            int64_t f, l;
            if (!reader_.chunkTimeRange(mid, f, l)) return;
            if (l >= startUs) hi = mid; else lo = mid + 1;
        }
        first = lo;
    }
    for (size_t i = first; i < size_t(info.chunkCount); ++i) {
        int64_t f, l;
        if (!reader_.chunkTimeRange(i, f, l)) return;
        if (f >= endUs) { last = i; break; }
        last = i + 1;
    }
    for (size_t i = first; i < last; ++i) {
        prefetchBufs_.emplace_back();
        prefetchFuts_.push_back(reader_.chunkEventsAsync(i, &prefetchBufs_.back()));
    }
}

void ChartStream::harvestPrefetch() {
    // Each future owns one deque buffer; deque push_back never invalidates
    // element references, so buffers stay valid until their decode finishes.
    // Keep everything until every outstanding decode is done, then drop it
    // all — decoded chunks remain in the reader's LRU cache regardless.
    for (auto& f : prefetchFuts_)
        if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    prefetchFuts_.clear();
    prefetchBufs_.clear();
}

}  // namespace gh::raid
