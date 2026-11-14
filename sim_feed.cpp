// Checks the two things drawLanes silently depends on:
//  1. range() output sorted by timeUs (globally and per lane)
//  2. session lane vectors stay sorted by n.t (binary search in drawLanes)
// Also replicates the draw window to count candidates per frame.
#include "raid_chart.hpp"
#include "session.hpp"
#include "chart.hpp"

#include <cstdio>
#include <vector>
#include <algorithm>

using namespace gh;

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1]
        : "C:\\Users\\YalishannaZhou\\Watermelon.RAID";
    raid::ChartStream cs;
    Chart meta;
    std::string err;
    if (!cs.open(path, meta, err)) { std::printf("open failed: %s\n", err.c_str()); return 1; }
    std::printf("chart: keys=%d len=%.0fms speed=%.3f events=%llu\n", meta.keys, cs.lengthMs(),
                meta.speed, (unsigned long long)cs.logicalEvents());

    SessionConfig cfg{};
    Session s;
    s.init(meta, cfg);
    s.setBotplay(true);

    constexpr double kDt = 1000.0 / 60.0;
    constexpr double kLookaheadMs = 4000.0;
    constexpr double kPrefetchMs = 16000.0;
    int64_t fedUs = 0, prefetchUs = 0;
    const double len = cs.lengthMs();

    long long fedTotal = 0, outOfOrderGlobal = 0, outOfOrderLane[2][16] = {};
    int64_t lastT = INT64_MIN;
    // per-lane last appended t (session order check)
    double lastLaneT[2][16];
    for (auto& a : lastLaneT) for (double& d : a) d = -1e18;

    long long candTotal = 0, drawnTotal = 0;
    int reportBucket = -1;

    for (double songMs = 0; songMs < len + 5000.0; songMs += kDt) {
        const int64_t endUs = int64_t(std::max(0.0, songMs + kLookaheadMs) * 1000.0);
        if (endUs > fedUs) {
            std::vector<raid::Event> evs;
            std::vector<uint64_t> idx;
            if (!cs.pump(fedUs, endUs, evs, idx, err)) {
                std::printf("PUMP FAIL @%.0fms: %s\n", songMs, err.c_str());
                return 2;
            }
            fedTotal += (long long)evs.size();
            for (size_t i = 0; i < evs.size(); ++i) {
                const raid::Event& e = evs[i];
                if (e.timeUs < lastT) ++outOfOrderGlobal;
                lastT = e.timeUs;
                if (e.lane < meta.keys) {
                    const int side = int(e.side);
                    const double t = double(e.timeUs) / 1000.0;
                    if (t < lastLaneT[side][e.lane]) ++outOfOrderLane[side][e.lane];
                    lastLaneT[side][e.lane] = t;
                    LaneNote ln;
                    ln.t = t;
                    ln.len = double(e.lengthUs) / 1000.0;
                    ln.kind = e.kind;
                    ln.flags = e.flags;
                    ln.type = e.type;
                    ln.stack = std::max<uint16_t>(1, e.stack);
                    ln.id = uint32_t(idx[i]);
                    s.appendLaneNote(side, int(e.lane), ln);
                }
            }
            fedUs = endUs;
            s.gcBefore(songMs);
        }
        if (endUs >= prefetchUs) {
            cs.prefetch(endUs, endUs + int64_t(kPrefetchMs * 1000.0));
            prefetchUs = endUs + int64_t(kPrefetchMs * 500.0);
        }
        s.update(songMs);

        // replicate drawLanes window math (downscroll irrelevant to counts)
        const double pxPerMs = 0.45 * meta.speed;   // rate_=1, scrollSpeed=chart speed
        const double reach = (1080.0 * 1.5 + 105.0) / pxPerMs;
        const double tStart = songMs - reach * 0.5;
        const double tEnd = songMs + reach;
        for (int side = 0; side < 2; ++side)
            for (int lane = 0; lane < meta.keys; ++lane) {
                const auto& v = s.lane(side, lane);
                auto lo = std::lower_bound(v.begin(), v.end(), tStart,
                                           [](const LaneNote& n, double t) { return n.t < t; });
                auto hi = std::upper_bound(lo, v.end(), tEnd,
                                           [](double t, const LaneNote& n) { return t < n.t; });
                for (auto it = lo; it != hi; ++it) {
                    const LaneNote& n = *it;
                    if (n.state != kPending && n.state != kMissed) continue;
                    ++candTotal;
                    // minGap dedup approximated: count all (upper bound on drawn)
                    ++drawnTotal;
                }
            }

        const int b = int(songMs / 20000.0);
        if (b != reportBucket) {
            reportBucket = b;
            std::printf("t=%6.0fs fed=%lld oooGlobal=%lld laneOOO=%lld,%lld,%lld,%lld | cand=%lld drawn~=%lld\n",
                        songMs / 1000.0, fedTotal, outOfOrderGlobal,
                        outOfOrderLane[0][0] + outOfOrderLane[1][0],
                        outOfOrderLane[0][1] + outOfOrderLane[1][1],
                        outOfOrderLane[0][2] + outOfOrderLane[1][2],
                        outOfOrderLane[0][3] + outOfOrderLane[1][3],
                        candTotal, drawnTotal);
        }
    }
    std::printf("done fed=%lld ooo=%lld\n", fedTotal, outOfOrderGlobal);
    return 0;
}
