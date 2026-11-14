// .rchr writer/reader — see rchr.hpp for the format. Self-contained in
// gh_core (no raylib) so both raid_engine.exe and the game can use it.
#include "rchr.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "miniz.h"

namespace gh::rchr {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'R', 'C', 'H', 'R'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 96;
constexpr size_t kIndexEntrySize = 40;

void putU16(void* b_, size_t at, uint16_t v) {
    auto* b = static_cast<unsigned char*>(b_);
    b[at] = char(v & 0xFF); b[at + 1] = char(v >> 8);
}
void putU32(void* b_, size_t at, uint32_t v) {
    auto* b = static_cast<unsigned char*>(b_);
    for (int i = 0; i < 4; ++i) b[at + i] = char((v >> (8 * i)) & 0xFF);
}
void putU64(void* b_, size_t at, uint64_t v) {
    auto* b = static_cast<unsigned char*>(b_);
    for (int i = 0; i < 8; ++i) b[at + i] = char((v >> (8 * i)) & 0xFF);
}
uint16_t getU16(const unsigned char* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t getU32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t getU64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

uint32_t crcOf(const void* p, size_t n) {
    return uint32_t(mz_crc32(0, reinterpret_cast<const unsigned char*>(p), n));
}

// Whole-file crc32, used for the cache dir name.
uint32_t fileCrc(const std::string& path, bool& ok) {
    FILE* f = std::fopen(path.c_str(), "rb");
    ok = f != nullptr;
    if (!f) return 0;
    uint32_t crc = 0;
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) crc = mz_crc32(crc, reinterpret_cast<const unsigned char*>(buf), n);
    std::fclose(f);
    return crc;
}

bool readAll(const std::string& path, std::vector<char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out.resize(size_t(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, size_t(n), f) == size_t(n);
    std::fclose(f);
    return ok;
}

bool deflate(const char* src, size_t n, std::vector<char>& out) {
    mz_ulong cap = mz_compressBound(mz_ulong(n));
    out.resize(cap);
    mz_ulong dst = cap;
    if (mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &dst,
                     reinterpret_cast<const unsigned char*>(src), mz_ulong(n), 6) != MZ_OK)
        return false;
    out.resize(dst);
    return true;
}

bool inflate(const char* src, size_t n, size_t rawSize, std::vector<char>& out) {
    out.resize(rawSize);
    mz_ulong dst = mz_ulong(rawSize);
    if (mz_uncompress(reinterpret_cast<unsigned char*>(out.data()), &dst,
                      reinterpret_cast<const unsigned char*>(src), mz_ulong(n)) != MZ_OK || dst != rawSize)
        return false;
    return true;
}

// Normalizes an arcname: backslashes to '/', no leading "./" or '/'.
std::string normName(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

struct Parsed {
    uint32_t entryCount = 0, nameBytes = 0;
    uint64_t indexOffset = 0, indexSize = 0, dataOffset = 0, dataSize = 0;
    std::vector<char> nameBlob;
    std::vector<unsigned char> index;  // raw index bytes
    std::vector<char> header;          // raw 96-byte header (for reads)
};

// Reads + validates the fixed parts of an archive. `f` must be open "rb".
bool parseHeader(FILE* f, Parsed& p, std::string& err) {
    p.header.resize(kHeaderSize);
    if (std::fread(p.header.data(), 1, kHeaderSize, f) != kHeaderSize) { err = "short header"; return false; }
    const unsigned char* h = reinterpret_cast<const unsigned char*>(p.header.data());
    if (std::memcmp(h, kMagic, 4) != 0) { err = "not an .rchr file"; return false; }
    if (getU16(h + 4) != kVersion) { err = "unsupported .rchr version"; return false; }
    if (crcOf(h, 48) != getU32(h + 48)) { err = "header crc mismatch"; return false; }
    p.entryCount = getU32(h + 8);
    p.nameBytes = getU32(h + 12);
    p.indexOffset = getU64(h + 16);
    p.indexSize = getU64(h + 24);
    p.dataOffset = getU64(h + 32);
    p.dataSize = getU64(h + 40);
    if (p.indexSize != uint64_t(p.entryCount) * kIndexEntrySize) { err = "bad index size"; return false; }
    if (p.indexOffset != kHeaderSize + p.nameBytes) { err = "bad index offset"; return false; }
    if (p.dataOffset != p.indexOffset + p.indexSize) { err = "bad data offset"; return false; }
    if (p.indexOffset > (1ull << 40) || p.dataOffset > (1ull << 40) || p.dataSize > (1ull << 40)) {
        err = "archive too large";
        return false;
    }
    p.nameBlob.resize(p.nameBytes);
    if (p.nameBytes && std::fread(p.nameBlob.data(), 1, p.nameBytes, f) != p.nameBytes) { err = "short name blob"; return false; }
    p.index.resize(size_t(p.indexSize));
    if (p.indexSize && std::fread(p.index.data(), 1, size_t(p.indexSize), f) != size_t(p.indexSize)) {
        err = "short index";
        return false;
    }
    return true;
}

const char* nameAt(const Parsed& p, const unsigned char* rec) {
    const uint32_t off = getU32(rec), len = getU32(rec + 4);
    if (uint64_t(off) + len > p.nameBlob.size()) return nullptr;
    return p.nameBlob.data() + off;
}

// Fills `out` from a parsed archive; f positioned anywhere (we seek).
bool entriesFrom(FILE* f, Parsed& p, std::vector<Entry>& out, std::string& err) {
    out.clear();
    out.reserve(p.entryCount);
    for (uint32_t i = 0; i < p.entryCount; ++i) {
        const unsigned char* rec = p.index.data() + size_t(i) * kIndexEntrySize;
        const char* nm = nameAt(p, rec);
        if (!nm) { err = "entry name out of range"; return false; }
        Entry e;
        e.path.assign(nm, getU32(rec + 4));
        e.offset = getU64(rec + 8);
        e.storedSize = getU64(rec + 16);
        e.rawSize = getU64(rec + 24);
        e.crc = getU32(rec + 32);
        e.method = rec[36];
        if (e.method > 1 || e.offset < p.dataOffset || e.offset + e.storedSize > p.dataOffset + p.dataSize ||
            e.storedSize > (1ull << 32) || e.rawSize > (1ull << 32)) {
            err = "bad entry " + e.path;
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

bool writeAll(FILE* f, const void* p, size_t n) { return std::fwrite(p, 1, n, f) == n; }

// Refuses paths that could escape destDir when extracted.
bool safeRel(const std::string& rel) {
    if (rel.empty() || rel.find('\\') != std::string::npos) return false;
    if (rel.front() == '/' || rel.find(':') != std::string::npos) return false;
    size_t pos = 0;
    while (true) {
        const size_t slash = rel.find('/', pos);
        const std::string part = rel.substr(pos, slash == std::string::npos ? slash : slash - pos);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

}  // namespace

bool isRchrFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    const bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, kMagic, 4) == 0;
    std::fclose(f);
    return ok;
}

bool pack(const std::string& outPath, const std::vector<std::pair<std::string, std::string>>& files, std::string& err) {
    if (files.empty()) { err = "nothing to pack"; return false; }
    std::vector<std::pair<std::string, std::vector<char>>> payloads;
    payloads.reserve(files.size());
    uint64_t nameBytes = 0;
    for (const auto& [arc, disk] : files) {
        const std::string name = normName(arc);
        if (!safeRel(name)) { err = "bad archive path: " + arc; return false; }
        std::vector<char> raw;
        if (!readAll(disk, raw)) { err = "cannot read " + disk; return false; }
        nameBytes += name.size();
        payloads.push_back({name, std::move(raw)});
    }
    // Name blob + per-entry stored bytes.
    std::vector<char> nameBlob;
    nameBlob.reserve(size_t(nameBytes));
    std::vector<std::vector<char>> stored;
    stored.reserve(payloads.size());
    std::vector<uint8_t> methods;
    methods.reserve(payloads.size());
    for (const auto& [name, raw] : payloads) {
        std::vector<char> comp;
        int method = 0;
        if (!raw.empty() && deflate(raw.data(), raw.size(), comp) && comp.size() < raw.size()) {
            stored.push_back(std::move(comp));
            method = 1;
        } else {
            stored.push_back(raw);
        }
        methods.push_back(uint8_t(method));
    }
    std::vector<uint32_t> nameOff(payloads.size());
    for (size_t i = 0; i < payloads.size(); ++i) {
        nameOff[i] = uint32_t(nameBlob.size());
        nameBlob.insert(nameBlob.end(), payloads[i].first.begin(), payloads[i].first.end());
    }

    const uint64_t indexOffset = kHeaderSize + nameBlob.size();
    const uint64_t indexSize = uint64_t(payloads.size()) * kIndexEntrySize;
    const uint64_t dataOffset = indexOffset + indexSize;
    uint64_t dataSize = 0;
    for (const auto& s : stored) dataSize += s.size();

    std::vector<char> header(kHeaderSize, 0);
    std::memcpy(header.data(), kMagic, 4);
    putU16(header.data(), 4, kVersion);
    putU32(header.data(), 8, uint32_t(payloads.size()));
    putU32(header.data(), 12, uint32_t(nameBlob.size()));
    putU64(header.data(), 16, indexOffset);
    putU64(header.data(), 24, indexSize);
    putU64(header.data(), 32, dataOffset);
    putU64(header.data(), 40, dataSize);
    // crc over [0,48) goes at 48
    putU32(header.data(), 48, crcOf(header.data(), 48));

    std::vector<unsigned char> index(size_t(indexSize), 0);
    uint64_t dataPos = dataOffset;
    for (size_t i = 0; i < payloads.size(); ++i) {
        unsigned char* rec = index.data() + i * kIndexEntrySize;
        putU32(rec, 0, nameOff[i]);
        putU32(rec, 4, uint32_t(payloads[i].first.size()));
        putU64(rec, 8, dataPos);
        putU64(rec, 16, stored[i].size());
        putU64(rec, 24, payloads[i].second.size());
        putU32(rec, 32, crcOf(payloads[i].second.data(), payloads[i].second.size()));
        rec[36] = methods[i];
        dataPos += stored[i].size();
    }

    FILE* f = std::fopen(outPath.c_str(), "wb");
    if (!f) { err = "cannot write " + outPath; return false; }
    bool ok = writeAll(f, header.data(), header.size()) &&
              writeAll(f, nameBlob.data(), nameBlob.size()) &&
              writeAll(f, index.data(), index.size());
    for (const auto& s : stored) ok = ok && writeAll(f, s.data(), s.size());
    std::fclose(f);
    if (!ok) { err = "write failed for " + outPath; return false; }
    return true;
}

bool list(const std::string& path, std::vector<Entry>& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    Parsed p;
    const bool ok = parseHeader(f, p, err) && entriesFrom(f, p, out, err);
    std::fclose(f);
    return ok;
}

bool readEntry(const std::string& path, const Entry& e, std::vector<char>& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    Parsed p;
    bool ok = parseHeader(f, p, err);
    std::fclose(f);
    if (!ok) return false;
    if (e.offset < p.dataOffset || e.offset + e.storedSize > p.dataOffset + p.dataSize) {
        err = "entry outside data region";
        return false;
    }
    f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<char> stored(size_t(e.storedSize));
    ok = e.storedSize == 0 || (std::fseek(f, long(e.offset), SEEK_SET) == 0 &&
                               std::fread(stored.data(), 1, size_t(e.storedSize), f) == size_t(e.storedSize));
    std::fclose(f);
    if (!ok) { err = "cannot read entry data"; return false; }
    if (e.method == 1) {
        ok = inflate(stored.data(), stored.size(), size_t(e.rawSize), out);
    } else {
        out = std::move(stored);
        ok = out.size() == size_t(e.rawSize);
    }
    if (!ok) { err = "cannot decompress " + e.path; return false; }
    if (crcOf(out.data(), out.size()) != e.crc) { err = "crc mismatch in " + e.path; return false; }
    return true;
}

bool extract(const std::string& rchrPath, const std::string& destDir, std::string& err) {
    std::vector<Entry> entries;
    if (!list(rchrPath, entries, err)) return false;
    for (const Entry& e : entries) {
        if (!safeRel(e.path)) { err = "unsafe path in archive: " + e.path; return false; }
        const fs::path out = fs::path(destDir) / fs::path(e.path);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        if (ec) { err = "cannot create " + out.parent_path().string(); return false; }
        std::vector<char> raw;
        if (!readEntry(rchrPath, e, raw, err)) return false;
        FILE* f = std::fopen(out.string().c_str(), "wb");
        if (!f) { err = "cannot write " + out.string(); return false; }
        const bool ok = raw.empty() || writeAll(f, raw.data(), raw.size());
        std::fclose(f);
        if (!ok) { err = "write failed for " + out.string(); return false; }
    }
    return true;
}

std::string mountDir(const std::string& rchrPath, const std::string& cacheRoot, std::string& err) {
    bool ok = false;
    const uint32_t crc = fileCrc(rchrPath, ok);
    if (!ok) { err = "cannot read " + rchrPath; return ""; }
    char hex[16];
    std::snprintf(hex, sizeof hex, "%08x", crc);
    const fs::path dir = fs::path(cacheRoot) / hex;
    // Already extracted? A sentinel file marks a complete extraction.
    const fs::path done = dir / ".rchr-ok";
    std::error_code ec;
    if (fs::exists(done, ec)) return dir.string();
    if (!extract(rchrPath, dir.string(), err)) return "";
    FILE* f = std::fopen(done.string().c_str(), "wb");
    if (f) { std::fputs("ok", f); std::fclose(f); }
    return dir.string();
}

}  // namespace gh::rchr
