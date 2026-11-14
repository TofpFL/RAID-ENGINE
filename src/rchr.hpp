// .rchr — RAID Character archive. One file holding a whole character pack:
// its Psych-format JSON, sprite PNGs, Lua scripts and icons, stored under
// their install-relative paths ("characters/bob.json", "images/bob.png"...).
// The game extracts an archive once into a cache dir and mounts that dir as
// a content root, so every existing character/stage/script lookup works
// unchanged — a .rchr is a portable mini-mod.
//
// Layout (all integers little-endian):
//   [0..95]   header, 96 bytes
//   [96..]    name blob: every entry's '/'-separated UTF-8 path, concatenated
//   index     entryCount records of 40 bytes
//   data      payloads, each stored raw or deflate-compressed (method byte)
//
// Header fields:
//   magic[4] "RCHR", u16 version (=1), u16 flags (=0),
//   u32 entryCount, u32 nameBytes,
//   u64 indexOffset, u64 indexSize, u64 dataOffset, u64 dataSize,
//   u32 headerCrc (crc32 of bytes [0,48)), then zero padding to 96.
//
// Index record (40 bytes):
//   u32 nameOff, u32 nameLen   (into the name blob)
//   u64 dataOff                (absolute file offset of the stored payload)
//   u64 storedSize, u64 rawSize
//   u32 crc                    (crc32 of the raw/uncompressed bytes)
//   u8  method                 (0 = stored, 1 = deflate)
//   u8  reserved[7]
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gh::rchr {

struct Entry {
    std::string path;        // install-relative, '/'-separated
    uint64_t offset = 0;     // absolute file offset of stored payload
    uint64_t storedSize = 0;
    uint64_t rawSize = 0;
    uint32_t crc = 0;        // of the raw bytes
    int method = 0;          // 0 store, 1 deflate
};

bool isRchrFile(const std::string& path);

// Packs files into a new archive. `files` pairs an install-relative arcname
// (forward slashes, no leading "./") with the disk path to read. Entries are
// stored when deflate would not shrink them (PNGs are already compressed).
bool pack(const std::string& outPath, const std::vector<std::pair<std::string, std::string>>& files,
          std::string& err);

// Lists an archive's entries (names + sizes; payloads are not read).
bool list(const std::string& path, std::vector<Entry>& out, std::string& err);

// Extracts every entry under destDir, creating directories, verifying CRCs.
bool extract(const std::string& rchrPath, const std::string& destDir, std::string& err);

// Reads and verifies one entry's raw bytes.
bool readEntry(const std::string& rchrPath, const Entry& e, std::vector<char>& out, std::string& err);

// Cache-mount helper for the game: extracts `rchrPath` into
// <cacheRoot>/<crc32-of-file-hex>/ (skipping work when already extracted)
// and returns that directory, or "" on failure. `err` gets a reason.
std::string mountDir(const std::string& rchrPath, const std::string& cacheRoot, std::string& err);

}  // namespace gh::rchr
