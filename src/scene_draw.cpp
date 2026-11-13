#include "scene_draw.hpp"

#include <algorithm>
#include <cmath>

#include <cstdarg>
#include <cstdio>

#include "common.hpp"
#include "gfx.hpp"
#include "rlgl.h"
#include "shader_glsl.hpp"

namespace gh {

using scene::Cam;
using scene::Camera;
using scene::Sprite;

namespace {
Color argbColor(uint32_t argb, float alphaMul = 1.0f) {
    const float a = float((argb >> 24) & 255) * alphaMul;
    return Color{(unsigned char)((argb >> 16) & 255), (unsigned char)((argb >> 8) & 255), (unsigned char)(argb & 255),
                 (unsigned char)clampv(a, 0.0f, 255.0f)};
}

struct Xf {  // 2x3 affine transform
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    Vector2 apply(float x, float y) const { return {a * x + c * y + tx, b * x + d * y + ty}; }
};

// Camera: zoom and rotation about the view centre, then shake and position.
Vector2 camPoint(const Camera& cam, float px, float py) {
    const float cx = scene::kW * 0.5f, cy = scene::kH * 0.5f;
    float dx = (px - cx) * cam.zoom, dy = (py - cy) * cam.zoom;
    if (cam.angle != 0) {
        const float r = cam.angle * float(kPi) / 180.0f, cs = std::cos(r), sn = std::sin(r);
        const float rx = dx * cs - dy * sn, ry = dx * sn + dy * cs;
        dx = rx;
        dy = ry;
    }
    return {cx + dx + cam.shakeX + cam.x, cy + dy + cam.shakeY + cam.y};
}

void setBlend(int& cur, int want) {
    if (cur == want) return;
    rlDrawRenderBatchActive();
    switch (want) {
        case 1: rlSetBlendMode(BLEND_ADDITIVE); break;
        case 2: rlSetBlendMode(BLEND_MULTIPLIED); break;
        case 3: rlSetBlendMode(BLEND_ADD_COLORS); break;
        case 4: rlSetBlendMode(BLEND_SUBTRACT_COLORS); break;
        case 5: rlSetBlendMode(BLEND_ALPHA_PREMULTIPLY); break;
        default: rlSetBlendMode(BLEND_ALPHA); break;
    }
    cur = want;
}

std::string* g_shaderLog = nullptr;
void captureLog(int level, const char* text, va_list args) {
    if (!g_shaderLog || level < LOG_WARNING) return;
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, text, args);
    *g_shaderLog += buf;
    *g_shaderLog += '\n';
}
}  // namespace

SceneRenderer::~SceneRenderer() { clear(); }

SceneRenderer::CompiledShader* SceneRenderer::shaderFor(const scene::Scene& sc, const std::string& name) {
    auto it = shaders_.find(name);
    if (it != shaders_.end()) return &it->second;
    CompiledShader& cs = shaders_[name];
    auto src = sc.shaderSources.find(name);
    if (src == sc.shaderSources.end() || src->second.frag.empty()) {
        errors.push_back("shader " + name + ": no fragment shader");
        return &cs;
    }
    const std::string glsl = translateShader(src->second.frag, rlGetVersion() == RL_OPENGL_ES_20);
    std::string log;
    g_shaderLog = &log;
    SetTraceLogCallback(captureLog);
    cs.sh = LoadShaderFromMemory(nullptr, glsl.c_str());
    SetTraceLogCallback(nullptr);
    g_shaderLog = nullptr;
    cs.ok = cs.sh.id != 0 && cs.sh.id != rlGetShaderIdDefault();
    if (!cs.ok) {
        // Keep the compiler's own message (first error line).
        std::string first;
        size_t p = log.find("ERROR");
        if (p == std::string::npos) p = log.find("error");
        if (p != std::string::npos) first = log.substr(p, log.find('\n', p) - p);
        errors.push_back("shader " + name + " failed to compile" + (first.empty() ? "" : ": " + first));
    }
    return &cs;
}

int SceneRenderer::loc(CompiledShader& cs, const std::string& name) {
    auto it = cs.locs.find(name);
    if (it != cs.locs.end()) return it->second;
    const int l = GetShaderLocation(cs.sh, name.c_str());
    cs.locs[name] = l;
    return l;
}

void SceneRenderer::applyUniforms(CompiledShader& cs, const scene::ShaderInstance& inst, float texW, float texH,
                                  bool renderTexture, const scene::Scene& sc) {
    const float size[2] = {texW, texH};
    if (int l = loc(cs, "openfl_TextureSize"); l >= 0) SetShaderValue(cs.sh, l, size, SHADER_UNIFORM_VEC2);
    const float flip = renderTexture ? 1.0f : 0.0f, premul = renderTexture ? 0.0f : 1.0f;
    if (int l = loc(cs, "gh_flipY"); l >= 0) SetShaderValue(cs.sh, l, &flip, SHADER_UNIFORM_FLOAT);
    if (int l = loc(cs, "gh_premul"); l >= 0) SetShaderValue(cs.sh, l, &premul, SHADER_UNIFORM_FLOAT);
    const int one = 1;
    if (int l = loc(cs, "hasTransform"); l >= 0) SetShaderValue(cs.sh, l, &one, SHADER_UNIFORM_INT);
    if (int l = loc(cs, "hasColorTransform"); l >= 0) SetShaderValue(cs.sh, l, &one, SHADER_UNIFORM_INT);
    for (const auto& [name, v] : inst.floats) {
        const int l = loc(cs, name);
        if (l < 0 || v.empty()) continue;
        static const int types[] = {SHADER_UNIFORM_FLOAT, SHADER_UNIFORM_VEC2, SHADER_UNIFORM_VEC3, SHADER_UNIFORM_VEC4};
        if (v.size() <= 4) SetShaderValue(cs.sh, l, v.data(), types[v.size() - 1]);
        else SetShaderValueV(cs.sh, l, v.data(), SHADER_UNIFORM_FLOAT, int(v.size()));
    }
    for (const auto& [name, v] : inst.ints) {
        const int l = loc(cs, name);
        if (l < 0 || v.empty()) continue;
        static const int types[] = {SHADER_UNIFORM_INT, SHADER_UNIFORM_IVEC2, SHADER_UNIFORM_IVEC3, SHADER_UNIFORM_IVEC4};
        if (v.size() <= 4) SetShaderValue(cs.sh, l, v.data(), types[v.size() - 1]);
        else SetShaderValueV(cs.sh, l, v.data(), SHADER_UNIFORM_INT, int(v.size()));
    }
    for (const auto& [name, key] : inst.samplers) {
        const int l = loc(cs, name);
        if (l < 0) continue;
        scene::GraphicPtr g = const_cast<scene::Scene&>(sc).gfx.image(key);
        if (!g || g->files.empty()) continue;
        if (Texture2D* t = texture(g->files[0])) SetShaderValueTexture(cs.sh, l, *t);
    }
}

void SceneRenderer::drawFiltered(scene::Scene& sc, const std::vector<scene::ShaderPtr>& filters, const RenderTexture2D& src,
                                 Rectangle dest) {
    const float w = float(src.texture.width), h = float(src.texture.height);
    std::vector<CompiledShader*> chain;
    std::vector<const scene::ShaderInstance*> insts;
    for (const scene::ShaderPtr& f : filters) {
        if (!f) continue;
        CompiledShader* cs = shaderFor(sc, f->name);
        if (cs && cs->ok) { chain.push_back(cs); insts.push_back(f.get()); }
    }
    rlDrawRenderBatchActive();
    if (chain.empty()) {
        rlSetBlendMode(BLEND_ALPHA_PREMULTIPLY);
        DrawTexturePro(src.texture, {0, 0, w, -h}, dest, {0, 0}, 0, WHITE);
        rlDrawRenderBatchActive();
        rlSetBlendMode(BLEND_ALPHA);
        return;
    }
    const Texture2D* in = &src.texture;
    int k = 0;
    for (size_t i = 0; i < chain.size(); ++i) {
        const bool last = i + 1 == chain.size();
        if (!last) {
            RenderTexture2D& rt = pingPong_[k];
            if (!rt.id || rt.texture.width != int(w) || rt.texture.height != int(h)) {
                if (rt.id) UnloadRenderTexture(rt);
                rt = LoadRenderTexture(int(w), int(h));
            }
            gfx::beginTexture(rt);
            ClearBackground(BLANK);
        }
        BeginShaderMode(chain[i]->sh);
        applyUniforms(*chain[i], *insts[i], w, h, true, sc);
        rlSetBlendMode(BLEND_ALPHA_PREMULTIPLY);
        DrawTexturePro(*in, {0, 0, w, -h}, last ? dest : Rectangle{0, 0, w, h}, {0, 0}, 0, WHITE);
        rlDrawRenderBatchActive();
        EndShaderMode();
        rlSetBlendMode(BLEND_ALPHA);
        if (!last) {
            gfx::endTexture();
            in = &pingPong_[k].texture;
            k ^= 1;
        }
    }
}

void SceneRenderer::clear() {
    if (!IsWindowReady()) return;
    for (auto& kv : shaders_)
        if (kv.second.ok) UnloadShader(kv.second.sh);
    shaders_.clear();
    for (RenderTexture2D& rt : pingPong_)
        if (rt.id) { UnloadRenderTexture(rt); rt = RenderTexture2D{}; }
    for (auto& kv : tex_)
        if (kv.second.id) UnloadTexture(kv.second);
    tex_.clear();
    filter_.clear();
    for (auto& kv : fonts_)
        if (kv.second.texture.id) UnloadFont(kv.second);
    fonts_.clear();
}

Texture2D* SceneRenderer::texture(const std::string& file) {
    if (auto d = dynamic_.find(file); d != dynamic_.end()) return d->second.id ? &d->second : nullptr;
    if (file.rfind("video:", 0) == 0) return nullptr;
    auto it = tex_.find(file);
    if (it != tex_.end()) return it->second.id ? &it->second : nullptr;
    Texture2D t{};
    std::string bytes;
    if (scene::readFile(file, bytes) && !bytes.empty()) {
        Image img = LoadImageFromMemory(".png", reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()));
        if (img.data) {
            t = LoadTextureFromImage(img);
            UnloadImage(img);
            SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
        }
    }
    tex_[file] = t;
    filter_[file] = 1;
    return t.id ? &tex_[file] : nullptr;
}

Font* SceneRenderer::font(const std::string& file) {
    if (file.empty()) return nullptr;
    auto it = fonts_.find(file);
    if (it != fonts_.end()) return it->second.texture.id ? &it->second : nullptr;
    Font f{};
    std::string bytes;
    std::string ext = toLower(file.substr(file.find_last_of('.') == std::string::npos ? file.size() : file.find_last_of('.')));
    if (scene::readFile(file, bytes) && !bytes.empty() && (ext == ".ttf" || ext == ".otf"))
        f = LoadFontFromMemory(ext.c_str(), reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), 64,
                               nullptr, 0);
    if (f.texture.id) SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    fonts_[file] = f;
    return f.texture.id ? &fonts_[file] : nullptr;
}

void SceneRenderer::preload(scene::Scene& sc) {
    for (const std::string& f : sc.gfx.pngFiles()) texture(f);
}

Vector2 SceneRenderer::toScreen(const Camera& c, float x, float y, float sfx, float sfy) {
    return camPoint(c, x - c.scrollX * sfx, y - c.scrollY * sfy);
}

void SceneRenderer::drawSprite(const Sprite& s, const Camera& cam) {
    if (!s.graphic || s.graphic->frames.empty()) return;
    const float alpha = s.alpha * cam.alpha;
    if (alpha <= 0.002f || s.scaleX == 0 || s.scaleY == 0) return;
    const scene::Graphic& g = *s.graphic;
    const psych::AtlasFrame& f = g.frames[size_t(clampv(s.frame, 0, int(g.frames.size()) - 1))];

    Texture2D* tex = nullptr;
    if (!g.solid) {
        if (f.tex >= g.files.size()) return;
        const std::string& file = g.files[f.tex];
        tex = texture(file);
        if (!tex) return;
        const int want = s.antialiasing ? 1 : 0;
        int& cur = filter_[file];
        if (cur != want) {
            rlDrawRenderBatchActive();
            SetTextureFilter(*tex, want ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
            cur = want;
        }
    }

    // Frame-local rectangle (inside the untrimmed source box), with flips.
    const float dw = f.rotated ? f.h : f.w, dh = f.rotated ? f.w : f.h;
    const float ox = -f.fx, oy = -f.fy;
    float lx[4] = {ox, ox + dw, ox + dw, ox};  // TL, TR, BR, BL
    float ly[4] = {oy, oy, oy + dh, oy + dh};
    if (s.flipX)
        for (float& v : lx) v = f.fw - v;
    if (s.flipY)
        for (float& v : ly) v = f.fh - v;

    // Sprite transform (Flixel drawComplex), then the camera.
    const float rad = s.angle * float(kPi) / 180.0f, cs = std::cos(rad), sn = std::sin(rad);
    const float px = s.x - cam.scrollX * s.scrollX - s.offsetX + s.originX;
    const float py = s.y - cam.scrollY * s.scrollY - s.offsetY + s.originY;
    Vector2 pts[4];
    for (int i = 0; i < 4; ++i) {
        const float ux = (lx[i] - s.originX) * s.scaleX, uy = (ly[i] - s.originY) * s.scaleY;
        pts[i] = camPoint(cam, px + ux * cs - uy * sn, py + ux * sn + uy * cs);
    }

    // Texture coordinates for TL, TR, BR, BL of the displayed frame.
    float u[4], v[4];
    if (tex) {
        const float tw = float(tex->width), th = float(tex->height);
        const float x0 = f.x / tw, y0 = f.y / th, x1 = (f.x + f.w) / tw, y1 = (f.y + f.h) / th;
        if (!f.rotated) {
            u[0] = x0; v[0] = y0; u[1] = x1; v[1] = y0; u[2] = x1; v[2] = y1; u[3] = x0; v[3] = y1;
        } else {  // stored rotated 90 degrees clockwise
            u[0] = x1; v[0] = y0; u[1] = x1; v[1] = y1; u[2] = x0; v[2] = y1; u[3] = x0; v[3] = y0;
        }
    } else {
        // makeGraphic: a bitmap of its own in Flixel, so a shader on it sees
        // openfl_TextureCoordv run from 0 to 1 across the sprite (sampling the
        // 1x1 white texture anywhere gives the same colour).
        u[0] = 0; v[0] = 0; u[1] = 1; v[1] = 0; u[2] = 1; v[2] = 1; u[3] = 0; v[3] = 1;
    }

    uint32_t tint = s.color & 0xFFFFFF;
    Color col;
    if (g.solid) {
        const uint32_t sc = g.solidColor;
        auto mul = [&](int sh) { return (unsigned char)(((sc >> sh) & 255) * ((tint >> sh) & 255) / 255); };
        col = Color{mul(16), mul(8), mul(0), (unsigned char)clampv(float((sc >> 24) & 255) * alpha, 0.0f, 255.0f)};
    } else {
        col = argbColor(0xFF000000u | tint, alpha);
    }
    CompiledShader* shp = nullptr;
    if (s.shader && sc_) {
        shp = shaderFor(*sc_, s.shader->name);
        if (shp && !shp->ok) shp = nullptr;
    }
    if (shp) {
        // Shader output is premultiplied (OpenFL style).
        rlDrawRenderBatchActive();
        BeginShaderMode(shp->sh);
        // openfl_TextureSize: the bitmap's size (a makeGraphic's own width and height).
        applyUniforms(*shp, *s.shader, tex ? float(tex->width) : std::max(1.0f, f.w), tex ? float(tex->height) : std::max(1.0f, f.h),
                      false, *sc_);
        setBlend(blend_, 5);
    } else {
        setBlend(blend_, s.blend);
    }
    rlSetTexture(tex ? tex->id : rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
    rlColor4ub(col.r, col.g, col.b, col.a);
    rlNormal3f(0, 0, 1);
    const int order[4] = {0, 3, 2, 1};  // TL, BL, BR, TR (raylib's winding)
    for (int k : order) {
        rlTexCoord2f(u[k], v[k]);
        rlVertex2f(pts[k].x, pts[k].y);
    }
    rlEnd();
    rlSetTexture(0);
    if (shp) {
        rlDrawRenderBatchActive();
        EndShaderMode();
    }
}

void SceneRenderer::drawText(Sprite& s, const Camera& cam) {
    const scene::TextStyle& t = s.text;
    Font* custom = nullptr;
    if (!t.font.empty() && t.font != "vcr.ttf") {
        // Mods ship fonts in fonts/; the scene's file lookup is not reachable
        // here, so the play state stores resolved paths in the font field.
        custom = font(t.font);
    }
    const Font& fnt = custom ? *custom : gfx::fontOf(gfx::FontId::Ui);
    const float size = std::max(1.0f, t.size * (custom ? 1.0f : 1.15f));
    // Measure and wrap to the field width.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char ch : t.text) {
            if (ch == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        lines.push_back(cur);
    }
    if (t.fieldWidth > 0) {
        std::vector<std::string> wrapped;
        for (const std::string& line : lines) {
            std::string acc;
            size_t p = 0;
            while (p <= line.size()) {
                size_t sp = line.find(' ', p);
                if (sp == std::string::npos) sp = line.size();
                const std::string word = line.substr(p, sp - p);
                const std::string trial = acc.empty() ? word : acc + " " + word;
                if (!acc.empty() && MeasureTextEx(fnt, trial.c_str(), size, 0).x > t.fieldWidth) {
                    wrapped.push_back(acc);
                    acc = word;
                } else {
                    acc = trial;
                }
                p = sp + 1;
            }
            wrapped.push_back(acc);
        }
        lines.swap(wrapped);
    }
    float maxW = 0;
    std::vector<float> widths;
    for (const std::string& l : lines) {
        widths.push_back(MeasureTextEx(fnt, l.c_str(), size, 0).x);
        maxW = std::max(maxW, widths.back());
    }
    const float boxW = t.fieldWidth > 0 ? t.fieldWidth : maxW + 4;
    const float lineH = size * 1.1f;
    s.width = boxW * std::fabs(s.scaleX);
    s.height = (lineH * float(lines.size()) + 4) * std::fabs(s.scaleY);
    s.frameW = boxW;
    s.frameH = lineH * float(lines.size()) + 4;
    const float alpha = s.alpha * cam.alpha;
    if (alpha <= 0.002f || t.text.empty()) return;

    const Vector2 tl = camPoint(cam, s.x - cam.scrollX * s.scrollX - s.offsetX, s.y - cam.scrollY * s.scrollY - s.offsetY);
    const float z = cam.zoom * std::fabs(s.scaleX);
    const Color fill = argbColor(t.color, alpha);
    const Color border = argbColor(t.borderColor, alpha);
    setBlend(blend_, 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        float x = 2;
        if (t.align == 1) x = (boxW - widths[i]) * 0.5f;
        else if (t.align == 2) x = boxW - widths[i] - 2;
        const Vector2 p{tl.x + x * z, tl.y + (2 + lineH * float(i)) * z};
        const float fs = size * z;
        if (t.borderSize > 0 && border.a > 0) {
            const float b = t.borderSize * z;
            for (int k = 0; k < 8; ++k) {
                const float ang = float(k) * float(kPi) / 4.0f;
                DrawTextPro(fnt, lines[i].c_str(), {p.x + std::cos(ang) * b, p.y + std::sin(ang) * b}, {0, 0}, s.angle, fs, 0,
                            border);
            }
        }
        DrawTextPro(fnt, lines[i].c_str(), p, {0, 0}, s.angle, fs, 0, fill);
    }
}

void SceneRenderer::drawCamera(scene::Scene& sc, Cam which,
                               const std::function<void(const Sprite& marker)>& marker,
                               const std::function<void(const scene::Character& c, Vector2 feet, float scale)>& builtin) {
    const Camera& cam = which == Cam::Game ? sc.camGame : which == Cam::Hud ? sc.camHud : sc.camOther;
    if (!cam.visible) return;
    sc_ = &sc;
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    blend_ = 0;
    // Copy: scripts may reorder while we draw markers.
    const std::vector<scene::SpritePtr> list = sc.order;
    for (const scene::SpritePtr& sp : list) {
        Sprite& s = *sp;
        if (s.kind == scene::Kind::Marker) {
            if (marker) {
                setBlend(blend_, 0);
                marker(s);
            }
            continue;
        }
        if (s.cam != which || !s.visible) continue;
        if (s.kind == scene::Kind::Text) {
            drawText(s, cam);
            continue;
        }
        if (s.kind == scene::Kind::Character) {
            const auto& c = static_cast<const scene::Character&>(s);
            if (c.builtin) {
                if (builtin) {
                    setBlend(blend_, 0);
                    const Vector2 feet = toScreen(cam, c.x + c.width * 0.5f, c.y + c.height, c.scrollX, c.scrollY);
                    builtin(c, feet, cam.zoom * std::fabs(c.scaleX));
                }
                continue;
            }
        }
        drawSprite(s, cam);
    }
    setBlend(blend_, 0);
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
}

void SceneRenderer::drawEffects(const Camera& c) {
    if (c.flashTime > 0 && c.flashDur > 0) {
        Color col = argbColor(c.flashColor, c.flashTime / c.flashDur);
        DrawRectangleRec({-10, -10, scene::kW + 20, scene::kH + 20}, col);
    }
    if (c.fadeDur > 0 && c.fadeAlpha > 0.001f) {
        Color col = argbColor(c.fadeColor, c.fadeAlpha);
        DrawRectangleRec({-10, -10, scene::kW + 20, scene::kH + 20}, col);
    }
}

}  // namespace gh
