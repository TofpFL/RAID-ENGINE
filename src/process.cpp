#include "process.hpp"

#include <cstdlib>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#elif defined(__ANDROID__)
#include <sys/wait.h>  // no ffmpeg to run on Android (and posix_spawn needs API 28)
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace gh {

namespace fs = std::filesystem;

namespace {
fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
}  // namespace

int runProcess(const std::string& exe, const std::vector<std::string>& args) {
#if defined(_WIN32)
    std::wstring cmd = L"\"" + toPath(exe).wstring() + L"\"";
    for (const std::string& a : args) cmd += L" \"" + toPath(a).wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
#else
    std::string cmd = "'" + exe + "'";
    for (const std::string& a : args) {
        std::string q;
        for (char c : a) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        cmd += " '" + q + "'";
    }
    cmd += " >/dev/null 2>&1 </dev/null";
    const int rc = std::system(cmd.c_str());
    if (rc == -1) return -1;
#if defined(WEXITSTATUS)
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#else
    return rc;
#endif
#endif
}

// ------------------------------------------------------------ pipe process --

PipeProcess::~PipeProcess() { finish(); }

#if defined(_WIN32)

bool PipeProcess::start(const std::string& exe, const std::vector<std::string>& args, const std::string& logFile) {
    finish();
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 20)) return false;
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);  // only the child's end is inherited
    HANDLE log = CreateFileW(logFile.empty() ? L"NUL" : toPath(logFile).wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring cmd = L"\"" + toPath(exe).wstring() + L"\"";
    for (const std::string& a : args) cmd += L" \"" + toPath(a).wstring() + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = readEnd;
    si.hStdOutput = log != INVALID_HANDLE_VALUE ? log : nullptr;
    si.hStdError = si.hStdOutput;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(readEnd);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        CloseHandle(writeEnd);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    input_ = writeEnd;
    started_ = true;
    return true;
}

bool PipeProcess::write(const void* data, size_t bytes) {
    if (!input_) return false;
    const char* p = static_cast<const char*>(data);
    while (bytes > 0) {
        DWORD done = 0;
        const DWORD n = DWORD(bytes > (1u << 30) ? (1u << 30) : bytes);
        if (!WriteFile(static_cast<HANDLE>(input_), p, n, &done, nullptr) || done == 0) return false;
        p += done;
        bytes -= done;
    }
    return true;
}

int PipeProcess::finish() {
    if (!started_) return -1;
    if (input_) CloseHandle(static_cast<HANDLE>(input_));
    input_ = nullptr;
    WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    started_ = false;
    return int(code);
}

#elif defined(__ANDROID__)

bool PipeProcess::start(const std::string&, const std::vector<std::string>&, const std::string&) { return false; }
bool PipeProcess::write(const void*, size_t) { return false; }
int PipeProcess::finish() { return -1; }

#else

bool PipeProcess::start(const std::string& exe, const std::vector<std::string>& args, const std::string& logFile) {
    finish();
    std::signal(SIGPIPE, SIG_IGN);  // a program that stops reading must not end the game
    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    const std::string out = logFile.empty() ? std::string("/dev/null") : logFile;
    posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[0]);
    if (rc != 0) {
        close(fds[1]);
        return false;
    }
    pid_ = pid;
    input_ = fds[1];
    started_ = true;
    return true;
}

bool PipeProcess::write(const void* data, size_t bytes) {
    if (input_ < 0) return false;
    const char* p = static_cast<const char*>(data);
    while (bytes > 0) {
        const ssize_t n = ::write(input_, p, bytes);
        if (n <= 0) return false;
        p += n;
        bytes -= size_t(n);
    }
    return true;
}

int PipeProcess::finish() {
    if (!started_) return -1;
    if (input_ >= 0) close(input_);
    input_ = -1;
    int status = 0;
    waitpid(pid_, &status, 0);
    pid_ = -1;
    started_ = false;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

bool openWithSystem(const std::string& path) {
#if defined(_WIN32)
    const std::wstring w = toPath(path).wstring();
    return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__ANDROID__)
    (void)path;
    return false;
#else
#if defined(__APPLE__)
    const std::string opener = "open";
#else
    const std::string opener = "xdg-open";
#endif
    std::thread([opener, path] { runProcess(opener, {path}); }).detach();
    return true;
#endif
}

std::string findProgram(const std::string& baseDir, const std::string& name, const std::string& probeArg) {
#if defined(_WIN32)
    const std::string file = name + ".exe";
#else
    const std::string file = name;
#endif
    const std::string local = baseDir + file;
    std::error_code ec;
    if (fs::exists(toPath(local), ec) && runProcess(local, {probeArg}) == 0) return local;
    if (runProcess(file, {probeArg}) == 0) return file;
    return "";
}

}  // namespace gh
