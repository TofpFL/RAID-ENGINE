// Finding songs in every mod layout (song_scan.cpp), PulseForge charts, audio
// lengths and assets shared between mods.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
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
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
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

}  // namespace

void runScanTests() {
    testPulseForgeCharts();
    testAudioSeconds();
    testModLayouts();
}
