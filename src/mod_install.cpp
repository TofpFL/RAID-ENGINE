#include "mod_install.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

#include "common.hpp"
#include "miniz.h"

namespace gh {

namespace fs = std::filesystem;

namespace {
fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool safeRelative(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) return false;
    size_t p = 0;
    while (p <= name.size()) {
        size_t s = name.find_first_of("/\\", p);
        if (s == std::string::npos) s = name.size();
        if (name.compare(p, s - p, "..") == 0 && s - p == 2) return false;
        p = s + 1;
    }
    return true;
}

bool installOne(const fs::path& zipPath, const fs::path& modsDir, std::string& msg) {
    std::ifstream in(zipPath, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    mz_zip_archive zip{};
    if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
        msg = "not a zip archive";
        return false;
    }
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    std::vector<std::string> names;
    for (mz_uint i = 0; i < n; ++i) {
        char buf[1024];
        mz_zip_reader_get_filename(&zip, i, buf, sizeof buf);
        std::string s = buf;
        for (char& c : s)
            if (c == '\\') c = '/';
        names.push_back(s);
    }
    // Strip a leading "mods/" and use a single top-level folder as the mod name.
    std::string strip;
    bool allMods = !names.empty();
    for (const std::string& s : names) allMods &= s.rfind("mods/", 0) == 0;
    if (allMods) strip = "mods/";
    std::string top;
    bool single = true;
    for (const std::string& s0 : names) {
        const std::string s = s0.substr(strip.size());
        if (s.empty()) continue;
        const size_t slash = s.find('/');
        const std::string first = slash == std::string::npos ? "" : s.substr(0, slash);
        if (first.empty() || (!top.empty() && first != top)) { single = false; break; }
        top = first;
    }
    // A zip whose single folder is "data", "images"... is a mod's contents, not a mod.
    static const char* contentDirs[] = {"data", "images", "songs", "characters", "stages", "scripts", "weeks", "music", "sounds"};
    for (const char* c : contentDirs)
        if (single && equalsI(top, c)) single = false;
    fs::path dest = modsDir / (single ? toPath(top) : zipPath.stem());
    const std::string prefix = strip + (single ? top + "/" : "");
    int written = 0, skipped = 0;
    for (mz_uint i = 0; i < n; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        std::string rel = names[i];
        if (rel.rfind(prefix, 0) != 0) { ++skipped; continue; }
        rel = rel.substr(prefix.size());
        if (!safeRelative(rel)) { ++skipped; continue; }
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!p) { ++skipped; continue; }
        const fs::path out = dest / toPath(rel);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream o(out, std::ios::binary);
        o.write(static_cast<const char*>(p), std::streamsize(size));
        mz_free(p);
        if (o) ++written;
        else ++skipped;
    }
    mz_zip_reader_end(&zip);
    msg = std::to_string(written) + " files into " + pathStr(dest.filename()) + (skipped ? " (" + std::to_string(skipped) + " skipped)" : "");
    return written > 0;
}
}  // namespace

int installModZips(const std::string& modsDirS, std::string& report) {
    const fs::path modsDir = toPath(modsDirS);
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return 0;
    std::vector<fs::path> zips;
    for (fs::directory_iterator it(modsDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && endsWithI(pathStr(it->path().filename()), ".zip")) zips.push_back(it->path());
    }
    int installed = 0;
    for (const fs::path& z : zips) {
        std::string msg;
        const bool ok = installOne(z, modsDir, msg);
        report += pathStr(z.filename()) + ": " + (ok ? "installed, " : "failed, ") + msg + "\n";
        if (!ok) continue;
        ++installed;
        const fs::path done = modsDir / ".installed";
        fs::create_directories(done, ec);
        fs::rename(z, done / z.filename(), ec);
        if (ec) fs::remove(z, ec);
    }
    return installed;
}

}  // namespace gh
