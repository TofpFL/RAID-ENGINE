// raid_engine: standalone RAID Engine command line tool (no raylib).
//
//   raid_engine convert <chart|flp|fsc> <out.raid>   encode a chart to .raid
//   raid_engine info <file.raid>                     header + encoder stats
//   raid_engine verify <file.raid>                   decode every chunk, count events
//   raid_engine window <file.raid> <startMs> <endMs> events in a time window
//   raid_engine meta <file.raid>                     the chart metadata (JSON)
//   raid_engine patterns <file.raid> [startMs] [endMs] op-stream log: which
//                                                    pattern plays when
#include <algorithm>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "flp_import.hpp"
#include "raid.hpp"
#include "raid_chart.hpp"

using namespace gh;

static int convert(const std::string& in, const std::string& out) {
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;

    // Whole .flp/.fsc projects encode directly: patterns are stored once as
    // pattern-table definitions and playlist clips place them on the
    // timeline â€” the file decodes as one contiguous chart. A `#N` suffix
    // still selects a single pattern through the Chart path.
    std::string flpFile;
    int flpPat = -1;
    if (flpChartPath(in, flpFile, flpPat) && flpPat < 0) {
        FlpProject p;
        if (!readFlpFile(flpFile, p, err)) {
            std::printf("load failed: %s\n", err.c_str());
            return 1;
        }
        if (!raid::writeRaidFlp(out, p, opt, &info, err)) {
            std::printf("encode failed: %s\n", err.c_str());
            return 1;
        }
    } else {
        Chart c;
        if (!loadChartFile(in, "", c, err)) {
            std::printf("load failed: %s\n", err.c_str());
            return 1;
        }
        if (!raid::saveChart(out, c, opt, &info, err)) {
            std::printf("encode failed: %s\n", err.c_str());
            return 1;
        }
    }
    std::printf("%s -> %s\n", in.c_str(), out.c_str());
    std::printf("  logical events : %llu\n", (unsigned long long)info.logicalEvents);
    std::printf("  literal events : %llu (%.1f%%)\n", (unsigned long long)info.literalEvents,
                info.logicalEvents ? 100.0 * double(info.literalEvents) / double(info.logicalEvents) : 0.0);
    std::printf("  patterns       : %llu\n", (unsigned long long)info.patternCount);
    std::printf("  chunks         : %llu\n", (unsigned long long)info.chunkCount);
    std::printf("  payload bytes  : %llu raw -> %llu stored (%.2fx)\n",
                (unsigned long long)info.rawBytes, (unsigned long long)info.storedBytes,
                info.storedBytes ? double(info.rawBytes) / double(info.storedBytes) : 0.0);
    return 0;
}

static int info(const std::string& path) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const raid::Info& i = rd.info();
    std::printf("%s\n", path.c_str());
    std::printf("  version        : %u\n", i.version);
    std::printf("  logical events : %llu\n", (unsigned long long)i.logicalEvents);
    std::printf("  chunks         : %llu\n", (unsigned long long)i.chunkCount);
    std::printf("  patterns       : %llu\n", (unsigned long long)i.patternCount);
    std::printf("  duration       : %.0f ms\n", double(rd.durationUs()) / 1000.0);
    rd.close();
    return 0;
}

static int verify(const std::string& path) {
    Chart c;
    std::string err;
    if (!raid::loadChart(path, c, err)) {
        std::printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s: %zu notes, title \"%s\", %.0f ms\n", path.c_str(), c.notes.size(),
                c.title.c_str(), c.lengthMs);
    return 0;
}

static int window(const std::string& path, const char* a, const char* b) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const int64_t lo = int64_t(std::atof(a) * 1000.0), hi = int64_t(std::atof(b) * 1000.0);
    std::vector<raid::Event> evs;
    if (!rd.range(lo, hi, evs, err)) {
        std::printf("range failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%lld events in [%.0f, %.0f) ms\n", (long long)evs.size(),
                double(lo) / 1000.0, double(hi) / 1000.0);
    for (const raid::Event& e : evs)
        std::printf("  t=%lld.%03d ms lane=%u side=%u kind=%u len=%lld us\n",
                    (long long)(e.timeUs / 1000), int(e.timeUs % 1000), e.lane, e.side, e.kind,
                    (long long)e.lengthUs);
    rd.close();
    return 0;
}

static int meta(const std::string& path) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    std::fputs(rd.metaJson().c_str(), stdout);
    std::fputc('\n', stdout);
    rd.close();
    return 0;
}

static int patterns(const std::string& path, const char* a, const char* b) {
    raid::Reader rd;
    std::string err;
    if (!rd.open(path, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const int64_t lo = a ? int64_t(std::atof(a) * 1000.0) : 0;
    const int64_t hi = b ? int64_t(std::atof(b) * 1000.0) : INT64_MAX;
    std::vector<raid::Reader::OpSegment> segs;
    if (!rd.opLog(lo, hi, segs, err)) {
        std::printf("op log failed: %s\n", err.c_str());
        return 1;
    }
    uint64_t lits = 0, pats = 0, ppChanges = 0;
    std::set<uint32_t> used;
    int64_t prevPat = -1;  // patternId of the previous pattern segment
    for (const raid::Reader::OpSegment& s : segs) {
        if (s.kind == 2) {
            ++pats;
            used.insert(s.count);
            if (prevPat >= 0 && int64_t(s.count) != prevPat) ++ppChanges;
            prevPat = int64_t(s.count);
            std::printf("  pat#%-6u x%-4u chunk %-3u [%10.1f - %10.1f ms]  pitch%+d lane%+d\n",
                        s.count, s.aux, s.chunk, double(s.startUs) / 1000.0,
                        double(s.endUs) / 1000.0, s.pitchDelta, s.laneDelta);
        } else {
            ++lits;
            prevPat = -1;  // a literal breaks pattern-to-pattern adjacency
            std::printf("  lit      chunk %-3u [%10.1f - %10.1f ms]  %u events\n",
                        s.chunk, double(s.startUs) / 1000.0,
                        double(s.endUs) / 1000.0, s.count);
        }
    }
    std::printf("  segments: %zu (%llu literal runs, %llu pattern instances)\n",
                segs.size(), (unsigned long long)lits, (unsigned long long)pats);
    std::printf("  pattern->pattern changes: %llu   distinct patterns: %zu\n",
                (unsigned long long)ppChanges, used.size());
    rd.close();
    return 0;
}

// Replays the game's feed loop: 60 fps song clock, 4 s pump window,
// 16 s async prefetch, per-10 s event buckets. Catches stream failures
// and dropped lanes that `verify` (one sequential decode) cannot see.
static int streamtest(const std::string& path) {
    raid::ChartStream cs;
    Chart meta;
    std::string err;
    if (!cs.open(path, meta, err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s: %.0f ms, keys %d, P%llu O%llu notes\n", path.c_str(), meta.lengthMs,
                meta.keys, (unsigned long long)meta.playerNotes,
                (unsigned long long)meta.opponentNotes);
    int64_t fedUs = std::numeric_limits<int64_t>::min() / 4, prefetchUs = fedUs;
    double songMs = -300.0;
    const double stepMs = 1000.0 / 60.0;
    uint64_t total = 0, badLane = 0, stackSum = 0;
    std::vector<uint64_t> bucket(48, 0);
    while (songMs < meta.lengthMs + 5000.0) {
        songMs += stepMs;
        const int64_t endUs = int64_t(std::max(0.0, songMs + 4000.0) * 1000.0);
        if (endUs > fedUs) {
            std::vector<raid::Event> evs;
            std::vector<uint64_t> idx;
            if (!cs.pump(fedUs, endUs, evs, idx, err)) {
                std::printf("PUMP FAIL @ %.0f ms: %s\n", songMs, err.c_str());
                return 1;
            }
            for (const raid::Event& e : evs) {
                ++total;
                if (e.lane >= meta.keys) ++badLane;
                stackSum += e.stack ? e.stack : 1;
                const size_t b = std::min<size_t>(47, size_t(std::max<int64_t>(0, e.timeUs / 10000000)));
                ++bucket[b];
            }
            fedUs = endUs;
        }
        if (endUs >= prefetchUs) {
            cs.prefetch(endUs, endUs + int64_t(16000.0 * 1000.0));
            prefetchUs = endUs + int64_t(8000.0 * 1000.0);
        }
    }
    cs.close();
    std::printf("fed %llu events, %llu stacked notes (%llu out-of-range lanes)\n",
                (unsigned long long)total, (unsigned long long)stackSum, (unsigned long long)badLane);
    for (size_t i = 0; i < bucket.size(); ++i)
        if (bucket[i]) std::printf("  %4zu0-%4zus: %llu\n", i, i, (unsigned long long)bucket[i]);
    return 0;
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("raid_engine - RAID Engine command line tool\n"
                    "  raid_engine convert <chart|flp|fsc> <out.raid>\n"
                    "  raid_engine info <file.raid>\n"
                    "  raid_engine verify <file.raid>\n"
                    "  raid_engine window <file.raid> <startMs> <endMs>\n"
                    "  raid_engine meta <file.raid>\n"
                    "  raid_engine patterns <file.raid> [startMs] [endMs]\n"
                    "  raid_engine streamtest <file.raid>\n");
        return argc < 2 ? 2 : 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "convert" && argc >= 4) return convert(argv[2], argv[3]);
    if (cmd == "info" && argc >= 3) return info(argv[2]);
    if (cmd == "verify" && argc >= 3) return verify(argv[2]);
    if (cmd == "window" && argc >= 5) return window(argv[2], argv[3], argv[4]);
    if (cmd == "meta" && argc >= 3) return meta(argv[2]);
    if (cmd == "streamtest" && argc >= 3) return streamtest(argv[2]);
    if (cmd == "patterns" && argc >= 3)
        return patterns(argv[2], argc >= 4 ? argv[3] : nullptr,
                        argc >= 5 ? argv[4] : nullptr);
    std::printf("unknown or incomplete command\n");
    return 2;
}
