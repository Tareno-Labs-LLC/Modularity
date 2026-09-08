#include "ViewportRenderHelpers.h"
#include "UiGlassBlur.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace ViewportRenderHelpers {

uint32_t ParticleHash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float ParticleUnit(uint32_t &state) {
  state = ParticleHash(state + 0x9e3779b9u);
  return static_cast<float>(state & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

float ParticleRange(ParticleSystem2DComponent::MinMaxFloat range, uint32_t &state) {
  if (!range.random) return range.min;
  return glm::mix(range.min, range.max, ParticleUnit(state));
}

void RestartParticleSystem2D(ParticleSystem2DComponent &ps, double now) {
  ps.particles.clear();
  ps.runtimeAccumulator = 0.0f;
  ps.runtimeTime = 0.0f;
  ps.runtimeLastUpdateTime = now;
  ps.runtimeInitialized = true;
  ps.playing = ps.playOnAwake;
  ps.paused = false;
}

void EmitParticle2D(SceneObject &obj, uint32_t emissionIndex) {
  ParticleSystem2DComponent &ps = obj.particleSystem2D;
  if (static_cast<int>(ps.particles.size()) >= std::max(1, ps.maxParticles)) return;
  uint32_t seed = ps.autoRandomSeed
                      ? ParticleHash(static_cast<uint32_t>(obj.id) * 747796405u + emissionIndex + static_cast<uint32_t>(ps.runtimeTime * 1000.0f))
                      : ParticleHash(ps.randomSeed + emissionIndex);
  ParticleSystem2DComponent::Particle particle;
  particle.alive = true;
  particle.seed = seed;
  particle.lifetime = std::max(0.01f, ParticleRange(ps.startLifetime, seed));
  particle.size = std::max(0.0f, ParticleRange(ps.startSize, seed));
  particle.rotation = ParticleRange(ps.startRotation, seed);
  particle.startColor = ps.startColor;

  const float angle = ParticleUnit(seed) * 6.28318530718f;
  glm::vec3 direction(std::cos(angle), std::sin(angle), 0.0f);
  if (ps.emitIn3D) {
    const float z = ParticleUnit(seed) * 2.0f - 1.0f;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    direction = glm::vec3(std::cos(angle) * r, std::sin(angle) * r, z);
  }

  if (ps.shape == 1) {
    const float u = ParticleUnit(seed);
    const float radius = ps.shapeRadius *
                         (ps.emitIn3D ? std::cbrt(u) : std::sqrt(u));
    particle.position = direction * radius;
  } else if (ps.shape == 2) {
    particle.position = glm::vec3((ParticleUnit(seed) - 0.5f) * ps.shapeBox.x,
                                  (ParticleUnit(seed) - 0.5f) * ps.shapeBox.y,
                                  ps.emitIn3D ? (ParticleUnit(seed) - 0.5f) * ps.shapeBox.z
                                              : 0.0f);
    direction = glm::length(particle.position) > 0.0001f
                    ? glm::normalize(particle.position)
                    : direction;
  }
  particle.velocity = direction * ParticleRange(ps.startSpeed, seed);
  particle.angularVelocity = ps.rotationOverLifetimeEnabled ? ps.rotationOverLifetime : 0.0f;
  ps.particles.push_back(particle);
}

void SimulateParticleSystem2D(SceneObject &obj, double now) {
  if (!obj.hasParticleSystem2D) return;
  ParticleSystem2DComponent &ps = obj.particleSystem2D;
  if (!ps.enabled) return;
  if (!ps.runtimeInitialized) {
    RestartParticleSystem2D(ps, now);
    if (ps.prewarm) {
      const float prewarmStep = 1.0f / 30.0f;
      const float prewarmTime = std::max(0.0f, ps.startLifetime.max);
      for (float t = 0.0f; t < prewarmTime; t += prewarmStep) {
        ps.runtimeLastUpdateTime -= prewarmStep;
        SimulateParticleSystem2D(obj, now - prewarmTime + t);
      }
      ps.runtimeLastUpdateTime = now;
    }
  }
  float dt = static_cast<float>(now - ps.runtimeLastUpdateTime);
  ps.runtimeLastUpdateTime = now;
  if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
  if (!ps.playing || ps.paused) return;
  dt *= std::max(0.0f, ps.simulationSpeed);
  ps.runtimeTime += dt;
  if (ps.runtimeTime < ps.startDelay) return;

  ps.runtimeAccumulator += std::max(0.0f, ps.emissionRate) * dt;
  int emitCount = static_cast<int>(ps.runtimeAccumulator);
  ps.runtimeAccumulator -= static_cast<float>(emitCount);
  if (ps.burstCount > 0) {
    if (ps.burstLoop && ps.burstTime > 0.0f) {
      const int previousBurst =
          static_cast<int>(std::floor((ps.runtimeTime - dt) / ps.burstTime));
      const int currentBurst =
          static_cast<int>(std::floor(ps.runtimeTime / ps.burstTime));
      if (currentBurst > previousBurst) {
        emitCount += (currentBurst - previousBurst) * ps.burstCount;
      }
    } else if (ps.runtimeTime - dt <= ps.burstTime &&
               ps.runtimeTime >= ps.burstTime) {
      emitCount += ps.burstCount;
    }
  }
  for (int i = 0; i < emitCount; ++i) {
    EmitParticle2D(obj, static_cast<uint32_t>(ps.particles.size() + i));
  }

  for (auto &particle : ps.particles) {
    if (!particle.alive) continue;
    particle.age += dt;
    if (particle.age >= particle.lifetime) {
      particle.alive = false;
      continue;
    }
    glm::vec3 velocity = particle.velocity;
    if (ps.velocityOverLifetimeEnabled) velocity += ps.velocityOverLifetime;
    velocity.y -= ps.gravityModifier * dt;
    if (ps.noiseEnabled && ps.noiseStrength > 0.0f) {
      const float n = std::sin((ps.runtimeTime + static_cast<float>(particle.seed % 997u)) * std::max(0.01f, ps.noiseFrequency));
      const float m = std::cos((ps.runtimeTime * 0.7f + static_cast<float>(particle.seed % 613u)) * std::max(0.01f, ps.noiseFrequency));
      glm::vec3 drift(std::cos(n * 6.28318f), std::sin(n * 6.28318f),
                      ps.emitIn3D ? std::sin(m * 6.28318f) : 0.0f);
      velocity += drift * ps.noiseStrength;
    }
    particle.velocity = velocity;
    particle.position += velocity * dt;
    particle.rotation += particle.angularVelocity * dt;
  }
  ps.particles.erase(std::remove_if(ps.particles.begin(), ps.particles.end(),
                                    [](const auto &p) { return !p.alive; }),
                     ps.particles.end());
  if (!ps.looping && ps.runtimeTime > ps.startDelay + ps.startLifetime.max) {
    ps.playing = false;
  }
}

ImVec2 ComputeAspectFitSize(const ImVec2 &available, float aspect) {
  const float safeWidth = std::max(1.0f, available.x);
  const float safeHeight = std::max(1.0f, available.y);
  float width = safeWidth;
  float height = width / std::max(0.001f, aspect);
  if (height > safeHeight) {
    height = safeHeight;
    width = height * std::max(0.001f, aspect);
  }
  return ImVec2(std::max(1.0f, std::floor(width)),
                std::max(1.0f, std::floor(height)));
}

void DrawViewportProfilerPanel(ImDrawList *drawList, const ImVec2 &topLeft,
                               const std::vector<std::string> &lines) {
  if (drawList == nullptr || lines.empty()) {
    return;
  }
  const float lineHeight = ImGui::GetFontSize() + 2.0f;
  const ImVec2 pad(8.0f, 6.0f);
  float maxWidth = 0.0f;
  for (const std::string &line : lines) {
    maxWidth = std::max(maxWidth, ImGui::CalcTextSize(line.c_str()).x);
  }
  const ImVec2 panelMax(topLeft.x + maxWidth + pad.x * 2.0f,
                        topLeft.y + lineHeight * static_cast<float>(lines.size()) +
                            pad.y * 2.0f);

  drawList->AddRectFilled(topLeft, panelMax, IM_COL32(18, 18, 24, 210), 6.0f);
  drawList->AddRect(topLeft, panelMax, IM_COL32(255, 255, 255, 40), 6.0f);
  for (size_t i = 0; i < lines.size(); ++i) {
    const ImVec2 textPos(topLeft.x + pad.x,
                         topLeft.y + pad.y + lineHeight * static_cast<float>(i));
    drawList->AddText(textPos, IM_COL32(235, 235, 245, 255), lines[i].c_str());
  }
}

EmbeddedViewportLayout BuildEmbeddedViewportLayout(
    const ImVec2 &panelMin, const ImVec2 &panelSize, int renderWidth,
    int renderHeight, ViewportDisplayMode displayMode, float zoom) {
  EmbeddedViewportLayout layout;
  layout.panelMin = panelMin;
  layout.panelSize =
      ImVec2(std::max(1.0f, panelSize.x), std::max(1.0f, panelSize.y));
  layout.panelMax = ImVec2(layout.panelMin.x + layout.panelSize.x,
                           layout.panelMin.y + layout.panelSize.y);

  const float safeRenderWidth = static_cast<float>(std::max(1, renderWidth));
  const float safeRenderHeight = static_cast<float>(std::max(1, renderHeight));
  const float aspect = safeRenderWidth / safeRenderHeight;
  const float panelAspect =
      layout.panelSize.x / std::max(1.0f, layout.panelSize.y);

  ImVec2 displaySize = layout.panelSize;
  ImVec2 baseUvMin(0.0f, 0.0f);
  ImVec2 baseUvMax(1.0f, 1.0f);
  switch (displayMode) {
  case ViewportDisplayMode::Fit:
    displaySize = ComputeAspectFitSize(layout.panelSize, aspect);
    break;
  case ViewportDisplayMode::Fill:
    displaySize = layout.panelSize;
    if (panelAspect > aspect) {
      const float croppedHeight =
          std::clamp(aspect / std::max(0.001f, panelAspect), 0.0f, 1.0f);
      const float cropOffset = (1.0f - croppedHeight) * 0.5f;
      baseUvMin.y = cropOffset;
      baseUvMax.y = cropOffset + croppedHeight;
    } else {
      const float croppedWidth =
          std::clamp(panelAspect / std::max(0.001f, aspect), 0.0f, 1.0f);
      const float cropOffset = (1.0f - croppedWidth) * 0.5f;
      baseUvMin.x = cropOffset;
      baseUvMax.x = cropOffset + croppedWidth;
    }
    break;
  case ViewportDisplayMode::IntegerScale: {
    const float scaleX = layout.panelSize.x / safeRenderWidth;
    const float scaleY = layout.panelSize.y / safeRenderHeight;
    float integerScale = std::floor(std::min(scaleX, scaleY));
    if (integerScale < 1.0f) {
      displaySize = ComputeAspectFitSize(layout.panelSize, aspect);
    } else {
      displaySize =
          ImVec2(std::max(1.0f, std::floor(safeRenderWidth * integerScale)),
                 std::max(1.0f, std::floor(safeRenderHeight * integerScale)));
    }
    break;
  }
  case ViewportDisplayMode::Stretch:
  default:
    displaySize = layout.panelSize;
    break;
  }

  const float offsetX = (layout.panelSize.x - displaySize.x) * 0.5f;
  const float offsetY = (layout.panelSize.y - displaySize.y) * 0.5f;
  layout.displayMin =
      ImVec2(layout.panelMin.x + offsetX, layout.panelMin.y + offsetY);
  layout.displayMax = ImVec2(layout.displayMin.x + displaySize.x,
                             layout.displayMin.y + displaySize.y);
  layout.displaySize = displaySize;

  const float safeZoom = std::clamp(zoom, 1.0f, 8.0f);
  const float baseUvWidth = std::max(0.0f, baseUvMax.x - baseUvMin.x);
  const float baseUvHeight = std::max(0.0f, baseUvMax.y - baseUvMin.y);
  const float zoomedUvWidth = std::clamp(baseUvWidth / safeZoom, 0.0f, 1.0f);
  const float zoomedUvHeight = std::clamp(baseUvHeight / safeZoom, 0.0f, 1.0f);
  const float zoomOffsetX = (baseUvWidth - zoomedUvWidth) * 0.5f;
  const float zoomOffsetY = (baseUvHeight - zoomedUvHeight) * 0.5f;
  layout.uvMin = ImVec2(baseUvMin.x + zoomOffsetX, baseUvMin.y + zoomOffsetY);
  layout.uvMax =
      ImVec2(layout.uvMin.x + zoomedUvWidth, layout.uvMin.y + zoomedUvHeight);
  return layout;
}

bool TryMapScreenPointToRenderPixel(const EmbeddedViewportLayout &layout,
                                    const ImVec2 &screenPoint, int renderWidth,
                                    int renderHeight, glm::vec2 &outRenderPixel,
                                    glm::vec2 *outNormalized) {
  const ImVec2 visibleMin(std::max(layout.panelMin.x, layout.displayMin.x),
                          std::max(layout.panelMin.y, layout.displayMin.y));
  const ImVec2 visibleMax(std::min(layout.panelMax.x, layout.displayMax.x),
                          std::min(layout.panelMax.y, layout.displayMax.y));
  if (screenPoint.x < visibleMin.x || screenPoint.x > visibleMax.x ||
      screenPoint.y < visibleMin.y || screenPoint.y > visibleMax.y) {
    return false;
  }

  const float displayWidth = std::max(1.0f, layout.displaySize.x);
  const float displayHeight = std::max(1.0f, layout.displaySize.y);
  const float normX = std::clamp(
      (screenPoint.x - layout.displayMin.x) / displayWidth, 0.0f, 1.0f);
  const float normY = std::clamp(
      (screenPoint.y - layout.displayMin.y) / displayHeight, 0.0f, 1.0f);
  const float sourceU =
      layout.uvMin.x + (layout.uvMax.x - layout.uvMin.x) * normX;
  const float sourceV =
      layout.uvMin.y + (layout.uvMax.y - layout.uvMin.y) * normY;
  outRenderPixel =
      glm::vec2(sourceU * static_cast<float>(std::max(1, renderWidth)),
                sourceV * static_cast<float>(std::max(1, renderHeight)));
  if (outNormalized != nullptr) {
    *outNormalized = glm::vec2(sourceU, sourceV);
  }
  return true;
}

void SubmitRuntime2DWorldProfilerStats(const Runtime2DWorldProfilerStats &stats) {
  if (!stats.useWorldUi) {
    return;
  }

  ModuRuntime2DProfiler_RecordUiRuntime(
      stats.uiRuntimeMs, stats.spriteBatchBuildMs, stats.visibleObjectCount);

  Profiler &profiler = Profiler::instance();
  std::string worldRenderLabel = "2D World Render";
  if (stats.visibleObjectCount > 0) {
    worldRenderLabel +=
        " (" + std::to_string(stats.visibleObjectCount) + " visible)";
  }
  profiler.addSyntheticSample(worldRenderLabel, ProfilerSampleCategory::Render,
                              std::max(0.0, stats.uiRuntimeMs));

  if (stats.spriteBatchBuildMs > 0.0001) {
    profiler.addSyntheticSample("2D Sprite Build",
                                ProfilerSampleCategory::RenderDetail,
                                stats.spriteBatchBuildMs);
  }

  if (stats.light2DBuildMs > 0.0001f) {
    std::string lightBuildLabel = "2D Light2D Build";
    if (stats.light2DVisibleLights > 0 || stats.light2DVisibleSprites > 0) {
      lightBuildLabel +=
          " (" + std::to_string(std::max(0, stats.light2DVisibleLights)) +
          " lights, " +
          std::to_string(std::max(0, stats.light2DVisibleSprites)) +
          " sprites)";
    }
    profiler.addSyntheticSample(lightBuildLabel,
                                ProfilerSampleCategory::RenderDetail,
                                static_cast<double>(stats.light2DBuildMs));
  }
}

void ApplyNearestTextureSampling(GLuint textureId) {
  if (textureId == 0 || glfwGetCurrentContext() == nullptr)
    return;

  GLint previousTexture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
  glBindTexture(GL_TEXTURE_2D, textureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
}

bool RuntimeUiDrawOrderLess(const SceneObject *a, const SceneObject *b) {
  if (a == nullptr || b == nullptr)
    return false;
  if (a->layer != b->layer)
    return a->layer < b->layer;

  const int parallaxOrderA =
      (a->hasParallaxLayer2D && a->parallaxLayer2D.enabled)
          ? a->parallaxLayer2D.order
          : 0;
  const int parallaxOrderB =
      (b->hasParallaxLayer2D && b->parallaxLayer2D.enabled)
          ? b->parallaxLayer2D.order
          : 0;
  if (parallaxOrderA != parallaxOrderB)
    return parallaxOrderA < parallaxOrderB;

  const int sortA = RuntimeUiSortOrderOf(*a);
  const int sortB = RuntimeUiSortOrderOf(*b);
  if (sortA != sortB)
    return sortA < sortB;

  return false;
}

bool IsAssemblageLayerDrawable(const SceneObject &obj) {
  return obj.hasAssemblageLayer && obj.assemblageLayer.enabled &&
         obj.assemblageLayer.layerId >= 0;
}

const SceneObject *FindAssemblageRootFor(const std::vector<SceneObject> &objects,
                                         const SceneObject &layerObj) {
  if (layerObj.hasAssemblage)
    return &layerObj;

  int currentId = layerObj.parentId;
  for (int guard = 0; guard < 64 && currentId >= 0; ++guard) {
    const SceneObject *found = nullptr;
    for (const SceneObject &candidate : objects) {
      if (candidate.id == currentId) {
        found = &candidate;
        break;
      }
    }
    if (found == nullptr)
      return nullptr;
    if (found->hasAssemblage)
      return found;
    currentId = found->parentId;
  }
  return nullptr;
}

void StableSortRuntimeUiDrawList(std::vector<SceneObject *> &drawList) {
  std::stable_sort(drawList.begin(), drawList.end(), RuntimeUiDrawOrderLess);
}

void EmitRuntimeUiBackdropBlur(ImDrawList *drawList, const SceneObject &obj,
                               const ImVec2 &rectMin, const ImVec2 &rectMax) {
  if (drawList == nullptr || !obj.hasUI)
    return;
  const float strength = std::min(1.0f, std::max(0.0f, obj.ui.backdropBlur));
  if (strength <= 0.001f)
    return;
  if (rectMax.x - rectMin.x < 1.0f || rectMax.y - rectMin.y < 1.0f)
    return;

  const ImTextureID backdrop = Modularity::UiGlassBlur::BackdropTexture();
  if (backdrop == 0)
    return;

  const ImGuiViewport *viewport = ImGui::GetWindowViewport();
  if (viewport == nullptr || viewport->Size.x < 1.0f || viewport->Size.y < 1.0f)
    return;
  const ImVec2 uvMin((rectMin.x - viewport->Pos.x) / viewport->Size.x,
                     (rectMin.y - viewport->Pos.y) / viewport->Size.y);
  const ImVec2 uvMax((rectMax.x - viewport->Pos.x) / viewport->Size.x,
                     (rectMax.y - viewport->Pos.y) / viewport->Size.y);

  if (!Modularity::UiGlassBlur::EmitBackdropCapture(drawList))
    return;
  drawList->AddImageRounded(backdrop, rectMin, rectMax, uvMin, uvMax,
                            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, strength)),
                            std::max(0.0f, obj.ui.backdropRounding));
}

bool ProjectWorldToOverlayPoint(const glm::vec3 &worldPos,
                                const glm::mat4 &view, const glm::mat4 &proj,
                                const ImVec2 &overlayPos,
                                const ImVec2 &overlaySize, ImVec2 &outScreen) {
  glm::vec4 clip = proj * view * glm::vec4(worldPos, 1.0f);
  if (clip.w <= 0.0001f) {
    return false;
  }

  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  if (ndc.z < -1.0f || ndc.z > 1.0f) {
    return false;
  }

  outScreen.x = overlayPos.x + (ndc.x * 0.5f + 0.5f) * overlaySize.x;
  outScreen.y = overlayPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * overlaySize.y;
  return true;
}

bool HasMeaningfulSpriteFrameScales(const UIElementComponent &ui) {
  if (ui.spriteCustomFrameScales.size() != ui.spriteCustomFrames.size() ||
      ui.spriteCustomFrameScales.empty()) {
    return false;
  }
  for (const glm::vec2 &scale : ui.spriteCustomFrameScales) {
    if (std::abs(scale.x - 1.0f) > 0.0001f ||
        std::abs(scale.y - 1.0f) > 0.0001f) {
      return true;
    }
  }
  return false;
}

glm::vec2 ResolveSpriteFrameScale(const UIElementComponent &ui) {
  if (!ui.spriteCustomFramesEnabled || ui.spriteCustomFrames.empty()) {
    return glm::vec2(1.0f);
  }

  const int frameCount = static_cast<int>(ui.spriteCustomFrames.size());
  const int frame = std::clamp(ui.spriteSheetFrame, 0, frameCount - 1);
  if (HasMeaningfulSpriteFrameScales(ui)) {
    const glm::vec2 authored =
        ui.spriteCustomFrameScales[static_cast<size_t>(frame)];
    return glm::vec2(std::max(0.01f, authored.x), std::max(0.01f, authored.y));
  }

  const glm::ivec4 &referenceRect = ui.spriteCustomFrames.front();
  const glm::ivec4 &frameRect =
      ui.spriteCustomFrames[static_cast<size_t>(frame)];
  const float referenceWidth = static_cast<float>(std::max(1, referenceRect.z));
  const float referenceHeight =
      static_cast<float>(std::max(1, referenceRect.w));
  return glm::vec2(
      static_cast<float>(std::max(1, frameRect.z)) / referenceWidth,
      static_cast<float>(std::max(1, frameRect.w)) / referenceHeight);
}

bool ResolveProjectedSprite25DRect(const SceneObject &obj,
                                   const glm::mat4 &view, const glm::mat4 &proj,
                                   const ImVec2 &overlayPos,
                                   const ImVec2 &overlaySize, ImVec2 &outMin,
                                   ImVec2 &outMax) {
  glm::mat4 invView = glm::inverse(view);
  glm::vec3 cameraRight = glm::normalize(glm::vec3(invView[0]));
  glm::vec3 cameraUp = glm::normalize(glm::vec3(invView[1]));
  glm::vec2 baseSize =
      glm::max(obj.ui.size, glm::vec2(0.01f)) * ResolveSpriteFrameScale(obj.ui);
  glm::vec3 objectScale = glm::max(glm::abs(obj.scale), glm::vec3(0.01f));
  glm::vec2 worldHalfExtents =
      glm::vec2(baseSize.x * objectScale.x, baseSize.y * objectScale.y) *
      0.005f;

  ImVec2 center;
  ImVec2 rightPoint;
  ImVec2 upPoint;
  if (!ProjectWorldToOverlayPoint(obj.position, view, proj, overlayPos,
                                  overlaySize, center) ||
      !ProjectWorldToOverlayPoint(
          obj.position + cameraRight * worldHalfExtents.x, view, proj,
          overlayPos, overlaySize, rightPoint) ||
      !ProjectWorldToOverlayPoint(obj.position + cameraUp * worldHalfExtents.y,
                                  view, proj, overlayPos, overlaySize,
                                  upPoint)) {
    return false;
  }

  float halfWidth = std::max(1.0f, std::abs(rightPoint.x - center.x));
  float halfHeight = std::max(1.0f, std::abs(upPoint.y - center.y));
  outMin = ImVec2(center.x - halfWidth, center.y - halfHeight);
  outMax = ImVec2(center.x + halfWidth, center.y + halfHeight);
  return true;
}

void ApplyUIFontFilterCallback(const ImDrawList *, const ImDrawCmd *cmd) {
  if (!cmd)
    return;
  if (glfwGetCurrentContext() == nullptr)
    return;

  const intptr_t mode = reinterpret_cast<intptr_t>(cmd->UserCallbackData);
  const bool usePoint = (mode == 1);
  const ImTextureID fontTexRef = ImGui::GetIO().Fonts->TexRef.GetTexID();
  const uintptr_t rawTextureId = (uintptr_t)fontTexRef;
  const GLuint fontTextureId = static_cast<GLuint>(rawTextureId);
  if (fontTextureId == 0)
    return;

  glBindTexture(GL_TEXTURE_2D, fontTextureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  usePoint ? GL_NEAREST : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                  usePoint ? GL_NEAREST : GL_LINEAR);
}

void AppendWrappedTextSegment(ImFont *font, float fontSize, const char *start,
                              const char *end, float wrapWidth, bool autoWrap,
                              std::vector<std::string> &outLines) {
  if (!start || !end || end < start)
    return;
  if (start == end) {
    outLines.emplace_back();
    return;
  }

  if (!autoWrap || wrapWidth <= 1.0f) {
    outLines.emplace_back(start, end);
    return;
  }

  const char *cursor = start;
  while (cursor < end) {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
      ++cursor;
    }
    if (cursor >= end) {
      outLines.emplace_back();
      break;
    }

    const char *wrapPos =
        font->CalcWordWrapPositionA(fontSize, cursor, end, wrapWidth);
    if (!wrapPos || wrapPos <= cursor) {
      unsigned int codepoint = 0;
      int bytes = ImTextCharFromUtf8(&codepoint, cursor, end);
      if (bytes <= 0)
        bytes = 1;
      wrapPos = std::min(end, cursor + bytes);
    }

    const char *trimmedEnd = wrapPos;
    while (trimmedEnd > cursor &&
           (trimmedEnd[-1] == ' ' || trimmedEnd[-1] == '\t')) {
      --trimmedEnd;
    }
    outLines.emplace_back(cursor, trimmedEnd);
    cursor = wrapPos;
  }
}

std::vector<std::string> BuildWrappedTextLines(ImFont *font, float fontSize,
                                               const char *text,
                                               float wrapWidth, bool autoWrap) {
  std::vector<std::string> lines;
  if (!font || !text)
    return lines;

  const char *lineStart = text;
  const char *cursor = text;
  while (true) {
    if (*cursor == '\n' || *cursor == '\0') {
      AppendWrappedTextSegment(font, fontSize, lineStart, cursor, wrapWidth,
                               autoWrap, lines);
      if (*cursor == '\0')
        break;
      lineStart = cursor + 1;
    }
    ++cursor;
  }
  return lines;
}

void DrawUITextLineWithEffects(ImDrawList *drawList, ImFont *font,
                               float fontSize, const ImVec2 &linePos,
                               ImU32 baseColor, const char *lineText,
                               int effectFlags, float effectSpeed,
                               float effectIntensity,
                               const UITextRevealParams &reveal,
                               int *glyphOffset) {
  if (!drawList || !font || !lineText || !*lineText)
    return;
  const bool effectsActive = (effectFlags != 0 && effectIntensity > 0.0f);
  const bool revealActive = reveal.active();
  if (!effectsActive && !revealActive) {
    drawList->AddText(font, fontSize, linePos, baseColor, lineText);
    return;
  }

  const int r = (baseColor >> IM_COL32_R_SHIFT) & 0xFF;
  const int g = (baseColor >> IM_COL32_G_SHIFT) & 0xFF;
  const int b = (baseColor >> IM_COL32_B_SHIFT) & 0xFF;
  const int a = (baseColor >> IM_COL32_A_SHIFT) & 0xFF;

  const float time = static_cast<float>(ImGui::GetTime());
  const float speed = std::max(0.01f, effectSpeed);
  const float amplitude =
      std::max(0.0f, effectIntensity) * std::max(1.0f, fontSize * 0.15f);

  float penX = linePos.x;
  int glyphIndex = glyphOffset ? *glyphOffset : 0;
  const char *cursor = lineText;
  while (*cursor != '\0') {
    unsigned int codepoint = 0;
    int bytes = ImTextCharFromUtf8(&codepoint, cursor, nullptr);
    if (bytes <= 0)
      bytes = 1;

    char glyphBuf[8] = {};
    const int copyBytes =
        std::min(bytes, static_cast<int>(sizeof(glyphBuf) - 1));
    std::memcpy(glyphBuf, cursor, static_cast<size_t>(copyBytes));
    glyphBuf[copyBytes] = '\0';

    const float advance =
        font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, glyphBuf).x;
    ImVec2 glyphPos(penX, linePos.y);
    ImU32 glyphColor = baseColor;
    const bool whitespace = (codepoint == ' ' || codepoint == '\t');

    float glyphScale = 1.0f;
    float glyphAlphaMul = 1.0f;
    if (revealActive) {
      const float past = reveal.revealChars - static_cast<float>(glyphIndex);
      if (past <= 0.0f) {
        penX += advance;
        cursor += bytes;
        ++glyphIndex;
        continue;
      }
      const float softness = std::max(0.0001f, reveal.softness);
      if (past < softness) {
        const float t = std::clamp(past / softness, 0.0f, 1.0f);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        glyphScale = reveal.popScale + (1.0f - reveal.popScale) * eased;
        glyphAlphaMul *= eased;
      }
    }

    if (effectsActive && !whitespace) {
      const float phase =
          time * speed * 2.0f + static_cast<float>(glyphIndex) * 0.45f;
      if ((effectFlags & (1 << 0)) != 0) {
        glyphPos.y += std::sin(phase) * amplitude;
      }
      if ((effectFlags & (1 << 1)) != 0) {
        glyphPos.x += std::sin(phase * 12.7f) * amplitude * 0.35f;
        glyphPos.y += std::cos(phase * 17.3f) * amplitude * 0.35f;
      }
      if ((effectFlags & (1 << 2)) != 0) {
        glyphPos.y -= std::abs(std::sin(phase)) * amplitude * 1.15f;
      }
      if ((effectFlags & (1 << 3)) != 0) {
        glyphPos.x += std::cos(phase) * amplitude * 0.6f;
        glyphPos.y += std::sin(phase) * amplitude * 0.6f;
      }
      if ((effectFlags & (1 << 4)) != 0) {
        glyphAlphaMul *= 0.35f + 0.65f * (0.5f + 0.5f * std::sin(phase));
      }
    }

    if (std::abs(glyphAlphaMul - 1.0f) > 1e-4f) {
      glyphColor = IM_COL32(r, g, b,
                            static_cast<int>(std::clamp(
                                glyphAlphaMul * static_cast<float>(a),
                                0.0f, 255.0f)));
    }

    if (std::abs(glyphScale - 1.0f) > 1e-4f) {
      const float scaledSize = std::max(1.0f, fontSize * glyphScale);
      const ImVec2 scaledPos(
          glyphPos.x + (advance - advance * glyphScale) * 0.5f,
          glyphPos.y + (fontSize - scaledSize) * 0.5f);
      drawList->AddText(font, scaledSize, scaledPos, glyphColor, glyphBuf,
                        glyphBuf + copyBytes);
    } else {
      drawList->AddText(font, fontSize, glyphPos, glyphColor, glyphBuf,
                        glyphBuf + copyBytes);
    }
    penX += advance;
    cursor += bytes;
    ++glyphIndex;
  }
  if (glyphOffset)
    *glyphOffset = glyphIndex;
}

void AddUITextWithFilter(ImDrawList *drawList,
                         MaterialProperties::TextureFilter filter, ImFont *font,
                         float fontSize, const ImVec2 &drawMin,
                         const ImVec2 &drawMax, ImU32 color, const char *text,
                         bool autoWrap, UITextHAlign hAlign,
                         UITextVAlign vAlign, int effectFlags,
                         float effectSpeed, float effectIntensity,
                         float stableRasterFontSize,
                         const UITextRevealParams &reveal) {
  if (!drawList || !font || !text || !*text)
    return;

  const float safeFontSize = std::max(1.0f, fontSize);
  const float rasterFontSize =
      stableRasterFontSize > 0.0f ? std::max(1.0f, stableRasterFontSize)
                                 : safeFontSize;
  const float viewportTextScale = safeFontSize / rasterFontSize;
  const ImVec2 layoutMax(
      drawMin.x + (drawMax.x - drawMin.x) / viewportTextScale,
      drawMin.y + (drawMax.y - drawMin.y) / viewportTextScale);
  const ImVec2 contentMin(drawMin.x + 4.0f, drawMin.y + 2.0f);
  const ImVec2 contentMax(layoutMax.x - 4.0f, layoutMax.y - 2.0f);
  const float contentWidth = std::max(1.0f, contentMax.x - contentMin.x);
  const float contentHeight = std::max(1.0f, contentMax.y - contentMin.y);

  std::vector<std::string> lines =
      BuildWrappedTextLines(font, rasterFontSize, text, contentWidth, autoWrap);
  if (lines.empty())
    return;

  const float lineHeight = rasterFontSize;
  const float totalHeight = lineHeight * static_cast<float>(lines.size());
  float startY = contentMin.y;
  if (vAlign == UITextVAlign::Middle) {
    startY = contentMin.y + (contentHeight - totalHeight) * 0.5f;
  } else if (vAlign == UITextVAlign::Bottom) {
    startY = contentMax.y - totalHeight;
  }
  startY = std::max(contentMin.y, startY);

  const bool remapClipToLayoutSpace =
      std::abs(viewportTextScale - 1.0f) > 1e-4f;
  const ImVec2 screenClipMin = drawList->GetClipRectMin();
  const ImVec2 screenClipMax = drawList->GetClipRectMax();
  int clipPatchFirstCmd = 0;
  if (remapClipToLayoutSpace) {
    const float invTextScale = 1.0f / viewportTextScale;
    drawList->PushClipRect(
        ImVec2(drawMin.x + (screenClipMin.x - drawMin.x) * invTextScale,
               drawMin.y + (screenClipMin.y - drawMin.y) * invTextScale),
        ImVec2(drawMin.x + (screenClipMax.x - drawMin.x) * invTextScale,
               drawMin.y + (screenClipMax.y - drawMin.y) * invTextScale),
        false);
    if (drawList->CmdBuffer.Size > 0 &&
        drawList->CmdBuffer.back().ElemCount != 0) {
      drawList->AddDrawCmd();
    }
    clipPatchFirstCmd = std::max(0, drawList->CmdBuffer.Size - 1);
  }

  if (filter == MaterialProperties::TextureFilter::Point) {
    drawList->AddCallback(ApplyUIFontFilterCallback,
                          reinterpret_cast<void *>(static_cast<intptr_t>(1)));
  }

  const int textVtxStart = drawList->VtxBuffer.Size;
  int revealGlyphCursor = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string &line = lines[i];
    const float y = startY + static_cast<float>(i) * lineHeight;
    if (y > contentMax.y || y + lineHeight < contentMin.y) {
      if (reveal.active()) {
        const char *scan = line.c_str();
        while (*scan != '\0') {
          unsigned int cp = 0;
          int n = ImTextCharFromUtf8(&cp, scan, nullptr);
          if (n <= 0)
            n = 1;
          scan += n;
          ++revealGlyphCursor;
        }
      }
      continue;
    }

    const float lineWidth =
        font->CalcTextSizeA(rasterFontSize, FLT_MAX, 0.0f, line.c_str()).x;
    float x = contentMin.x;
    if (hAlign == UITextHAlign::Center) {
      x = contentMin.x + (contentWidth - lineWidth) * 0.5f;
    } else if (hAlign == UITextHAlign::Right) {
      x = contentMax.x - lineWidth;
    }
    x = std::max(contentMin.x, x);

    DrawUITextLineWithEffects(drawList, font, rasterFontSize, ImVec2(x, y),
                              color, line.c_str(), effectFlags, effectSpeed,
                              effectIntensity, reveal, &revealGlyphCursor);
  }

  if (remapClipToLayoutSpace) {
    ScaleDrawListVertices(
        drawList, textVtxStart, drawList->VtxBuffer.Size, drawMin,
        ImVec2(viewportTextScale, viewportTextScale));
  }

  if (filter == MaterialProperties::TextureFilter::Point) {
    drawList->AddCallback(ApplyUIFontFilterCallback,
                          reinterpret_cast<void *>(static_cast<intptr_t>(0)));
  }

  if (remapClipToLayoutSpace) {
    const int clipPatchEndCmd = drawList->CmdBuffer.Size;
    drawList->PopClipRect();
    const ImVec4 screenClip(screenClipMin.x, screenClipMin.y, screenClipMax.x,
                            screenClipMax.y);
    for (int i = clipPatchFirstCmd;
         i < clipPatchEndCmd && i < drawList->CmdBuffer.Size; ++i) {
      drawList->CmdBuffer[i].ClipRect = screenClip;
    }
  }
}

void RenderUISliderStyle(ImDrawList *dl, UISliderStyle style,
                         const ImVec2 &drawMin, const ImVec2 &drawMax,
                         const ImVec2 &drawSize,
                         ImU32 bg, ImU32 fillColor, ImU32 border, ImU32 textColor,
                         float t, float minValue, float maxValue,
                         const char *label) {
  if (!dl) return;
  const char *safeLabel = label ? label : "";
  const float clampedT = std::clamp(t, 0.0f, 1.0f);

  if (style == UISliderStyle::Fill) {
    const float rounding = std::min(drawSize.x, drawSize.y) * 0.5f;
    ImVec2 fillMax(drawMin.x + drawSize.x * clampedT, drawMax.y);
    dl->AddRectFilled(drawMin, drawMax, bg, rounding);
    if (fillMax.x > drawMin.x) {
      dl->AddRectFilled(drawMin, fillMax, fillColor, rounding);
    }
    dl->AddRect(drawMin, drawMax, border, rounding);
    ImVec2 textSize = ImGui::CalcTextSize(safeLabel);
    ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                   drawMin.y + (drawSize.y - textSize.y) * 0.5f);
    dl->AddText(textPos, textColor, safeLabel);
    return;
  }

  if (style == UISliderStyle::Vertical) {
    const float rounding = std::min(drawSize.x, drawSize.y) * 0.5f;
    ImVec2 fillMin(drawMin.x, drawMax.y - drawSize.y * clampedT);
    dl->AddRectFilled(drawMin, drawMax, bg, rounding);
    if (fillMin.y < drawMax.y) {
      dl->AddRectFilled(fillMin, drawMax, fillColor, rounding);
    }
    dl->AddRect(drawMin, drawMax, border, rounding);
    ImVec2 textSize = ImGui::CalcTextSize(safeLabel);
    ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                   drawMin.y + (drawSize.y - textSize.y) * 0.5f);
    dl->AddText(textPos, textColor, safeLabel);
    return;
  }

  if (style == UISliderStyle::Stepped) {
    const float rounding = 4.0f;
    dl->AddRectFilled(drawMin, drawMax, bg, rounding);
    int steps = static_cast<int>(std::round(maxValue - minValue));
    if (steps < 2 || steps > 64) steps = 10;
    const float pipPad = 3.0f;
    const float pipW =
        std::max(2.0f, (drawSize.x - pipPad * (steps + 1)) /
                            static_cast<float>(steps));
    ImVec4 bgVec = ImGui::ColorConvertU32ToFloat4(bg);
    const ImU32 inactivePip = ImGui::GetColorU32(ImVec4(
        std::min(1.0f, bgVec.x + 0.12f), std::min(1.0f, bgVec.y + 0.12f),
        std::min(1.0f, bgVec.z + 0.12f), bgVec.w));
    for (int i = 0; i < steps; ++i) {
      float fraction = (i + 1) / static_cast<float>(steps);
      ImU32 col = (fraction <= clampedT + 1e-4f) ? fillColor : inactivePip;
      float x0 = drawMin.x + pipPad + i * (pipW + pipPad);
      ImVec2 a(x0, drawMin.y + pipPad);
      ImVec2 b(x0 + pipW, drawMax.y - pipPad);
      dl->AddRectFilled(a, b, col, 2.0f);
    }
    dl->AddRect(drawMin, drawMax, border, rounding);
    return;
  }

  if (style == UISliderStyle::Circle || style == UISliderStyle::Ring) {
    ImVec2 center((drawMin.x + drawMax.x) * 0.5f,
                  (drawMin.y + drawMax.y) * 0.5f);
    float radius =
        std::max(2.0f, std::min(drawSize.x, drawSize.y) * 0.5f - 2.0f);
    const float start = -IM_PI * 0.5f;
    const float twoPi = IM_PI * 2.0f;
    const float arcEnd = start + clampedT * twoPi;
    const int totalSegs = 64;

    if (style == UISliderStyle::Circle) {
      dl->AddCircleFilled(center, radius, bg, 32);
      if (clampedT > 0.001f) {
        const int segs =
            std::max(1, static_cast<int>(std::ceil(totalSegs * clampedT)));
        const float aStep = (arcEnd - start) / static_cast<float>(segs);
        for (int i = 0; i < segs; ++i) {
          float a0 = start + aStep * i;
          float a1 = start + aStep * (i + 1);
          ImVec2 p0(center.x + std::cos(a0) * radius,
                    center.y + std::sin(a0) * radius);
          ImVec2 p1(center.x + std::cos(a1) * radius,
                    center.y + std::sin(a1) * radius);
          dl->AddTriangleFilled(center, p0, p1, fillColor);
        }
      }
      dl->AddCircle(center, radius, border, 32, 2.0f);
    } else { // Ring
      const float ringThickness = std::max(3.0f, radius * 0.18f);
      dl->AddCircle(center, radius, bg, 64, ringThickness);
      if (clampedT > 0.001f) {
        dl->PathClear();
        dl->PathArcTo(
            center, radius, start, arcEnd,
            std::max(2, static_cast<int>(std::ceil(totalSegs * clampedT))));
        dl->PathStroke(fillColor, ImDrawFlags_None, ringThickness);
      }
      dl->AddCircle(center, radius + ringThickness * 0.5f, border, 64, 1.0f);
    }
    ImVec2 textSize = ImGui::CalcTextSize(safeLabel);
    ImVec2 textPos(center.x - textSize.x * 0.5f,
                   center.y - textSize.y * 0.5f);
    dl->AddText(textPos, textColor, safeLabel);
    return;
  }
}

void RotateDrawListVertices(ImDrawList *dl, int firstVertex, int lastVertex,
                            const ImVec2 &pivot, float angleRadians) {
  if (!dl || std::abs(angleRadians) < 1e-4f) return;
  const float cs = std::cos(angleRadians);
  const float sn = std::sin(angleRadians);
  const int total = dl->VtxBuffer.Size;
  const int first = std::clamp(firstVertex, 0, total);
  const int last = std::clamp(lastVertex, first, total);
  ImDrawVert *v = dl->VtxBuffer.Data;
  for (int i = first; i < last; ++i) {
    const float dx = v[i].pos.x - pivot.x;
    const float dy = v[i].pos.y - pivot.y;
    v[i].pos.x = pivot.x + dx * cs - dy * sn;
    v[i].pos.y = pivot.y + dx * sn + dy * cs;
  }
}

void ScaleDrawListVertices(ImDrawList *dl, int firstVertex, int lastVertex,
                           const ImVec2 &pivot, const ImVec2 &scale) {
  if (!dl || (std::abs(scale.x - 1.0f) < 1e-4f &&
              std::abs(scale.y - 1.0f) < 1e-4f)) {
    return;
  }
  const int total = dl->VtxBuffer.Size;
  const int first = std::clamp(firstVertex, 0, total);
  const int last = std::clamp(lastVertex, first, total);
  ImDrawVert *v = dl->VtxBuffer.Data;
  for (int i = first; i < last; ++i) {
    v[i].pos.x = pivot.x + (v[i].pos.x - pivot.x) * scale.x;
    v[i].pos.y = pivot.y + (v[i].pos.y - pivot.y) * scale.y;
  }
}

float ComputeViewportTextFontSize(float baseFontSize, float textScale,
                                  bool useWorldUi, float worldUiZoom,
                                  float viewportRenderScale) {
  const float safeBaseFontSize = std::max(1.0f, baseFontSize);
  const float safeTextScale = std::max(0.1f, textScale);
  const float safeViewportScale = std::max(0.01f, viewportRenderScale);
  if (useWorldUi) {
    const float worldScale = std::max(0.01f, worldUiZoom / 100.0f);
    return std::max(1.0f, safeBaseFontSize * safeTextScale * worldScale * safeViewportScale);
  }
  return std::max(1.0f, safeBaseFontSize * safeTextScale * safeViewportScale);
}

float ResolveViewportUITextAuthoredFontSize(float baseFontSize,
                                            float textScale,
                                            float explicitFontSize) {
  return (explicitFontSize > 0.0f)
             ? explicitFontSize
             : std::max(1.0f, baseFontSize) * std::max(0.1f, textScale);
}

float ResolveViewportUITextFontSize(float baseFontSize, float textScale,
                                    float explicitFontSize, bool useWorldUi,
                                    float worldUiZoom,
                                    float viewportRenderScale,
                                    float elementScaleY) {
  // obj.scale on a Text element is intentionally not applied to the glyph
  // RectTransform scale works in TextMeshPro). The elementScaleY parameter
  // is retained for callers that still want to bake transform scale into
  // the glyph height directly, but the standard viewport path passes 1.0.
  const float authoredFontSize = ResolveViewportUITextAuthoredFontSize(
      baseFontSize, textScale, explicitFontSize);
  const float safeElementScale = std::max(0.01f, std::abs(elementScaleY));
  const float safeViewportScale = std::max(0.01f, viewportRenderScale);
  const float worldScale =
      useWorldUi ? std::max(0.01f, worldUiZoom / 100.0f) : 1.0f;
  return std::max(1.0f, authoredFontSize * safeElementScale * worldScale *
                            safeViewportScale);
}

void ApplyUITextScaleFromRectResize(SceneObject &obj, float startTextScale,
                                    float startFontSize,
                                    const ImVec2 &startScreenSize,
                                    const ImVec2 &newScreenSize) {
  if (obj.ui.type != UIElementType::Text || startScreenSize.y <= 0.01f)
    return;

  const float heightRatio =
      std::max(0.01f, newScreenSize.y) / std::max(0.01f, startScreenSize.y);
  if (std::abs(heightRatio - 1.0f) < 0.001f)
    return;

  if (startFontSize > 0.0f) {
    obj.ui.fontSize = std::max(0.1f, startFontSize * heightRatio);
  } else {
    obj.ui.fontSize = 0.0f;
    obj.ui.textScale = std::max(0.1f, startTextScale * heightRatio);
  }
}

ImVec2 ResolveUiSourceFrameSizePx(const SceneObject &obj, int frame,
                                  const Texture *texture) {
  if (obj.ui.spriteCustomFramesEnabled && !obj.ui.spriteCustomFrames.empty()) {
    const int clampedFrame = std::clamp(
        frame, 0, static_cast<int>(obj.ui.spriteCustomFrames.size()) - 1);
    const glm::ivec4 &rect =
        obj.ui.spriteCustomFrames[static_cast<size_t>(clampedFrame)];
    return ImVec2(static_cast<float>(std::max(1, rect.z)),
                  static_cast<float>(std::max(1, rect.w)));
  }

  if (texture != nullptr) {
    float frameWidth = static_cast<float>(std::max(1, texture->GetWidth()));
    float frameHeight = static_cast<float>(std::max(1, texture->GetHeight()));
    if (obj.ui.spriteSheetEnabled) {
      frameWidth = std::max(
          1.0f, frameWidth /
                    static_cast<float>(std::max(1, obj.ui.spriteSheetColumns)));
      frameHeight = std::max(
          1.0f, frameHeight /
                    static_cast<float>(std::max(1, obj.ui.spriteSheetRows)));
    }
    return ImVec2(frameWidth, frameHeight);
  }

  return ImVec2(std::max(1.0f, obj.ui.size.x), std::max(1.0f, obj.ui.size.y));
}

bool DrawNineSliceSprite(BatchedSpriteEmitter &spriteBatch,
                         ImTextureID textureId, const SceneObject &obj,
                         const ImVec2 &drawMin, const ImVec2 &drawMax,
                         const std::array<ImVec2, 4> &uvQuad,
                         const ImVec2 &sourceFrameSizePx, float angleRad,
                         ImU32 color) {
  if (!obj.ui.nineSliceEnabled || textureId == 0) {
    return false;
  }

  const float rectWidth = drawMax.x - drawMin.x;
  const float rectHeight = drawMax.y - drawMin.y;
  if (rectWidth <= 1.0f || rectHeight <= 1.0f) {
    return false;
  }

  const float srcW = std::max(1.0f, sourceFrameSizePx.x);
  const float srcH = std::max(1.0f, sourceFrameSizePx.y);

  float srcLeft = std::max(0.0f, obj.ui.nineSliceBorder.x);
  float srcRight = std::max(0.0f, obj.ui.nineSliceBorder.y);
  float srcTop = std::max(0.0f, obj.ui.nineSliceBorder.z);
  float srcBottom = std::max(0.0f, obj.ui.nineSliceBorder.w);

  if (srcLeft + srcRight > srcW) {
    const float k = srcW / std::max(1e-4f, srcLeft + srcRight);
    srcLeft *= k;
    srcRight *= k;
  }
  if (srcTop + srcBottom > srcH) {
    const float k = srcH / std::max(1e-4f, srcTop + srcBottom);
    srcTop *= k;
    srcBottom *= k;
  }

  const float uniformScale =
      std::max(0.01f, std::min(rectWidth / srcW, rectHeight / srcH));
  float dstLeft = srcLeft * uniformScale;
  float dstRight = srcRight * uniformScale;
  float dstTop = srcTop * uniformScale;
  float dstBottom = srcBottom * uniformScale;

  if (dstLeft + dstRight > rectWidth) {
    const float k = rectWidth / std::max(1e-4f, dstLeft + dstRight);
    dstLeft *= k;
    dstRight *= k;
  }
  if (dstTop + dstBottom > rectHeight) {
    const float k = rectHeight / std::max(1e-4f, dstTop + dstBottom);
    dstTop *= k;
    dstBottom *= k;
  }

  const float x[4] = {drawMin.x, drawMin.x + dstLeft, drawMax.x - dstRight,
                      drawMax.x};
  const float y[4] = {drawMin.y, drawMin.y + dstTop, drawMax.y - dstBottom,
                      drawMax.y};

  const float sx[4] = {0.0f, srcLeft / srcW, 1.0f - srcRight / srcW, 1.0f};
  const float sy[4] = {0.0f, srcTop / srcH, 1.0f - srcBottom / srcH, 1.0f};

  const float uStart = uvQuad[0].x;
  const float uEnd = uvQuad[2].x;
  const float vStart = uvQuad[0].y;
  const float vEnd = uvQuad[2].y;
  const ImVec2 center((drawMin.x + drawMax.x) * 0.5f,
                      (drawMin.y + drawMax.y) * 0.5f);
  const float c = std::cos(angleRad);
  const float s = std::sin(angleRad);
  const bool rotate = std::abs(angleRad) > 1e-4f;

  auto remapU = [&](float t) { return uStart + (uEnd - uStart) * t; };
  auto remapV = [&](float t) { return vStart + (vEnd - vStart) * t; };
  auto rotatePoint = [&](ImVec2 &point) {
    if (!rotate)
      return;
    const float dx = point.x - center.x;
    const float dy = point.y - center.y;
    point.x = center.x + dx * c - dy * s;
    point.y = center.y + dx * s + dy * c;
  };
  auto emit = [&](float x0, float x1, float y0, float y1, float tx0, float tx1,
                  float ty0, float ty1) {
    if (x1 <= x0 || y1 <= y0)
      return;
    ImVec2 p0(x0, y0);
    ImVec2 p1(x1, y0);
    ImVec2 p2(x1, y1);
    ImVec2 p3(x0, y1);
    rotatePoint(p0);
    rotatePoint(p1);
    rotatePoint(p2);
    rotatePoint(p3);
    spriteBatch.push(
        textureId, p0, p1, p2, p3, ImVec2(remapU(tx0), remapV(ty0)),
        ImVec2(remapU(tx1), remapV(ty0)), ImVec2(remapU(tx1), remapV(ty1)),
        ImVec2(remapU(tx0), remapV(ty1)), color);
  };
  auto emitTiledX = [&](float x0, float x1, float y0, float y1, float tx0,
                        float tx1, float ty0, float ty1, float tileWidth) {
    if (x1 <= x0 || y1 <= y0 || tileWidth <= 0.01f)
      return;
    const int tileCount = static_cast<int>(std::ceil((x1 - x0) / tileWidth));
    if (tileCount > 2048) {
      emit(x0, x1, y0, y1, tx0, tx1, ty0, ty1);
      return;
    }
    float cursor = x0;
    while (cursor < x1 - 0.001f) {
      const float next = std::min(cursor + tileWidth, x1);
      const float frac = (next - cursor) / tileWidth;
      emit(cursor, next, y0, y1, tx0, tx0 + (tx1 - tx0) * frac, ty0, ty1);
      cursor = next;
    }
  };
  auto emitTiledY = [&](float x0, float x1, float y0, float y1, float tx0,
                        float tx1, float ty0, float ty1, float tileHeight) {
    if (x1 <= x0 || y1 <= y0 || tileHeight <= 0.01f)
      return;
    const int tileCount = static_cast<int>(std::ceil((y1 - y0) / tileHeight));
    if (tileCount > 2048) {
      emit(x0, x1, y0, y1, tx0, tx1, ty0, ty1);
      return;
    }
    float cursor = y0;
    while (cursor < y1 - 0.001f) {
      const float next = std::min(cursor + tileHeight, y1);
      const float frac = (next - cursor) / tileHeight;
      emit(x0, x1, cursor, next, tx0, tx1, ty0, ty0 + (ty1 - ty0) * frac);
      cursor = next;
    }
  };
  auto emitTiledXY = [&](float x0, float x1, float y0, float y1, float tx0,
                         float tx1, float ty0, float ty1, float tileWidth,
                         float tileHeight) {
    if (x1 <= x0 || y1 <= y0 || tileWidth <= 0.01f || tileHeight <= 0.01f)
      return;
    const int tilesX = static_cast<int>(std::ceil((x1 - x0) / tileWidth));
    const int tilesY = static_cast<int>(std::ceil((y1 - y0) / tileHeight));
    if (tilesX <= 0 || tilesY <= 0 ||
        static_cast<long long>(tilesX) * static_cast<long long>(tilesY) >
            2048) {
      emit(x0, x1, y0, y1, tx0, tx1, ty0, ty1);
      return;
    }
    float yCursor = y0;
    while (yCursor < y1 - 0.001f) {
      const float yNext = std::min(yCursor + tileHeight, y1);
      const float yFrac = (yNext - yCursor) / tileHeight;
      float xCursor = x0;
      while (xCursor < x1 - 0.001f) {
        const float xNext = std::min(xCursor + tileWidth, x1);
        const float xFrac = (xNext - xCursor) / tileWidth;
        emit(xCursor, xNext, yCursor, yNext, tx0, tx0 + (tx1 - tx0) * xFrac,
             ty0, ty0 + (ty1 - ty0) * yFrac);
        xCursor = xNext;
      }
      yCursor = yNext;
    }
  };

  // Corners
  emit(x[0], x[1], y[0], y[1], sx[0], sx[1], sy[0], sy[1]);
  emit(x[2], x[3], y[0], y[1], sx[2], sx[3], sy[0], sy[1]);
  emit(x[0], x[1], y[2], y[3], sx[0], sx[1], sy[2], sy[3]);
  emit(x[2], x[3], y[2], y[3], sx[2], sx[3], sy[2], sy[3]);

  // Edges
  const float centerSrcW = std::max(0.0f, srcW - srcLeft - srcRight);
  const float centerSrcH = std::max(0.0f, srcH - srcTop - srcBottom);
  const float centerDstTileW = centerSrcW * uniformScale;
  const float centerDstTileH = centerSrcH * uniformScale;

  if (obj.ui.nineSliceTileEdges && centerSrcW > 0.01f) {
    emitTiledX(x[1], x[2], y[0], y[1], sx[1], sx[2], sy[0], sy[1],
               centerDstTileW);
    emitTiledX(x[1], x[2], y[2], y[3], sx[1], sx[2], sy[2], sy[3],
               centerDstTileW);
  } else {
    emit(x[1], x[2], y[0], y[1], sx[1], sx[2], sy[0], sy[1]);
    emit(x[1], x[2], y[2], y[3], sx[1], sx[2], sy[2], sy[3]);
  }

  if (obj.ui.nineSliceTileEdges && centerSrcH > 0.01f) {
    emitTiledY(x[0], x[1], y[1], y[2], sx[0], sx[1], sy[1], sy[2],
               centerDstTileH);
    emitTiledY(x[2], x[3], y[1], y[2], sx[2], sx[3], sy[1], sy[2],
               centerDstTileH);
  } else {
    emit(x[0], x[1], y[1], y[2], sx[0], sx[1], sy[1], sy[2]);
    emit(x[2], x[3], y[1], y[2], sx[2], sx[3], sy[1], sy[2]);
  }

  // Center
  if (obj.ui.nineSliceTileCenter && centerSrcW > 0.01f && centerSrcH > 0.01f) {
    emitTiledXY(x[1], x[2], y[1], y[2], sx[1], sx[2], sy[1], sy[2],
                centerDstTileW, centerDstTileH);
  } else {
    emit(x[1], x[2], y[1], y[2], sx[1], sx[2], sy[1], sy[2]);
  }

  return true;
}

bool DrawScrollingUiSprite(BatchedSpriteEmitter &spriteBatch,
                           ImTextureID textureId, const SceneObject &obj,
                           const ImVec2 &drawMin, const ImVec2 &drawMax,
                           float angleRad, ImU32 color, double timeSeconds) {
  if (!obj.material.uvScrollEnabled || textureId == 0) {
    return false;
  }
  const float width = drawMax.x - drawMin.x;
  const float height = drawMax.y - drawMin.y;
  if (width <= 1.0f || height <= 1.0f) {
    return false;
  }

  glm::vec2 dir = obj.material.scrollDirection;
  const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
  if (len < 1e-6f) {
    return false;
  }
  dir /= len;

  const double drift = static_cast<double>(obj.material.scrollSpeed) * timeSeconds;
  auto wrap01 = [](double v) {
    double f = std::fmod(v, 1.0);
    if (f < 0.0) f += 1.0;
    return static_cast<float>(f);
  };
  const float offU = wrap01(drift * static_cast<double>(dir.x));
  const float offV = wrap01(drift * static_cast<double>(dir.y));

  const float splitX = drawMin.x + (1.0f - offU) * width;
  const float splitY = drawMin.y + (1.0f - offV) * height;

  const float xs[3] = {drawMin.x, splitX, drawMax.x};
  const float ys[3] = {drawMin.y, splitY, drawMax.y};
  const float us[3] = {offU, 1.0f, offU};
  const float vs[3] = {offV, 1.0f, offV};

  const ImVec2 center((drawMin.x + drawMax.x) * 0.5f,
                      (drawMin.y + drawMax.y) * 0.5f);
  const ImVec2 half(width * 0.5f, height * 0.5f);
  const bool rotated = std::abs(angleRad) > 1e-4f;
  const float c = std::cos(angleRad);
  const float s = std::sin(angleRad);
  const ImVec2 pivotOffset = UiAnchorPivot(obj.ui.anchor, ImVec2(width, height));
  const float px = pivotOffset.x - half.x;
  const float py = pivotOffset.y - half.y;
  auto place = [&](float x, float y) {
    if (!rotated) return ImVec2(x, y);
    const float dx = (x - center.x) - px;
    const float dy = (y - center.y) - py;
    return ImVec2(center.x + px + dx * c - dy * s,
                  center.y + py + dx * s + dy * c);
  };

  for (int iy = 0; iy < 2; ++iy) {
    for (int ix = 0; ix < 2; ++ix) {
      const float x0 = ix == 0 ? xs[0] : xs[1];
      const float x1 = ix == 0 ? xs[1] : xs[2];
      const float y0 = iy == 0 ? ys[0] : ys[1];
      const float y1 = iy == 0 ? ys[1] : ys[2];
      if (x1 - x0 <= 0.01f || y1 - y0 <= 0.01f) continue;
      const float u0 = ix == 0 ? us[0] : 0.0f;
      const float u1 = ix == 0 ? us[1] : us[2];
      const float v0 = iy == 0 ? vs[0] : 0.0f;
      const float v1 = iy == 0 ? vs[1] : vs[2];
      const ImVec2 uv0(u0, 1.0f - v0);
      const ImVec2 uv1(u1, 1.0f - v0);
      const ImVec2 uv2(u1, 1.0f - v1);
      const ImVec2 uv3(u0, 1.0f - v1);
      spriteBatch.push(textureId, place(x0, y0), place(x1, y0), place(x1, y1),
                       place(x0, y1), uv0, uv1, uv2, uv3, color);
    }
  }
  return true;
}

glm::vec2 ResolvePseudo3DLayoutSize(const SceneObject &canvas) {
  const glm::vec2 fallback(std::max(1.0f, canvas.ui.size.x),
                           std::max(1.0f, canvas.ui.size.y));
  if (canvas.ui.pseudo3DPanelSize.x > 0.0f &&
      canvas.ui.pseudo3DPanelSize.y > 0.0f) {
    return glm::vec2(std::max(1.0f, canvas.ui.pseudo3DPanelSize.x),
                     std::max(1.0f, canvas.ui.pseudo3DPanelSize.y));
  }
  return fallback;
}

float Cross2D(const ImVec2 &a, const ImVec2 &b) {
  return a.x * b.y - a.y * b.x;
}

bool PointInTriangleBarycentric(const ImVec2 &p, const ImVec2 &a,
                                const ImVec2 &b, const ImVec2 &c, float &wa,
                                float &wb, float &wc) {
  const ImVec2 v0 = ImVec2(b.x - a.x, b.y - a.y);
  const ImVec2 v1 = ImVec2(c.x - a.x, c.y - a.y);
  const ImVec2 v2 = ImVec2(p.x - a.x, p.y - a.y);
  const float denom = Cross2D(v0, v1);
  if (std::abs(denom) <= 1e-6f) {
    return false;
  }
  wb = Cross2D(v2, v1) / denom;
  wc = Cross2D(v0, v2) / denom;
  wa = 1.0f - wb - wc;
  const float eps = -1e-4f;
  return wa >= eps && wb >= eps && wc >= eps;
}

bool MapPointToPseudo3DQuadUV(const std::array<ImVec2, 4> &corners,
                              const ImVec2 &point, ImVec2 &outUv) {
  float w0 = 0.0f;
  float w1 = 0.0f;
  float w2 = 0.0f;
  if (PointInTriangleBarycentric(point, corners[0], corners[1], corners[2], w0,
                                 w1, w2)) {
    const ImVec2 uv0(0.0f, 1.0f);
    const ImVec2 uv1(1.0f, 1.0f);
    const ImVec2 uv2(1.0f, 0.0f);
    outUv = ImVec2(uv0.x * w0 + uv1.x * w1 + uv2.x * w2,
                   uv0.y * w0 + uv1.y * w1 + uv2.y * w2);
    return true;
  }

  if (PointInTriangleBarycentric(point, corners[0], corners[2], corners[3], w0,
                                 w1, w2)) {
    const ImVec2 uv0(0.0f, 1.0f);
    const ImVec2 uv2(1.0f, 0.0f);
    const ImVec2 uv3(0.0f, 0.0f);
    outUv = ImVec2(uv0.x * w0 + uv2.x * w1 + uv3.x * w2,
                   uv0.y * w0 + uv2.y * w1 + uv3.y * w2);
    return true;
  }

  return false;
}

std::array<ImVec2, 4>
BuildPseudo3DPanelCorners(const ImVec2 &panelMin, const ImVec2 &panelMax,
                          const UIElementComponent &ui, float distanceScale,
                          float perspectiveDistanceFactor,
                          const std::array<ImVec2, 4> *baseCorners) {
  const ImVec2 pivotNorm(std::clamp(ui.pseudo3DPivot.x, 0.0f, 1.0f),
                         std::clamp(ui.pseudo3DPivot.y, 0.0f, 1.0f));

  std::array<ImVec2, 4> corners;
  float halfW = 0.0f;
  float halfH = 0.0f;

  if (baseCorners != nullptr) {
    // Transform-driven: corners come from a world-space quad already projected
    // to screen. Apply distance scaling around the pivot point (TL + (BR-TL) *
    // pivotNorm), then layer skew/curvature/per-corner offsets relative to the
    // bounding box of the projected quad.
    const ImVec2 c0 = (*baseCorners)[0];
    const ImVec2 c2 = (*baseCorners)[2];
    const ImVec2 pivot(c0.x + (c2.x - c0.x) * pivotNorm.x,
                       c0.y + (c2.y - c0.y) * pivotNorm.y);
    const float scale = std::max(0.01f, distanceScale);
    for (int i = 0; i < 4; ++i) {
      const ImVec2 src = (*baseCorners)[i];
      corners[i] = ImVec2(pivot.x + (src.x - pivot.x) * scale,
                          pivot.y + (src.y - pivot.y) * scale);
    }
    float minX = corners[0].x, maxX = corners[0].x;
    float minY = corners[0].y, maxY = corners[0].y;
    for (int i = 1; i < 4; ++i) {
      minX = std::min(minX, corners[i].x);
      maxX = std::max(maxX, corners[i].x);
      minY = std::min(minY, corners[i].y);
      maxY = std::max(maxY, corners[i].y);
    }
    halfW = std::max(1.0f, (maxX - minX) * 0.5f);
    halfH = std::max(1.0f, (maxY - minY) * 0.5f);
  } else {
    const ImVec2 baseSize(std::max(1.0f, panelMax.x - panelMin.x),
                          std::max(1.0f, panelMax.y - panelMin.y));
    const ImVec2 pivot(panelMin.x + baseSize.x * pivotNorm.x,
                       panelMin.y + baseSize.y * pivotNorm.y);
    const ImVec2 scaledSize(baseSize.x * std::max(0.01f, distanceScale),
                            baseSize.y * std::max(0.01f, distanceScale));
    const ImVec2 scaledMin(pivot.x - scaledSize.x * pivotNorm.x,
                           pivot.y - scaledSize.y * pivotNorm.y);
    const ImVec2 scaledMax(scaledMin.x + scaledSize.x,
                           scaledMin.y + scaledSize.y);

    corners = {ImVec2(scaledMin.x, scaledMin.y),
               ImVec2(scaledMax.x, scaledMin.y),
               ImVec2(scaledMax.x, scaledMax.y),
               ImVec2(scaledMin.x, scaledMax.y)};
    halfW = scaledSize.x * 0.5f;
    halfH = scaledSize.y * 0.5f;
  }

  const float perspective =
      ui.pseudo3DPerspectiveIntensity * perspectiveDistanceFactor;
  const float skew = ui.pseudo3DSkewAmount;
  const float curvature = ui.pseudo3DCurvatureAmount;
  const float offsetScale = std::max(0.01f, distanceScale);

  corners[0].x += perspective * halfW;
  corners[1].x -= perspective * halfW;
  corners[2].x += perspective * halfW;
  corners[3].x -= perspective * halfW;

  corners[0].x += skew * halfH;
  corners[1].x += skew * halfH;
  corners[2].x -= skew * halfH;
  corners[3].x -= skew * halfH;

  corners[0].y -= curvature * halfH;
  corners[1].y -= curvature * halfH;
  corners[2].y += curvature * halfH;
  corners[3].y += curvature * halfH;

  corners[0].x += ui.pseudo3DTopLeftOffset.x * offsetScale;
  corners[0].y += ui.pseudo3DTopLeftOffset.y * offsetScale;
  corners[1].x += ui.pseudo3DTopRightOffset.x * offsetScale;
  corners[1].y += ui.pseudo3DTopRightOffset.y * offsetScale;
  corners[2].x += ui.pseudo3DBottomRightOffset.x * offsetScale;
  corners[2].y += ui.pseudo3DBottomRightOffset.y * offsetScale;
  corners[3].x += ui.pseudo3DBottomLeftOffset.x * offsetScale;
  corners[3].y += ui.pseudo3DBottomLeftOffset.y * offsetScale;
  return corners;
}

bool ProjectTransformDrivenCanvasCorners(const SceneObject &canvas,
                                         const glm::mat4 &view,
                                         const glm::mat4 &proj,
                                         const ImVec2 &overlayPos,
                                         const ImVec2 &overlaySize,
                                         std::array<ImVec2, 4> &outCorners) {
  glm::mat4 model(1.0f);
  model = glm::translate(model, canvas.position);
  model = glm::rotate(model, glm::radians(canvas.rotation.x),
                      glm::vec3(1.0f, 0.0f, 0.0f));
  model = glm::rotate(model, glm::radians(canvas.rotation.y),
                      glm::vec3(0.0f, 1.0f, 0.0f));
  model = glm::rotate(model, glm::radians(canvas.rotation.z),
                      glm::vec3(0.0f, 0.0f, 1.0f));
  model = glm::scale(model, canvas.scale);

  // Order matches BuildPseudo3DPanelCorners: 0=TL, 1=TR, 2=BR, 3=BL.
  // ImGui screen space has y-down, so "top" of the canvas corresponds to
  // local +Y (which projects to a smaller screen y).
  const std::array<glm::vec3, 4> local = {
      glm::vec3(-0.5f, +0.5f, 0.0f), glm::vec3(+0.5f, +0.5f, 0.0f),
      glm::vec3(+0.5f, -0.5f, 0.0f), glm::vec3(-0.5f, -0.5f, 0.0f)};

  for (int i = 0; i < 4; ++i) {
    const glm::vec3 world = glm::vec3(model * glm::vec4(local[i], 1.0f));
    if (!ProjectWorldToOverlayPoint(world, view, proj, overlayPos, overlaySize,
                                    outCorners[i])) {
      return false;
    }
  }
  return true;
}

void ResolvePseudo3DDistanceState(const UIElementComponent &ui, float distance,
                                  float &outScale, float &outPerspectiveFactor,
                                  bool &outAllowInteraction) {
  outScale = 1.0f;
  outPerspectiveFactor = 1.0f;
  outAllowInteraction = ui.pseudo3DAllowInteraction;

  if (ui.pseudo3DDistanceScalingEnabled) {
    const float minDist = std::max(0.01f, ui.pseudo3DMinDistance);
    const float maxDist = std::max(minDist + 0.01f, ui.pseudo3DMaxDistance);
    const float t =
        std::clamp((distance - minDist) / (maxDist - minDist), 0.0f, 1.0f);
    outScale = 1.0f - t * 0.65f;
    if (ui.pseudo3DAdjustPerspectiveWithDistance) {
      outPerspectiveFactor = 1.0f - t;
    }
  }

  if (ui.pseudo3DInteractionDistance > 0.0f &&
      distance > ui.pseudo3DInteractionDistance) {
    outAllowInteraction = false;
  }
}

} // namespace ViewportRenderHelpers
