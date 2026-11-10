// The system's "choose a folder" dialog on desktops. Kept apart from the rest
// of the platform code: windows.h and raylib.h do not mix.
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#elif !defined(__ANDROID__)
#include <cstdio>
#endif

namespace gh::platform {

#if defined(_WIN32)
std::string pickFolderNative(const std::string& title) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring wt(title.size() + 1, L'\0');
    wt.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wt.data(), int(wt.size()))));
    BROWSEINFOW bi{};
    bi.lpszTitle = wt.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    std::string out;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH * 4] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                out.resize(size_t(n));
                WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), n, nullptr, nullptr);
                out.resize(size_t(n - 1));
            }
        }
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    for (char& c : out)
        if (c == '\\') c = '/';
    return out;
}
#elif defined(__ANDROID__)
std::string pickFolderNative(const std::string&) { return {}; }
#else
namespace {
std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
}  // namespace

std::string pickFolderNative(const std::string& title) {
    std::string safe;
    for (char c : title)
        if (c != '"' && c != '\'' && c != '\\' && c != '`' && c != '$') safe += c;
#if defined(__APPLE__)
    return run("osascript -e 'POSIX path of (choose folder with prompt \"" + safe + "\")' 2>/dev/null");
#else
    std::string out = run("zenity --file-selection --directory --title=\"" + safe + "\" 2>/dev/null");
    if (out.empty()) out = run("kdialog --getexistingdirectory . --title \"" + safe + "\" 2>/dev/null");
    return out;
#endif
}
#endif

}  // namespace gh::platform
