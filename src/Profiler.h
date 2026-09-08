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
};

struct ProfilerFrameRecord {
    uint64_t frameId = 0;
    double cpuMs = 0.0;
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

private:
    Profiler() = default;

    struct ActiveSample {
        int sampleIndex = -1;
        std::chrono::steady_clock::time_point startTime{};
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

    bool recording = true;
    bool frameActive = false;
    ProfilerFrameRecord currentFrame;
    std::vector<ActiveSample> sampleStack;
    std::array<ProfilerFrameRecord, kMaxHistoryFrames> historyFrames;
    size_t historyWriteIndex = 0;
    size_t historyCount = 0;
    uint64_t latestFrameId = 0;

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
