// Ready-made screen effects for scripts: JS Engine's Lua shader functions
// (addChromaticAbberationEffect, addVCREffect, addWiggleEffect, ...) and the
// "Rainbow Eyesore" event. Each effect is a fragment shader of our own in the
// Flixel dialect the shader translator already handles, taking the same
// parameters as JS Engine's.
#pragma once

#include <string>
#include <string_view>

#include "scene.hpp"

namespace gh::fx {

// Source of a built-in effect ("" if there is none by that name).
std::string_view source(std::string_view name);

// Makes sure the scene can compile `name` (a built-in effect); false if unknown.
bool install(scene::Scene& sc, const std::string& name);

// A new instance of a built-in effect, its time uniform advancing on its own
// when `timed` (Scene::update adds the elapsed time to "uTime").
scene::ShaderPtr make(scene::Scene& sc, const std::string& name, bool timed);

// Wiggle styles by name, as JS Engine reads them: dreamy, wavy, horizontal
// (heat_wave_horizontal), vertical, flag, both.
int wiggleType(std::string_view name);

}  // namespace gh::fx
