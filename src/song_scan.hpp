// Finding songs on disk: the game's songs/ folder and every mod in mods/, in
// whatever layout it came in. Psych mods, full game builds (charts in
// assets/preload/data or assets/shared/data next to assets/songs, the mods/
// folder of a build), chart libraries, loose charts and loose audio,
// PulseForge charts.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gh {

struct SongDef;

struct SongEntry {
    std::string id;        // stable key for scores
    std::string title;
    std::string subtitle;  // blurb or source pack
    bool builtin = false;
    const SongDef* def = nullptr;
    std::vector<std::string> diffs;
    std::vector<std::string> chartPaths;  // parallel to diffs (custom songs)
    std::string metaPath;                 // V-Slice metadata
    std::string inst, voices, voicesP, voicesO;
    uint32_t color = 0xB48CDC;
    std::string opponent;  // built-in character id, "" for custom
    // Psych content: the mod folder (or the song folder for songs/), the
    // chart folder and the song's folder name (data/<songKey>/).
    std::string modRoot, dataDir, songKey;
    // Every folder the scene looks in for images, characters, stages and
    // sounds: the content root plus, for full game builds, assets/shared,
    // assets/week1 and so on.
    std::vector<std::string> assetRoots;
    std::string pack;        // folder of the mod in mods/ ("" for songs/)
    bool autoChart = false;  // no chart: generated from the audio
    bool noAudio = false;    // chart found but no audio for it anywhere
};

struct SongScan {
    std::vector<SongEntry> songs;
    // Every folder of every enabled mod that holds assets (images/,
    // characters/, stages/, sounds/, music/, videos/, shaders/, fonts/), in
    // scan order, and the same folders by mod.
    std::vector<std::string> sharedRoots;
    std::map<std::string, std::vector<std::string>> packRoots;
};

// The game's songs/ folder: loose audio (AutoChart) and song folders.
void scanSongsDir(const std::string& songsDir, SongScan& out);
// Every enabled mod folder in modsDir (zips are unpacked before this).
void scanModsDir(const std::string& modsDir, SongScan& out);
// Where a song looks for the assets its own folders lack: the other asset
// folders of its own mod first, then every other mod's (mods reuse each
// other's characters, stages, images and sounds).
std::vector<std::string> sharedRootsFor(const SongScan& scan, const SongEntry& e);

// Length of an audio file from its headers (WAV, Ogg Vorbis/Opus, MP3, FLAC,
// QOA), in seconds; -1 when it cannot tell.
double audioSeconds(const std::string& path);

}  // namespace gh
