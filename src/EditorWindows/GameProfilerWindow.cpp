#include "../EditorLocalization.h"
#include "../Engine.h"

namespace Loc = Modularity::Loc;

namespace {
ImU32 GetProfilerSeriesColor(ProfilerSampleCategory category) {
    switch (category) {
        case ProfilerSampleCategory::Render:
        case ProfilerSampleCategory::RenderDetail:
            return IM_COL32(164, 124, 255, 255);
        case ProfilerSampleCategory::GC:
            return IM_COL32(86, 211, 118, 255);
        case ProfilerSampleCategory::Physics:
            return IM_COL32(255, 186, 86, 255);
        case ProfilerSampleCategory::Script:
            return IM_COL32(120, 194, 255, 255);
        default:
            return IM_COL32(232, 236, 245, 255);
    }
}

const char* FormatMs(double value, char* buffer, size_t bufferSize) {
    std::snprintf(buffer, bufferSize, "%.3f ms", value);
    return buffer;
}

const char* FormatBytes(uint64_t value, char* buffer, size_t bufferSize) {
    static const char* kUnits[] = { "B", "KB", "MB", "GB" };
    double display = static_cast<double>(value);
    int unitIndex = 0;
    while (display >= 1024.0 && unitIndex < 3) {
        display /= 1024.0;
        ++unitIndex;
    }
    std::snprintf(buffer, bufferSize, "%.2f %s", display, kUnits[unitIndex]);
    return buffer;
}

const char* FormatSignedBytes(int64_t value, char* buffer, size_t bufferSize) {
    const char sign = value >= 0 ? '+' : '-';
    const uint64_t magnitude = static_cast<uint64_t>(value >= 0 ? value : -value);
    char bytes[96];
    FormatBytes(magnitude, bytes, sizeof(bytes));
    const std::string text = std::string(1, sign) + bytes;
    if (bufferSize == 0) {
        return buffer;
    }
    const size_t copyCount = std::min(bufferSize - 1, text.size());
    std::copy_n(text.data(), copyCount, buffer);
    buffer[copyCount] = '\0';
    return buffer;
}

void DrawGraphLegend(const char* label, ImU32 color) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(cursor, ImVec2(cursor.x + 10.0f, cursor.y + 10.0f), color, 2.0f);
    ImGui::Dummy(ImVec2(12.0f, 10.0f));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted(label);
}

bool SampleHasVisibleContent(const ProfilerFrameRecord& frame,
                             const std::vector<std::vector<int>>& children,
                             int sampleIndex,
                             bool hideZeroEntries) {
    const ProfilerSampleRecord& sample = frame.samples[sampleIndex];
    const bool nonZero = sample.cpuMs > 0.0001 ||
                         sample.gpuMs > 0.0001 ||
                         sample.memoryDeltaBytes != 0 ||
                         sample.memoryTotalBytes != 0 ||
                         sample.callCount > 1;
    if (!hideZeroEntries || nonZero) {
        return true;
    }
    for (int childIndex : children[sampleIndex]) {
        if (SampleHasVisibleContent(frame, children, childIndex, hideZeroEntries)) {
            return true;
        }
    }
    return false;
}

void DrawSampleTreeRow(const ProfilerFrameRecord& frame,
                       const std::vector<std::vector<int>>& children,
                       int sampleIndex,
                       bool hideZeroEntries) {
    if (!SampleHasVisibleContent(frame, children, sampleIndex, hideZeroEntries)) {
        return;
    }

    const ProfilerSampleRecord& sample = frame.samples[sampleIndex];
    bool hasVisibleChildren = false;
    for (int childIndex : children[sampleIndex]) {
        if (SampleHasVisibleContent(frame, children, childIndex, hideZeroEntries)) {
            hasVisibleChildren = true;
            break;
        }
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns;
    if (!hasVisibleChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (sample.depth <= 1) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, GetProfilerSeriesColor(sample.category));
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(sampleIndex)),
                                        flags,
                                        "%s",
                                        sample.name.c_str());
    ImGui::PopStyleColor();

    ImGui::TableNextColumn();
    ImGui::Text("%u", sample.callCount);

    char buffer[96];
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(FormatMs(sample.cpuMs, buffer, sizeof(buffer)));

    ImGui::TableNextColumn();
    if (sample.gpuMs > 0.0001) {
        ImGui::TextUnformatted(FormatMs(sample.gpuMs, buffer, sizeof(buffer)));
    } else {
        ImGui::TextDisabled("-");
    }

    ImGui::TableNextColumn();
    if (sample.memoryDeltaBytes != 0) {
        ImGui::TextUnformatted(FormatSignedBytes(sample.memoryDeltaBytes, buffer, sizeof(buffer)));
    } else {
        ImGui::TextDisabled("-");
    }

    ImGui::TableNextColumn();
    if (sample.memoryTotalBytes != 0) {
        ImGui::TextUnformatted(FormatBytes(sample.memoryTotalBytes, buffer, sizeof(buffer)));
    } else {
        ImGui::TextDisabled("-");
    }

    if (!hasVisibleChildren || !open) {
        return;
    }

    for (int childIndex : children[sampleIndex]) {
        DrawSampleTreeRow(frame, children, childIndex, hideZeroEntries);
    }
    ImGui::TreePop();
}
} // namespace

void Engine::drawGameProfilerContent() {
    Profiler& profiler = Profiler::instance();
    static bool followLatest = true;
    static bool hideZeroEntries = false;
    static uint64_t selectedFrameId = 0;

    const size_t historyCount = profiler.getHistoryCount();
    const uint64_t latestFrameId = profiler.getLatestFrameId();
    if (followLatest && latestFrameId != 0) {
        selectedFrameId = latestFrameId;
    }
    if (selectedFrameId == 0 && latestFrameId != 0) {
        selectedFrameId = latestFrameId;
    }

    if (ImGui::Button(profiler.isRecording() ? "Stop Recording" : "Start Recording")) {
        profiler.setRecording(!profiler.isRecording());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        profiler.clearHistory();
        selectedFrameId = 0;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow Latest", &followLatest);
    ImGui::SameLine();
    ImGui::Checkbox("Hide Zero Entries", &hideZeroEntries);

    const ProfilerFrameRecord* selectedFrame = profiler.getFrameById(selectedFrameId);
    if (!selectedFrame && latestFrameId != 0) {
        selectedFrameId = latestFrameId;
        selectedFrame = profiler.getFrameById(selectedFrameId);
    }

    ImGui::Separator();

    DrawGraphLegend("CPU", IM_COL32(89, 150, 255, 255));
    ImGui::SameLine(0.0f, 18.0f);
    DrawGraphLegend("GPU", IM_COL32(231, 94, 94, 255));
    ImGui::SameLine(0.0f, 18.0f);
    DrawGraphLegend("GC", IM_COL32(86, 211, 118, 255));
    ImGui::SameLine(0.0f, 18.0f);
    DrawGraphLegend("Rendering", IM_COL32(164, 124, 255, 255));
    ImGui::SameLine(0.0f, 18.0f);
    DrawGraphLegend("UI", IM_COL32(244, 184, 96, 255));
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    const float graphHeight = 220.0f;
    const ImVec2 graphSize(ImGui::GetContentRegionAvail().x, graphHeight);
    ImGui::InvisibleButton("##GameProfilerGraph", graphSize);
    const ImRect graphRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(graphRect.Min, graphRect.Max, IM_COL32(16, 18, 25, 255), 8.0f);
    drawList->AddRect(graphRect.Min, graphRect.Max, IM_COL32(255, 255, 255, 28), 8.0f);
    const float graphLeftPadding = 56.0f;
    const float graphRightPadding = 12.0f;
    const float graphTopPadding = 14.0f;
    const float graphBottomPadding = 16.0f;
    const ImRect plotRect(ImVec2(graphRect.Min.x + graphLeftPadding, graphRect.Min.y + graphTopPadding),
                          ImVec2(graphRect.Max.x - graphRightPadding, graphRect.Max.y - graphBottomPadding));

    if (historyCount > 0) {
        double maxValue = 16.6;
        for (size_t i = 0; i < historyCount; ++i) {
            const ProfilerFrameRecord* frame = profiler.getHistoryFrame(i);
            if (!frame) continue;
            maxValue = std::max(maxValue, frame->cpuMs);
            maxValue = std::max(maxValue, frame->gpuMs);
            maxValue = std::max(maxValue, frame->gcMs);
            maxValue = std::max(maxValue, frame->renderMs);
            maxValue = std::max(maxValue, frame->uiMs);
        }
        maxValue = std::max(8.0, maxValue * 1.15);

        for (int guide = 1; guide <= 4; ++guide) {
            const float y = plotRect.Max.y - (plotRect.GetHeight() * guide / 4.0f);
            drawList->AddLine(ImVec2(plotRect.Min.x, y),
                              ImVec2(plotRect.Max.x, y),
                              IM_COL32(255, 255, 255, 18),
                              1.0f);
            char label[32];
            std::snprintf(label, sizeof(label), "%.1f ms", maxValue * guide / 4.0);
            drawList->AddText(ImVec2(graphRect.Min.x + 8.0f, y - ImGui::GetTextLineHeight() * 0.5f),
                              IM_COL32(168, 178, 196, 220),
                              label);
        }

        auto drawSeries = [&](auto valueFn, ImU32 color) {
            ImVector<ImVec2> points;
            points.reserve(static_cast<int>(historyCount));
            for (size_t i = 0; i < historyCount; ++i) {
                const ProfilerFrameRecord* frame = profiler.getHistoryFrame(i);
                if (!frame) continue;
                const float t = (historyCount > 1)
                    ? static_cast<float>(i) / static_cast<float>(historyCount - 1)
                    : 0.0f;
                const double value = valueFn(*frame);
                const float x = plotRect.Min.x + t * plotRect.GetWidth();
                const float y = plotRect.Max.y - static_cast<float>(value / maxValue) * plotRect.GetHeight();
                points.push_back(ImVec2(x, y));
            }
            if (points.Size > 1) {
                drawList->AddPolyline(points.Data, points.Size, color, false, 2.0f);
            }
        };

        drawSeries([](const ProfilerFrameRecord& frame) { return frame.cpuMs; }, IM_COL32(89, 150, 255, 255));
        drawSeries([](const ProfilerFrameRecord& frame) { return frame.gpuMs; }, IM_COL32(231, 94, 94, 255));
        drawSeries([](const ProfilerFrameRecord& frame) { return frame.gcMs; }, IM_COL32(86, 211, 118, 255));
        drawSeries([](const ProfilerFrameRecord& frame) { return frame.renderMs; }, IM_COL32(164, 124, 255, 255));
        drawSeries([](const ProfilerFrameRecord& frame) { return frame.uiMs; }, IM_COL32(244, 184, 96, 255));

        size_t selectedHistoryIndex = historyCount - 1;
        for (size_t i = 0; i < historyCount; ++i) {
            const ProfilerFrameRecord* frame = profiler.getHistoryFrame(i);
            if (frame && frame->frameId == selectedFrameId) {
                selectedHistoryIndex = i;
                break;
            }
        }
        if (historyCount > 0) {
            const float selectedT = (historyCount > 1)
                ? static_cast<float>(selectedHistoryIndex) / static_cast<float>(historyCount - 1)
                : 0.0f;
            const float selectedX = plotRect.Min.x + selectedT * plotRect.GetWidth();
            drawList->AddLine(ImVec2(selectedX, plotRect.Min.y),
                              ImVec2(selectedX, plotRect.Max.y),
                              IM_COL32(255, 255, 255, 110),
                              1.5f);
        }

        if (ImGui::IsItemHovered()) {
            const float normalizedX = std::clamp(
                (ImGui::GetIO().MousePos.x - plotRect.Min.x) / std::max(1.0f, plotRect.GetWidth()),
                0.0f,
                1.0f);
            const size_t hoveredIndex = (historyCount <= 1)
                ? 0
                : static_cast<size_t>(std::round(normalizedX * static_cast<float>(historyCount - 1)));
            const ProfilerFrameRecord* hoveredFrame = profiler.getHistoryFrame(hoveredIndex);
            if (hoveredFrame) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    selectedFrameId = hoveredFrame->frameId;
                    followLatest = false;
                    selectedFrame = hoveredFrame;
                }
                ImGui::BeginTooltip();
                ImGui::Text("Frame %llu", static_cast<unsigned long long>(hoveredFrame->frameId));
                ImGui::Text("CPU: %.3f ms", hoveredFrame->cpuMs);
                ImGui::Text("GPU: %.3f ms", hoveredFrame->gpuMs);
                ImGui::Text("GC: %.3f ms", hoveredFrame->gcMs);
                ImGui::Text("Render: %.3f ms", hoveredFrame->renderMs);
                ImGui::Text("UI: %.3f ms", hoveredFrame->uiMs);
                ImGui::Text("2D Quads: %llu | Batches: %llu",
                            static_cast<unsigned long long>(hoveredFrame->sprite2DQuads),
                            static_cast<unsigned long long>(hoveredFrame->sprite2DBatches));
                ImGui::EndTooltip();
            }
        }
    } else {
        drawList->AddText(ImVec2(plotRect.Min.x + 8.0f, plotRect.Min.y + 8.0f),
                          IM_COL32(168, 178, 196, 255),
                          profiler.isRecording()
                              ? "Recording will populate frame history here."
                              : "Recording is stopped.");
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    if (!selectedFrame) {
        ImGui::TextDisabled("No captured frame selected.");
        return;
    }

    char bytesA[64];
    char bytesB[64];
    if (ImGui::BeginTable("##GameProfilerSummary",
                          2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Frame %llu", static_cast<unsigned long long>(selectedFrame->frameId));
        ImGui::TableNextColumn();
        if (selectedFrame->gpuTimingSupported) {
            ImGui::TextDisabled("CPU %.3f ms | GPU %.3f ms | GC %.3f ms | Render %.3f ms | UI %.3f ms",
                                selectedFrame->cpuMs,
                                selectedFrame->gpuMs,
                                selectedFrame->gcMs,
                                selectedFrame->renderMs,
                                selectedFrame->uiMs);
        } else {
            ImGui::TextDisabled("CPU %.3f ms | GPU unavailable | GC %.3f ms | Render %.3f ms | UI %.3f ms",
                                selectedFrame->cpuMs,
                                selectedFrame->gcMs,
                                selectedFrame->renderMs,
                                selectedFrame->uiMs);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Managed Heap: %s / %s | Delta: %s",
                            FormatBytes(selectedFrame->managedHeapUsedBytes, bytesA, sizeof(bytesA)),
                            FormatBytes(selectedFrame->managedHeapReservedBytes, bytesB, sizeof(bytesB)),
                            FormatSignedBytes(selectedFrame->managedHeapDeltaBytes, bytesA, sizeof(bytesA)));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Texture Cache: %s / %s",
                            FormatBytes(selectedFrame->renderMemoryBytes, bytesA, sizeof(bytesA)),
                            FormatBytes(selectedFrame->renderMemoryBudgetBytes, bytesB, sizeof(bytesB)));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Draw Calls: %llu | Texture Binds: %llu | State Binds: %llu",
                            static_cast<unsigned long long>(selectedFrame->drawCalls),
                            static_cast<unsigned long long>(selectedFrame->textureBinds),
                            static_cast<unsigned long long>(selectedFrame->stateBinds));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("GC Collections: %u / %u / %u",
                            selectedFrame->gcCollectionDelta[0],
                            selectedFrame->gcCollectionDelta[1],
                            selectedFrame->gcCollectionDelta[2]);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("2D Quads: %llu | 2D Batches: %llu | PostFX Passes: %llu",
                            static_cast<unsigned long long>(selectedFrame->sprite2DQuads),
                            static_cast<unsigned long long>(selectedFrame->sprite2DBatches),
                            static_cast<unsigned long long>(selectedFrame->postFxPasses));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Viewport Redraws: %llu | Skipped: %llu | Cached Layer Reuse: %llu | UI Dir Scans: %llu",
                            static_cast<unsigned long long>(selectedFrame->viewportRedraws),
                            static_cast<unsigned long long>(selectedFrame->skippedRedraws),
                            static_cast<unsigned long long>(selectedFrame->cachedLayerReuses),
                            static_cast<unsigned long long>(selectedFrame->uiDirScans));
        ImGui::EndTable();
    }

    ImGui::Separator();

    const float footerHeight = ImGui::GetTextLineHeightWithSpacing() + 14.0f;
    const float tableHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y - footerHeight);
    if (ImGui::BeginTable("##GameProfilerTable",
                          6,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_SizingStretchSame,
                          ImVec2(0.0f, tableHeight))) {
        ImGui::TableSetupColumn("Sample");
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Mem Delta", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Mem Total", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        std::vector<std::vector<int>> children(selectedFrame->samples.size());
        for (size_t i = 0; i < selectedFrame->samples.size(); ++i) {
            const int parentIndex = selectedFrame->samples[i].parentIndex;
            if (parentIndex >= 0 && parentIndex < static_cast<int>(selectedFrame->samples.size())) {
                children[parentIndex].push_back(static_cast<int>(i));
            }
        }

        if (!selectedFrame->samples.empty()) {
            DrawSampleTreeRow(*selectedFrame, children, 0, hideZeroEntries);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (selectedFrame->gpuTimingSupported) {
        ImGui::TextDisabled(selectedFrame->gpuTimingPartial
                                ? "GPU timing uses partial backend support in this build."
                                : "GPU timing is fully supported for this captured frame.");
    } else if (usingVulkan()) {
        ImGui::TextDisabled("GPU timing is not wired for the Vulkan backend yet.");
    } else {
        ImGui::TextDisabled("GPU timing is unavailable for this frame. OpenGL timing requires an active game render.");
    }
}

void Engine::renderGameProfilerWindow() {
    const bool wasOpen = showGameProfilerWindow;
    if (ImGui::Begin(Loc::Window("WINDOW_PROFILER", "Profiler"), &showGameProfilerWindow)) {
        drawGameProfilerContent();
    }
    ImGui::End();
    if (wasOpen != showGameProfilerWindow) {
        saveEditorUserSettings();
    }
}
