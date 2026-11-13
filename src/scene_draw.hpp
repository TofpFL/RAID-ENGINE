// Renders a scene::Scene with raylib, reproducing Flixel's sprite transform
// (trimmed atlas frames, origin, scale, rotation, flips, scroll factors) and
// FlxCamera zoom/rotation/shake, plus camera flashes and fades.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "raylib.h"
#include "scene.hpp"

namespace gh {

class SceneRenderer {
public:
    ~SceneRenderer();
    // Uploads every texture the scene references so far (call while loading).
    void preload(scene::Scene& sc);
    // Draws every object on camera `cam` in scene order. Markers call `marker`
    // (gfGroup, hud); built-in characters call `builtin`.
    void drawCamera(scene::Scene& sc, scene::Cam cam,
                    const std::function<void(const scene::Sprite& marker)>& marker,
                    const std::function<void(const scene::Character& c, Vector2 feet, float scale)>& builtin);
    void drawEffects(const scene::Camera& c);  // flash / fade overlays (screen space)
    // World point -> canvas point for a camera and scroll factor.
    static Vector2 toScreen(const scene::Camera& c, float x, float y, float sfx, float sfy);
    Texture2D* texture(const std::string& file);
    // Textures owned elsewhere (video frames), drawn for graphics whose file is `key`.
    void setDynamicTexture(const std::string& key, const Texture2D& t) { dynamic_[key] = t; }
    void removeDynamicTexture(const std::string& key) { dynamic_.erase(key); }
    void clear();
    // Draws `src` (a camera rendered to a texture) through the camera's
    // shaders into `dest` on the current target.
    void drawFiltered(scene::Scene& sc, const std::vector<scene::ShaderPtr>& filters, const RenderTexture2D& src,
                      Rectangle dest);
    std::vector<std::string> errors;  // shader problems, shown by the play state
    // Psych's Anti-Aliasing option: OFF draws every picture with sharp pixels.
    static inline bool smoothScaling = true;

private:
    struct CompiledShader {
        Shader sh{};
        bool ok = false;
        std::map<std::string, int> locs;
    };
    CompiledShader* shaderFor(const scene::Scene& sc, const std::string& name);
    int loc(CompiledShader& cs, const std::string& name);
    void applyUniforms(CompiledShader& cs, const scene::ShaderInstance& inst, float texW, float texH, bool renderTexture,
                       const scene::Scene& sc);
    std::map<std::string, CompiledShader> shaders_;
    RenderTexture2D pingPong_[2]{};
    scene::Scene* sc_ = nullptr;

    void drawSprite(const scene::Sprite& s, const scene::Camera& c);
    void drawText(scene::Sprite& s, const scene::Camera& c);
    Font* font(const std::string& file);
    std::map<std::string, Texture2D> tex_;
    std::map<std::string, Texture2D> dynamic_;
    std::map<std::string, int> filter_;
    std::map<std::string, Font> fonts_;
    int blend_ = 0;
};

}  // namespace gh
