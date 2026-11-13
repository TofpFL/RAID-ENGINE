// Running helper programs (ffmpeg, curl) without a console window.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gh {

// A program reading what this side writes to its standard input (ffmpeg
// receiving raw video frames). Its output goes to `logFile` ("" = nowhere).
class PipeProcess {
public:
    PipeProcess() = default;
    PipeProcess(const PipeProcess&) = delete;
    PipeProcess& operator=(const PipeProcess&) = delete;
    ~PipeProcess();
    bool start(const std::string& exe, const std::vector<std::string>& args, const std::string& logFile = "");
    bool running() const { return started_; }
    // False once the program is gone (it stopped reading).
    bool write(const void* data, size_t bytes);
    // Closes its input and waits for it to end: its exit code (-1 if it never started).
    int finish();

private:
    bool started_ = false;
#if defined(_WIN32)
    void* process_ = nullptr;
    void* input_ = nullptr;
#else
    int pid_ = -1;
    int input_ = -1;
#endif
};

// Runs exe with args, waits, returns its exit code (-1 if it could not start).
int runProcess(const std::string& exe, const std::vector<std::string>& args);
// Opens a file or folder with what the system uses for it (a text editor for
// .lua, the file manager for folders), without waiting. False where there is
// no such thing (Android).
bool openWithSystem(const std::string& path);
// exe next to the game or on the PATH, "" if neither runs with `probeArg`.
std::string findProgram(const std::string& baseDir, const std::string& name, const std::string& probeArg = "--version");

}  // namespace gh
