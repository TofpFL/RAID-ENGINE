#include "sysinfo.hpp"

#include <algorithm>
#include <cctype>
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
#include <pdh.h>
#include <pdhmsg.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
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
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>

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


// ----------------------------------------------------------- graphics card --

namespace {

#if !defined(__ANDROID__)
// Video memory from the card's own OpenGL extensions: NVIDIA's
// GL_NVX_gpu_memory_info (total and free, KB) or AMD's GL_ATI_meminfo (free).
// Only NVIDIA's and AMD's drivers are asked: others (Mesa's software
// renderer among them) can answer with no error and made-up numbers. A card
// without the extensions answers GL_INVALID_ENUM, which is cleared again.
void glVideoMemory(GpuUse& g) {
    static const int vendor = [] {
        std::string v;
        for (GLenum e : {GLenum(GL_VENDOR), GLenum(GL_RENDERER)})
            if (const auto* t = reinterpret_cast<const char*>(glGetString(e))) v += std::string(t) + " ";
        for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (v.find("nvidia") != std::string::npos) return 1;
        if (v.find("amd") != std::string::npos || v.find("radeon") != std::string::npos || v.find("ati technologies") != std::string::npos)
            return 2;
        return 0;
    }();
    if (vendor == 0) return;
    while (glGetError() != GL_NO_ERROR) {
    }
    GLint dedicated = 0, total = 0, avail = -1;
    glGetIntegerv(0x9047, &dedicated);  // GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
    glGetIntegerv(0x9048, &total);      // GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
    glGetIntegerv(0x9049, &avail);      // GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
    if (glGetError() == GL_NO_ERROR && dedicated > 0 && total > 0 && avail >= 0 && avail <= total) {
        g.memTotal = uint64_t(total) * 1024u;
        g.memUsed = uint64_t(total - avail) * 1024u;
        return;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    GLint ati[4] = {0, 0, 0, 0};
    glGetIntegerv(0x87FC, ati);  // GL_TEXTURE_FREE_MEMORY_ATI
    if (glGetError() == GL_NO_ERROR && ati[0] > 0) g.memFree = uint64_t(ati[0]) * 1024u;
    while (glGetError() != GL_NO_ERROR) {
    }
}
#endif

#if defined(_WIN32)
// Windows' own GPU counters, as Task Manager reads them: each 3D engine's use
// added up over the processes (instances are "pid_<n>_luid_<card>_phys_<n>_eng_<n>_engtype_3D"),
// the busiest engine's; and the dedicated video memory in use.
struct PdhGpu {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER busy = nullptr, mem = nullptr;
    bool tried = false, ok = false;
};

// The counter's values by instance (false when there are none yet).
bool counterValues(PDH_HCOUNTER c, std::vector<std::pair<std::wstring, double>>& out) {
    out.clear();
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA) || size == 0)
        return false;
    std::vector<unsigned char> buf(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            out.push_back({items[i].szName ? items[i].szName : L"", items[i].FmtValue.doubleValue});
    return true;
}

bool windowsGpu(GpuUse& g) {
    static PdhGpu p;
    if (!p.tried) {
        p.tried = true;
        if (PdhOpenQueryW(nullptr, 0, &p.query) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(p.query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &p.busy) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(p.query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &p.mem) != ERROR_SUCCESS) p.mem = nullptr;
            p.ok = PdhCollectQueryData(p.query) == ERROR_SUCCESS;
        }
        return !p.ok;  // a use needs two readings
    }
    if (!p.ok || PdhCollectQueryData(p.query) != ERROR_SUCCESS) return true;
    std::vector<std::pair<std::wstring, double>> v;
    if (counterValues(p.busy, v)) {
        std::vector<std::pair<std::wstring, double>> engines;
        for (const auto& [name, use] : v) {
            const size_t at = name.find(L"luid_");
            const std::wstring engine = at == std::wstring::npos ? name : name.substr(at);
            auto e = std::find_if(engines.begin(), engines.end(), [&](const auto& x) { return x.first == engine; });
            if (e == engines.end()) engines.push_back({engine, use});
            else e->second += use;
        }
        double busiest = 0;
        for (const auto& e : engines) busiest = std::max(busiest, e.second);
        g.busy = std::clamp(busiest, 0.0, 100.0);
    }
    if (p.mem && counterValues(p.mem, v)) {
        double used = 0;
        for (const auto& x : v) used += x.second;
        if (used > 0) g.memUsed = uint64_t(used);
    }
    return true;
}
#elif defined(__APPLE__)
// macOS: the graphics driver's PerformanceStatistics in the I/O Registry.
long long cfNumber(CFDictionaryRef d, const char* key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = k ? CFDictionaryGetValue(d, k) : nullptr;
    if (k) CFRelease(k);
    long long out = -1;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &out);
    return out;
}

void macGpu(GpuUse& g) {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return;
    for (io_object_t svc; (svc = IOIteratorNext(it)) != 0;) {
        CFMutableDictionaryRef props = nullptr;
        if (IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
            CFTypeRef perf = CFDictionaryGetValue(props, CFSTR("PerformanceStatistics"));
            if (perf && CFGetTypeID(perf) == CFDictionaryGetTypeID()) {
                const auto* d = static_cast<CFDictionaryRef>(perf);
                const long long busy = cfNumber(d, "Device Utilization %");
                if (busy >= 0 && g.busy < 0) g.busy = double(std::min(100LL, busy));
                long long used = cfNumber(d, "vramUsedBytes");
                if (used <= 0) used = cfNumber(d, "In use system memory");
                if (used > 0 && g.memUsed == 0) g.memUsed = uint64_t(used);
            }
            CFRelease(props);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
}
#else
long long readNumber(const std::string& path) {
    std::ifstream f(path);
    long long v = -1;
    if (!(f >> v)) return -1;
    return v;
}

#if defined(__ANDROID__)
// Phones: Adreno (kgsl) and Mali say it here when the system lets apps read it.
void androidGpu(GpuUse& g) {
    for (const char* p : {"/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "/sys/kernel/gpu/gpu_busy", "/sys/class/misc/mali0/device/utilization"}) {
        const long long v = readNumber(p);
        if (v >= 0) {
            g.busy = double(std::min(100LL, v));
            return;
        }
    }
    std::ifstream f("/sys/class/kgsl/kgsl-3d0/gpubusy");
    long long busy = 0, total = 0;
    if (f >> busy >> total && total > 0) g.busy = 100.0 * double(busy) / double(total);
}
#else
// Linux: AMD's driver says how busy the card is and its memory in sysfs;
// NVIDIA's says it through its management library when it is installed.
void linuxGpu(GpuUse& g) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        const std::string dev = e.path().string() + "/device/";
        const long long busy = readNumber(dev + "gpu_busy_percent");
        if (busy < 0) continue;
        g.busy = double(std::min(100LL, busy));
        const long long used = readNumber(dev + "mem_info_vram_used"), total = readNumber(dev + "mem_info_vram_total");
        if (used > 0 && total > 0) {
            g.memUsed = uint64_t(used);
            g.memTotal = uint64_t(total);
        }
        return;
    }
    struct NvmlUtilization {
        unsigned gpu, memory;
    };
    struct NvmlMemory {
        unsigned long long total, free, used;
    };
    static void* lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    static void* device = nullptr;
    static bool tried = false;
    using InitFn = int (*)();
    using HandleFn = int (*)(unsigned, void**);
    using UtilFn = int (*)(void*, NvmlUtilization*);
    using MemFn = int (*)(void*, NvmlMemory*);
    if (!lib) return;
    if (!tried) {
        tried = true;
        auto init = reinterpret_cast<InitFn>(dlsym(lib, "nvmlInit_v2"));
        auto handle = reinterpret_cast<HandleFn>(dlsym(lib, "nvmlDeviceGetHandleByIndex_v2"));
        if (!init || !handle || init() != 0 || handle(0, &device) != 0) device = nullptr;
    }
    if (!device) return;
    if (auto util = reinterpret_cast<UtilFn>(dlsym(lib, "nvmlDeviceGetUtilizationRates"))) {
        NvmlUtilization u{};
        if (util(device, &u) == 0) g.busy = double(u.gpu);
    }
    if (auto mem = reinterpret_cast<MemFn>(dlsym(lib, "nvmlDeviceGetMemoryInfo"))) {
        NvmlMemory m{};
        if (mem(device, &m) == 0 && m.total > 0) {
            g.memTotal = m.total;
            g.memUsed = m.used;
        }
    }
}
#endif
#endif

// What the system says is read on a thread of its own, twice a second while
// the counter asks: Windows' GPU counters can take a good part of a frame.
struct GpuSampler {
    std::mutex m;
    GpuUse latest;
    std::atomic<long long> askedMs{0};
};

long long steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// False while a reading is not complete yet.
bool sampleSystem(GpuUse& g) {
#if defined(_WIN32)
    return windowsGpu(g);
#elif defined(__APPLE__)
    macGpu(g);
#elif defined(__ANDROID__)
    androidGpu(g);
#else
    linuxGpu(g);
#endif
    return true;
}

GpuSampler& sampler() {
    // Never freed: the thread runs until the game ends, past static destructors.
    static GpuSampler* s = [] {
        auto* p = new GpuSampler;
        p->askedMs = steadyMs();
        std::thread([p] {
            for (;;) {
                if (steadyMs() - p->askedMs.load() < 3000) {
                    GpuUse g;
                    g.ready = sampleSystem(g);
                    std::lock_guard<std::mutex> lock(p->m);
                    p->latest = g;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }).detach();
        return p;
    }();
    return *s;
}

}  // namespace

GpuUse gpuUse() {
    GpuSampler& s = sampler();
    s.askedMs = steadyMs();
    GpuUse g;
    {
        std::lock_guard<std::mutex> lock(s.m);
        g = s.latest;
    }
#if !defined(__ANDROID__)
    // The card's own count of its memory, when the system gave no total.
    if (!(g.memUsed && g.memTotal)) {
        GpuUse gl;
        glVideoMemory(gl);
        if (gl.memTotal) {
            g.memUsed = gl.memUsed;
            g.memTotal = gl.memTotal;
        } else if (!g.memUsed) {
            g.memFree = gl.memFree;
        }
    }
#endif
    return g;
}

std::string formatBytes(uint64_t bytes) {
    char buf[32];
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (mb < 1024.0) std::snprintf(buf, sizeof buf, "%.1f MB", mb);
    else std::snprintf(buf, sizeof buf, "%.2f GB", mb / 1024.0);
    return buf;
}

}  // namespace gh::sysinfo
