#include "effects.hpp"

#include <algorithm>
#include <cctype>

namespace gh::fx {

namespace {

// Colours from flixel_texture2D() are premultiplied by alpha; effects that
// change colours divide it out first and put it back after.
constexpr const char* kChromatic = R"GLSL(#pragma header
uniform float rOffset;
uniform float gOffset;
uniform float bOffset;
void main() {
    vec2 uv = openfl_TextureCoordv;
    vec4 r = flixel_texture2D(bitmap, vec2(uv.x + rOffset, uv.y));
    vec4 g = flixel_texture2D(bitmap, vec2(uv.x + gOffset, uv.y));
    vec4 b = flixel_texture2D(bitmap, vec2(uv.x + bOffset, uv.y));
    gl_FragColor = vec4(r.r, g.g, b.b, max(max(r.a, g.a), b.a));
}
)GLSL";

// Every other line of the 720-line picture goes dark.
constexpr const char* kScanline = R"GLSL(#pragma header
uniform float lockAlpha;
void main() {
    vec2 uv = openfl_TextureCoordv;
    vec4 c = flixel_texture2D(bitmap, uv);
    if (mod(floor(uv.y * 720.0), 2.0) < 1.0) c = vec4(0.0, 0.0, 0.0, lockAlpha > 0.5 ? c.a : 1.0);
    gl_FragColor = c;
}
)GLSL";

constexpr const char* kGrain = R"GLSL(#pragma header
uniform float uTime;
uniform float grainSize;
uniform float lumAmount;
uniform float lockAlpha;
float gh_hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float gh_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = gh_hash(i);
    float b = gh_hash(i + vec2(1.0, 0.0));
    float c = gh_hash(i + vec2(0.0, 1.0));
    float d = gh_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
void main() {
    vec2 uv = openfl_TextureCoordv;
    vec4 c = flixel_texture2D(bitmap, uv);
    vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    float frame = floor(uTime * 24.0);
    vec2 p = uv * vec2(1280.0, 720.0) / max(grainSize, 0.5) + vec2(frame * 17.0, frame * 31.0);
    float g = (gh_noise(p) - 0.5) * 0.2;
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    g *= mix(1.0, 1.0 - lum, clamp(lumAmount, 0.0, 1.0));
    rgb = clamp(rgb + vec3(g), 0.0, 1.0);
    float a = lockAlpha > 0.5 ? c.a : max(c.a, abs(g) * 2.0);
    gl_FragColor = vec4(rgb * a, a);
}
)GLSL";

// Sharp along a horizontal band at `center` (0 top .. 1 bottom), blurrier away from it.
constexpr const char* kTiltshift = R"GLSL(#pragma header
uniform float bluramount;
uniform float center;
void main() {
    vec2 uv = openfl_TextureCoordv;
    float r = clamp(abs(uv.y - center) * bluramount * 0.015, 0.0, 0.05);
    vec4 sum = flixel_texture2D(bitmap, uv) * 2.0;
    float w = 2.0;
    for (int i = 0; i < 12; i++) {
        float ang = float(i) * 0.5235988;
        vec2 o = vec2(cos(ang) * 0.5625, sin(ang)) * r;
        sum += flixel_texture2D(bitmap, uv + o);
        sum += flixel_texture2D(bitmap, uv + o * 0.5);
        w += 2.0;
    }
    gl_FragColor = sum / w;
}
)GLSL";

// Old tape: bent screen, wobbling lines, colour fringes, a rolling bar,
// scanlines, vignette and noise. glitchModifier makes the tears stronger.
constexpr const char* kVcr = R"GLSL(#pragma header
uniform float uTime;
uniform float glitchModifier;
uniform float distortionOn;
uniform float perspectiveOn;
uniform float vignetteMoving;
float gh_rand(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main() {
    vec2 uv = openfl_TextureCoordv;
    if (perspectiveOn > 0.5) {
        vec2 cc = uv * 2.0 - 1.0;
        cc *= 1.0 + 0.06 * dot(cc.yx, cc.yx);
        uv = cc * 0.5 + 0.5;
    }
    vec2 suv = uv;
    if (distortionOn > 0.5) {
        float line = floor(uv.y * 240.0);
        suv.x += (gh_rand(vec2(line, floor(uTime * 30.0))) - 0.5) * 0.004;
        float tear = step(0.985, gh_rand(vec2(floor(uv.y * 24.0), floor(uTime * 8.0))));
        suv.x += tear * (0.02 + 0.06 * glitchModifier) * sin(uTime * 40.0 + uv.y * 30.0);
        suv.x += sin(uv.y * 8.0 + uTime * 2.0) * 0.0015;
    }
    suv.x += (gh_rand(vec2(floor(uTime * 12.0), floor(uv.y * 60.0))) - 0.5) * 0.01 * glitchModifier;
    vec4 mid = flixel_texture2D(bitmap, suv);
    vec4 c = vec4(flixel_texture2D(bitmap, suv + vec2(0.0015, 0.0)).r, mid.g,
                  flixel_texture2D(bitmap, suv - vec2(0.0015, 0.0)).b, mid.a);
    float roll = smoothstep(0.0, 0.08, abs(fract(uv.y * 0.5 - uTime * 0.12) - 0.5));
    c.rgb *= 0.92 + 0.08 * roll;
    c.rgb *= 0.9 + 0.1 * sin(uv.y * 720.0 * 3.14159);
    vec2 vv = uv - 0.5;
    float vig = 1.0 - dot(vv, vv) * (vignetteMoving > 0.5 ? 1.2 + 0.25 * sin(uTime * 1.7) : 1.2);
    c.rgb *= clamp(vig, 0.0, 1.0);
    c.rgb += (gh_rand(uv * 800.0 + vec2(uTime)) - 0.5) * 0.06 * c.a;
    c.rgb = clamp(c.rgb, 0.0, c.a);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) c = vec4(0.0, 0.0, 0.0, 1.0);
    gl_FragColor = c;
}
)GLSL";

// effectType: 0 dreamy (rows slide sideways), 1 wavy (columns bob), 2 heat
// haze across, 3 heat haze up and down, 4 flag, 5 both hazes.
constexpr const char* kWiggle = R"GLSL(#pragma header
uniform float uTime;
uniform float uSpeed;
uniform float uFrequency;
uniform float uWaveAmplitude;
uniform float effectType;
uniform float verticalStrength;
uniform float horizontalStrength;
void main() {
    vec2 pt = openfl_TextureCoordv;
    float w = uTime * uSpeed;
    float t = effectType;
    float h = uWaveAmplitude * horizontalStrength;
    float v = uWaveAmplitude * verticalStrength;
    if (t < 0.5) {
        pt.x += sin(pt.y * uFrequency + 10.0 + w) * h;
    } else if (t < 1.5) {
        pt.y += sin(pt.x * uFrequency + 10.0 + w) * v;
    } else if (t < 2.5) {
        pt.x += sin(pt.x * uFrequency + w) * h;
    } else if (t < 3.5) {
        pt.y += sin(pt.y * uFrequency + w) * v;
    } else if (t < 4.5) {
        float ny = pt.y + sin(pt.y * uFrequency + 10.0 + pt.x * uFrequency + w) * v;
        pt.x += sin(pt.x * uFrequency + 5.0 + pt.y * uFrequency + w) * h;
        pt.y = ny;
    } else {
        pt.x += sin(pt.x * uFrequency + w) * h;
        pt.y += sin(pt.y * uFrequency + w) * v;
    }
    gl_FragColor = flixel_texture2D(bitmap, pt);
}
)GLSL";

// A waving warp that grows towards the bottom right (Dave and Bambi style).
constexpr const char* kGlitch = R"GLSL(#pragma header
uniform float uTime;
uniform float uSpeed;
uniform float uFrequency;
uniform float uWaveAmplitude;
void main() {
    vec2 pt = openfl_TextureCoordv;
    float ox = sin(pt.y * uFrequency + uTime * uSpeed) * uWaveAmplitude * (0.5 + pt.y);
    float oy = sin(pt.x * uFrequency - uTime * uSpeed) * uWaveAmplitude * (0.5 + pt.x);
    gl_FragColor = flixel_texture2D(bitmap, pt + vec2(ox, oy));
}
)GLSL";

// A slow rippling background.
constexpr const char* kDistort = R"GLSL(#pragma header
uniform float uTime;
uniform float uSpeed;
uniform float uFrequency;
uniform float uWaveAmplitude;
void main() {
    vec2 pt = openfl_TextureCoordv;
    pt.x += sin(pt.x * uFrequency + uTime * uSpeed) * uWaveAmplitude * pt.y;
    pt.y += sin(pt.y * uFrequency * 0.7 - uTime * uSpeed) * uWaveAmplitude;
    gl_FragColor = flixel_texture2D(bitmap, pt);
}
)GLSL";

// Psychedelic colour warp, off unless uEnabled (as in JS Engine, where only
// the Rainbow Eyesore event turns it on).
constexpr const char* kPulse = R"GLSL(#pragma header
uniform float uTime;
uniform float uSpeed;
uniform float uFrequency;
uniform float uWaveAmplitude;
uniform float uEnabled;
void main() {
    vec4 c = flixel_texture2D(bitmap, openfl_TextureCoordv);
    if (uEnabled > 0.5 && uWaveAmplitude > 0.0 && c.a > 0.0) {
        vec3 p = c.rgb / c.a;
        float k = clamp(uWaveAmplitude * uWaveAmplitude, 0.0, 1.0);
        vec3 w = 0.5 + 0.5 * sin(vec3(p.y, p.z, p.x) * uFrequency * 3.0 + uTime * uSpeed * vec3(1.0, 0.7, 1.3) +
                                 vec3(0.0, 2.1, 4.2) + openfl_TextureCoordv.y * 6.0);
        p = mix(p, w, k);
        c = vec4(p * c.a, c.a);
    }
    gl_FragColor = c;
}
)GLSL";

constexpr const char* kInvert = R"GLSL(#pragma header
void main() {
    vec4 c = flixel_texture2D(bitmap, openfl_TextureCoordv);
    vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    gl_FragColor = vec4((1.0 - rgb) * c.a, c.a);
}
)GLSL";

constexpr const char* kGreyscale = R"GLSL(#pragma header
void main() {
    vec4 c = flixel_texture2D(bitmap, openfl_TextureCoordv);
    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    gl_FragColor = vec4(l, l, l, c.a);
}
)GLSL";

// The picture as a card turned in 3D (radians) and seen in perspective.
constexpr const char* k3D = R"GLSL(#pragma header
uniform float xrot;
uniform float yrot;
uniform float zrot;
uniform float dept;
void main() {
    vec2 uv = openfl_TextureCoordv;
    float aspect = openfl_TextureSize.x / max(openfl_TextureSize.y, 1.0);
    float f = 1.0 + dept;
    vec3 d = vec3((uv.x - 0.5) * aspect, uv.y - 0.5, f);
    float sx = sin(xrot), cx = cos(xrot), sy = sin(yrot), cy = cos(yrot), sz = sin(zrot), cz = cos(zrot);
    mat3 rx = mat3(1.0, 0.0, 0.0, 0.0, cx, sx, 0.0, -sx, cx);
    mat3 ry = mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy);
    mat3 rz = mat3(cz, sz, 0.0, -sz, cz, 0.0, 0.0, 0.0, 1.0);
    mat3 r = rz * ry * rx;
    vec3 n = r * vec3(0.0, 0.0, 1.0);
    vec3 c0 = vec3(0.0, 0.0, f);
    float den = dot(d, n);
    vec4 outc = vec4(0.0);
    if (abs(den) > 0.0001) {
        float t = dot(c0, n) / den;
        vec3 hit = d * t - c0;
        vec2 q = vec2(dot(hit, r * vec3(1.0, 0.0, 0.0)) / aspect + 0.5, dot(hit, r * vec3(0.0, 1.0, 0.0)) + 0.5);
        if (t > 0.0 && q.x >= 0.0 && q.x <= 1.0 && q.y >= 0.0 && q.y <= 1.0) outc = flixel_texture2D(bitmap, q);
    }
    gl_FragColor = outc;
}
)GLSL";

// Glow: the picture plus a soft cross blur of itself.
constexpr const char* kBloom = R"GLSL(#pragma header
uniform float blurSize;
uniform float intensity;
void main() {
    vec2 uv = openfl_TextureCoordv;
    vec4 base = flixel_texture2D(bitmap, uv);
    vec4 sum = vec4(0.0);
    for (int i = -4; i <= 4; i++) {
        float fi = float(i);
        float wgt = exp(-fi * fi / 8.0) * 0.1995;
        sum += flixel_texture2D(bitmap, vec2(uv.x + fi * blurSize, uv.y)) * wgt;
        sum += flixel_texture2D(bitmap, vec2(uv.x, uv.y + fi * blurSize)) * wgt;
    }
    vec4 o = base + sum * intensity;
    o.a = min(o.a, 1.0);
    o.rgb = min(o.rgb, vec3(o.a));
    gl_FragColor = o;
}
)GLSL";

// Random 16-pixel blocks jump aside for a moment, with colour fringes.
constexpr const char* kBlocked = R"GLSL(#pragma header
uniform float uTime;
uniform vec2 screenSize;
uniform float glitchColor;
float gh_rand(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main() {
    vec2 uv = openfl_TextureCoordv;
    vec2 block = floor(uv * max(screenSize, vec2(1.0)) / 16.0);
    float st = floor(uTime * 10.0);
    float r = gh_rand(block / 64.0 + vec2(st * 0.123, st * 0.371));
    vec2 shift = vec2(0.0);
    if (r > 0.92) shift = (vec2(gh_rand(block + vec2(st)), gh_rand(block - vec2(st))) - 0.5) * 0.06;
    vec4 c = flixel_texture2D(bitmap, uv + shift);
    if (glitchColor > 0.5 && r > 0.92) {
        vec4 c2 = flixel_texture2D(bitmap, uv + shift * 1.5 + vec2(0.01, 0.0));
        c = vec4(c2.r, c.g, min(c.b * 1.2, c.a), max(c.a, c2.a));
    }
    gl_FragColor = c;
}
)GLSL";

struct Builtin {
    const char* name;
    const char* frag;
};
const Builtin kBuiltins[] = {
    {"__fx_chromatic", kChromatic}, {"__fx_scanline", kScanline}, {"__fx_grain", kGrain},
    {"__fx_tiltshift", kTiltshift}, {"__fx_vcr", kVcr},          {"__fx_wiggle", kWiggle},
    {"__fx_glitch", kGlitch},       {"__fx_distort", kDistort},  {"__fx_pulse", kPulse},
    {"__fx_invert", kInvert},       {"__fx_greyscale", kGreyscale}, {"__fx_3d", k3D},
    {"__fx_bloom", kBloom},         {"__fx_blocked", kBlocked},
};

}  // namespace

std::string_view source(std::string_view name) {
    for (const Builtin& b : kBuiltins)
        if (name == b.name) return b.frag;
    return {};
}

bool install(scene::Scene& sc, const std::string& name) {
    if (sc.shaderSources.count(name)) return true;
    const std::string_view src = source(name);
    if (src.empty()) return false;
    sc.shaderSources[name].frag = std::string(src);
    return true;
}

scene::ShaderPtr make(scene::Scene& sc, const std::string& name, bool timed) {
    if (!install(sc, name)) return nullptr;
    auto inst = std::make_shared<scene::ShaderInstance>();
    inst->name = name;
    if (timed) {
        inst->timeRate = 1.0f;
        inst->floats["uTime"] = {0.0f};
    }
    return inst;
}

int wiggleType(std::string_view name) {
    std::string n;
    for (char c : name)
        if (c != '_' && c != '-' && !std::isspace((unsigned char)c)) n += char(std::tolower((unsigned char)c));
    if (n == "wavy") return 1;
    if (n == "horizontal" || n == "heatwavehorizontal") return 2;
    if (n == "vertical" || n == "heatwavevertical") return 3;
    if (n == "flag") return 4;
    if (n == "both" || n == "heatwaveboth") return 5;
    return 0;  // dreamy
}

}  // namespace gh::fx
