#include "Profiler.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>
#if !defined(_WIN32)
#include <ctime>
#endif

#ifndef MODULARITY_PROFILE_ALLOCS
#define MODULARITY_PROFILE_ALLOCS 0
#endif

#if MODULARITY_PROFILE_ALLOCS
// Counting replacement for the global allocator, compiled only into profiling builds. The counter
// is thread-local so the per-frame figure is the main thread's own allocations and the hot path
// stays a single non-atomic increment.
namespace { thread_local uint64_t tlsModuAllocCount = 0; }
void* operator new(std::size_t size) {
    ++tlsModuAllocCount;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    ++tlsModuAllocCount;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    ++tlsModuAllocCount;
    return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    ++tlsModuAllocCount;
    return std::malloc(size ? size : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif

namespace {
using ProfilerClock = std::chrono::steady_clock;

double threadCpuNowMs() {
#if !defined(_WIN32) && defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) * 1e-6;
    }
#endif
    return 0.0;
}

bool isWaitSampleName(const std::string& name) {
    return name == "Swap Buffers" || name == "Frame Pacing" || name == "Idle Throttle";
}

double percentileOfSorted(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const size_t index = std::min(sorted.size() - 1,
        static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1) + 0.5));
    return sorted[index];
}

struct ProfileDumpState {
    bool enabled = false;
    double intervalSec = 5.0;
    bool windowStarted = false;
    ProfilerClock::time_point windowStart;
    int frameCount = 0;
    double frameMsSum = 0.0;
    std::unordered_map<std::string, std::pair<double, uint64_t>> sampleTotals;
    std::unordered_map<std::string, uint64_t> sampleAllocTotals;
    double worstFrameMs = 0.0;
    std::vector<std::pair<std::string, double>> worstFrameSamples;
    std::vector<double> frameMsSamples;
    // Per-frame wall time minus the wait scopes: what the engine itself cost.
    std::vector<double> workMsSamples;
    double threadCpuMsSum = 0.0;
    double waitMsSum = 0.0;
    double gpuMsSum = 0.0;
    uint64_t gpuMsCount = 0;
    uint64_t drawCallSum = 0;
    uint64_t textureBindSum = 0;
    uint64_t stateBindSum = 0;
    uint64_t allocSum = 0;
    size_t topCount = 12;
};

ProfileDumpState& profileDumpState() {
    static ProfileDumpState state = [] {
        ProfileDumpState s;
        if (const char* env = std::getenv("MODULARITY_PROFILE_DUMP")) {
            s.enabled = env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
            const double parsed = std::atof(env);
            if (parsed > 0.0) {
                s.intervalSec = std::max(0.5, parsed);
            }
        }
        if (const char* env = std::getenv("MODULARITY_PROFILE_DUMP_TOP")) {
            const int parsed = std::atoi(env);
            if (parsed > 0) s.topCount = static_cast<size_t>(parsed);
        }
        return s;
    }();
    return state;
}

void accumulateProfileDump(const ProfilerFrameRecord& frame) {
    ProfileDumpState& state = profileDumpState();
    if (!state.enabled) {
        return;
    }

    const auto now = ProfilerClock::now();
    if (!state.windowStarted) {
        state.windowStarted = true;
        state.windowStart = now;
    }

    state.frameCount += 1;
    state.frameMsSum += frame.cpuMs;
    state.frameMsSamples.push_back(frame.cpuMs);
    state.workMsSamples.push_back(std::max(0.0, frame.cpuMs - frame.waitMs));
    state.threadCpuMsSum += frame.threadCpuMs;
    state.waitMsSum += frame.waitMs;
    state.drawCallSum += frame.drawCalls;
    state.textureBindSum += frame.textureBinds;
    state.stateBindSum += frame.stateBinds;
    state.allocSum += frame.allocCount;
    for (const ProfilerSampleRecord& sample : frame.samples) {
        if (sample.parentIndex < 0) continue;
        auto& total = state.sampleTotals[sample.name];
        total.first += sample.cpuMs;
        total.second += sample.callCount;
        if (sample.allocCount > 0) {
            state.sampleAllocTotals[sample.name] += sample.allocCount;
        }
    }
    if (frame.cpuMs > state.worstFrameMs) {
        state.worstFrameMs = frame.cpuMs;
        state.worstFrameSamples.clear();
        for (const ProfilerSampleRecord& sample : frame.samples) {
            if (sample.parentIndex < 0) continue;
            state.worstFrameSamples.emplace_back(sample.name, sample.cpuMs);
        }
        std::sort(state.worstFrameSamples.begin(), state.worstFrameSamples.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        if (state.worstFrameSamples.size() > 8) {
            state.worstFrameSamples.resize(8);
        }
    }

    const double elapsedSec = std::chrono::duration<double>(now - state.windowStart).count();
    if (elapsedSec < state.intervalSec || state.frameCount <= 0) {
        return;
    }

    const double avgFrameMs = state.frameMsSum / state.frameCount;
    const double fps = avgFrameMs > 0.0 ? 1000.0 / avgFrameMs : 0.0;
    std::sort(state.frameMsSamples.begin(), state.frameMsSamples.end());
    const size_t onePercentIndex = state.frameMsSamples.empty()
        ? 0
        : state.frameMsSamples.size() - 1 - (state.frameMsSamples.size() / 100);
    const double onePercentLowMs = state.frameMsSamples.empty()
        ? 0.0 : state.frameMsSamples[onePercentIndex];
    std::fprintf(stdout, "[ProfileDump avg/%df] frame=%.3fms fps=%.0f | 1%%low=%.3fms (%.0f fps) | worst=%.3fms\n",
                 state.frameCount, avgFrameMs, fps,
                 onePercentLowMs, onePercentLowMs > 0.0 ? 1000.0 / onePercentLowMs : 0.0,
                 state.worstFrameMs);
    {
        // One machine-readable line per window. "work" is wall time outside the wait scopes,
        // "cpu" is main-thread CPU time; a frame whose work is far above its cpu was blocked
        // somewhere the wait scopes do not cover (driver, page faults, a contended lock).
        std::sort(state.workMsSamples.begin(), state.workMsSamples.end());
        const double frames = static_cast<double>(state.frameCount);
        double workSum = 0.0;
        for (double v : state.workMsSamples) workSum += v;
        std::fprintf(stdout,
                     "[ProfileStats] frames=%d frame_avg=%.3f frame_p50=%.3f frame_p95=%.3f frame_p99=%.3f "
                     "work_avg=%.3f work_p50=%.3f work_p95=%.3f work_p99=%.3f cpu_avg=%.3f wait_avg=%.3f "
                     "gpu_avg=%.3f gpu_samples=%llu draws=%.1f texbinds=%.1f statebinds=%.1f allocs=%.1f\n",
                     state.frameCount, avgFrameMs,
                     percentileOfSorted(state.frameMsSamples, 0.50),
                     percentileOfSorted(state.frameMsSamples, 0.95),
                     percentileOfSorted(state.frameMsSamples, 0.99),
                     workSum / frames,
                     percentileOfSorted(state.workMsSamples, 0.50),
                     percentileOfSorted(state.workMsSamples, 0.95),
                     percentileOfSorted(state.workMsSamples, 0.99),
                     state.threadCpuMsSum / frames,
                     state.waitMsSum / frames,
                     state.gpuMsCount > 0 ? state.gpuMsSum / static_cast<double>(state.gpuMsCount) : 0.0,
                     static_cast<unsigned long long>(state.gpuMsCount),
                     static_cast<double>(state.drawCallSum) / frames,
                     static_cast<double>(state.textureBindSum) / frames,
                     static_cast<double>(state.stateBindSum) / frames,
                     static_cast<double>(state.allocSum) / frames);
    }
    for (const auto& sample : state.worstFrameSamples) {
        std::fprintf(stdout, "[ProfileDump]   worst> %-26s %8.3fms\n",
                     sample.first.c_str(), sample.second);
    }

    std::vector<std::pair<std::string, std::pair<double, uint64_t>>> sorted(
        state.sampleTotals.begin(), state.sampleTotals.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second.first > b.second.first;
    });
    const size_t topCount = std::min<size_t>(sorted.size(), state.topCount);
    for (size_t i = 0; i < topCount; ++i) {
        const auto allocIt = state.sampleAllocTotals.find(sorted[i].first);
        if (allocIt != state.sampleAllocTotals.end()) {
            std::fprintf(stdout, "[ProfileDump]   %-32s %8.3fms/frame  (%.1f calls/frame, %.1f allocs/frame)\n",
                         sorted[i].first.c_str(),
                         sorted[i].second.first / state.frameCount,
                         static_cast<double>(sorted[i].second.second) / state.frameCount,
                         static_cast<double>(allocIt->second) / state.frameCount);
            continue;
        }
        std::fprintf(stdout, "[ProfileDump]   %-32s %8.3fms/frame  (%.1f calls/frame)\n",
                     sorted[i].first.c_str(),
                     sorted[i].second.first / state.frameCount,
                     static_cast<double>(sorted[i].second.second) / state.frameCount);
    }
    std::fflush(stdout);

    state.frameCount = 0;
    state.frameMsSum = 0.0;
    state.sampleTotals.clear();
    state.sampleAllocTotals.clear();
    state.worstFrameMs = 0.0;
    state.worstFrameSamples.clear();
    state.frameMsSamples.clear();
    state.workMsSamples.clear();
    state.threadCpuMsSum = 0.0;
    state.waitMsSum = 0.0;
    state.gpuMsSum = 0.0;
    state.gpuMsCount = 0;
    state.drawCallSum = 0;
    state.textureBindSum = 0;
    state.stateBindSum = 0;
    state.allocSum = 0;
    state.windowStart = now;
}
}

uint64_t Profiler::threadAllocationCount() {
#if MODULARITY_PROFILE_ALLOCS
    return tlsModuAllocCount;
#else
    return 0;
#endif
}

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

void Profiler::setRecording(bool enabled) {
    recording = enabled;
    if (!recording && frameActive) {
        frameActive = false;
        currentFrame = {};
        sampleStack.clear();
    }
}

void Profiler::clearHistory() {
    historyWriteIndex = 0;
    historyCount = 0;
    latestFrameId = 0;
    currentFrame = {};
    sampleStack.clear();
}

void Profiler::beginFrame(uint64_t frameId) {
    resolvePendingGpuQueries();

    frameActive = recording;
    currentFrame = {};
    sampleStack.clear();
    latestFrameId = std::max(latestFrameId, frameId);

    if (!frameActive) {
        return;
    }

    currentFrame.frameId = frameId;
    frameStartThreadCpuMs = threadCpuNowMs();
    frameStartAllocCount = threadAllocationCount();
    currentFrame.samples.reserve(64);
    currentFrame.samples.push_back({
        "Frame",
        ProfilerSampleCategory::Frame,
        -1,
        0,
        1,
        0.0,
        0.0,
        0,
        0,
        false
    });
    sampleStack.push_back({ 0, ProfilerClock::now(), threadAllocationCount() });
}

void Profiler::endFrame() {
    resolvePendingGpuQueries();

    if (!frameActive) {
        return;
    }

    while (sampleStack.size() > 1) {
        endSample();
    }

    const auto now = ProfilerClock::now();
    if (!sampleStack.empty()) {
        ActiveSample root = sampleStack.back();
        sampleStack.pop_back();
        currentFrame.samples[root.sampleIndex].cpuMs =
            std::chrono::duration<double, std::milli>(now - root.startTime).count();
        currentFrame.cpuMs = currentFrame.samples[root.sampleIndex].cpuMs;
    }

    currentFrame.threadCpuMs = std::max(0.0, threadCpuNowMs() - frameStartThreadCpuMs);
    currentFrame.allocCount = threadAllocationCount() - frameStartAllocCount;

    currentFrame.renderMs = 0.0;
    currentFrame.waitMs = 0.0;
    for (const ProfilerSampleRecord& sample : currentFrame.samples) {
        if (sample.category == ProfilerSampleCategory::Render) {
            currentFrame.renderMs += sample.cpuMs;
        }
        if (isWaitSampleName(sample.name)) {
            currentFrame.waitMs += sample.cpuMs;
        }
    }

    currentFrame.uiMs = 0.0;
    for (size_t i = 0; i < currentFrame.samples.size(); ++i) {
        const ProfilerSampleRecord& sample = currentFrame.samples[i];
        if (sample.category != ProfilerSampleCategory::UI) {
            continue;
        }
        const int parentIndex = sample.parentIndex;
        const bool parentIsUI =
            parentIndex >= 0 &&
            parentIndex < static_cast<int>(currentFrame.samples.size()) &&
            currentFrame.samples[parentIndex].category == ProfilerSampleCategory::UI;
        if (!parentIsUI) {
            currentFrame.uiMs += sample.cpuMs;
        }
    }

    accumulateProfileDump(currentFrame);

    historyFrames[historyWriteIndex] = std::move(currentFrame);
    historyWriteIndex = (historyWriteIndex + 1) % kMaxHistoryFrames;
    historyCount = std::min(historyCount + 1, kMaxHistoryFrames);
    frameActive = false;
}

bool Profiler::beginSample(std::string_view name, ProfilerSampleCategory category) {
    if (!frameActive || sampleStack.empty()) {
        return false;
    }

    const int parentIndex = sampleStack.back().sampleIndex;
    const int depth = currentFrame.samples[parentIndex].depth + 1;
    const int sampleIndex = static_cast<int>(currentFrame.samples.size());
    currentFrame.samples.push_back({
        std::string(name),
        category,
        parentIndex,
        depth,
        1,
        0.0,
        0.0,
        0,
        0,
        false
    });
    sampleStack.push_back({ sampleIndex, ProfilerClock::now(), threadAllocationCount() });
    return true;
}

void Profiler::endSample() {
    if (!frameActive || sampleStack.size() <= 1) {
        return;
    }

    const auto now = ProfilerClock::now();
    ActiveSample active = sampleStack.back();
    sampleStack.pop_back();
    currentFrame.samples[active.sampleIndex].cpuMs =
        std::chrono::duration<double, std::milli>(now - active.startTime).count();
    currentFrame.samples[active.sampleIndex].allocCount =
        threadAllocationCount() - active.startAllocCount;
}

void Profiler::addSyntheticSample(std::string_view name,
                                  ProfilerSampleCategory category,
                                  double cpuMs,
                                  uint32_t callCount,
                                  int parentIndex,
                                  int64_t memoryDeltaBytes,
                                  uint64_t memoryTotalBytes) {
    if (!frameActive || parentIndex < 0 || parentIndex >= static_cast<int>(currentFrame.samples.size())) {
        return;
    }

    const int depth = currentFrame.samples[parentIndex].depth + 1;
    currentFrame.samples.push_back({
        std::string(name),
        category,
        parentIndex,
        depth,
        std::max<uint32_t>(1, callCount),
        cpuMs,
        0.0,
        memoryDeltaBytes,
        memoryTotalBytes,
        true
    });
}

void Profiler::setCurrentSampleMemory(int64_t deltaBytes, uint64_t totalBytes) {
    if (!frameActive || sampleStack.empty()) {
        return;
    }
    ProfilerSampleRecord& sample = currentFrame.samples[sampleStack.back().sampleIndex];
    sample.memoryDeltaBytes = deltaBytes;
    sample.memoryTotalBytes = totalBytes;
}

void Profiler::setCurrentFrameGcMetrics(double gcMs,
                                        int64_t heapDeltaBytes,
                                        uint64_t heapUsedBytes,
                                        uint64_t heapReservedBytes,
                                        const std::array<uint32_t, 3>& collectionCounts,
                                        const std::array<uint32_t, 3>& collectionDelta) {
    if (!frameActive) {
        return;
    }
    currentFrame.gcMs = gcMs;
    currentFrame.managedHeapDeltaBytes = heapDeltaBytes;
    currentFrame.managedHeapUsedBytes = heapUsedBytes;
    currentFrame.managedHeapReservedBytes = heapReservedBytes;
    currentFrame.gcCollections = collectionCounts;
    currentFrame.gcCollectionDelta = collectionDelta;
    uint32_t totalCollections = 0;
    for (uint32_t delta : collectionDelta) {
        totalCollections += delta;
    }
    currentFrame.gcTriggered = (totalCollections > 0) || (gcMs > 0.0001);
}

void Profiler::setCurrentFrameRenderCounters(uint64_t drawCalls, uint64_t textureBinds, uint64_t stateBinds) {
    if (!frameActive) {
        return;
    }
    currentFrame.drawCalls = drawCalls;
    currentFrame.textureBinds = textureBinds;
    currentFrame.stateBinds = stateBinds;
}

void Profiler::setCurrentFrame2DCounters(uint64_t spriteQuads,
                                         uint64_t spriteBatches,
                                         uint64_t postFxPasses,
                                         uint64_t viewportRedraws,
                                         uint64_t skippedRedraws,
                                         uint64_t cachedLayerReuses,
                                         uint64_t uiDirScans) {
    if (!frameActive) {
        return;
    }
    currentFrame.sprite2DQuads = spriteQuads;
    currentFrame.sprite2DBatches = spriteBatches;
    currentFrame.postFxPasses = postFxPasses;
    currentFrame.viewportRedraws = viewportRedraws;
    currentFrame.skippedRedraws = skippedRedraws;
    currentFrame.cachedLayerReuses = cachedLayerReuses;
    currentFrame.uiDirScans = uiDirScans;
}

void Profiler::setCurrentFrameRenderMemory(uint64_t usedBytes, uint64_t budgetBytes) {
    if (!frameActive) {
        return;
    }
    currentFrame.renderMemoryBytes = usedBytes;
    currentFrame.renderMemoryBudgetBytes = budgetBytes;
}

void Profiler::setCurrentFrameGpuCapability(bool supported, bool partial) {
    if (!frameActive) {
        return;
    }
    currentFrame.gpuTimingSupported = supported;
    currentFrame.gpuTimingPartial = partial;
}

bool Profiler::supportsOpenGlGpuTimers() const {
#if MODULARITY_OPENGL_ES
    return false;
#elif defined(GL_TIME_ELAPSED)
    return (GLAD_GL_VERSION_3_3 != 0);
#else
    return false;
#endif
}

bool Profiler::beginOpenGlGpuFrame(bool wholeFrame) {
    resolvePendingGpuQueries();
    if (!frameActive || activeGpuQuery != 0 || !supportsOpenGlGpuTimers()) {
        return false;
    }

    activeGpuQuery = acquireGpuQuery();
    if (activeGpuQuery == 0) {
        return false;
    }

    activeGpuQueryFrameId = currentFrame.frameId;
    activeGpuQueryPartial = !wholeFrame;
    currentFrame.gpuTimingSupported = true;
    currentFrame.gpuTimingPartial = !wholeFrame;
#if !MODULARITY_OPENGL_ES
    glBeginQuery(GL_TIME_ELAPSED, activeGpuQuery);
#endif
    return true;
}

void Profiler::endOpenGlGpuFrame() {
    if (activeGpuQuery == 0) {
        return;
    }

#if !MODULARITY_OPENGL_ES
    glEndQuery(GL_TIME_ELAPSED);
#endif
    pendingGpuQueries.push_back({ activeGpuQuery, activeGpuQueryFrameId, activeGpuQueryPartial });
    activeGpuQuery = 0;
    activeGpuQueryFrameId = 0;
    activeGpuQueryPartial = false;
}

size_t Profiler::getHistoryCount() const {
    return historyCount;
}

const ProfilerFrameRecord* Profiler::getHistoryFrame(size_t orderedIndex) const {
    if (orderedIndex >= historyCount) {
        return nullptr;
    }
    const size_t oldestIndex = (historyWriteIndex + kMaxHistoryFrames - historyCount) % kMaxHistoryFrames;
    const size_t slot = (oldestIndex + orderedIndex) % kMaxHistoryFrames;
    return &historyFrames[slot];
}

const ProfilerFrameRecord* Profiler::getFrameById(uint64_t frameId) const {
    if (frameActive && currentFrame.frameId == frameId) {
        return &currentFrame;
    }
    for (size_t i = 0; i < historyCount; ++i) {
        const ProfilerFrameRecord* frame = getHistoryFrame(i);
        if (frame && frame->frameId == frameId) {
            return frame;
        }
    }
    return nullptr;
}

uint64_t Profiler::getLatestFrameId() const {
    if (historyCount > 0) {
        const size_t latestIndex = (historyWriteIndex + kMaxHistoryFrames - 1) % kMaxHistoryFrames;
        return historyFrames[latestIndex].frameId;
    }
    return latestFrameId;
}

void Profiler::resolvePendingGpuQueries() {
#if MODULARITY_OPENGL_ES
    while (!pendingGpuQueries.empty()) {
        releaseGpuQuery(pendingGpuQueries.front().query);
        pendingGpuQueries.pop_front();
    }
#else
    while (!pendingGpuQueries.empty()) {
        const PendingGpuQuery& pending = pendingGpuQueries.front();
        GLuint available = GL_FALSE;
        glGetQueryObjectuiv(pending.query, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == GL_FALSE) {
            break;
        }

        GLuint64 elapsedNs = 0;
        glGetQueryObjectui64v(pending.query, GL_QUERY_RESULT, &elapsedNs);
        attachGpuTimeToFrame(pending.frameId,
                             static_cast<double>(elapsedNs) / 1000000.0,
                             pending.partial);
        releaseGpuQuery(pending.query);
        pendingGpuQueries.pop_front();
    }
#endif
}

GLuint Profiler::acquireGpuQuery() {
    if (!gpuQueryPool.empty()) {
        GLuint query = gpuQueryPool.back();
        gpuQueryPool.pop_back();
        return query;
    }

    GLuint query = 0;
    glGenQueries(1, &query);
    return query;
}

void Profiler::releaseGpuQuery(GLuint query) {
    if (query != 0) {
        gpuQueryPool.push_back(query);
    }
}

ProfilerFrameRecord* Profiler::findFrameMutable(uint64_t frameId) {
    if (frameActive && currentFrame.frameId == frameId) {
        return &currentFrame;
    }
    for (size_t i = 0; i < historyCount; ++i) {
        const size_t oldestIndex = (historyWriteIndex + kMaxHistoryFrames - historyCount) % kMaxHistoryFrames;
        const size_t slot = (oldestIndex + i) % kMaxHistoryFrames;
        if (historyFrames[slot].frameId == frameId) {
            return &historyFrames[slot];
        }
    }
    return nullptr;
}

void Profiler::noteGpuTimeForDump(double gpuMs) {
    ProfileDumpState& state = profileDumpState();
    if (state.enabled) {
        state.gpuMsSum += gpuMs;
        state.gpuMsCount += 1;
    }
}

void Profiler::attachGpuTimeToFrame(uint64_t frameId, double gpuMs, bool partial) {
    noteGpuTimeForDump(gpuMs);
    ProfilerFrameRecord* frame = findFrameMutable(frameId);
    if (!frame) {
        return;
    }

    frame->gpuMs = gpuMs;
    frame->gpuTimingSupported = true;
    frame->gpuTimingPartial = partial;

    for (ProfilerSampleRecord& sample : frame->samples) {
        if (sample.category == ProfilerSampleCategory::Render) {
            sample.gpuMs = gpuMs;
            break;
        }
    }
}
