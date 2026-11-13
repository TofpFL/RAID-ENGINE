// What the detailed FPS counter shows besides the frame rate: the game's
// memory and processor use, the system's memory, and the hardware (processor,
// graphics card, OpenGL version, system). Windows, Linux, macOS and Android.
// Kept apart from raylib.h: the Windows and OpenGL headers clash with it.
#pragma once

#include <cstdint>
#include <string>

namespace gh::sysinfo {

struct Hardware {
    std::string cpu;        // "AMD Ryzen 7 5800X 8-Core Processor"
    std::string gpu;        // OpenGL's renderer: "NVIDIA GeForce RTX 3060/PCIe/SSE2"
    std::string gl;         // OpenGL's version: "3.3.0 NVIDIA 535.54"
    std::string os;         // "Windows 10.0 (build 19045)", "Linux 6.5.0", "macOS (Darwin 24.1.0)"
    int threads = 0;        // logical processors
    uint64_t totalRam = 0;  // bytes (0: unknown)
};

// Queried once; the graphics card's names need the window's OpenGL context,
// so the first call has to come from the thread that draws.
const Hardware& hardware();

uint64_t processMemory();      // bytes the game holds in RAM now (resident / working set; 0: unknown)
uint64_t peakProcessMemory();  // the most it has held (0: unknown)
uint64_t systemMemoryUsed();   // bytes in use on the whole system (0: unknown)
double processCpuSeconds();    // processor time the game has used, all its threads together

// "512.3 MB", "1.25 GB"
std::string formatBytes(uint64_t bytes);

}  // namespace gh::sysinfo
