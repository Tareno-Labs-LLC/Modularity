#include "Engine.h"
#include "ViewportRenderHelpers.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>

using namespace ViewportRenderHelpers;

#pragma region 3D UI Canvas Targets
void Engine::renderUiCanvas3DTargets() {
  if (!rendererInitialized || !projectManager.currentProject.isLoaded)
    return;

  ImGuiContext *mainContext = ImGui::GetCurrentContext();
  if (!mainContext)
    return;

  ImGuiStyle mainStyle = ImGui::GetStyle();
  ImGuiIO &mainIo = ImGui::GetIO();
  refreshSceneObjectIndexCache();

  auto findSceneObject = [&](int id) -> SceneObject * {
    auto it = sceneObjectIndexById.find(id);
    if (it == sceneObjectIndexById.end() || it->second >= sceneObjects.size()) {
      return nullptr;
    }
    return &sceneObjects[it->second];
  };

  auto isUiType = [](const SceneObject &target) {
    return target.hasUI && target.ui.type != UIElementType::None;
  };

  for (auto &obj : sceneObjects) {
    if (obj.hasUI && obj.ui.type == UIElementType::Canvas &&
        !obj.ui.renderIn3D) {
      if (obj.hasRenderer && obj.renderType == RenderType::Sprite) {
        obj.hasRenderer = false;
        obj.renderType = RenderType::None;
      }
    }
  }

  auto isOffscreenCanvas = [](const SceneObject &canvas) {
    return canvas.enabled && canvas.hasUI &&
           canvas.ui.type == UIElementType::Canvas &&
           (canvas.ui.renderIn3D || (canvas.ui.pseudo3DEnabled &&
                                     canvas.ui.pseudo3DUseOffscreenSurface));
  };

  std::unordered_set<int> activeCanvasIds;
  for (const auto &canvas : sceneObjects) {
    if (isOffscreenCanvas(canvas)) {
      activeCanvasIds.insert(canvas.id);
    }
  }

  std::unordered_map<int, std::vector<SceneObject *>> elementsByCanvas;
	  if (!activeCanvasIds.empty()) {
    std::vector<int> canvasRootByIndex(sceneObjects.size(), -2);
    std::vector<size_t> walkPath;
    auto resolveCanvasRoot = [&](size_t startIndex) {
      walkPath.clear();
      size_t index = startIndex;
      int rootId = -1;
      for (;;) {
        if (canvasRootByIndex[index] != -2) {
          rootId = canvasRootByIndex[index];
          break;
        }
        walkPath.push_back(index);
        if (walkPath.size() > sceneObjects.size())
          break;
        const SceneObject &node = sceneObjects[index];
        if (node.parentId < 0)
          break;
        auto it = sceneObjectIndexById.find(node.parentId);
        if (it == sceneObjectIndexById.end() || it->second >= sceneObjects.size())
          break;
        index = it->second;
      }
      for (auto pathIt = walkPath.rbegin(); pathIt != walkPath.rend(); ++pathIt) {
        const SceneObject &node = sceneObjects[*pathIt];
        if (rootId < 0 && node.hasUI && node.ui.type == UIElementType::Canvas) {
          rootId = node.id;
        }
        canvasRootByIndex[*pathIt] = rootId;
      }
      return canvasRootByIndex[startIndex];
    };

    for (size_t i = 0; i < sceneObjects.size(); ++i) {
      SceneObject &obj = sceneObjects[i];
      if (!isUiType(obj) || obj.ui.type == UIElementType::Canvas)
        continue;
      const int rootId = resolveCanvasRoot(i);
      if (rootId < 0 || activeCanvasIds.find(rootId) == activeCanvasIds.end())
        continue;
      if (!IsObjectEnabledInHierarchy(obj))
        continue;
      elementsByCanvas[rootId].push_back(&obj);
	    }
	  }

  auto hashCombine64 = [](uint64_t seed, uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
  };
  auto hashFloat64 = [](float value) -> uint64_t {
    if (!std::isfinite(value))
      return 0;
    return static_cast<uint64_t>(
        static_cast<int64_t>(std::llround(static_cast<double>(value) * 1024.0)));
  };
  auto hashString64 = [](const std::string &value) -> uint64_t {
    return static_cast<uint64_t>(std::hash<std::string>{}(value));
  };
  auto addVec2 = [&](uint64_t seed, const glm::vec2 &value) {
    seed = hashCombine64(seed, hashFloat64(value.x));
    return hashCombine64(seed, hashFloat64(value.y));
  };
  auto addVec4 = [&](uint64_t seed, const glm::vec4 &value) {
    seed = hashCombine64(seed, hashFloat64(value.x));
    seed = hashCombine64(seed, hashFloat64(value.y));
    seed = hashCombine64(seed, hashFloat64(value.z));
    return hashCombine64(seed, hashFloat64(value.w));
  };
  auto addUIObjectSignature = [&](uint64_t seed, const SceneObject &obj) {
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.id));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.parentId));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.enabled ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.hasUI ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.type));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.anchor));
    seed = addVec2(seed, obj.ui.position);
    seed = addVec2(seed, obj.ui.size);
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.maskChildren ? 1 : 0));
    seed = hashCombine64(seed, hashFloat64(obj.ui.rotation));
    seed = hashCombine64(seed, hashFloat64(obj.ui.sliderValue));
    seed = hashCombine64(seed, hashFloat64(obj.ui.sliderMin));
    seed = hashCombine64(seed, hashFloat64(obj.ui.sliderMax));
    seed = hashCombine64(seed, hashString64(obj.ui.label));
    seed = addVec4(seed, obj.ui.color);
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.interactable ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.sliderStyle));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.buttonStyle));
    seed = hashCombine64(seed, hashString64(obj.ui.stylePreset));
    seed = hashCombine64(seed, hashFloat64(obj.ui.textScale));
    seed = hashCombine64(seed, hashString64(obj.ui.textFont));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.textAutoWrap ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.textHAlign));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.textVAlign));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.textEffectFlags));
    seed = hashCombine64(seed, hashFloat64(obj.ui.textEffectSpeed));
    seed = hashCombine64(seed, hashFloat64(obj.ui.textEffectIntensity));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.spriteSheetEnabled ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.spriteSheetColumns));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.spriteSheetRows));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.spriteSheetFrame));
    seed = hashCombine64(seed, hashFloat64(obj.ui.spriteSheetFps));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.spriteSheetLoop ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.spriteCustomFramesEnabled ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.spriteCustomFrames.size()));
    if (obj.ui.spriteCustomFramesEnabled && !obj.ui.spriteCustomFrames.empty()) {
      const int frame = resolveSpriteSheetFrame(obj);
      const glm::ivec4 &rect =
          obj.ui.spriteCustomFrames[static_cast<size_t>(
              std::clamp(frame, 0, static_cast<int>(obj.ui.spriteCustomFrames.size()) - 1))];
      seed = hashCombine64(seed, static_cast<uint64_t>(rect.x));
      seed = hashCombine64(seed, static_cast<uint64_t>(rect.y));
      seed = hashCombine64(seed, static_cast<uint64_t>(rect.z));
      seed = hashCombine64(seed, static_cast<uint64_t>(rect.w));
      if (obj.ui.spriteCustomFrameScales.size() == obj.ui.spriteCustomFrames.size()) {
        seed = addVec2(seed, obj.ui.spriteCustomFrameScales[static_cast<size_t>(
                              std::clamp(frame, 0, static_cast<int>(obj.ui.spriteCustomFrameScales.size()) - 1))]);
      }
    }
    seed = hashCombine64(seed, hashString64(obj.ui.spriteSheetAssetPath));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.nineSliceEnabled ? 1 : 0));
    seed = addVec4(seed, obj.ui.nineSliceBorder);
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.nineSliceTileEdges ? 1 : 0));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.nineSliceTileCenter ? 1 : 0));
    seed = addVec4(seed, obj.ui.fillColor);
    seed = addVec4(seed, obj.ui.backgroundColor);
    seed = addVec4(seed, obj.ui.borderColor);
    seed = addVec4(seed, obj.ui.textColor);
    seed = hashCombine64(seed, hashFloat64(obj.ui.fontSize));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.ui.sortingOrder));
    seed = hashCombine64(seed, hashString64(obj.albedoTexturePath));
    seed = hashCombine64(seed, static_cast<uint64_t>(obj.material.textureFilter));
    auto animIt = uiAnimationStates.find(obj.id);
    if (animIt != uiAnimationStates.end()) {
      seed = hashCombine64(seed, hashFloat64(animIt->second.hover));
      seed = hashCombine64(seed, hashFloat64(animIt->second.active));
      seed = hashCombine64(seed, hashFloat64(animIt->second.sliderValue));
      seed = hashCombine64(seed, static_cast<uint64_t>(animIt->second.initialized ? 1 : 0));
    }
    return seed;
  };

	  struct UiCanvas3DRenderPlan {
	    float layoutWidth = 1.0f;
	    float layoutHeight = 1.0f;
	    int targetWidth = 16;
	    int targetHeight = 16;
	    bool pseudoCanvas = false;
	    bool targetInvalidated = false;
	    bool hasTimeAnimatedContent = false;
	    uint64_t contentSignature = 0;
	    Renderer::UiTargetInfo target;
	  };

	  std::unordered_map<int, UiCanvas3DRenderPlan> canvasRenderPlans;
	  canvasRenderPlans.reserve(activeCanvasIds.size());
	  for (auto &canvas : sceneObjects) {
	    if (!isOffscreenCanvas(canvas))
	      continue;

	    UiCanvas3DRenderPlan plan;
	    plan.pseudoCanvas = !canvas.ui.renderIn3D && canvas.ui.pseudo3DEnabled;
	    const glm::vec2 pseudoLayout =
	        plan.pseudoCanvas ? ResolvePseudo3DLayoutSize(canvas) : glm::vec2(0.0f);
	    plan.layoutWidth =
	        plan.pseudoCanvas ? pseudoLayout.x : std::max(1.0f, canvas.ui.size.x);
	    plan.layoutHeight =
	        plan.pseudoCanvas ? pseudoLayout.y : std::max(1.0f, canvas.ui.size.y);
	    plan.targetWidth = (canvas.ui.renderTargetSize.x > 0)
	                           ? canvas.ui.renderTargetSize.x
	                           : static_cast<int>(plan.layoutWidth);
	    plan.targetHeight = (canvas.ui.renderTargetSize.y > 0)
	                            ? canvas.ui.renderTargetSize.y
	                            : static_cast<int>(plan.layoutHeight);
	    plan.targetWidth = std::clamp(plan.targetWidth, 16, 4096);
	    plan.targetHeight = std::clamp(plan.targetHeight, 16, 4096);
	    plan.target = renderer.ensureUiTarget(
	        canvas.id, plan.targetWidth, plan.targetHeight, canvas.layer,
	        canvas.ui.renderTargetFilter, canvas.ui.renderIn3D);
	    plan.targetInvalidated = plan.target.invalidated;
	    uint64_t signature = 0x84222325cbf29ce4ull;
	    signature = hashCombine64(signature, static_cast<uint64_t>(uiAnimationMode));
	    signature = hashCombine64(signature, hashString64(uiEditorFontAsset));
	    signature = hashCombine64(signature, static_cast<uint64_t>(canvas.layer));
	    signature = hashCombine64(signature, static_cast<uint64_t>(canvas.ui.renderTargetFilter));
	    signature = hashCombine64(signature, static_cast<uint64_t>(plan.pseudoCanvas ? 1 : 0));
	    signature = hashCombine64(signature, hashFloat64(plan.layoutWidth));
	    signature = hashCombine64(signature, hashFloat64(plan.layoutHeight));
	    signature = hashCombine64(signature, static_cast<uint64_t>(plan.targetWidth));
	    signature = hashCombine64(signature, static_cast<uint64_t>(plan.targetHeight));
	    signature = addUIObjectSignature(signature, canvas);
	    auto drawListIt = elementsByCanvas.find(canvas.id);
	    if (drawListIt != elementsByCanvas.end()) {
	      signature =
	          hashCombine64(signature, static_cast<uint64_t>(drawListIt->second.size()));
	      for (const SceneObject *objPtr : drawListIt->second) {
	        if (!objPtr)
	          continue;
	        if (objPtr->ui.type == UIElementType::Text &&
	            objPtr->ui.textEffectFlags != 0) {
	          plan.hasTimeAnimatedContent = true;
	        }
	        signature = addUIObjectSignature(signature, *objPtr);
	      }
	    }
	    plan.contentSignature = signature;
	    canvasRenderPlans[canvas.id] = plan;
	  }

	  bool renderStateCaptured = false;
	  GLint savedFbo = 0;
	  GLint savedViewport[4] = {};
	  GLboolean savedScissorEnabled = GL_FALSE;
	  GLint savedScissorBox[4] = {};
	  auto captureRenderState = [&]() {
	    if (renderStateCaptured)
	      return;
	    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
	    glGetIntegerv(GL_VIEWPORT, savedViewport);
	    savedScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	    glGetIntegerv(GL_SCISSOR_BOX, savedScissorBox);
	    renderStateCaptured = true;
	  };

	  for (auto &canvas : sceneObjects) {
	    if (!isOffscreenCanvas(canvas))
	      continue;
	    auto planIt = canvasRenderPlans.find(canvas.id);
	    if (planIt == canvasRenderPlans.end())
	      continue;
	    const UiCanvas3DRenderPlan &plan = planIt->second;

    if (canvas.ui.renderIn3D) {
      canvas.hasRenderer = true;
      canvas.renderType = RenderType::Sprite;
      canvas.material.textureMix = 1.0f;
    } else if (canvas.hasRenderer && canvas.renderType == RenderType::Sprite) {
      canvas.hasRenderer = false;
      canvas.renderType = RenderType::None;
    }
    if (canvas.ui.pseudo3DEnabled && !canvas.ui.renderIn3D) {
      canvas.faceCamera = false;
    }

	    const float layoutWidth = plan.layoutWidth;
	    const float layoutHeight = plan.layoutHeight;

	    const Renderer::UiTargetInfo &target = plan.target;
	    if (target.fbo == 0 || target.texture == 0)
	      continue;
	    const bool targetInvalidated = plan.targetInvalidated;

    {
      auto ctxIt = uiCanvas3DContexts.find(canvas.id);
      const bool everRendered =
          ctxIt != uiCanvas3DContexts.end() && ctxIt->second.backendReady &&
          ctxIt->second.hasRenderedTarget;
      auto skipInputIt = uiCanvas3DInputs.find(canvas.id);
      const bool inputNow = skipInputIt != uiCanvas3DInputs.end() &&
                            skipInputIt->second.hasInput;
      const bool contentChanged =
          !everRendered || ctxIt->second.lastContentSignature != plan.contentSignature;
	      if (!targetInvalidated && everRendered && !inputNow &&
	          !ctxIt->second.hadInputLastFrame && !plan.hasTimeAnimatedContent &&
	          !contentChanged) {
	        Modu2DStats::CountSkippedRedraw();
	        continue;
	      }
	      if (!targetInvalidated && everRendered && canvas.ui.renderIn3D && !inputNow &&
	          !ctxIt->second.hadInputLastFrame &&
	          renderer.hasCapturedSceneFrustums() &&
	          !renderer.isObjectVisibleInCapturedFrustums(canvas, 1.25f)) {
	        Modu2DStats::CountSkippedRedraw();
	        continue;
      }
    }

    UiCanvas3DContext &ctxEntry = uiCanvas3DContexts[canvas.id];
    if (!ctxEntry.context) {
      ctxEntry.context = ImGui::CreateContext();
      ImGui::SetCurrentContext(ctxEntry.context);
      ImGuiIO &io = ImGui::GetIO();
      preloadUIFontCatalogForContext(ctxEntry.context);
      ImFont *canvasFont = getUIFontForContext(uiEditorFontAsset, ctxEntry.context);
      if (!canvasFont) {
        canvasFont = getUIFontForContext("builtin:imgui-default", ctxEntry.context);
      }
      if (canvasFont) {
        io.FontDefault = canvasFont;
      }
      ImGui_ImplOpenGL3_Init(Modularity::OpenGLImGuiGlslVersion());
      ctxEntry.backendReady = true;
    }

	    ImGui::SetCurrentContext(ctxEntry.context);
	    ImGuiIO &io = ImGui::GetIO();
	    const float framebufferScaleX =
	        (layoutWidth > 0.0f) ? (static_cast<float>(target.width) / layoutWidth)
	                             : 1.0f;
	    const float framebufferScaleY =
	        (layoutHeight > 0.0f) ? (static_cast<float>(target.height) / layoutHeight)
	                              : 1.0f;
	    const ImVec2 canvasWindowPos(
	        target.usesAtlas ? (static_cast<float>(target.x) / framebufferScaleX) : 0.0f,
	        target.usesAtlas ? (static_cast<float>(target.y) / framebufferScaleY) : 0.0f);
	    io.DisplaySize = target.usesAtlas
	                         ? ImVec2(static_cast<float>(target.framebufferWidth) / framebufferScaleX,
	                                  static_cast<float>(target.framebufferHeight) / framebufferScaleY)
	                         : ImVec2(layoutWidth, layoutHeight);
	    io.DisplayFramebufferScale = ImVec2(framebufferScaleX, framebufferScaleY);
    io.DeltaTime =
        (mainIo.DeltaTime > 0.0f) ? mainIo.DeltaTime : (1.0f / 60.0f);
	    auto inputIt = uiCanvas3DInputs.find(canvas.id);
	    if (inputIt != uiCanvas3DInputs.end() && inputIt->second.hasInput) {
	      io.MousePos = ImVec2(canvasWindowPos.x + inputIt->second.mousePos.x,
	                           canvasWindowPos.y + inputIt->second.mousePos.y);
      io.MouseDown[0] = inputIt->second.mouseDown[0];
      io.MouseDown[1] = inputIt->second.mouseDown[1];
      io.MouseDown[2] = inputIt->second.mouseDown[2];
      io.MouseWheel = inputIt->second.mouseWheel;
      ctxEntry.hadInputLastFrame = true;
    } else {
      io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
      io.MouseDown[0] = false;
      io.MouseDown[1] = false;
      io.MouseDown[2] = false;
      io.MouseWheel = 0.0f;
      ctxEntry.hadInputLastFrame = false;
    }

    ImGui::GetStyle() = mainStyle;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

	    ImGui::SetNextWindowPos(canvasWindowPos);
    ImGui::SetNextWindowSize(ImVec2(layoutWidth, layoutHeight));
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##Canvas3D", nullptr, flags);

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
    auto rotateAbout = [](ImVec2 p, ImVec2 pivot, float rad) {
      const float c = std::cos(rad), s = std::sin(rad);
      const float dx = p.x - pivot.x, dy = p.y - pivot.y;
      return ImVec2(pivot.x + dx * c - dy * s, pivot.y + dx * s + dy * c);
    };
    auto rotateDir = [](ImVec2 v, float rad) {
      const float c = std::cos(rad), s = std::sin(rad);
      return ImVec2(v.x * c - v.y * s, v.x * s + v.y * c);
    };
    auto resolveUIRect = [&](const SceneObject &obj, ImVec2 &outMin,
                             ImVec2 &outMax, float *outRotRad = nullptr) {
      std::vector<const SceneObject *> chain;
      const SceneObject *current = &obj;
      while (current) {
        if (isUiType(*current) && current->id != canvas.id) {
          chain.push_back(current);
        }
        if (current->parentId < 0)
          break;
        current = findSceneObject(current->parentId);
        if (!current)
          break;
        if (current->id == canvas.id)
          break;
      }
      std::reverse(chain.begin(), chain.end());

      ImVec2 regionMin = ImGui::GetWindowPos();
      ImVec2 regionMax =
          ImVec2(regionMin.x + layoutWidth, regionMin.y + layoutHeight);
      float accumRot = 0.0f;
      ImVec2 parentCenter((regionMin.x + regionMax.x) * 0.5f,
                          (regionMin.y + regionMax.y) * 0.5f);
      for (const SceneObject *node : chain) {
        glm::vec2 nodeSize = getSpriteDisplaySize(*node);
        ImVec2 size =
            ImVec2(std::max(1.0f, nodeSize.x), std::max(1.0f, nodeSize.y));
        ImVec2 anchorLocal =
            anchorToPoint(node->ui.anchor, regionMin, regionMax);
        ImVec2 anchorWorld = rotateAbout(anchorLocal, parentCenter, accumRot);
        ImVec2 off = rotateDir(
            ImVec2(node->ui.position.x, node->ui.position.y), accumRot);
        ImVec2 pivot(anchorWorld.x + off.x, anchorWorld.y + off.y);
        const float nodeRot = accumRot + glm::radians(node->ui.rotation);
        ImVec2 pivotOffset = anchorToPivot(node->ui.anchor, size);
        ImVec2 centerOff = rotateDir(
            ImVec2(size.x * 0.5f - pivotOffset.x, size.y * 0.5f - pivotOffset.y),
            nodeRot);
        ImVec2 center(pivot.x + centerOff.x, pivot.y + centerOff.y);
        regionMin =
            ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
        regionMax =
            ImVec2(center.x + size.x * 0.5f, center.y + size.y * 0.5f);
        parentCenter = center;
        accumRot = nodeRot;
      }
      outMin = regionMin;
      outMax = regionMax;
      if (outRotRad)
        *outRotRad = accumRot;
    };

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
    BatchedSpriteEmitter spriteBatch(ImGui::GetWindowDrawList());
    auto resolveCanvasMaskRectForObject =
        [&](const SceneObject &obj, ImVec2 &outMin, ImVec2 &outMax) -> bool {
      bool hasMask = false;
      ImVec2 maskMin(0.0f, 0.0f);
      ImVec2 maskMax(0.0f, 0.0f);
      const SceneObject *current = &obj;
      while (current && current->parentId >= 0) {
        current = findSceneObject(current->parentId);
        if (!current)
          break;
        if (!(current->hasUI && current->ui.type == UIElementType::Canvas &&
              current->ui.maskChildren)) {
          continue;
        }

        ImVec2 canvasMin, canvasMax;
        resolveUIRect(*current, canvasMin, canvasMax);
        if (!hasMask) {
          maskMin = canvasMin;
          maskMax = canvasMax;
          hasMask = true;
        } else {
          maskMin.x = std::max(maskMin.x, canvasMin.x);
          maskMin.y = std::max(maskMin.y, canvasMin.y);
          maskMax.x = std::min(maskMax.x, canvasMax.x);
          maskMax.y = std::min(maskMax.y, canvasMax.y);
        }
      }
      if (!hasMask)
        return false;
      outMin = maskMin;
      outMax = maskMax;
      return (outMax.x > outMin.x) && (outMax.y > outMin.y);
    };

    std::vector<SceneObject *> &canvasDrawList = elementsByCanvas[canvas.id];
    StableSortRuntimeUiDrawList(canvasDrawList);

    for (SceneObject *objPtr : canvasDrawList) {
      SceneObject &obj = *objPtr;
      if (ViewportRenderHelpers::IsAssemblageLayerDrawable(obj))
        continue;
      ImVec2 rectMin, rectMax;
      float elementRotRad = 0.0f;
      resolveUIRect(obj, rectMin, rectMax, &elementRotRad);
      ImVec2 rectSize(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
      if (rectSize.x <= 1.0f || rectSize.y <= 1.0f)
        continue;

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

      ImVec2 drawMin = rectMin;
      ImVec2 drawMax = rectMax;
      ImVec2 drawSize(drawMax.x - drawMin.x, drawMax.y - drawMin.y);
      ImVec2 localMin(drawMin.x - ImGui::GetWindowPos().x,
                      drawMin.y - ImGui::GetWindowPos().y);
      bool pushedCanvasMask = false;
      if (obj.ui.type != UIElementType::Canvas) {
        ImVec2 maskMin, maskMax;
        if (resolveCanvasMaskRectForObject(obj, maskMin, maskMax)) {
          maskMin.x = std::max(maskMin.x, ImGui::GetWindowPos().x);
          maskMin.y = std::max(maskMin.y, ImGui::GetWindowPos().y);
          maskMax.x =
              std::min(maskMax.x, ImGui::GetWindowPos().x + layoutWidth);
          maskMax.y =
              std::min(maskMax.y, ImGui::GetWindowPos().y + layoutHeight);
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
      if (obj.ui.type == UIElementType::Image ||
          obj.ui.type == UIElementType::Sprite2D) {
        Texture *spriteTex = nullptr;
        unsigned int texId = 0;
        if (rendererInitialized && !obj.albedoTexturePath.empty()) {
          spriteTex = renderer.getTexture(
              obj.albedoTexturePath, MaterialProperties::TextureFilter::Point);
          if (spriteTex != nullptr) {
            texId = spriteTex->GetID();
          }
        }
        std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
        const int frame = resolveSpriteSheetFrame(obj);
        const ImVec2 sourceFrameSizePx =
            ResolveUiSourceFrameSizePx(obj, frame, spriteTex);
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        const ImU32 tintColor = ImGui::GetColorU32(tint);
        float angle = elementRotRad;
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
          }
        }
      } else if (obj.ui.type == UIElementType::Slider) {
        spriteBatch.flush();
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        const ImU32 s3SliderBg     = (obj.ui.backgroundColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.backgroundColor.r, obj.ui.backgroundColor.g, obj.ui.backgroundColor.b, obj.ui.backgroundColor.a)) : ImGui::GetColorU32(ImVec4(tint.x * 0.2f, tint.y * 0.2f, tint.z * 0.2f, tint.w * 0.6f));
        const ImU32 s3SliderFill   = (obj.ui.fillColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.fillColor.r, obj.ui.fillColor.g, obj.ui.fillColor.b, obj.ui.fillColor.a))             : ImGui::GetColorU32(tint);
        const ImU32 s3SliderBorder = (obj.ui.borderColor.a > 0.0f)     ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a))     : ImGui::GetColorU32(brighten(tint, 0.85f));
        const ImU32 s3SliderText   = (obj.ui.textColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a))             : IM_COL32(240, 240, 245, 220);
        if (obj.ui.sliderStyle == UISliderStyle::ImGui) {
          float minValue = obj.ui.sliderMin;
          float maxValue = obj.ui.sliderMax;
          float range = (maxValue - minValue);
          if (range <= 1e-6f)
            range = 1.0f;
          ImDrawList *dl = ImGui::GetWindowDrawList();
          if (obj.ui.interactable) {
            ImGui::SetCursorPos(localMin);
            ImGui::InvisibleButton("##UISliderImGui", drawSize);
            if (ImGui::IsItemActive() &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left) && drawSize.x > 1.0f) {
              float mouseT = (ImGui::GetIO().MousePos.x - drawMin.x) / drawSize.x;
              mouseT = std::clamp(mouseT, 0.0f, 1.0f);
              obj.ui.sliderValue = minValue + mouseT * range;
            }
          }
          float t = (obj.ui.sliderValue - minValue) / range;
          t = std::clamp(t, 0.0f, 1.0f);
          float rounding = std::min(drawSize.x, drawSize.y) * 0.5f;
          ImVec2 fillMax(drawMin.x + drawSize.x * t, drawMax.y);
          dl->AddRectFilled(drawMin, drawMax, s3SliderBg, rounding);
          if (fillMax.x > drawMin.x) {
            dl->AddRectFilled(drawMin, fillMax, s3SliderFill, rounding);
          }
          dl->AddRect(drawMin, drawMax, s3SliderBorder, rounding);
          ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
          ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                         drawMin.y + (drawSize.y - textSize.y) * 0.5f);
          dl->AddText(textPos, s3SliderText, obj.ui.label.c_str());
        } else {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 bg     = s3SliderBg;
          ImU32 fill   = s3SliderFill;
          ImU32 border = s3SliderBorder;
          float minValue = obj.ui.sliderMin;
          float maxValue = obj.ui.sliderMax;
          float range = (maxValue - minValue);
          if (range <= 1e-6f)
            range = 1.0f;
          ImGui::SetCursorPos(localMin);
          ImGui::BeginDisabled(!obj.ui.interactable);
          ImGui::InvisibleButton("##UISlider", drawSize);
          bool held = obj.ui.interactable && ImGui::IsItemActive();
          if (held && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
              drawSize.x > 1.0f) {
            float mouseT = (ImGui::GetIO().MousePos.x - drawMin.x) / drawSize.x;
            mouseT = std::clamp(mouseT, 0.0f, 1.0f);
            float newValue = minValue + mouseT * range;
            obj.ui.sliderValue = newValue;
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
              border, s3SliderText, t, minValue, maxValue,
              obj.ui.label.c_str());
        }
      } else if (obj.ui.type == UIElementType::Button) {
        spriteBatch.flush();
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        obj.ui.buttonPressed = false;
        obj.ui.uiHovered = false;
        obj.ui.uiActive = false;
        const ImU32 s3BtnText = (obj.ui.textColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a)) : IM_COL32(240, 240, 245, 220);
        if (obj.ui.buttonStyle == UIButtonStyle::ImGui) {
          ImGui::SetCursorPos(localMin);
          ImGui::PushStyleColor(ImGuiCol_Button, tint);
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighten(tint, 1.1f));
          ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighten(tint, 1.2f));
          ImGui::BeginDisabled(!obj.ui.interactable);
          obj.ui.buttonPressed = ImGui::Button(obj.ui.label.c_str(), drawSize);
          obj.ui.uiHovered = ImGui::IsItemHovered();
          obj.ui.uiActive  = ImGui::IsItemActive();
          ImGui::EndDisabled();
          ImGui::PopStyleColor(3);
        } else if (obj.ui.buttonStyle == UIButtonStyle::Outline) {
          ImGui::SetCursorPos(localMin);
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 border = (obj.ui.borderColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a)) : ImGui::GetColorU32(tint);
          ImGui::BeginDisabled(!obj.ui.interactable);
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
          dl->AddText(textPos, s3BtnText, obj.ui.label.c_str());
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
            ImGui::GetFontSize(), scale, obj.ui.fontSize, false, 100.0f);
        const bool textIsRotated = std::abs(elementRotRad) > 1e-4f;
        if (!textIsRotated)
          ImGui::PushClipRect(drawMin, drawMax, true);
        const int textVtxStart = dl->VtxBuffer.Size;
        AddUITextWithFilter(dl, obj.material.textureFilter, textFont,
                            fontSize, drawMin, drawMax,
                            ImGui::GetColorU32(tint), obj.ui.label.c_str(),
                            obj.ui.textAutoWrap, obj.ui.textHAlign,
                            obj.ui.textVAlign, obj.ui.textEffectFlags,
                            obj.ui.textEffectSpeed, obj.ui.textEffectIntensity);
        if (textIsRotated) {
          const ImVec2 textPivotOffset = ViewportRenderHelpers::UiAnchorPivot(
              obj.ui.anchor, ImVec2(drawMax.x - drawMin.x, drawMax.y - drawMin.y));
          const ImVec2 textPivot(drawMin.x + textPivotOffset.x,
                                 drawMin.y + textPivotOffset.y);
          ViewportRenderHelpers::RotateDrawListVertices(
              dl, textVtxStart, dl->VtxBuffer.Size, textPivot, elementRotRad);
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

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::Render();

	    captureRenderState();
	    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
	    glViewport(0, 0, target.framebufferWidth, target.framebufferHeight);
	    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	    if (target.usesAtlas) {
	      glEnable(GL_SCISSOR_TEST);
	      glScissor(target.clearX,
	                target.framebufferHeight - (target.clearY + target.clearHeight),
	                target.clearWidth, target.clearHeight);
	    } else {
	      glDisable(GL_SCISSOR_TEST);
	    }
	    glClear(GL_COLOR_BUFFER_BIT);
	    glDisable(GL_SCISSOR_TEST);
	    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	    ctxEntry.lastContentSignature = plan.contentSignature;
	    ctxEntry.hasRenderedTarget = true;
	    renderer.markUiTargetRendered(canvas.id);
	  }

  if (renderStateCaptured) {
    glBindFramebuffer(GL_FRAMEBUFFER, savedFbo);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2],
               savedViewport[3]);
    if (savedScissorEnabled) {
      glEnable(GL_SCISSOR_TEST);
    } else {
      glDisable(GL_SCISSOR_TEST);
    }
    glScissor(savedScissorBox[0], savedScissorBox[1], savedScissorBox[2],
              savedScissorBox[3]);
  }

  for (auto it = uiCanvas3DContexts.begin(); it != uiCanvas3DContexts.end();) {
    if (activeCanvasIds.find(it->first) == activeCanvasIds.end()) {
      if (it->second.context) {
        ImGui::SetCurrentContext(it->second.context);
        if (it->second.backendReady) {
          ImGui_ImplOpenGL3_Shutdown();
        }
        uiFontContexts.erase(it->second.context);
        ImGui::DestroyContext(it->second.context);
      }
      it = uiCanvas3DContexts.erase(it);
    } else {
      ++it;
    }
  }
  renderer.cleanupUiTargets(activeCanvasIds);

  ImGui::SetCurrentContext(mainContext);
}
#pragma endregion
