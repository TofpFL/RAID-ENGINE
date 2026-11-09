// Definitions for the original game's weeks 1-6, which older engines (base
// game, Kade) hard-code instead of shipping as files: character JSONs, stage
// JSONs and stage scripts written against the Psych Lua API. They are used
// only when a mod has the matching images but no definition of its own.
#pragma once

#include <string>
#include <string_view>

namespace gh::basegame {

// Psych-format character JSON, or nullptr for an unknown name.
const char* characterJson(std::string_view name);
// Psych-format stage JSON and a Lua script that builds the stage.
const char* stageJson(std::string_view stage);
const char* stageLua(std::string_view stage);
// An image the stage needs (relative to images/): its presence means the mod
// carries the original assets.
const char* stageProbeImage(std::string_view stage);
// The stage and girlfriend the original game picks for a song.
std::string stageForSong(std::string_view song);
std::string gfForStage(std::string_view stage);

}  // namespace gh::basegame
