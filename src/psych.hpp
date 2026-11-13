// Psych Engine / FNF content formats: Sparrow and Packer atlases, character
// and stage JSON, chart events, easing and colours. Pure data, no rendering,
// so every parser here is covered by the headless tests.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace json { class Document; }

namespace gh::psych {

// ---------------------------------------------------------------- atlases --
struct AtlasFrame {
    std::string name;
    float x = 0, y = 0, w = 0, h = 0;     // rectangle inside the texture
    float fx = 0, fy = 0, fw = 0, fh = 0; // trim: offset (<= 0) and source size
    bool rotated = false;                 // stored rotated 90 degrees clockwise
    uint16_t tex = 0;                     // texture index for multi-atlas graphics
};

struct Atlas {
    std::string imagePath;  // as written in the XML (informational)
    std::vector<AtlasFrame> frames;

    bool parseSparrow(std::string_view xml, std::string* err = nullptr);
    bool parsePacker(std::string_view txt);  // "name = x y w h" lines
    // Flixel addByPrefix: frames whose name starts with prefix, ordered by the
    // number that follows the prefix.
    std::vector<int> byPrefix(std::string_view prefix) const;
    // Flixel addByIndices: for each index, the frame "prefix<number>" whose
    // number equals it (frames without a match are skipped).
    std::vector<int> byIndices(std::string_view prefix, const std::vector<int>& indices) const;
};
std::vector<int> framesByPrefix(const std::vector<AtlasFrame>& frames, std::string_view prefix);
std::vector<int> framesByIndices(const std::vector<AtlasFrame>& frames, std::string_view prefix,
                                 const std::vector<int>& indices);

// Width and height of a PNG from its header (no decoding). False if not a PNG.
bool pngSize(std::string_view bytes, int& w, int& h);

// ------------------------------------------------------------- characters --
struct AnimDef {
    std::string anim;   // name used by the game ("idle", "singLEFT", ...)
    std::string prefix; // atlas prefix
    double fps = 24;
    bool loop = false;
    std::vector<int> indices;
    float offX = 0, offY = 0;
};

struct CharacterDef {
    std::vector<std::string> images;  // one or more atlases ("characters/BOYFRIEND")
    double scale = 1.0;
    float posX = 0, posY = 0;         // added to the stage position
    float camX = 0, camY = 0;         // camera_position
    bool flipX = false;
    bool antialiasing = true;
    std::string icon = "face";
    uint32_t barColor = 0xA1A1A1;
    double singDuration = 4.0;
    std::vector<AnimDef> anims;
    const AnimDef* find(std::string_view anim) const;
};

// Psych 0.4-1.0 character JSON, plus V-Slice "data/characters" (sparrow /
// multisparrow render types).
bool parseCharacter(std::string_view json, CharacterDef& out, std::string& err);

// ----------------------------------------------------------------- stages --
struct StageObject {  // Psych 0.7.3+ stage editor objects
    std::string type;  // sprite, animatedSprite, square, gf, dad, boyfriend
    std::string name, image;
    float x = 0, y = 0, scaleX = 1, scaleY = 1, scrollX = 1, scrollY = 1;
    float alpha = 1, angle = 0;
    uint32_t color = 0xFFFFFF;
    bool flipX = false, flipY = false, antialiasing = true;
    std::vector<AnimDef> anims;
    std::string firstAnim;
};

struct StageDef {
    std::string directory;
    double zoom = 0.9;
    bool pixel = false, hideGf = false;
    float bf[2] = {770, 100}, gf[2] = {400, 130}, dad[2] = {100, 100};
    float camBf[2] = {0, 0}, camGf[2] = {0, 0}, camDad[2] = {0, 0};
    double camSpeed = 1.0;
    std::vector<StageObject> objects;
};
bool parseStage(std::string_view json, StageDef& out, std::string& err);

// ----------------------------------------------------------------- events --
struct ChartEvent {
    double timeMs = 0;
    std::string name, v1, v2;
};
// Psych "events" arrays ([[time, [[name, v1, v2], ...]], ...]), legacy event
// notes inside sectionNotes ([time, -1, name, v1, v2]) and separate
// events.json files. V-Slice events are mapped onto Psych equivalents.
void parseEvents(std::string_view json, std::vector<ChartEvent>& out);
void parseEvents(const json::Document& doc, std::vector<ChartEvent>& out);

// Chart header fields Psych uses for the scene.
struct SongMeta {
    std::string player = "bf", opponent = "dad", gf = "gf", stage;
    bool hasPlayer = false, hasOpponent = false, hasGf = false;
};
SongMeta parseSongMeta(std::string_view chartJson, std::string_view vsliceMeta = {});

// ----------------------------------------------------------- tween / colour --
double ease(std::string_view name, double t);  // FlxEase names, case-insensitive
// "FF0000", "0xFFFF0000", "#ff0000", "red", "0xFF0000" -> 0xAARRGGBB
uint32_t parseColor(std::string_view s, uint32_t fallback = 0xFFFFFFFF);

}  // namespace gh::psych
