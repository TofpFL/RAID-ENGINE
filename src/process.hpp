// Running helper programs (ffmpeg, curl) without a console window.
#pragma once

#include <string>
#include <vector>

namespace gh {

// Runs exe with args, waits, returns its exit code (-1 if it could not start).
int runProcess(const std::string& exe, const std::vector<std::string>& args);
// exe next to the game or on the PATH, "" if neither runs with `probeArg`.
std::string findProgram(const std::string& baseDir, const std::string& name, const std::string& probeArg = "--version");

}  // namespace gh
