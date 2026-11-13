// What differs between desktop and Android: where the game keeps its files,
// what to do when the app goes to the background, and how files are
// downloaded. On desktop most of these do nothing.
#pragma once

#include <string>
#include <vector>

namespace gh::platform {

constexpr bool kAndroid =
#if defined(__ANDROID__)
    true;
#else
    false;
#endif

// Touch-first device (no keyboard assumed): menus get the on-screen pad.
bool touchFirst();

// Android: every touch since the last call, in order, with the moment it
// happened (nowSec() clock), so a quick tap between two polls is not lost.
struct TouchEvent {
    enum Kind { Down, Move, Up, Cancel } kind;
    int id;
    float x, y;  // window pixels
    double t;
};
bool touchEvents();  // true where takeTouchEvents() is the source of touches
void takeTouchEvents(std::vector<TouchEvent>& out);  // appends

// Android log ("GodsHand" tag); nothing elsewhere.
void log(const std::string& s);

// Android: the app's storage folder (ends with '/'), "" on desktop.
std::string dataDir();
// Where settings, scores, script save data and replays live, apart from the
// game's own folder (which an update replaces): %APPDATA%/GodsHand/ on
// Windows, ~/Library/Application Support/GodsHand/ on macOS,
// $XDG_DATA_HOME/GodsHand/ or ~/.local/share/GodsHand/ on Linux, the app's
// folder on Android. Ends with '/'; "" when the system does not say.
std::string userDir();
// Android: copies the fonts and other files packed in the APK into base.
void prepareDataDir(const std::string& base);
// Android: keep the screen on and mute the game while it is in the background.
void afterWindowInit();
// Wait for the screen's refresh (on) or present frames as soon as they are
// ready (off). Applied at once; Android asks EGL for it.
void setVsync(bool on);
// The player picks a folder. Desktop: the system's dialog (blocking); its
// path comes back ("" if cancelled). Android: the system folder picker
// opens and the files the Psych Engine interface needs are copied into
// <data folder>/psych-ui/ in the background (follow importStatus); "" comes back.
std::string pickFolder(const std::string& title);
std::string pickFolderNative(const std::string& title);  // folder_picker.cpp
bool inBackground();
bool takeResumed();             // true once after the app comes back
void rememberVolume(float v);   // master volume to restore after the background
// Path shown to the player for a folder inside base (e.g. "Android/data/.../mods").
std::string displayPath(const std::string& path);

// HTTPS download to a file. Android only (through java.net); desktop uses curl.
bool download(const std::string& url, const std::string& dest, std::string& err);

// Android: the system file picker copies the chosen mod zips into mods/.
enum class Import { Idle, Picking, Copying, Done, Nothing };
bool pickModZips();               // false if the picker cannot be shown
Import importStatus(int* count);  // count: zips copied (when Done)
void resetImport();

}  // namespace gh::platform
