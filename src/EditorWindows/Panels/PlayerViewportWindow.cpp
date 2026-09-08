#include "Engine.h"
#include "ViewportRenderHelpers.h"
#include "GizmoToolbar.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <numeric>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <shlobj.h>
#endif

using namespace ViewportRenderHelpers;

#pragma region Player Viewport
void Engine::renderPlayerViewport() {
  const auto runtimeUiStart = std::chrono::steady_clock::now();
  double runtimeSpriteBatchBuildMs = 0.0;
  uint32_t runtimeVisibleObjectCount = 0;
  Runtime2DWorldProfilerStats runtime2DProfilerStats;
  int runtimeRenderWidth = kRuntimeInternalWidth;
  int runtimeRenderHeight = kRuntimeInternalHeight;
  getRuntimeInternalResolution(runtimeRenderWidth, runtimeRenderHeight);

  ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  if (playerMode && isPlaying && gameViewCursorLocked) {
    ImGui::SetNextWindowFocus();
  }

  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoScrollbar;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("PlayerViewport", nullptr, flags);
  ImGui::PopStyleVar();

  bool windowFocused =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  const ImVec2 availableSize = ImGui::GetContentRegionAvail();
  const ImVec2 panelMin = ImGui::GetCursorScreenPos();
  const ImVec2 imageSize =
      ComputeAspectFitSize(availableSize, getRuntimeInternalAspect());
  const ImVec2 cursorStart = ImGui::GetCursorPos();
  const float imageOffsetX =
      std::max(0.0f, (availableSize.x - imageSize.x) * 0.5f);
  const float imageOffsetY =
      std::max(0.0f, (availableSize.y - imageSize.y) * 0.5f);
  ImGui::SetCursorPos(
      ImVec2(cursorStart.x + imageOffsetX, cursorStart.y + imageOffsetY));

  if (!rendererInitialized) {
    ImGui::Dummy(imageSize);
  }

  if (rendererInitialized) {
    Modu2DStats::CountViewportRedraw();
    const SceneObject *activeRuntimeCamera =
        playerMode ? findPlayerCameraObject() : nullptr;
    const bool pure2DViewport =
        isProject2DPipeline() ||
        (activeRuntimeCamera && activeRuntimeCamera->camera.use2D);
    unsigned int tex = pure2DViewport ? 0 : getActiveSceneTexture();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::InvisibleButton("PlayerViewportFrame", imageSize);
    ImGui::PopStyleColor(3);
    ImVec2 imageMin = ImGui::GetItemRectMin();
    ImVec2 imageMax = ImGui::GetItemRectMax();
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    ApplyNearestTextureSampling(tex);
    if (tex != 0) {
      drawList->AddImage((void *)(intptr_t)tex, imageMin, imageMax,
                         ImVec2(0, 1), ImVec2(1, 0));
    } else {
      drawList->AddRectFilled(imageMin, imageMax,
                              pure2DViewport ? IM_COL32(0, 0, 0, 255)
                                             : IM_COL32(12, 14, 20, 255),
                              0.0f);
    }
    bool imageHovered =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool showingStartupSplash = false;

    if (playerMode && buildSettings.splashEnabled &&
        buildSettings.splashDurationSeconds > 0.0f) {
      if (startupSplashStartTime < 0.0) {
        startupSplashStartTime = glfwGetTime();
      }
      const double elapsed = glfwGetTime() - startupSplashStartTime;
      showingStartupSplash =
          elapsed < static_cast<double>(buildSettings.splashDurationSeconds);
    }

    auto updateUiCanvas3DInput = [&](const Camera &cam, float fovDeg,
                                     float nearPlane, float farPlane) {
      if (!imageHovered)
        return;
      ImVec2 mouse = ImGui::GetIO().MousePos;
      if (mouse.x < imageMin.x || mouse.x > imageMax.x ||
          mouse.y < imageMin.y || mouse.y > imageMax.y) {
        return;
      }
      float width = std::max(1.0f, imageMax.x - imageMin.x);
      float height = std::max(1.0f, imageMax.y - imageMin.y);
      float ndcX = ((mouse.x - imageMin.x) / width) * 2.0f - 1.0f;
      float ndcY = 1.0f - ((mouse.y - imageMin.y) / height) * 2.0f;

      glm::mat4 view = cam.getViewMatrix();
      glm::mat4 proj = glm::perspective(glm::radians(fovDeg), width / height,
                                        nearPlane, farPlane);
      glm::mat4 inv = glm::inverse(proj * view);
      glm::vec4 nearP = inv * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
      glm::vec4 farP = inv * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
      glm::vec3 origin = glm::vec3(nearP) / nearP.w;
      glm::vec3 dir = glm::normalize(glm::vec3(farP) / farP.w - origin);

      const float eps = 1e-5f;
      for (auto &canvas : sceneObjects) {
        if (!canvas.enabled || !canvas.hasUI ||
            canvas.ui.type != UIElementType::Canvas || !canvas.ui.renderIn3D)
          continue;
        glm::mat4 model(1.0f);
        model = glm::translate(model, canvas.position);
        model = glm::rotate(model, glm::radians(canvas.rotation.x),
                            glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(canvas.rotation.y),
                            glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(canvas.rotation.z),
                            glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, canvas.scale);

        glm::vec3 planePoint = glm::vec3(model * glm::vec4(0, 0, 0, 1));
        glm::vec3 planeNormal =
            glm::normalize(glm::mat3(model) * glm::vec3(0, 0, 1));
        float denom = glm::dot(planeNormal, dir);
        if (std::abs(denom) < eps)
          continue;
        float t = glm::dot(planeNormal, planePoint - origin) / denom;
        if (t < 0.0f)
          continue;
        glm::vec3 hit = origin + dir * t;
        glm::vec4 local4 = glm::inverse(model) * glm::vec4(hit, 1.0f);
        glm::vec3 local = glm::vec3(local4);
        if (std::abs(local.x) > 0.5f || std::abs(local.y) > 0.5f)
          continue;

        float u = local.x + 0.5f;
        float v = 0.5f - local.y;
        float layoutW = std::max(1.0f, canvas.ui.size.x);
        float layoutH = std::max(1.0f, canvas.ui.size.y);
        ImVec2 canvasPos(u * layoutW, v * layoutH);

        UiCanvas3DInput &input = uiCanvas3DInputs[canvas.id];
        if (!input.hasInput || t < input.hitT) {
          input.mousePos = canvasPos;
          input.mouseDown[0] = ImGui::GetIO().MouseDown[0];
          input.mouseDown[1] = ImGui::GetIO().MouseDown[1];
          input.mouseDown[2] = ImGui::GetIO().MouseDown[2];
          input.mouseWheel = ImGui::GetIO().MouseWheel;
          input.hasInput = true;
          input.hitT = t;
        }
      }
    };

    float runtimeFov = buildSettings.editorCameraFov;
    float runtimeNear = buildSettings.editorCameraNear;
    float runtimeFar = buildSettings.editorCameraFar;
    if (playerMode) {
      const SceneObject *runtimeCam = findPlayerCameraObject();
      if (runtimeCam) {
        runtimeFov = ResolveCameraVerticalFovDeg(
            runtimeCam->camera,
            static_cast<float>(std::max(1, runtimeRenderWidth)) /
                static_cast<float>(std::max(1, runtimeRenderHeight)));
        runtimeNear = std::max(0.01f, runtimeCam->camera.nearClip);
        runtimeFar = std::max(runtimeNear + 0.01f, runtimeCam->camera.farClip);
      }
    }
    updateUiCanvas3DInput(camera, runtimeFov, runtimeNear, runtimeFar);

    float uiScaleX =
        imageSize.x / static_cast<float>(std::max(1, runtimeRenderWidth));
    float uiScaleY =
        imageSize.y / static_cast<float>(std::max(1, runtimeRenderHeight));

    if (showCanvasOverlay) {
      ImVec2 pad(8.0f, 8.0f);
      ImVec2 tl(imageMin.x + pad.x, imageMin.y + pad.y);
      ImVec2 br(imageMax.x - pad.x, imageMax.y - pad.y);
      drawList->AddRect(tl, br, IM_COL32(110, 170, 255, 180), 8.0f, 0, 2.0f);
    }

    bool uiInteracting = false;
    UiSceneLookupCache uiSceneLookup(sceneObjects);
    auto find3DCanvasId = [&](const SceneObject &target) -> int {
      return uiSceneLookup.find3DCanvasId(target);
    };
    auto findPseudo3DCanvasId = [&](const SceneObject &target) -> int {
      return uiSceneLookup.findPseudo3DCanvasId(target);
    };
    [[maybe_unused]] auto isUiOn3DCanvas = [&](const SceneObject &target) {
      return find3DCanvasId(target) >= 0;
    };
    int editCanvas3DId = -1;
    if (SceneObject *selected = getSelectedObject()) {
      editCanvas3DId = find3DCanvasId(*selected);
    }
    auto isUIType = [&](const SceneObject &target) {
      if (target.type == ObjectType::Sprite25D)
        return false;
      if (!target.hasUI || target.ui.type == UIElementType::None)
        return false;
      int canvasId = find3DCanvasId(target);
      if (!((canvasId < 0) || (canvasId == editCanvas3DId))) {
        return false;
      }
      return findPseudo3DCanvasId(target) < 0;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::SetCursorScreenPos(imageMin);
    ImGui::BeginChild(
        "PlayerUIOverlay",
        ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y), false,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

    auto anchorToPivot = [](UIAnchor anchor, const ImVec2 &size) {
      switch (anchor) {
      case UIAnchor::Center:
        return ImVec2(size.x * 0.5f, size.y * 0.5f);
      case UIAnchor::TopLeft:
        return ImVec2(0.0f, 0.0f);
      case UIAnchor::TopRight:
        return ImVec2(size.x, 0.0f);
      case UIAnchor::BottomLeft:
        return ImVec2(0.0f, size.y);
      case UIAnchor::BottomRight:
        return ImVec2(size.x, size.y);
      default:
        return ImVec2(size.x * 0.5f, size.y * 0.5f);
      }
    };
    auto anchorToPoint = [](UIAnchor anchor, const ImVec2 &min,
                            const ImVec2 &max) {
      switch (anchor) {
      case UIAnchor::Center:
        return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
      case UIAnchor::TopLeft:
        return min;
      case UIAnchor::TopRight:
        return ImVec2(max.x, min.y);
      case UIAnchor::BottomLeft:
        return ImVec2(min.x, max.y);
      case UIAnchor::BottomRight:
        return max;
      default:
        return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
      }
    };

    auto resolveUIRect = [&](const SceneObject &obj, ImVec2 &outMin,
                             ImVec2 &outMax) {
      std::vector<const SceneObject *> chain;
      chain.reserve(8);
      const SceneObject *current = &obj;
      while (current) {
        if (isUIType(*current)) {
          chain.push_back(current);
        }
        if (current->parentId < 0)
          break;
        current = uiSceneLookup.find(current->parentId);
        if (current == nullptr)
          break;
      }
      std::reverse(chain.begin(), chain.end());

      ImVec2 regionMin = ImGui::GetWindowPos();
      ImVec2 regionMax = ImVec2(regionMin.x + ImGui::GetWindowWidth(),
                                regionMin.y + ImGui::GetWindowHeight());
      float accumRot = 0.0f;
      ImVec2 parentCenter((regionMin.x + regionMax.x) * 0.5f,
                          (regionMin.y + regionMax.y) * 0.5f);
      auto rotDir = [](ImVec2 v, float rad) {
        const float c = std::cos(rad), s = std::sin(rad);
        return ImVec2(v.x * c - v.y * s, v.x * s + v.y * c);
      };
      auto rotAbout = [&](ImVec2 p, ImVec2 pivot, float rad) {
        ImVec2 d = rotDir(ImVec2(p.x - pivot.x, p.y - pivot.y), rad);
        return ImVec2(pivot.x + d.x, pivot.y + d.y);
      };
      for (const SceneObject *node : chain) {
        glm::vec2 nodeSizeWorld = getSpriteDisplaySize(*node);
        ImVec2 size = ImVec2(std::max(1.0f, nodeSizeWorld.x * uiScaleX),
                             std::max(1.0f, nodeSizeWorld.y * uiScaleY));
        ImVec2 anchorPoint =
            anchorToPoint(node->ui.anchor, regionMin, regionMax);
        ImVec2 anchorWorld = rotAbout(anchorPoint, parentCenter, accumRot);
        ImVec2 off = rotDir(ImVec2(node->ui.position.x * uiScaleX,
                                   node->ui.position.y * uiScaleY),
                            accumRot);
        ImVec2 pivot(anchorWorld.x + off.x, anchorWorld.y + off.y);
        const float nodeRot = accumRot + glm::radians(node->ui.rotation);
        ImVec2 pivotOffset = anchorToPivot(node->ui.anchor, size);
        ImVec2 centerOff = rotDir(ImVec2(size.x * 0.5f - pivotOffset.x,
                                         size.y * 0.5f - pivotOffset.y),
                                  nodeRot);
        ImVec2 center(pivot.x + centerOff.x, pivot.y + centerOff.y);
        regionMin = ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
        regionMax = ImVec2(center.x + size.x * 0.5f, center.y + size.y * 0.5f);
        parentCenter = center;
        accumRot = nodeRot;
      }
      outMin = regionMin;
      outMax = regionMax;
    };

    ImVec2 overlayPos = ImGui::GetWindowPos();
    ImVec2 overlaySize = ImGui::GetWindowSize();
    const bool project2DPipeline = isProject2DPipeline();
    bool useWorldUi = false;
    SpriteTextureResolver spriteTextureResolver(rendererInitialized ? &renderer
                                                                    : nullptr);
    if (playerMode) {
      const SceneObject *runtimeCam = findPlayerCameraObject();
      useWorldUi =
          project2DPipeline || (runtimeCam && runtimeCam->camera.use2D);
      if (runtimeCam && useWorldUi) {
        uiWorldCamera.position =
            glm::vec2(runtimeCam->position.x, runtimeCam->position.y);
        uiWorldCamera.zoom = std::max(1.0f, runtimeCam->camera.pixelsPerUnit);
      }
    }
    runtime2DProfilerStats.useWorldUi = useWorldUi;
    uiWorldPanning = false;
    if (useWorldUi) {
      uiWorldCamera.viewportSize = glm::vec2(overlaySize.x, overlaySize.y);
    }
    Camera projectedUiCamera = camera;
    if (playerMode) {
      if (const SceneObject *runtimeCam = findPlayerCameraObject()) {
        projectedUiCamera = makeCameraFromObject(*runtimeCam);
      }
    }
    glm::mat4 projectedUiView = projectedUiCamera.getViewMatrix();
    glm::mat4 projectedUiProj =
        projectedUiCamera.orthographic
            ? glm::ortho(
                  -overlaySize.x /
                      (2.0f * std::max(1.0f, projectedUiCamera.pixelsPerUnit)),
                  overlaySize.x /
                      (2.0f * std::max(1.0f, projectedUiCamera.pixelsPerUnit)),
                  -overlaySize.y /
                      (2.0f * std::max(1.0f, projectedUiCamera.pixelsPerUnit)),
                  overlaySize.y /
                      (2.0f * std::max(1.0f, projectedUiCamera.pixelsPerUnit)),
                  runtimeNear, runtimeFar)
            : glm::perspective(
                  glm::radians(runtimeFov),
                  std::max(0.1f, overlaySize.x / std::max(1.0f, overlaySize.y)),
                  runtimeNear, runtimeFar);
    bool hasProjectedUiCamera = true;
    auto worldToScreen = [&](const glm::vec2 &world) {
      glm::vec2 local = uiWorldCamera.WorldToScreen(world);
      return ImVec2(overlayPos.x + local.x, overlayPos.y + local.y);
    };
    auto parallaxOffset = [&](const SceneObject &obj) {
      if (!obj.hasParallaxLayer2D || !obj.parallaxLayer2D.enabled)
        return glm::vec2(0.0f);
      const float factor = std::clamp(obj.parallaxLayer2D.factor, 0.0f, 1.0f);
      return uiWorldCamera.position * (1.0f - factor);
    };
    glm::vec2 worldViewMin =
        useWorldUi ? uiWorldCamera.ScreenToWorld(glm::vec2(0.0f, overlaySize.y))
                   : glm::vec2(0.0f);
    glm::vec2 worldViewMax =
        useWorldUi ? uiWorldCamera.ScreenToWorld(glm::vec2(overlaySize.x, 0.0f))
                   : glm::vec2(0.0f);
    auto resolveUIRectWorld = [&](const SceneObject &obj, ImVec2 &outMin,
                                  ImVec2 &outMax) {
      if (obj.type == ObjectType::Sprite25D && hasProjectedUiCamera) {
        return ResolveProjectedSprite25DRect(obj, projectedUiView,
                                             projectedUiProj, overlayPos,
                                             overlaySize, outMin, outMax);
      }
      glm::vec2 parentOffset = uiSceneLookup.getWorldParentOffset(obj);
      float parentRot = uiSceneLookup.getWorldParentRotationRad(obj);
      glm::vec2 worldPos =
          parentOffset +
          ViewportRenderHelpers::RotateUiVec2(
              glm::vec2(obj.ui.position.x, obj.ui.position.y), parentRot) +
          parallaxOffset(obj);
      glm::vec2 sizeWorld = getSpriteDisplaySize(obj);
      ImVec2 pivotOffset =
          anchorToPivot(obj.ui.anchor, ImVec2(sizeWorld.x, sizeWorld.y));
      glm::vec2 worldMin = worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
      glm::vec2 worldMax = worldMin + sizeWorld;
      ImVec2 s0 = worldToScreen(worldMin);
      ImVec2 s1 = worldToScreen(worldMax);
      outMin = ImVec2(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
      outMax = ImVec2(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
      return true;
    };
    struct CachedPlayerUiWorldRect {
      bool resolved = false;
      bool valid = false;
      ImVec2 min = ImVec2(0.0f, 0.0f);
      ImVec2 max = ImVec2(0.0f, 0.0f);
    };
    std::unordered_map<int, CachedPlayerUiWorldRect> cachedUiWorldRects;
    cachedUiWorldRects.reserve(sceneObjects.size());
    auto resolveCachedUiWorldRect = [&](const SceneObject &obj, ImVec2 &outMin,
                                        ImVec2 &outMax) {
      CachedPlayerUiWorldRect &cached = cachedUiWorldRects[obj.id];
      if (!cached.resolved) {
        cached.resolved = true;
        cached.valid = resolveUIRectWorld(obj, cached.min, cached.max);
      }
      if (!cached.valid) {
        return false;
      }
      outMin = cached.min;
      outMax = cached.max;
      return true;
    };

    bool uiWorldCameraActive = false;

    auto brighten = [](const ImVec4 &c, float k) {
      return ImVec4(std::clamp(c.x * k, 0.0f, 1.0f),
                    std::clamp(c.y * k, 0.0f, 1.0f),
                    std::clamp(c.z * k, 0.0f, 1.0f), c.w);
    };
    float animSpeed = 0.0f;
    if (uiAnimationMode == UIAnimationMode::Fluid) {
      animSpeed = 8.0f;
    } else if (uiAnimationMode == UIAnimationMode::Snappy) {
      animSpeed = 18.0f;
    }
    float animStep =
        (uiAnimationMode == UIAnimationMode::Off)
            ? 1.0f
            : (1.0f - std::exp(-animSpeed * ImGui::GetIO().DeltaTime));
    auto animateValue = [&](float &current, float target, bool immediate) {
      if (uiAnimationMode == UIAnimationMode::Off || immediate) {
        current = target;
      } else {
        current += (target - current) * animStep;
      }
      return current;
    };
    std::vector<SceneObject *> uiDrawList;
    uiDrawList.reserve(sceneObjects.size());
    for (auto &obj : sceneObjects) {
      if (!IsObjectEnabledInHierarchy(obj))
        continue;
      if (!isUIType(obj) && !ViewportRenderHelpers::IsAssemblageLayerDrawable(obj))
        continue;
      uiDrawList.push_back(&obj);
    }
    if (uiDrawList.size() > 1) {
      StableSortRuntimeUiDrawList(uiDrawList);
    }
    const auto spriteBatchBuildStart = std::chrono::steady_clock::now();
    BatchedSpriteEmitter spriteBatch(ImGui::GetWindowDrawList());
    spriteBatch.reserve(uiDrawList.size());
    struct CachedMaskRect {
      bool resolved = false;
      bool hasMask = false;
      ImVec2 min = ImVec2(0.0f, 0.0f);
      ImVec2 max = ImVec2(0.0f, 0.0f);
    };
    std::unordered_map<int, CachedMaskRect> cachedMaskRects;
    cachedMaskRects.reserve(sceneObjects.size());
    std::function<const CachedMaskRect &(const SceneObject &)>
        resolveCachedMaskRect =
            [&](const SceneObject &node) -> const CachedMaskRect & {
      CachedMaskRect &cached = cachedMaskRects[node.id];
      if (cached.resolved) {
        return cached;
      }
      cached.resolved = true;

      if (node.parentId >= 0) {
        const SceneObject *parent = uiSceneLookup.find(node.parentId);
        if (parent) {
          const CachedMaskRect &parentMask = resolveCachedMaskRect(*parent);
          if (parentMask.hasMask) {
            cached.hasMask = true;
            cached.min = parentMask.min;
            cached.max = parentMask.max;
          }

          if (parent->hasUI && parent->ui.type == UIElementType::Canvas &&
              parent->ui.maskChildren) {
            ImVec2 canvasMin, canvasMax;
            bool hasCanvasRect = true;
            if (useWorldUi || parent->type == ObjectType::Sprite25D) {
              hasCanvasRect =
                  resolveCachedUiWorldRect(*parent, canvasMin, canvasMax);
            } else {
              resolveUIRect(*parent, canvasMin, canvasMax);
            }
            if (hasCanvasRect) {
              if (!cached.hasMask) {
                cached.hasMask = true;
                cached.min = canvasMin;
                cached.max = canvasMax;
              } else {
                cached.min.x = std::max(cached.min.x, canvasMin.x);
                cached.min.y = std::max(cached.min.y, canvasMin.y);
                cached.max.x = std::min(cached.max.x, canvasMax.x);
                cached.max.y = std::min(cached.max.y, canvasMax.y);
              }
            }
          }
        }
      }

      if (cached.hasMask &&
          (cached.max.x <= cached.min.x || cached.max.y <= cached.min.y)) {
        cached.hasMask = false;
      }
      return cached;
    };
    auto resolveCanvasMaskRectForObject =
        [&](const SceneObject &obj, ImVec2 &outMin, ImVec2 &outMax) -> bool {
      const CachedMaskRect &cached = resolveCachedMaskRect(obj);
      if (!cached.hasMask) {
        return false;
      }
      outMin = cached.min;
      outMax = cached.max;
      return true;
    };

    std::unordered_set<int> light2DRenderedObjectIds;
    bool renderedLight2DComposite = false;
    Light2DDebugStats light2DStats;
    int activeLight2DCount = 0;
    int litSprite2DCount = 0;
    int litWorldImageCount = 0;
    bool lightBufferHadContent = false;
    std::unordered_map<int, std::string> light2DRoutingReasons;
    SceneObject *selectedForRoutingReasons =
        showInspector ? getSelectedObject() : nullptr;
    const bool captureLight2DRoutingReasons =
        selectedForRoutingReasons && selectedForRoutingReasons->hasUI;
    if (captureLight2DRoutingReasons) {
      light2DRoutingReasons.reserve(uiDrawList.size());
    }
    auto setLight2DRoutingReason = [&](int objectId, const char *reason) {
      if (captureLight2DRoutingReasons) {
        light2DRoutingReasons[objectId] = reason;
      }
    };
    if (useWorldUi && rendererInitialized) {
      Light2DRenderRequest lightRequest;
      lightRequest.width =
          std::max(1, static_cast<int>(std::round(overlaySize.x)));
      lightRequest.height =
          std::max(1, static_cast<int>(std::round(overlaySize.y)));
      lightRequest.clearColor = glm::vec4(0.0f);
      lightRequest.baseAmbient = glm::vec3(0.0f);
      lightRequest.lightingBufferScale = buildSettings.light2DLightingResolution;
      lightRequest.lightingBufferScaleAuto = buildSettings.light2DLightingResolutionAuto;
      lightRequest.lightingPixelFilter = buildSettings.light2DLightingPixelFilter;
      lightRequest.lightingSDR = buildSettings.light2DLightingSDR;
      lightRequest.postFx = resolveWorld2DPostFx(projectedUiCamera);
      lightRequest.blendStyles = light2DBlendStyles;
      lightRequest.sprites.reserve(uiDrawList.size());
      lightRequest.lights.reserve(sceneObjects.size());
      const glm::vec2 lightOverlayMax(
          std::max(1.0f, overlaySize.x), std::max(1.0f, overlaySize.y));
      auto lightBoundsOutsideOverlay = [&](const glm::vec2 &boundsMin,
                                           const glm::vec2 &boundsMax,
                                           float margin = 0.0f) {
        const float m = std::max(0.0f, margin);
        return boundsMax.x < -m || boundsMin.x > lightOverlayMax.x + m ||
               boundsMax.y < -m || boundsMin.y > lightOverlayMax.y + m;
      };
      const float flickerTime = static_cast<float>(glfwGetTime());
      auto computeFlickerMultiplier =
          [flickerTime](const Light2DFlickerSettings &flicker) {
            if (!flicker.enabled || flicker.amount <= 0.0001f) {
              return 1.0f;
            }
            const float base =
                std::sin(flickerTime * std::max(0.01f, flicker.speed) +
                         flicker.seed);
            const float jitter =
                std::sin(flickerTime *
                             std::max(0.01f, flicker.speed * 2.173f) +
                         flicker.seed * 1.913f);
            const float noise = 0.5f + 0.35f * base + 0.15f * jitter;
            return glm::mix(1.0f, std::max(0.0f, noise),
                            std::clamp(flicker.amount, 0.0f, 1.0f));
          };

      int drawOrder = 0;
      assemblageTileQuadsLastFrame = 0;
      for (SceneObject *objPtr : uiDrawList) {
        SceneObject &obj = *objPtr;
        if (ViewportRenderHelpers::IsAssemblageLayerDrawable(obj)) {
          if (useWorldUi) {
            const int layerDrawOrder = drawOrder++;
            assemblageTileQuadsLastFrame += ViewportRenderHelpers::AppendAssemblageLayerQuads(
                sceneObjects, obj, assemblageRuntime, renderer, glfwGetTime(),
                worldViewMin, worldViewMax, worldToScreen,
                [&](const ViewportRenderHelpers::AssemblageScreenQuad &quad) {
                  Light2DScreenSprite sprite;
                  sprite.objectId = obj.id;
                  sprite.layer = obj.layer;
                  sprite.drawOrder = layerDrawOrder;
                  sprite.textureId = quad.textureId;
                  sprite.tint = quad.tint;
                  sprite.receiveLighting = obj.assemblageLayer.receiveLighting2D;
                  sprite.unlit = obj.assemblageLayer.unlitLighting2D;
                  sprite.emissiveIntensity = obj.assemblageLayer.emissiveLighting2D;
                  for (int corner = 0; corner < 4; ++corner) {
                    sprite.positions[corner] =
                        glm::vec2(quad.pos[corner].x - overlayPos.x,
                                  quad.pos[corner].y - overlayPos.y);
                    sprite.uvs[corner] = glm::vec2(quad.uv[corner].x, quad.uv[corner].y);
                  }
                  lightRequest.sprites.push_back(sprite);
                });
            light2DRenderedObjectIds.insert(obj.id);
          }
          continue;
        }
        if (!(obj.ui.type == UIElementType::Image ||
              obj.ui.type == UIElementType::Sprite2D))
          continue;
        if (obj.ui.nineSliceEnabled) {
          setLight2DRoutingReason(obj.id, "Legacy path: nine-slice sprites are "
                                          "not routed through Light2D yet.");
          continue;
        }
        if (obj.ui.unlitLighting2D) {
          setLight2DRoutingReason(obj.id, "Legacy path: Force Unlit keeps this "
                                          "sprite on the legacy 2D renderer.");
          continue;
        }
        const bool repeatX = obj.hasParallaxLayer2D &&
                             obj.parallaxLayer2D.enabled &&
                             obj.parallaxLayer2D.repeatX;
        const bool repeatY = obj.hasParallaxLayer2D &&
                             obj.parallaxLayer2D.enabled &&
                             obj.parallaxLayer2D.repeatY;
        const bool disableCulling = obj.hasParallaxLayer2D &&
                                    obj.parallaxLayer2D.enabled &&
                                    obj.parallaxLayer2D.disableCulling;

        ImVec2 rectMin, rectMax;
        if (!resolveCachedUiWorldRect(obj, rectMin, rectMax)) {
          setLight2DRoutingReason(
              obj.id, "Legacy path: failed to resolve a world-space sprite "
                      "rect for the active viewport.");
          continue;
        }

        Texture *spriteTex = spriteTextureResolver.resolveTexture(obj);
        if (!spriteTex || spriteTex->GetID() == 0) {
          setLight2DRoutingReason(
              obj.id,
              "Legacy path: no sprite texture is bound for this object.");
          continue;
        }

        std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
        const float angle = uiSceneLookup.getWorldParentRotationRad(obj) +
                            glm::radians(obj.ui.rotation);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        ImVec2 maskMin, maskMax;
        const bool hasMaskRect =
            resolveCanvasMaskRectForObject(obj, maskMin, maskMax);
        auto quadOutsideOverlay = [&](const ImVec2 &quadMin,
                                      const ImVec2 &quadMax) {
          return quadMax.x < overlayPos.x ||
                 quadMin.x > overlayPos.x + overlaySize.x ||
                 quadMax.y < overlayPos.y ||
                 quadMin.y > overlayPos.y + overlaySize.y;
        };
        auto appendSpriteQuad = [&](const ImVec2 &quadMin,
                                    const ImVec2 &quadMax) {
          if (!disableCulling && quadOutsideOverlay(quadMin, quadMax)) {
            return false;
          }
          if (hasMaskRect) {
            const bool maskClipsSprite =
                quadMin.x < maskMin.x || quadMax.x > maskMax.x ||
                quadMin.y < maskMin.y || quadMax.y > maskMax.y;
            if (maskClipsSprite) {
              return false;
            }
          }

          Light2DScreenSprite sprite;
          sprite.objectId = obj.id;
          sprite.layer = obj.layer;
          sprite.drawOrder = drawOrder++;
          sprite.textureId = spriteTex->GetID();
          sprite.tint = obj.ui.color;
          sprite.receiveLighting = obj.ui.receiveLighting2D;
          sprite.unlit = obj.ui.unlitLighting2D;
          sprite.emissiveIntensity = obj.ui.emissiveLighting2D;

          const glm::vec2 center(
              ((quadMin.x + quadMax.x) * 0.5f) - overlayPos.x,
              ((quadMin.y + quadMax.y) * 0.5f) - overlayPos.y);
          const glm::vec2 half(std::max(0.5f, (quadMax.x - quadMin.x) * 0.5f),
                               std::max(0.5f, (quadMax.y - quadMin.y) * 0.5f));
          auto rotatePoint = [&](float x, float y) {
            return glm::vec2(center.x + x * c - y * s,
                             center.y + x * s + y * c);
          };
          sprite.positions[0] = rotatePoint(-half.x, -half.y);
          sprite.positions[1] = rotatePoint(half.x, -half.y);
          sprite.positions[2] = rotatePoint(half.x, half.y);
          sprite.positions[3] = rotatePoint(-half.x, half.y);
          sprite.uvs[0] = glm::vec2(uvQuad[0].x, uvQuad[0].y);
          sprite.uvs[1] = glm::vec2(uvQuad[1].x, uvQuad[1].y);
          sprite.uvs[2] = glm::vec2(uvQuad[2].x, uvQuad[2].y);
          sprite.uvs[3] = glm::vec2(uvQuad[3].x, uvQuad[3].y);
          lightRequest.sprites.push_back(sprite);
          return true;
        };

        bool addedAnySprite = false;
        if (repeatX || repeatY) {
          glm::vec2 spriteSizeWorld = getSpriteDisplaySize(obj);
          glm::vec2 spacing = obj.hasParallaxLayer2D
                                  ? obj.parallaxLayer2D.repeatSpacing
                                  : glm::vec2(0.0f);
          float stepX = spriteSizeWorld.x + spacing.x;
          float stepY = spriteSizeWorld.y + spacing.y;
          ImVec2 pivotOffset = anchorToPivot(
              obj.ui.anchor, ImVec2(spriteSizeWorld.x, spriteSizeWorld.y));
          glm::vec2 parentOffset = uiSceneLookup.getWorldParentOffset(obj);
          glm::vec2 worldPos = parentOffset +
                               glm::vec2(obj.ui.position.x, obj.ui.position.y) +
                               parallaxOffset(obj);
          glm::vec2 baseWorldMin =
              worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
          int startX = repeatX
                           ? static_cast<int>(std::floor(
                                 (worldViewMin.x - baseWorldMin.x) / stepX)) -
                                 1
                           : 0;
          int endX = repeatX ? static_cast<int>(std::ceil(
                                   (worldViewMax.x - baseWorldMin.x) / stepX)) +
                                   1
                             : 0;
          int startY = repeatY
                           ? static_cast<int>(std::floor(
                                 (worldViewMin.y - baseWorldMin.y) / stepY)) -
                                 1
                           : 0;
          int endY = repeatY ? static_cast<int>(std::ceil(
                                   (worldViewMax.y - baseWorldMin.y) / stepY)) +
                                   1
                             : 0;
          for (int ix = startX; ix <= endX; ++ix) {
            for (int iy = startY; iy <= endY; ++iy) {
              float dx = repeatX ? static_cast<float>(ix) * stepX : 0.0f;
              float dy = repeatY ? static_cast<float>(iy) * stepY : 0.0f;
              glm::vec2 tileMin = baseWorldMin + glm::vec2(dx, dy);
              ImVec2 s0 = worldToScreen(tileMin);
              ImVec2 s1 = worldToScreen(
                  tileMin + glm::vec2(spriteSizeWorld.x, spriteSizeWorld.y));
              ImVec2 tileRectMin(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
              ImVec2 tileRectMax(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
              addedAnySprite =
                  appendSpriteQuad(tileRectMin, tileRectMax) || addedAnySprite;
            }
          }
        } else {
          addedAnySprite = appendSpriteQuad(rectMin, rectMax);
        }

        if (!addedAnySprite) {
          setLight2DRoutingReason(
              obj.id,
              hasMaskRect
                  ? "Legacy path: repeating or masked tiles still use legacy "
                    "rendering when the canvas clip cuts the visible tile."
                  : "Skipped Light2D: object has no visible tiles inside the "
                    "current 2D world overlay.");
          continue;
        }

        light2DRenderedObjectIds.insert(obj.id);
        if (obj.ui.type == UIElementType::Sprite2D) {
          if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D) {
            ++litSprite2DCount;
          }
        } else if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D) {
          ++litWorldImageCount;
        }
        if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D) {
          setLight2DRoutingReason(
              obj.id, repeatX || repeatY
                          ? "Lit path: repeating parallax tiles are routed "
                            "through the Light2D compositor."
                          : "Lit path: routed through the Light2D compositor.");
        } else if (obj.ui.unlitLighting2D) {
          setLight2DRoutingReason(obj.id,
                                  "Lit compositor path: object is routed, but "
                                  "Force Unlit is enabled.");
        } else {
          setLight2DRoutingReason(obj.id,
                                  "Lit compositor path: object is routed, but "
                                  "Receive Lighting is disabled.");
        }
      }

      auto particleWorldToRenderLocal = [&](const glm::vec2 &world) {
        ImVec2 screen = worldToScreen(world);
        return glm::vec2(screen.x - overlayPos.x, screen.y - overlayPos.y);
      };
      auto particleParentOffset = [&](const SceneObject &obj) {
        return uiSceneLookup.getWorldParentOffset(obj);
      };
      auto particleRectOutsideOverlay = [&](const ImVec2 &rectMin,
                                            const ImVec2 &rectMax) {
        return rectMax.x < overlayPos.x ||
               rectMin.x > overlayPos.x + overlaySize.x ||
               rectMax.y < overlayPos.y ||
               rectMin.y > overlayPos.y + overlaySize.y;
      };
      if (useWorldUi) {
        AppendParticleSystem2DSprites(sceneObjects, renderer, glfwGetTime(),
                                      worldToScreen, particleWorldToRenderLocal,
                                      particleParentOffset,
                                      particleRectOutsideOverlay, lightRequest,
                                      drawOrder);
      } else {
        auto projectedParticleOutside = [&](const ImVec2 &min,
                                            const ImVec2 &max) {
          return max.x < 0.0f || min.x > overlaySize.x || max.y < 0.0f ||
                 min.y > overlaySize.y;
        };
        AppendProjectedParticleSystem2DSprites(
            sceneObjects, renderer, glfwGetTime(), projectedUiView,
            projectedUiProj, overlayPos, overlaySize, overlayPos,
            projectedParticleOutside, lightRequest, drawOrder);
      }

      for (const SceneObject &obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasLight2D ||
            !obj.light2D.enabled) {
          continue;
        }
        ++activeLight2DCount;

        if (obj.light2D.type == Light2DType::Global) {
          lightRequest.baseAmbient +=
              glm::vec3(obj.light2D.color) * obj.light2D.intensity;
          continue;
        }

        Light2DScreenLight light;
        light.objectId = obj.id;
        light.enabled = obj.light2D.enabled;
        light.type = obj.light2D.type;
        light.blendStyle = obj.light2D.blendStyle;
        light.lightOrder = obj.light2D.lightOrder;
        light.overlapOperation = obj.light2D.overlapOperation;
        light.targetAllLayers = obj.light2D.targetAllLayers;
        light.targetLayerMask = obj.light2D.targetLayerMask;
        light.cullWhenOffscreen = obj.light2D.cullWhenOffscreen;
        light.offscreenCullMarginPx =
            obj.light2D.offscreenCullMargin * uiWorldCamera.zoom;
        light.color = obj.light2D.color;
        light.intensity = obj.light2D.intensity *
                          computeFlickerMultiplier(obj.light2D.flicker);
        light.radius = std::max(obj.light2D.radius, obj.light2D.outerRadius) *
                       uiWorldCamera.zoom;
        light.innerRadius = obj.light2D.innerRadius * uiWorldCamera.zoom;
        light.outerRadius =
            std::max(obj.light2D.innerRadius, obj.light2D.outerRadius) *
            uiWorldCamera.zoom;
        light.falloffStrength = obj.light2D.falloffStrength;
        light.innerSpotAngle = obj.light2D.innerSpotAngle;
        light.outerSpotAngle = obj.light2D.outerSpotAngle;
        light.shadowStrength = obj.light2D.shadowStrength;
        light.volumetricEnabled = obj.light2D.volumetricEnabled;
        light.castsShadows = obj.light2D.castsShadows;
        light.rotationRad = glm::radians(obj.rotation.z);
        light.cookieScale = obj.light2D.cookieScale;
        light.cookieRotationRad = glm::radians(obj.light2D.cookieRotation);
        light.freeformFeatherPx =
            obj.light2D.freeformFeather * uiWorldCamera.zoom;
        light.freeformEdgeFalloff = obj.light2D.freeformEdgeFalloff;

        ImVec2 lightPos =
            worldToScreen(glm::vec2(obj.position.x, obj.position.y));
        light.position =
            glm::vec2(lightPos.x - overlayPos.x, lightPos.y - overlayPos.y);

        if (obj.light2D.type == Light2DType::Freeform ||
            obj.light2D.type == Light2DType::Sprite) {
          const Light2DPolygonCache &polygonCache =
              lighting2DRenderer.updatePolygonCache(obj.id, obj.light2D);
          if (!polygonCache.valid || polygonCache.vertices.empty()) {
            continue;
          }

          ImVec2 screenBoundsMin = worldToScreen(
              glm::vec2(obj.position.x + polygonCache.boundsMin.x,
                        obj.position.y + polygonCache.boundsMin.y));
          ImVec2 screenBoundsMax = worldToScreen(
              glm::vec2(obj.position.x + polygonCache.boundsMax.x,
                        obj.position.y + polygonCache.boundsMax.y));
          light.boundsMin = glm::vec2(std::min(screenBoundsMin.x, screenBoundsMax.x) -
                                          std::max(light.radius, light.outerRadius) -
                                          overlayPos.x,
                                      std::min(screenBoundsMin.y, screenBoundsMax.y) -
                                          std::max(light.radius, light.outerRadius) -
                                          overlayPos.y);
          light.boundsMax = glm::vec2(std::max(screenBoundsMin.x, screenBoundsMax.x) +
                                          std::max(light.radius, light.outerRadius) -
                                          overlayPos.x,
                                      std::max(screenBoundsMin.y, screenBoundsMax.y) +
                                          std::max(light.radius, light.outerRadius) -
                                          overlayPos.y);
          if (obj.light2D.cullWhenOffscreen &&
              lightBoundsOutsideOverlay(light.boundsMin, light.boundsMax,
                                        light.offscreenCullMarginPx)) {
            continue;
          }

          light.polygon.reserve(polygonCache.vertices.size());
          for (const glm::vec2 &point : polygonCache.vertices) {
            ImVec2 screenPoint = worldToScreen(
                glm::vec2(obj.position.x + point.x, obj.position.y + point.y));
            light.polygon.emplace_back(screenPoint.x - overlayPos.x,
                                       screenPoint.y - overlayPos.y);
          }
        } else {
          const float extent = std::max(light.radius, light.outerRadius);
          light.boundsMin = light.position - glm::vec2(extent);
          light.boundsMax = light.position + glm::vec2(extent);
          if (obj.light2D.cullWhenOffscreen &&
              lightBoundsOutsideOverlay(light.boundsMin, light.boundsMax,
                                        light.offscreenCullMarginPx)) {
            continue;
          }
        }

        if (!obj.light2D.cookieTexturePath.empty()) {
          if (Texture *cookieTexture = renderer.getTexture(
                  obj.light2D.cookieTexturePath,
                  MaterialProperties::TextureFilter::Bilinear)) {
            light.cookieTextureId = cookieTexture->GetID();
          }
        }

        lightRequest.lights.push_back(light);
      }

      const bool hasAmbientOnly =
          glm::length(lightRequest.baseAmbient) > 0.0001f;
      const bool wantsPostFxComposite =
          Light2DHasVisiblePostFx(lightRequest.postFx);
      lightBufferHadContent = hasAmbientOnly || !lightRequest.lights.empty();
      if (!lightRequest.sprites.empty() &&
          (hasAmbientOnly || !lightRequest.lights.empty() ||
           wantsPostFxComposite)) {
        unsigned int lightTexture =
            lighting2DRenderer.render(lightRequest, renderer);
        if (lightTexture != 0) {
          unsigned int presentedTexture = lightTexture;
          ImGui::GetWindowDrawList()->AddImage(
              (ImTextureID)(intptr_t)presentedTexture, overlayPos,
              ImVec2(overlayPos.x + overlaySize.x,
                     overlayPos.y + overlaySize.y),
              ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
          renderedLight2DComposite = true;
          light2DStats = lighting2DRenderer.getLastStats();
        } else {
          for (int objectId : light2DRenderedObjectIds) {
            setLight2DRoutingReason(
                objectId, "Legacy path: Light2D compositor did not produce a "
                          "valid output texture this frame.");
          }
          light2DRenderedObjectIds.clear();
        }
      } else {
        for (int objectId : light2DRenderedObjectIds) {
          setLight2DRoutingReason(
              objectId,
              wantsPostFxComposite
                  ? "Legacy path: the Light2D compositor was expected to run "
                    "for 2D post FX, but no output was requested."
                  : "Legacy path: no active Light2D or Global Light2D affected "
                    "this frame.");
        }
        light2DRenderedObjectIds.clear();
      }
    }

    const ImVec2 postFxOverlayMin = overlayPos;
    const ImVec2 postFxOverlayMax(overlayPos.x + overlaySize.x,
                                  overlayPos.y + overlaySize.y);
    const Renderer::PostFX2DScope postFx2DScope =
        renderer.resolvePostFX2DScope(projectedUiCamera, sceneObjects);
    bool postFx2DBandOpen = false;
    bool postFx2DScopeResolved = false;
    auto emitPostFx2DScopeBoundary = [&](int sortOrder) {
      if (!postFx2DScope.active || postFx2DScopeResolved) {
        return;
      }
      ImDrawList *dl = ImGui::GetWindowDrawList();
      if (postFx2DScope.mode == PostFX2DScopeMode::Band) {
        if (!postFx2DBandOpen) {
          if (sortOrder >= postFx2DScope.minOrder &&
              sortOrder <= postFx2DScope.maxOrder) {
            spriteBatch.flush();
            queueOverlayPostFx(dl, projectedUiCamera, postFxOverlayMin,
                               postFxOverlayMax, false,
                               OverlayPostFxKind::BandBegin);
            postFx2DBandOpen = true;
          }
          return;
        }
        if (sortOrder > postFx2DScope.maxOrder) {
          spriteBatch.flush();
          queueOverlayPostFx(dl, projectedUiCamera, postFxOverlayMin,
                             postFxOverlayMax, false,
                             OverlayPostFxKind::BandEnd);
          postFx2DBandOpen = false;
          postFx2DScopeResolved = true;
        }
        return;
      }
      if (sortOrder > postFx2DScope.maxOrder) {
        spriteBatch.flush();
        queueOverlayPostFx(dl, projectedUiCamera, postFxOverlayMin,
                           postFxOverlayMax, false,
                           OverlayPostFxKind::FullRegion);
        postFx2DScopeResolved = true;
      }
    };

    for (SceneObject *objPtr : uiDrawList) {
      SceneObject &obj = *objPtr;
      emitPostFx2DScopeBoundary(RuntimeUiSortOrderOf(obj));
      if (ViewportRenderHelpers::IsAssemblageLayerDrawable(obj)) {
        if (useWorldUi && light2DRenderedObjectIds.count(obj.id) == 0) {
          assemblageTileQuadsLastFrame += ViewportRenderHelpers::AppendAssemblageLayerQuads(
              sceneObjects, obj, assemblageRuntime, renderer, glfwGetTime(), worldViewMin,
              worldViewMax, worldToScreen,
              [&](const ViewportRenderHelpers::AssemblageScreenQuad &quad) {
                spriteBatch.push(quad.textureId, quad.pos[0], quad.pos[1], quad.pos[2],
                                 quad.pos[3], quad.uv[0], quad.uv[1], quad.uv[2], quad.uv[3],
                                 ImGui::GetColorU32(ImVec4(quad.tint.r, quad.tint.g,
                                                           quad.tint.b, quad.tint.a)));
              });
        }
        continue;
      }
      ImVec2 rectMin, rectMax;
      if (useWorldUi || obj.type == ObjectType::Sprite25D) {
        if (!resolveCachedUiWorldRect(obj, rectMin, rectMax))
          continue;
      } else {
        resolveUIRect(obj, rectMin, rectMax);
      }
      ImVec2 rectSize(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
      if (rectSize.x <= 1.0f || rectSize.y <= 1.0f)
        continue;
      ++runtimeVisibleObjectCount;

      ImGuiStyle savedStyle;
      bool styleApplied = false;
      bool fontApplied = false;
      if (!obj.ui.stylePreset.empty()) {
        if (const auto *preset = getUIStylePreset(obj.ui.stylePreset)) {
          savedStyle = ImGui::GetStyle();
          ImGui::GetStyle() = preset->style;
          styleApplied = true;
          if (ImFont *presetFont =
                  getUIFontForContext(preset->fontAsset, ImGui::GetCurrentContext())) {
            ImGui::PushFont(presetFont, preset->style.FontSizeBase);
            fontApplied = true;
          }
        }
      }

      if (obj.ui.type == UIElementType::Canvas) {
        spriteBatch.flush();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 edgeColor = obj.ui.maskChildren
                                    ? IM_COL32(74, 228, 255, 225)
                                    : IM_COL32(110, 170, 255, 140);
        const float thickness = obj.ui.maskChildren ? 2.4f : 1.5f;
        dl->AddRect(rectMin, rectMax, edgeColor, 6.0f, 0, thickness);
        if (obj.ui.maskChildren) {
          const float inset = 2.0f;
          if ((rectMax.x - rectMin.x) > inset * 2.0f &&
              (rectMax.y - rectMin.y) > inset * 2.0f) {
            dl->AddRect(ImVec2(rectMin.x + inset, rectMin.y + inset),
                        ImVec2(rectMax.x - inset, rectMax.y - inset),
                        IM_COL32(32, 190, 230, 175), 5.0f, 0, 1.0f);
          }
        }
        if (fontApplied)
          ImGui::PopFont();
        if (styleApplied)
          ImGui::GetStyle() = savedStyle;
        continue;
      }

      ImVec2 drawMin = rectMin;
      ImVec2 drawMax = rectMax;
      ImVec2 drawSize(drawMax.x - drawMin.x, drawMax.y - drawMin.y);
      ImVec2 localMin(drawMin.x - overlayPos.x, drawMin.y - overlayPos.y);
      bool pushedCanvasMask = false;
      if (obj.ui.type != UIElementType::Canvas) {
        ImVec2 maskMin, maskMax;
        if (resolveCanvasMaskRectForObject(obj, maskMin, maskMax)) {
          maskMin.x = std::max(maskMin.x, overlayPos.x);
          maskMin.y = std::max(maskMin.y, overlayPos.y);
          maskMax.x = std::min(maskMax.x, overlayPos.x + overlaySize.x);
          maskMax.y = std::min(maskMax.y, overlayPos.y + overlaySize.y);
          if (maskMax.x <= maskMin.x || maskMax.y <= maskMin.y) {
            if (fontApplied)
              ImGui::PopFont();
            if (styleApplied)
              ImGui::GetStyle() = savedStyle;
            continue;
          }
          if (drawMax.x <= maskMin.x || drawMin.x >= maskMax.x ||
              drawMax.y <= maskMin.y || drawMin.y >= maskMax.y) {
            if (fontApplied)
              ImGui::PopFont();
            if (styleApplied)
              ImGui::GetStyle() = savedStyle;
            continue;
          }
          spriteBatch.flush();
          ImGui::PushClipRect(maskMin, maskMax, true);
          pushedCanvasMask = true;
        }
      }

      ImGui::PushID(obj.id);
      UIAnimationState &animState = uiAnimationStates[obj.id];
      if (!animState.initialized) {
        animState.sliderValue = obj.ui.sliderValue;
        animState.initialized = true;
      }
      if (obj.ui.backdropBlur > 0.001f) {
        spriteBatch.flush();
        EmitRuntimeUiBackdropBlur(ImGui::GetWindowDrawList(), obj, drawMin, drawMax);
      }
      if (obj.ui.type == UIElementType::Image ||
          obj.ui.type == UIElementType::Sprite2D) {
        if (light2DRenderedObjectIds.find(obj.id) !=
            light2DRenderedObjectIds.end()) {
          if (pushedCanvasMask) {
            ImGui::PopClipRect();
          }
          ImGui::PopID();
          if (fontApplied)
            ImGui::PopFont();
          if (styleApplied)
            ImGui::GetStyle() = savedStyle;
          continue;
        }
        Texture *spriteTex = spriteTextureResolver.resolveTexture(obj);
        unsigned int texId = (spriteTex != nullptr) ? spriteTex->GetID() : 0;
        std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
        const int frame = resolveSpriteSheetFrame(obj);
        const ImVec2 sourceFrameSizePx =
            ResolveUiSourceFrameSizePx(obj, frame, spriteTex);
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        const ImU32 tintColor = ImGui::GetColorU32(tint);
        float angle = uiSceneLookup.getWorldParentRotationRad(obj) +
                      glm::radians(obj.ui.rotation);
        if (DrawScrollingUiSprite(spriteBatch, (ImTextureID)(intptr_t)texId,
                                  obj, drawMin, drawMax, angle, tintColor,
                                  ImGui::GetTime())) {
        } else if (DrawNineSliceSprite(spriteBatch, (ImTextureID)(intptr_t)texId, obj,
                                drawMin, drawMax, uvQuad, sourceFrameSizePx,
                                angle, tintColor)) {
        } else if (std::abs(angle) > 1e-4f) {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImVec2 center = ImVec2((drawMin.x + drawMax.x) * 0.5f,
                                 (drawMin.y + drawMax.y) * 0.5f);
          ImVec2 half = ImVec2(drawSize.x * 0.5f, drawSize.y * 0.5f);
          ImVec2 p0, p1, p2, p3;
          ViewportRenderHelpers::UiRotatedCornersAboutAnchor(
              obj.ui.anchor, center, half, angle, p0, p1, p2, p3);
          if (texId != 0) {
            spriteBatch.push((ImTextureID)(intptr_t)texId, p0, p1, p2, p3,
                             uvQuad[0], uvQuad[1], uvQuad[2], uvQuad[3],
                             tintColor);
          } else {
            spriteBatch.flush();
            ImU32 fill = tintColor;
            ImU32 border = ImGui::GetColorU32(brighten(tint, 0.85f));
            dl->AddQuadFilled(p0, p1, p2, p3, fill);
            dl->AddQuad(p0, p1, p2, p3, border, 2.0f);
            ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
            ImVec2 textPos(center.x - textSize.x * 0.5f,
                           center.y - textSize.y * 0.5f);
            dl->AddText(textPos, IM_COL32(210, 210, 220, 220),
                        obj.ui.label.c_str());
          }
        } else {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          if (texId != 0) {
            spriteBatch.push((ImTextureID)(intptr_t)texId, drawMin,
                             ImVec2(drawMax.x, drawMin.y), drawMax,
                             ImVec2(drawMin.x, drawMax.y), uvQuad[0],
                             ImVec2(uvQuad[2].x, uvQuad[0].y), uvQuad[2],
                             ImVec2(uvQuad[0].x, uvQuad[2].y), tintColor);
          } else {
            spriteBatch.flush();
            ImU32 fill = tintColor;
            ImU32 border = ImGui::GetColorU32(brighten(tint, 0.85f));
            dl->AddRectFilled(drawMin, drawMax, fill, 6.0f);
            dl->AddRect(drawMin, drawMax, border, 6.0f);
            ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
            ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                           drawMin.y + (drawSize.y - textSize.y) * 0.5f);
            dl->AddText(textPos, IM_COL32(210, 210, 220, 220),
                        obj.ui.label.c_str());
            ImGui::Dummy(drawSize);
          }
        }
      } else if (obj.ui.type == UIElementType::Slider) {
        spriteBatch.flush();
        const int sliderVtxStart = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const float sliderRotationRad =
            uiSceneLookup.getWorldParentRotationRad(obj) +
            glm::radians(obj.ui.rotation);
        ImGui::SetCursorPos(localMin);
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        const ImU32 s4SliderBg     = (obj.ui.backgroundColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.backgroundColor.r, obj.ui.backgroundColor.g, obj.ui.backgroundColor.b, obj.ui.backgroundColor.a)) : ImGui::GetColorU32(ImVec4(tint.x * 0.2f, tint.y * 0.2f, tint.z * 0.2f, tint.w * 0.6f));
        const ImU32 s4SliderFill   = (obj.ui.fillColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.fillColor.r, obj.ui.fillColor.g, obj.ui.fillColor.b, obj.ui.fillColor.a))             : ImGui::GetColorU32(tint);
        const ImU32 s4SliderBorder = (obj.ui.borderColor.a > 0.0f)     ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a))     : ImGui::GetColorU32(brighten(tint, 0.85f));
        const ImU32 s4SliderText   = (obj.ui.textColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a))             : IM_COL32(240, 240, 245, 220);
        if (obj.ui.sliderStyle == UISliderStyle::ImGui) {
          float minValue = obj.ui.sliderMin;
          float maxValue = obj.ui.sliderMax;
          float range = (maxValue - minValue);
          if (range <= 1e-6f)
            range = 1.0f;
          ImDrawList *dl = ImGui::GetWindowDrawList();
          const bool sliderInteractive = obj.ui.interactable && !uiWorldCameraActive;
          if (sliderInteractive) {
            ImGui::InvisibleButton("##UISliderImGui", drawSize);
            if (ImGui::IsItemActive() &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left) && drawSize.x > 1.0f) {
              float mouseT = (ImGui::GetIO().MousePos.x - drawMin.x) / drawSize.x;
              mouseT = std::clamp(mouseT, 0.0f, 1.0f);
              float newValue = minValue + mouseT * range;
              if (newValue != obj.ui.sliderValue) {
                obj.ui.sliderValue = newValue;
                projectManager.currentProject.hasUnsavedChanges = true;
              }
            }
          }
          float t = (obj.ui.sliderValue - minValue) / range;
          t = std::clamp(t, 0.0f, 1.0f);
          float rounding = std::min(drawSize.x, drawSize.y) * 0.5f;
          ImVec2 fillMax(drawMin.x + drawSize.x * t, drawMax.y);
          dl->AddRectFilled(drawMin, drawMax, s4SliderBg, rounding);
          if (fillMax.x > drawMin.x) {
            dl->AddRectFilled(drawMin, fillMax, s4SliderFill, rounding);
          }
          dl->AddRect(drawMin, drawMax, s4SliderBorder, rounding);
          ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
          ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                         drawMin.y + (drawSize.y - textSize.y) * 0.5f);
          dl->AddText(textPos, s4SliderText, obj.ui.label.c_str());
        } else {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 bg     = s4SliderBg;
          ImU32 fill   = s4SliderFill;
          ImU32 border = s4SliderBorder;
          float minValue = obj.ui.sliderMin;
          float maxValue = obj.ui.sliderMax;
          float range = (maxValue - minValue);
          if (range <= 1e-6f)
            range = 1.0f;
          ImGui::BeginDisabled(!obj.ui.interactable || uiWorldCameraActive);
          ImGui::InvisibleButton("##UISlider", drawSize);
          bool held = obj.ui.interactable && ImGui::IsItemActive();
          if (held && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
              drawSize.x > 1.0f) {
            float mouseT = (ImGui::GetIO().MousePos.x - drawMin.x) / drawSize.x;
            mouseT = std::clamp(mouseT, 0.0f, 1.0f);
            float newValue = minValue + mouseT * range;
            if (newValue != obj.ui.sliderValue) {
              obj.ui.sliderValue = newValue;
              projectManager.currentProject.hasUnsavedChanges = true;
            }
          }
          ImGui::EndDisabled();

          animateValue(animState.sliderValue, obj.ui.sliderValue, held);
          float displayValue = (uiAnimationMode == UIAnimationMode::Off)
                                   ? obj.ui.sliderValue
                                   : animState.sliderValue;
          float t = (displayValue - minValue) / range;
          t = std::clamp(t, 0.0f, 1.0f);

          ViewportRenderHelpers::RenderUISliderStyle(
              dl, obj.ui.sliderStyle, drawMin, drawMax, drawSize, bg, fill,
              border, s4SliderText, t, minValue, maxValue,
              obj.ui.label.c_str());
        }
        if (std::abs(sliderRotationRad) > 1e-4f) {
          const ImVec2 sliderPivotOffset = ViewportRenderHelpers::UiAnchorPivot(
              obj.ui.anchor,
              ImVec2(drawMax.x - drawMin.x, drawMax.y - drawMin.y));
          const ImVec2 sliderPivot(drawMin.x + sliderPivotOffset.x,
                                   drawMin.y + sliderPivotOffset.y);
          ViewportRenderHelpers::RotateDrawListVertices(
              ImGui::GetWindowDrawList(), sliderVtxStart,
              ImGui::GetWindowDrawList()->VtxBuffer.Size, sliderPivot,
              sliderRotationRad);
        }
      } else if (obj.ui.type == UIElementType::Button) {
        spriteBatch.flush();
        ImGui::SetCursorPos(localMin);
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        obj.ui.buttonPressed = false;
        obj.ui.uiHovered = false;
        obj.ui.uiActive = false;
        const ImU32 s4BtnText = (obj.ui.textColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a)) : IM_COL32(240, 240, 245, 220);
        if (obj.ui.buttonStyle == UIButtonStyle::ImGui) {
          ImGui::PushStyleColor(ImGuiCol_Button, tint);
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighten(tint, 1.1f));
          ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighten(tint, 1.2f));
          ImGui::BeginDisabled(!obj.ui.interactable || uiWorldCameraActive);
          obj.ui.buttonPressed = ImGui::Button(obj.ui.label.c_str(), drawSize);
          obj.ui.uiHovered = ImGui::IsItemHovered();
          obj.ui.uiActive  = ImGui::IsItemActive();
          ImGui::EndDisabled();
          ImGui::PopStyleColor(3);
        } else if (obj.ui.buttonStyle == UIButtonStyle::Outline) {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 border = (obj.ui.borderColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a)) : ImGui::GetColorU32(tint);
          ImGui::BeginDisabled(!obj.ui.interactable || uiWorldCameraActive);
          if (ImGui::InvisibleButton("##UIButton", drawSize)) {
            obj.ui.buttonPressed = obj.ui.interactable;
          }
          bool hovered = ImGui::IsItemHovered();
          bool active = ImGui::IsItemActive();
          obj.ui.uiHovered = hovered;
          obj.ui.uiActive  = active;
          ImGui::EndDisabled();
          float hoverT =
              animateValue(animState.hover, hovered ? 1.0f : 0.0f, false);
          float activeT =
              animateValue(animState.active, active ? 1.0f : 0.0f, false);
          if (hoverT > 0.001f) {
            ImVec4 hoverCol = brighten(tint, 0.45f);
            hoverCol.w *= std::clamp(hoverT, 0.0f, 1.0f);
            dl->AddRectFilled(drawMin, drawMax, ImGui::GetColorU32(hoverCol),
                              6.0f);
          }
          if (activeT > 0.001f) {
            ImVec4 activeCol = brighten(tint, 0.65f);
            activeCol.w *= std::clamp(activeT, 0.0f, 1.0f);
            dl->AddRectFilled(drawMin, drawMax, ImGui::GetColorU32(activeCol),
                              6.0f);
          }
          dl->AddRect(drawMin, drawMax, border, 6.0f, 0, 2.0f);
          ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
          ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                         drawMin.y + (drawSize.y - textSize.y) * 0.5f);
          dl->AddText(textPos, s4BtnText, obj.ui.label.c_str());
        }
      } else if (obj.ui.type == UIElementType::Text) {
        spriteBatch.flush();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        ImFont *textFont =
            obj.ui.textFont.empty()
                ? ImGui::GetFont()
                : getUIFontForContext(obj.ui.textFont, ImGui::GetCurrentContext());
        if (!textFont) {
          textFont = ImGui::GetFont();
        }
        float scale = std::max(0.1f, obj.ui.textScale);
        float fontSize = ResolveViewportUITextFontSize(
            ImGui::GetFontSize(), scale, obj.ui.fontSize, useWorldUi,
            uiWorldCamera.zoom);
        const float textRotationRad =
            uiSceneLookup.getWorldParentRotationRad(obj) +
            glm::radians(obj.ui.rotation);
        const bool textIsRotated = std::abs(textRotationRad) > 1e-4f;
        if (!textIsRotated) {
          ImGui::PushClipRect(drawMin, drawMax, true);
        }
        const int textVtxStart = dl->VtxBuffer.Size;
        AddUITextWithFilter(dl, obj.material.textureFilter, textFont,
                            fontSize, drawMin, drawMax,
                            ImGui::GetColorU32(tint), obj.ui.label.c_str(),
                            obj.ui.textAutoWrap, obj.ui.textHAlign,
                            obj.ui.textVAlign, obj.ui.textEffectFlags,
                            obj.ui.textEffectSpeed, obj.ui.textEffectIntensity,
                            0.0f,
                            ViewportRenderHelpers::UITextRevealParams{
                                obj.ui.textRevealChars,
                                obj.ui.textRevealPopScale,
                                obj.ui.textRevealSoftness});
        if (textIsRotated) {
          const ImVec2 textPivotOffset = ViewportRenderHelpers::UiAnchorPivot(
              obj.ui.anchor, ImVec2(drawMax.x - drawMin.x, drawMax.y - drawMin.y));
          const ImVec2 textPivot(drawMin.x + textPivotOffset.x,
                                 drawMin.y + textPivotOffset.y);
          ViewportRenderHelpers::RotateDrawListVertices(
              dl, textVtxStart, dl->VtxBuffer.Size, textPivot,
              textRotationRad);
        } else {
          ImGui::PopClipRect();
        }
      }
      if (pushedCanvasMask) {
        spriteBatch.flush();
        ImGui::PopClipRect();
      }
      ImGui::PopID();
      if (fontApplied)
        ImGui::PopFont();
      if (styleApplied)
        ImGui::GetStyle() = savedStyle;
    }
    spriteBatch.flush();
    if (postFx2DScope.active && !postFx2DScopeResolved) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      if (postFx2DScope.mode == PostFX2DScopeMode::Band) {
        if (postFx2DBandOpen) {
          queueOverlayPostFx(dl, projectedUiCamera, postFxOverlayMin,
                             postFxOverlayMax, false,
                             OverlayPostFxKind::BandEnd);
          postFx2DBandOpen = false;
        }
      } else {
        queueOverlayPostFx(dl, projectedUiCamera, postFxOverlayMin,
                           postFxOverlayMax, false,
                           OverlayPostFxKind::FullRegion);
      }
      postFx2DScopeResolved = true;
    }
    if (useWorldUi) {
      light2DCompositorRanLastFrame = renderedLight2DComposite;
      light2DLightBufferHadContentLastFrame = lightBufferHadContent;
      light2DActiveCountLastFrame = activeLight2DCount;
      light2DLitSprite2DCountLastFrame = litSprite2DCount;
      light2DLitWorldImageCountLastFrame = litWorldImageCount;
      runtime2DProfilerStats.light2DBuildMs = light2DStats.cpuBuildMs;
      runtime2DProfilerStats.light2DVisibleLights = light2DStats.visibleLights;
      runtime2DProfilerStats.light2DVisibleSprites =
          light2DStats.visibleSprites;
      if (captureLight2DRoutingReasons) {
        light2DObjectRoutingReasonsLastFrame = std::move(light2DRoutingReasons);
      }
    }

    bool pseudoPanelInteracting = false;
    struct PseudoPanelDrawEntry {
      int canvasId = -1;
      unsigned int textureId = 0;
      ImVec2 layoutSize = ImVec2(1.0f, 1.0f);
      std::array<ImVec2, 4> corners;
      int depthSort = 0;
      float distance = 0.0f;
      bool allowInteraction = false;
    };
    std::vector<PseudoPanelDrawEntry> pseudoPanels;
    pseudoPanels.reserve(sceneObjects.size());

    auto resolvePseudoAnchorScreen = [&](const SceneObject &canvas,
                                         ImVec2 &outScreen,
                                         float &outDistance) -> bool {
      outDistance = 1.0f;
      if (canvas.ui.pseudo3DAnchorTargetId < 0) {
        return false;
      }
      const SceneObject *anchorObj =
          uiSceneLookup.find(canvas.ui.pseudo3DAnchorTargetId);
      if (!anchorObj) {
        return false;
      }
      if (useWorldUi) {
        outScreen = worldToScreen(
            glm::vec2(anchorObj->position.x, anchorObj->position.y));
        outDistance = glm::length(
            glm::vec2(uiWorldCamera.position.x - anchorObj->position.x,
                      uiWorldCamera.position.y - anchorObj->position.y));
        return true;
      }
      if (ProjectWorldToOverlayPoint(anchorObj->position, projectedUiView,
                                     projectedUiProj, overlayPos, overlaySize,
                                     outScreen)) {
        outDistance = glm::length(camera.position - anchorObj->position);
        return true;
      }
      return false;
    };
    auto resolvePseudoCanvasRect = [&](const SceneObject &canvas,
                                       const glm::vec2 &layoutSizePx,
                                       const ImVec2 &baseRegionMin,
                                       const ImVec2 &baseRegionMax,
                                       float scaleX, float scaleY,
                                       ImVec2 &outMin, ImVec2 &outMax) -> bool {
      std::vector<const SceneObject *> chain;
      chain.reserve(8);
      const SceneObject *current = &canvas;
      while (current) {
        if (current->hasUI && current->ui.type != UIElementType::None) {
          const int canvas3DId = find3DCanvasId(*current);
          if (canvas3DId < 0 || current->id == canvas.id) {
            chain.push_back(current);
          }
        }
        if (current->parentId < 0)
          break;
        current = uiSceneLookup.find(current->parentId);
        if (!current)
          break;
      }
      if (chain.empty()) {
        return false;
      }
      std::reverse(chain.begin(), chain.end());

      ImVec2 regionMin = baseRegionMin;
      ImVec2 regionMax = baseRegionMax;
      for (const SceneObject *node : chain) {
        ImVec2 size(1.0f, 1.0f);
        if (node->id == canvas.id) {
          size = ImVec2(std::max(1.0f, layoutSizePx.x * scaleX),
                        std::max(1.0f, layoutSizePx.y * scaleY));
        } else {
          const glm::vec2 nodeSize = getSpriteDisplaySize(*node);
          size = ImVec2(std::max(1.0f, nodeSize.x * scaleX),
                        std::max(1.0f, nodeSize.y * scaleY));
        }
        const ImVec2 anchorPoint =
            anchorToPoint(node->ui.anchor, regionMin, regionMax);
        const ImVec2 pivot(anchorPoint.x + node->ui.position.x * scaleX,
                           anchorPoint.y + node->ui.position.y * scaleY);
        const ImVec2 pivotOffset = anchorToPivot(node->ui.anchor, size);
        regionMin = ImVec2(pivot.x - pivotOffset.x, pivot.y - pivotOffset.y);
        regionMax = ImVec2(regionMin.x + size.x, regionMin.y + size.y);
      }
      outMin = regionMin;
      outMax = regionMax;
      return true;
    };

    for (auto &canvas : sceneObjects) {
      if (!IsObjectEnabledInHierarchy(canvas) || !canvas.hasUI ||
          canvas.ui.type != UIElementType::Canvas || canvas.ui.renderIn3D ||
          !canvas.ui.pseudo3DEnabled ||
          !canvas.ui.pseudo3DUseOffscreenSurface) {
        continue;
      }

      const glm::vec2 layoutSizePx = ResolvePseudo3DLayoutSize(canvas);
      ImVec2 rectMin;
      ImVec2 rectMax;
      const int targetWidth = std::clamp((canvas.ui.renderTargetSize.x > 0)
                                             ? canvas.ui.renderTargetSize.x
                                             : static_cast<int>(layoutSizePx.x),
                                         16, 4096);
      const int targetHeight = std::clamp(
          (canvas.ui.renderTargetSize.y > 0) ? canvas.ui.renderTargetSize.y
                                             : static_cast<int>(layoutSizePx.y),
          16, 4096);
	      Renderer::UiTargetInfo target =
	          renderer.ensureUiTarget(canvas.id, targetWidth, targetHeight,
	                                  canvas.layer,
	                                  canvas.ui.renderTargetFilter, false);
      if (target.texture == 0) {
        continue;
      }

      float distance = 1.0f;
      ImVec2 anchorScreen(0.0f, 0.0f);
      const bool anchored =
          resolvePseudoAnchorScreen(canvas, anchorScreen, distance);
      const bool stablePanelSpace = !useWorldUi && !anchored;
      if (!anchored) {
        if (useWorldUi) {
          distance = glm::length(
              glm::vec2(uiWorldCamera.position.x - canvas.position.x,
                        uiWorldCamera.position.y - canvas.position.y));
        } else {
          distance = glm::length(camera.position - canvas.position);
        }
      }

      const ImVec2 pseudoRegionMin =
          stablePanelSpace ? panelMin : overlayPos;
      const ImVec2 pseudoRegionMax =
          stablePanelSpace
              ? ImVec2(panelMin.x + availableSize.x, panelMin.y + availableSize.y)
              : ImVec2(overlayPos.x + overlaySize.x,
                       overlayPos.y + overlaySize.y);
      const float pseudoScaleX = stablePanelSpace ? 1.0f : uiScaleX;
      const float pseudoScaleY = stablePanelSpace ? 1.0f : uiScaleY;
      if (!resolvePseudoCanvasRect(canvas, layoutSizePx, pseudoRegionMin,
                                   pseudoRegionMax, pseudoScaleX,
                                   pseudoScaleY, rectMin, rectMax)) {
        continue;
      }

      if (anchored) {
        const ImVec2 center((rectMin.x + rectMax.x) * 0.5f,
                            (rectMin.y + rectMax.y) * 0.5f);
        const ImVec2 shift(anchorScreen.x - center.x,
                           anchorScreen.y - center.y);
        rectMin = ImVec2(rectMin.x + shift.x, rectMin.y + shift.y);
        rectMax = ImVec2(rectMax.x + shift.x, rectMax.y + shift.y);
      }

      float distanceScale = 1.0f;
      float perspectiveFactor = 1.0f;
      bool allowInteraction = false;
      ResolvePseudo3DDistanceState(canvas.ui, distance, distanceScale,
                                   perspectiveFactor, allowInteraction);

      PseudoPanelDrawEntry entry;
      entry.canvasId = canvas.id;
      entry.textureId = target.texture;
      entry.layoutSize = ImVec2(layoutSizePx.x, layoutSizePx.y);
      entry.corners = BuildPseudo3DPanelCorners(
          rectMin, rectMax, canvas.ui, distanceScale, perspectiveFactor);
      entry.depthSort = canvas.ui.pseudo3DDepthSort;
      entry.distance = distance;
      entry.allowInteraction = allowInteraction;
      pseudoPanels.push_back(entry);
    }

    if (!pseudoPanels.empty()) {
      std::stable_sort(
          pseudoPanels.begin(), pseudoPanels.end(),
          [](const PseudoPanelDrawEntry &a, const PseudoPanelDrawEntry &b) {
            if (std::abs(a.distance - b.distance) > 0.001f)
              return a.distance > b.distance;
            if (a.depthSort != b.depthSort)
              return a.depthSort < b.depthSort;
            return a.canvasId < b.canvasId;
          });

      ImDrawList *panelDrawList = ImGui::GetWindowDrawList();
      for (const PseudoPanelDrawEntry &panel : pseudoPanels) {
        panelDrawList->AddImageQuad(
            (ImTextureID)(intptr_t)panel.textureId, panel.corners[0],
            panel.corners[1], panel.corners[2], panel.corners[3],
            ImVec2(0.0f, 1.0f), ImVec2(1.0f, 1.0f), ImVec2(1.0f, 0.0f),
            ImVec2(0.0f, 0.0f), IM_COL32_WHITE);
      }

      if (imageHovered && !uiWorldCameraActive) {
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool inputAssigned = false;
        for (auto it = pseudoPanels.rbegin(); it != pseudoPanels.rend(); ++it) {
          ImVec2 uv(0.0f, 0.0f);
          if (!MapPointToPseudo3DQuadUV(it->corners, mousePos, uv)) {
            continue;
          }

          pseudoPanelInteracting =
              pseudoPanelInteracting ||
              ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
              ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
              std::abs(ImGui::GetIO().MouseWheel) > 0.0f;
          if (!it->allowInteraction || inputAssigned) {
            continue;
          }

          UiCanvas3DInput &input = uiCanvas3DInputs[it->canvasId];
          const float u = std::clamp(uv.x, 0.0f, 1.0f);
          const float v = std::clamp(uv.y, 0.0f, 1.0f);
          input.mousePos =
              ImVec2(u * std::max(1.0f, it->layoutSize.x),
                     (1.0f - v) * std::max(1.0f, it->layoutSize.y));
          input.mouseDown[0] = ImGui::GetIO().MouseDown[0];
          input.mouseDown[1] = ImGui::GetIO().MouseDown[1];
          input.mouseDown[2] = ImGui::GetIO().MouseDown[2];
          input.mouseWheel = ImGui::GetIO().MouseWheel;
          input.hasInput = true;
          input.hitT = -1000.0f - static_cast<float>(it->depthSort);
          inputAssigned = true;
        }
      }
    }
    runtimeSpriteBatchBuildMs +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - spriteBatchBuildStart)
            .count();

    uiInteracting = ImGui::IsAnyItemActive() || uiWorldCameraActive ||
                    pseudoPanelInteracting;
    if (!postFx2DScope.active) {
      queueOverlayPostFx(ImGui::GetWindowDrawList(), projectedUiCamera,
                         overlayPos,
                         ImVec2(overlayPos.x + overlaySize.x,
                                overlayPos.y + overlaySize.y),
                         true);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (showingStartupSplash) {
      ImDrawList *splashDraw = ImGui::GetWindowDrawList();
      splashDraw->AddRectFilled(imageMin, imageMax, IM_COL32(0, 0, 0, 230));

      fs::path splashPath = resolveSplashImagePath();
      Texture *splashTex = nullptr;
      if (!splashPath.empty() && fs::exists(splashPath)) {
        splashTex = renderer.getTexture(splashPath.string());
      }
      if (splashTex) {
        float availW = imageMax.x - imageMin.x;
        float availH = imageMax.y - imageMin.y;
        float texW = static_cast<float>(std::max(1, splashTex->GetWidth()));
        float texH = static_cast<float>(std::max(1, splashTex->GetHeight()));
        float scale = std::min(availW / texW, availH / texH);
        scale = std::min(scale, 1.0f);
        ImVec2 drawSize(texW * scale, texH * scale);
        ImVec2 drawMin(imageMin.x + (availW - drawSize.x) * 0.5f,
                       imageMin.y + (availH - drawSize.y) * 0.5f);
        ImVec2 drawMax(drawMin.x + drawSize.x, drawMin.y + drawSize.y);
        splashDraw->AddImage((ImTextureID)(intptr_t)splashTex->GetID(), drawMin,
                             drawMax, ImVec2(0, 0), ImVec2(1, 1),
                             IM_COL32(255, 255, 255, 255));
      } else {
        const char *fallback = "Loading...";
        ImVec2 textSize = ImGui::CalcTextSize(fallback);
        ImVec2 textPos((imageMin.x + imageMax.x) * 0.5f - textSize.x * 0.5f,
                       (imageMin.y + imageMax.y) * 0.5f - textSize.y * 0.5f);
        splashDraw->AddText(textPos, IM_COL32(240, 240, 240, 255), fallback);
      }

      std::string splashTitle = buildSettings.buildName;
      if (splashTitle.empty())
        splashTitle = "Game";
      if (!buildSettings.version.empty())
        splashTitle += " " + buildSettings.version;
      ImVec2 titleSize = ImGui::CalcTextSize(splashTitle.c_str());
      splashDraw->AddText(
          ImVec2((imageMin.x + imageMax.x) * 0.5f - titleSize.x * 0.5f,
                 imageMax.y - titleSize.y - 32.0f),
          IM_COL32(240, 240, 240, 230), splashTitle.c_str());
    }

    if (showingStartupSplash && gameViewCursorLocked) {
      gameViewCursorLocked = false;
    }
    bool clicked = imageHovered && isPlaying && !showingStartupSplash &&
                   ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                   !uiInteracting;
    if (clicked && !gameViewCursorLocked) {
      gameViewCursorLocked = true;
    }
    if (gameViewCursorLocked &&
        (!isPlaying || ImGui::IsKeyPressed(ImGuiKey_Escape))) {
      gameViewCursorLocked = false;
    }
    gameViewportFocused = windowFocused || gameViewCursorLocked;
  }

  const auto runtimeUiEnd = std::chrono::steady_clock::now();
  runtime2DProfilerStats.uiRuntimeMs =
      std::chrono::duration<double, std::milli>(runtimeUiEnd - runtimeUiStart)
          .count();
  runtime2DProfilerStats.spriteBatchBuildMs = runtimeSpriteBatchBuildMs;
  runtime2DProfilerStats.visibleObjectCount = runtimeVisibleObjectCount;
  SubmitRuntime2DWorldProfilerStats(runtime2DProfilerStats);

  ImGui::End();
}
#pragma endregion
