#include "basegame.hpp"

#include <cstring>

#include "common.hpp"

namespace gh::basegame {

namespace {

struct Named {
    const char* name;
    const char* text;
};

// Offsets and prefixes follow the original game's Character.hx (the same
// values Psych Engine ships in its character JSONs).
const Named kCharacters[] = {
    {"bf", R"({"image": "characters/BOYFRIEND", "position": [0, 350], "camera_position": [0, 0], "healthicon": "bf",
  "flip_x": true, "healthbar_colors": [49, 176, 209], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "BF idle dance", "fps": 24, "loop": false, "offsets": [-5, 0]},
  {"anim": "singLEFT", "name": "BF NOTE LEFT0", "fps": 24, "loop": false, "offsets": [12, -6]},
  {"anim": "singDOWN", "name": "BF NOTE DOWN0", "fps": 24, "loop": false, "offsets": [-10, -50]},
  {"anim": "singUP", "name": "BF NOTE UP0", "fps": 24, "loop": false, "offsets": [-29, 27]},
  {"anim": "singRIGHT", "name": "BF NOTE RIGHT0", "fps": 24, "loop": false, "offsets": [-38, -7]},
  {"anim": "singLEFTmiss", "name": "BF NOTE LEFT MISS", "fps": 24, "loop": false, "offsets": [12, 24]},
  {"anim": "singDOWNmiss", "name": "BF NOTE DOWN MISS", "fps": 24, "loop": false, "offsets": [-11, -19]},
  {"anim": "singUPmiss", "name": "BF NOTE UP MISS", "fps": 24, "loop": false, "offsets": [-29, 27]},
  {"anim": "singRIGHTmiss", "name": "BF NOTE RIGHT MISS", "fps": 24, "loop": false, "offsets": [-30, 21]},
  {"anim": "hey", "name": "BF HEY", "fps": 24, "loop": false, "offsets": [7, 4]},
  {"anim": "hurt", "name": "BF hit", "fps": 24, "loop": false, "offsets": [14, 18]},
  {"anim": "scared", "name": "BF idle shaking", "fps": 24, "loop": true, "offsets": [-4, 0]},
  {"anim": "dodge", "name": "boyfriend dodge", "fps": 24, "loop": false, "offsets": [-10, -16]},
  {"anim": "attack", "name": "boyfriend attack", "fps": 24, "loop": false, "offsets": [294, 267]},
  {"anim": "pre-attack", "name": "bf pre attack", "fps": 24, "loop": false, "offsets": [-40, -40]}]})"},
    {"bf-car", R"({"image": "characters/bfCar", "position": [0, 350], "camera_position": [0, 0], "healthicon": "bf",
  "flip_x": true, "healthbar_colors": [49, 176, 209], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "BF idle dance", "fps": 24, "loop": false, "offsets": [-5, 0]},
  {"anim": "singLEFT", "name": "BF NOTE LEFT0", "fps": 24, "loop": false, "offsets": [12, -6]},
  {"anim": "singDOWN", "name": "BF NOTE DOWN0", "fps": 24, "loop": false, "offsets": [-10, -50]},
  {"anim": "singUP", "name": "BF NOTE UP0", "fps": 24, "loop": false, "offsets": [-29, 27]},
  {"anim": "singRIGHT", "name": "BF NOTE RIGHT0", "fps": 24, "loop": false, "offsets": [-38, -7]},
  {"anim": "singLEFTmiss", "name": "BF NOTE LEFT MISS", "fps": 24, "loop": false, "offsets": [12, 24]},
  {"anim": "singDOWNmiss", "name": "BF NOTE DOWN MISS", "fps": 24, "loop": false, "offsets": [-11, -19]},
  {"anim": "singUPmiss", "name": "BF NOTE UP MISS", "fps": 24, "loop": false, "offsets": [-29, 27]},
  {"anim": "singRIGHTmiss", "name": "BF NOTE RIGHT MISS", "fps": 24, "loop": false, "offsets": [-30, 21]}]})"},
    {"bf-christmas", R"({"image": "characters/bfChristmas", "position": [0, 350], "camera_position": [0, 0], "healthicon": "bf",
  "flip_x": true, "healthbar_colors": [49, 176, 209], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "BF idle dance", "fps": 24, "loop": false, "offsets": [-5, 0]},
  {"anim": "singLEFT", "name": "BF NOTE LEFT0", "fps": 24, "loop": false, "offsets": [12, -6]},
  {"anim": "singDOWN", "name": "BF NOTE DOWN0", "fps": 24, "loop": false, "offsets": [-10, -50]},
  {"anim": "singUP", "name": "BF NOTE UP0", "fps": 24, "loop": false, "offsets": [-29, 27]},
  {"anim": "singRIGHT", "name": "BF NOTE RIGHT0", "fps": 24, "loop": false, "offsets": [-38, -7]},
  {"anim": "singLEFTmiss", "name": "BF NOTE LEFT MISS", "fps": 24, "loop": false, "offsets": [12, 24]},
  {"anim": "singDOWNmiss", "name": "BF NOTE DOWN MISS", "fps": 24, "loop": false, "offsets": [-11, -19]},
  {"anim": "singUPmiss", "name": "BF NOTE UP MISS", "fps": 24, "loop": false, "offsets": [-29, 27]},
  {"anim": "singRIGHTmiss", "name": "BF NOTE RIGHT MISS", "fps": 24, "loop": false, "offsets": [-30, 21]},
  {"anim": "hey", "name": "BF HEY", "fps": 24, "loop": false, "offsets": [7, 4]}]})"},
    {"bf-pixel", R"({"image": "characters/bfPixel", "position": [0, 350], "camera_position": [0, 0], "healthicon": "bf-pixel",
  "flip_x": true, "no_antialiasing": true, "healthbar_colors": [123, 214, 246], "sing_duration": 4, "scale": 6, "animations": [
  {"anim": "idle", "name": "BF IDLE", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "BF LEFT NOTE", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singDOWN", "name": "BF DOWN NOTE", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singUP", "name": "BF UP NOTE", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singRIGHT", "name": "BF RIGHT NOTE", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFTmiss", "name": "BF LEFT MISS", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singDOWNmiss", "name": "BF DOWN MISS", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singUPmiss", "name": "BF UP MISS", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singRIGHTmiss", "name": "BF RIGHT MISS", "fps": 24, "loop": false, "offsets": [0, 0]}]})"},
    {"gf", R"({"image": "characters/GF_assets", "position": [0, 0], "camera_position": [0, 0], "healthicon": "gf",
  "flip_x": false, "healthbar_colors": [165, 0, 77], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "cheer", "name": "GF Cheer", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "GF left note", "fps": 24, "loop": false, "offsets": [0, -19]},
  {"anim": "singDOWN", "name": "GF Down Note", "fps": 24, "loop": false, "offsets": [0, -20]},
  {"anim": "singUP", "name": "GF Up Note", "fps": 24, "loop": false, "offsets": [0, 4]},
  {"anim": "singRIGHT", "name": "GF Right Note", "fps": 24, "loop": false, "offsets": [0, -20]},
  {"anim": "sad", "name": "gf sad", "fps": 24, "loop": false, "offsets": [-2, -2], "indices": [0,1,2,3,4,5,6,7,8,9,10,11,12]},
  {"anim": "danceLeft", "name": "GF Dancing Beat", "fps": 24, "loop": false, "offsets": [0, -9], "indices": [30,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14]},
  {"anim": "danceRight", "name": "GF Dancing Beat", "fps": 24, "loop": false, "offsets": [0, -9], "indices": [15,16,17,18,19,20,21,22,23,24,25,26,27,28,29]},
  {"anim": "hairBlow", "name": "GF Dancing Beat Hair blowing", "fps": 24, "loop": true, "offsets": [45, -8], "indices": [0,1,2,3]},
  {"anim": "hairFall", "name": "GF Dancing Beat Hair Landing", "fps": 24, "loop": false, "offsets": [0, -9], "indices": [0,1,2,3,4,5,6,7,8,9,10,11]},
  {"anim": "scared", "name": "GF FEAR", "fps": 24, "loop": true, "offsets": [-2, -17]}]})"},
    {"gf-christmas", R"({"image": "characters/gfChristmas", "position": [0, 0], "camera_position": [0, 0], "healthicon": "gf",
  "flip_x": false, "healthbar_colors": [165, 0, 77], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "cheer", "name": "GF Cheer", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "GF left note", "fps": 24, "loop": false, "offsets": [0, -19]},
  {"anim": "singDOWN", "name": "GF Down Note", "fps": 24, "loop": false, "offsets": [0, -20]},
  {"anim": "singUP", "name": "GF Up Note", "fps": 24, "loop": false, "offsets": [0, 4]},
  {"anim": "singRIGHT", "name": "GF Right Note", "fps": 24, "loop": false, "offsets": [0, -20]},
  {"anim": "sad", "name": "gf sad", "fps": 24, "loop": false, "offsets": [-2, -21], "indices": [0,1,2,3,4,5,6,7,8,9,10,11,12]},
  {"anim": "danceLeft", "name": "GF Dancing Beat", "fps": 24, "loop": false, "offsets": [0, -9], "indices": [30,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14]},
  {"anim": "danceRight", "name": "GF Dancing Beat", "fps": 24, "loop": false, "offsets": [0, -9], "indices": [15,16,17,18,19,20,21,22,23,24,25,26,27,28,29]},
  {"anim": "hairBlow", "name": "GF Dancing Beat Hair blowing", "fps": 24, "loop": true, "offsets": [45, -8], "indices": [0,1,2,3]},
  {"anim": "hairFall", "name": "GF Dancing Beat Hair Landing", "fps": 24, "loop": false, "offsets": [0, -9], "indices": [0,1,2,3,4,5,6,7,8,9,10,11]},
  {"anim": "scared", "name": "GF FEAR", "fps": 24, "loop": true, "offsets": [-2, -17]}]})"},
    {"gf-car", R"({"image": "characters/gfCar", "position": [0, 0], "camera_position": [0, 0], "healthicon": "gf",
  "flip_x": false, "healthbar_colors": [165, 0, 77], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "singUP", "name": "GF Dancing Beat Hair blowing CAR", "fps": 24, "loop": false, "offsets": [0, 0], "indices": [0]},
  {"anim": "danceLeft", "name": "GF Dancing Beat Hair blowing CAR", "fps": 24, "loop": false, "offsets": [0, 0], "indices": [30,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14]},
  {"anim": "danceRight", "name": "GF Dancing Beat Hair blowing CAR", "fps": 24, "loop": false, "offsets": [0, 0], "indices": [15,16,17,18,19,20,21,22,23,24,25,26,27,28,29]}]})"},
    {"gf-pixel", R"({"image": "characters/gfPixel", "position": [0, 0], "camera_position": [0, 0], "healthicon": "gf",
  "flip_x": false, "no_antialiasing": true, "healthbar_colors": [165, 0, 77], "sing_duration": 4, "scale": 6, "animations": [
  {"anim": "singUP", "name": "GF IDLE", "fps": 24, "loop": false, "offsets": [0, 0], "indices": [2]},
  {"anim": "danceLeft", "name": "GF IDLE", "fps": 24, "loop": false, "offsets": [0, 0], "indices": [30,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14]},
  {"anim": "danceRight", "name": "GF IDLE", "fps": 24, "loop": false, "offsets": [0, 0], "indices": [15,16,17,18,19,20,21,22,23,24,25,26,27,28,29]}]})"},
    {"dad", R"({"image": "characters/DADDY_DEAREST", "position": [0, 0], "camera_position": [0, 0], "healthicon": "dad",
  "flip_x": false, "healthbar_colors": [175, 102, 206], "sing_duration": 6.1, "scale": 1, "animations": [
  {"anim": "idle", "name": "Dad idle dance", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Dad Sing Note LEFT", "fps": 24, "loop": false, "offsets": [-10, 10]},
  {"anim": "singDOWN", "name": "Dad Sing Note DOWN", "fps": 24, "loop": false, "offsets": [0, -30]},
  {"anim": "singUP", "name": "Dad Sing Note UP", "fps": 24, "loop": false, "offsets": [-6, 50]},
  {"anim": "singRIGHT", "name": "Dad Sing Note RIGHT", "fps": 24, "loop": false, "offsets": [0, 27]}]})"},
    {"spooky", R"({"image": "characters/spooky_kids_assets", "position": [0, 200], "camera_position": [0, 0], "healthicon": "spooky",
  "flip_x": false, "healthbar_colors": [213, 126, 0], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "danceLeft", "name": "spooky dance idle", "fps": 12, "loop": false, "offsets": [0, 0], "indices": [0,2,6]},
  {"anim": "danceRight", "name": "spooky dance idle", "fps": 12, "loop": false, "offsets": [0, 0], "indices": [8,10,12,14]},
  {"anim": "singLEFT", "name": "note sing left", "fps": 24, "loop": false, "offsets": [130, -10]},
  {"anim": "singDOWN", "name": "spooky DOWN note", "fps": 24, "loop": false, "offsets": [-50, -130]},
  {"anim": "singUP", "name": "spooky UP NOTE", "fps": 24, "loop": false, "offsets": [-20, 26]},
  {"anim": "singRIGHT", "name": "spooky sing right", "fps": 24, "loop": false, "offsets": [-130, -14]}]})"},
    {"pico", R"({"image": "characters/Pico_FNF_assetss", "position": [0, 300], "camera_position": [0, 0], "healthicon": "pico",
  "flip_x": true, "healthbar_colors": [183, 216, 85], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "Pico Idle Dance", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Pico Note Right0", "fps": 24, "loop": false, "offsets": [65, 9]},
  {"anim": "singDOWN", "name": "Pico Down Note0", "fps": 24, "loop": false, "offsets": [200, -70]},
  {"anim": "singUP", "name": "pico Up note0", "fps": 24, "loop": false, "offsets": [-29, 27]},
  {"anim": "singRIGHT", "name": "Pico NOTE LEFT0", "fps": 24, "loop": false, "offsets": [-68, -7]},
  {"anim": "singLEFTmiss", "name": "Pico Note Right Miss", "fps": 24, "loop": false, "offsets": [62, 64]},
  {"anim": "singDOWNmiss", "name": "Pico Down Note MISS", "fps": 24, "loop": false, "offsets": [210, -28]},
  {"anim": "singUPmiss", "name": "pico Up note miss", "fps": 24, "loop": false, "offsets": [-19, 67]},
  {"anim": "singRIGHTmiss", "name": "Pico NOTE LEFT miss", "fps": 24, "loop": false, "offsets": [-60, 41]}]})"},
    {"mom", R"({"image": "characters/Mom_Assets", "position": [0, 0], "camera_position": [0, 0], "healthicon": "mom",
  "flip_x": false, "healthbar_colors": [216, 85, 142], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "Mom Idle", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Mom Left Pose", "fps": 24, "loop": false, "offsets": [250, -23]},
  {"anim": "singDOWN", "name": "MOM DOWN POSE", "fps": 24, "loop": false, "offsets": [20, -160]},
  {"anim": "singUP", "name": "Mom Up Pose", "fps": 24, "loop": false, "offsets": [14, 71]},
  {"anim": "singRIGHT", "name": "Mom Pose Left", "fps": 24, "loop": false, "offsets": [10, -60]}]})"},
    {"mom-car", R"({"image": "characters/momCar", "position": [0, 0], "camera_position": [0, 0], "healthicon": "mom",
  "flip_x": false, "healthbar_colors": [216, 85, 142], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "Mom Idle", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Mom Left Pose", "fps": 24, "loop": false, "offsets": [250, -23]},
  {"anim": "singDOWN", "name": "MOM DOWN POSE", "fps": 24, "loop": false, "offsets": [20, -160]},
  {"anim": "singUP", "name": "Mom Up Pose", "fps": 24, "loop": false, "offsets": [14, 71]},
  {"anim": "singRIGHT", "name": "Mom Pose Left", "fps": 24, "loop": false, "offsets": [10, -60]}]})"},
    {"monster", R"({"image": "characters/Monster_Assets", "position": [0, 100], "camera_position": [0, 0], "healthicon": "monster",
  "flip_x": false, "healthbar_colors": [243, 255, 110], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "monster idle", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Monster left note", "fps": 24, "loop": false, "offsets": [-30, 0]},
  {"anim": "singDOWN", "name": "monster down", "fps": 24, "loop": false, "offsets": [-30, -40]},
  {"anim": "singUP", "name": "monster up note", "fps": 24, "loop": false, "offsets": [-20, 50]},
  {"anim": "singRIGHT", "name": "Monster Right note", "fps": 24, "loop": false, "offsets": [-51, 0]}]})"},
    {"monster-christmas", R"({"image": "characters/monsterChristmas", "position": [0, 130], "camera_position": [0, 0], "healthicon": "monster",
  "flip_x": false, "healthbar_colors": [243, 255, 110], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "monster idle", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Monster left note", "fps": 24, "loop": false, "offsets": [-30, 0]},
  {"anim": "singDOWN", "name": "monster down", "fps": 24, "loop": false, "offsets": [-40, -94]},
  {"anim": "singUP", "name": "monster up note", "fps": 24, "loop": false, "offsets": [-20, 50]},
  {"anim": "singRIGHT", "name": "Monster Right note", "fps": 24, "loop": false, "offsets": [-51, 0]}]})"},
    {"parents-christmas", R"({"image": "characters/mom_dad_christmas_assets", "position": [-500, 0], "camera_position": [0, 0],
  "healthicon": "parents", "flip_x": false, "healthbar_colors": [196, 94, 174], "sing_duration": 4, "scale": 1, "animations": [
  {"anim": "idle", "name": "Parent Christmas Idle", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Parent Left Note Dad", "fps": 24, "loop": false, "offsets": [-30, 16]},
  {"anim": "singDOWN", "name": "Parent Down Note Dad", "fps": 24, "loop": false, "offsets": [-31, -29]},
  {"anim": "singUP", "name": "Parent Up Note Dad", "fps": 24, "loop": false, "offsets": [-47, 24]},
  {"anim": "singRIGHT", "name": "Parent Right Note Dad", "fps": 24, "loop": false, "offsets": [-1, -23]},
  {"anim": "singLEFT-alt", "name": "Parent Left Note Mom", "fps": 24, "loop": false, "offsets": [-30, 15]},
  {"anim": "singDOWN-alt", "name": "Parent Down Note Mom", "fps": 24, "loop": false, "offsets": [-30, -27]},
  {"anim": "singUP-alt", "name": "Parent Up Note Mom", "fps": 24, "loop": false, "offsets": [-47, 24]},
  {"anim": "singRIGHT-alt", "name": "Parent Right Note Mom", "fps": 24, "loop": false, "offsets": [-1, -24]}]})"},
    {"senpai", R"({"image": "characters/senpai", "position": [150, 360], "camera_position": [0, 0], "healthicon": "senpai",
  "flip_x": false, "no_antialiasing": true, "healthbar_colors": [255, 170, 111], "sing_duration": 4, "scale": 6, "animations": [
  {"anim": "idle", "name": "Senpai Idle", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "SENPAI LEFT NOTE", "fps": 24, "loop": false, "offsets": [40, 0]},
  {"anim": "singDOWN", "name": "SENPAI DOWN NOTE", "fps": 24, "loop": false, "offsets": [14, 0]},
  {"anim": "singUP", "name": "SENPAI UP NOTE", "fps": 24, "loop": false, "offsets": [5, 37]},
  {"anim": "singRIGHT", "name": "SENPAI RIGHT NOTE", "fps": 24, "loop": false, "offsets": [0, 0]}]})"},
    {"senpai-angry", R"({"image": "characters/senpai", "position": [150, 360], "camera_position": [0, 0], "healthicon": "senpai",
  "flip_x": false, "no_antialiasing": true, "healthbar_colors": [255, 170, 111], "sing_duration": 4, "scale": 6, "animations": [
  {"anim": "idle", "name": "Angry Senpai Idle", "fps": 24, "loop": false, "offsets": [0, 0]},
  {"anim": "singLEFT", "name": "Angry Senpai LEFT NOTE", "fps": 24, "loop": false, "offsets": [40, 0]},
  {"anim": "singDOWN", "name": "Angry Senpai DOWN NOTE", "fps": 24, "loop": false, "offsets": [14, 0]},
  {"anim": "singUP", "name": "Angry Senpai UP NOTE", "fps": 24, "loop": false, "offsets": [5, 37]},
  {"anim": "singRIGHT", "name": "Angry Senpai RIGHT NOTE", "fps": 24, "loop": false, "offsets": [0, 0]}]})"},
    {"spirit", R"({"image": "characters/spirit", "position": [-150, 100], "camera_position": [0, 0], "healthicon": "spirit",
  "flip_x": false, "no_antialiasing": true, "healthbar_colors": [255, 60, 110], "sing_duration": 4, "scale": 6, "animations": [
  {"anim": "idle", "name": "idle spirit_", "fps": 24, "loop": false, "offsets": [-220, -280]},
  {"anim": "singLEFT", "name": "left_", "fps": 24, "loop": false, "offsets": [-200, -280]},
  {"anim": "singDOWN", "name": "spirit down_", "fps": 24, "loop": false, "offsets": [170, 110]},
  {"anim": "singUP", "name": "up_", "fps": 24, "loop": false, "offsets": [-220, -240]},
  {"anim": "singRIGHT", "name": "right_", "fps": 24, "loop": false, "offsets": [-220, -280]}]})"},
};

// Positions and zooms from the original PlayState (as in Psych's stage JSONs).
const Named kStageJson[] = {
    {"stage", R"({"defaultZoom": 0.9, "isPixelStage": false, "boyfriend": [770, 100], "girlfriend": [400, 130], "opponent": [100, 100],
  "hide_girlfriend": false, "camera_boyfriend": [0, 0], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
    {"spooky", R"({"defaultZoom": 1.05, "isPixelStage": false, "boyfriend": [770, 100], "girlfriend": [400, 130], "opponent": [100, 100],
  "hide_girlfriend": false, "camera_boyfriend": [0, 0], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
    {"philly", R"({"defaultZoom": 1.05, "isPixelStage": false, "boyfriend": [770, 100], "girlfriend": [400, 130], "opponent": [100, 100],
  "hide_girlfriend": false, "camera_boyfriend": [0, 0], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
    {"limo", R"({"defaultZoom": 0.9, "isPixelStage": false, "boyfriend": [1030, -120], "girlfriend": [400, 130], "opponent": [100, 100],
  "hide_girlfriend": false, "camera_boyfriend": [-200, 0], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
    {"mall", R"({"defaultZoom": 0.8, "isPixelStage": false, "boyfriend": [970, 100], "girlfriend": [400, 130], "opponent": [100, 100],
  "hide_girlfriend": false, "camera_boyfriend": [0, -100], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
    {"mallEvil", R"({"defaultZoom": 1.05, "isPixelStage": false, "boyfriend": [1090, 100], "girlfriend": [400, 130], "opponent": [100, 20],
  "hide_girlfriend": false, "camera_boyfriend": [0, 0], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
    {"school", R"({"defaultZoom": 1.05, "isPixelStage": true, "boyfriend": [970, 320], "girlfriend": [580, 430], "opponent": [100, 100],
  "hide_girlfriend": false, "camera_boyfriend": [-100, -100], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
    {"schoolEvil", R"({"defaultZoom": 1.05, "isPixelStage": true, "boyfriend": [970, 320], "girlfriend": [580, 430], "opponent": [100, 100],
  "hide_girlfriend": false, "camera_boyfriend": [-100, -100], "camera_opponent": [0, 0], "camera_girlfriend": [0, 0], "camera_speed": 1})"},
};

const Named kStageLua[] = {
    {"stage", R"LUA(
function onCreate()
    makeLuaSprite('stageback', 'stageback', -600, -200)
    setScrollFactor('stageback', 0.9, 0.9)
    addLuaSprite('stageback', false)
    makeLuaSprite('stagefront', 'stagefront', -650, 600)
    setScrollFactor('stagefront', 0.9, 0.9)
    scaleObject('stagefront', 1.1, 1.1)
    addLuaSprite('stagefront', false)
    makeLuaSprite('stagecurtains', 'stagecurtains', -500, -300)
    setScrollFactor('stagecurtains', 1.3, 1.3)
    scaleObject('stagecurtains', 0.9, 0.9)
    addLuaSprite('stagecurtains', false)
end
)LUA"},
    {"spooky", R"LUA(
local lightningStrikeBeat = 0
local lightningOffset = 8
function onCreate()
    makeAnimatedLuaSprite('halloweenBG', 'halloween_bg', -200, -100)
    addAnimationByPrefix('halloweenBG', 'idle', 'halloweem bg0', 24, false)
    addAnimationByPrefix('halloweenBG', 'lightning', 'halloweem bg lightning strike', 24, false)
    playAnim('halloweenBG', 'idle', true)
    addLuaSprite('halloweenBG', false)
end
function onBeatHit()
    if getRandomBool(10) and curBeat > lightningStrikeBeat + lightningOffset then
        playAnim('halloweenBG', 'lightning', true)
        playSound('thunder_' .. getRandomInt(1, 2))
        lightningStrikeBeat = curBeat
        lightningOffset = getRandomInt(8, 24)
        characterPlayAnim('boyfriend', 'scared', true)
        characterPlayAnim('gf', 'scared', true)
    end
end
)LUA"},
    {"philly", R"LUA(
local curLight = 0
local trainMoving = false
local trainFrameTiming = 0
local trainCars = 8
local trainFinishing = false
local trainCooldown = 0
local startedMoving = false
function onCreate()
    makeLuaSprite('sky', 'philly/sky', -100, 0)
    setScrollFactor('sky', 0.1, 0.1)
    addLuaSprite('sky', false)
    makeLuaSprite('city', 'philly/city', -10, 0)
    setScrollFactor('city', 0.3, 0.3)
    scaleObject('city', 0.85, 0.85)
    addLuaSprite('city', false)
    for i = 0, 4 do
        makeLuaSprite('win' .. i, 'philly/win' .. i, -10, 0)
        setScrollFactor('win' .. i, 0.3, 0.3)
        scaleObject('win' .. i, 0.85, 0.85)
        setProperty('win' .. i .. '.visible', false)
        addLuaSprite('win' .. i, false)
    end
    makeLuaSprite('streetBehind', 'philly/behindTrain', -40, 50)
    addLuaSprite('streetBehind', false)
    makeLuaSprite('phillyTrain', 'philly/train', 2000, 360)
    addLuaSprite('phillyTrain', false)
    makeLuaSprite('street', 'philly/street', -40, 50)
    addLuaSprite('street', false)
end
local function trainReset()
    setProperty('phillyTrain.x', 1280 + 200)
    trainMoving = false
    trainCars = 8
    trainFinishing = false
    startedMoving = false
    characterPlayAnim('gf', 'hairFall', true)
end
function onUpdate(elapsed)
    if trainMoving then
        trainFrameTiming = trainFrameTiming + elapsed
        if trainFrameTiming >= 1 / 24 then
            if not startedMoving then
                startedMoving = true
                characterPlayAnim('gf', 'hairBlow', false)
            end
            if startedMoving then
                setProperty('phillyTrain.x', getProperty('phillyTrain.x') - 400)
                if getProperty('phillyTrain.x') < -2000 and not trainFinishing then
                    setProperty('phillyTrain.x', -1150)
                    trainCars = trainCars - 1
                    if trainCars <= 0 then trainFinishing = true end
                end
                if getProperty('phillyTrain.x') < -4000 and trainFinishing then trainReset() end
            end
            trainFrameTiming = 0
        end
    end
end
function onBeatHit()
    if not trainMoving then trainCooldown = trainCooldown + 1 end
    if curBeat % 4 == 0 then
        setProperty('win' .. curLight .. '.visible', false)
        curLight = getRandomInt(0, 4)
        setProperty('win' .. curLight .. '.visible', true)
    end
    if curBeat % 8 == 4 and getRandomBool(30) and not trainMoving and trainCooldown > 8 then
        trainCooldown = getRandomInt(-4, 0)
        trainMoving = true
        playSound('train_passes', 1, 'train')
    end
end
)LUA"},
    {"limo", R"LUA(
local danced = false
local carCanDrive = true
function onCreate()
    makeLuaSprite('skyBG', 'limo/limoSunset', -120, -50)
    setScrollFactor('skyBG', 0.1, 0.1)
    addLuaSprite('skyBG', false)
    makeAnimatedLuaSprite('bgLimo', 'limo/bgLimo', -200, 480)
    addAnimationByPrefix('bgLimo', 'drive', 'background limo pink', 24, true)
    setScrollFactor('bgLimo', 0.4, 0.4)
    addLuaSprite('bgLimo', false)
    for i = 0, 4 do
        local tag = 'limoDancer' .. i
        makeAnimatedLuaSprite(tag, 'limo/limoDancer', (370 * i) + 130, 80)
        addAnimationByIndices(tag, 'danceLeft', 'bg dancer sketch PINK', '0,1,2,3,4,5,6,7,8,9,10,11,12,13,14', 24)
        addAnimationByIndices(tag, 'danceRight', 'bg dancer sketch PINK', '15,16,17,18,19,20,21,22,23,24,25,26,27,28,29', 24)
        playAnim(tag, 'danceLeft', true)
        setScrollFactor(tag, 0.4, 0.4)
        addLuaSprite(tag, false)
    end
    makeLuaSprite('fastCar', 'limo/fastCarLol', -300, 160)
    addLuaSprite('fastCar', false)
    makeAnimatedLuaSprite('limo', 'limo/limoDrive', -120, 550)
    addAnimationByPrefix('limo', 'drive', 'Limo stage', 24, true)
    addLuaSprite('limo', true)
end
function onCreatePost()
    -- The car sits between girlfriend and the singers.
    setObjectOrder('limo', getObjectOrder('dadGroup'))
end
local function resetFastCar()
    setProperty('fastCar.x', -12600)
    setProperty('fastCar.y', getRandomInt(140, 250))
    carCanDrive = true
end
function onBeatHit()
    danced = not danced
    for i = 0, 4 do playAnim('limoDancer' .. i, danced and 'danceRight' or 'danceLeft', true) end
    if getRandomBool(10) and carCanDrive then
        playSound('carPass' .. getRandomInt(0, 1), 0.7)
        setProperty('fastCar.x', -12600)
        setProperty('fastCar.y', getRandomInt(140, 250))
        doTweenX('fastCarDrive', 'fastCar', 2000, 1.2, 'linear')
        carCanDrive = false
        runTimer('fastCarReset', 2)
    end
end
function onTimerCompleted(tag)
    if tag == 'fastCarReset' then resetFastCar() end
end
)LUA"},
    {"mall", R"LUA(
function onCreate()
    makeLuaSprite('bgWalls', 'christmas/bgWalls', -1000, -500)
    setScrollFactor('bgWalls', 0.2, 0.2)
    scaleObject('bgWalls', 0.8, 0.8)
    addLuaSprite('bgWalls', false)
    makeAnimatedLuaSprite('upperBoppers', 'christmas/upperBop', -240, -90)
    addAnimationByPrefix('upperBoppers', 'bop', 'Upper Crowd Bob', 24, false)
    setScrollFactor('upperBoppers', 0.33, 0.33)
    scaleObject('upperBoppers', 0.85, 0.85)
    addLuaSprite('upperBoppers', false)
    makeLuaSprite('bgEscalator', 'christmas/bgEscalator', -1100, -600)
    setScrollFactor('bgEscalator', 0.3, 0.3)
    scaleObject('bgEscalator', 0.9, 0.9)
    addLuaSprite('bgEscalator', false)
    makeLuaSprite('tree', 'christmas/christmasTree', 370, -250)
    setScrollFactor('tree', 0.4, 0.4)
    addLuaSprite('tree', false)
    makeAnimatedLuaSprite('bottomBoppers', 'christmas/bottomBop', -300, 140)
    addAnimationByPrefix('bottomBoppers', 'bop', 'Bottom Level Boppers', 24, false)
    setScrollFactor('bottomBoppers', 0.9, 0.9)
    addLuaSprite('bottomBoppers', false)
    makeLuaSprite('fgSnow', 'christmas/fgSnow', -600, 700)
    addLuaSprite('fgSnow', false)
    makeAnimatedLuaSprite('santa', 'christmas/santa', -840, 150)
    addAnimationByPrefix('santa', 'idle', 'santa idle in fear', 24, false)
    addLuaSprite('santa', false)
end
function onBeatHit()
    playAnim('upperBoppers', 'bop', true)
    playAnim('bottomBoppers', 'bop', true)
    playAnim('santa', 'idle', true)
end
)LUA"},
    {"mallEvil", R"LUA(
function onCreate()
    makeLuaSprite('evilBG', 'christmas/evilBG', -400, -500)
    setScrollFactor('evilBG', 0.2, 0.2)
    scaleObject('evilBG', 0.8, 0.8)
    addLuaSprite('evilBG', false)
    makeLuaSprite('evilTree', 'christmas/evilTree', 300, -300)
    setScrollFactor('evilTree', 0.2, 0.2)
    addLuaSprite('evilTree', false)
    makeLuaSprite('evilSnow', 'christmas/evilSnow', -200, 700)
    addLuaSprite('evilSnow', false)
end
)LUA"},
    {"school", R"LUA(
local danced = false
local function pixel(tag, widthMult)
    setGraphicSize(tag, math.floor(getProperty(tag .. '.width') * widthMult))
    updateHitbox(tag)
    setProperty(tag .. '.antialiasing', false)
end
function onCreate()
    local rep = -200
    makeLuaSprite('bgSky', 'weeb/weebSky', 0, 0)
    setScrollFactor('bgSky', 0.1, 0.1)
    addLuaSprite('bgSky', false)
    makeLuaSprite('bgSchool', 'weeb/weebSchool', rep, 0)
    setScrollFactor('bgSchool', 0.6, 0.9)
    addLuaSprite('bgSchool', false)
    makeLuaSprite('bgStreet', 'weeb/weebStreet', rep, 0)
    setScrollFactor('bgStreet', 0.95, 0.95)
    addLuaSprite('bgStreet', false)
    makeLuaSprite('fgTrees', 'weeb/weebTreesBack', rep + 170, 130)
    setScrollFactor('fgTrees', 0.9, 0.9)
    addLuaSprite('fgTrees', false)
    makeAnimatedLuaSprite('bgTrees', 'weeb/weebTrees', rep - 380, -800)
    addAnimation('bgTrees', 'treeLoop', {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18}, 12, true)
    setScrollFactor('bgTrees', 0.85, 0.85)
    addLuaSprite('bgTrees', false)
    makeAnimatedLuaSprite('treeLeaves', 'weeb/petals', rep, -40)
    addAnimationByPrefix('treeLeaves', 'leaves', 'PETALS ALL', 24, true)
    setScrollFactor('treeLeaves', 0.85, 0.85)
    addLuaSprite('treeLeaves', false)
    pixel('bgSky', 6); pixel('bgSchool', 6); pixel('bgStreet', 6)
    pixel('bgTrees', 6 * 1.4); pixel('fgTrees', 6 * 0.8); pixel('treeLeaves', 6)
    makeAnimatedLuaSprite('bgGirls', 'weeb/bgFreaks', -100, 190)
    if songName and (string.lower(songName) == 'roses') then
        addAnimationByIndices('bgGirls', 'danceLeft', 'BG fangirls dissuaded', '0,1,2,3,4,5,6,7,8,9,10,11,12,13', 24)
        addAnimationByIndices('bgGirls', 'danceRight', 'BG fangirls dissuaded', '15,16,17,18,19,20,21,22,23,24,25,26,27,28,29', 24)
    else
        addAnimationByIndices('bgGirls', 'danceLeft', 'BG girls group', '0,1,2,3,4,5,6,7,8,9,10,11,12,13', 24)
        addAnimationByIndices('bgGirls', 'danceRight', 'BG girls group', '15,16,17,18,19,20,21,22,23,24,25,26,27,28,29', 24)
    end
    playAnim('bgGirls', 'danceLeft', true)
    setScrollFactor('bgGirls', 0.9, 0.9)
    pixel('bgGirls', 6)
    addLuaSprite('bgGirls', false)
end
function onBeatHit()
    danced = not danced
    playAnim('bgGirls', danced and 'danceRight' or 'danceLeft', true)
end
)LUA"},
    {"schoolEvil", R"LUA(
function onCreate()
    makeAnimatedLuaSprite('evilSchool', 'weeb/animatedEvilSchool', 400, 200)
    addAnimationByPrefix('evilSchool', 'idle', 'background 2', 24, true)
    setScrollFactor('evilSchool', 0.8, 0.9)
    scaleObject('evilSchool', 6, 6, false)
    setProperty('evilSchool.antialiasing', false)
    addLuaSprite('evilSchool', false)
end
)LUA"},
};

const Named kProbe[] = {
    {"stage", "stageback"}, {"spooky", "halloween_bg"}, {"philly", "philly/sky"}, {"limo", "limo/limoSunset"},
    {"mall", "christmas/bgWalls"}, {"mallEvil", "christmas/evilBG"}, {"school", "weeb/weebSky"},
    {"schoolEvil", "weeb/animatedEvilSchool"},
};

template <size_t N>
const char* lookup(const Named (&table)[N], std::string_view name) {
    for (const Named& n : table)
        if (equalsI(n.name, name)) return n.text;
    return nullptr;
}

}  // namespace

const char* characterJson(std::string_view name) { return lookup(kCharacters, name); }
const char* stageJson(std::string_view stage) { return lookup(kStageJson, stage); }
const char* stageLua(std::string_view stage) { return lookup(kStageLua, stage); }
const char* stageProbeImage(std::string_view stage) { return lookup(kProbe, stage); }

std::string stageForSong(std::string_view song) {
    const std::string s = songPathName(song);
    if (s == "spookeez" || s == "south" || s == "monster") return "spooky";
    if (s == "pico" || s == "philly" || s == "philly-nice" || s == "blammed") return "philly";
    if (s == "satin-panties" || s == "high" || s == "milf") return "limo";
    if (s == "cocoa" || s == "eggnog") return "mall";
    if (s == "winter-horrorland") return "mallEvil";
    if (s == "senpai" || s == "roses") return "school";
    if (s == "thorns") return "schoolEvil";
    return "stage";
}

std::string gfForStage(std::string_view stage) {
    const std::string s = toLower(stage);
    if (s == "limo") return "gf-car";
    if (s == "mall" || s == "mallevil") return "gf-christmas";
    if (s == "school" || s == "schoolevil") return "gf-pixel";
    return "gf";
}

}  // namespace gh::basegame
