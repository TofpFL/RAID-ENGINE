// Shared helpers used by every module (no raylib dependency).
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gh {

constexpr double kPi = 3.14159265358979323846;

// Monotonic clock in seconds. Used by input timestamps, the audio thread and
// the song clock so that all of them share one time base.
inline double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline double lerpd(double a, double b, double t) { return a + (b - a) * t; }
inline float saturate(float v) { return clampv(v, 0.0f, 1.0f); }
inline float easeOutCubic(float t) { t = saturate(t); float u = 1.0f - t; return 1.0f - u * u * u; }
inline float easeOutBack(float t) {
    t = saturate(t);
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}
inline float easeInOutSine(float t) { return -(std::cos(float(kPi) * saturate(t)) - 1.0f) * 0.5f; }
// Frame-rate independent exponential approach.
inline float approach(float cur, float target, float rate, float dt) {
    return target + (cur - target) * std::exp(-rate * dt);
}

// Small deterministic PRNG (xorshift64*). Same seed -> same song/chart on every platform.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 0x9E3779B97F4A7C15ull) { next(); next(); }
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
    int range(int lo, int hi) { return lo + int(next() % uint64_t(hi - lo + 1)); }  // inclusive
    bool chance(double p) { return uniform() < p; }
    template <class T> const T& pick(const std::vector<T>& v) { return v[size_t(next() % v.size())]; }
};

inline std::string toLower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}
inline std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}
inline bool endsWithI(std::string_view s, std::string_view suf) {
    if (suf.size() > s.size()) return false;
    for (size_t i = 0; i < suf.size(); ++i)
        if (std::tolower((unsigned char)s[s.size() - suf.size() + i]) != std::tolower((unsigned char)suf[i])) return false;
    return true;
}
inline bool equalsI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
// Psych-style song path: lower case, spaces -> '-'.
inline std::string songPathName(std::string_view s) {
    std::string r = toLower(trim(s));
    for (auto& c : r) if (c == ' ') c = '-';
    return r;
}
// "my-cool_song" -> "My Cool Song"
inline std::string prettyName(std::string_view s) {
    std::string r;
    bool up = true;
    for (char c : s) {
        if (c == '-' || c == '_') { r += ' '; up = true; continue; }
        r += up ? char(std::toupper((unsigned char)c)) : c;
        up = (c == ' ');
    }
    return r;
}
inline std::string fmtTime(double sec) {
    if (sec < 0) sec = 0;
    int s = int(sec);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}
inline std::string fmtThousands(long long v) {
    bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    std::string d = std::to_string(u), r;
    int n = 0;
    for (size_t i = d.size(); i-- > 0;) {
        r.insert(r.begin(), d[i]);
        if (++n % 3 == 0 && i > 0) r.insert(r.begin(), ',');
    }
    return neg ? "-" + r : r;
}

}  // namespace gh
