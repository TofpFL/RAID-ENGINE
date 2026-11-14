// Finding songs in every mod layout (song_scan.cpp), PulseForge charts, audio
// lengths and assets shared between mods.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "flp_import.hpp"
#include "raid.hpp"
#include "scene.hpp"
#include "song_scan.hpp"

using namespace gh;
namespace fs = std::filesystem;

extern int g_fail, g_pass;
#define SCHECK(cond)                                                             \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

namespace {

std::string str(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A silent 8 kHz mono 8-bit WAV of the given length.
void putWav(const fs::path& p, double seconds) {
    const uint32_t rate = 8000, data = uint32_t(seconds * rate);
    std::string w = "RIFF";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w += char((v >> (8 * i)) & 0xFF); };
    auto u16 = [&](uint16_t v) { w += char(v & 0xFF); w += char(v >> 8); };
    u32(36 + data);
    w += "WAVEfmt ";
    u32(16);
    u16(1);      // PCM
    u16(1);      // mono
    u32(rate);
    u32(rate);   // bytes per second
    u16(1);
    u16(8);
    w += "data";
    u32(data);
    w.append(data, char(0x80));
    put(p, w);
}

std::string legacyChart(const std::string& song, int notes) {
    std::string s = "{\"song\":{\"song\":\"" + song + "\",\"bpm\":120,\"speed\":2,\"notes\":[{\"mustHitSection\":true,\"sectionNotes\":[";
    for (int i = 0; i < notes; ++i) s += (i ? "," : "") + std::string("[") + std::to_string(500 * i) + "," + std::to_string(i % 4) + ",0]";
    return s + "]}]}}";
}

const SongEntry* byTitle(const SongScan& s, const std::string& title, int nth = 0) {
    for (const SongEntry& e : s.songs)
        if (equalsI(e.title, title) && nth-- == 0) return &e;
    return nullptr;
}

int countTitle(const SongScan& s, const std::string& title) {
    int n = 0;
    for (const SongEntry& e : s.songs) n += equalsI(e.title, title);
    return n;
}

bool endsWith(const std::string& s, const std::string& suf) {
    // Compare with '/' separators: pathStr() yields native separators, which
    // are '\\' on Windows while the expectations below are written portably.
    auto norm = [](std::string v) {
        for (char& c : v) if (c == '\\') c = '/';
        return v;
    };
    const std::string a = norm(s), b = norm(suf);
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}

const char* kPulseForgeEditor = R"({"format":"pulseforge-editor-chart","version":1,
 "metadata":{"title":"Forge Song","difficulty":"hard","stage":"street","player":"bf","opponent":"pico","girlfriend":"gf"},
 "keyCount":6,"scrollSpeed":2.5,
 "tempos":[{"id":1,"time":0,"bpm":150,"numerator":4,"denominator":4},{"id":2,"time":4000,"bpm":200}],
 "notes":[{"id":2,"time":0,"duration":0,"lane":5,"owner":"player","kind":"normal","payloadId":0},
          {"id":3,"time":100,"duration":400,"lane":1,"owner":"opponent","kind":"normal"},
          {"id":4,"time":200,"duration":0,"lane":2,"owner":"girlfriend","kind":"normal"},
          {"id":5,"time":300,"duration":0,"lane":3,"owner":"player","kind":"Hurt Note"},
          {"id":6,"time":300.4,"duration":0,"lane":3,"owner":"player","kind":"Hurt Note"},
          {"id":7,"time":5000,"duration":5,"lane":0,"owner":"player"}],
 "events":[{"time":1000,"name":"Hey!","value1":"bf","value2":"0.6"},[2000,[["Change Scroll Speed","1.5","0"]]]]})";

void testPulseForgeCharts() {
    Chart c;
    std::string err;
    SCHECK(parseChart(kPulseForgeEditor, "Hard", c, err));
    SCHECK(c.title == "Forge Song");
    SCHECK(c.keys == 6);
    SCHECK(std::fabs(c.speed - 2.5) < 1e-9);
    SCHECK(c.player == "bf" && c.opponent == "pico" && c.gf == "gf" && c.stage == "street");
    SCHECK(c.tempo.size() == 2 && std::fabs(c.bpmAt(4500) - 200) < 1e-9);
    SCHECK(c.notes.size() == 5);  // the two hurt notes 0.4 ms apart are one stacked note
    SCHECK(c.rawNoteCount == 6);
    SCHECK(c.playerNotes == 4 && c.opponentNotes == 2);
    if (c.notes.size() == 5) {
        SCHECK(c.notes[0].side == 1 && c.notes[0].lane == 5);
        SCHECK(c.notes[1].side == 0 && c.notes[1].lane == 1 && std::fabs(c.notes[1].length - 400) < 1e-9);
        SCHECK(c.notes[2].side == 0 && (c.notes[2].flags & kFlagGF));
        SCHECK(c.notes[3].kind == kMine && c.notes[3].stack == 2);
        SCHECK(c.notes[4].length == 0);  // shorter than a step: a tap
    }
    SCHECK(c.events.size() == 2 && c.events[0].name == "Hey!" && c.events[0].v2 == "0.6" &&
           c.events[1].name == "Change Scroll Speed" && c.events[1].v1 == "1.5");

    // Extra keys off: 4 lanes, as for every other chart.
    Chart four;
    SCHECK(parseChart(kPulseForgeEditor, "Hard", four, err, {}, false));
    SCHECK(four.keys == 4 && !four.notes.empty() && four.notes[0].lane == 1);

    // The exported format keeps the scene in "song".
    const char* exported = R"({"format":"pulseforge-chart","version":1,
      "song":{"title":"Fantrack","difficulty":"expert","keyCount":4,"scrollSpeed":1},
      "audio":{"instrumental":"Inst.mp3","vocals":[]},
      "tempos":[{"time":0,"bpm":193.9655,"numerator":4,"denominator":4}],
      "notes":[{"time":725.3,"duration":0,"lane":2,"owner":"player","kind":"normal"},
               {"time":826.6,"duration":0,"lane":1,"owner":"opponent","kind":"normal"}],"events":[]})";
    Chart e;
    SCHECK(parseChart(exported, "Expert", e, err));
    SCHECK(e.title == "Fantrack" && e.keys == 4 && e.notes.size() == 2 && e.playerNotes == 1);

    // Big files are streamed: the same chart read either way.
    const fs::path f = fs::temp_directory_path() / "gh_pulseforge_stream.pfchart.json";
    put(f, kPulseForgeEditor);
    Chart streamed;
    SCHECK(loadChartFile(str(f), "Hard", streamed, err, {}, 1));
    SCHECK(streamed.notes.size() == c.notes.size() && streamed.rawNoteCount == 6 && streamed.keys == 6);
    SCHECK(streamed.playerNotes == 4 && streamed.opponentNotes == 2 && streamed.events.size() == 2);
    if (streamed.notes.size() == 5) {
        SCHECK(streamed.notes[2].side == 0 && (streamed.notes[2].flags & kFlagGF));
        SCHECK(streamed.notes[3].kind == kMine && streamed.notes[3].stack == 2);
    }
    const ChartPeek pk = peekChart(str(f));
    SCHECK(pk.pulseForge && pk.isChart && pk.song == "Forge Song" && pk.difficulty == "hard");
    fs::remove(f);
}

void testAudioSeconds() {
    const fs::path dir = fs::temp_directory_path() / "gh_audio_len";
    fs::remove_all(dir);
    putWav(dir / "a.wav", 3.5);
    SCHECK(std::fabs(audioSeconds(str(dir / "a.wav")) - 3.5) < 0.01);

    // Ogg Vorbis: the rate from the identification header, the length from the last page.
    std::string ogg = "OggS";
    ogg.append(1, '\0');
    ogg.append(1, '\2');
    ogg.append(8, '\0');
    ogg.append(12, '\0');
    ogg += char(1);
    ogg += char(30);
    ogg += "\x01vorbis";
    ogg.append(4, '\0');
    ogg += char(2);
    const uint32_t rate = 44100;
    for (int i = 0; i < 4; ++i) ogg += char((rate >> (8 * i)) & 0xFF);
    ogg.append(2000, 'x');
    ogg += "OggS";
    ogg.append(1, '\0');
    ogg.append(1, '\4');
    const uint64_t granule = uint64_t(rate) * 95;
    for (int i = 0; i < 8; ++i) ogg += char((granule >> (8 * i)) & 0xFF);
    ogg.append(16, '\0');
    put(dir / "b.ogg", ogg);
    SCHECK(std::fabs(audioSeconds(str(dir / "b.ogg")) - 95) < 0.01);

    // MP3: 128 kbps frames after an ID3 tag.
    std::string mp3 = "ID3";
    mp3 += char(4);
    mp3.append(2, '\0');
    mp3 += std::string("\0\0\0\x0A", 4);  // tag of 10 bytes
    mp3.append(10, '\0');
    mp3 += "\xFF\xFB\x90\x00";
    mp3.append(1600000 - mp3.size(), '\0');
    put(dir / "c.mp3", mp3);
    const double secs = audioSeconds(str(dir / "c.mp3"));
    SCHECK(secs > 99 && secs < 101);
    fs::remove_all(dir);
}

void testModLayouts() {
    const fs::path base = fs::temp_directory_path() / "gh_scan_layouts";
    fs::remove_all(base);
    const fs::path mods = base / "mods";

    // A Psych Engine build: charts in assets/preload/data/<song>/ and
    // assets/shared/data/<song>/, audio in assets/songs/<song>/.
    const fs::path game = mods / "packA" / "Game Build";
    const fs::path assets = game / "assets";
    put(assets / "songs" / "foo" / "Inst.ogg", "x");
    put(assets / "songs" / "foo" / "Voices.ogg", "x");
    put(assets / "preload" / "data" / "foo" / "foo.json", legacyChart("Foo", 8));
    put(assets / "preload" / "data" / "foo" / "foo-hard.json", legacyChart("Foo", 12));
    put(assets / "preload" / "data" / "foo" / "dialogue.json", R"({"dialogue":[{"portrait":"bf","text":"hi","speed":0.05}]})");
    put(assets / "songs" / "qux" / "Inst.ogg", "x");
    put(assets / "shared" / "data" / "qux" / "qux.json", legacyChart("Qux", 4));
    put(assets / "shared" / "images" / "stageback.png", "png");
    put(assets / "shared" / "data" / "characters" / "bf.json", R"({"animations":[],"image":"characters/BOYFRIEND","scale":1})");
    // data/charts/<song>/ and a chart straight in data/
    put(assets / "songs" / "bar" / "Inst.ogg", "x");
    put(assets / "data" / "charts" / "bar" / "bar.json", legacyChart("Bar", 5));
    put(assets / "data" / "loose-song.json", legacyChart("Loose Song", 6));
    // The build's mods/ folder holds charts of its own and a mod folder.
    put(game / "mods" / "data" / "global" / "global.json", legacyChart("Global", 3));
    put(game / "mods" / "songs" / "global" / "Inst.ogg", "x");
    put(game / "mods" / "MyMod" / "data" / "baz" / "baz.json", legacyChart("Baz", 7));
    put(game / "mods" / "MyMod" / "songs" / "baz" / "Inst.ogg", "x");
    put(game / "mods" / "MyMod" / "characters" / "pico.json", R"({"animations":[]})");
    // An exported copy of the build deep inside: the same chart is offered once.
    const fs::path exportAssets = game / "export" / "release" / "windows" / "bin" / "assets";
    put(exportAssets / "data" / "foo" / "foo.json", legacyChart("Foo", 8));
    put(exportAssets / "songs" / "foo" / "Inst.ogg", "x");
    // ...and a second, different version of a song: both are offered, with their folder.
    put(exportAssets / "data" / "qux" / "qux.json", legacyChart("Qux", 9));
    put(exportAssets / "songs" / "qux" / "Inst.ogg", "x");

    // A PulseForge chart with its audio; editor projects and autosaves.
    const fs::path packB = mods / "packB";
    put(packB / "chart.json", R"({"format":"pulseforge-chart","version":1,"song":{"title":"Fantrack","difficulty":"expert","keyCount":4},
        "tempos":[{"time":0,"bpm":120}],"notes":[{"time":0,"duration":0,"lane":1,"owner":"player","kind":"normal"}],"events":[]})");
    put(packB / "Inst.mp3", "x");
    put(packB / "autosaves" / "victim-easy.autosave.pfchart.json", kPulseForgeEditor);
    put(packB / "autosaves" / "untitled.autosave.pfchart.json",
        R"({"format":"pulseforge-editor-chart","version":1,"metadata":{"title":"Empty"},"keyCount":4,"notes":[],"events":[]})");
    put(packB / "musical-conversions" / "template.pfm.json", R"({"format":"pulseforge-pfm-source-v1","notes":[],"scrollSpeed":1})");
    put(packB / "images" / "icons" / "icon-pico.png", "png");
    // A recovered chart with only events: nothing to play, no song.
    put(packB / "charts" / "_unknown" / "unknown-1.json",
        R"({"song":{"song":"Mystery","bpm":150,"events":[[100,[["Hey!","",""]]]],"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})");
    // A lone section recovered from a bigger chart plays as a chart.
    put(packB / "charts" / "_fragments" / "frag-1.json",
        R"({"lengthInSteps":16,"mustHitSection":false,"sectionNotes":[[100,0,0],[200,5,0]]})");
    // Title screen and freeplay files in data/ are no songs.
    put(assets / "data" / "title" / "offsets.json", R"({"bpm":102,"song":"freakyMenu","x":0})");

    // Songs without charts: AutoChart for songs, not for sound effects.
    const fs::path packC = mods / "packC" / "loose songs";
    putWav(packC / "Long Song_Inst.wav", 31);
    putWav(packC / "Long Song_Voices.wav", 31);
    putWav(packC / "whoosh.wav", 2);
    putWav(packC / "Inst.wav", 40);  // named after its folder

    SongScan s;
    scanModsDir(str(mods), s);
    if (std::getenv("GH_SCAN_DEBUG"))
        for (const SongEntry& e : s.songs) {
            std::printf("%s | %s | %s | inst %s\n", e.id.c_str(), e.title.c_str(), e.subtitle.c_str(), e.inst.c_str());
            for (size_t i = 0; i < e.diffs.size(); ++i) std::printf("   [%s] %s\n", e.diffs[i].c_str(), e.chartPaths[i].c_str());
        }

    const SongEntry* foo = byTitle(s, "Foo");
    SCHECK(foo && !foo->autoChart && foo->diffs.size() == 2);
    SCHECK(countTitle(s, "Foo") == 1);
    if (foo) {
        SCHECK(foo->chartPaths.size() == 2 && foo->chartPaths[0].find("preload") != std::string::npos);
        SCHECK(endsWith(foo->inst, "assets/songs/foo/Inst.ogg") && endsWith(foo->voices, "Voices.ogg"));
        SCHECK(foo->pack == "packA");
        for (const std::string& d : foo->diffs) SCHECK(!equalsI(d, "Dialogue"));
        bool hasShared = false;
        for (const std::string& r : foo->assetRoots) hasShared |= endsWith(r, "assets/shared/");
        SCHECK(hasShared);
    }
    SCHECK(countTitle(s, "Qux") == 2);
    const SongEntry* qux = byTitle(s, "Qux");
    SCHECK(qux && !qux->autoChart && endsWith(qux->inst, "Inst.ogg"));
    if (const SongEntry* q2 = byTitle(s, "Qux", 1)) SCHECK(q2->subtitle != qux->subtitle);
    SCHECK(!byTitle(s, "Characters"));
    const SongEntry* bar = byTitle(s, "Bar");
    SCHECK(bar && !bar->autoChart && endsWith(bar->inst, "assets/songs/bar/Inst.ogg"));
    const SongEntry* loose = byTitle(s, "Loose Song");
    SCHECK(loose && loose->chartPaths.size() == 1 && endsWith(loose->modRoot, "assets/"));
    SCHECK(byTitle(s, "Global") && byTitle(s, "Baz"));
    if (const SongEntry* baz = byTitle(s, "Baz")) SCHECK(endsWith(baz->inst, "MyMod/songs/baz/Inst.ogg"));

    const SongEntry* fan = byTitle(s, "Fantrack");
    SCHECK(fan && fan->diffs.size() == 1 && fan->diffs[0] == "Expert" && endsWith(fan->inst, "packB/Inst.mp3"));
    const SongEntry* victim = byTitle(s, "Forge Song");
    SCHECK(victim && victim->diffs.size() == 1 && victim->diffs[0] == "Hard (autosave)");  // the chart's own difficulty
    SCHECK(!byTitle(s, "Empty") && !byTitle(s, "Template"));
    SCHECK(!byTitle(s, "Mystery") && !byTitle(s, "Title") && !byTitle(s, "Offsets"));
    const SongEntry* frag = byTitle(s, "Frag");
    SCHECK(frag && frag->chartPaths.size() == 1);
    if (frag && !frag->chartPaths.empty()) {
        Chart fc;
        std::string ferr;
        SCHECK(loadChartFile(frag->chartPaths[0], "Normal", fc, ferr));
        SCHECK(fc.notes.size() == 2 && fc.opponentNotes == 1 && fc.playerNotes == 1);
    }

    const SongEntry* longSong = byTitle(s, "Long Song");
    SCHECK(longSong && longSong->autoChart && endsWith(longSong->inst, "Long Song_Inst.wav") &&
           endsWith(longSong->voices, "Long Song_Voices.wav"));
    SCHECK(byTitle(s, "Loose Songs") && byTitle(s, "Loose Songs")->autoChart);
    SCHECK(!byTitle(s, "Whoosh"));

    std::vector<std::string> ids;
    for (const SongEntry& e : s.songs) ids.push_back(e.id);
    std::sort(ids.begin(), ids.end());
    SCHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Asset folders: the song's own mod first, then the other mods.
    SCHECK(s.packRoots.count("packA") && s.packRoots.count("packB"));
    if (fan) {
        const std::vector<std::string> shared = sharedRootsFor(s, *fan);
        SCHECK(!shared.empty() && endsWith(shared.front(), "packB/"));
        bool packA = false;
        for (const std::string& r : shared) packA |= endsWith(r, "MyMod/") || endsWith(r, "assets/shared/");
        SCHECK(packA);
    }

    // The song's own folders first; another mod's files only through find().
    scene::Files files;
    files.setRoots({str(packB) + "/"});
    files.setSharedRoots(fan ? sharedRootsFor(s, *fan) : std::vector<std::string>{});
    SCHECK(endsWith(files.find("images/icons/icon-pico.png"), "packB/images/icons/icon-pico.png"));
    SCHECK(endsWith(files.find("characters/pico.json"), "MyMod/characters/pico.json"));
    SCHECK(endsWith(files.find("images/stageback.png"), "assets/shared/images/stageback.png"));
    SCHECK(files.find("characters/nobody.json").empty());
    SCHECK(files.list("characters", ".json").empty());  // scripts of other mods never run

    // A disabled mod is left out.
    put(mods / "modsList.txt", "packC|0\n");
    SongScan off;
    scanModsDir(str(mods), off);
    SCHECK(!byTitle(off, "Long Song") && byTitle(off, "Foo"));
    fs::remove_all(base);
}

// A small FL Studio project: an FPC channel with a chart in pattern 1
// ("hard"), one note in pattern 2 ("easy"), a melody in pattern 3, and a
// second channel whose notes are not the chart's.
std::string makeFlp() {
    std::string ev;
    auto byte = [&](uint8_t id, uint8_t v) { ev += char(id); ev += char(v); };
    auto word = [&](uint8_t id, uint16_t v) { ev += char(id); ev += char(v & 255); ev += char(v >> 8); };
    auto dword = [&](uint8_t id, uint32_t v) {
        ev += char(id);
        for (int i = 0; i < 4; ++i) ev += char((v >> (8 * i)) & 255);
    };
    auto array = [&](uint8_t id, const std::string& data) {
        ev += char(id);
        size_t n = data.size();
        do {
            uint8_t c = uint8_t(n & 0x7F);
            n >>= 7;
            if (n) c |= 0x80;
            ev += char(c);
        } while (n);
        ev += data;
    };
    auto utf16 = [](const std::string& a) {
        std::string w;
        for (char c : a) { w += c; w += char(0); }
        return w + std::string(2, '\0');
    };
    struct N { uint32_t t; uint16_t ch; uint32_t dur, pitch; uint8_t vel; };
    auto notes = [](const std::vector<N>& v) {
        std::string d;
        for (const N& n : v) {
            std::string r(24, '\0');
            for (int i = 0; i < 4; ++i) r[size_t(i)] = char((n.t >> (8 * i)) & 255);
            r[6] = char(n.ch & 255);
            for (int i = 0; i < 4; ++i) r[size_t(8 + i)] = char((n.dur >> (8 * i)) & 255);
            for (int i = 0; i < 4; ++i) r[size_t(12 + i)] = char((n.pitch >> (8 * i)) & 255);
            r[21] = char(n.vel);
            d += r;
        }
        return d;
    };
    array(0xC7, "20.8.4.2576");
    dword(0x9C, 150000);  // 150 BPM
    array(0xC2, utf16("Forge Test"));
    word(0x40, 0);
    array(0xC9, utf16("FPC"));
    word(0x40, 1);
    array(0xC9, utf16("3x Osc"));
    word(0x40, 2);
    array(0xC9, utf16("Cam Events"));
    byte(0x1C, 3);
    word(0x41, 1);
    array(0xC1, utf16("hard"));
    word(0x41, 2);
    array(0xC1, utf16("easy"));
    word(0x41, 1);
    array(0xE0, notes({{0, 2, 24, 54, 100},     // Cam Events: camera on the opponent
                       {0, 0, 24, 60, 100},     // opponent left
                       {96, 0, 24, 61, 100},    // opponent down, one beat in
                       {384, 2, 24, 53, 100},   // Cam Events: section 1 camera on the player
                       {384, 0, 96, 48, 100},   // player left, held a beat
                       {480, 0, 48, 51, 50},    // player right, soft: a hold
                       {768, 2, 24, 58, 100},   // Cam Events: section 2 player alt animation
                       {768, 0, 24, 49, 100},   // player down
                       {100, 1, 24, 60, 100}}));  // another channel: not the chart
    word(0x41, 2);
    array(0xE0, notes({{0, 0, 24, 50, 100}}));
    word(0x41, 3);
    array(0xE0, notes({{0, 0, 96, 65, 100}, {96, 0, 96, 67, 100}, {192, 0, 96, 69, 100}, {288, 0, 96, 60, 100},
                       {384, 0, 96, 72, 100}, {480, 0, 96, 71, 100}, {576, 0, 96, 64, 100}}));
    std::string f = "FLhd";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f += char((v >> (8 * i)) & 255); };
    u32(6);
    f += std::string("\0\0\x02\0", 4);
    f += char(96);
    f += char(0);
    f += "FLdt";
    u32(uint32_t(ev.size()));
    return f + ev;
}

// A project whose channels are named "Player"/"Opp": every key on those
// channels charts, the side coming from the channel, not the pitch window.
std::string makeFlpNamed() {
    std::string ev;
    auto word = [&](uint8_t id, uint16_t v) { ev += char(id); ev += char(v & 255); ev += char(v >> 8); };
    auto dword = [&](uint8_t id, uint32_t v) {
        ev += char(id);
        for (int i = 0; i < 4; ++i) ev += char((v >> (8 * i)) & 255);
    };
    auto array = [&](uint8_t id, const std::string& data) {
        ev += char(id);
        size_t n = data.size();
        do {
            uint8_t c = uint8_t(n & 0x7F);
            n >>= 7;
            if (n) c |= 0x80;
            ev += char(c);
        } while (n);
        ev += data;
    };
    auto utf16 = [](const std::string& a) {
        std::string w;
        for (char c : a) { w += c; w += char(0); }
        return w + std::string(2, '\0');
    };
    auto notes = [&](const std::vector<std::tuple<uint32_t, uint16_t, uint32_t>>& v) {
        std::string d;
        for (const auto& [t, ch, pitch] : v) {
            std::string r(24, '\0');
            for (int i = 0; i < 4; ++i) r[size_t(i)] = char((t >> (8 * i)) & 255);
            r[6] = char(ch & 255);
            for (int i = 0; i < 4; ++i) r[size_t(12 + i)] = char((pitch >> (8 * i)) & 255);
            r[21] = char(100);
            d += r;
        }
        return d;
    };
    dword(0x9C, 120000);
    word(0x40, 0);
    array(0xC9, utf16("Player"));
    word(0x40, 1);
    array(0xC9, utf16("Opp"));
    word(0x40, 2);
    array(0xC9, utf16("3x Osc"));
    word(0x40, 3);
    array(0xC9, utf16("Opp2"));
    word(0x40, 4);
    array(0xC9, utf16("Player 7"));
    word(0x40, 5);
    array(0xC9, utf16("PlayerX"));
    word(0x40, 6);
    array(0xC9, utf16("Opp0"));
    word(0x41, 1);
    array(0xC1, utf16("both"));
    word(0x41, 1);
    array(0xE0, notes({{0, 0, 72},     // Player channel, C5: outside the windows -> player lane 0
                       {96, 1, 65},    // Opp channel, F4 -> opponent lane 1
                       {192, 1, 50},   // Opp channel, D#3: a player-window pitch, still opponent lane 2
                       {288, 2, 60},   // another channel: not the chart
                       {384, 3, 60},   // "Opp2": same opponent side, lane 0
                       {480, 4, 62},   // "Player 7": same player side, lane 2
                       {576, 5, 60},   // "PlayerX": not an index, ignored
                       {672, 6, 60}}));  // "Opp0": index 0 is out of range, ignored
    std::string f = "FLhd";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f += char((v >> (8 * i)) & 255); };
    u32(6);
    f += std::string("\0\0\x02\0", 4);
    f += char(96);
    f += char(0);
    f += "FLdt";
    u32(uint32_t(ev.size()));
    return f + ev;
}

// An FL project with a playlist arrangement: three patterns placed at
// ticks, two of whose event spans overlap (merged literals on encode).
std::string makeFlpPlaylist() {
    std::string ev;
    auto word = [&](uint8_t id, uint16_t v) { ev += char(id); ev += char(v & 255); ev += char(v >> 8); };
    auto dword = [&](uint8_t id, uint32_t v) {
        ev += char(id);
        for (int i = 0; i < 4; ++i) ev += char((v >> (8 * i)) & 255);
    };
    auto array = [&](uint8_t id, const std::string& data) {
        ev += char(id);
        size_t n = data.size();
        do {
            uint8_t c = uint8_t(n & 0x7F);
            n >>= 7;
            if (n) c |= 0x80;
            ev += char(c);
        } while (n);
        ev += data;
    };
    auto notes = [](const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>& v) {
        std::string d;
        for (const auto& [t, dur, pitch] : v) {
            std::string r(24, '\0');
            for (int i = 0; i < 4; ++i) r[size_t(i)] = char((t >> (8 * i)) & 255);
            for (int i = 0; i < 4; ++i) r[size_t(8 + i)] = char((dur >> (8 * i)) & 255);
            for (int i = 0; i < 4; ++i) r[size_t(12 + i)] = char((pitch >> (8 * i)) & 255);
            r[21] = char(100);
            d += r;
        }
        return d;
    };
    dword(0x9C, 150000);  // 150 BPM, ppqn 96 -> 4166.67 us per tick
    word(0x40, 0);
    array(0xC9, std::string("F\0P\0C\0\0\0", 8));  // UTF-16LE "FPC"
    word(0x41, 1);
    array(0xE0, notes({{0, 24, 60}, {96, 24, 61}, {192, 24, 48}}));
    word(0x41, 2);
    array(0xE0, notes({{0, 24, 62}, {96, 24, 49}}));
    word(0x41, 3);
    array(0xE0, notes({{0, 24, 60}, {96, 24, 63}}));
    // Playlist: 32-byte records {i32 pos, i16 patBase=20480, i16 itemIdx,
    // i32 len, i16 trackRvidx=499 (track 1)}.
    std::string pl;
    for (const auto& [pat, st, ln] : {std::tuple<int, int, int>{1, 0, 576},
                                     {std::tuple<int, int, int>{3, 96, 480}},
                                     {std::tuple<int, int, int>{2, 576, 288}}}) {
        std::string r(32, '\0');
        auto p32 = [&](int off, uint32_t x) { for (int i = 0; i < 4; ++i) r[size_t(off + i)] = char((x >> (8 * i)) & 255); };
        auto p16 = [&](int off, uint16_t x) { r[size_t(off)] = char(x & 255); r[size_t(off + 1)] = char(x >> 8); };
        p32(0, uint32_t(st));
        p16(4, 20480);
        p16(6, uint16_t(20480 + pat));
        p32(8, uint32_t(ln));
        p16(12, 499);
        pl += r;
    }
    array(0xE9, pl);
    std::string f = "FLhd";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f += char((v >> (8 * i)) & 255); };
    u32(6);
    f += std::string("\0\0\x02\0", 4);
    f += char(96);
    f += char(0);
    f += "FLdt";
    u32(uint32_t(ev.size()));
    return f + ev;
}

void testFlStudio() {
    FlpProject p;
    std::string err;
    SCHECK(readFlp(makeFlp(), p, err));
    SCHECK(p.ppqn == 96 && std::fabs(p.bpm - 150) < 1e-9 && p.title == "Forge Test" && p.chartChannel == 0);
    SCHECK(p.patterns.size() == 3 && p.patterns[0].name == "hard" && p.patterns[1].name == "easy");
    SCHECK(flpChartPatterns(p, true) == std::vector<int>({1, 2, 3}));       // every key charts now
    SCHECK(flpChartPatterns(p, false) == std::vector<int>({1, 2, 3}));
    SCHECK(flpPatternLabel(p, 1) == "Hard" && flpPatternLabel(p, 3) == "Pattern 3");

    Chart c;
    SCHECK(flpToChart(p, 1, c, err));
    SCHECK(c.keys == 4 && c.notes.size() == 5 && c.playerNotes == 5 && c.opponentNotes == 0);  // pattern 1 votes player
    if (c.notes.size() == 5) {
        // 96 ticks per beat at 150 BPM: 400 ms a beat.
        SCHECK(c.notes[0].side == 1 && c.notes[0].lane == 0 && std::fabs(c.notes[0].time) < 1e-6 && c.notes[0].length == 0);
        SCHECK(c.notes[1].side == 1 && c.notes[1].lane == 1 && std::fabs(c.notes[1].time - 400) < 1e-6);
        SCHECK(c.notes[2].side == 1 && c.notes[2].lane == 0 && std::fabs(c.notes[2].time - 1600) < 1e-6 &&
               std::fabs(c.notes[2].length - 300) < 1e-6);  // (96 - 24) ticks
        SCHECK(c.notes[3].side == 1 && c.notes[3].lane == 3 && std::fabs(c.notes[3].length - 100) < 1e-6);  // soft: (48 - 24) ticks
        SCHECK(c.notes[4].side == 1 && c.notes[4].lane == 1 && (c.notes[4].flags & kFlagAlt) && std::fabs(c.notes[4].time - 3200) < 1e-6);
    }
    SCHECK(c.sections.size() == 3 && !c.sections[0].mustHit && c.sections[1].mustHit && c.sections[2].mustHit);
    SCHECK(c.focusAt(100) == 0 && c.focusAt(2000) == 1);
    SCHECK(std::fabs(c.bpmAt(0) - 150) < 1e-9 && c.title == "Forge Test");

    std::string file;
    int pat = 0;
    SCHECK(flpChartPath("a b/song.flp#2", file, pat) && file == "a b/song.flp" && pat == 2);
    SCHECK(flpChartPath("song.FSC", file, pat) && pat == -1);
    SCHECK(!flpChartPath("song.json", file, pat));

    // Channels named "Player"/"Opp" (indexed 1-50 too) chart every key by channel.
    FlpProject pn;
    SCHECK(readFlp(makeFlpNamed(), pn, err));
    SCHECK(pn.channelNames.size() == 7);
    SCHECK(flpChartPatterns(pn, true) == std::vector<int>({1}));
    Chart cn;
    SCHECK(flpToChart(pn, -1, cn, err));
    SCHECK(cn.notes.size() == 5 && cn.playerNotes == 2 && cn.opponentNotes == 3);
    if (cn.notes.size() == 5) {
        SCHECK(cn.notes[0].side == 1 && cn.notes[0].lane == 0);  // C5 on "Player"
        SCHECK(cn.notes[1].side == 0 && cn.notes[1].lane == 1);  // F4 on "Opp"
        SCHECK(cn.notes[2].side == 0 && cn.notes[2].lane == 2);  // D#3 on "Opp": channel wins over the window
        SCHECK(cn.notes[3].side == 0 && cn.notes[3].lane == 0);  // C4 on "Opp2": same side
        SCHECK(cn.notes[4].side == 1 && cn.notes[4].lane == 2);  // D4 on "Player 7": same side
    }

    // Through the chart loader and the song scan.
    const fs::path base = fs::temp_directory_path() / "gh_flp_scan";
    fs::remove_all(base);
    put(base / "mods" / "flpack" / "Forge Test.flp", makeFlp());
    Chart loaded;
    SCHECK(loadChartFile(str(base / "mods" / "flpack" / "Forge Test.flp") + "#2", "Easy", loaded, err) && loaded.notes.size() == 1);
    SongScan s;
    scanModsDir(str(base / "mods"), s);
    const SongEntry* e = byTitle(s, "Forge Test");
    SCHECK(e && e->diffs.size() == 3 && e->diffs[0] == "Easy (FL Studio)" && e->diffs[1] == "Hard (FL Studio)" && e->diffs[2] == "Pattern 3 (FL Studio)");
    fs::remove_all(base);

    // A project with a playlist encodes straight to .raid: patterns become
    // pattern-table definitions, clips become instances on the timeline, and
    // overlapping clips merge into sorted literals — one contiguous chart.
    FlpProject pp;
    SCHECK(readFlp(makeFlpPlaylist(), pp, err));
    SCHECK(pp.playlist.size() == 3 && pp.playlist[0].pattern == 1 && pp.playlist[0].startTick == 0 &&
           pp.playlist[1].pattern == 3 && pp.playlist[2].pattern == 2 && pp.playlist[2].startTick == 576);
    const fs::path raidPath = fs::temp_directory_path() / "gh_flp_playlist.raid";
    raid::Info rinfo;
    SCHECK(raid::writeRaidFlp(str(raidPath), pp, raid::EncodeOptions{}, &rinfo, err));
    SCHECK(rinfo.logicalEvents == 7);
    raid::Reader rd;
    SCHECK(rd.open(str(raidPath), err));
    std::vector<raid::Event> evs;
    SCHECK(rd.range(0, 3000000, evs, err));
    // 150 BPM / 96 ppqn -> 4166.67 us a tick: pat1@0 + pat3@96 ticks overlap,
    // pat2@576 ticks is isolated (a pattern instance).
    const int64_t T96 = 400000, T192 = 800000, T576 = 2400000, T672 = 2800000;
    SCHECK(evs.size() == 7);
    if (evs.size() == 7) {
        SCHECK(evs[0].timeUs == 0 && evs[0].lane == 0 && evs[0].side == 0);
        SCHECK(evs[1].timeUs == T96 && evs[1].lane == 1 && evs[1].side == 0);
        SCHECK(evs[2].timeUs == T96 && evs[2].lane == 0 && evs[2].side == 0);  // pat3's first note
        SCHECK(evs[3].timeUs == T192 && evs[3].lane == 0 && evs[3].side == 0);  // pat1 is an opponent pattern
        SCHECK(evs[4].timeUs == T192 && evs[4].lane == 3 && evs[4].side == 0);  // pat3's second note
        SCHECK(evs[5].timeUs == T576 && evs[5].lane == 2 && evs[5].side == 1);  // pat2 ties -> player
        SCHECK(evs[6].timeUs == T672 && evs[6].lane == 1 && evs[6].side == 1);
    }
    std::vector<raid::Reader::OpSegment> segs;
    SCHECK(rd.opLog(0, 3000000, segs, err));
    int kinds = 0;
    for (const auto& s : segs) kinds += s.kind == 2 ? 1 : 0;
    SCHECK(kinds == 1);  // only the isolated pat2 clip stayed a pattern instance
    rd.close();
    fs::remove(raidPath);
}

}  // namespace

// gh_tests --flp a.flp ...: what an FL Studio file holds, as SNIFF sees it.
int printFlp(int argc, char** argv, int first) {
    for (int i = first; i < argc; ++i) {
        FlpProject p;
        std::string err;
        if (!readFlpFile(argv[i], p, err)) {
            std::printf("%s: %s\n", argv[i], err.c_str());
            continue;
        }
        std::printf("%s: ppqn %d, bpm %.3f, title '%s', chart channel %d, %zu patterns\n", argv[i], p.ppqn, p.bpm,
                    p.title.c_str(), p.chartChannel, p.patterns.size());
        if (!p.playlist.empty()) {
            std::printf("  playlist: %zu clips\n", p.playlist.size());
            for (size_t i = 0; i < p.playlist.size() && i < 40; ++i)
                std::printf("    clip %zu: pat %d track %d @ %.2f s len %.2f s\n", i,
                            p.playlist[i].pattern, p.playlist[i].track,
                            p.playlist[i].startTick / double(p.ppqn) * 60.0 / p.bpm,
                            p.playlist[i].lengthTicks / double(p.ppqn) * 60.0 / p.bpm);
            if (p.playlist.size() > 40) std::printf("    ...\n");
        }
        for (const FlpPattern& pat : p.patterns) {
            size_t chart = 0, onChannel = 0;
            for (const FlpNote& n : pat.notes) {
                onChannel += n.channel == p.chartChannel;
                chart += n.channel == p.chartChannel && ((n.pitch >= 48 && n.pitch <= 51) || (n.pitch >= 60 && n.pitch <= 63));
            }
            std::printf("  pattern %d '%s': %zu notes, %zu on the chart channel, %zu in the chart lanes\n", pat.number,
                        pat.name.c_str(), pat.notes.size(), onChannel, chart);
        }
        Chart c;
        if (flpToChart(p, -1, c, err)) std::printf("  chart: %zu notes, %.1f s\n", c.notes.size(), c.lengthMs / 1000.0);
        else std::printf("  no chart: %s\n", err.c_str());
    }
    return 0;
}

void runScanTests() {
    testFlStudio();
    testPulseForgeCharts();
    testAudioSeconds();
    testModLayouts();
}
