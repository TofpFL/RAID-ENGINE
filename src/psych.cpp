#include "psych.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include "common.hpp"
#include "json.hpp"

namespace gh::psych {

namespace {

// ------------------------------------------------------------------ XML --
// Just enough XML for texture atlases: elements with attributes, comments,
// processing instructions and the five predefined entities.
std::string decodeEntities(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { r += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) { r += s[i]; continue; }
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") r += '&';
        else if (ent == "lt") r += '<';
        else if (ent == "gt") r += '>';
        else if (ent == "quot") r += '"';
        else if (ent == "apos") r += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                   ? std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16)
                                   : std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
            if (cp < 0x80) r += char(cp);
            else if (cp < 0x800) { r += char(0xC0 | (cp >> 6)); r += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                r += char(0xE0 | (cp >> 12)); r += char(0x80 | ((cp >> 6) & 0x3F)); r += char(0x80 | (cp & 0x3F));
            } else {
                r += char(0xF0 | (cp >> 18)); r += char(0x80 | ((cp >> 12) & 0x3F));
                r += char(0x80 | ((cp >> 6) & 0x3F)); r += char(0x80 | (cp & 0x3F));
            }
        } else { r += s[i]; continue; }
        i = semi;
    }
    return r;
}

using Attrs = std::vector<std::pair<std::string, std::string>>;

template <class F>
void forEachElement(std::string_view x, F&& fn) {
    size_t i = 0;
    while ((i = x.find('<', i)) != std::string_view::npos) {
        if (x.compare(i, 4, "<!--") == 0) {
            const size_t e = x.find("-->", i + 4);
            if (e == std::string_view::npos) return;
            i = e + 3;
            continue;
        }
        if (i + 1 < x.size() && (x[i + 1] == '?' || x[i + 1] == '!' || x[i + 1] == '/')) {
            const size_t e = x.find('>', i + 1);
            if (e == std::string_view::npos) return;
            i = e + 1;
            continue;
        }
        size_t p = i + 1;
        while (p < x.size() && !std::isspace((unsigned char)x[p]) && x[p] != '>' && x[p] != '/') ++p;
        const std::string_view name = x.substr(i + 1, p - i - 1);
        Attrs attrs;
        while (p < x.size()) {
            while (p < x.size() && std::isspace((unsigned char)x[p])) ++p;
            if (p >= x.size() || x[p] == '>' || x[p] == '/') break;
            size_t k = p;
            while (k < x.size() && x[k] != '=' && !std::isspace((unsigned char)x[k]) && x[k] != '>') ++k;
            std::string key(x.substr(p, k - p));
            while (k < x.size() && std::isspace((unsigned char)x[k])) ++k;
            if (k >= x.size() || x[k] != '=') { p = k; continue; }
            ++k;
            while (k < x.size() && std::isspace((unsigned char)x[k])) ++k;
            if (k >= x.size()) break;
            const char q = x[k];
            if (q != '"' && q != '\'') { p = k; continue; }
            const size_t e = x.find(q, k + 1);
            if (e == std::string_view::npos) return;
            attrs.emplace_back(std::move(key), decodeEntities(x.substr(k + 1, e - k - 1)));
            p = e + 1;
        }
        fn(name, attrs);
        const size_t e = x.find('>', p);
        if (e == std::string_view::npos) return;
        i = e + 1;
    }
}

const std::string* attr(const Attrs& a, std::string_view key) {
    for (const auto& kv : a)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

float attrNum(const Attrs& a, std::string_view key, float def) {
    const std::string* v = attr(a, key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    const double d = std::strtod(v->c_str(), &end);
    return end == v->c_str() || !std::isfinite(d) ? def : float(d);
}

// Haxe Std.parseInt on the text between prefix and postfix (0 when absent).
bool leadingInt(std::string_view s, long long& out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
    if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
    long long v = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i]) && v < (1LL << 50)) v = v * 10 + (s[i++] - '0');
    out = neg ? -v : v;
    return true;
}

bool startsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

// JSON helpers
using json::Document;
using json::Type;
using json::Value;

float numAt(const Document& d, const Value* arr, size_t i, float def) {
    if (!arr || arr->type != Type::Array || d.size(*arr) <= i) return def;
    return float(Document::num(&d.at(*arr, i), def));
}

std::string strOf(const Document& d, const Value* v, std::string_view def = "") {
    if (!v) return std::string(def);
    if (v->type == Type::String) return std::string(d.str(*v));
    if (v->type == Type::Number) {
        char buf[64];
        const double n = v->num;
        if (n == std::floor(n) && std::fabs(n) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", n);
        else std::snprintf(buf, sizeof buf, "%g", n);
        return buf;
    }
    if (v->type == Type::Bool) return v->b ? "true" : "false";
    return std::string(def);
}

std::vector<int> intList(const Document& d, const Value* v) {
    std::vector<int> r;
    if (!v || v->type != Type::Array) return r;
    for (size_t i = 0; i < d.size(*v); ++i) r.push_back(int(Document::num(&d.at(*v, i), 0)));
    return r;
}

void readPsychAnims(const Document& d, const Value* arr, std::vector<AnimDef>& out) {
    if (!arr || arr->type != Type::Array) return;
    for (size_t i = 0; i < d.size(*arr); ++i) {
        const Value& a = d.at(*arr, i);
        if (a.type != Type::Object) continue;
        AnimDef ad;
        ad.anim = strOf(d, d.get(a, "anim"));
        ad.prefix = strOf(d, d.get(a, "name"));
        ad.fps = Document::num(d.get(a, "fps"), 24);
        ad.loop = Document::boolean(d.get(a, "loop"), false);
        ad.indices = intList(d, d.get(a, "indices"));
        const Value* off = d.get(a, "offsets");
        ad.offX = numAt(d, off, 0, 0);
        ad.offY = numAt(d, off, 1, 0);
        if (!ad.anim.empty()) out.push_back(std::move(ad));
    }
}

uint32_t rgbFromArray(const Document& d, const Value* v, uint32_t def) {
    if (!v || v->type != Type::Array || d.size(*v) < 3) return def;
    auto c = [&](size_t i) { return uint32_t(clampv(int(Document::num(&d.at(*v, i), 0)), 0, 255)); };
    return (c(0) << 16) | (c(1) << 8) | c(2);
}

void addEventEntry(const Document& d, double t, const Value& list, std::vector<ChartEvent>& out) {
    // [[name, v1, v2], ...]
    if (list.type != Type::Array) return;
    for (size_t k = 0; k < d.size(list); ++k) {
        const Value& e = d.at(list, k);
        if (e.type != Type::Array || d.size(e) < 1) continue;
        ChartEvent ev;
        ev.timeMs = t;
        ev.name = strOf(d, &d.at(e, 0));
        if (d.size(e) > 1) ev.v1 = strOf(d, &d.at(e, 1));
        if (d.size(e) > 2) ev.v2 = strOf(d, &d.at(e, 2));
        if (!ev.name.empty()) out.push_back(std::move(ev));
    }
}

void readEventArray(const Document& d, const Value* events, std::vector<ChartEvent>& out) {
    if (!events || events->type != Type::Array) return;
    for (size_t i = 0; i < d.size(*events); ++i) {
        const Value& e = d.at(*events, i);
        if (e.type == Type::Object) {  // V-Slice {t, e, v}
            ChartEvent ev;
            ev.timeMs = Document::num(d.get(e, "t"), 0);
            const std::string kind = strOf(d, d.get(e, "e"));
            const Value* v = d.get(e, "v");
            auto field = [&](const char* key, std::string_view def = "") {
                return v && v->type == Type::Object ? strOf(d, d.get(*v, key), def) : std::string(def);
            };
            if (kind == "PlayAnimation") {
                ev.name = "Play Animation";
                ev.v1 = field("anim");
                ev.v2 = field("target", "bf");
            } else if (kind == "ZoomCamera") {
                ev.name = "__vslice_zoom";
                ev.v1 = v && v->type == Type::Number ? strOf(d, v) : field("zoom", "1");
                ev.v2 = field("duration", "4") + "|" + field("ease", "linear") + "|" + field("mode", "stage");
            } else if (kind == "SetCameraBop") {
                ev.name = "__vslice_bop";
                ev.v1 = field("rate", "4");
                ev.v2 = field("intensity", "1");
            } else if (kind == "ScrollSpeed") {
                ev.name = "Change Scroll Speed";
                ev.v1 = v && v->type == Type::Number ? strOf(d, v) : field("scroll", "1");
                ev.v2 = field("duration", "0");
            } else if (kind == "FocusCamera") {
                ev.name = "__vslice_focus";
                ev.v1 = v && v->type == Type::Number ? strOf(d, v) : field("char", "0");
                ev.v2 = field("x", "0") + "|" + field("y", "0");
            } else {
                ev.name = kind;
                ev.v1 = v ? strOf(d, v) : "";
            }
            if (!ev.name.empty()) out.push_back(std::move(ev));
            continue;
        }
        if (e.type != Type::Array || d.size(e) < 2) continue;
        const double t = Document::num(&d.at(e, 0), 0);
        const Value& second = d.at(e, 1);
        if (second.type == Type::Array) addEventEntry(d, t, second, out);
        else if (second.type == Type::String && d.size(e) >= 2) {  // very old [t, name, v1, v2]
            ChartEvent ev;
            ev.timeMs = t;
            ev.name = std::string(d.str(second));
            if (d.size(e) > 2) ev.v1 = strOf(d, &d.at(e, 2));
            if (d.size(e) > 3) ev.v2 = strOf(d, &d.at(e, 3));
            out.push_back(std::move(ev));
        }
    }
}

}  // namespace

// -------------------------------------------------------------- Atlas --

bool Atlas::parseSparrow(std::string_view xml, std::string* err) {
    frames.clear();
    if (xml.size() >= 3 && (unsigned char)xml[0] == 0xEF && (unsigned char)xml[1] == 0xBB && (unsigned char)xml[2] == 0xBF)
        xml.remove_prefix(3);
    bool sawAtlas = false;
    forEachElement(xml, [&](std::string_view name, const Attrs& a) {
        if (name == "TextureAtlas") {
            sawAtlas = true;
            if (const std::string* ip = attr(a, "imagePath")) imagePath = *ip;
            return;
        }
        if (name != "SubTexture" && name != "sprite") return;
        AtlasFrame f;
        if (const std::string* n = attr(a, "name")) f.name = *n;
        else if (const std::string* n2 = attr(a, "n")) f.name = *n2;
        f.x = attrNum(a, "x", 0);
        f.y = attrNum(a, "y", 0);
        f.w = attrNum(a, "width", attrNum(a, "w", 0));
        f.h = attrNum(a, "height", attrNum(a, "h", 0));
        f.fx = attrNum(a, "frameX", 0);
        f.fy = attrNum(a, "frameY", 0);
        if (const std::string* r = attr(a, "rotated")) f.rotated = *r == "true";
        const float srcW = f.rotated ? f.h : f.w, srcH = f.rotated ? f.w : f.h;
        f.fw = attrNum(a, "frameWidth", srcW);
        f.fh = attrNum(a, "frameHeight", srcH);
        if (f.fw <= 0) f.fw = srcW;
        if (f.fh <= 0) f.fh = srcH;
        frames.push_back(std::move(f));
    });
    if (frames.empty()) {
        if (err) *err = sawAtlas ? "atlas has no frames" : "not a Sparrow atlas";
        return false;
    }
    return true;
}

bool Atlas::parsePacker(std::string_view txt) {
    frames.clear();
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t e = txt.find('\n', pos);
        if (e == std::string_view::npos) e = txt.size();
        const std::string line = trim(txt.substr(pos, e - pos));
        pos = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        AtlasFrame f;
        f.name = trim(std::string_view(line).substr(0, eq));
        float v[4] = {0, 0, 0, 0};
        if (std::sscanf(line.c_str() + eq + 1, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) != 4) continue;
        f.x = v[0];
        f.y = v[1];
        f.w = f.fw = v[2];
        f.h = f.fh = v[3];
        frames.push_back(std::move(f));
    }
    return !frames.empty();
}

std::vector<int> Atlas::byPrefix(std::string_view prefix) const { return framesByPrefix(frames, prefix); }

std::vector<int> Atlas::byIndices(std::string_view prefix, const std::vector<int>& indices) const {
    return framesByIndices(frames, prefix, indices);
}

std::vector<int> framesByPrefix(const std::vector<AtlasFrame>& frames, std::string_view prefix) {
    std::vector<int> idx;
    for (size_t i = 0; i < frames.size(); ++i)
        if (startsWith(frames[i].name, prefix)) idx.push_back(int(i));
    if (idx.empty()) return idx;
    const std::string& first = frames[size_t(idx[0])].name;
    const size_t dot = first.find('.', prefix.size());
    const size_t postLen = dot == std::string::npos ? 0 : first.size() - dot;
    std::vector<long long> key(frames.size(), 0);
    for (int i : idx) {
        const std::string& n = frames[size_t(i)].name;
        long long v = 0;
        if (n.size() >= prefix.size() + postLen &&
            leadingInt(std::string_view(n).substr(prefix.size(), n.size() - prefix.size() - postLen), v))
            key[size_t(i)] = v;
    }
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return key[size_t(a)] < key[size_t(b)]; });
    return idx;
}

std::vector<int> framesByIndices(const std::vector<AtlasFrame>& frames, std::string_view prefix,
                                 const std::vector<int>& indices) {
    std::map<long long, int> byNum;
    for (size_t i = 0; i < frames.size(); ++i) {
        const std::string& n = frames[i].name;
        if (!startsWith(n, prefix)) continue;
        long long v = 0;
        if (leadingInt(std::string_view(n).substr(prefix.size()), v) && !byNum.count(v)) byNum[v] = int(i);
    }
    std::vector<int> out;
    for (int k : indices) {
        auto it = byNum.find(k);
        if (it != byNum.end()) out.push_back(it->second);
    }
    return out;
}

bool pngSize(std::string_view b, int& w, int& h) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 24 || std::memcmp(b.data(), sig, 8) != 0 || b.compare(12, 4, "IHDR") != 0) return false;
    auto be = [&](size_t o) {
        return int((uint32_t((unsigned char)b[o]) << 24) | (uint32_t((unsigned char)b[o + 1]) << 16) |
                   (uint32_t((unsigned char)b[o + 2]) << 8) | uint32_t((unsigned char)b[o + 3]));
    };
    w = be(16);
    h = be(20);
    return w > 0 && h > 0;
}

// --------------------------------------------------------- characters --

const AnimDef* CharacterDef::find(std::string_view anim) const {
    for (const AnimDef& a : anims)
        if (a.anim == anim) return &a;
    return nullptr;
}

bool parseCharacter(std::string_view text, CharacterDef& out, std::string& err) {
    out = CharacterDef();
    Document d;
    if (!d.parse(text, &err)) return false;
    const Value& r = d.root();
    if (r.type != Type::Object) { err = "character is not an object"; return false; }

    if (const Value* asset = d.get(r, "assetPath")) {  // V-Slice character
        const std::string render = toLower(strOf(d, d.get(r, "renderType"), "sparrow"));
        if (render.find("animate") != std::string::npos) { err = "Adobe Animate atlases are not supported"; return false; }
        out.images.push_back(strOf(d, asset));
        out.scale = Document::num(d.get(r, "scale"), 1.0);
        const Value* off = d.get(r, "offsets");
        out.posX = numAt(d, off, 0, 0);
        out.posY = numAt(d, off, 1, 0);
        const Value* cam = d.get(r, "cameraOffsets");
        out.camX = numAt(d, cam, 0, 0);
        out.camY = numAt(d, cam, 1, 0);
        out.flipX = Document::boolean(d.get(r, "flipX"), false);
        out.antialiasing = !Document::boolean(d.get(r, "isPixel"), false);
        out.singDuration = Document::num(d.get(r, "singTime"), 8.0) / 2.0;
        if (const Value* hi = d.get(r, "healthIcon"); hi && hi->type == Type::Object)
            out.icon = strOf(d, d.get(*hi, "id"), "face");
        if (const Value* an = d.get(r, "animations"); an && an->type == Type::Array) {
            for (size_t i = 0; i < d.size(*an); ++i) {
                const Value& a = d.at(*an, i);
                AnimDef ad;
                ad.anim = strOf(d, d.get(a, "name"));
                ad.prefix = strOf(d, d.get(a, "prefix"));
                ad.fps = Document::num(d.get(a, "frameRate"), 24);
                ad.loop = Document::boolean(d.get(a, "looped"), false);
                ad.indices = intList(d, d.get(a, "frameIndices"));
                const Value* o = d.get(a, "offsets");
                ad.offX = numAt(d, o, 0, 0);
                ad.offY = numAt(d, o, 1, 0);
                if (const Value* ap = d.get(a, "assetPath"); ap && ap->type == Type::String) {
                    const std::string extra(d.str(*ap));
                    if (std::find(out.images.begin(), out.images.end(), extra) == out.images.end()) out.images.push_back(extra);
                }
                if (!ad.anim.empty()) out.anims.push_back(std::move(ad));
            }
        }
        return !out.images.empty();
    }

    const std::string image = strOf(d, d.get(r, "image"));
    size_t p = 0;
    while (p <= image.size()) {
        size_t c = image.find(',', p);
        if (c == std::string::npos) c = image.size();
        std::string part = trim(std::string_view(image).substr(p, c - p));
        if (!part.empty()) out.images.push_back(part);
        p = c + 1;
    }
    if (out.images.empty()) { err = "character has no image"; return false; }
    out.scale = Document::num(d.get(r, "scale"), 1.0);
    if (!(out.scale > 0) || !std::isfinite(out.scale)) out.scale = 1.0;
    const Value* pos = d.get(r, "position");
    out.posX = numAt(d, pos, 0, 0);
    out.posY = numAt(d, pos, 1, 0);
    const Value* cam = d.get(r, "camera_position");
    out.camX = numAt(d, cam, 0, 0);
    out.camY = numAt(d, cam, 1, 0);
    out.flipX = Document::boolean(d.get(r, "flip_x"), false);
    out.antialiasing = !Document::boolean(d.get(r, "no_antialiasing"), false);
    out.icon = strOf(d, d.get(r, "healthicon"), "face");
    out.barColor = rgbFromArray(d, d.get(r, "healthbar_colors"), 0xA1A1A1);
    out.singDuration = Document::num(d.get(r, "sing_duration"), 4.0);
    readPsychAnims(d, d.get(r, "animations"), out.anims);
    return true;
}

// ------------------------------------------------------------- stages --

bool parseStage(std::string_view text, StageDef& out, std::string& err) {
    out = StageDef();
    Document d;
    if (!d.parse(text, &err)) return false;
    const Value& r = d.root();
    if (r.type != Type::Object) { err = "stage is not an object"; return false; }
    out.directory = strOf(d, d.get(r, "directory"));
    out.zoom = Document::num(d.get(r, "defaultZoom"), 0.9);
    if (!(out.zoom > 0.01) || !std::isfinite(out.zoom)) out.zoom = 0.9;
    out.pixel = Document::boolean(d.get(r, "isPixelStage"), false) || strOf(d, d.get(r, "stageUI")) == "pixel";
    out.hideGf = Document::boolean(d.get(r, "hide_girlfriend"), false);
    auto pair = [&](const char* key, float* dst) {
        const Value* v = d.get(r, key);
        dst[0] = numAt(d, v, 0, dst[0]);
        dst[1] = numAt(d, v, 1, dst[1]);
    };
    pair("boyfriend", out.bf);
    pair("girlfriend", out.gf);
    pair("opponent", out.dad);
    pair("camera_boyfriend", out.camBf);
    pair("camera_girlfriend", out.camGf);
    pair("camera_opponent", out.camDad);
    out.camSpeed = Document::num(d.get(r, "camera_speed"), 1.0);
    if (const Value* objs = d.get(r, "objects"); objs && objs->type == Type::Array) {
        for (size_t i = 0; i < d.size(*objs); ++i) {
            const Value& o = d.at(*objs, i);
            if (o.type != Type::Object) continue;
            StageObject so;
            so.type = strOf(d, d.get(o, "type"));
            so.name = strOf(d, d.get(o, "name"));
            so.image = strOf(d, d.get(o, "image"));
            so.x = float(Document::num(d.get(o, "x"), 0));
            so.y = float(Document::num(d.get(o, "y"), 0));
            const Value* sc = d.get(o, "scale");
            so.scaleX = numAt(d, sc, 0, 1);
            so.scaleY = numAt(d, sc, 1, so.scaleX);
            const Value* scr = d.get(o, "scroll");
            so.scrollX = numAt(d, scr, 0, 1);
            so.scrollY = numAt(d, scr, 1, so.scrollX);
            so.alpha = float(Document::num(d.get(o, "alpha"), 1));
            so.angle = float(Document::num(d.get(o, "angle"), 0));
            so.color = parseColor(strOf(d, d.get(o, "color"), "FFFFFF")) & 0xFFFFFF;
            so.flipX = Document::boolean(d.get(o, "flipX"), false);
            so.flipY = Document::boolean(d.get(o, "flipY"), false);
            so.antialiasing = Document::boolean(d.get(o, "antialiasing"), true);
            readPsychAnims(d, d.get(o, "animations"), so.anims);
            so.firstAnim = strOf(d, d.get(o, "firstAnimation"));
            out.objects.push_back(std::move(so));
        }
    }
    return true;
}

// ------------------------------------------------------------- events --

void parseEvents(std::string_view text, std::vector<ChartEvent>& out) {
    Document d;
    if (!d.parse(text)) return;
    parseEvents(d, out);
}

void parseEvents(const Document& d, std::vector<ChartEvent>& out) {
    const Value& root = d.root();
    const Value* songV = d.get(root, "song");
    const Value& s = (songV && songV->type == Type::Object) ? *songV : root;
    readEventArray(d, d.get(s, "events"), out);
    if (&s != &root) readEventArray(d, d.get(root, "events"), out);
    // Legacy event notes: [time, -1, name, v1, v2] inside sectionNotes.
    if (const Value* secs = d.get(s, "notes"); secs && secs->type == Type::Array) {
        for (size_t i = 0; i < d.size(*secs); ++i) {
            const Value* sn = d.get(d.at(*secs, i), "sectionNotes");
            if (!sn || sn->type != Type::Array) continue;
            for (size_t j = 0; j < d.size(*sn); ++j) {
                const Value& n = d.at(*sn, j);
                if (n.type != Type::Array || d.size(n) < 3) continue;
                if (Document::num(&d.at(n, 1), 0) >= 0) continue;
                ChartEvent ev;
                ev.timeMs = Document::num(&d.at(n, 0), 0);
                ev.name = strOf(d, &d.at(n, 2));
                if (d.size(n) > 3) ev.v1 = strOf(d, &d.at(n, 3));
                if (d.size(n) > 4) ev.v2 = strOf(d, &d.at(n, 4));
                if (!ev.name.empty()) out.push_back(std::move(ev));
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const ChartEvent& a, const ChartEvent& b) { return a.timeMs < b.timeMs; });
}

SongMeta parseSongMeta(std::string_view chartJson, std::string_view vsliceMeta) {
    SongMeta m;
    auto set = [](std::string& dst, bool& flag, const std::string& v) {
        if (!v.empty()) { dst = v; flag = true; }
    };
    if (!vsliceMeta.empty()) {
        Document d;
        if (d.parse(vsliceMeta)) {
            if (const Value* pd = d.get(d.root(), "playData")) {
                if (const Value* ch = d.get(*pd, "characters")) {
                    set(m.player, m.hasPlayer, strOf(d, d.get(*ch, "player")));
                    set(m.opponent, m.hasOpponent, strOf(d, d.get(*ch, "opponent")));
                    set(m.gf, m.hasGf, strOf(d, d.get(*ch, "girlfriend")));
                }
                m.stage = strOf(d, d.get(*pd, "stage"));
            }
        }
        return m;
    }
    Document d;
    if (!d.parse(chartJson)) return m;
    const Value* songV = d.get(d.root(), "song");
    const Value& s = (songV && songV->type == Type::Object) ? *songV : d.root();
    set(m.player, m.hasPlayer, strOf(d, d.get(s, "player1")));
    set(m.opponent, m.hasOpponent, strOf(d, d.get(s, "player2")));
    std::string gf = strOf(d, d.get(s, "gfVersion"));
    if (gf.empty()) gf = strOf(d, d.get(s, "player3"));
    if (gf.empty()) gf = strOf(d, d.get(s, "gf"));
    set(m.gf, m.hasGf, gf);
    m.stage = strOf(d, d.get(s, "stage"));
    return m;
}

// ------------------------------------------------------------- easing --

double ease(std::string_view nameIn, double t) {
    t = clampv(t, 0.0, 1.0);
    const std::string n = toLower(nameIn);
    const double PI = kPi;
    auto bounceOut = [](double x) {
        const double B1 = 1 / 2.75, B2 = 2 / 2.75, B3 = 1.5 / 2.75, B4 = 2.5 / 2.75, B5 = 2.25 / 2.75, B6 = 2.625 / 2.75;
        if (x < B1) return 7.5625 * x * x;
        if (x < B2) return 7.5625 * (x - B3) * (x - B3) + .75;
        if (x < B4) return 7.5625 * (x - B5) * (x - B5) + .9375;
        return 7.5625 * (x - B6) * (x - B6) + .984375;
    };
    auto smoothInOut = [](double x) { return x * x * (x * -2 + 3); };
    auto smootherInOut = [](double x) { return x * x * x * (x * (x * 6 - 15) + 10); };
    const double EA = 1.0, EP = 0.4;
    if (n == "quadin") return t * t;
    if (n == "quadout") return -t * (t - 2);
    if (n == "quadinout") return t <= .5 ? t * t * 2 : 1 - (t - 1) * (t - 1) * 2;
    if (n == "cubein") return t * t * t;
    if (n == "cubeout") { const double u = t - 1; return 1 + u * u * u; }
    if (n == "cubeinout") { if (t <= .5) return t * t * t * 4; const double u = t - 1; return 1 + u * u * u * 4; }
    if (n == "quartin") return t * t * t * t;
    if (n == "quartout") { const double u = t - 1; return 1 - u * u * u * u; }
    if (n == "quartinout") {
        if (t <= .5) return t * t * t * t * 8;
        const double u = t * 2 - 2;
        return (1 - u * u * u * u) / 2 + .5;
    }
    if (n == "quintin") return t * t * t * t * t;
    if (n == "quintout") { const double u = t - 1; return u * u * u * u * u + 1; }
    if (n == "quintinout") {
        double u = t * 2;
        if (u < 1) return u * u * u * u * u / 2;
        u -= 2;
        return (u * u * u * u * u + 2) / 2;
    }
    if (n == "sinein") return -std::cos(PI / 2 * t) + 1;
    if (n == "sineout") return std::sin(PI / 2 * t);
    if (n == "sineinout") return -std::cos(PI * t) / 2 + .5;
    if (n == "bouncein") return 1 - bounceOut(1 - t);
    if (n == "bounceout") return bounceOut(t);
    if (n == "bounceinout") return t < .5 ? (1 - bounceOut(1 - 2 * t)) / 2 : (1 + bounceOut(2 * t - 1)) / 2;
    if (n == "circin") return -(std::sqrt(1 - t * t) - 1);
    if (n == "circout") return std::sqrt(1 - (t - 1) * (t - 1));
    if (n == "circinout") {
        if (t <= .5) return (std::sqrt(1 - t * t * 4) - 1) / -2;
        const double u = t * 2 - 2;
        return (std::sqrt(1 - u * u) + 1) / 2;
    }
    if (n == "expoin") return std::pow(2, 10 * (t - 1));
    if (n == "expoout") return -std::pow(2, -10 * t) + 1;
    if (n == "expoinout") return t < .5 ? std::pow(2, 10 * (t * 2 - 1)) / 2 : (-std::pow(2, -10 * (t * 2 - 1)) + 2) / 2;
    if (n == "backin") return t * t * (2.70158 * t - 1.70158);
    if (n == "backout") { const double u = t - 1; return 1 - u * u * (-2.70158 * u - 1.70158); }
    if (n == "backinout") {
        double u = t * 2;
        if (u < 1) return u * u * (2.70158 * u - 1.70158) / 2;
        u -= 2;
        return (1 - u * u * (-2.70158 * u - 1.70158)) / 2 + .5;
    }
    if (n == "elasticin") {
        const double u = t - 1;
        return -(EA * std::pow(2, 10 * u) * std::sin((u - (EP / (2 * PI) * std::asin(1 / EA))) * (2 * PI) / EP));
    }
    if (n == "elasticout")
        return EA * std::pow(2, -10 * t) * std::sin((t - (EP / (2 * PI) * std::asin(1 / EA))) * (2 * PI) / EP) + 1;
    if (n == "elasticinout") {
        if (t < 0.5) {
            const double u = t - 0.5;
            return -0.5 * (std::pow(2, 10 * u) * std::sin((u - (EP / 4)) * (2 * PI) / EP));
        }
        const double u = t - 0.5;
        return std::pow(2, -10 * u) * std::sin((u - (EP / 4)) * (2 * PI) / EP) * 0.5 + 1;
    }
    if (n == "smoothstepin") return 2 * smoothInOut(t / 2);
    if (n == "smoothstepout") return 2 * smoothInOut(t / 2 + 0.5) - 1;
    if (n == "smoothstepinout") return smoothInOut(t);
    if (n == "smootherstepin") return 2 * smootherInOut(t / 2);
    if (n == "smootherstepout") return 2 * smootherInOut(t / 2 + 0.5) - 1;
    if (n == "smootherstepinout") return smootherInOut(t);
    return t;  // linear and unknown names
}

uint32_t parseColor(std::string_view in, uint32_t fallback) {
    std::string s = toLower(trim(in));
    if (s.empty()) return fallback;
    static const std::pair<const char*, uint32_t> names[] = {
        {"white", 0xFFFFFFFF}, {"black", 0xFF000000}, {"red", 0xFFFF0000},     {"green", 0xFF008000},
        {"lime", 0xFF00FF00},  {"blue", 0xFF0000FF},  {"yellow", 0xFFFFFF00},  {"cyan", 0xFF00FFFF},
        {"magenta", 0xFFFF00FF}, {"purple", 0xFF800080}, {"pink", 0xFFFFC0CB}, {"orange", 0xFFFFA500},
        {"gray", 0xFF808080},  {"grey", 0xFF808080},  {"brown", 0xFF8B4513},   {"transparent", 0x00000000},
    };
    for (const auto& n : names)
        if (s == n.first) return n.second;
    bool hadAlphaPrefix = false;
    if (s.rfind("0x", 0) == 0) { s = s.substr(2); hadAlphaPrefix = true; }
    else if (s[0] == '#') s = s.substr(1);
    if (s.empty() || s.size() > 8) return fallback;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return fallback;
    const uint32_t v = uint32_t(std::strtoul(s.c_str(), nullptr, 16));
    if (s.size() <= 6) return 0xFF000000u | v;
    (void)hadAlphaPrefix;
    return v;
}

}  // namespace gh::psych
