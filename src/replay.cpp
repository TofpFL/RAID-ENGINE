#include "replay.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "common.hpp"

namespace gh {

namespace {
std::filesystem::path toPath(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
}

std::string replayFileName(const std::string& songId, const std::string& diff, const char* kind) {
    std::string n;
    for (char c : songId + "-" + diff) n += (std::isalnum((unsigned char)c) || c == '-') ? char(std::tolower((unsigned char)c)) : '_';
    return n + "-" + kind + ".ghr";
}

bool Replay::save(const std::string& path) const {
    std::error_code ec;
    std::filesystem::create_directories(toPath(path).parent_path(), ec);
    std::ofstream f(toPath(path), std::ios::binary);
    if (!f) return false;
    f << "GHREPLAY 1\n";
    f << "song=" << songId << "\ndiff=" << diff << "\ntitle=" << title << "\ndate=" << date << "\n";
    f << "rate=" << rate << "\nmirror=" << mirror << "\nghost=" << ghostTapping << "\nkeys=" << keys << "\n";
    {
        char w[160];
        std::snprintf(w, sizeof w, "windows=%.4f,%.4f,%.4f,%.4f\nratingOffset=%.4f\n", windows[0], windows[1], windows[2], windows[3],
                      ratingOffset);
        f << w;
    }
    f << "score=" << score << "\nacc=" << acc << "\nmisses=" << misses << "\nevents=" << events.size() << "\n";
    char buf[64];
    for (const ReplayEvent& e : events) {
        std::snprintf(buf, sizeof buf, "%.4f %d %d\n", e.t, int(e.lane), e.down ? 1 : 0);
        f << buf;
    }
    return bool(f);
}

bool Replay::load(const std::string& path) {
    std::ifstream f(toPath(path), std::ios::binary);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line) || line.rfind("GHREPLAY", 0) != 0) return false;
    *this = Replay();
    size_t count = 0;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) break;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "song") songId = v;
        else if (k == "diff") diff = v;
        else if (k == "title") title = v;
        else if (k == "date") date = v;
        else if (k == "rate") rate = std::atof(v.c_str());
        else if (k == "mirror") mirror = v == "1";
        else if (k == "ghost") ghostTapping = v == "1";
        else if (k == "keys") keys = std::atoi(v.c_str());
        else if (k == "windows") std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &windows[0], &windows[1], &windows[2], &windows[3]);
        else if (k == "ratingOffset") ratingOffset = std::atof(v.c_str());
        else if (k == "score") score = std::atoll(v.c_str());
        else if (k == "acc") acc = std::atof(v.c_str());
        else if (k == "misses") misses = std::atoll(v.c_str());
        else if (k == "events") { count = size_t(std::atoll(v.c_str())); break; }
    }
    events.reserve(count);
    double t;
    int lane, down;
    while (f >> t >> lane >> down) events.push_back({t, uint8_t(clampv(lane, 0, 8)), down != 0});
    if (!(rate > 0)) rate = 1.0;
    return !songId.empty();
}

}  // namespace gh
