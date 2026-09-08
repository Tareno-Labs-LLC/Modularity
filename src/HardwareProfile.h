#pragma once

#include <string>

namespace Modularity {
namespace HardwareProfile {

enum class Tier {
    Low = 0,
    Balanced = 1,
    High = 2
};

const char* ToString(Tier tier);
Tier FromString(const std::string& value);

struct StaticInfo {
    std::string glVendor;
    std::string glRenderer;
    std::string glVersion;
    int   glMajor = 0;
    int   glMinor = 0;
    bool  computeShaders = false;
    int   maxTextureImageUnits = 0;
    int   maxTextureSize = 0;
    bool  softwareRasterizer = false;
    unsigned cpuThreads = 0;
    unsigned long long systemMemoryMB = 0;

    std::string summary() const;
};

const StaticInfo& Static();

bool HasGL(int major, int minor);

void InvalidateStatic();

bool LooksLowEnd();

std::string LowEndReason();

struct BenchmarkResult {
    bool  valid = false;

    float fullscreenPassMs = 0.0f;
    float blurPassMs = 0.0f;
    float drawCallBatchMs = 0.0f;

    float fullscreenPassBudget60 = 0.0f;

    Tier  tier = Tier::High;
    std::string summary() const;
};

BenchmarkResult RunBenchmark();

const BenchmarkResult& LastBenchmark();

void SetCachedBenchmark(const BenchmarkResult& result);

// Total physical RAM in MB, or 0 if it could not be determined. Unlike Static() this touches no GL
// state, so it is safe to call before a context exists.
unsigned long long SystemMemoryMB();

// Multiplier for the hardcoded [ModuTimer] millisecond budgets. Those literals were chosen on a fast
// desktop; on a slow machine ordinary work crosses them and the log stops distinguishing "this frame
// was anomalous" from "this computer is slow". Uses the measured benchmark tier when one exists and
// falls back to a CPU-thread heuristic before then.
double TimingBudgetScale();

inline double TimingBudget(double fastMachineMs) { return fastMachineMs * TimingBudgetScale(); }

} // namespace HardwareProfile
} // namespace Modularity
