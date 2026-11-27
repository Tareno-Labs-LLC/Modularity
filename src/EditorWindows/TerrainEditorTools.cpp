// Terrain brush tool: the Scene view half of the Terrain component's editor (the Inspector section
// lives in InspectorWindow.cpp, the data kernels in TerrainRuntime.cpp). Editor only.
#include "../Engine.h"

#if MODULARITY_RUNTIME_ONLY
// The player has no terrain editor. InspectorWindow.cpp / ViewportWindows.cpp still compile into it
// and reference the tool, so these are inert.
SceneObject* Engine::terrainBrushTarget() { return nullptr; }
void Engine::endTerrainStroke() { terrainStrokeActive = false; terrainStrokeObjectId = -1; }
void Engine::updateTerrainBrush(const glm::mat4&, const glm::mat4&, const ImVec2&, const ImVec2&, bool, bool&) {}
#else

#include "../TerrainRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr double kStampInterval = 1.0 / 15.0;   // seconds between stamps while the button is held

ImU32 BrushColor(const TerrainRuntime::BrushParams& brush, const TerrainComponent& terrain, float alpha) {
    ImVec4 c(1.0f, 1.0f, 1.0f, alpha);
    switch (brush.mode) {
        case TerrainRuntime::BrushMode::Raise: c = ImVec4(0.35f, 1.0f, 0.45f, alpha); break;
        case TerrainRuntime::BrushMode::Lower: c = ImVec4(1.0f, 0.45f, 0.35f, alpha); break;
        case TerrainRuntime::BrushMode::Smooth: c = ImVec4(0.45f, 0.7f, 1.0f, alpha); break;
        case TerrainRuntime::BrushMode::Flatten: c = ImVec4(1.0f, 0.85f, 0.3f, alpha); break;
        case TerrainRuntime::BrushMode::GrassDensity: c = ImVec4(0.6f, 1.0f, 0.25f, alpha); break;
        case TerrainRuntime::BrushMode::PaintLayer: {
            const glm::vec3 tint = terrain.layers[std::clamp(brush.layer, 0, 3)].tint;
            static const glm::vec3 base[4] = {glm::vec3(0.35f, 0.8f, 0.25f), glm::vec3(0.55f, 0.4f, 0.25f),
                                              glm::vec3(0.85f, 0.7f, 0.5f), glm::vec3(0.7f, 0.7f, 0.72f)};
            const glm::vec3 shown = glm::clamp(base[std::clamp(brush.layer, 0, 3)] * tint, 0.0f, 1.0f);
            c = ImVec4(shown.x, shown.y, shown.z, alpha);
            break;
        }
    }
    return ImGui::ColorConvertFloat4ToU32(c);
}

bool ProjectToScreen(const glm::mat4& viewProj, const glm::vec3& world, const ImVec2& imageMin,
                     const ImVec2& imageMax, ImVec2& out) {
    const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-4f) return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    out.x = imageMin.x + (ndc.x * 0.5f + 0.5f) * (imageMax.x - imageMin.x);
    out.y = imageMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * (imageMax.y - imageMin.y);
    return true;
}

// A ring of `radius` metres (terrain-local) draped on the surface around a local XZ centre.
void DrawDrapedRing(ImDrawList* drawList, const SceneObject& object, const glm::mat4& world,
                    const glm::mat4& viewProj, const ImVec2& imageMin, const ImVec2& imageMax,
                    float cx, float cz, float radius, ImU32 color, float thickness) {
    constexpr int kSegments = 72;
    ImVec2 points[kSegments + 1];
    int count = 0;
    for (int i = 0; i <= kSegments; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(kSegments) * 6.2831853f;
        const float lx = cx + std::cos(angle) * radius;
        const float lz = cz + std::sin(angle) * radius;
        const float ly = TerrainRuntime::LocalHeightAt(object.terrain, lx, lz) + 0.12f;
        const glm::vec3 w = glm::vec3(world * glm::vec4(lx, ly, lz, 1.0f));
        ImVec2 p;
        if (!ProjectToScreen(viewProj, w, imageMin, imageMax, p)) {
            if (count > 1) drawList->AddPolyline(points, count, color, ImDrawFlags_None, thickness);
            count = 0;
            continue;
        }
        points[count++] = p;
    }
    if (count > 1) drawList->AddPolyline(points, count, color, ImDrawFlags_None, thickness);
}

} // namespace

SceneObject* Engine::terrainBrushTarget() {
    if (!terrainBrushEnabled || isPlaying) return nullptr;
    SceneObject* object = getSelectedObject();
    if (!object || !object->hasTerrain || !TerrainRuntime::IsActive(*object)) return nullptr;
    return object;
}

void Engine::endTerrainStroke() {
    terrainStrokeActive = false;
    terrainStrokeObjectId = -1;
    terrainStrokeFirstStamp = true;
}

void Engine::updateTerrainBrush(const glm::mat4& view, const glm::mat4& proj, const ImVec2& imageMin,
                                const ImVec2& imageMax, bool mouseOverImage, bool& leftClickConsumed) {
    SceneObject* target = terrainBrushTarget();
    terrainBrushHasHit = false;
    if (!target) {
        if (terrainStrokeActive) endTerrainStroke();
        // Selecting something else disarms the brush, so coming back to a terrain never paints by surprise.
        if (terrainBrushEnabled && !isPlaying) {
            const SceneObject* selected = getSelectedObject();
            if (!selected || !selected->hasTerrain) terrainBrushEnabled = false;
        }
        return;
    }
    if (imageMax.x <= imageMin.x || imageMax.y <= imageMin.y) return;
    ImGuiIO& io = ImGui::GetIO();
    TerrainComponent& terrain = target->terrain;

    // A stroke belongs to the object it started on; selecting something else ends it.
    if (terrainStrokeActive && terrainStrokeObjectId != target->id) endTerrainStroke();

    // MODU_AUTOSHOT_BRUSH_SIM=1 (with --autoshot): the cursor sits at the image centre and the left button
    // is held for 60 frames, so the stroke path (undo record, stamps, ring) can be exercised unattended.
    ImVec2 mouse = io.MousePos;
    bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    static int simFrame = 0;
    static int simStamps = 0;
    static const bool simulate = autoShot_.enabled && std::getenv("MODU_AUTOSHOT_BRUSH_SIM") != nullptr;
    if (simulate) {
        ++simFrame;
        mouse = ImVec2((imageMin.x + imageMax.x) * 0.5f, (imageMin.y + imageMax.y) * 0.5f);
        mouseOverImage = true;
        mouseDown = simFrame >= 10 && simFrame < 70;
        mouseClicked = simFrame == 10;
        if (simFrame == 71) {
            std::fprintf(stderr, "AUTOSHOT brush sim: stamps=%d dirty=%d hit=%d\n", simStamps,
                         terrain.dataDirty() ? 1 : 0, terrainBrushHasHit ? 1 : 0);
        }
    }
    // The ray under the cursor.
    const glm::mat4 invViewProj = glm::inverse(proj * view);
    const float nx = (mouse.x - imageMin.x) / (imageMax.x - imageMin.x) * 2.0f - 1.0f;
    const float ny = 1.0f - (mouse.y - imageMin.y) / (imageMax.y - imageMin.y) * 2.0f;
    glm::vec4 nearPoint = invViewProj * glm::vec4(nx, ny, -1.0f, 1.0f);
    glm::vec4 farPoint = invViewProj * glm::vec4(nx, ny, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::vec3 origin = glm::vec3(nearPoint);
    const glm::vec3 direction = glm::normalize(glm::vec3(farPoint - nearPoint));

    const bool typing = io.WantTextInput;
    const bool hover = mouseOverImage && !cursorLocked && !typing;
    float hitDistance = 0.0f;
    bool hit = false;
    if (hover || terrainStrokeActive) {
        hit = TerrainRuntime::Raycast(*target, origin, direction, 20000.0f, hitDistance);
    }
    glm::vec3 hitWorld(0.0f);
    glm::vec3 hitLocal(0.0f);
    const glm::mat4 world = TerrainRuntime::WorldMatrix(*target);
    if (hit) {
        hitWorld = origin + direction * hitDistance;
        hitLocal = glm::vec3(glm::inverse(world) * glm::vec4(hitWorld, 1.0f));
        terrainBrushHasHit = true;
        terrainBrushHitWorld = hitWorld;
    }

    if (hover) {
        // The brush owns the left button over the viewport: no object picking, no gizmo drags.
        leftClickConsumed = true;
        if (!io.KeyCtrl && !io.KeyAlt) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) terrainBrush.radius = std::max(0.5f, terrainBrush.radius / 1.15f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) terrainBrush.radius = std::min(400.0f, terrainBrush.radius * 1.15f);
        }
    }

    // Stroke control. Alt is the camera-orbit modifier, so Alt-click never paints.
    const bool wantsStart = hover && hit && !io.KeyAlt && mouseClicked;
    if (wantsStart && !terrainStrokeActive) {
        recordState("terrainBrush");   // one undo step per stroke; the buffers copy-on-write on the first stamp
        terrainStrokeActive = true;
        terrainStrokeObjectId = target->id;
        terrainStrokeFirstStamp = true;
        terrainStrokeNextStamp = 0.0;
        if (!terrainFlattenFixed) {
            terrainBrush.flattenHeight = TerrainRuntime::LocalHeightAt(terrain, hitLocal.x, hitLocal.z);
        }
    }
    if (terrainStrokeActive) {
        if (!mouseDown) {
            endTerrainStroke();
        } else if (hit) {
            const double now = ImGui::GetTime();
            if (now >= terrainStrokeNextStamp) {
                TerrainRuntime::BrushParams params = terrainBrush;
                if (io.KeyShift) {
                    if (params.mode == TerrainRuntime::BrushMode::Raise) params.mode = TerrainRuntime::BrushMode::Lower;
                    else if (params.mode == TerrainRuntime::BrushMode::Lower) params.mode = TerrainRuntime::BrushMode::Raise;
                    else if (params.mode == TerrainRuntime::BrushMode::GrassDensity) params.invert = !params.invert;
                }
                TerrainRuntime::ApplyBrush(terrain, hitLocal.x, hitLocal.z, params, terrainStrokeFirstStamp);
                terrainStrokeFirstStamp = false;
                ++simStamps;
                terrainStrokeNextStamp = now + kStampInterval;
                projectManager.currentProject.hasUnsavedChanges = true;
            }
        }
    }

    // Mode banner while the brush is armed.
    {
        static const char* kModeNames[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint layer", "Grass density"};
        char banner[160];
        std::snprintf(banner, sizeof(banner), "Terrain brush: %s  |  LMB stroke, Shift invert, [ ] radius  |  turn off Edit Terrain to move the object",
                      kModeNames[std::clamp(static_cast<int>(terrainBrush.mode), 0, 5)]);
        ImDrawList* bannerList = ImGui::GetWindowDrawList();
        bannerList->PushClipRect(imageMin, imageMax, true);
        const ImVec2 textSize = ImGui::CalcTextSize(banner);
        const ImVec2 pos(imageMin.x + 12.0f, imageMax.y - textSize.y - 16.0f);
        bannerList->AddRectFilled(ImVec2(pos.x - 6.0f, pos.y - 4.0f), ImVec2(pos.x + textSize.x + 6.0f, pos.y + textSize.y + 4.0f),
                                  IM_COL32(0, 0, 0, 140), 4.0f);
        bannerList->AddText(pos, IM_COL32(255, 255, 255, 230), banner);
        bannerList->PopClipRect();
    }

    // Ring cursor: the full radius, the hard core, and a centre cross. Drawn into the viewport's window
    // list, clipped to the image.
    if (hover && hit) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(imageMin, imageMax, true);
        const glm::mat4 viewProj = proj * view;
        const ImU32 outer = BrushColor(terrainBrush, terrain, 0.95f);
        const ImU32 inner = BrushColor(terrainBrush, terrain, 0.5f);
        DrawDrapedRing(drawList, *target, world, viewProj, imageMin, imageMax, hitLocal.x, hitLocal.z,
                       terrainBrush.radius, outer, 2.0f);
        const float core = terrainBrush.radius * std::clamp(1.0f - terrainBrush.falloff, 0.0f, 1.0f);
        if (core > 0.2f) {
            DrawDrapedRing(drawList, *target, world, viewProj, imageMin, imageMax, hitLocal.x, hitLocal.z, core, inner, 1.0f);
        }
        ImVec2 center;
        if (ProjectToScreen(viewProj, hitWorld + glm::vec3(0.0f, 0.12f, 0.0f), imageMin, imageMax, center)) {
            drawList->AddCircleFilled(center, 2.5f, outer, 10);
        }
        drawList->PopClipRect();
    }
}

#endif // MODULARITY_RUNTIME_ONLY
