#include "shader_glsl.hpp"

#include <cctype>
#include <vector>

namespace gh {

namespace {

constexpr const char* kPrelude = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 gh_FragColor;
uniform vec2 openfl_TextureSize;
uniform float gh_flipY;
uniform float gh_premul;
uniform bool hasTransform;
uniform bool hasColorTransform;
uniform bool openfl_HasColorTransform;
#define bitmap texture0
#define openfl_TextureCoordv vec2(fragTexCoord.x, mix(fragTexCoord.y, 1.0 - fragTexCoord.y, gh_flipY))
#define openfl_Alphav (fragColor.a)
#define openfl_ColorMultiplierv vec4(fragColor.rgb, 1.0)
#define openfl_ColorOffsetv vec4(0.0)
vec4 gh_tex(sampler2D s, vec2 uv) { vec4 c = texture(s, uv); return vec4(c.rgb * c.a, c.a); }
vec4 gh_texLod(sampler2D s, vec2 uv, float lod) { vec4 c = textureLod(s, uv, lod); return vec4(c.rgb * c.a, c.a); }
vec4 gh_texBitmap(vec2 uv) {
    vec4 c = texture(texture0, vec2(uv.x, mix(uv.y, 1.0 - uv.y, gh_flipY)));
    return gh_premul > 0.5 ? vec4(c.rgb * c.a, c.a) : c;
}
vec4 flixel_texture2D(sampler2D s, vec2 uv) {
    vec4 color = gh_texBitmap(uv);
    if (color.a == 0.0) return vec4(0.0);
    color = vec4(color.rgb / color.a, color.a) * vec4(fragColor.rgb, 1.0);
    return vec4(color.rgb * color.a * fragColor.a, color.a * fragColor.a);
}
#line 1
)GLSL";

bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Replaces whole-word occurrences of `from` (not inside other identifiers).
std::string replaceWord(const std::string& s, std::string_view from, std::string_view to) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, from.size(), from) == 0 && (i == 0 || !identChar(s[i - 1])) &&
            (i + from.size() >= s.size() || !identChar(s[i + from.size()]))) {
            out += to;
            i += from.size();
        } else {
            out += s[i++];
        }
    }
    return out;
}

std::string trimmed(std::string_view l) {
    size_t a = 0, b = l.size();
    while (a < b && std::isspace((unsigned char)l[a])) ++a;
    while (b > a && std::isspace((unsigned char)l[b - 1])) --b;
    return std::string(l.substr(a, b - a));
}

// "uniform sampler2D bitmap;" and friends are already in the prelude.
bool declaresBuiltin(const std::string& line) {
    std::string t = trimmed(line);
    for (const char* q : {"uniform ", "varying ", "attribute ", "in ", "out "}) {
        if (t.rfind(q, 0) != 0) continue;
        static const char* names[] = {"bitmap", "openfl_TextureCoordv", "openfl_TextureSize", "openfl_Alphav",
                                      "openfl_ColorMultiplierv", "openfl_ColorOffsetv", "openfl_HasColorTransform",
                                      "hasTransform", "hasColorTransform", "openfl_Position", "openfl_TextureCoord",
                                      "openfl_Alpha", "openfl_Matrix", "openfl_ColorMultiplier", "openfl_ColorOffset"};
        for (const char* n : names) {
            const size_t p = t.find(n);
            if (p == std::string::npos) continue;
            const size_t e = p + std::string_view(n).size();
            if ((p == 0 || !identChar(t[p - 1])) && (e >= t.size() || !identChar(t[e]))) return true;
        }
    }
    return false;
}

}  // namespace

std::string translateShader(std::string_view src) {
    // Line pass: drop directives and declarations the prelude provides.
    std::string body;
    size_t p = 0;
    while (p <= src.size()) {
        size_t e = src.find('\n', p);
        if (e == std::string_view::npos) e = src.size();
        std::string line(src.substr(p, e - p));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        p = e + 1;
        const std::string t = trimmed(line);
        if (t.rfind("#version", 0) == 0 || t.rfind("#extension", 0) == 0 || t == "#pragma header" ||
            t == "#pragma body" || t.rfind("#pragma ", 0) == 0 || declaresBuiltin(line)) {
            body += "\n";  // keep line numbers for error messages
            continue;
        }
        body += line + "\n";
        if (e == src.size()) break;
    }
    // GLSL ES 1.0 has no texture() builtin, so `texture` there is a user name.
    body = replaceWord(body, "texture", "gh_userTexture");
    // texture2D(bitmap, ...) samples the sprite: flip and alpha handling.
    {
        std::string out;
        size_t i = 0;
        while (i < body.size()) {
            if (body.compare(i, 9, "texture2D") == 0 && (i == 0 || !identChar(body[i - 1])) &&
                (i + 9 >= body.size() || !identChar(body[i + 9]))) {
                size_t k = i + 9;
                while (k < body.size() && std::isspace((unsigned char)body[k])) ++k;
                if (k < body.size() && body[k] == '(') {
                    size_t a = k + 1;
                    while (a < body.size() && std::isspace((unsigned char)body[a])) ++a;
                    if (body.compare(a, 6, "bitmap") == 0 && a + 6 < body.size() && !identChar(body[a + 6])) {
                        size_t c = a + 6;
                        while (c < body.size() && std::isspace((unsigned char)body[c])) ++c;
                        if (c < body.size() && body[c] == ',') {
                            out += "gh_texBitmap(";
                            i = c + 1;
                            continue;
                        }
                    }
                }
                out += "gh_tex";
                i += 9;
                continue;
            }
            out += body[i++];
        }
        body.swap(out);
    }
    body = replaceWord(body, "texture2DLodEXT", "gh_texLod");
    body = replaceWord(body, "texture2DLod", "gh_texLod");
    body = replaceWord(body, "gl_FragColor", "gh_FragColor");
    body = replaceWord(body, "varying", "");
    // gl_FragData[0] -> gh_FragColor
    for (size_t q; (q = body.find("gl_FragData[0]")) != std::string::npos;) body.replace(q, 14, "gh_FragColor");
    return std::string(kPrelude) + body;
}

}  // namespace gh
