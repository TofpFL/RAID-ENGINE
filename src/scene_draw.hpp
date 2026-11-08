// Renders a scene::Scene with raylib, reproducing Flixel's sprite transform
// (trimmed atlas frames, origin, scale, rotation, flips, scroll factors) and
// FlxCamera zoom/rotation/shake, plus camera flashes and fades.
#pragma once

#include <functional>
#include <map>
#include <string>

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
    void clear();

private:
    void drawSprite(const scene::Sprite& s, const scene::Camera& c);
    void drawText(scene::Sprite& s, const scene::Camera& c);
    Font* font(const std::string& file);
    std::map<std::string, Texture2D> tex_;
    std::map<std::string, int> filter_;
    std::map<std::string, Font> fonts_;
    int blend_ = 0;
};

}  // namespace gh
