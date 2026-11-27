#pragma once

#include "Common.h"
#include <array>
#include <deque>

enum class ProfilerSampleCategory : uint8_t {
    Frame = 0,
    Engine,
    Script,
    Physics,
    Animation,
    AI,
    Render,
    RenderDetail,
    GC,
    UI,
    Audio,
    Asset,
    Other
};

struct ProfilerSampleRecord {
    std::string name;
    ProfilerSampleCategory category = ProfilerSampleCategory::Other;
    int parentIndex = -1;
    int depth = 0;
    uint32_t callCount = 1;
    double cpuMs = 0.0;
    double gpuMs = 0.0;
    int64_t memoryDeltaBytes = 0;
    uint64_t memoryTotalBytes = 0;
    bool synthetic = false;
    // Heap allocations made on this thread inside the scope (MODULARITY_PROFILE_ALLOCS builds).
    uint64_t allocCount = 0;
};

struct ProfilerFrameRecord {
    uint64_t frameId = 0;
    double cpuMs = 0.0;
    // Main-thread CPU time actually burned this frame (CLOCK_THREAD_CPUTIME_ID). Unlike cpuMs,
    // which is wall time, this excludes time blocked in swap/vsync/sleep, so on a GPU-bound or
    // paced frame the gap between the two is the wait. 0 where the platform has no cheap clock.
    double threadCpuMs = 0.0;
    // Wall time spent inside the known wait scopes (Swap Buffers / Frame Pacing / Idle Throttle).
    double waitMs = 0.0;
    uint64_t allocCount = 0;
    double gpuMs = 0.0;
    double gcMs = 0.0;
    double renderMs = 0.0;
    double uiMs = 0.0;
    uint64_t drawCalls = 0;
    uint64_t textureBinds = 0;
    uint64_t stateBinds = 0;
    uint64_t sprite2DQuads = 0;
    uint64_t sprite2DBatches = 0;
    uint64_t postFxPasses = 0;
    uint64_t viewportRedraws = 0;
    uint64_t skippedRedraws = 0;
    uint64_t cachedLayerReuses = 0;
    uint64_t uiDirScans = 0;
    uint64_t managedHeapUsedBytes = 0;
    uint64_t managedHeapReservedBytes = 0;
    int64_t managedHeapDeltaBytes = 0;
    std::array<uint32_t, 3> gcCollections = { 0, 0, 0 };
    std::array<uint32_t, 3> gcCollectionDelta = { 0, 0, 0 };
    uint64_t renderMemoryBytes = 0;
    uint64_t renderMemoryBudgetBytes = 0;
    bool gpuTimingSupported = false;
    bool gpuTimingPartial = false;
    bool gcTriggered = false;
    std::vector<ProfilerSampleRecord> samples;
};

class Profiler {
public:
    static constexpr size_t kMaxHistoryFrames = 240;

    static Profiler& instance();

    void setRecording(bool enabled);
    bool isRecording() const { return recording; }

    void clearHistory();

    void beginFrame(uint64_t frameId);
    void endFrame();

    bool beginSample(std::string_view name, ProfilerSampleCategory category);
    void endSample();

    void addSyntheticSample(std::string_view name,
                            ProfilerSampleCategory category,
                            double cpuMs,
                            uint32_t callCount = 1,
                            int parentIndex = 0,
                            int64_t memoryDeltaBytes = 0,
                            uint64_t memoryTotalBytes = 0);

    void setCurrentSampleMemory(int64_t deltaBytes, uint64_t totalBytes);
    void setCurrentFrameGcMetrics(double gcMs,
                                  int64_t heapDeltaBytes,
                                  uint64_t heapUsedBytes,
                                  uint64_t heapReservedBytes,
                                  const std::array<uint32_t, 3>& collectionCounts,
                                  const std::array<uint32_t, 3>& collectionDelta);
    void setCurrentFrameRenderCounters(uint64_t drawCalls, uint64_t textureBinds, uint64_t stateBinds);
    void setCurrentFrame2DCounters(uint64_t spriteQuads,
                                   uint64_t spriteBatches,
                                   uint64_t postFxPasses,
                                   uint64_t viewportRedraws,
                                   uint64_t skippedRedraws,
                                   uint64_t cachedLayerReuses,
                                   uint64_t uiDirScans);
    void setCurrentFrameRenderMemory(uint64_t usedBytes, uint64_t budgetBytes);
    void setCurrentFrameGpuCapability(bool supported, bool partial);

    bool beginOpenGlGpuFrame(bool wholeFrame = false);
    void endOpenGlGpuFrame();

    size_t getHistoryCount() const;
    const ProfilerFrameRecord* getHistoryFrame(size_t orderedIndex) const;
    const ProfilerFrameRecord* getFrameById(uint64_t frameId) const;
    uint64_t getLatestFrameId() const;

    // Heap allocations made by the calling thread so far. Always 0 unless the build defines
    // MODULARITY_PROFILE_ALLOCS=1 (CMake -DMODULARITY_PROFILE_ALLOCS=ON), which replaces the
    // global operator new with a counting one.
    static uint64_t threadAllocationCount();

private:
    Profiler() = default;

    struct ActiveSample {
        int sampleIndex = -1;
        std::chrono::steady_clock::time_point startTime{};
        uint64_t startAllocCount = 0;
    };

    struct PendingGpuQuery {
        GLuint query = 0;
        uint64_t frameId = 0;
        bool partial = false;
    };

    bool supportsOpenGlGpuTimers() const;
    void resolvePendingGpuQueries();
    GLuint acquireGpuQuery();
    void releaseGpuQuery(GLuint query);
    ProfilerFrameRecord* findFrameMutable(uint64_t frameId);
    void attachGpuTimeToFrame(uint64_t frameId, double gpuMs, bool partial);
    void noteGpuTimeForDump(double gpuMs);

    bool recording = true;
    bool frameActive = false;
    ProfilerFrameRecord currentFrame;
    std::vector<ActiveSample> sampleStack;
    std::array<ProfilerFrameRecord, kMaxHistoryFrames> historyFrames;
    size_t historyWriteIndex = 0;
    size_t historyCount = 0;
    uint64_t latestFrameId = 0;
    double frameStartThreadCpuMs = 0.0;
    uint64_t frameStartAllocCount = 0;

    std::vector<GLuint> gpuQueryPool;
    std::deque<PendingGpuQuery> pendingGpuQueries;
    GLuint activeGpuQuery = 0;
    uint64_t activeGpuQueryFrameId = 0;
    bool activeGpuQueryPartial = false;
};

class ProfilerScope {
public:
    ProfilerScope(std::string_view name, ProfilerSampleCategory category)
        : active(Profiler::instance().beginSample(name, category)) {}

    ~ProfilerScope() {
        if (active) {
            Profiler::instance().endSample();
        }
    }

    ProfilerScope(const ProfilerScope&) = delete;
    ProfilerScope& operator=(const ProfilerScope&) = delete;

private:
    bool active = false;
};

#define MODU_PROFILE_SCOPE(name, category) \
    ProfilerScope moduProfilerScope_##__LINE__((name), (category))
