// raid_engine: standalone RAID Engine command line tool (no raylib).
//
//   raid_engine convert <chart|flp|fsc> <out.raid>   encode a chart to .raid
//   raid_engine info <file.raid>                     header + encoder stats
//   raid_engine verify <file.raid>                   decode every chunk, count events
//   raid_engine window <file.raid> <startMs> <endMs> events in a time window
//   raid_engine meta <file.raid>                     the chart metadata (JSON)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "raid.hpp"
#include "raid_chart.hpp"

using namespace gh;

static int convert(const std::string& in, const std::string& out) {
    Chart c;
    std::string err;
    if (!loadChartFile(in, "", c, err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    raid::EncodeOptions opt;
    raid::Info info;
    if (!raid::saveChart(out, c, opt, &info, err)) {
        std::printf("encode failed: %s\n", err.c_str());
        return 1;
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

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("raid_engine - RAID Engine command line tool\n"
                    "  raid_engine convert <chart|flp|fsc> <out.raid>\n"
                    "  raid_engine info <file.raid>\n"
                    "  raid_engine verify <file.raid>\n"
                    "  raid_engine window <file.raid> <startMs> <endMs>\n"
                    "  raid_engine meta <file.raid>\n");
        return argc < 2 ? 2 : 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "convert" && argc >= 4) return convert(argv[2], argv[3]);
    if (cmd == "info" && argc >= 3) return info(argv[2]);
    if (cmd == "verify" && argc >= 3) return verify(argv[2]);
    if (cmd == "window" && argc >= 5) return window(argv[2], argv[3], argv[4]);
    if (cmd == "meta" && argc >= 3) return meta(argv[2]);
    std::printf("unknown or incomplete command\n");
    return 2;
}
