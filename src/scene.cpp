#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "common.hpp"

namespace gh::scene {

namespace fs = std::filesystem;

namespace {
fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(toPath(p), ec);
}

std::string fmtNum(double d) {
    char buf[64];
    if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", d);
    else std::snprintf(buf, sizeof buf, "%.10g", d);
    return buf;
}

// "a.b.c" -> head "a", rest "b.c"; "a[2].x" -> head "a", rest "[2].x"
void splitHead(std::string_view path, std::string_view& head, std::string_view& rest) {
    size_t i = 0;
    while (i < path.size() && path[i] != '.' && path[i] != '[') ++i;
    head = path.substr(0, i);
    rest = i < path.size() ? path.substr(path[i] == '.' ? i + 1 : i) : std::string_view();
}

uint32_t lerpRgb(uint32_t a, uint32_t b, double t) {
    auto ch = [&](int s) {
        const double x = double((a >> s) & 255), y = double((b >> s) & 255);
        return uint32_t(clampv(x + (y - x) * t, 0.0, 255.0) + 0.5) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

float frand(unsigned& s) {
    s = s * 1664525u + 1013904223u;
    return float((s >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
}

// PlayState.boyfriendCameraOffset / opponentCameraOffset / girlfriendCameraOffset
// (the stage's camera offsets), by lower-case name.
const float* camOffsetOf(const psych::StageDef& st, const std::string& lower) {
    if (lower == "boyfriendcameraoffset") return st.camBf;
    if (lower == "opponentcameraoffset") return st.camDad;
    if (lower == "girlfriendcameraoffset") return st.camGf;
    return nullptr;
}
}  // namespace

double toNum(const Value& v, double def) {
    if (auto d = std::get_if<double>(&v)) return *d;
    if (auto b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
    if (auto s = std::get_if<std::string>(&v)) {
        char* end = nullptr;
        const std::string t = trim(*s);
        const double d = std::strtod(t.c_str(), &end);
        if (end != t.c_str() && std::isfinite(d)) return d;
    }
    return def;
}

bool toBool(const Value& v, bool def) {
    if (auto b = std::get_if<bool>(&v)) return *b;
    if (auto d = std::get_if<double>(&v)) return *d != 0;
    if (auto s = std::get_if<std::string>(&v)) {
        const std::string l = toLower(trim(*s));
        if (l == "true" || l == "1") return true;
        if (l == "false" || l == "0" || l.empty()) return false;
    }
    return def;
}

std::string toStr(const Value& v) {
    if (auto s = std::get_if<std::string>(&v)) return *s;
    if (auto d = std::get_if<double>(&v)) return fmtNum(*d);
    if (auto b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    return "";
}

// ---------------------------------------------------------------- Files --

void Files::setRoots(std::vector<std::string> roots) {
    roots_.clear();
    for (std::string& r : roots) {
        if (r.empty()) continue;
        if (r.back() != '/' && r.back() != '\\') r += '/';
        if (std::find(roots_.begin(), roots_.end(), r) == roots_.end()) roots_.push_back(r);
    }
    dirCache_.clear();
    sharedFound_.clear();
}

void Files::setSharedRoots(std::vector<std::string> roots) {
    shared_.clear();
    sharedFound_.clear();
    for (std::string& r : roots) {
        if (r.empty()) continue;
        if (r.back() != '/' && r.back() != '\\') r += '/';
        if (std::find(roots_.begin(), roots_.end(), r) == roots_.end() &&
            std::find(shared_.begin(), shared_.end(), r) == shared_.end())
            shared_.push_back(r);
    }
}

const std::vector<std::string>& Files::dirEntries(const std::string& dir) const {
    auto it = dirCache_.find(dir);
    if (it != dirCache_.end()) return it->second;
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator d(toPath(dir), ec), end; !ec && d != end; d.increment(ec))
        names.push_back(pathStr(d->path().filename()));
    std::sort(names.begin(), names.end());
    return dirCache_.emplace(dir, std::move(names)).first->second;
}

std::string Files::resolveIn(const std::string& root, const std::string& rel) const {
    const std::string direct = root + rel;
    if (fileExists(direct)) return direct;
    // Case-insensitive walk, one path component at a time.
    std::string cur = root;
    size_t p = 0;
    while (p < rel.size()) {
        size_t s = rel.find_first_of("/\\", p);
        if (s == std::string::npos) s = rel.size();
        const std::string part = rel.substr(p, s - p);
        p = s + 1;
        if (part.empty() || part == ".") continue;
        const auto& entries = dirEntries(cur);
        const std::string* match = nullptr;
        for (const std::string& e : entries)
            if (equalsI(e, part)) { match = &e; break; }
        if (!match) return "";
        cur += *match;
        if (s < rel.size()) cur += '/';
    }
    return fileExists(cur) ? cur : "";
}

std::string Files::find(const std::string& relIn) const {
    std::string rel = relIn;
    while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
    if (rel.empty() || rel.find("..") != std::string::npos) return "";
    for (const std::string& r : roots_) {
        std::string f = resolveIn(r, rel);
        if (!f.empty()) return f;
    }
    if (shared_.empty()) return "";
    // Hundreds of folders when many mods are installed: each answer is kept.
    auto it = sharedFound_.find(rel);
    if (it != sharedFound_.end()) return it->second;
    std::string found;
    for (const std::string& r : shared_)
        if (!(found = resolveIn(r, rel)).empty()) break;
    sharedFound_.emplace(rel, found);
    return found;
}

std::vector<std::string> Files::list(const std::string& relDir, const std::string& ext) const {
    std::vector<std::string> out, seen;
    for (const std::string& r : roots_) {
        std::string dir = r;
        size_t p = 0;
        bool ok = true;
        while (p < relDir.size() && ok) {
            size_t s = relDir.find('/', p);
            if (s == std::string::npos) s = relDir.size();
            const std::string part = relDir.substr(p, s - p);
            p = s + 1;
            if (part.empty()) continue;
            ok = false;
            for (const std::string& e : dirEntries(dir))
                if (equalsI(e, part)) { dir += e + "/"; ok = true; break; }
        }
        if (!ok) continue;
        for (const std::string& e : dirEntries(dir)) {
            if (!endsWithI(e, ext) || !fileExists(dir + e)) continue;
            const std::string low = toLower(e);
            if (std::find(seen.begin(), seen.end(), low) != seen.end()) continue;
            seen.push_back(low);
            out.push_back(dir + e);
        }
    }
    return out;
}

// --------------------------------------------------------- GraphicCache --

bool GraphicCache::loadPng(const std::string& key, std::string& path, int& w, int& h) {
    path = files_.find("images/" + key + ".png");
    if (path.empty()) path = files_.find(key + ".png");
    if (path.empty()) return false;
    std::ifstream f(toPath(path), std::ios::binary);
    char head[32] = {};
    f.read(head, sizeof head);
    return psych::pngSize(std::string_view(head, size_t(f.gcount())), w, h);
}

GraphicPtr GraphicCache::image(const std::string& key, int gw, int gh) {
    const std::string ck = "img:" + key + ":" + std::to_string(gw) + "x" + std::to_string(gh);
    if (auto it = cache_.find(ck); it != cache_.end()) return it->second;
    auto g = std::make_shared<Graphic>();
    std::string path;
    int w = 0, h = 0;
    if (!loadPng(key, path, w, h)) {
        missing.push_back(key);
        cache_[ck] = nullptr;
        return nullptr;
    }
    g->files.push_back(path);
    g->sizes.push_back({w, h});
    if (gw > 0 && gh > 0 && gw <= w && gh <= h) {
        for (int y = 0; y + gh <= h; y += gh)
            for (int x = 0; x + gw <= w; x += gw) {
                psych::AtlasFrame f;
                f.name = std::to_string(g->frames.size());
                f.x = float(x); f.y = float(y); f.w = f.fw = float(gw); f.h = f.fh = float(gh);
                g->frames.push_back(f);
            }
    } else {
        psych::AtlasFrame f;
        f.name = key;
        f.w = f.fw = float(w);
        f.h = f.fh = float(h);
        g->frames.push_back(f);
    }
    cache_[ck] = g;
    return g;
}

GraphicPtr GraphicCache::atlas(const std::string& keys) {
    const std::string ck = "atlas:" + keys;
    if (auto it = cache_.find(ck); it != cache_.end()) return it->second;
    auto g = std::make_shared<Graphic>();
    size_t p = 0;
    while (p <= keys.size()) {
        size_t c = keys.find(',', p);
        if (c == std::string::npos) c = keys.size();
        const std::string key = trim(std::string_view(keys).substr(p, c - p));
        p = c + 1;
        if (key.empty()) continue;
        std::string png;
        int w = 0, h = 0;
        if (!loadPng(key, png, w, h)) { missing.push_back(key); continue; }
        psych::Atlas a;
        std::string text;
        std::string xml = files_.find("images/" + key + ".xml");
        bool ok = false;
        if (!xml.empty() && readFile(xml, text)) ok = a.parseSparrow(text);
        if (!ok) {
            const std::string txt = files_.find("images/" + key + ".txt");
            if (!txt.empty() && readFile(txt, text)) ok = a.parsePacker(text);
        }
        if (!ok) {  // plain image: one frame
            a.frames.clear();
            psych::AtlasFrame f;
            f.name = key;
            f.w = f.fw = float(w);
            f.h = f.fh = float(h);
            a.frames.push_back(f);
        }
        const uint16_t tex = uint16_t(g->files.size());
        g->files.push_back(png);
        g->sizes.push_back({w, h});
        for (psych::AtlasFrame& f : a.frames) {
            f.tex = tex;
            g->frames.push_back(std::move(f));
        }
    }
    GraphicPtr out = g->frames.empty() ? nullptr : g;
    cache_[ck] = out;
    return out;
}

GraphicPtr GraphicCache::solid(int w, int h, uint32_t argb) {
    auto g = std::make_shared<Graphic>();
    g->solid = true;
    g->solidColor = argb;
    psych::AtlasFrame f;
    f.w = f.fw = float(std::max(1, w));
    f.h = f.fh = float(std::max(1, h));
    g->frames.push_back(f);
    return g;
}

std::vector<std::string> GraphicCache::pngFiles() const {
    std::vector<std::string> out;
    for (const auto& kv : cache_)
        if (kv.second)
            for (const std::string& f : kv.second->files)
                if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
    return out;
}

// --------------------------------------------------------------- Sprite --

void Sprite::setGraphic(GraphicPtr g) {
    graphic = std::move(g);
    anims.clear();
    cur = -1;
    frame = 0;
    finished = true;
    if (graphic && !graphic->frames.empty()) {
        frameW = graphic->frames[0].fw;
        frameH = graphic->frames[0].fh;
    } else {
        frameW = frameH = 0;
    }
    width = frameW;
    height = frameH;
    centerOrigin();
}

void Sprite::updateHitbox() {
    width = std::fabs(scaleX) * frameW;
    height = std::fabs(scaleY) * frameH;
    offsetX = -0.5f * (width - frameW);
    offsetY = -0.5f * (height - frameH);
    centerOrigin();
}

void Sprite::setGraphicSize(float w, float h) {
    if ((w <= 0 && h <= 0) || frameW <= 0 || frameH <= 0) return;
    const float nx = w / frameW, ny = h / frameH;
    scaleX = nx;
    scaleY = ny;
    if (w <= 0) scaleX = ny;
    else if (h <= 0) scaleY = nx;
}

void Sprite::screenCenter(bool cx, bool cy) {
    if (cx) x = (kW - width) * 0.5f;
    if (cy) y = (kH - height) * 0.5f;
}

bool Sprite::hasAnim(std::string_view n) const {
    for (const Anim& a : anims)
        if (a.name == n) return true;
    return false;
}

Anim* Sprite::anim(std::string_view n) {
    for (Anim& a : anims)
        if (a.name == n) return &a;
    return nullptr;
}

void Sprite::addAnim(Anim a) {
    for (Anim& e : anims)
        if (e.name == a.name) {
            const bool keepOff = e.hasOffset && !a.hasOffset;
            const float ox = e.offX, oy = e.offY;
            e = std::move(a);
            if (keepOff) { e.hasOffset = true; e.offX = ox; e.offY = oy; }
            return;
        }
    anims.push_back(std::move(a));
}

bool Sprite::play(const std::string& name, bool force, bool rev, int startFrame) {
    int idx = -1;
    for (size_t i = 0; i < anims.size(); ++i)
        if (anims[i].name == name) { idx = int(i); break; }
    if (idx < 0) return false;
    const Anim& a = anims[size_t(idx)];
    if (!force && idx == cur && !finished && reversed == rev) {
        paused = false;
        return true;
    }
    cur = idx;
    reversed = rev;
    paused = false;
    animTime = 0;
    const int n = int(a.frames.size());
    finished = a.fps <= 0 || n == 0;
    int f = clampv(startFrame, 0, std::max(0, n - 1));
    if (rev) f = std::max(0, n - 1 - f);
    animFrame = f;
    if (n > 0) frame = a.frames[size_t(f)];
    if (a.hasOffset) { offsetX = a.offX; offsetY = a.offY; }
    return true;
}

void Sprite::finishAnim() {
    const Anim* a = curAnim();
    if (!a || a->frames.empty()) return;
    animFrame = reversed ? 0 : int(a->frames.size()) - 1;
    frame = a->frames[size_t(animFrame)];
    finished = true;
}

void Sprite::updateAnim(double dt) {
    const Anim* a = curAnim();
    if (!a || finished || paused || a->fps <= 0 || a->frames.empty()) return;
    const double delay = 1.0 / a->fps;
    const int last = int(a->frames.size()) - 1;
    animTime += dt;
    while (animTime > delay && !finished) {
        animTime -= delay;
        if (!reversed) {
            if (animFrame >= last) {
                if (a->loop) animFrame = 0;
                else finished = true;
            } else {
                ++animFrame;
            }
        } else {
            if (animFrame <= 0) {
                if (a->loop) animFrame = last;
                else finished = true;
            } else {
                --animFrame;
            }
        }
    }
    frame = a->frames[size_t(clampv(animFrame, 0, last))];
}

bool Sprite::get(std::string_view f, Value& out) const {
    auto num = [&](double v) { out = v; return true; };
    if (f == "x") return num(x);
    if (f == "y") return num(y);
    if (f == "alpha") return num(alpha);
    if (f == "angle") return num(angle);
    if (f == "visible") { out = visible; return true; }
    if (f == "active") { out = active; return true; }
    if (f == "color") return num(double(color | 0xFF000000u));
    if (f == "flipX") { out = flipX; return true; }
    if (f == "flipY") { out = flipY; return true; }
    if (f == "antialiasing") { out = antialiasing; return true; }
    if (f == "width") return num(width);
    if (f == "height") return num(height);
    if (f == "frameWidth") return num(frameW);
    if (f == "frameHeight") return num(frameH);
    if (f == "scale.x") return num(scaleX);
    if (f == "scale.y") return num(scaleY);
    if (f == "scrollFactor.x") return num(scrollX);
    if (f == "scrollFactor.y") return num(scrollY);
    if (f == "offset.x") return num(offsetX);
    if (f == "offset.y") return num(offsetY);
    if (f == "origin.x") return num(originX);
    if (f == "origin.y") return num(originY);
    if (f == "animation.curAnim.name" || f == "animation.name") { out = curAnimName(); return true; }
    if (f == "animation.curAnim.finished" || f == "animation.finished") { out = finished; return true; }
    if (f == "animation.curAnim.curFrame") return num(animFrame);
    if (f == "animation.curAnim.numFrames") return num(curAnim() ? double(curAnim()->frames.size()) : 0.0);
    if (f == "animation.curAnim.frameRate") return num(curAnim() ? curAnim()->fps : 0.0);
    if (f == "animation.curAnim.paused" || f == "animation.paused") { out = paused; return true; }
    if (f == "animation.frameIndex") return num(frame);
    if (f == "text") { out = text.text; return true; }
    if (f == "size") return num(text.size);
    if (f == "borderSize") return num(text.borderSize);
    if (f == "fieldWidth") return num(text.fieldWidth);
    if (f == "blend") return num(blend);
    return false;
}

bool Sprite::set(std::string_view f, const Value& v) {
    const double d = toNum(v, 0);
    if (f == "x") { x = float(d); return true; }
    if (f == "y") { y = float(d); return true; }
    if (f == "alpha") { alpha = clampv(float(d), 0.0f, 1.0f); return true; }
    if (f == "angle") { angle = float(d); return true; }
    if (f == "visible") { visible = toBool(v, true); return true; }
    if (f == "active") { active = toBool(v, true); return true; }
    if (f == "color") {
        if (std::holds_alternative<std::string>(v)) color = psych::parseColor(toStr(v)) & 0xFFFFFF;
        else color = uint32_t(int64_t(d)) & 0xFFFFFF;
        return true;
    }
    if (f == "flipX") { flipX = toBool(v); return true; }
    if (f == "flipY") { flipY = toBool(v); return true; }
    if (f == "antialiasing") { antialiasing = toBool(v, true); return true; }
    if (f == "width") { width = float(d); return true; }
    if (f == "height") { height = float(d); return true; }
    if (f == "scale.x") { scaleX = float(d); return true; }
    if (f == "scale.y") { scaleY = float(d); return true; }
    if (f == "scrollFactor.x") { scrollX = float(d); return true; }
    if (f == "scrollFactor.y") { scrollY = float(d); return true; }
    if (f == "offset.x") { offsetX = float(d); return true; }
    if (f == "offset.y") { offsetY = float(d); return true; }
    if (f == "origin.x") { originX = float(d); return true; }
    if (f == "origin.y") { originY = float(d); return true; }
    if (f == "animation.curAnim.curFrame") {
        if (const Anim* a = curAnim(); a && !a->frames.empty()) {
            animFrame = clampv(int(d), 0, int(a->frames.size()) - 1);
            frame = a->frames[size_t(animFrame)];
        }
        return true;
    }
    if (f == "animation.curAnim.paused" || f == "animation.paused") { paused = toBool(v); return true; }
    if (f == "animation.curAnim.frameRate") {
        if (cur >= 0) anims[size_t(cur)].fps = d;
        return true;
    }
    if (f == "animation.frameIndex") {
        if (graphic) frame = clampv(int(d), 0, int(graphic->frames.size()) - 1);
        return true;
    }
    if (f == "text") { text.text = toStr(v); return true; }
    if (f == "size") { text.size = float(d); return true; }
    if (f == "borderSize") { text.borderSize = float(d); return true; }
    if (f == "fieldWidth") { text.fieldWidth = float(d); return true; }
    if (f == "blend") { blend = int(d); return true; }
    return false;
}

// ------------------------------------------------------------ Character --

void Character::recalcDance(bool initial) {
    const bool was = danceIdle;
    danceIdle = hasAnim("danceLeft" + idleSuffix) && hasAnim("danceRight" + idleSuffix);
    if (initial) danceEvery = danceIdle ? 1 : 2;
    else if (was != danceIdle) danceEvery = std::max(1, int(std::lround(danceIdle ? danceEvery / 2.0 : danceEvery * 2.0)));
}

void Character::dance() {
    if (skipDance || specialAnim) return;
    lastDanceAt = now;
    if (builtin) { lastAnim = "idle"; return; }
    if (danceIdle) {
        danced = !danced;
        playAnim((danced ? "danceRight" : "danceLeft") + idleSuffix);
    } else if (hasAnim("idle" + idleSuffix)) {
        playAnim("idle" + idleSuffix);
    }
}

void Character::playAnim(const std::string& n, bool force, bool rev, int startFrame) {
    specialAnim = false;
    lastAnim = n;
    lastAnimAt = now;
    if (builtin) return;
    play(n, force, rev, startFrame);
    if (name == "gf" || name.rfind("gf-", 0) == 0) {
        if (n == "singLEFT") danced = true;
        else if (n == "singRIGHT") danced = false;
        if (n == "singUP" || n == "singDOWN") danced = !danced;
    }
}

void Character::updateChar(double dt, double stepMs, bool playerHolding) {
    now += dt;
    const std::string an = builtin ? lastAnim : curAnimName();
    const bool animDone = builtin ? (now - lastAnimAt > 0.4) : finished;
    if (heyTimer > 0) {
        heyTimer -= dt;
        if (heyTimer <= 0) {
            if (specialAnim && (an == "hey" || an == "cheer")) { specialAnim = false; dance(); }
            heyTimer = 0;
        }
    } else if (specialAnim && animDone) {
        specialAnim = false;
        dance();
    } else if (an.size() >= 4 && an.compare(an.size() - 4, 4, "miss") == 0 && animDone) {
        dance();
        finishAnim();
    }
    const bool singing = an.rfind("sing", 0) == 0;
    if (singing) holdTimer += dt;
    else if (isPlayer) holdTimer = 0;
    const double limit = stepMs * 0.0011 * singDuration;
    if (singing && holdTimer >= limit) {
        const bool missAnim = an.size() >= 4 && an.compare(an.size() - 4, 4, "miss") == 0;
        if (!isPlayer || (!playerHolding && !missAnim)) {
            dance();
            holdTimer = 0;
        }
    }
    if (!builtin && finished && hasAnim(an + "-loop")) playAnim(an + "-loop");
}

bool Character::get(std::string_view f, Value& out) const {
    if (f == "holdTimer") { out = holdTimer; return true; }
    if (f == "heyTimer") { out = heyTimer; return true; }
    if (f == "specialAnim") { out = specialAnim; return true; }
    if (f == "stunned") { out = stunned; return true; }
    if (f == "skipDance") { out = skipDance; return true; }
    if (f == "danceIdle") { out = danceIdle; return true; }
    if (f == "danceEveryNumBeats") { out = double(danceEvery); return true; }
    if (f == "idleSuffix") { out = idleSuffix; return true; }
    if (f == "singDuration") { out = singDuration; return true; }
    if (f == "curCharacter") { out = name; return true; }
    if (f == "healthIcon") { out = icon; return true; }
    if (f == "isPlayer") { out = isPlayer; return true; }
    if (f == "hasMissAnimations") { out = hasMiss; return true; }
    if (f == "cameraPosition[0]") { out = double(camX); return true; }
    if (f == "cameraPosition[1]") { out = double(camY); return true; }
    if (f == "positionArray[0]") { out = double(posX); return true; }
    if (f == "positionArray[1]") { out = double(posY); return true; }
    if (f == "healthColorArray[0]") { out = double((barColor >> 16) & 255); return true; }
    if (f == "healthColorArray[1]") { out = double((barColor >> 8) & 255); return true; }
    if (f == "healthColorArray[2]") { out = double(barColor & 255); return true; }
    if (builtin && (f == "animation.curAnim.name" || f == "animation.name")) { out = lastAnim; return true; }
    return Sprite::get(f, out);
}

bool Character::set(std::string_view f, const Value& v) {
    if (f == "holdTimer") { holdTimer = toNum(v); return true; }
    if (f == "heyTimer") { heyTimer = toNum(v); return true; }
    if (f == "specialAnim") { specialAnim = toBool(v); return true; }
    if (f == "stunned") { stunned = toBool(v); return true; }
    if (f == "skipDance") { skipDance = toBool(v); return true; }
    if (f == "danceEveryNumBeats") { danceEvery = std::max(1, int(toNum(v, 2))); return true; }
    if (f == "idleSuffix") { idleSuffix = toStr(v); recalcDance(false); return true; }
    if (f == "singDuration") { singDuration = toNum(v, 4); return true; }
    if (f == "healthIcon") { icon = toStr(v); return true; }
    if (f == "cameraPosition[0]") { camX = float(toNum(v)); return true; }
    if (f == "cameraPosition[1]") { camY = float(toNum(v)); return true; }
    return Sprite::set(f, v);
}

// --------------------------------------------------------------- Camera --

void Camera::update(double dt, unsigned seed) {
    if (shakeTime > 0) {
        shakeTime -= float(dt);
        unsigned s = seed;
        shakeX = frand(s) * shakeIntensity * kW;
        shakeY = frand(s) * shakeIntensity * kH;
        if (shakeTime <= 0) shakeX = shakeY = 0;
    }
    if (flashTime > 0) flashTime = std::max(0.0f, flashTime - float(dt));
    if (fadeDur > 0 && fadeTime < fadeDur) {
        fadeTime = std::min(fadeDur, fadeTime + float(dt));
        const float k = fadeTime / fadeDur;
        fadeAlpha = fadeIn ? 1.0f - k : k;
    }
}

bool Camera::get(std::string_view f, Value& out) const {
    if (f == "zoom") { out = double(zoom); return true; }
    if (f == "angle") { out = double(angle); return true; }
    if (f == "alpha") { out = double(alpha); return true; }
    if (f == "visible") { out = visible; return true; }
    if (f == "x") { out = double(x); return true; }
    if (f == "y") { out = double(y); return true; }
    if (f == "scroll.x") { out = double(scrollX); return true; }
    if (f == "scroll.y") { out = double(scrollY); return true; }
    if (f == "width") { out = double(kW); return true; }
    if (f == "height") { out = double(kH); return true; }
    if (f == "bgColor") { out = double(bgColor); return true; }
    return false;
}

bool Camera::set(std::string_view f, const Value& v) {
    const float d = float(toNum(v));
    if (f == "zoom") { zoom = d; return true; }
    if (f == "angle") { angle = d; return true; }
    if (f == "alpha") { alpha = clampv(d, 0.0f, 1.0f); return true; }
    if (f == "visible") { visible = toBool(v, true); return true; }
    if (f == "x") { x = d; return true; }
    if (f == "y") { y = d; return true; }
    if (f == "scroll.x") { scrollX = d; return true; }
    if (f == "scroll.y") { scrollY = d; return true; }
    if (f == "bgColor") { bgColor = uint32_t(int64_t(toNum(v))); return true; }
    return false;
}

// ---------------------------------------------------------------- Strum --

bool Strum::get(std::string_view f, Value& out) const {
    if (f == "x") { out = double(x); return true; }
    if (f == "y") { out = double(y); return true; }
    if (f == "alpha") { out = double(alpha); return true; }
    if (f == "angle") { out = double(angle); return true; }
    if (f == "visible") { out = visible; return true; }
    if (f == "scale.x") { out = double(scaleX); return true; }
    if (f == "scale.y") { out = double(scaleY); return true; }
    if (f == "direction") { out = double(direction); return true; }
    if (f == "downScroll") { out = downScroll; return true; }
    return false;
}

bool Strum::set(std::string_view f, const Value& v) {
    const float d = float(toNum(v));
    if (f == "x") { x = d; return true; }
    if (f == "y") { y = d; return true; }
    if (f == "alpha") { alpha = clampv(d, 0.0f, 1.0f); return true; }
    if (f == "angle") { angle = d; return true; }
    if (f == "visible") { visible = toBool(v, true); return true; }
    if (f == "scale.x") { scaleX = d; return true; }
    if (f == "scale.y") { scaleY = d; return true; }
    if (f == "direction") { direction = d; return true; }
    if (f == "downScroll") { downScroll = toBool(v); return true; }
    return false;
}

// ---------------------------------------------------------------- Scene --

Cam Scene::camFromName(std::string_view n) {
    const std::string l = toLower(n);
    if (l == "camhud" || l == "hud") return Cam::Hud;
    if (l == "camother" || l == "other") return Cam::Other;
    return Cam::Game;
}

Camera* Scene::camera(std::string_view name) {
    const std::string l = toLower(name);
    if (l == "camgame" || l == "game" || l == "flxg.camera" || l == "camera") return &camGame;
    if (l == "camhud" || l == "hud") return &camHud;
    if (l == "camother" || l == "other") return &camOther;
    return nullptr;
}

Character* Scene::character(std::string_view n) const {
    const std::string l = toLower(n);
    if (l == "bf" || l == "boyfriend" || l == "0" || l == "player" || l == "boyfriendgroup") return bf.get();
    if (l == "dad" || l == "opponent" || l == "1" || l == "dadgroup") return dad.get();
    if (l == "gf" || l == "girlfriend" || l == "2" || l == "gfgroup") return gf.get();
    return nullptr;
}

Sprite* Scene::find(std::string_view name) const {
    if (auto it = objects.find(name); it != objects.end()) return it->second.get();
    if (Character* c = character(name)) return c;
    if (auto it = hud.find(name); it != hud.end()) return it->second.get();
    return nullptr;
}

void Scene::addBehindCharacters(const SpritePtr& s) {
    remove(s);
    // Psych inserts below the lowest character group.
    size_t pos = order.size();
    for (size_t i = 0; i < order.size(); ++i) {
        const Sprite* o = order[i].get();
        if (o == gfMarker.get() || o == dad.get() || o == bf.get() || o == gf.get()) { pos = i; break; }
    }
    order.insert(order.begin() + std::ptrdiff_t(pos), s);
    s->inScene = true;
}

void Scene::addFront(const SpritePtr& s) {
    remove(s);
    order.push_back(s);
    s->inScene = true;
}

void Scene::remove(const SpritePtr& s) {
    order.erase(std::remove(order.begin(), order.end(), s), order.end());
    s->inScene = false;
}

int Scene::indexOf(std::string_view name) const {
    const Sprite* target = nullptr;
    const std::string l = toLower(name);
    if (l == "gfgroup") target = gfMarker.get();
    else target = find(name);
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i].get() == target) return int(i);
    return -1;
}

void Scene::setOrder(std::string_view name, int pos) {
    const int i = indexOf(name);
    if (i < 0) return;
    SpritePtr s = order[size_t(i)];
    order.erase(order.begin() + i);
    pos = clampv(pos, 0, int(order.size()));
    order.insert(order.begin() + pos, s);
}

Strum* Scene::strumByGroup(std::string_view group, int index) {
    const std::string g = toLower(group);
    int i = index;
    if (g == "playerstrums") i = keys + index;
    else if (g == "opponentstrums") i = index;
    else if (g != "strumlinenotes") return nullptr;
    if (i < 0 || i >= int(strums.size())) return nullptr;
    if ((g == "playerstrums" || g == "opponentstrums") && (index < 0 || index >= keys)) return nullptr;
    return &strums[size_t(i)];
}

bool Scene::getProp(std::string_view path, Value& out) const {
    std::string_view head, rest;
    splitHead(path, head, rest);
    const std::string h = toLower(head);
    Scene* self = const_cast<Scene*>(this);
    if (Camera* c = self->camera(head); c && !rest.empty()) return c->get(rest, out);
    // camFollow is where the camera is heading; camFollowPos (Psych 0.6,
    // JS Engine) is where it is now: the centre of camGame.
    if (h == "camfollow") {
        if (rest == "x") { out = double(followX); return true; }
        if (rest == "y") { out = double(followY); return true; }
        return false;
    }
    if (h == "camfollowpos") {
        if (rest == "x") { out = double(camGame.scrollX + kW * 0.5f); return true; }
        if (rest == "y") { out = double(camGame.scrollY + kH * 0.5f); return true; }
        return false;
    }
    if (const float* off = camOffsetOf(stage, h)) {  // boyfriendCameraOffset[0] and friends
        if (rest != "[0]" && rest != "[1]") return false;
        out = double(off[rest == "[1]" ? 1 : 0]);
        return true;
    }
    if (path == "defaultCamZoom") { out = defaultCamZoom; return true; }
    if (path == "cameraSpeed") { out = cameraSpeed; return true; }
    if (path == "camZooming") { out = camZooming; return true; }
    if (path == "camZoomingMult") { out = camZoomingMult; return true; }
    if (path == "camZoomingDecay") { out = camZoomingDecay; return true; }
    if (path == "isCameraOnForcedPos") { out = camForced; return true; }
    if (path == "gfSpeed") { out = gfSpeed; return true; }
    if (h == "playerstrums" || h == "opponentstrums" || h == "strumlinenotes") {
        if (rest == "length") {
            out = double(h == "strumlinenotes" ? strums.size() : size_t(keys));
            return true;
        }
        if (rest.rfind("members[", 0) == 0) {
            const size_t e = rest.find(']');
            if (e == std::string_view::npos) return false;
            const int idx = std::atoi(std::string(rest.substr(8, e - 8)).c_str());
            std::string_view f = rest.substr(e + 1);
            if (!f.empty() && f[0] == '.') f.remove_prefix(1);
            if (const Strum* s = self->strumByGroup(head, idx)) return s->get(f, out);
        }
        return false;
    }
    if (h == "boyfriendgroup" || h == "dadgroup" || h == "gfgroup") {
        const Character* c = character(h);
        if (!c) return false;
        if (rest == "x") { out = double(c->groupX); return true; }
        if (rest == "y") { out = double(c->groupY); return true; }
        return c->get(rest, out);
    }
    if (const Sprite* s = find(head)) {
        if (rest.empty()) { out = std::string(head); return true; }
        return s->get(rest, out);
    }
    if (auto it = vars.find(path); it != vars.end()) { out = it->second; return true; }
    return false;
}

bool Scene::setProp(std::string_view path, const Value& v) {
    std::string_view head, rest;
    splitHead(path, head, rest);
    const std::string h = toLower(head);
    if (Camera* c = camera(head); c && !rest.empty()) return c->set(rest, v);
    if (h == "camfollow") {
        if (rest == "x") { followX = float(toNum(v)); return true; }
        if (rest == "y") { followY = float(toNum(v)); return true; }
        return false;
    }
    if (h == "camfollowpos") {  // moves the camera itself
        if (rest == "x") { camGame.scrollX = float(toNum(v)) - kW * 0.5f; return true; }
        if (rest == "y") { camGame.scrollY = float(toNum(v)) - kH * 0.5f; return true; }
        return false;
    }
    if (const float* off = camOffsetOf(stage, h)) {
        if (rest != "[0]" && rest != "[1]") return false;
        const_cast<float*>(off)[rest == "[1]" ? 1 : 0] = float(toNum(v));
        return true;
    }
    if (path == "defaultCamZoom") { defaultCamZoom = toNum(v, defaultCamZoom); return true; }
    if (path == "cameraSpeed") { cameraSpeed = toNum(v, 1); return true; }
    if (path == "camZooming") { camZooming = toBool(v); return true; }
    if (path == "camZoomingMult") { camZoomingMult = toNum(v, 1); return true; }
    if (path == "camZoomingDecay") { camZoomingDecay = toNum(v, 1); return true; }
    if (path == "isCameraOnForcedPos") { camForced = toBool(v); return true; }
    if (path == "gfSpeed") { gfSpeed = std::max(1.0, toNum(v, 1)); return true; }
    if (h == "playerstrums" || h == "opponentstrums" || h == "strumlinenotes") {
        if (rest.rfind("members[", 0) != 0) return false;
        const size_t e = rest.find(']');
        if (e == std::string_view::npos) return false;
        const int idx = std::atoi(std::string(rest.substr(8, e - 8)).c_str());
        std::string_view f = rest.substr(e + 1);
        if (!f.empty() && f[0] == '.') f.remove_prefix(1);
        if (Strum* s = strumByGroup(head, idx)) return s->set(f, v);
        return false;
    }
    if (h == "boyfriendgroup" || h == "dadgroup" || h == "gfgroup") {
        Character* c = character(h);
        if (!c) return false;
        if (rest == "x") { const float nx = float(toNum(v)); c->x += nx - c->groupX; c->groupX = nx; return true; }
        if (rest == "y") { const float ny = float(toNum(v)); c->y += ny - c->groupY; c->groupY = ny; return true; }
        return c->set(rest, v);
    }
    if (Sprite* s = find(head)) return !rest.empty() && s->set(rest, v);
    vars[std::string(path)] = v;
    return true;
}

void Scene::tween(Tween tw) {
    if (!tw.tag.empty()) cancelTween(tw.tag);
    tw.dur = std::max(1e-6, tw.dur);
    tweens.push_back(std::move(tw));
}

void Scene::cancelTween(std::string_view tag) {
    tweens.erase(std::remove_if(tweens.begin(), tweens.end(), [&](const Tween& t) { return t.tag == tag; }), tweens.end());
}

void Scene::timer(Timer t) {
    if (!t.tag.empty()) cancelTimer(t.tag);
    t.time = std::max(0.0, t.time);
    timers.push_back(std::move(t));
}

void Scene::cancelTimer(std::string_view tag) {
    timers.erase(std::remove_if(timers.begin(), timers.end(), [&](const Timer& t) { return t.tag == tag; }), timers.end());
}

void Scene::cameraTarget(float& x, float& y, int who) const {
    const Character* c = who == 0 ? bf.get() : who == 1 ? dad.get() : gf.get();
    if (!c) {
        x = followX;
        y = followY;
        return;
    }
    const float mx = c->x + c->width * 0.5f, my = c->y + c->height * 0.5f;
    if (who == 1) {
        x = mx + 150 + c->camX + stage.camDad[0];
        y = my - 100 + c->camY + stage.camDad[1];
    } else if (who == 0) {
        x = mx - 100 - c->camX + stage.camBf[0];
        y = my - 100 + c->camY + stage.camBf[1];
    } else {
        x = mx + c->camX + stage.camGf[0];
        y = my + c->camY + stage.camGf[1];
    }
}

void Scene::focusOn(int who) {
    if (camForced) return;
    cameraTarget(followX, followY, who);
}

void Scene::update(double dt, double stepMs, bool playerHolding) {
    now += dt;
    ++frameSeed;
    // Tweens (a completion may start new tweens, so iterate by index).
    for (size_t i = 0; i < tweens.size();) {
        Tween& tw = tweens[i];
        if (tw.delay > 0) { tw.delay -= dt; ++i; continue; }
        // An empty target tweens a top-level value such as defaultCamZoom.
        auto pathOf = [&](const std::string& field) { return tw.target.empty() ? field : tw.target + "." + field; };
        if (!tw.isColor)
            for (Tween::Field& f : tw.fields)
                if (!f.init) {
                    Value cur;
                    f.from = getProp(pathOf(f.name), cur) ? toNum(cur, f.to) : f.to;
                    f.init = true;
                }
        tw.t += dt;
        const double k = psych::ease(tw.ease, tw.t / tw.dur);
        if (tw.isColor) {
            setProp(pathOf("color"), double(lerpRgb(tw.c0, tw.c1, k)));
        } else {
            for (const Tween::Field& f : tw.fields) setProp(pathOf(f.name), f.from + (f.to - f.from) * k);
        }
        if (tw.t >= tw.dur) {
            Completion c;
            c.script = tw.script;
            c.tag = tw.tag;
            c.fn = tw.onComplete;
            completions.push_back(std::move(c));
            tweens.erase(tweens.begin() + std::ptrdiff_t(i));
            continue;
        }
        ++i;
    }
    for (size_t i = 0; i < timers.size();) {
        Timer& t = timers[i];
        t.t += dt;
        bool removeIt = false;
        int guard = 0;
        while (t.t >= t.time && guard++ < 64) {
            t.t -= t.time;
            ++t.done;
            Completion c;
            c.timer = true;
            c.script = t.script;
            c.tag = t.tag;
            c.loops = t.loops;
            c.left = t.loops > 0 ? std::max(0, t.loops - t.done) : 0;
            completions.push_back(c);
            if (t.loops > 0 && t.done >= t.loops) { removeIt = true; break; }
            if (t.time <= 0) break;
        }
        if (removeIt) timers.erase(timers.begin() + std::ptrdiff_t(i));
        else ++i;
    }
    // Sprites
    for (auto& kv : objects) {
        Sprite& s = *kv.second;
        if (!s.active) continue;
        s.updateAnim(dt);
        // Extra characters made by scripts behave like the main three.
        if (s.kind == Kind::Character && &s != bf.get() && &s != dad.get() && &s != gf.get())
            static_cast<Character&>(s).updateChar(dt, stepMs, false);
    }
    for (Character* c : {bf.get(), dad.get(), gf.get()}) {
        if (!c) continue;
        c->updateAnim(dt);
        c->updateChar(dt, stepMs, c == bf.get() && playerHolding);
    }
    camGame.update(dt, frameSeed * 7919u);
    camHud.update(dt, frameSeed * 104729u);
    camOther.update(dt, frameSeed * 1299709u);
    // Built-in effects keep their own clock.
    auto tick = [&](const ShaderPtr& sh) {
        if (!sh || sh->timeRate == 0) return;
        std::vector<float>& t = sh->floats[sh->timeName];
        if (t.empty()) t.push_back(0);
        t[0] += float(dt) * sh->timeRate;
    };
    for (Camera* c : {&camGame, &camHud, &camOther})
        for (const ShaderPtr& sh : c->filters) tick(sh);
    for (auto& kv : objects) tick(kv.second->shader);
    for (Character* c : {bf.get(), dad.get(), gf.get()})
        if (c) tick(c->shader);
    // Camera follow and zoom decay (Psych PlayState.update).
    const float tx = followX - kW * 0.5f, ty = followY - kH * 0.5f;
    const float rate = float(2.4 * cameraSpeed);
    camGame.scrollX = approach(camGame.scrollX, tx, rate, float(dt));
    camGame.scrollY = approach(camGame.scrollY, ty, rate, float(dt));
    if (camZooming) {
        // The zoom drifts back to its default, except while a tween (a zoom
        // event, a script) is moving it: the tween wins.
        auto tweened = [&](const Camera& c) {
            for (const Tween& t : tweens) {
                if (t.delay > 0 || camera(t.target) != &c) continue;
                for (const Tween::Field& f : t.fields)
                    if (f.name == "zoom") return true;
            }
            return false;
        };
        const float zr = float(3.125 * camZoomingDecay);
        if (!tweened(camGame)) camGame.zoom = approach(camGame.zoom, float(defaultCamZoom), zr, float(dt));
        if (!tweened(camHud)) camHud.zoom = approach(camHud.zoom, 1.0f, zr, float(dt));
    }
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(toPath(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace gh::scene
