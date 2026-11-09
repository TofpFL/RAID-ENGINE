#include "process.hpp"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
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
