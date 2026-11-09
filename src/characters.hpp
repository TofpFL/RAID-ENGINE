// Original characters, drawn entirely with vector shapes.
#pragma once

#include <string>

#include "raylib.h"

namespace gh {

enum class CharId { Herald, Principality, Hand, Demise, Shade };
CharId charFromName(const std::string& name);
const char* charDisplayName(CharId id);
Color charColor(CharId id);

struct CharacterState {
    CharId id = CharId::Herald;
    Color tint = WHITE;    // palette accent for the generic Shade
    int singDir = -1;      // 0 L, 1 D, 2 U, 3 R
    double singStart = -10, singUntil = -10;
    double missUntil = -10;
    double beatAt = -10;   // time of the last idle bop
    bool dead = false;

    void sing(int dir, double now, double holdSec);
    void miss(int dir, double now);
    void beat(double now);
};

// feet: ground anchor. faceLeft mirrors the body (the player faces left).
void drawCharacter(const CharacterState& s, Vector2 feet, float scale, double now, bool faceLeft);
void drawIcon(CharId id, Color tint, Vector2 center, float size, bool losing, double now);

}  // namespace gh
