#include "sysinfo.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2  // GetProcessMemoryInfo from kernel32 (Windows 7 and later)
#endif
#include <windows.h>
#include <psapi.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#elif defined(__ANDROID__)
#include <GLES2/gl2.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>
#else
#include <GL/gl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define GH_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace gh::sysinfo {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    s.erase(0, i);
    // "Intel(R) Core(TM) i7-8700  CPU @ 3.20GHz": runs of spaces become one
    std::string out;
    for (char c : s)
        if (!(c == ' ' && !out.empty() && out.back() == ' ')) out += c;
    return out;
}

// The processor's own name (x86: the CPUID brand string).
std::string cpuBrand() {
#ifdef GH_X86
    unsigned regs[12] = {};
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuid(info, int(0x80000000u));
    if (unsigned(info[0]) < 0x80000004u) return {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(info, int(0x80000002u + unsigned(i)));
        std::memcpy(regs + 4 * i, info, 16);
    }
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x80000004u) return {};
    for (unsigned i = 0; i < 3; ++i) {
        __get_cpuid(0x80000002u + i, &a, &b, &c, &d);
        regs[4 * i] = a;
        regs[4 * i + 1] = b;
        regs[4 * i + 2] = c;
        regs[4 * i + 3] = d;
    }
#endif
    char s[49] = {};
    std::memcpy(s, regs, 48);
    return trim(s);
#else
    return {};
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)
// "key : value" from /proc/cpuinfo, /proc/meminfo, /proc/self/status...
std::string procField(const char* file, const char* key) {
    std::ifstream f(file);
    std::string line;
    const size_t n = std::strlen(key);
    while (std::getline(f, line)) {
        if (line.compare(0, n, key) != 0) continue;
        const size_t colon = line.find(':', n);
        if (colon == std::string::npos) continue;
        if (line.find_first_not_of(" \t", n) != colon) continue;  // "model name" but not "model"
        return trim(line.substr(colon + 1));
    }
    return {};
}

uint64_t procKb(const char* file, const char* key) {
    const std::string v = procField(file, key);
    return v.empty() ? 0 : uint64_t(std::strtoull(v.c_str(), nullptr, 10)) * 1024u;
}
#endif

#if defined(__ANDROID__)
std::string prop(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return trim(v);
}
#endif

#if defined(__APPLE__)
std::string sysctlString(const char* name) {
    char buf[256] = {};
    size_t len = sizeof buf - 1;
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return {};
    return trim(std::string(buf, len));
}
#endif

std::string glString(unsigned name) {
    const GLubyte* s = glGetString(GLenum(name));
    return s ? trim(reinterpret_cast<const char*>(s)) : std::string();
}

Hardware query() {
    Hardware h;
    h.threads = int(std::thread::hardware_concurrency());
    h.cpu = cpuBrand();
    h.gpu = glString(GL_RENDERER);
    h.gl = glString(GL_VERSION);
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) h.totalRam = ms.ullTotalPhys;
    // RtlGetVersion tells the real version (GetVersionEx stops at 8).
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (FARPROC p = GetProcAddress(nt, "RtlGetVersion")) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(p));
            if (fn(&v) == 0) {
                // Windows 11 still says 10.0: its builds start at 22000.
                const std::string name = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000
                                             ? "11"
                                             : std::to_string(v.dwMajorVersion) + (v.dwMinorVersion ? "." + std::to_string(v.dwMinorVersion) : "");
                h.os = "Windows " + name + " (build " + std::to_string(v.dwBuildNumber) + ")";
            }
        }
    }
    if (h.os.empty()) h.os = "Windows";
    if (h.cpu.empty()) {  // ARM: the name Windows keeps for the first processor
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            char name[256] = {};
            DWORD size = sizeof name - 1, type = 0;
            if (RegQueryValueExA(key, "ProcessorNameString", nullptr, &type, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS &&
                type == REG_SZ)
                h.cpu = trim(name);
            RegCloseKey(key);
        }
    }
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) h.totalRam = mem;
    if (h.cpu.empty()) h.cpu = sysctlString("machdep.cpu.brand_string");
    const std::string ver = sysctlString("kern.osproductversion");
    h.os = ver.empty() ? "macOS" : "macOS " + ver;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) h.totalRam = uint64_t(pages) * uint64_t(page);
    if (h.cpu.empty()) {
        for (const char* key : {"model name", "Hardware", "Processor", "cpu model"}) {
            h.cpu = procField("/proc/cpuinfo", key);
            if (!h.cpu.empty()) break;
        }
    }
#if defined(__ANDROID__)
    if (h.cpu.empty()) {
        const std::string maker = prop("ro.soc.manufacturer"), model = prop("ro.soc.model");
        h.cpu = trim(maker + " " + model);
        if (h.cpu.empty()) h.cpu = prop("ro.board.platform");
    }
    const std::string release = prop("ro.build.version.release"), sdk = prop("ro.build.version.sdk"), device = prop("ro.product.model");
    h.os = "Android " + release + (sdk.empty() ? "" : " (API " + sdk + ")") + (device.empty() ? "" : ", " + device);
#else
    // "Ubuntu 22.04.3 LTS (Linux 6.5.0)"
    std::string pretty;
    {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                pretty = line.substr(12);
                if (pretty.size() >= 2 && pretty.front() == '"' && pretty.back() == '"') pretty = pretty.substr(1, pretty.size() - 2);
            }
    }
    utsname u{};
    std::string kernel;
    if (uname(&u) == 0) kernel = std::string(u.sysname) + " " + u.release;
    h.os = pretty.empty() ? kernel : (kernel.empty() ? pretty : pretty + " (" + kernel + ")");
#endif
#endif
    if (h.cpu.empty()) h.cpu = "unknown processor";
    return h;
}

}  // namespace

const Hardware& hardware() {
    static const Hardware h = query();
    return h;
}

uint64_t processMemory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return uint64_t(pmc.WorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return uint64_t(info.resident_size);
    return 0;
#else
    std::ifstream f("/proc/self/statm");
    unsigned long long size = 0, resident = 0;
    if (!(f >> size >> resident)) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    return uint64_t(resident) * uint64_t(page > 0 ? page : 4096);
#endif
}

uint64_t peakProcessMemory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return uint64_t(pmc.PeakWorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return uint64_t(info.resident_size_max);
    return 0;
#else
    return procKb("/proc/self/status", "VmHWM");
#endif
}

uint64_t systemMemoryUsed() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) return ms.ullTotalPhys - ms.ullAvailPhys;
    return 0;
#elif defined(__APPLE__)
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS) return 0;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    return (uint64_t(vm.active_count) + uint64_t(vm.wire_count) + uint64_t(vm.compressor_page_count)) * uint64_t(page);
#else
    const uint64_t total = procKb("/proc/meminfo", "MemTotal"), avail = procKb("/proc/meminfo", "MemAvailable");
    return total > avail ? total - avail : 0;
#endif
}

double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto secs = [](const FILETIME& t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7; };
    return secs(kernel) + secs(user);
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
    return double(r.ru_utime.tv_sec) + double(r.ru_utime.tv_usec) * 1e-6 + double(r.ru_stime.tv_sec) + double(r.ru_stime.tv_usec) * 1e-6;
#endif
}

std::string formatBytes(uint64_t bytes) {
    char buf[32];
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (mb < 1024.0) std::snprintf(buf, sizeof buf, "%.1f MB", mb);
    else std::snprintf(buf, sizeof buf, "%.2f GB", mb / 1024.0);
    return buf;
}

}  // namespace gh::sysinfo
