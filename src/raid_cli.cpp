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
#include <filesystem>
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
#include "rchr.hpp"

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
    std::printf("  unique notes   : %llu in %llu stored events (pattern table)\n",
                (unsigned long long)info.uniqueNotes, (unsigned long long)info.uniqueEvents);
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
    {
        uint64_t uev = 0, un = 0;
        if (rd.uniqueStats(uev, un, err))
            std::printf("  unique notes   : %llu in %llu stored events (pattern table)\n",
                        (unsigned long long)un, (unsigned long long)uev);
    }
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
// ---- .rchr (RAID Character archives) ---------------------------------------

namespace fs = std::filesystem;

// Expands pack arguments (files and directories, recursively) into
// (arcname, disk path) pairs. Arcnames are paths relative to `root` when
// possible, else the file name.
static void collectRchrInputs(const std::vector<std::string>& args, const fs::path& root,
                              std::vector<std::pair<std::string, std::string>>& out, std::string& err) {
    for (const std::string& a : args) {
        std::error_code ec;
        const fs::path p(a);
        if (fs::is_regular_file(p, ec)) {
            fs::path rel = fs::relative(p, root, ec);
            if (ec) rel = p.filename();
            out.push_back({rel.generic_string(), fs::absolute(p, ec).string()});
        } else if (fs::is_directory(p, ec)) {
            for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::error_code ec2;
                fs::path rel = fs::relative(it->path(), root, ec2);
                if (ec2) rel = it->path().filename();
                out.push_back({rel.generic_string(), it->path().string()});
            }
            if (ec) { err = "cannot scan " + a; return; }
        } else {
            err = "not a file or directory: " + a;
            return;
        }
    }
}

static int rchrPack(int argc, char** argv) {
    // rchr pack <out.rchr> [--root dir] <file|dir> [...]
    if (argc < 4) return 2;
    const std::string out = argv[3];
    fs::path root = fs::current_path();
    std::vector<std::string> inputs;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--root" && i + 1 < argc) {
            root = fs::absolute(argv[++i]);
        } else {
            inputs.push_back(a);
        }
    }
    std::vector<std::pair<std::string, std::string>> files;
    std::string err;
    collectRchrInputs(inputs, root, files, err);
    if (!err.empty()) { std::printf("pack failed: %s\n", err.c_str()); return 1; }
    if (!rchr::pack(out, files, err)) { std::printf("pack failed: %s\n", err.c_str()); return 1; }
    uint64_t raw = 0, stored = 0;
    std::error_code ec;
    for (const auto& [arc, disk] : files) raw += fs::file_size(disk, ec);
    stored = fs::file_size(out, ec);
    std::printf("%s: %zu files, %llu -> %llu bytes\n", out.c_str(), files.size(),
                (unsigned long long)raw, (unsigned long long)stored);
    for (const auto& [arc, disk] : files) std::printf("  %s\n", arc.c_str());
    return 0;
}

static int rchrCmd(int argc, char** argv) {
    const std::string sub = argc >= 3 ? argv[2] : "";
    if (sub == "pack" && argc >= 4) return rchrPack(argc, argv);
    if (sub == "list" && argc >= 4) {
        std::vector<rchr::Entry> entries;
        std::string err;
        if (!rchr::list(argv[3], entries, err)) { std::printf("list failed: %s\n", err.c_str()); return 1; }
        uint64_t raw = 0, stored = 0;
        for (const rchr::Entry& e : entries) {
            raw += e.rawSize;
            stored += e.storedSize;
            std::printf("  %-48s %8llu -> %8llu  %s\n", e.path.c_str(),
                        (unsigned long long)e.rawSize, (unsigned long long)e.storedSize,
                        e.method ? "deflate" : "store");
        }
        std::printf("%s: %zu entries, %llu -> %llu bytes\n", argv[3], entries.size(),
                    (unsigned long long)raw, (unsigned long long)stored);
        return 0;
    }
    if (sub == "unpack" && argc >= 5) {
        std::string err;
        if (!rchr::extract(argv[3], argv[4], err)) { std::printf("unpack failed: %s\n", err.c_str()); return 1; }
        std::printf("unpacked %s -> %s\n", argv[3], argv[4]);
        return 0;
    }
    std::printf("raid_engine rchr pack <out.rchr> [--root dir] <file|dir> [...]\n"
                "raid_engine rchr list <file.rchr>\n"
                "raid_engine rchr unpack <file.rchr> <outdir>\n");
    return 2;
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
                    "  raid_engine streamtest <file.raid>\n"
                    "  raid_engine rchr pack|list|unpack ...\n");
        return argc < 2 ? 2 : 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "convert" && argc >= 4) return convert(argv[2], argv[3]);
    if (cmd == "info" && argc >= 3) return info(argv[2]);
    if (cmd == "verify" && argc >= 3) return verify(argv[2]);
    if (cmd == "window" && argc >= 5) return window(argv[2], argv[3], argv[4]);
    if (cmd == "meta" && argc >= 3) return meta(argv[2]);
    if (cmd == "streamtest" && argc >= 3) return streamtest(argv[2]);
    if (cmd == "rchr" && argc >= 3) return rchrCmd(argc, argv);
    if (cmd == "patterns" && argc >= 3)
        return patterns(argv[2], argc >= 4 ? argv[3] : nullptr,
                        argc >= 5 ? argv[4] : nullptr);
    std::printf("unknown or incomplete command\n");
    return 2;
}
