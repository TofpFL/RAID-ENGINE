// Headless tests for the RAID Engine: .raid write/read roundtrip, pattern
// compression, streaming range decode, integrity checks and the Chart bridge.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "raid.hpp"
#include "raid_chart.hpp"
#include "session.hpp"

using namespace gh;

extern int g_fail, g_pass;
#define RCHECK(cond)                                                             \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

namespace {

std::string tmpPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

Chart makeChart() {
    Chart c;
    c.title = "Raid Test";
    c.difficulty = "Hard";
    c.opponent = "bf";
    c.player = "dad";
    c.keys = 4;
    c.speed = 1.5;
    c.noteTypes = {"", "Hurt Note"};
    c.tempo = {{0, 120, 0}, {8000, 140, 8}};
    c.focus = {{0, 1}, {8000, 0}};
    // 200 notes, mixed lanes/sides/holds, unsorted-stack-free
    Rng rng(12345);
    for (int i = 0; i < 200; ++i) {
        Note n;
        n.time = 100.0 + i * 95.0 + rng.range(0, 20);
        n.lane = uint8_t(rng.range(0, 3));
        n.side = uint8_t(rng.range(0, 1));
        n.length = rng.chance(0.2) ? 120.0 : 0.0;
        n.flags = rng.chance(0.1) ? NoteFlag::kFlagAlt : 0;
        if (rng.chance(0.05)) n.type = 1;
        n.stack = 1;
        c.notes.push_back(n);
    }
    std::stable_sort(c.notes.begin(), c.notes.end(), [](const Note& a, const Note& b) {
        return a.time < b.time;
    });
    c.finalize(120);
    return c;
}

void testRoundtrip() {
    Chart c = makeChart();
    const std::string path = tmpPath("gh_raid_roundtrip.raid");
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;
    RCHECK(raid::saveChart(path, c, opt, &info, err));
    RCHECK(info.logicalEvents == c.notes.size());
    RCHECK(raid::isRaidFile(path));

    Chart d;
    RCHECK(raid::loadChart(path, d, err));
    RCHECK(d.title == c.title && d.difficulty == c.difficulty);
    RCHECK(d.keys == c.keys && d.opponent == c.opponent);
    RCHECK(d.notes.size() == c.notes.size());
    size_t bad = 0;
    for (size_t i = 0; i < c.notes.size() && i < d.notes.size(); ++i) {
        const Note& a = c.notes[i];
        const Note& b = d.notes[i];
        if (std::fabs(a.time - b.time) > 0.001 || std::fabs(a.length - b.length) > 0.001 ||
            a.lane != b.lane || a.side != b.side || a.kind != b.kind ||
            a.flags != b.flags || a.type != b.type || a.stack != b.stack)
            ++bad;
    }
    RCHECK(bad == 0);
    // through the generic loader too
    Chart g;
    RCHECK(loadChartFile(path, "", g, err));
    RCHECK(g.notes.size() == c.notes.size());
    std::filesystem::remove(path);
}

void testPatternCompression() {
    // An 8-note motif repeated 16 times at different times: perfect pattern
    // material. Lanes cycle 0,1,2,3,0,1,2,3 with alternating sides.
    std::vector<raid::Event> ev;
    const int64_t motifUs = 40000;  // 40 ms spacing inside the motif
    const int64_t repUs = 400000;   // 400 ms between repeats
    for (int r = 0; r < 16; ++r) {
        for (int k = 0; k < 8; ++k) {
            raid::Event e;
            e.timeUs = r * repUs + k * motifUs;
            e.lane = uint8_t(k % 4);
            e.side = uint8_t(k % 2);
            e.lengthUs = k == 3 ? 50000 : 0;
            e.flags = k == 7 ? 2 : 0;
            e.stack = 1;
            ev.push_back(e);
        }
    }
    const std::string path = tmpPath("gh_raid_patterns.raid");
    raid::EncodeOptions opt;
    opt.chunkEvents = 256;
    raid::Info info;
    std::string err;
    RCHECK(raid::writeRaid(path, ev, "{\"raid\":1}", opt, &info, err));
    RCHECK(info.patternCount >= 1);
    RCHECK(info.literalEvents < info.logicalEvents);
    RCHECK(info.storedBytes < info.rawBytes);

    // Decode through the streaming reader and compare every event.
    raid::Reader rd;
    RCHECK(rd.open(path, err));
    RCHECK(rd.info().logicalEvents == ev.size());
    std::vector<raid::Event> all;
    RCHECK(rd.range(0, (16 * repUs) + 1000, all, err));
    RCHECK(all.size() == ev.size());
    size_t bad = 0;
    for (size_t i = 0; i < ev.size() && i < all.size(); ++i) {
        if (all[i].timeUs != ev[i].timeUs || all[i].lane != ev[i].lane ||
            all[i].side != ev[i].side || all[i].lengthUs != ev[i].lengthUs ||
            all[i].flags != ev[i].flags)
            ++bad;
    }
    RCHECK(bad == 0);
    rd.close();  // Windows: an open FILE* blocks filesystem::remove
    std::filesystem::remove(path);
}

void testPitchTransform() {
    // Keyboard-grid style: the same 4-note figure transposed across pitch
    // origins (the "L D U R across batches of keys" case). Chart notes carry
    // no pitch, so this exercises the Event-level pitch fields directly.
    std::vector<raid::Event> ev;
    const int64_t step = 500000;   // 500 ms between transposed copies
    const int64_t stepUs = 125000; // 125 ms inside the figure
    for (int t = 0; t < 8; ++t) {
        for (int k = 0; k < 4; ++k) {
            raid::Event e;
            e.timeUs = t * step + k * stepUs;
            e.pitch = 48 + t * 12 + k;   // transposed by 12 per copy
            e.lane = uint8_t(k);
            e.side = 1;
            e.stack = 1;
            ev.push_back(e);
        }
    }
    const std::string path = tmpPath("gh_raid_pitch.raid");
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;
    RCHECK(raid::writeRaid(path, ev, "{\"raid\":1}", opt, &info, err));
    RCHECK(info.patternCount >= 1);

    raid::Reader rd;
    RCHECK(rd.open(path, err));
    std::vector<raid::Event> all;
    RCHECK(rd.range(0, 8 * step + 1000, all, err));
    RCHECK(all.size() == ev.size());
    size_t bad = 0;
    for (size_t i = 0; i < ev.size() && i < all.size(); ++i)
        if (all[i].pitch != ev[i].pitch || all[i].timeUs != ev[i].timeUs ||
            all[i].lane != ev[i].lane)
            ++bad;
    RCHECK(bad == 0);
    rd.close();
    std::filesystem::remove(path);
}

void testStreamingRange() {
    Chart c = makeChart();
    const std::string path = tmpPath("gh_raid_stream.raid");
    raid::EncodeOptions opt;
    opt.chunkEvents = 64;  // many small chunks -> exercise the index
    raid::Info info;
    std::string err;
    RCHECK(raid::saveChart(path, c, opt, &info, err));
    RCHECK(info.chunkCount > 1);

    raid::Reader rd;
    RCHECK(rd.open(path, err));
    // range queries at several windows must match the materialized notes
    for (double t : {0.0, 3000.0, 9500.0, 12345.0}) {
        int64_t a = int64_t(t * 1000), b = a + 200000;
        std::vector<raid::Event> win;
        RCHECK(rd.range(a, b, win, err));
        size_t expect = 0;
        for (const Note& n : c.notes) {
            int64_t nt = int64_t(std::llround(n.time * 1000));
            if (nt >= a && nt < b) ++expect;
        }
        RCHECK(win.size() == expect);
    }
    // chunkAtOrBefore sanity: -1 before the first note's time, valid inside
    RCHECK(rd.chunkAtOrBefore(-1) == -1);
    RCHECK(rd.chunkAtOrBefore(int64_t(c.notes.front().time * 1000) - 1) == -1);
    RCHECK(rd.chunkAtOrBefore(int64_t(c.notes.back().time * 1000)) >= 0);
    rd.close();
    std::filesystem::remove(path);
}

void testIntegrityAndLimits() {
    Chart c = makeChart();
    const std::string path = tmpPath("gh_raid_integrity.raid");
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;
    RCHECK(raid::saveChart(path, c, opt, &info, err));

    // corrupt one payload byte near the end
    {
        FILE* f = std::fopen(path.c_str(), "r+b");
        RCHECK(f != nullptr);
        if (f) {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, sz - 8, SEEK_SET);
            int ch = std::fgetc(f);
            std::fseek(f, sz - 8, SEEK_SET);
            std::fputc(ch ^ 0xFF, f);
            std::fclose(f);
        }
    }
    raid::Reader rd;
    RCHECK(rd.open(path, err));  // header/index still fine
    std::vector<raid::Event> out;
    bool decoded = rd.range(0, 1LL << 60, out, err);
    RCHECK(!decoded || out.size() <= c.notes.size());  // must not crash; crc catches it
    rd.close();
    std::filesystem::remove(path);

    // incompressible unique data: literalEvents == logicalEvents (honest
    // reporting, no fake compression claims)
    std::vector<raid::Event> ev;
    Rng rng(999);
    int64_t t = 0;
    for (int i = 0; i < 5000; ++i) {
        raid::Event e;
        t += rng.range(1, 50) * 1000;
        e.timeUs = t;
        e.lane = uint8_t(rng.range(0, 3));
        e.side = uint8_t(rng.range(0, 1));
        e.pitch = rng.range(0, 1 << 20);
        e.lengthUs = uint32_t(rng.range(0, 100000));
        e.flags = uint8_t(rng.range(0, 3));
        e.type = uint16_t(rng.range(0, 5));
        e.stack = uint16_t(rng.range(1, 4));
        ev.push_back(e);
    }
    const std::string path2 = tmpPath("gh_raid_random.raid");
    raid::Info info2;
    RCHECK(raid::writeRaid(path2, ev, "{}", opt, &info2, err));
    RCHECK(info2.literalEvents == ev.size());
    RCHECK(info2.patternCount == 0);
    // Honest reporting: no fake pattern compression (above), and the per-chunk
    // deflate fallback keeps stored <= raw even when data barely compresses.
    // (Small-range columns like lane/type still deflate well, so storedBytes
    // is legitimately below rawBytes here.)
    RCHECK(info2.storedBytes <= info2.rawBytes);
    raid::Reader rd2;
    RCHECK(rd2.open(path2, err));
    std::vector<raid::Event> all;
    RCHECK(rd2.range(0, t + 1, all, err));
    RCHECK(all.size() == ev.size());
    size_t bad = 0;
    for (size_t i = 0; i < ev.size(); ++i)
        if (all[i].timeUs != ev[i].timeUs || all[i].pitch != ev[i].pitch ||
            all[i].lane != ev[i].lane || all[i].stack != ev[i].stack ||
            all[i].type != ev[i].type)
            ++bad;
    RCHECK(bad == 0);
    rd2.close();
    std::filesystem::remove(path2);
}

void testEmptyAndAsync() {
    // empty chart still produces a valid file
    Chart c;
    c.title = "Empty";
    c.keys = 4;
    const std::string path = tmpPath("gh_raid_empty.raid");
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;
    RCHECK(raid::saveChart(path, c, opt, &info, err));
    RCHECK(info.logicalEvents == 0);
    Chart d;
    RCHECK(raid::loadChart(path, d, err));
    RCHECK(d.title == "Empty");
    std::filesystem::remove(path);

    // async chunk decode matches sync
    Chart c2 = makeChart();
    const std::string path2 = tmpPath("gh_raid_async.raid");
    RCHECK(raid::saveChart(path2, c2, opt, &info, err));
    raid::Reader rd;
    RCHECK(rd.open(path2, err));
    if (info.chunkCount > 0) {
        std::vector<raid::Event> sync, async;
        RCHECK(rd.chunkEvents(0, sync, err));
        auto fut = rd.chunkEventsAsync(0, &async);
        RCHECK(fut.get());
        RCHECK(async.size() == sync.size());
    }
    rd.close();
    std::filesystem::remove(path2);
}

void testEncoderValidation() {
    // writeRaid must reject events the format cannot represent, instead of
    // writing a file that decodes differently or not at all.
    std::vector<raid::Event> ev(4);
    for (int i = 0; i < 4; ++i) {
        ev[i].timeUs = i * 1000;
        ev[i].lane = 1;
        ev[i].side = 1;
        ev[i].kind = 0;
        ev[i].stack = 1;
        ev[i].type = 0;
    }
    const std::string path = tmpPath("gh_raid_invalid.raid");
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;

    auto expectFail = [&](int idx, auto mutate) {
        std::vector<raid::Event> bad = ev;
        mutate(bad[idx]);
        RCHECK(!raid::writeRaid(path, bad, "{}", opt, &info, err));
        RCHECK(!err.empty());
    };
    expectFail(1, [](raid::Event& e) { e.timeUs = -5; });
    expectFail(2, [](raid::Event& e) { e.timeUs = 500; });  // unsorted (prev is 1000)
    // lane/type/stack are uint8_t/uint16_t: out-of-range values cannot be
    // expressed (they truncate before validation), so only the wider fields
    // need negative tests.
    expectFail(1, [](raid::Event& e) { e.side = 2; });
    expectFail(1, [](raid::Event& e) { e.kind = 2; });
    expectFail(1, [](raid::Event& e) { e.pitch = -2; });
    expectFail(1, [](raid::Event& e) { e.pitch = 0x10000000; });

    // Bad options must be rejected too (they would produce unreadable files).
    RCHECK(!raid::writeRaid(path, ev, "{}", raid::EncodeOptions{0}, &info, err));
    RCHECK(!raid::writeRaid(path, ev, "{}", raid::EncodeOptions{8192, 1}, &info, err));
    RCHECK(!raid::writeRaid(path, ev, "{}",
                            raid::EncodeOptions{8192, 8, raid::kMaxChunkExpand + 1}, &info, err));
    RCHECK(!raid::writeRaid(path, ev, "{}",
                            raid::EncodeOptions{uint32_t(raid::kMaxChunkExpand) + 1}, &info, err));

    // A valid pitch survives the round trip.
    ev[1].pitch = 60;
    RCHECK(raid::writeRaid(path, ev, "{}", opt, &info, err));
    raid::Reader rd;
    RCHECK(rd.open(path, err));
    std::vector<raid::Event> all;
    RCHECK(rd.range(0, 1LL << 50, all, err));
    RCHECK(all.size() == 4 && all[1].pitch == 60);
    rd.close();
    std::filesystem::remove(path);
}

void testMalformed() {
    // Build one valid file, then corrupt it deterministically. Every mutated
    // copy must either decode identically or fail cleanly — never crash,
    // hang, or return different data (chunk CRCs make silent corruption
    // essentially impossible).
    std::vector<raid::Event> ev;
    for (int i = 0; i < 300; ++i) {
        raid::Event e;
        e.timeUs = i * 777;
        e.lane = uint8_t(i % 4);
        e.side = uint8_t(i % 2);
        e.pitch = i % 3 == 0 ? 60 + i % 12 : -1;
        e.lengthUs = uint32_t(i % 5) * 1000;
        e.stack = 1;
        ev.push_back(e);
    }
    const std::string base = tmpPath("gh_raid_fuzz_base.raid");
    raid::EncodeOptions opt;
    opt.chunkEvents = 64;
    raid::Info info;
    std::string err;
    RCHECK(raid::writeRaid(base, ev, "{}", opt, &info, err));

    std::vector<raid::Event> want;
    {
        raid::Reader rd;
        RCHECK(rd.open(base, err));
        RCHECK(rd.range(0, 1LL << 50, want, err));
        rd.close();
    }

    FILE* f = std::fopen(base.c_str(), "rb");
    RCHECK(f != nullptr);
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::vector<char> bytes((size_t)sz);  // extra parens: not a function decl
    std::fseek(f, 0, SEEK_SET);
    RCHECK(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
    std::fclose(f);

    const std::string mut = tmpPath("gh_raid_fuzz_mut.raid");
    Rng rng(0xA1D);
    int survived = 0, rejected = 0;
    for (int iter = 0; iter < 400; ++iter) {
        std::vector<char> b = bytes;
        const int mode = iter % 4;
        if (mode == 0) {
            // single byte flip at a deterministic pseudo-random offset
            size_t pos = size_t(rng.range(0, int(bytes.size() - 1)));
            b[pos] = char(b[pos] ^ (1 << rng.range(0, 7)));
        } else if (mode == 1) {
            // truncate
            size_t keep = size_t(rng.range(16, int(bytes.size())));
            b.resize(keep);
        } else if (mode == 2) {
            // overwrite a small run with 0xFF
            size_t pos = size_t(rng.range(0, int(bytes.size() - 8)));
            for (int k = 0; k < 8; ++k) b[pos + size_t(k)] = char(0xFF);
        } else {
            // corrupt the chunk index region specifically (no per-entry CRC)
            size_t pos = size_t(128 + rng.range(0, int(std::min<size_t>(bytes.size() - 129, 4096))));
            b[pos] ^= char(0x5A);
        }
        FILE* o = std::fopen(mut.c_str(), "wb");
        if (!o) break;
        std::fwrite(b.data(), 1, b.size(), o);
        std::fclose(o);

        raid::Reader rd;
        if (!rd.open(mut, err)) { ++rejected; continue; }
        std::vector<raid::Event> out;
        if (!rd.range(0, 1LL << 50, out, err)) { ++rejected; rd.close(); continue; }
        // If it decoded, it must be bit-identical to the original.
        RCHECK(out.size() == want.size());
        size_t diff = 0;
        for (size_t i = 0; i < out.size() && i < want.size(); ++i)
            if (out[i].timeUs != want[i].timeUs || out[i].pitch != want[i].pitch ||
                out[i].lane != want[i].lane || out[i].lengthUs != want[i].lengthUs ||
                out[i].side != want[i].side || out[i].kind != want[i].kind ||
                out[i].flags != want[i].flags || out[i].type != want[i].type ||
                out[i].stack != want[i].stack)
                ++diff;
        RCHECK(diff == 0);
        ++survived;
        rd.close();
    }
    RCHECK(survived + rejected == 400);
    std::filesystem::remove(base);
    std::filesystem::remove(mut);
}

void testConcurrentAndBudget() {
    // Many small chunks so windows and async decodes overlap heavily.
    Chart c = makeChart();
    for (int i = 0; i < 400; ++i) {  // ~600 notes total
        Note n;
        n.time = 50.0 + i * 30.0;
        n.lane = uint8_t(i % 4);
        n.side = uint8_t(i % 2);
        n.length = i % 11 == 0 ? 100.0 : 0.0;
        n.stack = 1;
        c.notes.push_back(n);
    }
    c.finalize(120);
    const std::string path = tmpPath("gh_raid_conc.raid");
    raid::EncodeOptions opt;
    opt.chunkEvents = 32;
    raid::Info info;
    std::string err;
    RCHECK(raid::saveChart(path, c, opt, &info, err));
    RCHECK(info.chunkCount > 8);

    // Concurrent range() queries over overlapping windows: every event in
    // every window must match the materialized chart exactly.
    {
        raid::Reader rd;
        RCHECK(rd.open(path, err));
        rd.setMaxConcurrentDecodes(4);
        std::atomic<int> failures{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) {
            ts.emplace_back([&, t] {
                for (int w = 0; w < 20; ++w) {
                    int64_t a = int64_t((t * 977 + w * 131) % 18000) * 1000;
                    int64_t b = a + int64_t(500 + (w % 5) * 250) * 1000;
                    std::vector<raid::Event> win;
                    std::string werr;
                    if (!rd.range(a, b, win, werr)) { ++failures; continue; }
                    size_t expect = 0;
                    for (const Note& n : c.notes) {
                        int64_t nt = int64_t(std::llround(n.time * 1000));
                        if (nt >= a && nt < b) ++expect;
                    }
                    if (win.size() != expect) ++failures;
                }
            });
        }
        for (auto& t : ts) t.join();
        RCHECK(failures == 0);
        rd.close();
    }

    // Async decodes racing range() on the same Reader.
    {
        raid::Reader rd;
        RCHECK(rd.open(path, err));
        std::vector<std::future<bool>> futs;
        std::vector<std::vector<raid::Event>> outs(info.chunkCount);
        for (size_t i = 0; i < info.chunkCount; ++i)
            futs.push_back(rd.chunkEventsAsync(i, &outs[i]));
        std::vector<raid::Event> win;
        RCHECK(rd.range(0, 1LL << 50, win, err));
        for (auto& f : futs) RCHECK(f.get());
        uint64_t total = 0;
        for (auto& o : outs) total += o.size();
        RCHECK(total == win.size());
        rd.close();
    }

    // Destroying the Reader while async decodes are in flight must be safe:
    // workers hold the file state, results are still delivered.
    {
        std::vector<std::vector<raid::Event>> outs(4);
        std::vector<std::future<bool>> futs;
        {
            raid::Reader rd;
            RCHECK(rd.open(path, err));
            for (int i = 0; i < 4; ++i)
                futs.push_back(rd.chunkEventsAsync(size_t(i * 3), &outs[size_t(i)]));
        }  // rd destroyed here, futures still pending
        for (auto& f : futs) RCHECK(f.get());
        for (auto& o : outs) RCHECK(!o.empty());
    }

    // Memory budget: a chunk that cannot fit the budget is rejected, not
    // silently over-allocated.
    {
        raid::Reader rd;
        RCHECK(rd.open(path, err));
        rd.setMemoryBudgetBytes(64);  // < one Event: maxChunkEvents_ clamps to 1
        std::vector<raid::Event> out;
        RCHECK(!rd.chunkEvents(0, out, err));
        RCHECK(err.find("budget") != std::string::npos);
        // Generous budget decodes fine again.
        rd.setMemoryBudgetBytes(64ull << 20);
        RCHECK(rd.chunkEvents(0, out, err));
        rd.close();
    }
    std::filesystem::remove(path);
}

// The PlayState path: ChartStream metadata + windowed pump + Session
// incremental feed must reproduce the materialized chart exactly.
void testChartStream() {
    const std::string file = tmpPath("gh_raid_stream.raid");
    Chart c;
    c.title = "stream";
    c.difficulty = "Normal";
    c.keys = 4;
    c.tempo.push_back({0, 120});
    for (int i = 0; i < 4000; ++i) {
        Note n;
        n.time = 100.0 + i * 14.7;
        n.length = (i % 7 == 0) ? 300.0 : 0.0;
        n.lane = i % 4;
        n.side = (i / 4) % 2;
        n.kind = kNormal;
        n.stack = uint16_t(1 + (i % 3 == 0 ? 1 : 0));
        c.notes.push_back(n);
    }
    c.finalize(120);
    raid::EncodeOptions opt;
    raid::Info info;
    std::string err;
    RCHECK(raid::saveChart(file, c, opt, &info, err));

    raid::ChartStream cs;
    Chart meta;
    RCHECK(cs.open(file, meta, err));
    RCHECK(meta.notes.empty());  // metadata only — the point of streaming
    RCHECK(meta.rawNoteCount == info.logicalEvents);
    RCHECK(meta.playerNotes == c.playerNotes && meta.opponentNotes == c.opponentNotes);
    RCHECK(std::abs(meta.lengthMs - c.lengthMs) < 1.0);

    // Warm the first chunks on workers, then pump the whole chart in
    // 5 s windows — exactly what PlayState::pumpNotes does.
    cs.prefetch(0, 20'000'000);
    cs.harvestPrefetch();
    std::vector<raid::Event> evs;
    std::vector<uint64_t> idx;
    const int64_t totalUs = int64_t(meta.lengthMs * 1000.0) + 2'000'000;
    for (int64_t w = -1'000'000; w < totalUs; w += 5'000'000)
        RCHECK(cs.pump(w, std::min(w + 5'000'000, totalUs), evs, idx, err));
    RCHECK(evs.size() == info.logicalEvents);
    RCHECK(idx.size() == evs.size());
    for (size_t i = 1; i < idx.size(); ++i) RCHECK(idx[i] > idx[i - 1]);

    // Every streamed event must equal the materialized encoding, in order.
    const std::vector<raid::Event> want = raid::chartEvents(c);
    RCHECK(want.size() == evs.size());
    size_t bad = 0;
    for (size_t i = 0; i < evs.size(); ++i) {
        const raid::Event& a = evs[i], &b = want[i];
        if (a.timeUs != b.timeUs || a.lengthUs != b.lengthUs || a.lane != b.lane ||
            a.side != b.side || a.kind != b.kind || a.flags != b.flags ||
            a.type != b.type || a.stack != b.stack)
            ++bad;
    }
    RCHECK(bad == 0);
    cs.close();
    std::filesystem::remove(file);
}

void testSessionStreaming() {
    Chart c;
    c.keys = 4;
    c.tempo.push_back({0, 120});
    for (int i = 0; i < 2000; ++i) {
        Note n;
        n.time = 50.0 + i * 30.0;
        n.length = (i % 5 == 0) ? 500.0 : 0.0;
        n.lane = i % 4;
        n.side = i % 2;
        n.kind = kNormal;
        n.type = uint16_t(i % 2);
        n.stack = uint16_t(1 + (i % 4 == 0 ? 2 : 0));
        c.notes.push_back(n);
    }
    c.finalize(120);

    // Reference session over the materialized chart.
    Session ref;
    SessionConfig cfg;
    ref.init(c, cfg);

    // Streaming session: metadata chart (no notes), then incremental feed.
    Chart meta = c;
    meta.notes.clear();
    Session s;
    s.init(meta, cfg);
    s.setTotals(c.playerNotes, c.opponentNotes);
    RCHECK(s.totalPlayerNotes == ref.totalPlayerNotes);
    RCHECK(s.totalOpponentNotes == ref.totalOpponentNotes);

    const std::vector<raid::Event> evs = raid::chartEvents(c);
    for (size_t i = 0; i < evs.size(); ++i) {
        const raid::Event& e = evs[i];
        LaneNote ln;
        ln.t = double(e.timeUs) / 1000.0;
        ln.len = double(e.lengthUs) / 1000.0;
        ln.kind = e.kind;
        ln.flags = e.flags;
        ln.type = e.type;
        ln.stack = std::max<uint16_t>(1, e.stack);
        ln.id = uint32_t(i);
        s.appendLaneNote(int(e.side), int(e.lane), ln);
    }
    for (int side = 0; side < 2; ++side)
        for (int l = 0; l < 4; ++l) {
            const auto& a = s.lane(side, l), &b = ref.lane(side, l);
            RCHECK(a.size() == b.size());
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                RCHECK(a[i].t == b[i].t && a[i].len == b[i].len && a[i].id == b[i].id &&
                       a[i].kind == b[i].kind && a[i].type == b[i].type);
        }

    // gcBefore: resolve everything by playing far past the end, then check
    // the lanes shrink and cursors stay consistent.
    s.update(1e9);
    RCHECK(s.allResolved());
    const size_t before = [&] {
        size_t n = 0;
        for (int side = 0; side < 2; ++side)
            for (int l = 0; l < 4; ++l) n += s.lane(side, l).size();
        return n;
    }();
    s.gcBefore(1e9);
    size_t after = 0;
    for (int side = 0; side < 2; ++side)
        for (int l = 0; l < 4; ++l) after += s.lane(side, l).size();
    RCHECK(after < before);
    RCHECK(s.allResolved());
}

}  // namespace

void runRaidTests() {
    testRoundtrip();
    testPatternCompression();
    testPitchTransform();
    testStreamingRange();
    testChartStream();
    testSessionStreaming();
    testIntegrityAndLimits();
    testEmptyAndAsync();
    testEncoderValidation();
    testMalformed();
    testConcurrentAndBudget();
}

// gh_tests --raid <input> <output.raid>: converts any chart loadChartFile
// understands (JSON charts, V-Slice, PulseForge, .flp/.fsc via SNIFF) into
// .raid and prints the encoder's honest compression report. Also verifies
// the file by reloading it.
int raidConvertCli(int argc, char** argv, int first) {
    if (first + 1 >= argc) {
        std::printf("usage: gh_tests --raid <chart|flp|fsc> <out.raid>\n");
        return 2;
    }
    const std::string in = argv[first], out = argv[first + 1];
    Chart c;
    std::string err;
    if (!loadChartFile(in, "", c, err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    const double t0 = nowSec();
    raid::EncodeOptions opt;
    raid::Info info;
    if (!raid::saveChart(out, c, opt, &info, err)) {
        std::printf("encode failed: %s\n", err.c_str());
        return 1;
    }
    const double dt = nowSec() - t0;
    std::printf("raid: %s -> %s\n", in.c_str(), out.c_str());
    std::printf("  logical events : %llu\n", (unsigned long long)info.logicalEvents);
    std::printf("  literal events : %llu (%.1f%%)\n", (unsigned long long)info.literalEvents,
                info.logicalEvents ? 100.0 * double(info.literalEvents) / double(info.logicalEvents) : 0.0);
    std::printf("  patterns       : %llu\n", (unsigned long long)info.patternCount);
    std::printf("  chunks         : %llu\n", (unsigned long long)info.chunkCount);
    std::printf("  payload bytes  : %llu raw -> %llu stored (%.2fx)\n",
                (unsigned long long)info.rawBytes, (unsigned long long)info.storedBytes,
                info.storedBytes ? double(info.rawBytes) / double(info.storedBytes) : 0.0);
    std::printf("  encode time    : %.3f s (%.0f events/s)\n", dt,
                dt > 0 ? double(info.logicalEvents) / dt : 0.0);
    // verify by reloading
    Chart d;
    if (!raid::loadChart(out, d, err)) {
        std::printf("verify failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("  verify         : %zu notes decoded, title \"%s\"\n", d.notes.size(), d.title.c_str());
    if (d.notes.size() != c.notes.size()) {
        std::printf("  MISMATCH: %zu != %zu\n", d.notes.size(), c.notes.size());
        return 1;
    }
    return 0;
}
