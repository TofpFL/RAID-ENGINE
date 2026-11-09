#include "json.hpp"

#include <cstdlib>
#include <cstring>

namespace json {

namespace {
constexpr uint32_t kInvalid = 0xFFFFFFFFu;
constexpr int kMaxDepth = 256;

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    }
}
// Fast JSON number parser: exact for the common case (<= 18 significant
// digits and a small exponent), falls back to strtod otherwise. Portable to
// standard libraries that lack floating-point std::from_chars.
const char* parseNumber(const char* p, const char* end, double& out) {
    static const double kPow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    const char* s = p;
    bool neg = false;
    if (s < end && (*s == '-' || *s == '+')) { neg = *s == '-'; ++s; }
    unsigned long long mant = 0;
    int digits = 0, exp10 = 0;
    bool any = false;
    while (s < end && *s >= '0' && *s <= '9') {
        any = true;
        if (digits < 18) { mant = mant * 10 + unsigned(*s - '0'); if (mant) ++digits; }
        else ++exp10;
        ++s;
    }
    if (s < end && *s == '.') {
        ++s;
        while (s < end && *s >= '0' && *s <= '9') {
            any = true;
            if (digits < 18) { mant = mant * 10 + unsigned(*s - '0'); if (mant) ++digits; --exp10; }
            ++s;
        }
    }
    if (!any) return nullptr;
    if (s < end && (*s == 'e' || *s == 'E')) {
        const char* e = s + 1;
        bool eneg = false;
        if (e < end && (*e == '-' || *e == '+')) { eneg = *e == '-'; ++e; }
        int ev = 0;
        bool edig = false;
        while (e < end && *e >= '0' && *e <= '9') { edig = true; if (ev < 10000) ev = ev * 10 + (*e - '0'); ++e; }
        if (edig) { exp10 += eneg ? -ev : ev; s = e; }
    }
    if (digits <= 15 && exp10 >= -22 && exp10 <= 22) {
        double v = double(mant);
        v = exp10 < 0 ? v / kPow10[-exp10] : v * kPow10[exp10];
        out = neg ? -v : v;
        return s;
    }
    char buf[128];
    const size_t n = size_t(s - p);
    if (n >= sizeof buf) { out = 0; return s; }
    std::memcpy(buf, p, n);
    buf[n] = 0;
    out = std::strtod(buf, nullptr);
    return s;
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool Document::fail(const char* msg) {
    if (ok_) {
        ok_ = false;
        err_ = std::string(msg) + " at byte " + std::to_string(size_t(p_ - begin_));
    }
    return false;
}

void Document::skipWs() {
    for (;;) {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
        // Lenient: allow // and /* */ comments (some hand-edited charts have them).
        if (p_ + 1 < end_ && p_[0] == '/' && p_[1] == '/') {
            while (p_ < end_ && *p_ != '\n') ++p_;
            continue;
        }
        if (p_ + 1 < end_ && p_[0] == '/' && p_[1] == '*') {
            p_ += 2;
            while (p_ + 1 < end_ && !(p_[0] == '*' && p_[1] == '/')) ++p_;
            p_ = (p_ + 1 < end_) ? p_ + 2 : end_;
            continue;
        }
        break;
    }
}

bool Document::parseString(uint32_t& start, uint32_t& count) {
    // p_ points at the opening quote.
    ++p_;
    start = uint32_t(pool_.size());
    const char* runStart = p_;
    while (p_ < end_) {
        char c = *p_;
        if (c == '"') {
            pool_.append(runStart, size_t(p_ - runStart));
            ++p_;
            count = uint32_t(pool_.size() - start);
            return true;
        }
        if (c == '\\') {
            pool_.append(runStart, size_t(p_ - runStart));
            ++p_;
            if (p_ >= end_) return fail("bad escape");
            char e = *p_++;
            switch (e) {
                case '"': pool_ += '"'; break;
                case '\\': pool_ += '\\'; break;
                case '/': pool_ += '/'; break;
                case 'b': pool_ += '\b'; break;
                case 'f': pool_ += '\f'; break;
                case 'n': pool_ += '\n'; break;
                case 'r': pool_ += '\r'; break;
                case 't': pool_ += '\t'; break;
                case 'u': {
                    auto read4 = [&](uint32_t& v) {
                        if (end_ - p_ < 4) return false;
                        v = 0;
                        for (int i = 0; i < 4; ++i) {
                            int h = hexv(p_[i]);
                            if (h < 0) return false;
                            v = (v << 4) | uint32_t(h);
                        }
                        p_ += 4;
                        return true;
                    };
                    uint32_t cp;
                    if (!read4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        uint32_t lo;
                        if (!read4(lo)) return fail("bad surrogate");
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(pool_, cp);
                    break;
                }
                default: pool_ += e; break;
            }
            runStart = p_;
            continue;
        }
        ++p_;
    }
    return fail("unterminated string");
}

uint32_t Document::parseValue(int depth) {
    if (depth > kMaxDepth) { fail("nesting too deep"); return kInvalid; }
    skipWs();
    if (p_ >= end_) { fail("unexpected end"); return kInvalid; }
    Value v;
    char c = *p_;
    if (c == '{' || c == '[') {
        const bool isObj = (c == '{');
        const char close = isObj ? '}' : ']';
        ++p_;
        const size_t base = stack_.size();
        skipWs();
        while (p_ < end_ && *p_ != close) {
            if (isObj) {
                skipWs();
                if (p_ >= end_ || *p_ != '"') { fail("expected key"); return kInvalid; }
                Value k;
                k.type = Type::String;
                if (!parseString(k.r.start, k.r.count)) return kInvalid;
                vals_.push_back(k);
                stack_.push_back(uint32_t(vals_.size() - 1));
                skipWs();
                if (p_ >= end_ || *p_ != ':') { fail("expected ':'"); return kInvalid; }
                ++p_;
            }
            uint32_t child = parseValue(depth + 1);
            if (child == kInvalid) return kInvalid;
            stack_.push_back(child);
            skipWs();
            if (p_ < end_ && *p_ == ',') { ++p_; skipWs(); continue; }  // trailing commas tolerated
            if (p_ < end_ && *p_ == close) break;
            fail("expected ',' or close");
            return kInvalid;
        }
        if (p_ >= end_) { fail("unterminated container"); return kInvalid; }
        ++p_;
        v.type = isObj ? Type::Object : Type::Array;
        v.r.start = uint32_t(kids_.size());
        v.r.count = uint32_t((stack_.size() - base) / (isObj ? 2 : 1));
        kids_.insert(kids_.end(), stack_.begin() + std::ptrdiff_t(base), stack_.end());
        stack_.resize(base);
    } else if (c == '"') {
        v.type = Type::String;
        if (!parseString(v.r.start, v.r.count)) return kInvalid;
    } else if (c == 't' && end_ - p_ >= 4 && std::memcmp(p_, "true", 4) == 0) {
        v.type = Type::Bool; v.b = true; p_ += 4;
    } else if (c == 'f' && end_ - p_ >= 5 && std::memcmp(p_, "false", 5) == 0) {
        v.type = Type::Bool; v.b = false; p_ += 5;
    } else if (c == 'n' && end_ - p_ >= 4 && std::memcmp(p_, "null", 4) == 0) {
        p_ += 4;
    } else if (c == 'N' && end_ - p_ >= 3 && std::memcmp(p_, "NaN", 3) == 0) {
        v.type = Type::Number; v.num = 0; p_ += 3;  // Haxe can emit NaN; treat as 0
    } else {
        double d = 0;
        const char* next = parseNumber(p_, end_, d);
        if (!next) {
            if (end_ - p_ >= 8 && std::memcmp(p_, "Infinity", 8) == 0) { d = 0; next = p_ + 8; }
            else if (end_ - p_ >= 9 && std::memcmp(p_, "-Infinity", 9) == 0) { d = 0; next = p_ + 9; }
            else { fail("bad value"); return kInvalid; }
        }
        p_ = next;
        v.type = Type::Number;
        v.num = d;
    }
    vals_.push_back(v);
    return uint32_t(vals_.size() - 1);
}

bool Document::parse(std::string_view text, std::string* err) {
    vals_.clear(); kids_.clear(); stack_.clear(); pool_.clear();
    ok_ = true; err_.clear();
    begin_ = p_ = text.data();
    end_ = text.data() + text.size();
    // Skip UTF-8 BOM.
    if (end_ - p_ >= 3 && (unsigned char)p_[0] == 0xEF && (unsigned char)p_[1] == 0xBB && (unsigned char)p_[2] == 0xBF) p_ += 3;
    // Rough reservation: charts are mostly short numbers.
    vals_.reserve(text.size() / 6 + 16);
    kids_.reserve(text.size() / 6 + 16);
    root_ = parseValue(0);
    if (root_ == kInvalid || !ok_) {
        vals_.clear();
        vals_.push_back(Value());
        root_ = 0;
        if (err) *err = err_;
        return false;
    }
    return true;
}

const Value* Document::get(const Value& obj, std::string_view key) const {
    if (obj.type != Type::Object) return nullptr;
    for (uint32_t i = 0; i < obj.r.count; ++i) {
        const Value& k = vals_[kids_[obj.r.start + 2 * i]];
        if (str(k) == key) return &vals_[kids_[obj.r.start + 2 * i + 1]];
    }
    return nullptr;
}

}  // namespace json
