// Headless tests for the engine core: JSON, chart import, gameplay rules and
// the procedural composer. Run: gh_tests [--audio] [--wav <dir>]
// Also: --chart <files...> (benchmark), --psych-corpus <dir> (mod content check).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "composer.hpp"
#include "json.hpp"
#include "session.hpp"

using namespace gh;

int g_fail = 0, g_pass = 0;
void runPsychTests();
void runScanTests();
#ifdef GH_AUDIO_TESTS
void runAudioTests();
#endif
int printFlp(int argc, char** argv, int first);
void runExtraTests(bool audio);
int runPsychCorpus(const char* dir);
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static void testJson() {
    json::Document d;
    CHECK(d.parse(R"({"a": [1, 2.5, -3e2], "s": "x\"yé", "t": true, "n": null, "o": {"k": false},})"));
    const json::Value& r = d.root();
    const json::Value* a = d.get(r, "a");
    CHECK(a && d.size(*a) == 3);
    CHECK(a && near(d.at(*a, 1).num, 2.5) && near(d.at(*a, 2).num, -300));
    CHECK(d.str(*d.get(r, "s")) == "x\"y\xC3\xA9");
    CHECK(json::Document::boolean(d.get(r, "t"), false));
    CHECK(d.get(r, "n") && d.get(r, "n")->type == json::Type::Null);
    CHECK(!json::Document::boolean(d.get(*d.get(r, "o"), "k"), true));
    std::string err;
    CHECK(!d.parse("{\"a\": [1, 2", &err) && !err.empty());
}

static const char* kLegacy = R"({"song": {
  "song": "Test Song", "bpm": 120, "speed": 2.2, "player1": "bf", "player2": "dad",
  "notes": [
    {"mustHitSection": true, "lengthInSteps": 16, "sectionNotes": [[0, 0, 0], [500, 5, 0], [600, 1, 400], [700, -1, 0, "event"]]},
    {"mustHitSection": false, "sectionBeats": 4, "changeBPM": true, "bpm": 60, "sectionNotes": [[2000, 2, 0], [2100, 6, 50, "Hurt Note"]]}
  ]}})";

static void testLegacyChart() {
    Chart c;
    std::string err;
    CHECK(parseChart(kLegacy, "Hard", c, err));
    CHECK(c.title == "Test Song");
    CHECK(c.keys == 4);
    CHECK(c.notes.size() == 5);
    // Section 1 mustHit: lanes 0-3 are the player.
    CHECK(c.notes[0].side == 1 && c.notes[0].lane == 0);
    CHECK(c.notes[1].side == 0 && c.notes[1].lane == 1);   // 5 -> opponent lane 1
    CHECK(c.notes[2].side == 1 && near(c.notes[2].length, 400));
    // Section 2 not mustHit: lane 2 is the opponent, lane 6 the player.
    CHECK(c.notes[3].side == 0 && c.notes[3].lane == 2);
    CHECK(c.notes[4].side == 1 && c.notes[4].lane == 2 && c.notes[4].kind == kMine);
    CHECK(near(c.notes[4].length, 0));  // 50 ms < one step at 60 bpm
    CHECK(c.tempo.size() == 2 && near(c.tempo[1].timeMs, 2000) && near(c.tempo[1].bpm, 60));
    CHECK(near(c.beatAt(2000), 4.0) && near(c.beatAt(3000), 5.0));
    CHECK(near(c.msAtBeat(5.0), 3000));
    CHECK(c.focusAt(100) == 1 && c.focusAt(2500) == 0);
}

static void testPsychV1AndExtraKeys() {
    const char* v1 = R"({"song": {"song": "V1", "bpm": 100, "speed": 1, "format": "psych_v1",
      "notes": [{"mustHitSection": false, "sectionNotes": [[0, 1, 0], [10, 6, 0]]}]}})";
    Chart c;
    std::string err;
    CHECK(parseChart(v1, "Normal", c, err));
    CHECK(c.notes.size() == 2);
    CHECK(c.notes[0].side == 1 && c.notes[0].lane == 1);  // v1: 0-3 always player
    CHECK(c.notes[1].side == 0 && c.notes[1].lane == 2);

    const char* nine = R"({"song": {"song": "9K", "bpm": 150, "speed": 3, "mania": 8,
      "notes": [{"mustHitSection": true, "sectionNotes": [[0, 0, 0], [5, 8, 0], [10, 17, 0], [15, 9, 0]]}]}})";
    CHECK(parseChart(nine, "Hard", c, err));
    CHECK(c.keys == 9);
    CHECK(c.notes.size() == 4 && c.playerNotes == 2 && c.opponentNotes == 2);

    // No key count in the file: 4 keys like Psych and JS Engine, whatever
    // values the notes use (lane = value % 4, 4 and up is the other side).
    const char* noCount = R"({"song": {"song": "Eons", "bpm": 150, "speed": 3,
      "notes": [{"mustHitSection": true, "sectionNotes": [[0, 0, 0], [5, 8, 0], [10, 17, 0], [15, 9, 0]]}]}})";
    CHECK(parseChart(noCount, "Hard", c, err));
    CHECK(c.keys == 4);
    CHECK(c.notes.size() == 4 && c.playerNotes == 1 && c.opponentNotes == 3);
    CHECK(c.notes[2].side == 0 && c.notes[2].lane == 1);  // 17 -> opponent lane 1

    // "mania": 3 is 9 keys in Kade forks and 4 keys in Psych Extra Keys: the notes decide.
    std::string kade = R"({"song": {"song": "K", "bpm": 150, "mania": 3, "notes": [{"mustHitSection": true, "sectionNotes": [)";
    std::string ek = kade;
    for (int i = 0; i < 40; ++i) {
        kade += (i ? "," : "") + std::string("[") + std::to_string(i * 10) + "," + std::to_string(i % 18) + ",0]";
        ek += (i ? "," : "") + std::string("[") + std::to_string(i * 10) + "," + std::to_string(i % 8) + ",0]";
    }
    kade += "]}]}}";
    ek += "]}]}}";
    CHECK(parseChart(kade, "Hard", c, err) && c.keys == 9);
    CHECK(parseChart(ek, "Hard", c, err) && c.keys == 4);

    // Shaggy-style multi-key numbers mania 0-8 as 4, 6, 9, 5, 7, 8, 1, 2, 3 keys.
    // Notes mostly on the section's own side: every lane of a side in use
    // (0-4 with mania 3, 0-7 with mania 5) says 5 and 8 keys, not Extra Keys' 4 and 6.
    auto chartWith = [](int mania, const std::vector<int>& lanes) {
        std::string j = R"({"song": {"song": "M", "bpm": 150, "mania": )" + std::to_string(mania) +
                        R"(, "notes": [{"mustHitSection": true, "sectionNotes": [)";
        for (size_t i = 0; i < lanes.size(); ++i)
            j += (i ? "," : "") + std::string("[") + std::to_string(i * 10) + "," + std::to_string(lanes[i]) + ",0]";
        return j + "]}]}}";
    };
    std::vector<int> five, eight, six;
    for (int i = 0; i < 50; ++i) {
        five.push_back(i % 5);
        eight.push_back(i % 8);
        six.push_back(i % 12);
    }
    eight.push_back(14);  // a stray note on the other side
    CHECK(parseChart(chartWith(3, five), "Hard", c, err) && c.keys == 5);
    CHECK(parseChart(chartWith(5, eight), "Hard", c, err) && c.keys == 8);
    CHECK(parseChart(chartWith(5, six), "Hard", c, err) && c.keys == 6);  // Extra Keys 6K, both sides

    // "options": {"mania": 3} (Andromeda / Dave & Bambi forks) is 9 keys only
    // when the notes use 9 lanes: Eons keeps it with notes on lanes 0-3 only and
    // plays with 4 keys in Psych and JS Engine.
    auto optionsChart = [](const std::vector<int>& lanes) {
        std::string j = R"({"song": {"song": "O", "bpm": 150, "notes": [{"mustHitSection": true, "sectionNotes": [)";
        for (size_t i = 0; i < lanes.size(); ++i)
            j += (i ? "," : "") + std::string("[") + std::to_string(i * 10) + "," + std::to_string(lanes[i]) + ",0]";
        return j + R"(]}], "options": {"mania": 3, "crits": false, "allowBot": true}}})";
    };
    std::vector<int> eons, both, dnb9;
    for (int i = 0; i < 60; ++i) {
        eons.push_back(i % 4);
        both.push_back(i % 8);
        dnb9.push_back(i % 18);
    }
    CHECK(parseChart(optionsChart(eons), "Hard", c, err) && c.keys == 4);
    CHECK(c.playerNotes == 60 && c.opponentNotes == 0);
    CHECK(parseChart(optionsChart(both), "Hard", c, err) && c.keys == 4);
    CHECK(parseChart(optionsChart(dnb9), "Hard", c, err) && c.keys == 9);
    // A song-level "mania" whose readings all fit worse than 4 keys: 4 keys.
    CHECK(parseChart(chartWith(2, both), "Hard", c, err) && c.keys == 4);

    // Stacked notes (same lane within 1 ms) are one note that counts as all of them.
    const char* stacked = R"({"song": {"song": "S", "bpm": 100, "notes": [{"mustHitSection": true,
      "sectionNotes": [[100, 0, 0], [100.4, 0, 0], [100, 1, 0], [100.2, 0, 0]]}]}})";
    CHECK(parseChart(stacked, "", c, err));
    CHECK(c.notes.size() == 2);
    CHECK(c.notes[0].lane == 0 && c.notes[0].stack == 3 && c.notes[1].stack == 1);
    CHECK(c.playerNotes == 4 && c.rawNoteCount == 4);
    Session s;
    SessionConfig bot;
    bot.botplay = true;
    s.init(c, bot);
    CHECK(s.totalPlayerNotes == 4);
    s.update(200);
    CHECK(s.judged() == 4 && s.combo == 4 && s.misses == 0);
    s.init(c, SessionConfig{});
    s.update(1000);  // nothing pressed: every stacked note is a miss
    CHECK(s.misses == 4 && s.judged() == 0);

    // No 32-bit limits: a full stack (65535 notes) hit with JS Engine's note
    // multiplier at 100000 counts 6.5 billion notes in one hit.
    Chart big;
    big.keys = 4;
    Note bn;
    bn.time = 500;
    bn.lane = 0;
    bn.side = 1;
    bn.stack = kMaxStack;
    big.notes.push_back(bn);
    bn.side = 0;
    big.notes.push_back(bn);
    big.finalize(120);
    s.init(big, bot);
    s.setNoteMultiplier(1, 100000);
    s.setNoteMultiplier(0, 100000);
    s.update(600);
    const long long expect = 65535LL * 100000LL;
    CHECK(s.judged() == expect && s.combo == expect && s.maxCombo == expect);
    CHECK(s.score == expect * ratingInfo(kDivine).score);
    CHECK(s.opponentHits == expect);
    CHECK(s.totalPlayerNotes == 65535 && s.totalOpponentNotes == 65535);
    CHECK(fmtThousands(expect) == "6,553,500,000");
}

static void testVSlice() {
    const char* meta = R"({"songName": "Slice", "timeChanges": [{"t": 0, "bpm": 150}],
      "playData": {"difficulties": ["easy", "hard"], "characters": {"player": "bf", "opponent": "pico"}}})";
    const char* chart = R"({"version": "2.0.0", "scrollSpeed": {"easy": 1.5, "hard": 2.5},
      "events": [{"t": 0, "e": "FocusCamera", "v": {"char": 1}}, {"t": 1000, "e": "FocusCamera", "v": {"char": 0}}],
      "notes": {"easy": [{"t": 0, "d": 0}], "hard": [{"t": 0, "d": 0, "l": 500}, {"t": 400, "d": 7}, {"t": 800, "d": 2, "k": "hurt"}]}})";
    Chart c;
    std::string err;
    CHECK(parseChart(chart, "Hard", c, err, meta));
    CHECK(c.title == "Slice");
    CHECK(near(c.speed, 2.5));
    CHECK(c.notes.size() == 3);
    CHECK(c.notes[0].side == 1 && near(c.notes[0].length, 500));
    CHECK(c.notes[1].side == 0 && c.notes[1].lane == 3);
    CHECK(c.notes[2].kind == kMine);
    CHECK(c.focusAt(10) == 0 && c.focusAt(1500) == 1);
    CHECK(near(c.bpmAt(0), 150));
}

static Chart simpleChart() {
    Chart c;
    c.keys = 4;
    auto add = [&](double t, int lane, int side, double len = 0, uint8_t kind = kNormal) {
        Note n; n.time = t; n.lane = uint8_t(lane); n.side = uint8_t(side); n.length = len; n.kind = kind;
        c.notes.push_back(n);
    };
    add(1000, 0, 1);
    add(1500, 1, 1);
    add(2000, 2, 1, 1000);
    add(4000, 3, 1);
    add(5000, 0, 1, 0, kMine);
    add(6000, 1, 1);
    add(1000, 0, 0);
    c.finalize(120);
    return c;
}

static void testSession() {
    Chart c = simpleChart();
    Session s;
    s.init(c, SessionConfig{});
    CHECK(s.totalPlayerNotes == 5);
    s.update(900);
    s.press(0, 1000);             // perfect
    s.release(0, 1050);
    s.update(1100);
    CHECK(s.counts[kDivine] == 1);
    bool oppHit = false;
    for (auto& e : s.events) oppHit |= e.type == GameEvent::OpponentHit;
    CHECK(oppHit);
    s.events.clear();
    s.press(1, 1600);             // 100 ms late -> Mortal
    s.release(1, 1650);
    CHECK(s.counts[kMortal] == 1);
    s.press(2, 1990);             // hold start
    s.update(2500);
    CHECK(s.isHolding(1, 2));
    s.release(2, 2500);           // released 500 ms early -> dropped
    CHECK(s.misses == 1 && s.combo == 0);
    s.update(4300);               // lane 3 note at 4000 passes -> miss
    CHECK(s.misses == 2);
    s.press(0, 5000);             // mine
    s.release(0, 5010);
    CHECK(s.health < 1.0);
    s.update(7000);               // last note missed
    CHECK(s.misses == 3);
    CHECK(s.allResolved());
    CHECK(s.accuracy() < 1.0 && s.rank() != "S+");

    // Ghost tapping off punishes empty presses.
    SessionConfig strict;
    strict.ghostTapping = false;
    s.init(c, strict);
    s.press(3, 100);
    CHECK(s.misses == 1);

    // Botplay clears everything perfectly.
    SessionConfig bot;
    bot.botplay = true;
    s.init(c, bot);
    for (double t = 0; t <= 8000; t += 16.6) s.update(t);
    CHECK(s.misses == 0 && s.counts[kDivine] == 5);
    CHECK(near(s.accuracy(), 1.0));
    CHECK(s.rank() == "S+" && s.clearLabel() == "SANCTIFIED");

    // Playback rate widens windows in song time (constant in real time).
    SessionConfig fast;
    fast.rate = 1.5;
    s.init(c, fast);
    s.press(0, 1060);  // 60 ms song time = 40 ms real -> Divine
    CHECK(s.counts[kDivine] == 1);
}

static void testComposer(bool audio, const char* wavDir) {
    for (const SongDef& def : builtinSongs()) {
        auto t0 = std::chrono::steady_clock::now();
        auto comp = compose(def, audio);
        auto t1 = std::chrono::steady_clock::now();
        auto again = compose(def, false);
        CHECK(comp->melody.size() == again->melody.size());
        CHECK(!comp->melody.empty());
        size_t prev = 0;
        for (const std::string& d : def.diffs) {
            Chart c;
            CHECK(buildChart(def, *comp, d, c));
            CHECK(c.playerNotes > 20 && c.opponentNotes > 20);
            CHECK(c.lengthMs < comp->lengthSec * 1000.0);
            CHECK(c.notes.size() >= prev);
            prev = c.notes.size();
            bool sorted = true;
            for (size_t i = 1; i < c.notes.size(); ++i) sorted &= c.notes[i - 1].time <= c.notes[i].time;
            CHECK(sorted);
            Session s;
            SessionConfig bot;
            bot.botplay = true;
            s.init(c, bot);
            for (double t = -1000; t <= c.lengthMs + 1000; t += 8.0) s.update(t);
            CHECK(s.misses == 0 && s.allResolved());
            std::printf("  %-13s %-9s notes P%5zu O%5zu  len %.1fs\n", def.title.c_str(), d.c_str(), c.playerNotes,
                        c.opponentNotes, c.lengthMs / 1000.0);
        }
        if (audio) {
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::printf("  rendered %s in %.0f ms (%zu frames)\n", def.title.c_str(), ms, comp->inst.size() / 2);
            CHECK(comp->inst.size() > 44100 * 60);
            CHECK(comp->voiceO.size() * 2 == comp->inst.size());
            int peak = 0;
            for (int16_t v : comp->inst) peak = std::max(peak, std::abs(int(v)));
            CHECK(peak > 8000);
            if (wavDir) {
                // Write a mixed WAV for listening/analysis.
                std::string path = std::string(wavDir) + "/" + def.id + ".wav";
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f) {
                    const uint32_t frames = uint32_t(comp->inst.size() / 2), dataBytes = frames * 4;
                    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
                    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
                    std::fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
                    w32(16); w16(1); w16(2); w32(44100); w32(44100 * 4); w16(4); w16(16);
                    std::fwrite("data", 1, 4, f); w32(dataBytes);
                    for (uint32_t i = 0; i < frames; ++i) {
                        int vo = comp->voiceO[i], vp = comp->voiceP[i];
                        for (int ch = 0; ch < 2; ++ch) {
                            int v = comp->inst[2 * i + ch] + vo + vp;
                            v = std::max(-32768, std::min(32767, v));
                            int16_t s16 = int16_t(v);
                            std::fwrite(&s16, 2, 1, f);
                        }
                    }
                    std::fclose(f);
                    std::printf("  wrote %s\n", path.c_str());
                }
            }
        }
    }
    auto menu = compose(menuTheme(), audio);
    CHECK(menu->melody.empty());
    if (audio) CHECK(std::fabs(double(menu->inst.size() / 2) - menu->loopSec * 44100.0) < 2.0);
}

// Parses real chart files and plays them with the bot: gh_tests --chart a.json b.json ...
// --keys <charts...>: the key count and note totals of each chart (no gameplay).
static int printKeys(int argc, char** argv, int first) {
    for (int i = first; i < argc; ++i) {
        Chart c;
        std::string err;
        if (!loadChartFile(argv[i], "Hard", c, err)) {
            std::printf("FAILED  %s: %s\n", argv[i], err.c_str());
            continue;
        }
        std::printf("%dK  P%llu O%llu  %s\n", c.keys, (unsigned long long)c.playerNotes, (unsigned long long)c.opponentNotes,
                    argv[i]);
    }
    return 0;
}

static int benchCharts(int argc, char** argv, int first) {
    int bad = 0;
    for (int i = first; i < argc; ++i) {
        Chart c;
        std::string err;
        const double sizeMb = double(fileSize(argv[i])) / 1e6;
        auto t0 = std::chrono::steady_clock::now();
        const bool ok = loadChartFile(argv[i], "Hard", c, err);
        auto t1 = std::chrono::steady_clock::now();
        if (!ok) { std::printf("%-48s FAILED: %s\n", argv[i], err.c_str()); ++bad; continue; }
        Session s;
        SessionConfig bot;
        bot.botplay = true;
        s.init(c, bot);
        for (double t = -1000; t <= c.lengthMs + 1000; t += 4.0) { s.update(t); s.events.clear(); }
        auto t2 = std::chrono::steady_clock::now();
        const double pm = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double sm = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::printf("%-48s %7.1f MB  %dK  raw %10llu  notes P%8zu O%8zu  bpm %.0f  len %6.1fs  parse %6.0f ms  sim %6.0f ms  %s\n",
                    argv[i], sizeMb, c.keys, (unsigned long long)c.rawNoteCount, c.playerNotes, c.opponentNotes, c.bpmAt(0), c.lengthMs / 1000.0, pm, sm,
                    (s.misses == 0 && s.allResolved()) ? "ok" : "BOT MISSED");
        if (s.misses != 0) ++bad;
    }
#if defined(__linux__)
    if (FILE* st = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, st))
            if (!std::strncmp(line, "VmHWM:", 6)) std::printf("peak memory %s", line + 6);
        std::fclose(st);
    }
#endif
    return bad == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    bool audio = false;
    const char* wav = nullptr;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--chart")) return benchCharts(argc, argv, i + 1);
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--keys")) return printKeys(argc, argv, i + 1);
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--psych-corpus")) return runPsychCorpus(argv[i + 1]);
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--flp")) return printFlp(argc, argv, i + 1);
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--audio")) audio = true;
        if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) { wav = argv[++i]; audio = true; }
    }
    testJson();
    testLegacyChart();
    testPsychV1AndExtraKeys();
    testVSlice();
    testSession();
    runPsychTests();
    runScanTests();
#ifdef GH_AUDIO_TESTS
    runAudioTests();
#endif
    runExtraTests(audio);
    testComposer(audio, wav);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
