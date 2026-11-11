// Loading chart files of any size. Small charts go through the JSON DOM.
// Large legacy/Psych charts (spam charts reach 1.5 GB and tens of millions of
// notes) are streamed: every "sectionNotes" array is read straight into a
// compact note buffer, and only the rest of the file (a "skeleton" with a
// marker where each note array was) goes through the DOM. Notes stacked in
// the same lane within 1 ms are merged while streaming, as finalize() would,
// so memory stays proportional to the playable notes.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "chart.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "flp_import.hpp"

namespace gh {

namespace {

constexpr size_t kStreamThreshold = 48u << 20;  // bytes

class Reader {
public:
    explicit Reader(FILE* f) : f_(f), buf_(4u << 20) { refill(); }
    bool eof() { return p_ == end_ && !refill(); }
    int peek() { return (p_ < end_ || refill()) ? (unsigned char)*p_ : -1; }
    int get() { return (p_ < end_ || refill()) ? (unsigned char)*p_++ : -1; }
    void skipWs() {
        for (;;) {
            while (p_ < end_) {
                const char c = *p_;
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { ++p_; continue; }
                if (c == '/' ) { skipComment(); continue; }
                return;
            }
            if (!refill()) return;
        }
    }
    uint64_t consumed() const { return total_ - uint64_t(end_ - p_); }
    // Makes at least n bytes readable from data() when the file has them.
    size_t ensure(size_t n) {
        size_t have = size_t(end_ - p_);
        if (have >= n || !f_) return have;
        std::memmove(buf_.data(), p_, have);
        p_ = buf_.data();
        end_ = p_ + have;
        const size_t got = std::fread(buf_.data() + have, 1, buf_.size() - have, f_);
        end_ += got;
        total_ += got;
        return size_t(end_ - p_);
    }
    const char* data() const { return p_; }
    void advance(size_t n) { p_ += n; }

private:
    void skipComment() {
        ++p_;
        const int c = get();
        if (c == '/') {
            for (int ch = get(); ch != -1 && ch != '\n'; ch = get()) {}
        } else if (c == '*') {
            int prev = 0;
            for (int ch = get(); ch != -1; prev = ch, ch = get())
                if (prev == '*' && ch == '/') break;
        }
    }
    bool refill() {
        if (!f_) return false;
        if (p_ < end_) return true;
        const size_t n = std::fread(buf_.data(), 1, buf_.size(), f_);
        p_ = buf_.data();
        end_ = p_ + n;
        total_ += n;
        return n > 0;
    }
    FILE* f_;
    std::vector<char> buf_;
    const char* p_ = nullptr;
    const char* end_ = nullptr;
    uint64_t total_ = 0;
};

const double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                         1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

class Streamer {
public:
    Streamer(Reader& r, StreamedNotes& out) : r_(r), out_(out) {}

    bool run(std::string& skeleton, std::string& err) {
        r_.skipWs();
        if (!value(skeleton, std::string_view(), 0)) {
            err = err_.empty() ? "invalid JSON" : err_;
            return false;
        }
        return true;
    }

private:
    bool fail(const char* m) {
        if (err_.empty()) err_ = std::string(m) + " near byte " + std::to_string(r_.consumed());
        return false;
    }

    // Copies a string token (with its quotes) to out; also returns its text.
    bool string(std::string& out, std::string* text) {
        if (r_.get() != '"') return fail("expected a string");
        out += '"';
        for (;;) {
            const int c = r_.get();
            if (c == -1) return fail("unterminated string");
            out += char(c);
            if (c == '"') return true;
            if (c == '\\') {
                const int e = r_.get();
                if (e == -1) return fail("unterminated string");
                out += char(e);
                if (text) text->push_back(e == 'n' ? '\n' : e == 't' ? '\t' : char(e));
                continue;
            }
            if (text) text->push_back(char(c));
        }
    }

    bool number(std::string& out) {
        for (int c = r_.peek(); c != -1; c = r_.peek()) {
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                out += char(r_.get());
            } else {
                break;
            }
        }
        return true;
    }

    // Fast number parse for note arrays; good to well under a microsecond of
    // error, which is all chart times need.
    bool fastNumber(double& v) {
        bool neg = false;
        int c = r_.peek();
        if (c == '-') { neg = true; r_.get(); c = r_.peek(); }
        else if (c == '+') { r_.get(); c = r_.peek(); }
        uint64_t mant = 0;
        int digits = 0, exp10 = 0;
        bool any = false;
        while (c >= '0' && c <= '9') {
            if (digits < 19) { mant = mant * 10 + uint64_t(c - '0'); ++digits; }
            else ++exp10;
            any = true;
            r_.get();
            c = r_.peek();
        }
        if (c == '.') {
            r_.get();
            c = r_.peek();
            while (c >= '0' && c <= '9') {
                if (digits < 19) { mant = mant * 10 + uint64_t(c - '0'); ++digits; --exp10; }
                any = true;
                r_.get();
                c = r_.peek();
            }
        }
        if (c == 'e' || c == 'E') {
            r_.get();
            c = r_.peek();
            bool eneg = false;
            if (c == '-' || c == '+') { eneg = c == '-'; r_.get(); c = r_.peek(); }
            int e = 0;
            while (c >= '0' && c <= '9') { e = std::min(e * 10 + (c - '0'), 1000); r_.get(); c = r_.peek(); }
            exp10 += eneg ? -e : e;
        }
        if (!any) {
            // null / true / false / NaN in a note array: read the word.
            std::string w;
            while (c != -1 && std::isalpha(c)) { w += char(r_.get()); c = r_.peek(); }
            if (w == "true") { v = 1; return true; }
            if (w == "false" || w == "null") { v = 0; return true; }
            v = std::nan("");
            return !w.empty() || fail("bad number");
        }
        double d = double(mant);
        if (exp10 > 0) d *= exp10 <= 22 ? kPow10[exp10] : std::pow(10.0, exp10);
        else if (exp10 < 0) d /= -exp10 <= 22 ? kPow10[-exp10] : std::pow(10.0, -exp10);
        v = neg ? -d : d;
        return true;
    }

    bool word(std::string& out) {
        for (int c = r_.peek(); c != -1 && std::isalpha(c); c = r_.peek()) out += char(r_.get());
        return true;
    }

    static bool isWs(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

    static bool numAt(const char*& p, const char* e, double& v) {
        bool neg = false;
        if (p < e && *p == '-') { neg = true; ++p; }
        uint64_t mant = 0;
        int digits = 0, exp10 = 0;
        bool any = false;
        while (p < e && *p >= '0' && *p <= '9') {
            if (digits < 19) { mant = mant * 10 + uint64_t(*p - '0'); ++digits; } else ++exp10;
            any = true;
            ++p;
        }
        if (p < e && *p == '.') {
            ++p;
            while (p < e && *p >= '0' && *p <= '9') {
                if (digits < 19) { mant = mant * 10 + uint64_t(*p - '0'); ++digits; --exp10; }
                any = true;
                ++p;
            }
        }
        if (!any) return false;
        if (p < e && (*p == 'e' || *p == 'E')) {
            ++p;
            bool eneg = false;
            if (p < e && (*p == '-' || *p == '+')) { eneg = *p == '-'; ++p; }
            int x = 0;
            bool anyE = false;
            while (p < e && *p >= '0' && *p <= '9') { x = std::min(x * 10 + (*p - '0'), 1000); ++p; anyE = true; }
            if (!anyE) return false;
            exp10 += eneg ? -x : x;
        }
        double d = double(mant);
        if (exp10 > 0) d *= exp10 <= 22 ? kPow10[exp10] : std::pow(10.0, exp10);
        else if (exp10 < 0) d /= -exp10 <= 22 ? kPow10[-exp10] : std::pow(10.0, -exp10);
        v = neg ? -d : d;
        return true;
    }

    // Plain [t, d] / [t, d, l] notes straight from the buffer. Returns false
    // (consuming nothing) for anything else.
    bool fastNote(std::vector<double>& lastT) {
        const size_t have = r_.ensure(4096);
        const char* p = r_.data();
        const char* e = p + have;
        if (p >= e || *p != '[') return false;
        ++p;
        double v[3] = {0, 0, 0};
        int n = 0;
        for (;;) {
            while (p < e && isWs(*p)) ++p;
            if (n == 3 || !numAt(p, e, v[n])) return false;
            ++n;
            while (p < e && isWs(*p)) ++p;
            if (p >= e) return false;
            if (*p == ']') { ++p; break; }
            if (*p != ',') return false;
            ++p;
        }
        if (n < 2 || !std::isfinite(v[0]) || !(v[1] >= 0) || v[1] >= 64) return false;
        r_.advance(size_t(p - r_.data()));
        addStreamed(v[0], v[1], n > 2 ? v[2] : 0.0, false, std::string(), false, lastT);
        return true;
    }

    void addStreamed(double t, double d, double l, bool hasType, const std::string& typeText, bool altBool,
                     std::vector<double>& lastT) {
        StreamedNote n;
        n.time = t;
        n.length = std::isfinite(l) && l > 0 ? float(l) : 0.0f;
        const int data = int(d) & 63;
        n.data = uint8_t(data | (altBool ? 128 : 0));
        if (hasType && !typeText.empty()) n.type = out_.typeIndex(typeText);
        out_.maxData = std::max(out_.maxData, data);
        ++out_.dataCount[size_t(data)];
        ++out_.rawCount;
        double& last = lastT[size_t(data)];
        if (std::fabs(n.time - last) < 1.0 && !out_.notes.empty()) {
            // Stacked note: counted on the kept one (search the recent tail).
            for (size_t i = out_.notes.size(); i-- > out_.rangeStart.back();) {
                StreamedNote& k = out_.notes[i];
                if (k.noteData() == data && std::fabs(k.time - n.time) < 1.0 && k.stack < 255) {
                    k.length = std::max(k.length, n.length);
                    if (n.type && !k.type) k.type = n.type;
                    ++k.stack;
                    return;
                }
                if (out_.notes.size() - i > 64) break;
            }
        }
        last = n.time;
        out_.notes.push_back(n);
    }

    // A note array element. Plain notes ([time, lane, length, type?]) go to the
    // buffer; anything else (legacy events, odd shapes) is copied to `kept`.
    bool noteElement(std::string& kept, bool& keptAny, std::vector<double>& lastT) {
        if (fastNote(lastT)) return true;
        if (r_.peek() != '[') {
            std::string tmp;
            if (!value(tmp, std::string_view(), 64)) return false;
            if (keptAny) kept += ',';
            kept += tmp;
            keptAny = true;
            return true;
        }
        r_.get();
        double nums[3] = {0, -1, 0};
        int count = 0;
        bool plain = true;
        std::string raw = "[";
        std::string typeText;
        bool hasType = false, altBool = false;
        r_.skipWs();
        if (r_.peek() == ']') { r_.get(); return true; }
        for (;;) {
            r_.skipWs();
            const int c = r_.peek();
            if (count > 0) raw += ',';
            if (c == '"') {
                std::string text;
                if (!string(raw, &text)) return false;
                if (count == 3) { typeText = std::move(text); hasType = true; }
                else if (count < 3) plain = false;
            } else if (c == '[' || c == '{') {
                std::string tmp;
                if (!value(tmp, std::string_view(), 64)) return false;
                raw += tmp;
                plain = false;
            } else if (c == 't' || c == 'f' || c == 'n') {
                std::string w;
                word(w);
                raw += w;
                if (count == 3 && w == "true") altBool = true;
                if (count < 3 && count != 2) plain = false;
            } else {
                double v = 0;
                const size_t before = raw.size();
                (void)before;
                if (!fastNumber(v)) return false;
                char tmp[40];
                std::snprintf(tmp, sizeof tmp, "%.17g", v);
                raw += tmp;
                if (count < 3) nums[count] = v;
            }
            ++count;
            r_.skipWs();
            const int sep = r_.get();
            if (sep == ']') break;
            if (sep != ',') return fail("expected , or ] in a note");
            r_.skipWs();
            if (r_.peek() == ']') { r_.get(); break; }  // trailing comma
        }
        raw += ']';
        if (!plain || count < 2 || !std::isfinite(nums[0]) || !(nums[1] >= 0) || nums[1] >= 64) {
            if (keptAny) kept += ',';
            kept += raw;
            keptAny = true;
            return true;
        }
        addStreamed(nums[0], nums[1], nums[2], hasType, typeText, altBool, lastT);
        return true;
    }

    bool value(std::string& out, std::string_view key, int depth) {
        if (depth > 200) return fail("JSON nested too deeply");
        r_.skipWs();
        const int c = r_.peek();
        if (c == '{') {
            r_.get();
            out += '{';
            bool first = true;
            for (;;) {
                r_.skipWs();
                if (r_.peek() == '}') { r_.get(); break; }
                if (!first) out += ',';
                first = false;
                std::string k;
                if (!string(out, &k)) return false;
                r_.skipWs();
                if (r_.get() != ':') return fail("expected :");
                out += ':';
                if (!value(out, k, depth + 1)) return false;
                r_.skipWs();
                const int sep = r_.get();
                if (sep == '}') break;
                if (sep != ',') return fail("expected , or }");
            }
            out += '}';
            return true;
        }
        if (c == '[') {
            r_.get();
            if (pulseForge_ && depth == 1 && key == "notes") {
                const size_t k = out_.rangeStart.size();
                out_.rangeStart.push_back(uint32_t(out_.notes.size()));
                std::vector<double> lastT(64, -1e300);
                for (;;) {
                    r_.skipWs();
                    if (r_.peek() == ']') { r_.get(); break; }
                    if (!pulseForgeNote(lastT)) return false;
                    r_.skipWs();
                    const int sep = r_.get();
                    if (sep == ']') break;
                    if (sep != ',') return fail("expected , or ] in notes");
                }
                out += "[{\"__gh_stream\":" + std::to_string(k) + "}]";
                return true;
            }
            if (key == "sectionNotes") {
                // Marker object first, then any kept (non-note) elements.
                const size_t k = out_.rangeStart.size();
                out_.rangeStart.push_back(uint32_t(out_.notes.size()));
                std::vector<double> lastT(64, -1e300);
                std::string kept;
                bool keptAny = false;
                for (;;) {
                    r_.skipWs();
                    if (r_.peek() == ']') { r_.get(); break; }
                    if (!noteElement(kept, keptAny, lastT)) return false;
                    r_.skipWs();
                    const int sep = r_.get();
                    if (sep == ']') break;
                    if (sep != ',') return fail("expected , or ] in sectionNotes");
                }
                out += "[{\"__gh_stream\":" + std::to_string(k) + "}";
                if (keptAny) out += "," + kept;
                out += ']';
                return true;
            }
            out += '[';
            bool first = true;
            for (;;) {
                r_.skipWs();
                if (r_.peek() == ']') { r_.get(); break; }
                if (!first) out += ',';
                first = false;
                if (!value(out, std::string_view(), depth + 1)) return false;
                r_.skipWs();
                const int sep = r_.get();
                if (sep == ']') break;
                if (sep != ',') return fail("expected , or ]");
            }
            out += ']';
            return true;
        }
        if (c == '"') {
            if (depth != 1 || key != "format") return string(out, nullptr);
            std::string text;
            if (!string(out, &text)) return false;
            pulseForge_ = text == "pulseforge-chart" || text == "pulseforge-editor-chart";
            return true;
        }
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) return number(out);
        if (std::isalpha(c)) {
            std::string w;
            word(w);
            if (w == "true" || w == "false" || w == "null") out += w;
            else out += "null";  // NaN, Infinity
            return true;
        }
        return fail("unexpected character");
    }

    // {"time": ms, "duration": ms, "lane": n, "owner": "player", "kind": "normal", ...}
    bool pulseForgeNote(std::vector<double>& lastT) {
        if (r_.get() != '{') return fail("expected a note object");
        double time = std::nan(""), duration = 0, lane = -1;
        std::string owner, kind;
        r_.skipWs();
        if (r_.peek() == '}') { r_.get(); return true; }
        for (;;) {
            r_.skipWs();
            std::string raw, k;
            if (!string(raw, &k)) return false;
            r_.skipWs();
            if (r_.get() != ':') return fail("expected :");
            r_.skipWs();
            if (k == "time" || k == "duration" || k == "lane") {
                double v = 0;
                if (!fastNumber(v)) return false;
                (k == "time" ? time : k == "duration" ? duration : lane) = v;
            } else if ((k == "owner" || k == "kind") && r_.peek() == '"') {
                std::string text;
                raw.clear();
                if (!string(raw, &text)) return false;
                (k == "owner" ? owner : kind) = std::move(text);
            } else {
                std::string skip;
                if (!value(skip, k, 64)) return false;
            }
            r_.skipWs();
            const int sep = r_.get();
            if (sep == '}') break;
            if (sep != ',') return fail("expected , or } in a note");
        }
        if (!std::isfinite(time) || !(lane >= 0) || lane >= 20) return true;
        const int who = owner == "opponent" || owner == "dad" || owner == "enemy" ? 1
                        : owner == "girlfriend" || owner == "gf"                 ? 2
                                                                                 : 0;
        const bool typed = !kind.empty() && kind != "normal";
        addStreamed(time, double(int(lane) + 20 * who), duration, typed, kind, false, lastT);
        return true;
    }

    Reader& r_;
    StreamedNotes& out_;
    std::string err_;
    bool pulseForge_ = false;
};

}  // namespace

uint16_t StreamedNotes::typeIndex(const std::string& name) {
    for (size_t i = 0; i < types.size(); ++i)
        if (types[i] == name) return uint16_t(i + 1);
    if (types.size() >= 65000) return 0;
    types.push_back(name);
    return uint16_t(types.size());
}

uint64_t fileSize(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    const long long sz = _ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    const long long sz = ftello(f);
#endif
    std::fclose(f);
    return sz > 0 ? uint64_t(sz) : 0;
}

bool streamChartSkeleton(const std::string& path, std::string& skeleton, StreamedNotes& notes, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    Reader r(f);
    // UTF-8 BOM
    if (r.peek() == 0xEF) { r.get(); r.get(); r.get(); }
    Streamer s(r, notes);
    skeleton.clear();
    const bool ok = s.run(skeleton, err);
    std::fclose(f);
    notes.rangeStart.push_back(uint32_t(notes.notes.size()));
    return ok;
}

bool loadChartFile(const std::string& path, const std::string& difficulty, Chart& out, std::string& err,
                   std::string_view vsliceMetadata, size_t streamThreshold, bool extraKeys) {
    std::string flpFile;
    int pattern = -1;
    if (flpChartPath(path, flpFile, pattern)) {  // an FL Studio project, read as SNIFF does
        FlpProject p;
        if (!readFlpFile(flpFile, p, err) || !flpToChart(p, pattern, out, err)) return false;
        out.difficulty = difficulty;
        return true;
    }
    const uint64_t size = fileSize(path);
    if (size == 0 && !fileExistsAt(path)) { err = "cannot read " + path; return false; }
    if (!vsliceMetadata.empty() || size < (streamThreshold ? streamThreshold : kStreamThreshold)) {
        std::string text;
        if (!readTextFile(path, text)) { err = "cannot read " + path; return false; }
        return parseChart(text, difficulty, out, err, vsliceMetadata, extraKeys);
    }
    std::string skeleton;
    StreamedNotes sn;
    if (!streamChartSkeleton(path, skeleton, sn, err)) return false;
    return parseChartStreamed(skeleton, sn, difficulty, out, err, extraKeys);
}

bool fileExistsAt(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// ------------------------------------------------------------ peeking ----

namespace {
std::string findStringField(std::string_view text, std::string_view key) {
    const std::string pat = "\"" + std::string(key) + "\"";
    size_t p = 0;
    while ((p = text.find(pat, p)) != std::string_view::npos) {
        size_t q = p + pat.size();
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t' || text[q] == '\r' || text[q] == '\n')) ++q;
        if (q < text.size() && text[q] == ':') {
            ++q;
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t' || text[q] == '\r' || text[q] == '\n')) ++q;
            if (q < text.size() && text[q] == '"') {
                std::string out;
                for (++q; q < text.size() && text[q] != '"'; ++q) {
                    if (text[q] == '\\' && q + 1 < text.size()) ++q;
                    out += text[q];
                }
                return out;
            }
        }
        p += pat.size();
    }
    return "";
}
}  // namespace

ChartPeek peekChart(const std::string& path) {
    ChartPeek pk;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return pk;
    constexpr size_t kWin = 96 * 1024;
    std::string head(kWin, '\0'), tail;
    head.resize(std::fread(head.data(), 1, kWin, f));
    const uint64_t size = fileSize(path);
    if (size > kWin) {
#if defined(_WIN32)
        _fseeki64(f, (long long)(size - std::min<uint64_t>(size - kWin, kWin)), SEEK_SET);
#else
        fseeko(f, off_t(size - std::min<uint64_t>(size - kWin, kWin)), SEEK_SET);
#endif
        tail.resize(kWin);
        tail.resize(std::fread(tail.data(), 1, kWin, f));
    }
    std::fclose(f);
    // Top-level fields come either before or after the (huge) notes array.
    // A section's "notes" never contain a "song" string, so the first match
    // in the head or the last in the tail is the song's own.
    auto field = [&](std::string_view key) {
        std::string v = findStringField(head, key);
        if (v.empty()) v = findStringField(tail, key);
        return v;
    };
    pk.pulseForge = head.find("\"pulseforge-chart\"") != std::string::npos ||
                    head.find("\"pulseforge-editor-chart\"") != std::string::npos;
    if (pk.pulseForge) {
        pk.song = field("title");
        pk.difficulty = field("difficulty");
        pk.player2 = field("opponent");
        pk.stage = field("stage");
        pk.isChart = true;
        return pk;
    }
    pk.song = field("song");
    if (pk.song.empty()) pk.song = field("songName");
    pk.player2 = field("player2");
    pk.stage = field("stage");
    pk.isChart = head.find("\"notes\"") != std::string::npos || head.find("\"sectionNotes\"") != std::string::npos ||
                 tail.find("\"sectionNotes\"") != std::string::npos || tail.find("\"bpm\"") != std::string::npos;
    return pk;
}

}  // namespace gh
