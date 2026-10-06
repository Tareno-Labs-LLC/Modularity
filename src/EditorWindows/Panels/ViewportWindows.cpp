#include "../../EditorLocalization.h"
#include "Engine.h"
#include "ViewportRenderHelpers.h"
#include "GizmoToolbar.h"
#include "MapMaker.h"
#include "MeshEditOperations.h"
#include "ModelLoader.h"
#ifdef __ANDROID__
#include "../../AndroidRuntime/AndroidRuntime.h"
#endif
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <numeric>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <shlobj.h>
#endif

namespace Loc = Modularity::Loc;

using namespace ViewportRenderHelpers;
namespace {
template <typename EnumType, size_t N>
ObjectType MapEnumToObjectType(EnumType value, const std::array<ObjectType, N> &mapping) {
  const size_t index = static_cast<size_t>(value);
  if (index >= mapping.size()) {return ObjectType::Empty;}
  return mapping[index];
}

static constexpr std::array<ObjectType, static_cast<size_t>(RenderType::Sprite) + 1>
    kRenderTypeMainObjectMap = {{
        ObjectType::Empty,   // RenderType::None
        ObjectType::Cube,    // RenderType::Cube
        ObjectType::Sphere,  // RenderType::Sphere
        ObjectType::Capsule, // RenderType::Capsule
        ObjectType::OBJMesh, // RenderType::OBJMesh
        ObjectType::Model,   // RenderType::Model
        ObjectType::Mirror,  // RenderType::Mirror
        ObjectType::Plane,   // RenderType::Plane
        ObjectType::Torus,   // RenderType::Torus
        ObjectType::Sprite   // RenderType::Sprite
    }};

static constexpr std::array<ObjectType, static_cast<size_t>(UIElementType::Sprite2D) + 1>
    kUiTypeMainObjectMap = {{
        ObjectType::Empty,    // UIElementType::None
        ObjectType::Canvas,   // UIElementType::Canvas
        ObjectType::UIImage,  // UIElementType::Image
        ObjectType::UISlider, // UIElementType::Slider
        ObjectType::UIButton, // UIElementType::Button
        ObjectType::UIText,   // UIElementType::Text
        ObjectType::Sprite2D  // UIElementType::Sprite2D
    }};

static constexpr std::array<ObjectType, static_cast<size_t>(LightType::Area) + 1>
    kLightTypeMainObjectMap = {{
        ObjectType::DirectionalLight, // LightType::Directional
        ObjectType::PointLight,       // LightType::Point
        ObjectType::SpotLight,        // LightType::Spot
        ObjectType::AreaLight         // LightType::Area
    }};

struct ViewportAxisArrow {
  glm::vec3 direction;
  ImU32 color;
  const char *label;
};

struct ViewportAxisArrowGeometry {
  ImVec2 origin;
  ImVec2 tip;
  ImVec2 shaftStartLeft;
  ImVec2 shaftStartRight;
  ImVec2 shaftEndLeft;
  ImVec2 shaftEndRight;
  ImVec2 coneBaseCenter;
  ImVec2 coneBaseLeft;
  ImVec2 coneBaseRight;
  ImVec2 labelPosition;
  ImVec2 circleCenter;
  float geometryOpacity;
  float circleRadius;
  float labelOpacity;
  float leftBrightness;
  float rightBrightness;
};

struct ViewportAxisArrowRenderItem {
  const ViewportAxisArrow *arrow = nullptr;
  ViewportAxisArrowGeometry geometry;
  float cameraDepth = 0.0f;
};

ImU32 ShadeViewportAxisColor(ImU32 color, float brightness, float opacity) {
  ImVec4 shaded = ImGui::ColorConvertU32ToFloat4(color);
  shaded.x = glm::clamp(shaded.x * brightness, 0.0f, 1.0f);
  shaded.y = glm::clamp(shaded.y * brightness, 0.0f, 1.0f);
  shaded.z = glm::clamp(shaded.z * brightness, 0.0f, 1.0f);
  shaded.w *= opacity;
  return ImGui::ColorConvertFloat4ToU32(shaded);
}

ViewportAxisArrowGeometry
BuildViewportAxisArrowGeometry(const ImVec2 &center, float radius,
                               const glm::vec3 &cameraDirection) {
  constexpr float kMinimumDepthScale = 0.25f;
  constexpr float kDepthScaleBase = 0.35f;
  constexpr float kDepthScaleRange = 0.65f;
  constexpr float kArrowLengthScale = 1.0f;
  constexpr float kArrowHeadLength = 14.0f;
  constexpr float kShaftHalfWidth = 2.1f;
  constexpr float kArrowHeadHalfWidth = 5.5f;
  constexpr float kCenterGap = 7.0f;
  constexpr float kLabelDistance = 1.08f;
  constexpr float kCircleTransitionStart = 0.08f;
  constexpr float kCircleTransitionEnd = 0.28f;
  constexpr float kFaceOnCircleRadius = 6.0f;

  const glm::vec2 projectedDirection(cameraDirection.x, -cameraDirection.y);
  const float projectedLength =
      glm::clamp(glm::length(projectedDirection), 0.0f, 1.0f);
  const glm::vec2 screenDirection = projectedLength > 1e-4f
                                        ? projectedDirection / projectedLength
                                        : glm::vec2(1.0f, 0.0f);
  const float normalizedDepth = (cameraDirection.z + 1.0f) * 0.5f;
  const float depthScale =
      glm::clamp(kDepthScaleBase + kDepthScaleRange * normalizedDepth,
                 kMinimumDepthScale, 1.0f);
  const float length =
      radius * kArrowLengthScale * depthScale * projectedLength;
  const float geometryOpacity = glm::smoothstep(
      kCircleTransitionStart, kCircleTransitionEnd, projectedLength);
  const bool facesCamera = cameraDirection.z > 0.0f;
  const float circleRadius =
      facesCamera ? kFaceOnCircleRadius * (1.0f - geometryOpacity) : 0.0f;
  const float labelOpacity = facesCamera ? 1.0f : geometryOpacity;

  const glm::vec2 screenPerpendicular(-screenDirection.y, screenDirection.x);
  const float coneLength = std::min(kArrowHeadLength, length * 0.48f);
  const float coneBaseDistance = std::max(0.0f, length - coneLength);
  const float originDistance =
      std::min(coneBaseDistance, kCenterGap * geometryOpacity);
  const float shaftHalfWidth = kShaftHalfWidth * geometryOpacity;
  const float arrowHeadHalfWidth = kArrowHeadHalfWidth * geometryOpacity;

  const glm::vec2 lightDirection = glm::normalize(glm::vec2(-0.65f, -0.75f));
  const float sideLight =
      glm::clamp(glm::dot(screenPerpendicular, lightDirection), -1.0f, 1.0f);
  const float depthBrightness = glm::mix(0.62f, 1.08f, normalizedDepth);
  const float leftBrightness = depthBrightness * (0.88f + 0.20f * sideLight);
  const float rightBrightness = depthBrightness * (0.88f - 0.20f * sideLight);

  const ImVec2 tip(center.x + screenDirection.x * length,
                   center.y + screenDirection.y * length);
  const ImVec2 origin(center.x + screenDirection.x * originDistance,
                      center.y + screenDirection.y * originDistance);
  const ImVec2 coneBaseCenter(center.x + screenDirection.x * coneBaseDistance,
                              center.y + screenDirection.y * coneBaseDistance);
  const ImVec2 shaftOffset(screenPerpendicular.x * shaftHalfWidth,
                           screenPerpendicular.y * shaftHalfWidth);
  const ImVec2 arrowHeadOffset(screenPerpendicular.x * arrowHeadHalfWidth,
                               screenPerpendicular.y * arrowHeadHalfWidth);

  return {
      origin,
      tip,
      ImVec2(origin.x + shaftOffset.x, origin.y + shaftOffset.y),
      ImVec2(origin.x - shaftOffset.x, origin.y - shaftOffset.y),
      ImVec2(coneBaseCenter.x + shaftOffset.x,
             coneBaseCenter.y + shaftOffset.y),
      ImVec2(coneBaseCenter.x - shaftOffset.x,
             coneBaseCenter.y - shaftOffset.y),
      coneBaseCenter,
      ImVec2(coneBaseCenter.x + arrowHeadOffset.x,
             coneBaseCenter.y + arrowHeadOffset.y),
      ImVec2(coneBaseCenter.x - arrowHeadOffset.x,
             coneBaseCenter.y - arrowHeadOffset.y),
      ImVec2(center.x + screenDirection.x * (length * kLabelDistance),
             center.y + screenDirection.y * (length * kLabelDistance)),
      center,
      geometryOpacity,
      circleRadius,
      labelOpacity,
      leftBrightness,
      rightBrightness,
  };
}

void DrawViewportAxisArrow(ImDrawList *drawList,
                           const ViewportAxisArrowGeometry &geometry,
                           const ViewportAxisArrow &arrow) {
  const ImU32 outlineColor =
      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
  const ImU32 leftColor = ShadeViewportAxisColor(
      arrow.color, geometry.leftBrightness, geometry.geometryOpacity);
  const ImU32 rightColor = ShadeViewportAxisColor(
      arrow.color, geometry.rightBrightness, geometry.geometryOpacity);
  const ImU32 labelColor = ImGui::GetColorU32(
      ImVec4(1.0f, 1.0f, 1.0f, 0.95f * geometry.labelOpacity));

  constexpr ImVec2 kLabelOffset(-4.0f, -14.0f);

  if (geometry.circleRadius > 0.01f) {
    const ImU32 circleShadow = ShadeViewportAxisColor(arrow.color, 0.62f, 1.0f);
    const ImU32 circleLight = ShadeViewportAxisColor(arrow.color, 1.12f, 1.0f);
    drawList->AddCircleFilled(geometry.circleCenter, geometry.circleRadius,
                              circleShadow, 16);
    drawList->AddCircleFilled(
        ImVec2(geometry.circleCenter.x - geometry.circleRadius * 0.18f,
               geometry.circleCenter.y - geometry.circleRadius * 0.18f),
        geometry.circleRadius * 0.78f, circleLight, 16);
    drawList->AddCircle(geometry.circleCenter, geometry.circleRadius,
                        outlineColor, 16);
  }

  if (geometry.geometryOpacity > 0.01f) {
    drawList->AddQuadFilled(geometry.origin, geometry.shaftStartLeft,
                            geometry.shaftEndLeft, geometry.coneBaseCenter,
                            leftColor);
    drawList->AddQuadFilled(geometry.origin, geometry.coneBaseCenter,
                            geometry.shaftEndRight, geometry.shaftStartRight,
                            rightColor);
    drawList->AddQuad(geometry.shaftStartLeft, geometry.shaftEndLeft,
                      geometry.shaftEndRight, geometry.shaftStartRight,
                      outlineColor);

    drawList->AddTriangleFilled(geometry.coneBaseLeft, geometry.tip,
                                geometry.coneBaseCenter, leftColor);
    drawList->AddTriangleFilled(geometry.coneBaseCenter, geometry.tip,
                                geometry.coneBaseRight, rightColor);
    drawList->AddTriangle(geometry.coneBaseLeft, geometry.tip,
                          geometry.coneBaseRight, outlineColor);
    drawList->AddLine(geometry.coneBaseLeft, geometry.coneBaseRight,
                      outlineColor);
  }
  if (geometry.labelOpacity > 0.01f && arrow.label[0] != '\0') {
    drawList->AddText(ImVec2(geometry.labelPosition.x + kLabelOffset.x,
                             geometry.labelPosition.y + kLabelOffset.y),
                      labelColor, arrow.label);
  }
}

bool IsPointWithinRadius(const ImVec2 &point, const ImVec2 &center,
                         float radius) {
  const float dx = point.x - center.x;
  const float dy = point.y - center.y;
  return std::sqrt(dx * dx + dy * dy) <= radius;
}

} // namespace

// Scene view object picking: the ray under a screen point against every enabled object's
// pickable shape, nearest hit wins. Click selection and the asset-drop raycast both use it, so
// what a drop lands on is always what a click there would select. `filter` skips objects the
// caller cannot use (a material drop passes objects without a renderer through to what is
// behind them). Returns -1 on a miss.
int Engine::pickSceneObjectAtScreen(const glm::mat4 &view, const glm::mat4 &proj,
                                    const ImVec2 &imageMin, const ImVec2 &imageMax,
                                    const ImVec2 &screenPos,
                                    const std::function<bool(const SceneObject &)> &filter) {
  glm::mat4 invViewProj = glm::inverse(proj * view);
  const ImVec2 mousePos = screenPos;

  auto makeRay = [&](const ImVec2 &pos) {
    float x = (pos.x - imageMin.x) / (imageMax.x - imageMin.x);
    float y = (pos.y - imageMin.y) / (imageMax.y - imageMin.y);
    x = x * 2.0f - 1.0f;
    y = 1.0f - y * 2.0f;

    glm::vec4 nearPt = invViewProj * glm::vec4(x, y, -1.0f, 1.0f);
    glm::vec4 farPt = invViewProj * glm::vec4(x, y, 1.0f, 1.0f);
    nearPt /= nearPt.w;
    farPt /= farPt.w;

    glm::vec3 origin = glm::vec3(nearPt);
    glm::vec3 dir = glm::normalize(glm::vec3(farPt - nearPt));
    return std::make_pair(origin, dir);
  };

  auto rayAabb = [](const glm::vec3 &orig, const glm::vec3 &dir,
                    const glm::vec3 &bmin, const glm::vec3 &bmax,
                    float &tHit) {
    float tmin = -FLT_MAX;
    float tmax = FLT_MAX;
    for (int i = 0; i < 3; ++i) {
      if (std::abs(dir[i]) < 1e-6f) {
        if (orig[i] < bmin[i] || orig[i] > bmax[i])
          return false;
        continue;
      }
      float invD = 1.0f / dir[i];
      float t1 = (bmin[i] - orig[i]) * invD;
      float t2 = (bmax[i] - orig[i]) * invD;
      if (t1 > t2)
        std::swap(t1, t2);
      tmin = std::max(tmin, t1);
      tmax = std::min(tmax, t2);
      if (tmin > tmax)
        return false;
    }
    tHit = (tmin >= 0.0f) ? tmin : tmax;
    return tmax >= 0.0f;
  };

  auto raySphere = [](const glm::vec3 &orig, const glm::vec3 &dir,
                      float radius, float &tHit) {
    float b = glm::dot(dir, orig);
    float c = glm::dot(orig, orig) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f)
      return false;
    float sqrtDisc = sqrtf(disc);
    float t0 = -b - sqrtDisc;
    float t1 = -b + sqrtDisc;
    float t = (t0 >= 0.0f) ? t0 : t1;
    if (t < 0.0f)
      return false;
    tHit = t;
    return true;
  };

  auto rayTriangle = [](const glm::vec3 &orig, const glm::vec3 &dir,
                        const glm::vec3 &v0, const glm::vec3 &v1,
                        const glm::vec3 &v2, float &tHit) {
    const float EPSILON = 1e-6f;
    glm::vec3 e1 = v1 - v0;
    glm::vec3 e2 = v2 - v0;
    glm::vec3 pvec = glm::cross(dir, e2);
    float det = glm::dot(e1, pvec);
    if (fabs(det) < EPSILON)
      return false;
    float invDet = 1.0f / det;
    glm::vec3 tvec = orig - v0;
    float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f)
      return false;
    glm::vec3 qvec = glm::cross(tvec, e1);
    float v = glm::dot(dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f)
      return false;
    float t = glm::dot(e2, qvec) * invDet;
    if (t < 0.0f)
      return false;
    tHit = t;
    return true;
  };

  auto ray = makeRay(mousePos);
  float closest = FLT_MAX;
  int hitId = -1;
  glm::mat4 invView = glm::inverse(view);
  glm::vec3 cameraPos = glm::vec3(invView[3]);
  glm::vec3 cameraUp = glm::normalize(glm::vec3(invView[1]));

  for (const auto &obj : sceneObjects) {
    if (!IsObjectEnabledInHierarchy(obj))
      continue;
    if (filter && !filter(obj))
      continue;
    if (obj.hasLight && !obj.light.enabled &&
        (obj.type == ObjectType::DirectionalLight ||
         obj.type == ObjectType::PointLight ||
         obj.type == ObjectType::SpotLight ||
         obj.type == ObjectType::AreaLight))
      continue;

    glm::vec3 aabbMin(-0.5f);
    glm::vec3 aabbMax(0.5f);

    glm::mat4 model(1.0f);
    model = glm::translate(model, obj.position);
    if (obj.type == ObjectType::Sprite25D ||
        (obj.renderType == RenderType::Sprite && obj.faceCamera)) {
      glm::vec3 forward = cameraPos - obj.position;
      if (glm::dot(forward, forward) < 1e-6f)
        forward = glm::vec3(0.0f, 0.0f, 1.0f);
      else
        forward = glm::normalize(forward);
      glm::vec3 right = glm::cross(cameraUp, forward);
      if (glm::dot(right, right) < 1e-6f) {
        right = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), forward);
      }
      right = glm::normalize(right);
      glm::vec3 up = glm::normalize(glm::cross(forward, right));
      glm::vec3 scale = glm::max(glm::abs(obj.scale), glm::vec3(0.0001f));
      model[0] = glm::vec4(
          right * scale.x * (obj.scale.x < 0.0f ? -1.0f : 1.0f), 0.0f);
      model[1] = glm::vec4(
          up * scale.y * (obj.scale.y < 0.0f ? -1.0f : 1.0f), 0.0f);
      model[2] = glm::vec4(
          forward * scale.z * (obj.scale.z < 0.0f ? -1.0f : 1.0f), 0.0f);
    } else {
      model = glm::rotate(model, glm::radians(obj.rotation.x),
                          glm::vec3(1, 0, 0));
      model = glm::rotate(model, glm::radians(obj.rotation.y),
                          glm::vec3(0, 1, 0));
      model = glm::rotate(model, glm::radians(obj.rotation.z),
                          glm::vec3(0, 0, 1));
      model = glm::scale(model, obj.scale);
    }

    glm::mat4 invModel = glm::inverse(model);
    glm::vec3 localOrigin =
        glm::vec3(invModel * glm::vec4(ray.first, 1.0f));
    glm::vec3 localDir =
        glm::normalize(glm::vec3(invModel * glm::vec4(ray.second, 0.0f)));

    float hitT = 0.0f;
    bool hit = false;
    switch (obj.type) {
    case ObjectType::Cube:
      hit = rayAabb(localOrigin, localDir, glm::vec3(-0.5f),
                    glm::vec3(0.5f), hitT);
      break;
    case ObjectType::Sphere:
      hit = raySphere(localOrigin, localDir, 0.5f, hitT);
      break;
    case ObjectType::Capsule:
      hit = rayAabb(localOrigin, localDir, glm::vec3(-0.35f, -0.9f, -0.35f),
                    glm::vec3(0.35f, 0.9f, 0.35f), hitT);
      break;
    case ObjectType::Plane:
      hit = rayAabb(localOrigin, localDir, glm::vec3(-0.5f, -0.5f, -0.02f),
                    glm::vec3(0.5f, 0.5f, 0.02f), hitT);
      break;
    case ObjectType::Mirror:
      hit = rayAabb(localOrigin, localDir, glm::vec3(-0.5f, -0.5f, -0.02f),
                    glm::vec3(0.5f, 0.5f, 0.02f), hitT);
      break;
    case ObjectType::Sprite:
    case ObjectType::Sprite25D:
    case ObjectType::ParticleSystem2D:
      hit = rayAabb(localOrigin, localDir, glm::vec3(-0.5f, -0.5f, -0.02f),
                    glm::vec3(0.5f, 0.5f, 0.02f), hitT);
      break;
    case ObjectType::Torus:
      hit = raySphere(localOrigin, localDir, 0.5f, hitT);
      break;
    case ObjectType::Sprite2D:
    case ObjectType::Canvas:
    case ObjectType::UIImage:
    case ObjectType::UISlider:
    case ObjectType::UIButton:
    case ObjectType::UIText:
      hit = false;
      break;
    case ObjectType::OBJMesh: {
      const auto *info = g_objLoader.getMeshInfo(obj.meshId);
      if (info && info->boundsMin.x < info->boundsMax.x) {
        aabbMin = info->boundsMin;
        aabbMax = info->boundsMax;
      }
      bool aabbHit = rayAabb(localOrigin, localDir, aabbMin, aabbMax, hitT);
      if (aabbHit && info && !info->triangleVertices.empty()) {
        float triBest = FLT_MAX;
        for (size_t i = 0; i + 2 < info->triangleVertices.size(); i += 3) {
          float triT = 0.0f;
          if (rayTriangle(localOrigin, localDir, info->triangleVertices[i],
                          info->triangleVertices[i + 1],
                          info->triangleVertices[i + 2], triT)) {
            if (triT < triBest && triT >= 0.0f)
              triBest = triT;
          }
        }
        if (triBest < FLT_MAX) {
          hit = true;
          hitT = triBest;
        } else {
          hit = false;
        }
      } else {
        hit = aabbHit;
      }
      break;
    }
    case ObjectType::Model: {
      const auto *info = getModelLoader().getMeshInfo(obj.meshId);
      if (info && info->boundsMin.x < info->boundsMax.x) {
        aabbMin = info->boundsMin;
        aabbMax = info->boundsMax;
      }
      bool aabbHit = rayAabb(localOrigin, localDir, aabbMin, aabbMax, hitT);
      if (aabbHit && info && !info->triangleVertices.empty()) {
        float triBest = FLT_MAX;
        for (size_t i = 0; i + 2 < info->triangleVertices.size(); i += 3) {
          float triT = 0.0f;
          if (rayTriangle(localOrigin, localDir, info->triangleVertices[i],
                          info->triangleVertices[i + 1],
                          info->triangleVertices[i + 2], triT)) {
            if (triT < triBest && triT >= 0.0f)
              triBest = triT;
          }
        }
        if (triBest < FLT_MAX) {
          hit = true;
          hitT = triBest;
        } else {
          hit = false;
        }
      } else {
        hit = aabbHit;
      }
      break;
    }
    case ObjectType::Camera:
      hit = raySphere(localOrigin, localDir, 0.3f, hitT);
      break;
    case ObjectType::DirectionalLight:
    case ObjectType::PointLight:
    case ObjectType::SpotLight:
    case ObjectType::AreaLight:
      hit = raySphere(localOrigin, localDir, 0.3f, hitT);
      break;
    case ObjectType::PostFXNode:
      hit = false;
      break;
    case ObjectType::Empty:
      hit = false;
      break;
    default:
      break;
    }

    if (hit && hitT >= 0.0f) {
      const glm::vec3 localHit = localOrigin + localDir * hitT;
      const glm::vec3 worldHit =
          glm::vec3(model * glm::vec4(localHit, 1.0f));
      const float worldT = glm::dot(worldHit - ray.first, ray.second);
      if (worldT < closest && worldT >= 0.0f) {
        closest = worldT;
        hitId = obj.id;
      }
    }
  }
  return hitId;
}

#pragma region Scene Viewport
// Final scene output for the editor viewport.
void Engine::renderViewport() {
  ImGuiWindowFlags viewportFlags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;
  if (viewportFullscreen) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    viewportFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const bool windowVisible = ImGui::Begin(Loc::Window("WINDOW_VIEWPORT", "Viewport"), &showViewport, viewportFlags);
  ImGui::PopStyleVar();
  if (!windowVisible) {ImGui::End(); return;}

  {
    static bool dockTabWasHidden = false;
    const bool dockTabHidden = IsCurrentDockTabHidden();
    const bool skipHiddenTab = dockTabHidden && dockTabWasHidden;
    dockTabWasHidden = dockTabHidden;
    if (skipHiddenTab) {
      Modu2DStats::CountWindowSkipped();
      Modu2DStats::CountSkippedRedraw();
      ImGui::End();
      return;
    }
  }

  bool viewportCreateMenuRequested = false;
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      !ImGui::GetIO().WantTextInput && !cursorLocked && !playerMode) {
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
      focusViewportOnSelection();
    }
    if (ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_A, false)) {
      viewportCreateMenuRequested = true;
    }
  }
  if (viewportCreateMenuRequested) {
    ImGui::OpenPopup("ViewportCreateMenu");
  }
  if (ImGui::BeginPopup("ViewportCreateMenu")) {
    renderSceneObjectCreateMenu();
    ImGui::EndPopup();
  }

  ImVec2 panelMin = ImGui::GetCursorScreenPos();
  ImVec2 panelSize = ImGui::GetContentRegionAvail();
  panelSize.x = std::max(1.0f, panelSize.x);
  panelSize.y = std::max(1.0f, panelSize.y);

  viewportWidth = std::clamp(static_cast<int>(std::floor(panelSize.x)), 1, 8192);
  viewportHeight = std::clamp(static_cast<int>(std::floor(panelSize.y)), 1, 8192);
  if (rendererInitialized) {renderer.resize(viewportWidth, viewportHeight);}

  const EmbeddedViewportLayout sceneLayout = BuildEmbeddedViewportLayout(panelMin, panelSize, viewportWidth, viewportHeight, sceneViewportDisplayMode);
  ImVec2 imageSize = sceneLayout.displaySize;

  bool mouseOverViewportImage = false;
  bool blockSelection = false;
  ImVec2 viewportImageMin(0.0f, 0.0f);
  ImVec2 viewportImageMax(0.0f, 0.0f);
  bool hasViewportImageRect = false;
  const bool project2DPipeline = isProject2DPipeline();
  const bool project25DPipeline = isProject25DPipeline();
  const bool worldUiEditing = is2DWorldEditingEnabled();
  const bool pure2DSceneView = project2DPipeline || worldUiEditing;
  const bool hasVulkanSceneTexture =
      usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
  static bool meshEditActionsPopupRequested = false;
  static ImVec2 meshEditActionsPopupPos(0.0f, 0.0f);

  auto collectSelectionRoots = [&](const std::vector<int> &ids) {
    std::vector<int> roots;
    if (ids.empty()) {return roots;}

    std::unordered_set<int> selectedSet(ids.begin(), ids.end());
    auto getParentId = [&](int id) -> int {
      auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(), [id](const SceneObject &obj) { return obj.id == id; });
      if (it == sceneObjects.end()) {return -1;}
      return it->parentId;
    };

    auto hasSelectedAncestor = [&](int id) -> bool {
      int parentId = getParentId(id);
      while (parentId != -1) {
        if (selectedSet.count(parentId) > 0) {return true;}
        parentId = getParentId(parentId);
      }
      return false;
    };

    roots.reserve(ids.size());
    for (int id : ids) {if (!hasSelectedAncestor(id)) {roots.push_back(id);}}
    return roots;
  };

  auto allocateRig25DNodeId = [&]() {
    int nextNodeId = 0;
    for (const SceneObject &obj : sceneObjects) {
      if (!obj.hasRig25DNode || obj.rig25DNode.nodeId < 0) continue;
      nextNodeId = std::max(nextNodeId, obj.rig25DNode.nodeId + 1);
    }
    return nextNodeId;
  };

  auto setRig25DMetadata = [&](SceneObject &obj, bool isRoot) {
    obj.type = ObjectType::Empty;
    obj.hasRenderer = false;
    obj.renderType = RenderType::None;
    obj.faceCamera = false;
    obj.hasRig25DRoot = isRoot;
    obj.rig25DRoot.enabled = isRoot;
    obj.hasRig25DNode = !isRoot;
    obj.rig25DNode.enabled = !isRoot;
    obj.rig25DNode.nodeId = isRoot ? -1 : allocateRig25DNodeId();
    obj.rig25DNode.nodeName = isRoot ? std::string() : obj.name;
    obj.localPosition = obj.position;
    obj.localRotation = NormalizeEulerDegrees(obj.rotation);
    obj.localScale = obj.scale;
    obj.localInitialized = true;
    EnsureInspectorComponentMetadata(obj);
  };

  auto zeroLocalTransform = [&](SceneObject &obj) {
    obj.localPosition = glm::vec3(0.0f);
    obj.localRotation = glm::vec3(0.0f);
    obj.localScale = glm::vec3(1.0f);
    obj.localInitialized = true;
  };

  auto mirrorLocalX = [&](SceneObject &obj) {
    obj.localPosition.x = -obj.localPosition.x;
    obj.localRotation.y = -obj.localRotation.y;
    obj.localRotation.z = -obj.localRotation.z;
    obj.localScale.x = -obj.localScale.x;
    obj.localRotation = NormalizeEulerDegrees(obj.localRotation);
    obj.localInitialized = true;
  };

  int activeGameResolutionWidth = 0;
  int activeGameResolutionHeight = 0;
  switch (gameViewportResolutionIndex) {
  case 0:
    if (gameViewportLastRenderWidth > 0 && gameViewportLastRenderHeight > 0) {
      activeGameResolutionWidth = gameViewportLastRenderWidth;
      activeGameResolutionHeight = gameViewportLastRenderHeight;
    } else {
      activeGameResolutionWidth = std::max(1, viewportWidth);
      activeGameResolutionHeight = std::max(1, viewportHeight);
    }
    break;
  case 1:
    activeGameResolutionWidth = 1920;
    activeGameResolutionHeight = 1080;
    break;
  case 2:
    activeGameResolutionWidth = 1280;
    activeGameResolutionHeight = 720;
    break;
  case 3:
    activeGameResolutionWidth = 2560;
    activeGameResolutionHeight = 1440;
    break;
  case 4:
    activeGameResolutionWidth = std::clamp(gameViewportCustomWidth, 64, 8192);
    activeGameResolutionHeight = std::clamp(gameViewportCustomHeight, 64, 8192);
    break;
  default:
    activeGameResolutionWidth = std::max(1, viewportWidth);
    activeGameResolutionHeight = std::max(1, viewportHeight);
    break;
  }
  int worldUiReferenceResolutionWidth = activeGameResolutionWidth;
  int worldUiReferenceResolutionHeight = activeGameResolutionHeight;
  if (worldUiEditing) {
    int bestArea = 0;
    for (const auto &obj : sceneObjects) {
      if (!IsObjectEnabledInHierarchy(obj)) continue;
      if (!(obj.hasUI && obj.ui.type == UIElementType::Canvas) || obj.ui.renderIn3D) continue;
      const int canvasWidth = std::clamp(
          static_cast<int>(std::round(std::max(1.0f, obj.ui.size.x))), 1, 8192);
      const int canvasHeight = std::clamp(
          static_cast<int>(std::round(std::max(1.0f, obj.ui.size.y))), 1, 8192);
      const int area = canvasWidth * canvasHeight;
      if (area > bestArea) {
        bestArea = area;
        worldUiReferenceResolutionWidth = canvasWidth;
        worldUiReferenceResolutionHeight = canvasHeight;
      }
    }
  }
  const float activeGameResolutionAspect =
      static_cast<float>(activeGameResolutionWidth) /
      std::max(1.0f, static_cast<float>(activeGameResolutionHeight));

  if (hasVulkanSceneTexture) {
    vulkanRenderer->setViewportSceneSize(
        static_cast<uint32_t>(std::max(1, viewportWidth)),
        static_cast<uint32_t>(std::max(1, viewportHeight)));
  }

  if (!rendererInitialized && !hasVulkanSceneTexture) {
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##SceneViewportPanelEmpty", panelSize, ImGuiButtonFlags_MouseButtonRight);
    ImVec2 imageMin = sceneLayout.displayMin;
    ImVec2 imageMax = sceneLayout.displayMax;
    ImVec2 drawSize(std::max(1.0f, imageMax.x - imageMin.x), std::max(1.0f, imageMax.y - imageMin.y));
    viewportImageMin = imageMin;
    viewportImageMax = imageMax;
    hasViewportImageRect = true;
    glm::vec2 hoveredPixel(0.0f);
    mouseOverViewportImage = TryMapScreenPointToRenderPixel(sceneLayout, ImGui::GetIO().MousePos, viewportWidth, viewportHeight, hoveredPixel);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(imageMin, imageMax, IM_COL32(14, 18, 30, 255), 8.0f);
    dl->AddRect(imageMin, imageMax, IM_COL32(78, 96, 128, 210), 8.0f, 0, 1.5f);

    const char *title = usingVulkan() ? "Vulkan Scene Viewport Unavailable" : "Scene Viewport Unavailable";
    const char *line1 = usingVulkan() ? "Vulkan scene render target is not ready." : "Renderer is not initialized for this session.";
    const char *line2 = usingVulkan() ? "Open a project scene or retry after renderer initialization." : "Open or create a project to initialize rendering.";

    ImVec2 titleSize = ImGui::CalcTextSize(title);
    ImVec2 line1Size = ImGui::CalcTextSize(line1);
    ImVec2 line2Size = ImGui::CalcTextSize(line2);
    float centerX = imageMin.x + drawSize.x * 0.5f;
    float baseY = imageMin.y + drawSize.y * 0.5f - 28.0f;
    dl->AddText(ImVec2(centerX - titleSize.x * 0.5f, baseY), IM_COL32(220, 228, 244, 255), title);
    dl->AddText(ImVec2(centerX - line1Size.x * 0.5f, baseY + 24.0f), IM_COL32(170, 184, 212, 255), line1);
    dl->AddText(ImVec2(centerX - line2Size.x * 0.5f, baseY + 44.0f), IM_COL32(170, 184, 212, 255), line2);
  }

  if (rendererInitialized || hasVulkanSceneTexture) {
    Modu2DStats::CountViewportRedraw();
    float editorFov = buildSettings.editorCameraFov;
#ifdef __ANDROID__
    {
      const float aspect =
          (float)viewportWidth / (float)std::max(1, viewportHeight);
      const float refAspect = 16.0f / 9.0f;
      if (aspect < refAspect && aspect > 0.01f) {
        const float halfV = glm::radians(editorFov) * 0.5f;
        const float newHalfV = std::atan(std::tan(halfV) * (refAspect / aspect));
        editorFov = glm::clamp(glm::degrees(newHalfV * 2.0f), editorFov, 120.0f);
      }
    }
#endif
    glm::mat4 proj = glm::perspective(
        glm::radians(editorFov),
        (float)viewportWidth / (float)viewportHeight,
        buildSettings.editorCameraNear, buildSettings.editorCameraFar);
    glm::mat4 view = camera.getViewMatrix();

    if (rendererInitialized) {
      unsigned int tex = 0;
      Modularity::Render25D::TMRenderer::RenderStats tmStats;
      std::string tmError;
      if (isProject25DPipeline()) {
        renderTMViewportPass(camera, viewportWidth, viewportHeight,
                             editorFov,
                             buildSettings.editorCameraNear,
                             buildSettings.editorCameraFar, &tex, &tmStats,
                             &tmError);
      } else if (!pure2DSceneView) {
        const bool showSelected3DColliderPreview = [&]() {
          if (worldUiEditing) return false;
          auto shouldPreview = [&](int id) {
            auto it = std::find_if(
                sceneObjects.begin(), sceneObjects.end(),
                [&](const SceneObject &obj) { return obj.id == id; });
            return it != sceneObjects.end() && IsObjectEnabledInHierarchy(*it) && it->hasCollider && it->collider.enabled;
          };
          for (int id : selectedObjectIds) {if (shouldPreview(id)) return true;}
          return selectedObjectIds.empty() && selectedObjectId >= 0 && shouldPreview(selectedObjectId);
        }();
        // While a material, script or audio clip is dragged over an object - here, in the
        // Hierarchy or on the Inspector - that object wears the selection outline instead of the
        // selection, so it is clear where the drop will land.
        static std::vector<int> dropHoverOutlineIds(1, -1);
        const int dropHoverId = activeAssetDropHover();
        dropHoverOutlineIds[0] = dropHoverId;
        renderer.beginRender(view, proj, camera.position);
        renderer.renderScene(
            camera, sceneObjects, selectedObjectId,
            editorFov, buildSettings.editorCameraNear,
            buildSettings.editorCameraFar, showSelected3DColliderPreview && dropHoverId < 0,
            dropHoverId >= 0 ? &dropHoverOutlineIds : &selectedObjectIds, sceneViewportRenderMode);
        tex = renderer.getViewportTexture();
      }
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton("##SceneViewportPanel", panelSize,
                             ImGuiButtonFlags_MouseButtonRight);
      ImDrawList *drawList = ImGui::GetWindowDrawList();
      drawList->AddRectFilled(sceneLayout.panelMin, sceneLayout.panelMax, IM_COL32(10, 12, 18, 255));
      if (tex != 0) {
        ApplyNearestTextureSampling(tex);
        drawList->PushClipRect(sceneLayout.panelMin, sceneLayout.panelMax, true);
        drawList->AddImage(
            (void *)(intptr_t)tex, sceneLayout.displayMin,
            sceneLayout.displayMax,
            ImVec2(sceneLayout.uvMin.x, 1.0f - sceneLayout.uvMin.y),
            ImVec2(sceneLayout.uvMax.x, 1.0f - sceneLayout.uvMax.y));
        drawList->PopClipRect();
      }
      if (isProject25DPipeline()) {
        const std::string label = buildTMOverlayLabel(tmStats, tmError);
        drawList->AddText(ImVec2(sceneLayout.displayMin.x + 10.0f, sceneLayout.displayMin.y + 10.0f), IM_COL32(236, 240, 246, 255), label.c_str());
      }

      ImVec2 imageMin = sceneLayout.displayMin;
      ImVec2 imageMax = sceneLayout.displayMax;
      viewportImageMin = imageMin;
      viewportImageMax = imageMax;
      hasViewportImageRect = true;

      glm::vec2 hoveredPixel(0.0f);
      mouseOverViewportImage = TryMapScreenPointToRenderPixel(
          sceneLayout, ImGui::GetIO().MousePos, viewportWidth, viewportHeight,
          hoveredPixel);

#ifdef __ANDROID__
      if (!isPlaying && showTouchSticks) {
        const float s = (uiDpiScale > 0.0f) ? uiDpiScale : 1.0f;
        const float r = touchStickRadius * s;
        const float grab = r * 1.7f;
        const ImVec2 leftC(imageMin.x + r + 26.0f * s, imageMax.y - r - 30.0f * s);
        const ImVec2 rightC(imageMax.x - r - 26.0f * s, imageMax.y - r - 30.0f * s);

        auto distTo = [](float x, float y, const ImVec2 &c) {
          const float dx = x - c.x, dy = y - c.y;
          return std::sqrt(dx * dx + dy * dy);
        };

        ImVec2 leftOff(0.0f, 0.0f), rightOff(0.0f, 0.0f);
        bool leftOn = false, rightOn = false;
        const int n = Modularity::AndroidRuntime::GetPointerCount();
        for (int i = 0; i < n; ++i) {
          int id = 0; float px = 0.0f, py = 0.0f;
          if (!Modularity::AndroidRuntime::GetPointer(i, &id, &px, &py)) continue;
          const float dl = distTo(px, py, leftC);
          const float dr = distTo(px, py, rightC);
          if (!leftOn && dl <= grab && dl <= dr) {
            leftOn = true; leftOff = ImVec2(px - leftC.x, py - leftC.y);
          } else if (!rightOn && dr <= grab) {
            rightOn = true; rightOff = ImVec2(px - rightC.x, py - rightC.y);
          }
        }
        auto clampLen = [&](ImVec2 o) {
          const float len = std::sqrt(o.x * o.x + o.y * o.y);
          if (len > r && len > 0.0f) { o.x *= r / len; o.y *= r / len; }
          return o;
        };
        leftOff = clampLen(leftOff);
        rightOff = clampLen(rightOff);

        if (leftOn) {
          const glm::vec3 right = glm::normalize(glm::cross(camera.front, camera.up));
          const float fwd = -leftOff.y / r, strafe = leftOff.x / r;
          camera.position += (camera.front * fwd + right * strafe) *
                             camera.moveSpeed * ImGui::GetIO().DeltaTime;
        }
        if (rightOn) {
          const float lookRate = touchStickSensitivity;
          const float yMul = touchStickInvertY ? 1.0f : -1.0f;
          camera.processMouseDelta(rightOff.x / r * lookRate,
                                   yMul * rightOff.y / r * lookRate);
        }

        ImDrawList *fg = ImGui::GetWindowDrawList();
        auto drawStick = [&](const ImVec2 &c, const ImVec2 &off, bool on) {
          fg->AddCircleFilled(c, r, IM_COL32(34, 38, 54, on ? 190 : 110));
          fg->AddCircle(c, r, IM_COL32(120, 140, 200, 200), 0, 2.0f * s);
          fg->AddCircleFilled(ImVec2(c.x + off.x, c.y + off.y), r * 0.42f,
                              IM_COL32(150, 170, 235, on ? 255 : 190));
        };
        drawStick(leftC, leftOff, leftOn);
        drawStick(rightC, rightOff, rightOn);
      }
#endif
    } else {
      ImTextureID texId = vulkanRenderer ? vulkanRenderer->getViewportSceneTextureID() : static_cast<ImTextureID>(0);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton("##SceneViewportPanelVk", panelSize, ImGuiButtonFlags_MouseButtonRight);
      if (texId != static_cast<ImTextureID>(0)) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(sceneLayout.panelMin, sceneLayout.panelMax, IM_COL32(10, 12, 18, 255));
        drawList->PushClipRect(sceneLayout.panelMin, sceneLayout.panelMax, true);
        drawList->AddImage(texId, sceneLayout.displayMin, sceneLayout.displayMax, sceneLayout.uvMin, sceneLayout.uvMax);
        drawList->PopClipRect();
      }
      ImVec2 imageMin = sceneLayout.displayMin;
      ImVec2 imageMax = sceneLayout.displayMax;
      viewportImageMin = imageMin;
      viewportImageMax = imageMax;
      hasViewportImageRect = true;
      glm::vec2 hoveredPixel(0.0f);
      mouseOverViewportImage = TryMapScreenPointToRenderPixel(
          sceneLayout, ImGui::GetIO().MousePos, viewportWidth, viewportHeight, hoveredPixel);
    }

    ImVec2 imageMin = sceneLayout.displayMin;
    ImVec2 imageMax = sceneLayout.displayMax;
    ImDrawList *viewportDrawList = ImGui::GetWindowDrawList();
    viewportDrawList->PushClipRect(sceneLayout.panelMin, sceneLayout.panelMax, true);

    if (worldUiEditing) {
      viewportDrawList->AddRectFilled(imageMin, imageMax, IM_COL32(14, 16, 20, 255));
    } else if (showSceneGrid3D) {
      auto clipLineToScreen = [&](glm::vec3 a, glm::vec3 b, ImVec2 &outA, ImVec2 &outB) -> bool {
        glm::vec4 va = view * glm::vec4(a, 1.0f);
        glm::vec4 vb = view * glm::vec4(b, 1.0f);
        const float nearZ = -buildSettings.editorCameraNear;
        if (va.z > nearZ && vb.z > nearZ) { return false;}
        if (va.z > nearZ || vb.z > nearZ) {
          float t = (nearZ - va.z) / (vb.z - va.z);
          t = std::clamp(t, 0.0f, 1.0f);
          glm::vec4 vclip = va + (vb - va) * t;
          if (va.z > nearZ) va = vclip;
          else              vb = vclip;
        }
        glm::vec4 ca = proj * va;
        glm::vec4 cb = proj * vb;
        if (ca.w <= 0.0f || cb.w <= 0.0f) return false;
        glm::vec3 ndcA = glm::vec3(ca) / ca.w;
        glm::vec3 ndcB = glm::vec3(cb) / cb.w;
        outA = ImVec2(imageMin.x + (ndcA.x * 0.5f + 0.5f) * (imageMax.x - imageMin.x), imageMin.y + (1.0f - (ndcA.y * 0.5f + 0.5f)) * (imageMax.y - imageMin.y));
        outB = ImVec2(imageMin.x + (ndcB.x * 0.5f + 0.5f) * (imageMax.x - imageMin.x), imageMin.y + (1.0f - (ndcB.y * 0.5f + 0.5f)) * (imageMax.y - imageMin.y));
        return true;
      };
      glm::vec2 camXZ(camera.position.x, camera.position.z);
      float camDist = glm::length(camXZ);
      float extent = 60.0f + camDist * 0.5f + std::abs(camera.position.y) * 4.0f;
      extent = std::clamp(extent, 60.0f, 1200.0f);
      float step = 1.0f;
      if (extent > 400.0f)      step = 20.0f;
      else if (extent > 200.0f) step = 10.0f;
      else if (extent > 120.0f) step = 5.0f;
      else if (extent > 70.0f)  step = 2.0f;
      float gridStrength = std::clamp(camDist / 120.0f, 0.15f, 1.0f);
      ImVec4 baseCol(0.35f, 0.43f, 0.55f, 0.55f * gridStrength);
      ImVec4 axisXCol(0.94f, 0.45f, 0.45f, 0.9f);
      ImVec4 axisZCol(0.5f, 0.7f, 0.95f, 0.9f);

      float startX = std::floor((camera.position.x - extent) / step) * step;
      float endX = std::floor((camera.position.x + extent) / step) * step;
      for (float x = startX; x <= endX; x += step) {
        float t = 1.0f - std::min(1.0f, std::abs(x - camera.position.x) / extent);
        ImVec4 col = baseCol;
        col.w *= t;
        if (col.w < 0.02f) continue;
        ImVec2 s0, s1;
        if (clipLineToScreen(glm::vec3(x, 0.0f, camera.position.z - extent),
                             glm::vec3(x, 0.0f, camera.position.z + extent), s0,
                             s1)) {
          viewportDrawList->AddLine(s0, s1, ImGui::GetColorU32(col), 1.0f);
        }
      }
      float startZ = std::floor((camera.position.z - extent) / step) * step;
      float endZ = std::floor((camera.position.z + extent) / step) * step;
      for (float z = startZ; z <= endZ; z += step) {
        float t = 1.0f - std::min(1.0f, std::abs(z - camera.position.z) / extent);
        ImVec4 col = baseCol;
        col.w *= t;
        if (col.w < 0.02f) continue;
        ImVec2 s0, s1;
        if (clipLineToScreen(glm::vec3(camera.position.x - extent, 0.0f, z),
                             glm::vec3(camera.position.x + extent, 0.0f, z), s0,
                             s1)) {
          viewportDrawList->AddLine(s0, s1, ImGui::GetColorU32(col), 1.0f);
        }
      }
      ImVec2 ax0, ax1;
      if (clipLineToScreen(glm::vec3(-extent, 0.0f, 0.0f),
                           glm::vec3(extent, 0.0f, 0.0f), ax0, ax1)) {
        viewportDrawList->AddLine(ax0, ax1, ImGui::GetColorU32(axisXCol), 2.0f);
      }
      ImVec2 az0, az1;
      if (clipLineToScreen(glm::vec3(0.0f, 0.0f, -extent),
                           glm::vec3(0.0f, 0.0f, extent), az0, az1)) {
        viewportDrawList->AddLine(az0, az1, ImGui::GetColorU32(axisZCol), 2.0f);
      }
    }

    auto importDroppedModel = [&](const fs::path &path) {
      std::error_code ec;
      fs::directory_entry entry(path, ec);
      if (ec) return;
      if (fileBrowser.isModelFile(entry)) {
        if (fileBrowser.isOBJFile(entry)) {importOBJToScene(path.string(), "");}
        else                              {importModelToScene(path.string(), "");}
        return;
      }
      if (fileBrowser.getFileCategory(entry) == FileCategory::Texture) {
        SceneObject* target = getSelectedObject();
        if (target && target->hasRenderer) {
          applyTextureAssetToObject(*target, entry.path());
        } else {
          addConsoleMessage("Select a renderable object before dropping a texture in Scene View.",
                            ConsoleMessageType::Warning);
        }
      }
    };

    // Materials, scripts and audio clips go onto the object under the cursor rather than into
    // the scene. The 3D view raycasts here; the 2D world view hit-tests its sprite rects further
    // down, once they are laid out for this frame.
    std::string viewportDragPath;
    const ObjectAssetDropKind viewportDragKind = activeObjectAssetDragKind(&viewportDragPath);
    if (viewportDragKind != ObjectAssetDropKind::None && mouseOverViewportImage && !pure2DSceneView) {
      // The raycast walks mesh triangles, so it only reruns when the cursor or camera moved.
      static ImVec2 lastDropPickMouse(-FLT_MAX, -FLT_MAX);
      static glm::mat4 lastDropPickViewProj(0.0f);
      static ObjectAssetDropKind lastDropPickKind = ObjectAssetDropKind::None;
      static int lastDropPickHit = -1;
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const glm::mat4 viewProj = proj * view;
      if (mouse.x != lastDropPickMouse.x || mouse.y != lastDropPickMouse.y ||
          viewProj != lastDropPickViewProj || viewportDragKind != lastDropPickKind) {
        lastDropPickHit = pickSceneObjectAtScreen(
            view, proj, imageMin, imageMax, mouse,
            [&](const SceneObject &candidate) { return canApplyAssetDrop(candidate, viewportDragKind); });
        lastDropPickMouse = mouse;
        lastDropPickViewProj = viewProj;
        lastDropPickKind = viewportDragKind;
      }
      setAssetDropHover(lastDropPickHit);
    }
    if (viewportDragKind != ObjectAssetDropKind::None && mouseOverViewportImage) {
      if (const SceneObject *hoverTarget = findObjectById(activeAssetDropHover())) {
        // Lights, cameras and empties have no mesh to outline, so the target is also named at the
        // cursor. Foreground list: the 2D world overlay is a child window drawn over this one.
        const char *action = viewportDragKind == ObjectAssetDropKind::Material ? "Assign material to "
                           : viewportDragKind == ObjectAssetDropKind::Script   ? "Add script to "
                                                                               : "Assign clip to ";
        const std::string tag = action + (hoverTarget->name.empty() ? std::string("Object") : hoverTarget->name);
        const ImVec2 textSize = ImGui::CalcTextSize(tag.c_str());
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 tagMin(mouse.x + 16.0f, mouse.y - textSize.y - 14.0f);
        const ImVec2 tagMax(tagMin.x + textSize.x + 12.0f, tagMin.y + textSize.y + 6.0f);
        ImDrawList *tagDrawList = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
        tagDrawList->PushClipRect(sceneLayout.panelMin, sceneLayout.panelMax, true);
        tagDrawList->AddRectFilled(tagMin, tagMax, IM_COL32(20, 22, 32, 220), 4.0f);
        tagDrawList->AddRect(tagMin, tagMax, ImGui::GetColorU32(ImGuiCol_DragDropTarget), 4.0f, 0, 1.5f);
        tagDrawList->AddText(ImVec2(tagMin.x + 6.0f, tagMin.y + 3.0f), IM_COL32(236, 240, 246, 255), tag.c_str());
        tagDrawList->PopClipRect();
      }
    }

    if (ImGui::BeginDragDropTarget()) {
      // An object drop draws its own highlight (the outline), not a rectangle round the view.
      const ImGuiDragDropFlags fileDropFlags =
          viewportDragKind != ObjectAssetDropKind::None ? ImGuiDragDropFlags_AcceptNoDrawDefaultRect : 0;
      if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("FILE_PATH", fileDropFlags)) {
        const char *path = static_cast<const char *>(payload->Data);
        if (viewportDragKind != ObjectAssetDropKind::None) {
          if (SceneObject *dropTarget = findObjectById(activeAssetDropHover())) {
            applyAssetDropToObject(*dropTarget, fs::path(path), true);
          } else {
            showEditorToast("Drop " + fs::path(path).filename().string() + " onto an object to assign it.",
                            ConsoleMessageType::Info, 1.8);
          }
        } else {
          importDroppedModel(fs::path(path));
        }
      }
      ImGui::EndDragDropTarget();
    }

    auto setCameraFacing = [&](const glm::vec3 &dir) {
      glm::vec3 worldUp = glm::vec3(0, 1, 0);
      glm::vec3 n = glm::normalize(dir);
      glm::vec3 up = worldUp;
      if (std::abs(glm::dot(n, worldUp)) > 0.98f) { up = glm::vec3(0, 0, 1); }
      glm::vec3 right = glm::normalize(glm::cross(up, n));
      if (glm::length(right) < 1e-4f) { right = glm::vec3(1, 0, 0); }
      up = glm::normalize(glm::cross(n, right));

      camera.front = n;
      camera.up = up;
      camera.pitch = glm::degrees(std::asin(glm::clamp(n.y, -1.0f, 1.0f)));
      camera.pitch = glm::clamp(camera.pitch, -89.0f, 89.0f);
      camera.yaw   = glm::degrees(std::atan2(n.z, n.x));
      camera.firstMouse = true;
    };

    // Draw small axis widget in top-right of viewport
    if (!worldUiEditing) {
      const float widgetSize = 94.0f;
      const float padding = 12.0f;
      ImVec2 center = ImVec2(imageMax.x - padding - widgetSize * 0.5f,
                             imageMin.y + padding + widgetSize * 0.5f);
      float radius = widgetSize * 0.46f;

      const glm::mat3 viewRotation = glm::mat3(view);
      const ImVec2 widgetMin =
          ImVec2(center.x - widgetSize * 0.5f, center.y - widgetSize * 0.5f);
      const ImVec2 widgetMax =
          ImVec2(center.x + widgetSize * 0.5f, center.y + widgetSize * 0.5f);
      const bool widgetHover = ImGui::IsMouseHoveringRect(widgetMin, widgetMax);
      const std::array<ViewportAxisArrow, 6> arrows = {{
          {glm::vec3(1, 0, 0),
           ImGui::GetColorU32(ImVec4(0.9f, 0.2f, 0.2f, 1.0f)), "X"},
          {glm::vec3(-1, 0, 0),
           ImGui::GetColorU32(ImVec4(0.6f, 0.2f, 0.2f, 1.0f)), ""},
          {glm::vec3(0, 1, 0),
           ImGui::GetColorU32(ImVec4(0.2f, 0.9f, 0.2f, 1.0f)), "Y"},
          {glm::vec3(0, -1, 0),
           ImGui::GetColorU32(ImVec4(0.2f, 0.6f, 0.2f, 1.0f)), ""},
          {glm::vec3(0, 0, 1),
           ImGui::GetColorU32(ImVec4(0.2f, 0.4f, 0.9f, 1.0f)), "Z"},
          {glm::vec3(0, 0, -1),
           ImGui::GetColorU32(ImVec4(0.2f, 0.3f, 0.6f, 1.0f)), ""},
      }};

      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const float clickRadius = 12.0f;
      const bool axisClickReleased =
          widgetHover && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
      const ViewportAxisArrow *clickedArrow = nullptr;
      std::array<ViewportAxisArrowRenderItem, 6> renderedArrows;

      for (size_t i = 0; i < arrows.size(); ++i) {
        const ViewportAxisArrow &arrow = arrows[i];
        const glm::vec3 cameraDirection = viewRotation * arrow.direction;
        renderedArrows[i] = {
            &arrow,
            BuildViewportAxisArrowGeometry(center, radius, cameraDirection),
            cameraDirection.z,
        };
      }

      std::sort(renderedArrows.begin(), renderedArrows.end(),
                [](const ViewportAxisArrowRenderItem &a,
                   const ViewportAxisArrowRenderItem &b) {
                  return a.cameraDepth < b.cameraDepth;
                });

      for (const ViewportAxisArrowRenderItem &renderedArrow : renderedArrows) {
        DrawViewportAxisArrow(viewportDrawList, renderedArrow.geometry,
                              *renderedArrow.arrow);

        if (axisClickReleased &&
            IsPointWithinRadius(mouse, renderedArrow.geometry.tip,
                                clickRadius)) {
          clickedArrow = renderedArrow.arrow;
        }
      }

      const char *projectionLabel = camera.orthographic ? "Ortho" : "Persp";
      const ImVec2 projectionLabelSize = ImGui::CalcTextSize(projectionLabel);
      viewportDrawList->AddText(
          ImVec2(center.x - projectionLabelSize.x * 0.5f,
                 center.y + radius * 0.72f),
          ImGui::GetColorU32(ImVec4(0.72f, 0.76f, 0.84f, 0.8f)),
          projectionLabel);

      if (clickedArrow) {
        setCameraFacing(clickedArrow->direction);
      }
      if (widgetHover) {
        blockSelection = true;
      }
    }

    ImGuiWindow *sceneViewportWindow = ImGui::GetCurrentWindow();
    ImGuiWindow *sceneViewportRootWindow =
        sceneViewportWindow ? sceneViewportWindow->RootWindowDockTree : nullptr;
    bool windowActive = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const float toolbarWidthEstimate = 520.0f;
    const float toolbarHeightEstimate = 42.0f;
    const float toolbarInsetX = 12.0f;
    const float toolbarInsetY = 12.0f;
    static ImVec2 toolbarSizeCache(toolbarWidthEstimate, toolbarHeightEstimate);
    ImVec2 toolbarRectMin(imageMin.x, imageMin.y);
    ImVec2 toolbarRectMax(imageMin.x, imageMin.y);
    auto computeToolbarRect = [&]() {
      float minX = imageMin.x + toolbarInsetX;
      float maxX = imageMax.x - toolbarInsetX;
      float minY = imageMin.y + toolbarInsetY;
      float maxY = imageMax.y - toolbarInsetY;
      float toolbarLeft = (sceneViewportToolbarCorner == ViewportToolbarCorner::BottomRight || sceneViewportToolbarCorner == ViewportToolbarCorner::TopRight) ? (maxX - toolbarSizeCache.x) : minX;
      if (toolbarLeft + toolbarSizeCache.x > maxX) toolbarLeft = maxX - toolbarSizeCache.x;
      if (toolbarLeft < minX)                      toolbarLeft = minX;
      float toolbarTop = (sceneViewportToolbarCorner == ViewportToolbarCorner::BottomLeft || sceneViewportToolbarCorner == ViewportToolbarCorner::BottomRight) ? (maxY - toolbarSizeCache.y) : minY;
      if (toolbarTop + toolbarSizeCache.y > maxY)  toolbarTop = maxY - toolbarSizeCache.y;
      if (toolbarTop < minY)                       toolbarTop = minY;
      toolbarRectMin = ImVec2(toolbarLeft, toolbarTop);
      toolbarRectMax = ImVec2(toolbarLeft + toolbarSizeCache.x, toolbarTop + toolbarSizeCache.y);
    };
    computeToolbarRect();
    static float toolbarHideAnim = 0.0f;
    float toolbarHideOffset = toolbarSizeCache.y + 14.0f;
    const float toolbarHideDirection = (sceneViewportToolbarCorner == ViewportToolbarCorner::TopLeft || sceneViewportToolbarCorner == ViewportToolbarCorner::TopRight) ? -1.0f : 1.0f;
    ImVec2 toolbarRectMinAnim = ImVec2(toolbarRectMin.x, toolbarRectMin.y + toolbarHideOffset * toolbarHideAnim * toolbarHideDirection);
    ImVec2 toolbarRectMaxAnim = ImVec2(toolbarRectMax.x, toolbarRectMax.y + toolbarHideOffset * toolbarHideAnim * toolbarHideDirection);
    bool mouseInViewportRect =
        ImGui::IsMouseHoveringRect(imageMin, imageMax, true);
    auto isToolbarHoverWindow = [&](ImGuiWindow *hoveredWindow) {
      if (!hoveredWindow) return false;
      if (sceneViewportRootWindow && hoveredWindow->RootWindowDockTree == sceneViewportRootWindow) return true;
      if (sceneViewportWindow && hoveredWindow->RootWindowPopupTree == sceneViewportWindow->RootWindowPopupTree) return true;
      return false;
    };
    auto isMouseInToolbarZone = [&](const ImVec2 &rectMin, const ImVec2 &rectMax) {
      ImGuiWindow *hoveredWindow = nullptr;
      ImGuiWindow *hoveredWindowUnderMovingWindow = nullptr;
      ImGui::FindHoveredWindowEx(ImGui::GetIO().MousePos, false, &hoveredWindow,
                                 &hoveredWindowUnderMovingWindow);
      if (!isToolbarHoverWindow(hoveredWindow)) return false;
      const bool overToolbarRect = ImGui::IsMouseHoveringRect(
          ImVec2(rectMin.x - 4.0f, rectMin.y - 4.0f),
          ImVec2(rectMax.x + 4.0f, rectMax.y + 4.0f), true);
      ImVec2 toolbarGuardMin(std::max(imageMin.x + 6.0f, rectMin.x - 12.0f), std::max(imageMin.y + 6.0f, rectMin.y - 12.0f));
      ImVec2 toolbarGuardMax(std::min(imageMax.x - 6.0f, rectMax.x + 12.0f), std::min(imageMax.y - 6.0f, rectMax.y + 12.0f));
      const bool overToolbarGuard = ImGui::IsMouseHoveringRect(toolbarGuardMin, toolbarGuardMax, true);
      return overToolbarRect || overToolbarGuard;
    };
    bool mouseInToolbarGuard =
        isMouseInToolbarZone(toolbarRectMinAnim, toolbarRectMaxAnim);
    bool mouseInToolbar = mouseInToolbarGuard;
    bool toolbarAllowed = !cursorLocked && !gameViewportFocused && !(isPlaying && showGameViewport);
    bool showViewportToolbar =
        toolbarAllowed && (windowActive || mouseInViewportRect || mouseInToolbar || toolbarHideAnim < 0.999f);
    bool toolbarHover = toolbarAllowed && (mouseInViewportRect || mouseInToolbar);
    float toolbarAnimSpeed = 10.0f;
    float toolbarTarget = toolbarHover ? 0.0f : 1.0f;
    if (worldUiEditing && toolbarAllowed && mouseInViewportRect) toolbarTarget = 0.0f;
    float toolbarAnimStep = 1.0f - std::exp(-toolbarAnimSpeed * ImGui::GetIO().DeltaTime);
    toolbarHideAnim += (toolbarTarget - toolbarHideAnim) * toolbarAnimStep;
    toolbarRectMin.y += toolbarHideOffset * toolbarHideAnim * toolbarHideDirection;
    toolbarRectMax.y += toolbarHideOffset * toolbarHideAnim * toolbarHideDirection;
    mouseInToolbar = isMouseInToolbarZone(toolbarRectMin, toolbarRectMax);
    if (showViewportToolbar && mouseInToolbar) blockSelection = true;

    SpriteTextureResolver spriteTextureResolver(rendererInitialized ? &renderer : nullptr);
    auto drawProjected25DSceneSprites = [&]() {
      auto brightenTint = [](const ImVec4 &c, float k) {
        return ImVec4(std::clamp(c.x * k, 0.0f, 1.0f),
                      std::clamp(c.y * k, 0.0f, 1.0f),
                      std::clamp(c.z * k, 0.0f, 1.0f), c.w);
      };
      BatchedSpriteEmitter spriteBatch(viewportDrawList);
      spriteBatch.reserve(sceneObjects.size());
      for (auto &obj : sceneObjects) {
        if (obj.type == ObjectType::Sprite25D)                                                   continue;
        if (!IsObjectEnabledInHierarchy(obj) || obj.type != ObjectType::Sprite25D || !obj.hasUI) continue;
        if (!(obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Sprite2D))    continue;
        ImVec2 rectMin, rectMax;
        if (!ResolveProjectedSprite25DRect(obj, view, proj, imageMin, ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y), rectMin, rectMax)) continue;
        ImVec2 drawSize(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
        if (drawSize.x <= 1.0f || drawSize.y <= 1.0f) continue;

        Texture *spriteTex = spriteTextureResolver.resolveTexture(obj);
        unsigned int texId = (spriteTex != nullptr) ? spriteTex->GetID() : 0;
        std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
        const int frame = resolveSpriteSheetFrame(obj);
        const ImVec2 sourceFrameSizePx = ResolveUiSourceFrameSizePx(obj, frame, spriteTex);
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b, obj.ui.color.a);
        const ImU32 tintColor = ImGui::GetColorU32(tint);
        float angle = glm::radians(obj.ui.rotation);
        if (DrawScrollingUiSprite(spriteBatch, (ImTextureID)(intptr_t)texId,
                                  obj, rectMin, rectMax, angle, tintColor,
                                  ImGui::GetTime())) {
        } else if (!DrawNineSliceSprite(spriteBatch, (ImTextureID)(intptr_t)texId, obj, rectMin, rectMax, uvQuad, sourceFrameSizePx, angle, tintColor)) {
          if (std::abs(angle) > 1e-4f) {
            ImVec2 center((rectMin.x + rectMax.x) * 0.5f, (rectMin.y + rectMax.y) * 0.5f);
            ImVec2 half(drawSize.x * 0.5f, drawSize.y * 0.5f);
            ImVec2 p0, p1, p2, p3;
            ViewportRenderHelpers::UiRotatedCornersAboutAnchor(
                obj.ui.anchor, center, half, angle, p0, p1, p2, p3);
            if (texId != 0) { spriteBatch.push((ImTextureID)(intptr_t)texId, p0, p1, p2, p3, uvQuad[0], uvQuad[1], uvQuad[2], uvQuad[3], tintColor); }
            else {
              spriteBatch.flush();
              ImU32 fill = tintColor;
              ImU32 border = ImGui::GetColorU32(brightenTint(tint, 0.85f));
              viewportDrawList->AddQuadFilled(p0, p1, p2, p3, fill);
              viewportDrawList->AddQuad(p0, p1, p2, p3, border, 1.5f);
            }
          } else if (texId != 0) {
            spriteBatch.push((ImTextureID)(intptr_t)texId, rectMin,
                             ImVec2(rectMax.x, rectMin.y), rectMax,
                             ImVec2(rectMin.x, rectMax.y), uvQuad[0],
                             ImVec2(uvQuad[2].x, uvQuad[0].y), uvQuad[2],
                             ImVec2(uvQuad[0].x, uvQuad[2].y), tintColor);
          } else {
            spriteBatch.flush();
            ImU32 fill = tintColor;
            ImU32 border = ImGui::GetColorU32(brightenTint(tint, 0.85f));
            viewportDrawList->AddRectFilled(rectMin, rectMax, fill, 4.0f);
            viewportDrawList->AddRect(rectMin, rectMax, border, 4.0f, 0, 1.5f);
          }
        }
      }

      // Particle Systems used to be overlaid here; the Renderer draws them into the 3D
      // frame itself now (Renderer::renderParticleSystems).
      spriteBatch.flush();
    };
    if (!worldUiEditing && !isProject25DPipeline()) drawProjected25DSceneSprites();

    bool uiWorldCameraActive = false;
    if (worldUiEditing) {
      UiSceneLookupCache uiSceneLookup(sceneObjects);
      int editCanvas3DId = -1;
      if (SceneObject *selected = getSelectedObject()) {editCanvas3DId = uiSceneLookup.find3DCanvasId(*selected);}
      auto isUIType = [&](const SceneObject &target) {
        if (target.type == ObjectType::Sprite25D) return false;
        if (!worldUiEditing)                                        return false;
        if (!target.hasUI || target.ui.type == UIElementType::None) return false;
        int canvasId = uiSceneLookup.find3DCanvasId(target);
        return (canvasId < 0) || (canvasId == editCanvas3DId);
      };
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::SetCursorScreenPos(imageMin);
      ImGui::BeginChild(
          "SceneUIWorldOverlay",
          ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y), false,
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
              ImGuiWindowFlags_NoScrollWithMouse |
              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

      ImVec2 overlayPos = ImGui::GetWindowPos();
      ImVec2 overlaySize = ImGui::GetWindowSize();
      uiWorldCamera.viewportSize = glm::vec2(overlaySize.x, overlaySize.y);
      bool mouseInToolbar =
          ImGui::IsMouseHoveringRect(
              ImVec2(toolbarRectMin.x - 4.0f, toolbarRectMin.y - 4.0f),
              ImVec2(toolbarRectMax.x + 4.0f, toolbarRectMax.y + 4.0f), true) ||
          mouseInToolbarGuard;
      bool uiWorldHover =
          (mouseOverViewportImage ||
           ImGui::IsWindowHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) && !mouseInToolbar;
      auto worldToScreen = [&](const glm::vec2 &world) {
        glm::vec2 local = uiWorldCamera.WorldToScreen(world);
        return ImVec2(overlayPos.x + local.x, overlayPos.y + local.y);
      };
      auto screenToWorld = [&](const ImVec2 &screen) {
        glm::vec2 local(screen.x - overlayPos.x, screen.y - overlayPos.y);
        return uiWorldCamera.ScreenToWorld(local);
      };
      auto parallaxOffset = [&](const SceneObject &obj) {
        if (!obj.hasParallaxLayer2D || !obj.parallaxLayer2D.enabled) return glm::vec2(0.0f);
        float factor = std::clamp(obj.parallaxLayer2D.factor, 0.0f, 1.0f);
        return uiWorldCamera.position * (1.0f - factor);
      };
      auto resolveUIRectWorld = [&](const SceneObject &obj, ImVec2 &outMin, ImVec2 &outMax) {
        if (obj.type == ObjectType::Sprite25D) {return ResolveProjectedSprite25DRect(obj, view, proj, overlayPos,overlaySize, outMin, outMax);}
        glm::vec2 parentOffset = uiSceneLookup.getWorldParentOffset(obj);
        float parentRot = uiSceneLookup.getWorldParentRotationRad(obj);
        glm::vec2 worldPos = parentOffset + ViewportRenderHelpers::RotateUiVec2(glm::vec2(obj.ui.position.x, obj.ui.position.y), parentRot) + parallaxOffset(obj);
        glm::vec2 sizeWorld = getSpriteDisplaySize(obj);
        ImVec2 pivotOffset = ImVec2(sizeWorld.x * 0.5f, sizeWorld.y * 0.5f);
        switch (obj.ui.anchor) {
        case UIAnchor::TopLeft: pivotOffset = ImVec2(0.0f, 0.0f); break;
        case UIAnchor::TopRight: pivotOffset = ImVec2(sizeWorld.x, 0.0f); break;
        case UIAnchor::BottomLeft: pivotOffset = ImVec2(0.0f, sizeWorld.y); break;
        case UIAnchor::BottomRight: pivotOffset = ImVec2(sizeWorld.x, sizeWorld.y); break;
        default: break;
        }
        glm::vec2 worldMin = worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
        glm::vec2 worldMax = worldMin + sizeWorld;
        ImVec2 s0 = worldToScreen(worldMin);
        ImVec2 s1 = worldToScreen(worldMax);
        outMin = ImVec2(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
        outMax = ImVec2(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
        return true;
      };
      auto rectOutsideOverlay = [&](const ImVec2 &min, const ImVec2 &max) {
        return (max.x < overlayPos.x || min.x > overlayPos.x + overlaySize.x || max.y < overlayPos.y || min.y > overlayPos.y + overlaySize.y);
      };

      if (uiWorldHover) {
        ImGuiIO &io = ImGui::GetIO();
        bool panHeld = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDown(ImGuiMouseButton_Left));
        if (panHeld) uiWorldPanning = true;
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !(ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
          uiWorldPanning = false;
        }
        if (uiWorldPanning) {
          ImVec2 delta = io.MouseDelta;
          if (delta.x != 0.0f || delta.y != 0.0f) {
            uiWorldCamera.position.x -= delta.x / uiWorldCamera.zoom;
            uiWorldCamera.position.y += delta.y / uiWorldCamera.zoom;
          }
          uiWorldCameraActive = true;
        }
        if (io.MouseWheel != 0.0f) {
          glm::vec2 mouseLocal(io.MousePos.x - overlayPos.x, io.MousePos.y - overlayPos.y);
          glm::vec2 worldBefore = uiWorldCamera.ScreenToWorld(mouseLocal);
          float zoomFactor = 1.0f + io.MouseWheel * 0.1f;
          float newZoom = std::clamp(uiWorldCamera.zoom * zoomFactor, 5.0f, 2000.0f);
          if (newZoom != uiWorldCamera.zoom) {
            uiWorldCamera.zoom = newZoom;
            glm::vec2 worldAfter = uiWorldCamera.ScreenToWorld(mouseLocal);
            uiWorldCamera.position += (worldBefore - worldAfter);
            uiWorldCameraActive = true;
          }
        }
        glm::vec2 panDir(0.0f);
        if (ImGui::IsKeyDown(ImGuiKey_A)) panDir.x -= 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_D)) panDir.x += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_W)) panDir.y += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_S)) panDir.y -= 1.0f;
        if (panDir.x != 0.0f || panDir.y != 0.0f) {
          float panSpeed = 6.0f;
          if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) panSpeed *= 2.5f;
          uiWorldCamera.position += panDir * (panSpeed * deltaTime);
          uiWorldCameraActive = true;
        }
      }

      auto brighten = [](const ImVec4 &c, float k) {
        return ImVec4(std::clamp(c.x * k, 0.0f, 1.0f), std::clamp(c.y * k, 0.0f, 1.0f), std::clamp(c.z * k, 0.0f, 1.0f), c.w);
      };

      if (showUIWorldGrid) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 overlayMax(overlayPos.x + overlaySize.x, overlayPos.y + overlaySize.y);
        dl->PushClipRect(overlayPos, overlayMax, true);
        float step = 1.0f;
        float minPx = 30.0f;
        float maxPx = 140.0f;
        while (step * uiWorldCamera.zoom < minPx) step *= 2.0f;
        while (step * uiWorldCamera.zoom > maxPx) step *= 0.5f;

        glm::vec2 worldMin = uiWorldCamera.ScreenToWorld(glm::vec2(0.0f, overlaySize.y));
        glm::vec2 worldMax = uiWorldCamera.ScreenToWorld(glm::vec2(overlaySize.x, 0.0f));
        float startX = std::floor(worldMin.x / step) * step;
        float endX = std::ceil(worldMax.x / step) * step;
        float startY = std::floor(worldMin.y / step) * step;
        float endY = std::ceil(worldMax.y / step) * step;
        ImU32 gridColor = IM_COL32(90, 110, 140, 50);
        ImU32 axisColorX = IM_COL32(240, 120, 120, 170);
        ImU32 axisColorY = IM_COL32(120, 240, 150, 170);

        for (float x = startX; x <= endX; x += step) {
          ImVec2 p0 = worldToScreen(glm::vec2(x, worldMin.y));
          ImVec2 p1 = worldToScreen(glm::vec2(x, worldMax.y));
          dl->AddLine(p0, p1, gridColor, 1.0f);
        }
        for (float y = startY; y <= endY; y += step) {
          ImVec2 p0 = worldToScreen(glm::vec2(worldMin.x, y));
          ImVec2 p1 = worldToScreen(glm::vec2(worldMax.x, y));
          dl->AddLine(p0, p1, gridColor, 1.0f);
        }

        ImVec2 axisX0 = worldToScreen(glm::vec2(worldMin.x, 0.0f));
        ImVec2 axisX1 = worldToScreen(glm::vec2(worldMax.x, 0.0f));
        ImVec2 axisY0 = worldToScreen(glm::vec2(0.0f, worldMin.y));
        ImVec2 axisY1 = worldToScreen(glm::vec2(0.0f, worldMax.y));
        dl->AddLine(axisX0, axisX1, axisColorX, 2.0f);
        dl->AddLine(axisY0, axisY1, axisColorY, 2.0f);
        dl->PopClipRect();
      }

      if (showCanvasOverlay && worldUiReferenceResolutionWidth > 0 && worldUiReferenceResolutionHeight > 0) {
        ImDrawList *dl = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
        ImVec2 overlayMax(overlayPos.x + overlaySize.x, overlayPos.y + overlaySize.y);
        dl->PushClipRect(overlayPos, overlayMax, true);

        const float fitScale = std::max(
            0.01f,
            std::min(overlaySize.x / static_cast<float>(worldUiReferenceResolutionWidth), overlaySize.y / static_cast<float>(worldUiReferenceResolutionHeight)));
        const ImVec2 frameSize(
            static_cast<float>(worldUiReferenceResolutionWidth) * fitScale,
            static_cast<float>(worldUiReferenceResolutionHeight) * fitScale);
        const ImVec2 frameMin(
            overlayPos.x + (overlaySize.x - frameSize.x) * 0.5f,
            overlayPos.y + (overlaySize.y - frameSize.y) * 0.5f);
        const ImVec2 frameMax(frameMin.x + frameSize.x, frameMin.y + frameSize.y);
        dl->AddRect(frameMin, frameMax, IM_COL32(84, 176, 255, 220), 4.0f, 0, 2.0f);

        char label[80];
        std::snprintf(label, sizeof(label), "Resolution %dx%d", worldUiReferenceResolutionWidth, worldUiReferenceResolutionHeight);
        ImVec2 labelSize = ImGui::CalcTextSize(label);
        ImVec2 labelPad(6.0f, 3.0f);
        ImVec2 labelMin(frameMin.x + 8.0f, frameMin.y + 8.0f);
        ImVec2 labelMax(labelMin.x + labelSize.x + labelPad.x * 2.0f, labelMin.y + labelSize.y + labelPad.y * 2.0f);
        dl->AddRectFilled(labelMin, labelMax, IM_COL32(12, 22, 34, 190), 4.0f);
        dl->AddRect(labelMin, labelMax, IM_COL32(84, 176, 255, 180), 4.0f, 0, 1.0f);
        dl->AddText(ImVec2(labelMin.x + labelPad.x, labelMin.y + labelPad.y), IM_COL32(214, 240, 255, 235), label);
        dl->PopClipRect();
      }

      if (showSceneGizmos && gizmoShowCameraOverlays) {
        ImDrawList *dl = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
        ImVec2 overlayMax(overlayPos.x + overlaySize.x, overlayPos.y + overlaySize.y);
        dl->PushClipRect(overlayPos, overlayMax, true);

        for (const auto &camObj : sceneObjects) {
          if (!camObj.hasCamera || !IsObjectEnabledInHierarchy(camObj)) continue;
          const bool cameraIs2D = project2DPipeline || camObj.camera.use2D;
          if (!cameraIs2D) continue;

          const float alpha = camObj.enabled ? 1.0f : 0.35f;
          const float pixelsPerUnit = std::max(1.0f, camObj.camera.pixelsPerUnit);
          const int cameraResolutionWidth = activeGameResolutionWidth;
          const int cameraResolutionHeight = activeGameResolutionHeight;
          const float halfWidth = static_cast<float>(cameraResolutionWidth) / (2.0f * pixelsPerUnit);
          const float halfHeight = static_cast<float>(cameraResolutionHeight) / (2.0f * pixelsPerUnit);

          glm::quat q = EulerXYZDegreesToQuat(camObj.rotation);
          glm::mat3 rot = glm::mat3_cast(q);
          glm::vec2 right(rot[0].x, rot[0].y);
          glm::vec2 up(rot[1].x, rot[1].y);
          if (glm::length(right) < 1e-4f) right = glm::vec2(1.0f, 0.0f);
          if (glm::length(up) < 1e-4f) up = glm::vec2(0.0f, 1.0f);
          right = glm::normalize(right);
          up = glm::normalize(up);

          glm::vec2 center(camObj.position.x, camObj.position.y);
          std::array<glm::vec2, 4> corners = {
              center - right * halfWidth + up * halfHeight,
              center + right * halfWidth + up * halfHeight,
              center + right * halfWidth - up * halfHeight,
              center - right * halfWidth - up * halfHeight};
          std::array<ImVec2, 4> screenCorners = {
              worldToScreen(corners[0]), worldToScreen(corners[1]),
              worldToScreen(corners[2]), worldToScreen(corners[3])};

          ImU32 boundsCol =
              ImGui::GetColorU32(ImVec4(0.28f, 0.88f, 1.0f, 0.92f * alpha));
          ImU32 diagCol = ImGui::GetColorU32(ImVec4(0.28f, 0.88f, 1.0f, 0.45f * alpha));
          const float edgeThickness = std::max( 1.4f, 2.0f * std::clamp(sceneGizmoOverlayScale, 0.4f, 3.0f));
          for (int i = 0; i < 4; ++i) {
            dl->AddLine(screenCorners[i], screenCorners[(i + 1) % 4], boundsCol, edgeThickness);
          }
          dl->AddLine(screenCorners[0], screenCorners[2], diagCol, 1.2f);
          dl->AddLine(screenCorners[1], screenCorners[3], diagCol, 1.2f);

          ImVec2 camCenter = worldToScreen(center);
          ImVec2 camForward = worldToScreen(center + up * std::max(0.1f, halfHeight * 0.45f));
          dl->AddLine(
              camCenter, camForward,
              ImGui::GetColorU32(ImVec4(0.94f, 0.98f, 1.0f, 0.88f * alpha)),
              edgeThickness);

          if (gizmoShowCameraFrustumLabels) {
            char label[96];
            std::snprintf(label, sizeof(label), "2D %dx%d | %.2fx%.2f",
                          cameraResolutionWidth, cameraResolutionHeight,
                          halfWidth * 2.0f, halfHeight * 2.0f);
            ImVec2 textSize = ImGui::CalcTextSize(label);
            ImVec2 labelPos(screenCorners[1].x + 6.0f,
                            screenCorners[1].y - textSize.y - 4.0f);
            ImVec2 pad(4.0f, 2.0f);
            ImVec2 bgMin(labelPos.x, labelPos.y);
            ImVec2 bgMax(labelPos.x + textSize.x + pad.x * 2.0f,
                         labelPos.y + textSize.y + pad.y * 2.0f);
            dl->AddRectFilled(
                bgMin, bgMax,
                IM_COL32(16, 24, 34, static_cast<int>(195.0f * alpha)), 4.0f);
            dl->AddRect(
                bgMin, bgMax,
                IM_COL32(120, 200, 240, static_cast<int>(180.0f * alpha)), 4.0f,
                0, 1.0f);
            dl->AddText(
                ImVec2(bgMin.x + pad.x, bgMin.y + pad.y),
                IM_COL32(214, 242, 255, static_cast<int>(235.0f * alpha)),
                label);
          }
        }

        dl->PopClipRect();
      }

      float animSpeed = 0.0f;
      if (uiAnimationMode == UIAnimationMode::Fluid)       animSpeed = 8.0f;
      else if (uiAnimationMode == UIAnimationMode::Snappy) animSpeed = 18.0f;
      float animStep =
          (uiAnimationMode == UIAnimationMode::Off) ? 1.0f : (1.0f - std::exp(-animSpeed * ImGui::GetIO().DeltaTime));
      auto animateValue = [&](float &current, float target, bool immediate) {
        if (uiAnimationMode == UIAnimationMode::Off || immediate) current = target;
        else current += (target - current) * animStep;
        return current;
      };

      std::vector<SceneObject *> uiDrawList;
      uiDrawList.reserve(sceneObjects.size());
      bool needsSort = false;
      for (auto &obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        if (!isUIType(obj) && !ViewportRenderHelpers::IsAssemblageLayerDrawable(obj))
          continue;
        uiDrawList.push_back(&obj);
        needsSort =
            needsSort ||
            (obj.hasParallaxLayer2D && obj.parallaxLayer2D.enabled &&
             obj.parallaxLayer2D.order != 0) ||
            (obj.hasUI && obj.ui.sortingOrder != 0) ||
            (obj.hasAssemblageLayer && obj.assemblageLayer.sortingOrder != 0);
      }
      if (needsSort && uiDrawList.size() > 1) StableSortRuntimeUiDrawList(uiDrawList);

      glm::vec2 worldViewMin = uiWorldCamera.ScreenToWorld(glm::vec2(0.0f, overlaySize.y));
      glm::vec2 worldViewMax = uiWorldCamera.ScreenToWorld(glm::vec2(overlaySize.x, 0.0f));
      BatchedSpriteEmitter spriteBatch(ImGui::GetWindowDrawList());
      spriteBatch.reserve(uiDrawList.size());
      struct ResolvedUiRect { ImVec2 min; ImVec2 max;};
      std::unordered_map<int, ResolvedUiRect> resolvedUiRects;
      resolvedUiRects.reserve(uiDrawList.size());
      std::vector<SceneObject *> drawnUiObjects;
      drawnUiObjects.reserve(uiDrawList.size());
      auto resolveCanvasMaskRectForObject = [&](const SceneObject &obj, ImVec2 &outMin, ImVec2 &outMax) -> bool {
        bool hasMask = false;
        ImVec2 maskMin(0.0f, 0.0f);
        ImVec2 maskMax(0.0f, 0.0f);
        const SceneObject *current = &obj;
        while (current && current->parentId >= 0) {
          current = uiSceneLookup.find(current->parentId);
          if (!current) break;
          if (!(current->hasUI && current->ui.type == UIElementType::Canvas && current->ui.maskChildren)) continue;

          ImVec2 canvasMin, canvasMax;
          if (!resolveUIRectWorld(*current, canvasMin, canvasMax)) continue;

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

        if (!hasMask) return false;
        outMin = maskMin;
        outMax = maskMax;
        return (outMax.x > outMin.x) && (outMax.y > outMin.y);
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
      const bool captureLight2DRoutingReasons = selectedForRoutingReasons && selectedForRoutingReasons->hasUI;
      if (captureLight2DRoutingReasons) light2DRoutingReasons.reserve(uiDrawList.size());
      auto setLight2DRoutingReason = [&](int objectId, const char *reason) {
        if (captureLight2DRoutingReasons) light2DRoutingReasons[objectId] = reason;
      };
      if (rendererInitialized) {
        Light2DRenderRequest lightRequest;
        lightRequest.width = std::max(1, static_cast<int>(std::round(overlaySize.x)));
        lightRequest.height = std::max(1, static_cast<int>(std::round(overlaySize.y)));
        lightRequest.clearColor = glm::vec4(0.0f);
        lightRequest.baseAmbient = glm::vec3(0.0f);
        lightRequest.lightingBufferScale = buildSettings.light2DLightingResolution;
        lightRequest.lightingBufferScaleAuto = buildSettings.light2DLightingResolutionAuto;
        lightRequest.lightingPixelFilter = buildSettings.light2DLightingPixelFilter;
        lightRequest.lightingSDR = buildSettings.light2DLightingSDR;
        lightRequest.postFx = resolveWorld2DPostFx(uiWorldCamera);
        lightRequest.blendStyles = light2DBlendStyles;
        auto computeFlickerMultiplier =
            [](const Light2DFlickerSettings &flicker) {
              if (!flicker.enabled || flicker.amount <= 0.0001f) return 1.0f;
              const float time = static_cast<float>(glfwGetTime());
              const float base = std::sin( time * std::max(0.01f, flicker.speed) + flicker.seed);
              const float jitter = std::sin(time * std::max(0.01f, flicker.speed * 2.173f) + flicker.seed * 1.913f);
              const float noise = 0.5f + 0.35f * base + 0.15f * jitter;
              return glm::mix(1.0f, std::max(0.0f, noise), std::clamp(flicker.amount, 0.0f, 1.0f));
            };

        int spriteDrawOrder = 0;
        assemblageTileQuadsLastFrame = 0;
        for (SceneObject *objPtr : uiDrawList) {
          SceneObject &obj = *objPtr;
          if (ViewportRenderHelpers::IsAssemblageLayerDrawable(obj)) {
            const int layerDrawOrder = spriteDrawOrder++;
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
            continue;
          }
          if (!(obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Sprite2D)) continue;
          if (ViewportRenderHelpers::HasUiImageMaterialShader(obj)) {
            setLight2DRoutingReason(
                obj.id, "Material shader: rendered by the UI image shader path.");
            continue;
          }
          if (obj.ui.nineSliceEnabled) {
            setLight2DRoutingReason(obj.id, "Legacy path: nine-slice sprites are not " "routed through Light2D yet.");
            continue;
          }
          if (obj.ui.unlitLighting2D) {
            setLight2DRoutingReason(obj.id, "Legacy path: Force Unlit keeps this " "sprite on the legacy 2D renderer.");
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
          if (!resolveUIRectWorld(obj, rectMin, rectMax)) {
            setLight2DRoutingReason( obj.id, "Legacy path: failed to resolve a world-space sprite " "rect for the active viewport.");
            continue;
          }
          if (!disableCulling && !repeatX && !repeatY &&
              rectOutsideOverlay(rectMin, rectMax)) {
            setLight2DRoutingReason(obj.id, "Skipped Light2D: object is outside the " "visible 2D world overlay.");
            continue;
          }

          Texture *spriteTex = spriteTextureResolver.resolveTexture(obj);
          if (!spriteTex || spriteTex->GetID() == 0) {
            setLight2DRoutingReason(obj.id, "Legacy path: no sprite texture is bound for this object.");
            continue;
          }

          std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
          const float angle = uiSceneLookup.getWorldParentRotationRad(obj) + glm::radians(obj.ui.rotation);
          const float c = std::cos(angle);
          const float s = std::sin(angle);
          ImVec2 maskMin, maskMax;
          const bool hasMaskRect = resolveCanvasMaskRectForObject(obj, maskMin, maskMax);
          auto appendSpriteQuad = [&](const ImVec2 &quadMin, const ImVec2 &quadMax) {
            if (!disableCulling && rectOutsideOverlay(quadMin, quadMax)) return false;
            if (hasMaskRect) {
              const bool maskClipsSprite = quadMin.x < maskMin.x || quadMax.x > maskMax.x || quadMin.y < maskMin.y || quadMax.y > maskMax.y;
              if (maskClipsSprite) return false;
            }

            Light2DScreenSprite sprite;
            sprite.objectId = obj.id;
            sprite.layer = obj.layer;
            sprite.drawOrder = spriteDrawOrder++;
            sprite.textureId = spriteTex->GetID();
            sprite.tint = obj.ui.color;
            sprite.receiveLighting = obj.ui.receiveLighting2D;
            sprite.unlit = obj.ui.unlitLighting2D;
            sprite.emissiveIntensity = obj.ui.emissiveLighting2D;

            const glm::vec2 center(
                ((quadMin.x + quadMax.x) * 0.5f) - overlayPos.x,
                ((quadMin.y + quadMax.y) * 0.5f) - overlayPos.y);
            const glm::vec2 half(
                std::max(0.5f, (quadMax.x - quadMin.x) * 0.5f),
                std::max(0.5f, (quadMax.y - quadMin.y) * 0.5f));
            auto rotatePoint = [&](float x, float y) {
              return glm::vec2(center.x + x * c - y * s, center.y + x * s + y * c);
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
            glm::vec2 spacing = obj.hasParallaxLayer2D ? obj.parallaxLayer2D.repeatSpacing : glm::vec2(0.0f);
            float stepX = spriteSizeWorld.x + spacing.x;
            float stepY = spriteSizeWorld.y + spacing.y;
            ImVec2 pivotOffset(spriteSizeWorld.x * 0.5f, spriteSizeWorld.y * 0.5f);
            switch (obj.ui.anchor) {
            case UIAnchor::TopLeft: pivotOffset = ImVec2(0.0f, 0.0f);                               break;
            case UIAnchor::TopRight: pivotOffset = ImVec2(spriteSizeWorld.x, 0.0f);                 break;
            case UIAnchor::BottomLeft: pivotOffset = ImVec2(0.0f, spriteSizeWorld.y);               break;
            case UIAnchor::BottomRight: pivotOffset = ImVec2(spriteSizeWorld.x, spriteSizeWorld.y); break;
            default:                                                                                     break;
            }
            glm::vec2 parentOffset = uiSceneLookup.getWorldParentOffset(obj);
            glm::vec2 worldPos = parentOffset + glm::vec2(obj.ui.position.x, obj.ui.position.y) + parallaxOffset(obj);
            glm::vec2 baseWorldMin = worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
            int startX = repeatX ? static_cast<int>(std::floor((worldViewMin.x - baseWorldMin.x) / stepX)) - 1 : 0;
            int endX = repeatX ? static_cast<int>(std::ceil((worldViewMax.x - baseWorldMin.x) / stepX)) + 1 : 0;
            int startY = repeatY ? static_cast<int>(std::floor((worldViewMin.y - baseWorldMin.y) / stepY)) - 1 : 0;
            int endY = repeatY ? static_cast<int>(std::ceil((worldViewMax.y - baseWorldMin.y) / stepY)) + 1 : 0;
            for (int ix = startX; ix <= endX; ++ix) {
              for (int iy = startY; iy <= endY; ++iy) {
                float dx = repeatX ? static_cast<float>(ix) * stepX : 0.0f;
                float dy = repeatY ? static_cast<float>(iy) * stepY : 0.0f;
                glm::vec2 tileMin = baseWorldMin + glm::vec2(dx, dy);
                ImVec2 s0 = worldToScreen(tileMin);
                ImVec2 s1 = worldToScreen( tileMin + glm::vec2(spriteSizeWorld.x, spriteSizeWorld.y));
                ImVec2 tileRectMin(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
                ImVec2 tileRectMax(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
                addedAnySprite = appendSpriteQuad(tileRectMin, tileRectMax) || addedAnySprite;
              }
            }
          } else addedAnySprite = appendSpriteQuad(rectMin, rectMax);

          if (!addedAnySprite) {
            setLight2DRoutingReason(obj.id, hasMaskRect
                    ? "Legacy path: repeating or masked tiles still use legacy " "rendering when the canvas clip cuts the visible tile."
                           : "Skipped Light2D: object has no visible tiles inside the " "current 2D world overlay.");
            continue;
          }

          light2DRenderedObjectIds.insert(obj.id);
          if (obj.ui.type == UIElementType::Sprite2D) {
            if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D) ++litSprite2DCount;
          } else if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D)++litWorldImageCount;
          if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D)
                                           setLight2DRoutingReason(obj.id, repeatX || repeatY ? "Lit path: repeating parallax tiles are routed through " "the Light2D compositor." : "Lit path: routed through the Light2D compositor.");
          else if (obj.ui.unlitLighting2D) setLight2DRoutingReason(obj.id, "Lit compositor path: object is routed, " "but Force Unlit is enabled.");
          else                             setLight2DRoutingReason(obj.id, "Lit compositor path: object is routed, " "but Receive Lighting is disabled.");
        }

        auto projectedParticleOutside = [&](const ImVec2 &min, const ImVec2 &max) { return max.x < 0.0f || min.x > overlaySize.x || max.y < 0.0f || min.y > overlaySize.y; };
        AppendProjectedParticleSystem2DSprites(
            sceneObjects, renderer, glfwGetTime(), view, proj, overlayPos,
            overlaySize, overlayPos, projectedParticleOutside, lightRequest,
            spriteDrawOrder);

        for (const SceneObject &obj : sceneObjects) {
          if (!IsObjectEnabledInHierarchy(obj) || !obj.hasLight2D || !obj.light2D.enabled) continue;
          ++activeLight2DCount;

          if (obj.light2D.type == Light2DType::Global) {
            lightRequest.baseAmbient += glm::vec3(obj.light2D.color) * obj.light2D.intensity;
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
          light.offscreenCullMarginPx = obj.light2D.offscreenCullMargin * uiWorldCamera.zoom;
          light.color = obj.light2D.color;
          light.intensity = obj.light2D.intensity * computeFlickerMultiplier(obj.light2D.flicker);
          light.radius = std::max(obj.light2D.radius, obj.light2D.outerRadius) * uiWorldCamera.zoom;
          light.innerRadius = obj.light2D.innerRadius * uiWorldCamera.zoom;
          light.outerRadius = std::max(obj.light2D.innerRadius, obj.light2D.outerRadius) * uiWorldCamera.zoom;
          light.falloffStrength = obj.light2D.falloffStrength;
          light.innerSpotAngle = obj.light2D.innerSpotAngle;
          light.outerSpotAngle = obj.light2D.outerSpotAngle;
          light.shadowStrength = obj.light2D.shadowStrength;
          light.volumetricEnabled = obj.light2D.volumetricEnabled;
          light.castsShadows = obj.light2D.castsShadows;
          light.rotationRad = glm::radians(obj.rotation.z);
          light.cookieScale = obj.light2D.cookieScale;
          light.cookieRotationRad = glm::radians(obj.light2D.cookieRotation);
          light.freeformFeatherPx = obj.light2D.freeformFeather * uiWorldCamera.zoom;
          light.freeformEdgeFalloff = obj.light2D.freeformEdgeFalloff;
          if (!obj.light2D.cookieTexturePath.empty()) {
            if (Texture *cookieTexture = renderer.getTexture(obj.light2D.cookieTexturePath, MaterialProperties::TextureFilter::Bilinear))
              light.cookieTextureId = cookieTexture->GetID();
          }

          ImVec2 lightPos = worldToScreen(glm::vec2(obj.position.x, obj.position.y));
          light.position = glm::vec2(lightPos.x - overlayPos.x, lightPos.y - overlayPos.y);

          if (obj.light2D.type == Light2DType::Freeform || obj.light2D.type == Light2DType::Sprite) {
            light.polygon.reserve(obj.light2D.shapePoints.size());
            for (const glm::vec2 &point : obj.light2D.shapePoints) {
              ImVec2 screenPoint = worldToScreen(glm::vec2(obj.position.x + point.x, obj.position.y + point.y));
              light.polygon.emplace_back(screenPoint.x - overlayPos.x, screenPoint.y - overlayPos.y);
            }
            if (!light.polygon.empty()) {
              glm::vec2 boundsMin(FLT_MAX);
              glm::vec2 boundsMax(-FLT_MAX);
              for (const glm::vec2 &point : light.polygon) {
                boundsMin.x = std::min(boundsMin.x, point.x);
                boundsMin.y = std::min(boundsMin.y, point.y);
                boundsMax.x = std::max(boundsMax.x, point.x);
                boundsMax.y = std::max(boundsMax.y, point.y);
              }
              light.boundsMin = boundsMin;
              light.boundsMax = boundsMax;
            }
          }
          else {
            const float extent = std::max(light.radius, light.outerRadius);
            light.boundsMin = light.position - glm::vec2(extent);
            light.boundsMax = light.position + glm::vec2(extent);
          }
          lightRequest.lights.push_back(light);
        }

        for (const SceneObject &obj : sceneObjects) {
          if (!IsObjectEnabledInHierarchy(obj) || !obj.hasShadowCaster2D || !obj.shadowCaster2D.enabled) continue;

          Light2DScreenShadowCaster caster;
          caster.objectId = obj.id;
          caster.enabled = obj.shadowCaster2D.enabled;
          caster.targetAllLayers = obj.shadowCaster2D.targetAllLayers;
          caster.targetLayerMask = obj.shadowCaster2D.targetLayerMask;
          caster.shadowStrength = obj.shadowCaster2D.shadowStrength;
          caster.polygon.reserve(obj.shadowCaster2D.points.size());
          for (const glm::vec2 &point : obj.shadowCaster2D.points) {
            ImVec2 screenPoint = worldToScreen(glm::vec2(obj.position.x + point.x, obj.position.y + point.y)); caster.polygon.emplace_back(screenPoint.x - overlayPos.x, screenPoint.y - overlayPos.y);
          }
          if (caster.polygon.size() >= 3) lightRequest.shadowCasters.push_back(std::move(caster));
        }

        const bool hasAmbientOnly = glm::length(lightRequest.baseAmbient) > 0.0001f;
        const bool wantsPostFxComposite = Light2DHasVisiblePostFx(lightRequest.postFx);
        lightBufferHadContent = hasAmbientOnly || !lightRequest.lights.empty();
        if (!lightRequest.sprites.empty() && (hasAmbientOnly || !lightRequest.lights.empty() || wantsPostFxComposite)) {
          unsigned int lightTexture = lighting2DRenderer.render(lightRequest, renderer);
          if (lightTexture != 0) {
            Camera effectCamera;
            effectCamera.position = glm::vec3(uiWorldCamera.position.x, uiWorldCamera.position.y, 0.0f);
            effectCamera.front = glm::vec3(0.0f, 0.0f, -1.0f);
            effectCamera.up = glm::vec3(0.0f, 1.0f, 0.0f);
            effectCamera.orthographic = true;
            effectCamera.pixelsPerUnit = std::max(1.0f, uiWorldCamera.zoom);
            unsigned int presentedTexture = renderer.postProcessTexture( effectCamera, sceneObjects, lightTexture, lightRequest.width, lightRequest.height, false);
            ImGui::GetWindowDrawList()->AddImage(
                (ImTextureID)(intptr_t)presentedTexture, overlayPos,
                ImVec2(overlayPos.x + overlaySize.x,
                       overlayPos.y + overlaySize.y),
                ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            renderedLight2DComposite = true;
            light2DStats = lighting2DRenderer.getLastStats();
          } else {
            for (int objectId : light2DRenderedObjectIds) {
              setLight2DRoutingReason(objectId, "Legacy path: Light2D compositor did not produce a " "valid output texture this frame.");
            }
            light2DRenderedObjectIds.clear();
          }
        } else {
          for (int objectId : light2DRenderedObjectIds) {
            setLight2DRoutingReason( objectId, wantsPostFxComposite
                    ? "Legacy path: the Light2D compositor was expected to run " "for 2D post FX, but no output was requested."
                    : "Legacy path: no active Light2D or Global Light2D " "affected this frame.");
          }
          light2DRenderedObjectIds.clear();
        }
      }

      for (SceneObject *objPtr : uiDrawList) {
        SceneObject &obj = *objPtr;
        if (ViewportRenderHelpers::IsAssemblageLayerDrawable(obj)) {
          if (light2DRenderedObjectIds.count(obj.id) == 0) {
            assemblageTileQuadsLastFrame += ViewportRenderHelpers::AppendAssemblageLayerQuads(
                sceneObjects, obj, assemblageRuntime, renderer, glfwGetTime(),
                worldViewMin, worldViewMax, worldToScreen,
                [&](const ViewportRenderHelpers::AssemblageScreenQuad &quad) {
                  spriteBatch.push(quad.textureId, quad.pos[0], quad.pos[1], quad.pos[2],
                                   quad.pos[3], quad.uv[0], quad.uv[1], quad.uv[2],
                                   quad.uv[3],
                                   ImGui::GetColorU32(ImVec4(quad.tint.r, quad.tint.g,
                                                             quad.tint.b, quad.tint.a)));
                });
          }
          continue;
        }
        ImVec2 rectMin, rectMax;
        if (!resolveUIRectWorld(obj, rectMin, rectMax)) continue;
        ImVec2 rectSize(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
        if (rectSize.x <= 1.0f || rectSize.y <= 1.0f) continue;
        const bool disableCulling = obj.hasParallaxLayer2D && obj.parallaxLayer2D.enabled && obj.parallaxLayer2D.disableCulling;
        if (!disableCulling && rectOutsideOverlay(rectMin, rectMax)) continue;
        resolvedUiRects[obj.id] = ResolvedUiRect{rectMin, rectMax};
        drawnUiObjects.push_back(&obj);

        ImGuiStyle savedStyle = ImGui::GetStyle();
        bool styleApplied = false;
        bool fontApplied = false;
        if (!obj.ui.stylePreset.empty()) {
          if (const auto *preset = getUIStylePreset(obj.ui.stylePreset)) {
            ImGui::GetStyle() = preset->style;
            styleApplied = true;
            if (ImFont *presetFont = getUIFontForContext(preset->fontAsset, ImGui::GetCurrentContext())) {
              ImGui::PushFont(presetFont, preset->style.FontSizeBase);
              fontApplied = true;
            }
          }
        }

        if (obj.ui.type == UIElementType::Canvas) {
          spriteBatch.flush();
          ImDrawList *dl = ImGui::GetWindowDrawList();
          const ImU32 edgeColor = obj.ui.maskChildren ? IM_COL32(74, 228, 255, 225) : IM_COL32(110, 170, 255, 140);
          const float thickness = obj.ui.maskChildren ? 2.4f : 1.5f;
          dl->AddRect(rectMin, rectMax, edgeColor, 6.0f, 0, thickness);
          if (obj.ui.maskChildren) {
            const float inset = 2.0f;
            if ((rectMax.x - rectMin.x) > inset * 2.0f && (rectMax.y - rectMin.y) > inset * 2.0f) {
              dl->AddRect(ImVec2(rectMin.x + inset, rectMin.y + inset), ImVec2(rectMax.x - inset, rectMax.y - inset), IM_COL32(32, 190, 230, 175), 5.0f, 0, 1.0f);
            }
          }
          if (styleApplied) ImGui::GetStyle() = savedStyle;
          if (fontApplied) ImGui::PopFont();
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
              if (fontApplied)  ImGui::PopFont();
              if (styleApplied) ImGui::GetStyle() = savedStyle;
              continue;
            }
            if (drawMax.x <= maskMin.x || drawMin.x >= maskMax.x ||
                drawMax.y <= maskMin.y || drawMin.y >= maskMax.y) {
              if (fontApplied)  ImGui::PopFont();
              if (styleApplied) ImGui::GetStyle() = savedStyle;
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
          if (light2DRenderedObjectIds.find(obj.id) != light2DRenderedObjectIds.end()) {
            if (pushedCanvasMask) ImGui::PopClipRect();
            ImGui::PopID();
            if (fontApplied)  ImGui::PopFont();
            if (styleApplied) ImGui::GetStyle() = savedStyle;
            continue;
          }
          Texture *spriteTex = spriteTextureResolver.resolveTexture(obj);
          unsigned int texId = (spriteTex != nullptr) ? spriteTex->GetID() : 0;
          std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
          bool renderedMaterialShader = false;
          if (ViewportRenderHelpers::HasUiImageMaterialShader(obj)) {
            spriteBatch.flush();
            const unsigned int shadedTexture =
                ViewportRenderHelpers::RenderUiImageMaterialShader(
                    renderer, obj, drawSize, uvQuad, texId, ImGui::GetTime());
            if (shadedTexture != 0) {
              texId = shadedTexture;
              uvQuad = {ImVec2(0.0f, 1.0f), ImVec2(1.0f, 1.0f),
                        ImVec2(1.0f, 0.0f), ImVec2(0.0f, 0.0f)};
              renderedMaterialShader = true;
            }
          }
          const int frame = resolveSpriteSheetFrame(obj);
          const ImVec2 sourceFrameSizePx = ResolveUiSourceFrameSizePx(obj, frame, spriteTex);
          ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b, obj.ui.color.a);
          const ImU32 tintColor = renderedMaterialShader
                                      ? IM_COL32_WHITE
                                      : ImGui::GetColorU32(tint);
          bool repeatX = obj.hasParallaxLayer2D && obj.parallaxLayer2D.enabled && obj.parallaxLayer2D.repeatX;
          bool repeatY = obj.hasParallaxLayer2D && obj.parallaxLayer2D.enabled && obj.parallaxLayer2D.repeatY;
          glm::vec2 spriteSizeWorld = getSpriteDisplaySize(obj);
          glm::vec2 spacing = obj.hasParallaxLayer2D  ? obj.parallaxLayer2D.repeatSpacing  : glm::vec2(0.0f);
          float stepX = spriteSizeWorld.x + spacing.x;
          float stepY = spriteSizeWorld.y + spacing.y;
          glm::vec2 baseWorldMin = worldViewMin;
          if (repeatX || repeatY) {
            glm::vec2 sizeWorld = spriteSizeWorld;
            ImVec2 pivotOffset = ImVec2(sizeWorld.x * 0.5f, sizeWorld.y * 0.5f);
            switch (obj.ui.anchor) {
            case UIAnchor::TopLeft: pivotOffset = ImVec2(0.0f, 0.0f); break;
            case UIAnchor::TopRight: pivotOffset = ImVec2(sizeWorld.x, 0.0f); break;
            case UIAnchor::BottomLeft: pivotOffset = ImVec2(0.0f, sizeWorld.y); break;
            case UIAnchor::BottomRight: pivotOffset = ImVec2(sizeWorld.x, sizeWorld.y); break;
            default: break;
            }
            glm::vec2 parentOffset = uiSceneLookup.getWorldParentOffset(obj);
            glm::vec2 worldPos = parentOffset + glm::vec2(obj.ui.position.x, obj.ui.position.y) + parallaxOffset(obj);
            baseWorldMin = worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
          }
          float angle = uiSceneLookup.getWorldParentRotationRad(obj) + glm::radians(obj.ui.rotation);
          auto drawImageRect = [&](const ImVec2 &min, const ImVec2 &max) {
            ImVec2 size(max.x - min.x, max.y - min.y);
            if (size.x <= 1.0f || size.y <= 1.0f) return;
            ImVec2 drawMinLocal(min.x, min.y);
            ImVec2 drawMaxLocal(max.x, max.y);
            if (!renderedMaterialShader &&
                DrawScrollingUiSprite(spriteBatch, (ImTextureID)(intptr_t)texId,
                                      obj, drawMinLocal, drawMaxLocal, angle, tintColor,
                                      ImGui::GetTime())) {
              return;
            }
            if (!renderedMaterialShader &&
                DrawNineSliceSprite(spriteBatch, (ImTextureID)(intptr_t)texId, obj, drawMinLocal, drawMaxLocal, uvQuad, sourceFrameSizePx, angle, tintColor)) {
              return;
            }
            if (std::abs(angle) > 1e-4f) {
              ImVec2 center((drawMinLocal.x + drawMaxLocal.x) * 0.5f, (drawMinLocal.y + drawMaxLocal.y) * 0.5f);
              ImVec2 half(size.x * 0.5f, size.y * 0.5f);
              ImVec2 p0, p1, p2, p3;
              ViewportRenderHelpers::UiRotatedCornersAboutAnchor(
                  obj.ui.anchor, center, half, angle, p0, p1, p2, p3);
              if (texId != 0) {
                spriteBatch.push((ImTextureID)(intptr_t)texId, p0, p1, p2, p3, uvQuad[0], uvQuad[1], uvQuad[2], uvQuad[3], tintColor);
              } else {
                spriteBatch.flush();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImU32 fill = tintColor;
                ImU32 border = ImGui::GetColorU32(brighten(tint, 0.85f));
                dl->AddQuadFilled(p0, p1, p2, p3, fill);
                dl->AddQuad(p0, p1, p2, p3, border, 2.0f);
                ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
                ImVec2 textPos(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);
                dl->AddText(textPos, IM_COL32(210, 210, 220, 220), obj.ui.label.c_str());
              }
            } else {
              ImDrawList *dl = ImGui::GetWindowDrawList();
              if (texId != 0) {
                spriteBatch.push(
                    (ImTextureID)(intptr_t)texId, drawMinLocal,
                    ImVec2(drawMaxLocal.x, drawMinLocal.y), drawMaxLocal,
                    ImVec2(drawMinLocal.x, drawMaxLocal.y), uvQuad[0],
                    ImVec2(uvQuad[2].x, uvQuad[0].y), uvQuad[2],
                    ImVec2(uvQuad[0].x, uvQuad[2].y), tintColor);
              } else {
                spriteBatch.flush();
                ImU32 fill = tintColor;
                ImU32 border = ImGui::GetColorU32(brighten(tint, 0.85f));
                dl->AddRectFilled(drawMinLocal, drawMaxLocal, fill, 6.0f);
                dl->AddRect(drawMinLocal, drawMaxLocal, border, 6.0f);
                ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
                ImVec2 textPos(drawMinLocal.x + (size.x - textSize.x) * 0.5f, drawMinLocal.y + (size.y - textSize.y) * 0.5f);
                dl->AddText(textPos, IM_COL32(210, 210, 220, 220), obj.ui.label.c_str());
              }
            }
          };

          if (repeatX || repeatY) {
            int startX = repeatX ? static_cast<int>(std::floor((worldViewMin.x - baseWorldMin.x) / stepX)) - 1  : 0;
            int endX = repeatX ? static_cast<int>(std::ceil((worldViewMax.x - baseWorldMin.x) / stepX)) + 1 : 0;
            int startY = repeatY ? static_cast<int>(std::floor((worldViewMin.y - baseWorldMin.y) / stepY)) - 1 : 0;
            int endY = repeatY ? static_cast<int>(std::ceil((worldViewMax.y - baseWorldMin.y) / stepY)) + 1 : 0;
            for (int ix = startX; ix <= endX; ++ix) {
              for (int iy = startY; iy <= endY; ++iy) {
                float dx = repeatX ? (float)ix * stepX : 0.0f;
                float dy = repeatY ? (float)iy * stepY : 0.0f;
                glm::vec2 tileMin = baseWorldMin + glm::vec2(dx, dy);
                ImVec2 s0 = worldToScreen(tileMin);
                ImVec2 s1 = worldToScreen(tileMin + glm::vec2(spriteSizeWorld.x, spriteSizeWorld.y));
                ImVec2 tMin(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
                ImVec2 tMax(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
                drawImageRect(tMin, tMax);
              }
            }
          } else drawImageRect(drawMin, drawMax);
        } else if (obj.ui.type == UIElementType::Slider) {
          spriteBatch.flush();
          ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b, obj.ui.color.a);
          const bool uiWidgetInteractive = isPlaying && !uiWorldCameraActive && obj.ui.interactable;
          if (uiWidgetInteractive) ImGui::SetCursorPos(localMin);
          const ImU32 s2SliderBg     = (obj.ui.backgroundColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.backgroundColor.r, obj.ui.backgroundColor.g, obj.ui.backgroundColor.b, obj.ui.backgroundColor.a)) : ImGui::GetColorU32(ImVec4(tint.x * 0.2f, tint.y * 0.2f, tint.z * 0.2f, tint.w * 0.6f));
          const ImU32 s2SliderFill   = (obj.ui.fillColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.fillColor.r, obj.ui.fillColor.g, obj.ui.fillColor.b, obj.ui.fillColor.a))             : ImGui::GetColorU32(tint);
          const ImU32 s2SliderBorder = (obj.ui.borderColor.a > 0.0f)     ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a))     : ImGui::GetColorU32(brighten(tint, 0.85f));
          const ImU32 s2SliderText   = (obj.ui.textColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a))             : IM_COL32(240, 240, 245, 220);
          if (obj.ui.sliderStyle == UISliderStyle::ImGui) {
            float minValue = obj.ui.sliderMin;
            float maxValue = obj.ui.sliderMax;
            float range = (maxValue - minValue);
            if (range <= 1e-6f) range = 1.0f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (uiWidgetInteractive) {
              ImGui::InvisibleButton("##UISliderImGui", drawSize);
              if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && drawSize.x > 1.0f) {
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
            float rounding = 6.0f;
            ImVec2 fillMax(drawMin.x + drawSize.x * t, drawMax.y);
            dl->AddRectFilled(drawMin, drawMax, s2SliderBg, rounding);
            if (fillMax.x > drawMin.x) dl->AddRectFilled(drawMin, fillMax, s2SliderFill, rounding);
            dl->AddRect(drawMin, drawMax, s2SliderBorder, rounding);
            ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
            ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f, drawMin.y + (drawSize.y - textSize.y) * 0.5f);
            dl->AddText(textPos, s2SliderText, obj.ui.label.c_str());
          } else {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImU32 bg     = s2SliderBg;
            ImU32 fill   = s2SliderFill;
            ImU32 border = s2SliderBorder;
            float minValue = obj.ui.sliderMin;
            float maxValue = obj.ui.sliderMax;
            float range = (maxValue - minValue);
            if (range <= 1e-6f) range = 1.0f;
            bool held = false;
            if (uiWidgetInteractive) {
              ImGui::InvisibleButton("##UISlider", drawSize);
              held = ImGui::IsItemActive();
            }
            if (held && ImGui::IsMouseDown(ImGuiMouseButton_Left) && drawSize.x > 1.0f) {
              float mouseT = (ImGui::GetIO().MousePos.x - drawMin.x) / drawSize.x;
              mouseT = std::clamp(mouseT, 0.0f, 1.0f);
              float newValue = minValue + mouseT * range;
              if (newValue != obj.ui.sliderValue) {
                obj.ui.sliderValue = newValue;
                projectManager.currentProject.hasUnsavedChanges = true;
              }
            }

            animateValue(animState.sliderValue, obj.ui.sliderValue, held);
            float displayValue = (uiAnimationMode == UIAnimationMode::Off) ? obj.ui.sliderValue : animState.sliderValue;
            float t = (displayValue - minValue) / range;
            t = std::clamp(t, 0.0f, 1.0f);

            ViewportRenderHelpers::RenderUISliderStyle(
                dl, obj.ui.sliderStyle, drawMin, drawMax, drawSize, bg, fill,
                border, s2SliderText, t, minValue, maxValue,
                obj.ui.label.c_str());
          }
        } else if (obj.ui.type == UIElementType::Button) {
          spriteBatch.flush();
          ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b, obj.ui.color.a);
          obj.ui.buttonPressed = false;
          obj.ui.uiHovered = false;
          obj.ui.uiActive = false;
          const bool uiWidgetInteractive = isPlaying && !uiWorldCameraActive && obj.ui.interactable;
          if (uiWidgetInteractive) ImGui::SetCursorPos(localMin);
          const ImU32 s2BtnText = (obj.ui.textColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a)) : IM_COL32(240, 240, 245, 220);
          if (obj.ui.buttonStyle == UIButtonStyle::ImGui) {
            if (uiWidgetInteractive) {
              ImGui::PushStyleColor(ImGuiCol_Button, tint);
              ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighten(tint, 1.1f));
              ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighten(tint, 1.2f));
              obj.ui.buttonPressed = ImGui::Button(obj.ui.label.c_str(), drawSize);
              obj.ui.uiHovered = ImGui::IsItemHovered();
              obj.ui.uiActive  = ImGui::IsItemActive();
              ImGui::PopStyleColor(3);
            } else {
              ImDrawList *dl = ImGui::GetWindowDrawList();
              ImU32 fill   = (obj.ui.fillColor.a > 0.0f)   ? ImGui::GetColorU32(ImVec4(obj.ui.fillColor.r, obj.ui.fillColor.g, obj.ui.fillColor.b, obj.ui.fillColor.a))     : ImGui::GetColorU32(tint);
              ImU32 border = (obj.ui.borderColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a)) : ImGui::GetColorU32(brighten(tint, 0.85f));
              dl->AddRectFilled(drawMin, drawMax, fill, 6.0f);
              dl->AddRect(drawMin, drawMax, border, 6.0f);
              ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
              ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f, drawMin.y + (drawSize.y - textSize.y) * 0.5f);
              dl->AddText(textPos, s2BtnText, obj.ui.label.c_str());
            }
          } else if (obj.ui.buttonStyle == UIButtonStyle::Outline) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImU32 border = (obj.ui.borderColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a)) : ImGui::GetColorU32(tint);
            bool hovered = false;
            bool active = false;
            if (uiWidgetInteractive) {
              if (ImGui::InvisibleButton("##UIButton", drawSize))obj.ui.buttonPressed = true;
              hovered = ImGui::IsItemHovered();
              active = ImGui::IsItemActive();
              obj.ui.uiHovered = hovered;
              obj.ui.uiActive  = active;
            }
            float hoverT = animateValue(animState.hover, hovered ? 1.0f : 0.0f, false);
            float activeT = animateValue(animState.active, active ? 1.0f : 0.0f, false);
            if (hoverT > 0.001f) {
              ImVec4 hoverCol = brighten(tint, 0.45f);
              hoverCol.w *= std::clamp(hoverT, 0.0f, 1.0f);
              dl->AddRectFilled(drawMin, drawMax, ImGui::GetColorU32(hoverCol), 6.0f);
            }
            if (activeT > 0.001f) {
              ImVec4 activeCol = brighten(tint, 0.65f);
              activeCol.w *= std::clamp(activeT, 0.0f, 1.0f);
              dl->AddRectFilled(drawMin, drawMax, ImGui::GetColorU32(activeCol), 6.0f);
            }
            dl->AddRect(drawMin, drawMax, border, 6.0f, 0, 2.0f);
            ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
            ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f, drawMin.y + (drawSize.y - textSize.y) * 0.5f);
            dl->AddText(textPos, s2BtnText, obj.ui.label.c_str());
          }
        } else if (obj.ui.type == UIElementType::Text) {
          spriteBatch.flush();
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                      obj.ui.color.a);
          ImFont *textFont =
              obj.ui.textFont.empty()
                  ? ImGui::GetFont()
                  : getUIFontForContext(obj.ui.textFont,
                                        ImGui::GetCurrentContext());
          if (!textFont) {
            textFont = ImGui::GetFont();
          }
          float scale = std::max(0.1f, obj.ui.textScale);
          const float authoredFontSize =
              ResolveViewportUITextAuthoredFontSize(
                  ImGui::GetFontSize(), scale, obj.ui.fontSize);
          float fontSize = ResolveViewportUITextFontSize(
              ImGui::GetFontSize(), scale, obj.ui.fontSize, true,
              uiWorldCamera.zoom);
          const float textRotationRad = uiSceneLookup.getWorldParentRotationRad(obj) + glm::radians(obj.ui.rotation);
          const bool textIsRotated = std::abs(textRotationRad) > 1e-4f;
          if (!textIsRotated) ImGui::PushClipRect(drawMin, drawMax, true);
          const int textVtxStart = dl->VtxBuffer.Size;
          AddUITextWithFilter(
              dl, obj.material.textureFilter, textFont, fontSize,
              drawMin, drawMax, ImGui::GetColorU32(tint),
              obj.ui.label.c_str(), obj.ui.textAutoWrap, obj.ui.textHAlign,
              obj.ui.textVAlign, obj.ui.textEffectFlags, obj.ui.textEffectSpeed,
              obj.ui.textEffectIntensity, authoredFontSize);
          if (textIsRotated) {
            const ImVec2 textPivotOffset = ViewportRenderHelpers::UiAnchorPivot(
                obj.ui.anchor, ImVec2(drawMax.x - drawMin.x, drawMax.y - drawMin.y));
            const ImVec2 textPivot(drawMin.x + textPivotOffset.x,
                                   drawMin.y + textPivotOffset.y);
            ViewportRenderHelpers::RotateDrawListVertices(
                dl, textVtxStart, dl->VtxBuffer.Size, textPivot,
                textRotationRad);
          } else ImGui::PopClipRect();
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
      if (worldUiEditing) {
        light2DCompositorRanLastFrame = renderedLight2DComposite;
        light2DLightBufferHadContentLastFrame = lightBufferHadContent;
        light2DActiveCountLastFrame = activeLight2DCount;
        light2DLitSprite2DCountLastFrame = litSprite2DCount;
        light2DLitWorldImageCountLastFrame = litWorldImageCount;
        if (captureLight2DRoutingReasons) {
          light2DObjectRoutingReasonsLastFrame =
              std::move(light2DRoutingReasons);
        }
      }
      if (renderedLight2DComposite && showLight2DStatsOverlay) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        char lightStatsLabel[160];
        std::snprintf(lightStatsLabel, sizeof(lightStatsLabel),
                      "2D Lights %d  Sprites %d  Cost %.2f ms",
                      light2DStats.visibleLights, light2DStats.visibleSprites,
                      light2DStats.cpuBuildMs);
        dl->AddText(ImVec2(overlayPos.x + 12.0f, overlayPos.y + 10.0f),
                    IM_COL32(255, 232, 170, 235), lightStatsLabel);
      }

      bool light2DHandleUsed = false;
      if (showSceneGizmos) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (const SceneObject &target : sceneObjects) {
          if (!IsObjectEnabledInHierarchy(target)) {
            continue;
          }

          if (target.hasLight2D && target.light2D.enabled) {
            const glm::vec2 lightWorld(target.position.x, target.position.y);
            const ImVec2 center = worldToScreen(lightWorld);
            const float outerRadiusPx =
                std::max(target.light2D.radius, target.light2D.outerRadius) *
                uiWorldCamera.zoom;
            const float innerRadiusPx =
                target.light2D.innerRadius * uiWorldCamera.zoom;
            const bool selectedLight =
                (selectedObjectId == target.id) ||
                (std::find(selectedObjectIds.begin(), selectedObjectIds.end(),
                           target.id) != selectedObjectIds.end());

            if (gizmoShowLight2DBounds &&
                (target.light2D.type == Light2DType::Point ||
                 target.light2D.type == Light2DType::Spot)) {
              const ImU32 outerColor = selectedLight
                                           ? IM_COL32(255, 238, 120, 255)
                                           : IM_COL32(214, 208, 84, 180);
              dl->AddCircle(center, std::max(2.0f, outerRadiusPx), outerColor,
                            64, 1.5f);
              if (innerRadiusPx > 1.0f) {
                dl->AddCircle(center, innerRadiusPx,
                              IM_COL32(255, 255, 255, 85), 48, 1.0f);
              }
              if (target.light2D.type == Light2DType::Spot) {
                const float dir = glm::radians(target.rotation.z);
                const float outerHalf =
                    glm::radians(target.light2D.outerSpotAngle) * 0.5f;
                const glm::vec2 rayA(std::cos(dir - outerHalf),
                                     std::sin(dir - outerHalf));
                const glm::vec2 rayB(std::cos(dir + outerHalf),
                                     std::sin(dir + outerHalf));
                dl->AddLine(center,
                            ImVec2(center.x + rayA.x * outerRadiusPx,
                                   center.y - rayA.y * outerRadiusPx),
                            outerColor, 1.5f);
                dl->AddLine(center,
                            ImVec2(center.x + rayB.x * outerRadiusPx,
                                   center.y - rayB.y * outerRadiusPx),
                            outerColor, 1.5f);
              }
            }

            if (gizmoShowLight2DShapes &&
                (target.light2D.type == Light2DType::Freeform ||
                 target.light2D.type == Light2DType::Sprite) &&
                target.light2D.shapePoints.size() >= 2) {
              const ImU32 edgeColor = selectedLight
                                          ? IM_COL32(255, 244, 150, 255)
                                          : IM_COL32(248, 198, 96, 190);
              std::vector<ImVec2> screenPoints;
              screenPoints.reserve(target.light2D.shapePoints.size());
              for (const glm::vec2 &point : target.light2D.shapePoints) {
                screenPoints.push_back(worldToScreen(lightWorld + point));
              }
              for (size_t i = 0; i < screenPoints.size(); ++i) {
                const ImVec2 a = screenPoints[i];
                const ImVec2 b = screenPoints[(i + 1) % screenPoints.size()];
                dl->AddLine(a, b, edgeColor, 1.8f);
              }
            }
          }

          if (target.hasAudioSource && target.audioSource.enabled &&
              !target.audioSource.clipPath.empty()) {
            const glm::vec2 audioWorld(target.position.x, target.position.y);
            const ImVec2 center = worldToScreen(audioWorld);
            const float spatialBlend = std::clamp(
                GetAudioSpatialBlend(target.audioSource), 0.0f, 1.0f);
            const float nearRadiusPx = std::max(
                2.0f, target.audioSource.minDistance * uiWorldCamera.zoom);
            const float farRadiusPx =
                std::max(nearRadiusPx + 1.0f,
                         target.audioSource.maxDistance * uiWorldCamera.zoom);
            const bool selectedAudio =
                (selectedObjectId == target.id) ||
                (std::find(selectedObjectIds.begin(), selectedObjectIds.end(),
                           target.id) != selectedObjectIds.end());
            const float alphaScale = 0.25f + spatialBlend * 0.75f;
            const ImU32 outerColor =
                selectedAudio ? ImGui::GetColorU32(ImVec4(0.56f, 0.86f, 1.00f,
                                                          0.95f * alphaScale))
                              : ImGui::GetColorU32(ImVec4(0.38f, 0.70f, 1.00f,
                                                          0.72f * alphaScale));
            const ImU32 innerColor =
                selectedAudio ? ImGui::GetColorU32(ImVec4(0.92f, 0.97f, 1.00f,
                                                          0.70f * alphaScale))
                              : ImGui::GetColorU32(ImVec4(0.86f, 0.94f, 1.00f,
                                                          0.42f * alphaScale));
            const ImU32 markerColor =
                selectedAudio
                    ? ImGui::GetColorU32(ImVec4(0.94f, 0.98f, 1.00f, 1.0f))
                    : ImGui::GetColorU32(ImVec4(0.62f, 0.82f, 1.00f, 0.92f));

            if (spatialBlend > 0.001f) {
              dl->AddCircle(center, farRadiusPx, outerColor, 72, 1.5f);
              if (nearRadiusPx > 1.0f) {
                dl->AddCircle(center, nearRadiusPx, innerColor, 48, 1.0f);
              }
            }

            dl->AddCircleFilled(center, selectedAudio ? 5.0f : 4.0f,
                                markerColor, 18);
            dl->AddLine(ImVec2(center.x - 8.0f, center.y),
                        ImVec2(center.x - 2.0f, center.y), markerColor,
                        selectedAudio ? 2.0f : 1.5f);
            dl->AddLine(ImVec2(center.x + 2.0f, center.y),
                        ImVec2(center.x + 8.0f, center.y), markerColor,
                        selectedAudio ? 2.0f : 1.5f);
            dl->AddLine(ImVec2(center.x, center.y - 8.0f),
                        ImVec2(center.x, center.y - 2.0f), markerColor,
                        selectedAudio ? 2.0f : 1.5f);
            dl->AddLine(ImVec2(center.x, center.y + 2.0f),
                        ImVec2(center.x, center.y + 8.0f), markerColor,
                        selectedAudio ? 2.0f : 1.5f);

            if (selectedAudio) {
              const std::string clipLabel =
                  fs::path(target.audioSource.clipPath).stem().string();
              const std::string label =
                  clipLabel.empty() ? "Audio Source" : clipLabel;
              dl->AddText(ImVec2(center.x + 10.0f, center.y - 20.0f),
                          IM_COL32(214, 240, 255, 245), label.c_str());
            }
          }

          if (gizmoShowShadowCaster2DBounds && target.hasShadowCaster2D &&
              target.shadowCaster2D.enabled &&
              target.shadowCaster2D.points.size() >= 2) {
            const glm::vec2 casterWorld(target.position.x, target.position.y);
            const bool selectedCaster =
                (selectedObjectId == target.id) ||
                (std::find(selectedObjectIds.begin(), selectedObjectIds.end(),
                           target.id) != selectedObjectIds.end());
            const ImU32 edgeColor = selectedCaster
                                        ? IM_COL32(120, 220, 255, 255)
                                        : IM_COL32(88, 160, 220, 190);
            std::vector<ImVec2> screenPoints;
            screenPoints.reserve(target.shadowCaster2D.points.size());
            for (const glm::vec2 &point : target.shadowCaster2D.points) {
              screenPoints.push_back(worldToScreen(casterWorld + point));
            }
            for (size_t i = 0; i < screenPoints.size(); ++i) {
              const ImVec2 a = screenPoints[i];
              const ImVec2 b = screenPoints[(i + 1) % screenPoints.size()];
              dl->AddLine(a, b, edgeColor, 1.6f);
            }
          }
        }
      }

      if (worldUiEditing && light2DShapeEditMode &&
          light2DShapeEditingObjectId >= 0) {
        SceneObject *editObject = findObjectById(light2DShapeEditingObjectId);
        if (editObject && IsObjectEnabledInHierarchy(*editObject)) {
          std::vector<glm::vec2> *editPoints = nullptr;
          bool editingLight = false;
          if (editObject->hasLight2D &&
              (editObject->light2D.type == Light2DType::Freeform ||
               editObject->light2D.type == Light2DType::Sprite)) {
            editPoints = &editObject->light2D.shapePoints;
            editingLight = true;
          } else if (editObject->hasShadowCaster2D) {
            editPoints = &editObject->shadowCaster2D.points;
          }

          if (editPoints && editPoints->size() >= 2) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const glm::vec2 origin(editObject->position.x,
                                   editObject->position.y);
            std::vector<ImVec2> handlePoints;
            handlePoints.reserve(editPoints->size());
            for (const glm::vec2 &point : *editPoints) {
              handlePoints.push_back(worldToScreen(origin + point));
            }

            for (size_t i = 0; i < handlePoints.size(); ++i) {
              dl->AddLine(handlePoints[i],
                          handlePoints[(i + 1) % handlePoints.size()],
                          editingLight ? IM_COL32(255, 248, 170, 255)
                                       : IM_COL32(128, 232, 255, 255),
                          2.0f);
            }

            int hoveredPoint = -1;
            float hoveredDistanceSq = 81.0f;
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            for (size_t i = 0; i < handlePoints.size(); ++i) {
              const float dx = mouse.x - handlePoints[i].x;
              const float dy = mouse.y - handlePoints[i].y;
              const float distanceSq = dx * dx + dy * dy;
              if (distanceSq < hoveredDistanceSq) {
                hoveredDistanceSq = distanceSq;
                hoveredPoint = static_cast<int>(i);
              }
            }

            for (size_t i = 0; i < handlePoints.size(); ++i) {
              const bool activePoint =
                  (light2DShapeEditingPointIndex == static_cast<int>(i));
              const bool hotPoint = (hoveredPoint == static_cast<int>(i));
              const ImU32 pointColor =
                  activePoint ? IM_COL32(255, 214, 86, 255)
                              : (hotPoint ? IM_COL32(255, 255, 255, 255)
                                          : IM_COL32(235, 235, 235, 220));
              dl->AddCircleFilled(handlePoints[i], activePoint ? 6.5f : 5.0f,
                                  pointColor, 18);
              dl->AddCircle(handlePoints[i], activePoint ? 6.5f : 5.0f,
                            IM_COL32(20, 20, 20, 220), 18, 1.0f);
            }

            if (uiWorldHover && !uiWorldCameraActive && !ImGuizmo::IsUsing() &&
                !ImGuizmo::IsOver()) {
              if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                light2DHandleUsed = (hoveredPoint >= 0);
                if (hoveredPoint >= 0) {
                  if (ImGui::GetIO().KeyAlt && editPoints->size() > 3) {
                    editPoints->erase(editPoints->begin() + hoveredPoint);
                    light2DShapeEditingPointIndex = -1;
                    if (editingLight) {
                      lighting2DRenderer.clearPolygonCache(editObject->id);
                    }
                    projectManager.currentProject.hasUnsavedChanges = true;
                  } else {
                    light2DShapeEditingPointIndex = hoveredPoint;
                  }
                } else if (ImGui::GetIO().KeyCtrl && editPoints->size() >= 2) {
                  int insertAfter = -1;
                  float bestSegmentDistanceSq = 144.0f;
                  for (size_t i = 0; i < handlePoints.size(); ++i) {
                    const ImVec2 a = handlePoints[i];
                    const ImVec2 b =
                        handlePoints[(i + 1) % handlePoints.size()];
                    const ImVec2 ab(b.x - a.x, b.y - a.y);
                    const float denom = ab.x * ab.x + ab.y * ab.y;
                    if (denom <= 0.0001f) {
                      continue;
                    }
                    const float t = std::clamp(
                        ((mouse.x - a.x) * ab.x + (mouse.y - a.y) * ab.y) /
                            denom,
                        0.0f, 1.0f);
                    const ImVec2 closest(a.x + ab.x * t, a.y + ab.y * t);
                    const float dx = mouse.x - closest.x;
                    const float dy = mouse.y - closest.y;
                    const float distanceSq = dx * dx + dy * dy;
                    if (distanceSq < bestSegmentDistanceSq) {
                      bestSegmentDistanceSq = distanceSq;
                      insertAfter = static_cast<int>(i);
                    }
                  }
                  if (insertAfter >= 0) {
                    const glm::vec2 worldMouse = screenToWorld(mouse);
                    editPoints->insert(editPoints->begin() + insertAfter + 1,
                                       worldMouse - origin);
                    light2DShapeEditingPointIndex = insertAfter + 1;
                    if (editingLight) {
                      lighting2DRenderer.clearPolygonCache(editObject->id);
                    }
                    projectManager.currentProject.hasUnsavedChanges = true;
                    light2DHandleUsed = true;
                  }
                }
              }

              if (ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                  light2DShapeEditingPointIndex >= 0 &&
                  light2DShapeEditingPointIndex <
                      static_cast<int>(editPoints->size())) {
                const glm::vec2 worldMouse = screenToWorld(mouse);
                (*editPoints)[static_cast<size_t>(
                    light2DShapeEditingPointIndex)] = worldMouse - origin;
                if (editingLight) {
                  lighting2DRenderer.clearPolygonCache(editObject->id);
                }
                projectManager.currentProject.hasUnsavedChanges = true;
                light2DHandleUsed = true;
              }
              if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                light2DShapeEditingPointIndex = -1;
              }
            }
          }
        } else {
          light2DShapeEditMode = false;
          light2DShapeEditingObjectId = -1;
          light2DShapeEditingPointIndex = -1;
        }
      }

      auto drawCollider2DWorldOutline = [&](const SceneObject &target,
                                            ImU32 outlineColor,
                                            float thickness) {
        if (!IsObjectEnabledInHierarchy(target))
          return;
        if (!(target.hasCollider2D && target.collider2D.enabled))
          return;
        if (!isUIType(target) || target.ui.type == UIElementType::Canvas)
          return;
        std::vector<glm::vec2> localPoints;
        if (target.collider2D.type == Collider2DType::Box) {
          glm::vec2 half = target.collider2D.boxSize * 0.5f;
          localPoints = {glm::vec2(-half.x, -half.y) + target.collider2D.offset,
                         glm::vec2(half.x, -half.y) + target.collider2D.offset,
                         glm::vec2(half.x, half.y) + target.collider2D.offset,
                         glm::vec2(-half.x, half.y) + target.collider2D.offset};
        } else {
          localPoints = target.collider2D.points;
          if (localPoints.empty() &&
              target.collider2D.type == Collider2DType::Edge) {
            float half = target.collider2D.boxSize.x * 0.5f;
            localPoints = {glm::vec2(-half, 0.0f), glm::vec2(half, 0.0f)};
          }
          for (glm::vec2 &point : localPoints) {
            point += target.collider2D.offset;
          }
        }

        if (localPoints.size() < 2)
          return;

        glm::vec2 parentOffset = uiSceneLookup.getWorldParentOffset(target);
        float parentRot = uiSceneLookup.getWorldParentRotationRad(target);
        glm::vec2 pivotWorld =
            parentOffset +
            ViewportRenderHelpers::RotateUiVec2(
                glm::vec2(target.ui.position.x, target.ui.position.y),
                parentRot) +
            parallaxOffset(target);
        float angle = parentRot + glm::radians(target.ui.rotation);
        float c = std::cos(angle);
        float s = std::sin(angle);
        auto rotatePoint2D = [c, s](const glm::vec2 &p) {
          return glm::vec2(p.x * c - p.y * s, p.x * s + p.y * c);
        };

        std::vector<ImVec2> screenPoints;
        screenPoints.reserve(localPoints.size());
        for (const glm::vec2 &point : localPoints) {
          screenPoints.push_back(
              worldToScreen(pivotWorld + rotatePoint2D(point)));
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (size_t i = 1; i < screenPoints.size(); ++i) {
          dl->AddLine(screenPoints[i - 1], screenPoints[i], outlineColor,
                      thickness);
        }
        if (target.collider2D.type == Collider2DType::Box ||
            target.collider2D.type == Collider2DType::Polygon ||
            target.collider2D.closed) {
          dl->AddLine(screenPoints.back(), screenPoints.front(), outlineColor,
                      thickness);
        }
      };
      auto drawSelectedCollider2DWorldOutlines = [&]() {
        std::vector<int> roots = selectedObjectIds;
        if (roots.empty() && selectedObjectId >= 0) {
          roots.push_back(selectedObjectId);
        }
        if (roots.empty()) {
          return;
        }

        std::unordered_set<int> rootSet(roots.begin(), roots.end());
        std::unordered_set<int> visited;
        std::vector<int> stack = roots;
        while (!stack.empty()) {
          const int id = stack.back();
          stack.pop_back();
          if (!visited.insert(id).second) {
            continue;
          }

          SceneObject *node = findObjectById(id);
          if (!node || !IsObjectEnabledInHierarchy(*node)) {
            continue;
          }

          if (node->hasCollider2D && node->collider2D.enabled) {
            const bool isDirectSelected = rootSet.find(id) != rootSet.end();
            const ImU32 color =
                isDirectSelected
                    ? ImGui::GetColorU32(ImVec4(0.24f, 0.95f, 1.0f, 0.95f))
                    : ImGui::GetColorU32(ImVec4(0.38f, 1.0f, 0.78f, 0.85f));
            const float thickness = isDirectSelected ? 2.2f : 1.8f;
            drawCollider2DWorldOutline(*node, color, thickness);
          }

          for (int childId : node->childIds) {
            if (childId >= 0) {
              stack.push_back(childId);
            }
          }
        }
      };
      drawSelectedCollider2DWorldOutlines();

      // Asset drag over the 2D world: the topmost sprite under the cursor is the drop target
      // (the same order click selection uses), glowing in the 3D selection outline's colour.
      if (viewportDragKind != ObjectAssetDropKind::None) {
        int dropHit = -1;
        if (uiWorldHover) {
          const ImVec2 mouse = ImGui::GetIO().MousePos;
          for (auto it = drawnUiObjects.rbegin(); it != drawnUiObjects.rend(); ++it) {
            const SceneObject &candidate = *(*it);
            if (candidate.ui.type == UIElementType::Canvas) continue;
            if (!canApplyAssetDrop(candidate, viewportDragKind)) continue;
            auto rectIt = resolvedUiRects.find(candidate.id);
            if (rectIt == resolvedUiRects.end()) continue;
            const ImVec2 &rectMin = rectIt->second.min;
            const ImVec2 &rectMax = rectIt->second.max;
            if (mouse.x >= rectMin.x && mouse.x <= rectMax.x &&
                mouse.y >= rectMin.y && mouse.y <= rectMax.y) {
              dropHit = candidate.id;
              break;
            }
          }
        }
        setAssetDropHover(dropHit);
        auto glowIt = dropHit >= 0 ? resolvedUiRects.find(dropHit) : resolvedUiRects.end();
        if (glowIt != resolvedUiRects.end()) {
          ImDrawList *glowList = ImGui::GetWindowDrawList();
          const ImVec2 &glowMin = glowIt->second.min;
          const ImVec2 &glowMax = glowIt->second.max;
          for (int ring = 3; ring >= 1; --ring) {
            const float pad = 2.0f * static_cast<float>(ring);
            glowList->AddRect(ImVec2(glowMin.x - pad, glowMin.y - pad), ImVec2(glowMax.x + pad, glowMax.y + pad),
                              IM_COL32(122, 110, 255, 40 * (4 - ring)), 3.0f + pad, 0, 2.0f);
          }
          glowList->AddRect(glowMin, glowMax, IM_COL32(122, 110, 255, 255), 2.0f, 0, 2.0f);
        }
      }

      const bool assemblageEditing =
          worldUiEditing && world2DEditMode == World2DEditMode::Assemblage;
#if !MODULARITY_RUNTIME_ONLY
      if (assemblageEditing) {
        renderAssemblageViewportOverlay(overlayPos, overlaySize,
                                        uiWorldHover && !blockSelection);
      }
#endif

      bool gizmoUsed = false;
      bool mouseBlockedBySelectedGizmo = false;
      if (assemblageEditing) {
        blockSelection = true;
      }
      if (worldUiEditing && !assemblageEditing && uiWorldHover) {
        SceneObject *selectedForGizmoHit = getSelectedObject();
        if (selectedForGizmoHit && isUIType(*selectedForGizmoHit)) {
          std::vector<int> selectedIdsForBounds;
          if (!selectedObjectIds.empty()) {
            selectedIdsForBounds = selectedObjectIds;
          } else if (selectedObjectId >= 0) {
            selectedIdsForBounds.push_back(selectedObjectId);
          } else {
            selectedIdsForBounds.push_back(selectedForGizmoHit->id);
          }

          ImVec2 selectedBoundsMin(FLT_MAX, FLT_MAX);
          ImVec2 selectedBoundsMax(-FLT_MAX, -FLT_MAX);
          for (int id : selectedIdsForBounds) {
            SceneObject *target = findObjectById(id);
            if (!target || !IsObjectEnabledInHierarchy(*target) ||
                !isUIType(*target))
              continue;
            ImVec2 targetMin, targetMax;
            auto rectIt = resolvedUiRects.find(id);
            if (rectIt != resolvedUiRects.end()) {
              targetMin = rectIt->second.min;
              targetMax = rectIt->second.max;
            } else if (!resolveUIRectWorld(*target, targetMin, targetMax)) {
              continue;
            }
            selectedBoundsMin.x = std::min(selectedBoundsMin.x, targetMin.x);
            selectedBoundsMin.y = std::min(selectedBoundsMin.y, targetMin.y);
            selectedBoundsMax.x = std::max(selectedBoundsMax.x, targetMax.x);
            selectedBoundsMax.y = std::max(selectedBoundsMax.y, targetMax.y);
          }

          if (selectedBoundsMin.x != FLT_MAX &&
              selectedBoundsMin.y != FLT_MAX) {
            const float gizmoHitPadding = 18.0f;
            ImVec2 mouse = ImGui::GetIO().MousePos;
            mouseBlockedBySelectedGizmo =
                mouse.x >= selectedBoundsMin.x - gizmoHitPadding &&
                mouse.x <= selectedBoundsMax.x + gizmoHitPadding &&
                mouse.y >= selectedBoundsMin.y - gizmoHitPadding &&
                mouse.y <= selectedBoundsMax.y + gizmoHitPadding;
          }
        }
      }
      if (worldUiEditing && uiWorldHover && !uiWorldCameraActive &&
          !light2DHandleUsed && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
          !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
          !mouseBlockedBySelectedGizmo) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
        int hitId = -1;
        for (auto it = drawnUiObjects.rbegin(); it != drawnUiObjects.rend();
             ++it) {
          const SceneObject &obj = *(*it);
          if (obj.ui.type == UIElementType::Canvas)
            continue;
          auto rectIt = resolvedUiRects.find(obj.id);
          if (rectIt == resolvedUiRects.end())
            continue;
          const ImVec2 &rectMin = rectIt->second.min;
          const ImVec2 &rectMax = rectIt->second.max;
          if (mouse.x >= rectMin.x && mouse.x <= rectMax.x &&
              mouse.y >= rectMin.y && mouse.y <= rectMax.y) {
            hitId = obj.id;
            break;
          }
        }
        if (hitId >= 0) {
          setPrimarySelection(hitId, additive);
          gizmoUsed = true;
        } else if (!additive) {
          clearSelection();
        }
      }

      SceneObject *selected = getSelectedObject();
      if (worldUiEditing && selected && isUIType(*selected) &&
          !selected->IsInvariable) {
        auto anchorToPivotUI = [](UIAnchor anchor, const ImVec2 &size) {
          switch (anchor) {
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
        auto resolveUiRectCached = [&](SceneObject &target, ImVec2 &outMin,
                                       ImVec2 &outMax) -> bool {
          auto itRect = resolvedUiRects.find(target.id);
          if (itRect != resolvedUiRects.end()) {
            outMin = itRect->second.min;
            outMax = itRect->second.max;
            return true;
          }
          return resolveUIRectWorld(target, outMin, outMax);
        };

        std::vector<int> candidateIds;
        if (!selectedObjectIds.empty()) {
          candidateIds = selectedObjectIds;
        } else if (selectedObjectId >= 0) {
          candidateIds.push_back(selectedObjectId);
        }
        if (candidateIds.empty()) {
          candidateIds.push_back(selected->id);
        }

        std::vector<int> gizmoTargets;
        gizmoTargets.reserve(candidateIds.size());
        for (int id : candidateIds) {
          SceneObject *candidate = findObjectById(id);
          if (!candidate || !IsObjectEnabledInHierarchy(*candidate) ||
              candidate->IsInvariable)
            continue;
          if (!isUIType(*candidate))
            continue;
          ImVec2 candidateMin, candidateMax;
          if (!resolveUiRectCached(*candidate, candidateMin, candidateMax))
            continue;
          gizmoTargets.push_back(id);
        }
        if (gizmoTargets.empty()) {
          gizmoTargets.push_back(selected->id);
        }

        std::unordered_set<int> selectedSet(gizmoTargets.begin(),
                                            gizmoTargets.end());
        auto hasSelectedAncestor = [&](int id) {
          SceneObject *current = findObjectById(id);
          int parentId = current ? current->parentId : -1;
          while (parentId != -1) {
            if (selectedSet.count(parentId) > 0) {
              return true;
            }
            SceneObject *parent = findObjectById(parentId);
            parentId = parent ? parent->parentId : -1;
          }
          return false;
        };

        std::vector<int> gizmoRoots;
        gizmoRoots.reserve(gizmoTargets.size());
        for (int id : gizmoTargets) {
          if (!hasSelectedAncestor(id)) {
            gizmoRoots.push_back(id);
          }
        }
        if (gizmoRoots.empty()) {
          gizmoRoots = gizmoTargets;
        }

        ImVec2 boundsMin(FLT_MAX, FLT_MAX);
        ImVec2 boundsMax(-FLT_MAX, -FLT_MAX);
        for (int id : gizmoRoots) {
          SceneObject *obj = findObjectById(id);
          if (!obj)
            continue;
          ImVec2 targetMin, targetMax;
          if (!resolveUiRectCached(*obj, targetMin, targetMax))
            continue;
          boundsMin.x = std::min(boundsMin.x, targetMin.x);
          boundsMin.y = std::min(boundsMin.y, targetMin.y);
          boundsMax.x = std::max(boundsMax.x, targetMax.x);
          boundsMax.y = std::max(boundsMax.y, targetMax.y);
        }
        if (boundsMin.x == FLT_MAX || boundsMin.y == FLT_MAX) {
          ImVec2 fallbackMin, fallbackMax;
          if (resolveUiRectCached(*selected, fallbackMin, fallbackMax)) {
            boundsMin = fallbackMin;
            boundsMax = fallbackMax;
          }
        }

        ImVec2 rectSize(boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y);
        if (rectSize.x > 1.0f && rectSize.y > 1.0f) {
          ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
          if (mCurrentGizmoOperation == ImGuizmo::SCALE) {
            op = ImGuizmo::SCALE;
          } else if (mCurrentGizmoOperation == ImGuizmo::BOUNDS) {
            op = ImGuizmo::BOUNDS;
          } else if (mCurrentGizmoOperation == ImGuizmo::ROTATE) {
            op = ImGuizmo::ROTATE;
          }
          glm::mat4 view(1.0f);
          glm::mat4 proj =
              glm::ortho(0.0f, (float)(imageMax.x - imageMin.x),
                         (float)(imageMax.y - imageMin.y), 0.0f, -1.0f, 1.0f);
          ImVec2 rectCenter((boundsMin.x + boundsMax.x) * 0.5f - imageMin.x,
                            (boundsMin.y + boundsMax.y) * 0.5f - imageMin.y);
          glm::vec3 gizmoScale(1.0f, 1.0f, 1.0f);
          if (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS) {
            gizmoScale = glm::vec3(rectSize.x, rectSize.y, 1.0f);
          }
          glm::mat4 model(1.0f);
          model = glm::translate(model,
                                 glm::vec3(rectCenter.x, rectCenter.y, 0.0f));
          model = glm::rotate(model,
                              uiSceneLookup.getWorldParentRotationRad(*selected) +
                                  glm::radians(selected->ui.rotation),
                              glm::vec3(0.0f, 0.0f, 1.0f));
          model = glm::scale(model, gizmoScale);
          const bool stableRectScale =
              (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS);
          if (stableRectScale && worldUiGizmoHistoryCaptured &&
              worldUiRectGizmoOperation == op &&
              !worldUiRectGizmoSnapshots.empty()) {
            model = worldUiRectGizmoModel;
          }
          const glm::mat4 originalModel = model;

          ImGuizmo::BeginFrame();
          ImGuizmo::Enable(true);
          ImGuizmo::SetOrthographic(true);
          ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
          ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                            imageMax.y - imageMin.y);
          glm::mat4 delta(1.0f);
          float bounds[6] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f};
          const float *boundsPtr = (op == ImGuizmo::BOUNDS) ? bounds : nullptr;
          ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op,
                               ImGuizmo::LOCAL, glm::value_ptr(model),
                               glm::value_ptr(delta), nullptr, boundsPtr,
                               nullptr);
          if (ImGuizmo::IsUsing()) {
            if (!worldUiGizmoHistoryCaptured) {
              recordState("worldUiGizmo");
              worldUiRectGizmoOperation = op;
              worldUiRectGizmoModel = originalModel;
              worldUiRectGizmoStartMouse = ImGui::GetIO().MousePos;
              worldUiRectGizmoSnapshots.clear();
              for (int id : gizmoRoots) {
                SceneObject *target = findObjectById(id);
                if (!target)
                  continue;
                ImVec2 targetMin, targetMax;
                if (!resolveUiRectCached(*target, targetMin, targetMax))
                  continue;
                worldUiRectGizmoSnapshots.push_back(UiRectGizmoSnapshot{
                    id, target->ui.position, target->ui.size,
                    target->ui.rotation, targetMin, targetMax,
                    target->ui.textScale, target->ui.fontSize});
              }
              worldUiGizmoHistoryCaptured = true;
            }
            const float scaleDragDx =
                ImGui::GetIO().MousePos.x - worldUiRectGizmoStartMouse.x;
            const float scaleDragDy =
                ImGui::GetIO().MousePos.y - worldUiRectGizmoStartMouse.y;
            const bool allowScaleApply =
                !stableRectScale || ((scaleDragDx * scaleDragDx +
                                      scaleDragDy * scaleDragDy) >= 9.0f);
            const bool applyPixelSnap = pixelGridSnapEnabled;
            const float pixelStep =
                static_cast<float>(std::max(1, pixelGridSnapStep));
            auto snapScreenToPixel = [&](ImVec2 p) {
              p.x = imageMin.x +
                    std::round((p.x - imageMin.x) / pixelStep) * pixelStep;
              p.y = imageMin.y +
                    std::round((p.y - imageMin.y) / pixelStep) * pixelStep;
              return p;
            };
            auto findRectSnapshot = [&](int id) -> const UiRectGizmoSnapshot * {
              for (const UiRectGizmoSnapshot &snapshot :
                   worldUiRectGizmoSnapshots) {
                if (snapshot.objectId == id)
                  return &snapshot;
              }
              return nullptr;
            };
            auto extractRectFromModel = [](const glm::mat4 &rectModel,
                                           ImVec2 &outCenter, ImVec2 &outSize) {
              const glm::vec4 corners[4] = {glm::vec4(-0.5f, -0.5f, 0.0f, 1.0f),
                                            glm::vec4(0.5f, -0.5f, 0.0f, 1.0f),
                                            glm::vec4(0.5f, 0.5f, 0.0f, 1.0f),
                                            glm::vec4(-0.5f, 0.5f, 0.0f, 1.0f)};
              ImVec2 pts[4];
              for (int i = 0; i < 4; ++i) {
                const glm::vec4 p = rectModel * corners[i];
                pts[i] = ImVec2(p.x, p.y);
              }
              outCenter =
                  ImVec2((pts[0].x + pts[1].x + pts[2].x + pts[3].x) * 0.25f,
                         (pts[0].y + pts[1].y + pts[2].y + pts[3].y) * 0.25f);
              auto distance = [](const ImVec2 &a, const ImVec2 &b) {
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                return std::sqrt(dx * dx + dy * dy);
              };
              outSize = ImVec2(std::max(0.01f, distance(pts[0], pts[1])),
                               std::max(0.01f, distance(pts[0], pts[3])));
            };

            const glm::mat4 gizmoDelta =
                model * glm::inverse(stableRectScale ? worldUiRectGizmoModel
                                                     : originalModel);
            const bool groupRotate =
                (op == ImGuizmo::ROTATE && gizmoRoots.size() > 1);

            for (int id : gizmoRoots) {
              SceneObject *target = findObjectById(id);
              if (!target)
                continue;

              ImVec2 targetMin, targetMax;
              float targetRotation = target->ui.rotation;
              if (stableRectScale) {
                const UiRectGizmoSnapshot *snapshot = findRectSnapshot(id);
                if (!snapshot)
                  continue;
                targetMin = snapshot->rectMin;
                targetMax = snapshot->rectMax;
                targetRotation = snapshot->rotation;
              } else {
                if (!resolveUiRectCached(*target, targetMin, targetMax))
                  continue;
              }
              ImVec2 targetSize(targetMax.x - targetMin.x,
                                targetMax.y - targetMin.y);
              if (targetSize.x <= 0.01f || targetSize.y <= 0.01f)
                continue;
              ImVec2 targetCenter(
                  (targetMin.x + targetMax.x) * 0.5f - imageMin.x,
                  (targetMin.y + targetMax.y) * 0.5f - imageMin.y);

              const float targetParentRot =
                  uiSceneLookup.getWorldParentRotationRad(*target);
              glm::mat4 targetModel(1.0f);
              targetModel = glm::translate(
                  targetModel, glm::vec3(targetCenter.x, targetCenter.y, 0.0f));
              targetModel = glm::rotate(
                  targetModel, targetParentRot + glm::radians(targetRotation),
                  glm::vec3(0.0f, 0.0f, 1.0f));
              targetModel = glm::scale(
                  targetModel, glm::vec3(targetSize.x, targetSize.y, 1.0f));

              glm::mat4 targetNewModel = gizmoDelta * targetModel;
              glm::vec3 pos, rot, scl;
              DecomposeMatrix(targetNewModel, pos, rot, scl);
              glm::vec3 euler = NormalizeEulerDegrees(glm::degrees(rot));
              ImVec2 targetNewCenter(imageMin.x + pos.x, imageMin.y + pos.y);
              ImVec2 targetNewScreenSize(targetSize.x, targetSize.y);
              if (stableRectScale) {
                extractRectFromModel(targetNewModel, targetNewCenter,
                                     targetNewScreenSize);
                targetNewCenter.x += imageMin.x;
                targetNewCenter.y += imageMin.y;
              }
              if (applyPixelSnap && op == ImGuizmo::TRANSLATE) {
                targetNewCenter = snapScreenToPixel(targetNewCenter);
              }

              glm::vec2 parentOffset =
                  uiSceneLookup.getWorldParentOffset(*target);
              glm::vec2 worldCenter = screenToWorld(targetNewCenter);

              if (op == ImGuizmo::ROTATE) {
                target->ui.rotation =
                    euler.z - glm::degrees(targetParentRot);
                if (groupRotate) {
                  glm::vec2 worldSize = getSpriteDisplaySize(*target);
                  ImVec2 pivotOffset = anchorToPivotUI(
                      target->ui.anchor, ImVec2(worldSize.x, worldSize.y));
                  glm::vec2 worldMin = worldCenter - worldSize * 0.5f;
                  glm::vec2 worldPivot =
                      worldMin + glm::vec2(pivotOffset.x, pivotOffset.y);
                  target->ui.position = ViewportRenderHelpers::RotateUiVec2(
                      worldPivot - parentOffset - parallaxOffset(*target),
                      -targetParentRot);
                }
              } else if (op == ImGuizmo::TRANSLATE) {
                glm::vec2 worldSize = getSpriteDisplaySize(*target);
                ImVec2 pivotOffset = anchorToPivotUI(
                    target->ui.anchor, ImVec2(worldSize.x, worldSize.y));
                glm::vec2 worldMin = worldCenter - worldSize * 0.5f;
                glm::vec2 worldPivot =
                    worldMin + glm::vec2(pivotOffset.x, pivotOffset.y);
                target->ui.position = ViewportRenderHelpers::RotateUiVec2(
                    worldPivot - parentOffset - parallaxOffset(*target),
                    -targetParentRot);
              } else if (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS) {
                if (allowScaleApply) {
                  const float minUiSize =
                      (target->ui.type == UIElementType::Image ||
                       target->ui.type == UIElementType::Sprite2D)
                          ? 0.01f
                          : 1.0f;
                  ImVec2 newSize =
                      stableRectScale
                          ? ImVec2(std::max(minUiSize, targetNewScreenSize.x),
                                   std::max(minUiSize, targetNewScreenSize.y))
                          : ImVec2(std::max(minUiSize, scl.x),
                                   std::max(minUiSize, scl.y));
                  if (stableRectScale) {
                    constexpr float kRectScaleDeadZonePx = 0.1f;
                    if (std::abs(newSize.x - targetSize.x) <
                        kRectScaleDeadZonePx)
                      newSize.x = targetSize.x;
                    if (std::abs(newSize.y - targetSize.y) <
                        kRectScaleDeadZonePx)
                      newSize.y = targetSize.y;
                  }
                  if (applyPixelSnap) {
                    newSize.x =
                        std::max(pixelStep,
                                 std::round(newSize.x / pixelStep) * pixelStep);
                    newSize.y =
                        std::max(pixelStep,
                                 std::round(newSize.y / pixelStep) * pixelStep);
                  }
                  glm::vec2 worldSize =
                      glm::vec2(newSize.x, newSize.y) / uiWorldCamera.zoom;
                  ImVec2 pivotOffset = anchorToPivotUI(
                      target->ui.anchor, ImVec2(worldSize.x, worldSize.y));
                  glm::vec2 worldMin = worldCenter - worldSize * 0.5f;
                  glm::vec2 worldPivot =
                      worldMin + glm::vec2(pivotOffset.x, pivotOffset.y);
                  target->ui.position = ViewportRenderHelpers::RotateUiVec2(
                      worldPivot - parentOffset - parallaxOffset(*target),
                      -targetParentRot);
                  glm::vec2 targetScale(
                      std::max(0.01f, std::abs(target->scale.x)),
                      std::max(0.01f, std::abs(target->scale.y)));
                  target->ui.size = worldSize / targetScale;
                  const UiRectGizmoSnapshot *snapshot = findRectSnapshot(id);
                  if (snapshot) {
                    ImVec2 startSize(
                        snapshot->rectMax.x - snapshot->rectMin.x,
                        snapshot->rectMax.y - snapshot->rectMin.y);
                    ApplyUITextScaleFromRectResize(
                        *target, snapshot->textScale, snapshot->fontSize,
                        startSize, newSize);
                  }
                }
              }
            }

            projectManager.currentProject.hasUnsavedChanges = true;
            gizmoUsed = true;
          } else {
            worldUiGizmoHistoryCaptured = false;
            worldUiRectGizmoSnapshots.clear();
            worldUiRectGizmoModel = glm::mat4(1.0f);
            worldUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
          }
        }
      } else {
        worldUiGizmoHistoryCaptured = false;
        worldUiRectGizmoSnapshots.clear();
        worldUiRectGizmoModel = glm::mat4(1.0f);
        worldUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
      }

      ImGui::EndChild();
      ImGui::PopStyleVar();

      if ((worldUiEditing && ImGui::IsAnyItemActive()) || uiWorldCameraActive ||
          gizmoUsed) {
        blockSelection = true;
      }
    }

    bool sceneInteractionOverlayActive = false;
    if (!worldUiEditing) {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::SetCursorScreenPos(imageMin);
      ImGui::BeginChild(
          "SceneViewportInteractionOverlay",
          ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y), false,
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
              ImGuiWindowFlags_NoScrollWithMouse |
              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
              ImGuiWindowFlags_NoNav);
      sceneInteractionOverlayActive = true;
    }

    auto projectToScreen = [&](const glm::vec3 &p) -> std::optional<ImVec2> {
      glm::vec4 clip = proj * view * glm::vec4(p, 1.0f);
      if (clip.w <= 0.0f)
        return std::nullopt;
      glm::vec3 ndc = glm::vec3(clip) / clip.w;
      ImVec2 screen;
      screen.x = imageMin.x + (ndc.x * 0.5f + 0.5f) * (imageMax.x - imageMin.x);
      screen.y = imageMin.y +
                 (1.0f - (ndc.y * 0.5f + 0.5f)) * (imageMax.y - imageMin.y);
      return screen;
    };

    SceneObject *selectedObj = getSelectedObject();
    bool selectedIsUiCanvas3D = selectedObj && selectedObj->hasUI &&
                                selectedObj->ui.type == UIElementType::Canvas &&
                                selectedObj->ui.renderIn3D;
    const bool selectedIsSprite25D =
        selectedObj && selectedObj->type == ObjectType::Sprite25D;
    const bool selectedRMeshObject =
        selectedObj && IsMeshEditablePath(selectedObj->meshPath);
    const ImVec2 rmeshModeButtonSize(34.0f, 30.0f);
    const ImVec2 rmeshActionsButtonSize(42.0f, 15.0f);
    const float rmeshModePadding = 6.0f;
    const float rmeshModeSpacing = 5.0f;
    const ImVec2 rmeshModeToolbarSize(
        rmeshModePadding * 2.0f + rmeshModeButtonSize.x * 6.0f +
            rmeshModeSpacing * 5.0f,
        rmeshModePadding * 2.0f + rmeshModeButtonSize.y +
            rmeshActionsButtonSize.y + 3.0f);
    auto computeRMeshModeToolbarMin = [&]() {
      ImVec2 toolbarMin(
          imageMin.x +
              (imageMax.x - imageMin.x - rmeshModeToolbarSize.x) * 0.5f,
          imageMin.y + toolbarInsetY);
      const float minX = imageMin.x + 6.0f;
      const float maxX =
          std::max(minX, imageMax.x - rmeshModeToolbarSize.x - 6.0f);
      const float minY = imageMin.y + 6.0f;
      const float maxY =
          std::max(minY, imageMax.y - rmeshModeToolbarSize.y - 6.0f);
      toolbarMin.x = std::clamp(toolbarMin.x, minX, maxX);
      toolbarMin.y = std::clamp(toolbarMin.y, minY, maxY);
      return toolbarMin;
    };
    auto pointInExpandedRect = [](const ImVec2 &p, const ImVec2 &min,
                                  const ImVec2 &max, float expand) {
      return p.x >= min.x - expand && p.x <= max.x + expand &&
             p.y >= min.y - expand && p.y <= max.y + expand;
    };
    const ImVec2 rmeshModeToolbarMin = computeRMeshModeToolbarMin();
    const ImVec2 rmeshModeToolbarMax(
        rmeshModeToolbarMin.x + rmeshModeToolbarSize.x,
        rmeshModeToolbarMin.y + rmeshModeToolbarSize.y);
    const ImVec2 rmeshMouse = ImGui::GetIO().MousePos;
    const bool mouseOverRMeshModeToolbar =
        selectedRMeshObject &&
        pointInExpandedRect(rmeshMouse, rmeshModeToolbarMin,
                            rmeshModeToolbarMax, 6.0f);
    const bool mouseOverRMeshActionsPopup =
        selectedRMeshObject &&
        ImGui::IsPopupOpen("##mesh_edit_context_menu", ImGuiPopupFlags_None);
    if (mouseOverRMeshModeToolbar || mouseOverRMeshActionsPopup) {
      blockSelection = true;
    }
    const bool sceneGizmoEligible =
        !worldUiEditing && selectedObj && IsObjectEnabledInHierarchy(*selectedObj) &&
        !selectedObj->IsInvariable &&
        !selectedObj->hasPostFX &&
        (!HasUIComponent(*selectedObj) || selectedIsUiCanvas3D ||
         selectedIsSprite25D);
    if (!sceneGizmoEligible && gizmoDragActive) {
      gizmoDragActive = false;
      gizmoDragPivotShift = glm::vec3(0.0f);
      gizmoDragStartTransforms.clear();
      gizmoDragLastAppliedTransforms.clear();
    }
    if (sceneGizmoEligible) {
      ImGuizmo::BeginFrame();
      ImGuizmo::Enable(true);
      ImGuizmo::SetOrthographic(false);
      ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
      ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                        imageMax.y - imageMin.y);

      bool meshModeActive = meshEditMode && ensureMeshEditTarget(selectedObj);

      if (meshModeActive && mouseOverViewportImage &&
          !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl &&
          !cursorLocked) {
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
          meshEditSelectionMode =
              (meshEditSelectionMode == MeshEditSelectionMode::Object)
                  ? MeshEditSelectionMode::Vertex
                  : MeshEditSelectionMode::Object;
          meshEditTriangleSelection = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
          meshEditSelectionMode = MeshEditSelectionMode::Vertex;
          meshEditTriangleSelection = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
          meshEditSelectionMode = MeshEditSelectionMode::Edge;
          meshEditTriangleSelection = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
          meshEditSelectionMode = MeshEditSelectionMode::Face;
          meshEditTriangleSelection = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_4, false)) {
          meshEditSelectionMode = MeshEditSelectionMode::UV;
          meshEditTriangleSelection = false;
        }
        if (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
          meshEditSelectionMode = MeshEditSelectionMode::Carve;
          meshEditTriangleSelection = false;
          resetCarveToolState();
        }
      }

      bool meshComponentMode =
          meshModeActive &&
          meshEditSelectionMode != MeshEditSelectionMode::Object &&
          meshEditSelectionMode != MeshEditSelectionMode::Carve &&
          !meshEditAsset.positions.empty();

      glm::vec3 pivotPos = selectedObj->position;
      if (!meshModeActive && selectedObjectIds.size() > 1 &&
          mCurrentGizmoMode == ImGuizmo::WORLD) {
        pivotPos = getSelectionCenterWorld(true);
      }

      glm::mat4 modelMatrix(1.0f);
      modelMatrix = glm::translate(modelMatrix, pivotPos);
      modelMatrix =
          glm::rotate(modelMatrix, glm::radians(selectedObj->rotation.x),
                      glm::vec3(1, 0, 0));
      modelMatrix =
          glm::rotate(modelMatrix, glm::radians(selectedObj->rotation.y),
                      glm::vec3(0, 1, 0));
      modelMatrix =
          glm::rotate(modelMatrix, glm::radians(selectedObj->rotation.z),
                      glm::vec3(0, 0, 1));
      modelMatrix = glm::scale(modelMatrix, selectedObj->scale);
      glm::mat4 originalModel = modelMatrix;

      if (meshModeActive &&
          meshEditSelectionMode == MeshEditSelectionMode::Carve &&
          !meshEditAsset.positions.empty()) {
        ImDrawList *carveDrawList = ImGui::GetWindowDrawList();
        const glm::mat4 invModel = glm::inverse(modelMatrix);
        const float carveGrid =
            (selectedObj->hasMapMesh && selectedObj->mapMesh.gridSize > 0.0f)
                ? selectedObj->mapMesh.gridSize
                : std::max(0.01f, meshEditGridSnap);
        auto snapUV = [&](glm::vec2 uv) {
          if (!carveSnapToGrid) return uv;
          uv.x = std::round(uv.x / carveGrid) * carveGrid;
          uv.y = std::round(uv.y / carveGrid) * carveGrid;
          return uv;
        };

        if (carveFaceIndex >= static_cast<int>(meshEditAsset.faces.size())) {
          resetCarveToolState();
        }

        const ImVec2 carveMouse = ImGui::GetIO().MousePos;
        glm::vec3 rayOriginLocal(0.0f);
        glm::vec3 rayDirLocal(0.0f, 0.0f, 1.0f);
        bool rayValid = false;
        if (imageMax.x - imageMin.x > 1.0f && imageMax.y - imageMin.y > 1.0f) {
          const glm::vec2 ndc(
              ((carveMouse.x - imageMin.x) / (imageMax.x - imageMin.x)) * 2.0f -
                  1.0f,
              1.0f -
                  ((carveMouse.y - imageMin.y) / (imageMax.y - imageMin.y)) *
                      2.0f);
          const glm::mat4 invViewProjCarve = glm::inverse(proj * view);
          glm::vec4 nearPoint = invViewProjCarve * glm::vec4(ndc, -1.0f, 1.0f);
          glm::vec4 farPoint = invViewProjCarve * glm::vec4(ndc, 1.0f, 1.0f);
          if (std::fabs(nearPoint.w) > 1e-9f && std::fabs(farPoint.w) > 1e-9f) {
            nearPoint /= nearPoint.w;
            farPoint /= farPoint.w;
            rayOriginLocal = glm::vec3(invModel * nearPoint);
            const glm::vec3 dirLocal =
                glm::vec3(invModel * farPoint) - rayOriginLocal;
            if (glm::length(dirLocal) > 1e-9f) {
              rayDirLocal = glm::normalize(dirLocal);
              rayValid = true;
            }
          }
        }

        auto rayTriangleLocal = [&](const glm::vec3 &a, const glm::vec3 &b,
                                    const glm::vec3 &c) -> float {
          const glm::vec3 e1 = b - a;
          const glm::vec3 e2 = c - a;
          const glm::vec3 pv = glm::cross(rayDirLocal, e2);
          const float det = glm::dot(e1, pv);
          if (std::fabs(det) < 1e-9f) return -1.0f;
          const float invDet = 1.0f / det;
          const glm::vec3 tv = rayOriginLocal - a;
          const float u = glm::dot(tv, pv) * invDet;
          if (u < 0.0f || u > 1.0f) return -1.0f;
          const glm::vec3 qv = glm::cross(tv, e1);
          const float v = glm::dot(rayDirLocal, qv) * invDet;
          if (v < 0.0f || u + v > 1.0f) return -1.0f;
          const float t = glm::dot(e2, qv) * invDet;
          return t > 1e-5f ? t : -1.0f;
        };

        int carveHoveredFace = -1;
        glm::vec3 carveHitLocal(0.0f);
        if (rayValid && mouseOverViewportImage && !blockSelection) {
          float bestT = FLT_MAX;
          for (size_t fi = 0; fi < meshEditAsset.faces.size(); ++fi) {
            const glm::u32vec3 &f = meshEditAsset.faces[fi];
            if (f.x >= meshEditAsset.positions.size() ||
                f.y >= meshEditAsset.positions.size() ||
                f.z >= meshEditAsset.positions.size()) {
              continue;
            }
            const float t = rayTriangleLocal(meshEditAsset.positions[f.x],
                                             meshEditAsset.positions[f.y],
                                             meshEditAsset.positions[f.z]);
            if (t > 0.0f && t < bestT) {
              bestT = t;
              carveHoveredFace = static_cast<int>(fi);
            }
          }
          if (carveHoveredFace >= 0) {
            carveHitLocal = rayOriginLocal + rayDirLocal * bestT;
          }
        }

        if (carveHoveredFace >= 0 &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          MapCarve::PlaneBasis pressBasis;
          std::string basisError;
          if (MapCarve::BuildPlaneBasisFromFace(meshEditAsset, carveHoveredFace,
                                                pressBasis, &basisError)) {
            carveFaceIndex = carveHoveredFace;
            carveDragActive = true;
            carveDragStartUV = snapUV(pressBasis.toPlane(carveHitLocal));
            carvePreviewDirty = true;
          }
        }
        MapCarve::PlaneBasis carveBasis;
        bool carveBasisValid =
            carveFaceIndex >= 0 &&
            carveFaceIndex < static_cast<int>(meshEditAsset.faces.size()) &&
            MapCarve::BuildPlaneBasisFromFace(meshEditAsset, carveFaceIndex,
                                              carveBasis, nullptr);
        if (carveDragActive && carveBasisValid) {
          const float denom = glm::dot(rayDirLocal, carveBasis.normal);
          if (rayValid && std::fabs(denom) > 1e-6f) {
            const float t = glm::dot(carveBasis.origin - rayOriginLocal,
                                     carveBasis.normal) /
                            denom;
            if (t > 0.0f) {
              const glm::vec2 currentUV =
                  snapUV(carveBasis.toPlane(rayOriginLocal + rayDirLocal * t));
              const glm::vec2 minUV = glm::min(carveDragStartUV, currentUV);
              const glm::vec2 maxUV = glm::max(carveDragStartUV, currentUV);
              const glm::vec2 span = maxUV - minUV;
              if (span.x > 0.01f && span.y > 0.01f) {
                carveRectCenter = (minUV + maxUV) * 0.5f;
                carveRectSize = span;
                carveHasRect = true;
                carvePreviewDirty = true;
              } else if (!carveHasRect) {
                carveRectCenter = carveDragStartUV;
                carveHasRect = true;
                carvePreviewDirty = true;
              }
            }
          }
          if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            carveDragActive = false;
            carvePreviewDirty = true;
          }
        }

        auto buildCarveShape = [&]() {
          const glm::vec2 halfSize =
              glm::max(carveRectSize * 0.5f, glm::vec2(0.02f));
          return carveShapeSegments >= 3
                     ? MapCarve::MakeEllipseShape(carveRectCenter, halfSize,
                                                  carveShapeSegments)
                     : MapCarve::MakeRectShape(carveRectCenter, halfSize);
        };

        if (carveHasRect && carveBasisValid && carvePreviewDirty) {
          carvePreviewDirty = false;
          const std::vector<int> island =
              MapCarve::GatherCoplanarIsland(meshEditAsset, carveFaceIndex);
          MapCarve::CarveShape checkShape = buildCarveShape();
          if (carveCreateFrame && carveFrameThickness > 1e-4f) {
            MapCarve::CarveShape expanded;
            if (MapCarve::OffsetConvexPolygon(checkShape, carveFrameThickness,
                                              expanded)) {
              checkShape = expanded;
            }
          }
          carveShapeValid = MapCarve::ShapeFitsIsland(
              meshEditAsset, island, carveBasis, checkShape,
              &carveInvalidReason);
        }

        auto cancelCarve = [&]() { resetCarveToolState(); };
        auto applyCarve = [&]() {
          if (!carveHasRect || !carveBasisValid) return;
          MapCarve::CarveOptions carveOptions;
          carveOptions.cutThrough = carveCutThrough;
          carveOptions.pocketDepth = carveDepth;
          carveOptions.surfaceOffset = carveSurfaceOffset;
          carveOptions.createFrame = carveCreateFrame;
          carveOptions.frameThickness = carveFrameThickness;
          carveOptions.maxThroughDistance = 32.0f;
          carveOptions.newFaceMaterial =
              static_cast<uint32_t>(std::max(0, meshEditActiveMaterialSlot));
          SceneSnapshot carveUndoSnapshot = captureSceneSnapshot();
          MapCarve::CarveOutcome carved = MapCarve::CarveShapeIntoMesh(
              meshEditAsset, carveFaceIndex, buildCarveShape(), carveOptions);
          if (!carved.success) {
            addConsoleMessage("Carve failed: " + carved.error,
                              ConsoleMessageType::Warning);
            return;
          }
          const std::vector<std::string> carveProblems =
              MapMaker::ValidateMeshAsset(meshEditAsset);
          pushUndoSnapshot(std::move(carveUndoSnapshot), "Carve");
          meshEditAsset.normals.assign(meshEditAsset.positions.size(),
                                       glm::vec3(0.0f));
          for (const auto &f : meshEditAsset.faces) {
            const glm::vec3 n = glm::cross(meshEditAsset.positions[f.y] -
                                               meshEditAsset.positions[f.x],
                                           meshEditAsset.positions[f.z] -
                                               meshEditAsset.positions[f.x]);
            meshEditAsset.normals[f.x] += n;
            meshEditAsset.normals[f.y] += n;
            meshEditAsset.normals[f.z] += n;
          }
          for (auto &n : meshEditAsset.normals) {
            if (glm::length(n) > 1e-6f) n = glm::normalize(n);
          }
          meshEditAsset.hasNormals = true;
          meshEditDirty = true;
          syncMeshEditToGPU(selectedObj);
          for (const std::string &problem : carveProblems) {
            addConsoleMessage("Mesh check (Carve): " + problem,
                              ConsoleMessageType::Warning);
          }
          addConsoleMessage("Carved opening (" +
                                std::to_string(carved.newFaceIndices.size()) +
                                " new faces)",
                            ConsoleMessageType::Success);
          if (carveCreateConnectedSector && carveCutThrough) {
            createConnectedSectorThroughCarve(selectedObj, carved, carveBasis,
                                              modelMatrix);
          }
          resetCarveToolState();
        };

        if (mouseOverViewportImage && !ImGui::GetIO().WantTextInput) {
          if (carveHasRect &&
              (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
            applyCarve();
          }
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (carveHasRect || carveDragActive) {
              cancelCarve();
            } else {
              meshEditSelectionMode = MeshEditSelectionMode::Object;
            }
          }
        }

        auto drawShapeOutline = [&](const MapCarve::CarveShape &shape,
                                    ImU32 lineColor, ImU32 fillColor) {
          std::vector<ImVec2> screenPoints;
          screenPoints.reserve(shape.size());
          for (const glm::vec2 &p : shape) {
            const glm::vec3 world = glm::vec3(
                modelMatrix * glm::vec4(carveBasis.toWorld(p), 1.0f));
            const auto screen = projectToScreen(world);
            if (!screen.has_value()) return;
            screenPoints.push_back(*screen);
          }
          if (screenPoints.size() < 3) return;
          if ((fillColor & IM_COL32_A_MASK) != 0) {
            carveDrawList->AddConvexPolyFilled(
                screenPoints.data(), static_cast<int>(screenPoints.size()),
                fillColor);
          }
          carveDrawList->AddPolyline(screenPoints.data(),
                                     static_cast<int>(screenPoints.size()),
                                     lineColor, ImDrawFlags_Closed, 2.0f);
        };

        if (carveHasRect && carveBasisValid) {
          const ImU32 line = carveShapeValid
                                 ? IM_COL32(80, 230, 160, 235)
                                 : IM_COL32(240, 80, 70, 235);
          const ImU32 fill = carveShapeValid ? IM_COL32(80, 230, 160, 40)
                                             : IM_COL32(240, 80, 70, 46);
          drawShapeOutline(buildCarveShape(), line, fill);
          const glm::vec3 centerWorld =
              glm::vec3(modelMatrix *
                        glm::vec4(carveBasis.toWorld(carveRectCenter), 1.0f));
          const float arrowLength =
              carveCutThrough ? 0.6f : std::max(0.1f, carveDepth);
          const glm::vec3 tipWorld = glm::vec3(
              modelMatrix *
              glm::vec4(carveBasis.toWorld(carveRectCenter) -
                            carveBasis.normal * arrowLength,
                        1.0f));
          const auto centerScreen = projectToScreen(centerWorld);
          const auto tipScreen = projectToScreen(tipWorld);
          if (centerScreen.has_value() && tipScreen.has_value()) {
            carveDrawList->AddLine(*centerScreen, *tipScreen,
                                   IM_COL32(255, 220, 120, 220), 2.0f);
            carveDrawList->AddCircleFilled(*tipScreen, 3.5f,
                                           IM_COL32(255, 220, 120, 220));
          }
          char dims[96];
          std::snprintf(dims, sizeof(dims), "%.2f x %.2f%s%s",
                        carveRectSize.x, carveRectSize.y,
                        carveCutThrough ? "  through" : "  pocket",
                        carveShapeValid ? "  (Enter to carve)" : "");
          if (centerScreen.has_value()) {
            carveDrawList->AddText(
                ImVec2(centerScreen->x + 10.0f, centerScreen->y - 18.0f),
                IM_COL32(255, 255, 255, 220), dims);
          }
          if (!carveShapeValid && !carveInvalidReason.empty() &&
              centerScreen.has_value()) {
            carveDrawList->AddText(
                ImVec2(centerScreen->x + 10.0f, centerScreen->y + 2.0f),
                IM_COL32(255, 120, 100, 235), carveInvalidReason.c_str());
          }
        } else if (carveHoveredFace >= 0 && !carveDragActive) {
          const glm::u32vec3 &f = meshEditAsset.faces[carveHoveredFace];
          const glm::vec3 corners[3] = {
            glm::vec3(modelMatrix *
                      glm::vec4(meshEditAsset.positions[f.x], 1.0f)),
            glm::vec3(modelMatrix *
                      glm::vec4(meshEditAsset.positions[f.y], 1.0f)),
            glm::vec3(modelMatrix *
                      glm::vec4(meshEditAsset.positions[f.z], 1.0f))
          };
          const auto s0 = projectToScreen(corners[0]);
          const auto s1 = projectToScreen(corners[1]);
          const auto s2 = projectToScreen(corners[2]);
          if (s0.has_value() && s1.has_value() && s2.has_value()) {
            carveDrawList->AddTriangle(*s0, *s1, *s2,
                                       IM_COL32(120, 220, 255, 180), 1.6f);
          }
        }

        {
          const ImVec2 panelSize(238.0f, carveCutThrough ? 300.0f : 322.0f);
          ImVec2 panelPos(imageMax.x - panelSize.x - 12.0f,
                          imageMin.y + 52.0f);
          panelPos.x = std::max(panelPos.x, imageMin.x + 4.0f);
          ImGui::SetCursorScreenPos(panelPos);
          ImVec4 carvePanelBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
          carvePanelBg.w = 0.92f;
          ImGui::PushStyleColor(ImGuiCol_ChildBg, carvePanelBg);
          ImGui::BeginChild("##CarveOptionsPanel", panelSize, true,
                            ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoSavedSettings);
          ImGui::TextUnformatted("Carve");
          ImGui::Separator();
          ImGui::SetNextItemWidth(-1.0f);
          float sizeValues[2] = { carveRectSize.x, carveRectSize.y };
          if (ImGui::DragFloat2("##CarveSize", sizeValues, 0.05f, 0.05f, 64.0f,
                                "%.2f")) {
            carveRectSize = glm::vec2(std::max(0.05f, sizeValues[0]),
                                      std::max(0.05f, sizeValues[1]));
            carvePreviewDirty = true;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opening width and height");
          }
          {
            static const char *kShapeNames[] = { "Rectangle", "Hexagon",
                                                 "Octagon", "Circle" };
            static const int kShapeSegments[] = { 0, 6, 8, 24 };
            int shapeIndex = 0;
            for (int i = 0; i < 4; ++i) {
              if (carveShapeSegments == kShapeSegments[i]) shapeIndex = i;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##CarveShape", &shapeIndex, kShapeNames, 4)) {
              carveShapeSegments = kShapeSegments[shapeIndex];
              carvePreviewDirty = true;
            }
          }
          if (ImGui::Checkbox("Cut Through", &carveCutThrough)) {
            carvePreviewDirty = true;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Cut through to the opposite wall. Off = carve a pocket.");
          }
          if (!carveCutThrough) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##CarveDepth", &carveDepth, 0.02f, 0.02f, 16.0f,
                             "depth %.2f");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##CarveOffset", &carveSurfaceOffset, 0.01f, 0.0f,
                             4.0f, "offset %.2f");
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("Extra recess added to the pocket depth");
            }
          }
          if (ImGui::Checkbox("Frame", &carveCreateFrame)) {
            carvePreviewDirty = true;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Adds a flush border ring around the opening using the active "
                "material slot");
          }
          if (carveCreateFrame) {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##CarveFrame", &carveFrameThickness, 0.01f,
                                 0.01f, 2.0f, "frame %.2f")) {
              carvePreviewDirty = true;
            }
          }
          ImGui::Checkbox("Snap To Grid", &carveSnapToGrid);
          ImGui::SameLine();
          ImGui::TextDisabled("(%.2f)", carveGrid);
          if (carveCutThrough) {
            ImGui::Checkbox("Connect Sector", &carveCreateConnectedSector);
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip(
                  "After carving, generate a connected room on the other side "
                  "with portals and a Sector Map entry");
            }
            if (carveCreateConnectedSector) {
              static const char *kRoomPresets[] = {
                "Small Room", "Medium Room", "Large Room", "Corridor",
                "Empty Sector"
              };
              ImGui::SetNextItemWidth(-1.0f);
              ImGui::Combo("##CarvePreset", &carveConnectedPreset,
                           kRoomPresets, 5);
            }
          }
          if (ImGui::Button("Center On Face") && carveBasisValid) {
            const std::vector<int> island =
                MapCarve::GatherCoplanarIsland(meshEditAsset, carveFaceIndex);
            glm::vec2 minUV(FLT_MAX);
            glm::vec2 maxUV(-FLT_MAX);
            for (int fi : island) {
              const glm::u32vec3 &islandFace = meshEditAsset.faces[fi];
              const uint32_t tri[3] = { islandFace.x, islandFace.y,
                                        islandFace.z };
              for (int k = 0; k < 3; ++k) {
                const glm::vec2 uv =
                    carveBasis.toPlane(meshEditAsset.positions[tri[k]]);
                minUV = glm::min(minUV, uv);
                maxUV = glm::max(maxUV, uv);
              }
            }
            if (minUV.x < maxUV.x) {
              carveRectCenter = snapUV((minUV + maxUV) * 0.5f);
              carveHasRect = true;
              carvePreviewDirty = true;
            }
          }
          ImGui::BeginDisabled(!carveHasRect || !carveShapeValid);
          if (ImGui::Button("Apply", ImVec2(104.0f, 0.0f))) {
            applyCarve();
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::BeginDisabled(!carveHasRect && !carveDragActive);
          if (ImGui::Button("Cancel", ImVec2(104.0f, 0.0f))) {
            cancelCarve();
          }
          ImGui::EndDisabled();
          if (!carveHasRect) {
            ImGui::TextWrapped("Drag on a wall to draw the opening.");
          }
          const bool carvePanelHovered = ImGui::IsWindowHovered(
              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
          ImGui::EndChild();
          ImGui::PopStyleColor();
          if (carvePanelHovered) {
            blockSelection = true;
          }
        }
      }

      if (meshComponentMode) {
        // Build helper edge list (dedup) for edge/face modes
        std::vector<glm::u32vec2> edges =
            MeshEditOperations::BuildCanonicalEdges(meshEditAsset);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImU32 vertCol = ImGui::GetColorU32(ImVec4(0.35f, 0.75f, 1.0f, 0.9f));
        ImU32 selCol = ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        float edgeAlpha =
            ((meshEditSelectionMode == MeshEditSelectionMode::Face) ||
             (meshEditSelectionMode == MeshEditSelectionMode::UV))
                ? 0.35f
                : 0.6f;
        ImU32 edgeCol = ImGui::GetColorU32(ImVec4(0.6f, 0.9f, 1.0f, edgeAlpha));
        ImU32 faceSelFillCol =
            ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 0.38f));
        ImU32 hoverCol = ImGui::GetColorU32(ImVec4(1.0f, 0.95f, 0.2f, 0.95f));
        ImU32 faceHoverFillCol =
            ImGui::GetColorU32(ImVec4(1.0f, 0.95f, 0.2f, 0.22f));

        float selectRadius =
            (meshEditSelectionMode == MeshEditSelectionMode::Edge) ? 8.0f
                                                                   : 10.0f;
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool clicked = mouseOverViewportImage && ImGui::IsMouseClicked(0) &&
                       !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
                       !blockSelection;
        bool doubleClicked = mouseOverViewportImage &&
                             ImGui::IsMouseDoubleClicked(0) &&
                             !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
                             !blockSelection;
        bool additiveClick = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
        bool meshSelectionChangedThisFrame = false;

        glm::mat4 invModel = glm::inverse(modelMatrix);
        glm::mat4 invViewProj = glm::inverse(proj * view);

        auto distPointToSegment = [](const ImVec2 &p, const ImVec2 &a,
                                     const ImVec2 &b) {
          ImVec2 ab = ImVec2(b.x - a.x, b.y - a.y);
          float len2 = ab.x * ab.x + ab.y * ab.y;
          if (len2 < 1e-4f) {
            float dx = p.x - a.x;
            float dy = p.y - a.y;
            return std::sqrt(dx * dx + dy * dy);
          }
          float t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2;
          t = std::clamp(t, 0.0f, 1.0f);
          ImVec2 proj = ImVec2(a.x + ab.x * t, a.y + ab.y * t);
          float dx = p.x - proj.x;
          float dy = p.y - proj.y;
          return std::sqrt(dx * dx + dy * dy);
        };

        auto makeRay = [&](const ImVec2 &pos) {
          float x = (pos.x - imageMin.x) / (imageMax.x - imageMin.x);
          float y = (pos.y - imageMin.y) / (imageMax.y - imageMin.y);
          x = x * 2.0f - 1.0f;
          y = 1.0f - y * 2.0f;

          glm::vec4 nearPt = invViewProj * glm::vec4(x, y, -1.0f, 1.0f);
          glm::vec4 farPt = invViewProj * glm::vec4(x, y, 1.0f, 1.0f);
          nearPt /= nearPt.w;
          farPt /= farPt.w;

          glm::vec3 origin = glm::vec3(nearPt);
          glm::vec3 dir = glm::normalize(glm::vec3(farPt - nearPt));
          return std::make_pair(origin, dir);
        };

        auto rayTriangle = [](const glm::vec3 &orig, const glm::vec3 &dir,
                              const glm::vec3 &v0, const glm::vec3 &v1,
                              const glm::vec3 &v2, float &tHit) {
          const float EPSILON = 1e-6f;
          glm::vec3 e1 = v1 - v0;
          glm::vec3 e2 = v2 - v0;
          glm::vec3 pvec = glm::cross(dir, e2);
          float det = glm::dot(e1, pvec);
          if (fabs(det) < EPSILON)
            return false;
          float invDet = 1.0f / det;
          glm::vec3 tvec = orig - v0;
          float u = glm::dot(tvec, pvec) * invDet;
          if (u < 0.0f || u > 1.0f)
            return false;
          glm::vec3 qvec = glm::cross(tvec, e1);
          float v = glm::dot(dir, qvec) * invDet;
          if (v < 0.0f || u + v > 1.0f)
            return false;
          float t = glm::dot(e2, qvec) * invDet;
          if (t < 0.0f)
            return false;
          tHit = t;
          return true;
        };
        auto findBestEdgeAtMouse = [&](const ImVec2 &mousePos) {
          int bestEdge = -1;
          float bestDist = selectRadius;
          float bestDepth = FLT_MAX;
          for (size_t ei = 0; ei < edges.size(); ++ei) {
            const auto &e = edges[ei];
            if (e.x >= meshEditAsset.positions.size() ||
                e.y >= meshEditAsset.positions.size())
              continue;
            glm::vec3 a = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[e.x], 1.0f));
            glm::vec3 b = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[e.y], 1.0f));
            auto sa = projectToScreen(a);
            auto sb = projectToScreen(b);
            if (!sa || !sb)
              continue;
            float dist = distPointToSegment(mousePos, *sa, *sb);
            if (dist > selectRadius)
              continue;
            glm::vec3 mid = (a + b) * 0.5f;
            glm::vec4 clip = proj * view * glm::vec4(mid, 1.0f);
            if (clip.w <= 1e-6f)
              continue;
            float depth = clip.z / clip.w;
            if (dist < bestDist - 0.1f ||
                (std::abs(dist - bestDist) <= 0.1f && depth < bestDepth)) {
              bestDist = dist;
              bestDepth = depth;
              bestEdge = static_cast<int>(ei);
            }
          }
          return bestEdge;
        };

        float baseEdgeThickness =
            (meshEditSelectionMode == MeshEditSelectionMode::Edge) ? 2.2f
                                                                   : 1.4f;
        for (size_t ei = 0; ei < edges.size(); ++ei) {
          const auto &e = edges[ei];
          glm::vec3 a = glm::vec3(
              modelMatrix * glm::vec4(meshEditAsset.positions[e.x], 1.0f));
          glm::vec3 b = glm::vec3(
              modelMatrix * glm::vec4(meshEditAsset.positions[e.y], 1.0f));
          auto sa = projectToScreen(a);
          auto sb = projectToScreen(b);
          if (!sa || !sb)
            continue;
          bool sel = meshEditSelectionMode == MeshEditSelectionMode::Edge &&
                     std::find(meshEditSelectedEdges.begin(),
                               meshEditSelectedEdges.end(),
                               (int)ei) != meshEditSelectedEdges.end();
          float thickness = sel ? baseEdgeThickness + 1.1f : baseEdgeThickness;
          ImU32 color = sel ? selCol : edgeCol;
          dl->AddLine(*sa, *sb, color, thickness);
        }

        if (meshEditSelectionMode == MeshEditSelectionMode::Vertex) {
          const size_t maxDraw =
              std::min<size_t>(meshEditAsset.positions.size(), 2000);
          float bestDist = selectRadius;
          int hoveredIndex = -1;
          for (size_t i = 0; i < maxDraw; ++i) {
            glm::vec3 world = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[i], 1.0f));
            auto screen = projectToScreen(world);
            if (!screen)
              continue;
            float dx = screen->x - mouse.x;
            float dy = screen->y - mouse.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < bestDist) {
              bestDist = dist;
              hoveredIndex = static_cast<int>(i);
            }
          }
          int clickedIndex = clicked ? hoveredIndex : -1;

          for (size_t i = 0; i < maxDraw; ++i) {
            glm::vec3 world = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[i], 1.0f));
            auto screen = projectToScreen(world);
            if (!screen)
              continue;
            bool sel = std::find(meshEditSelectedVertices.begin(),
                                 meshEditSelectedVertices.end(),
                                 (int)i) != meshEditSelectedVertices.end();
            bool hover = static_cast<int>(i) == hoveredIndex && !sel;
            float radius = sel ? 6.5f : (hover ? 6.0f : 5.0f);
            dl->AddCircleFilled(*screen, radius,
                                sel ? selCol : (hover ? hoverCol : vertCol));
          }

          if (clicked) {
            meshSelectionChangedThisFrame = true;
            if (clickedIndex >= 0) {
              if (additiveClick) {
                auto itSel =
                    std::find(meshEditSelectedVertices.begin(),
                              meshEditSelectedVertices.end(), clickedIndex);
                if (itSel == meshEditSelectedVertices.end()) {
                  meshEditSelectedVertices.push_back(clickedIndex);
                } else {
                  meshEditSelectedVertices.erase(itSel);
                }
              } else {
                meshEditSelectedVertices.clear();
                meshEditSelectedVertices.push_back(clickedIndex);
              }
            } else if (!additiveClick) {
              meshEditSelectedVertices.clear();
            }
            meshEditSelectedEdges.clear();
            meshEditSelectedFaces.clear();
          }
        } else if (meshEditSelectionMode == MeshEditSelectionMode::Edge) {
          int hoveredIndex =
              mouseOverViewportImage ? findBestEdgeAtMouse(mouse) : -1;
          int clickedIndex = clicked ? hoveredIndex : -1;

          if (hoveredIndex >= 0 &&
              hoveredIndex < static_cast<int>(edges.size())) {
            const auto &e = edges[hoveredIndex];
            glm::vec3 a = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[e.x], 1.0f));
            glm::vec3 b = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[e.y], 1.0f));
            auto sa = projectToScreen(a);
            auto sb = projectToScreen(b);
            if (sa && sb) {
              dl->AddLine(*sa, *sb, hoverCol, baseEdgeThickness + 1.8f);
            }
          }

          if (clicked) {
            meshSelectionChangedThisFrame = true;
            if (clickedIndex >= 0) {
              if (additiveClick) {
                auto itSel =
                    std::find(meshEditSelectedEdges.begin(),
                              meshEditSelectedEdges.end(), clickedIndex);
                if (itSel == meshEditSelectedEdges.end()) {
                  meshEditSelectedEdges.push_back(clickedIndex);
                } else {
                  meshEditSelectedEdges.erase(itSel);
                }
              } else {
                meshEditSelectedEdges.clear();
                meshEditSelectedEdges.push_back(clickedIndex);
              }
            } else if (!additiveClick) {
              meshEditSelectedEdges.clear();
            }
            meshEditSelectedVertices.clear();
            meshEditSelectedFaces.clear();
          }
        } else if (meshEditSelectionMode == MeshEditSelectionMode::Face ||
                   meshEditSelectionMode == MeshEditSelectionMode::UV) {
          auto computeFaceNormal = [&](const glm::u32vec3 &f,
                                       glm::vec3 &out) -> bool {
            if (f.x >= meshEditAsset.positions.size() ||
                f.y >= meshEditAsset.positions.size() ||
                f.z >= meshEditAsset.positions.size()) {
              return false;
            }
            const glm::vec3 &a = meshEditAsset.positions[f.x];
            const glm::vec3 &b = meshEditAsset.positions[f.y];
            const glm::vec3 &c = meshEditAsset.positions[f.z];
            glm::vec3 n = glm::cross(b - a, c - a);
            float len = glm::length(n);
            if (len < 1e-6f) {
              return false;
            }
            out = n / len;
            return true;
          };
          [[maybe_unused]] auto gatherCoplanarFaces = [&](int seed) {
            std::vector<int> group;
            const size_t faceCount = meshEditAsset.faces.size();
            if (seed < 0 || seed >= (int)faceCount)
              return group;
            glm::vec3 seedNormal(0.0f);
            if (!computeFaceNormal(meshEditAsset.faces[seed], seedNormal)) {
              group.push_back(seed);
              return group;
            }

            std::unordered_map<uint64_t, std::vector<int>> edgeToFaces;
            edgeToFaces.reserve(faceCount * 3);
            auto edgeKey = [](uint32_t a, uint32_t b) {
              return (static_cast<uint64_t>(std::min(a, b)) << 32) |
                     static_cast<uint64_t>(std::max(a, b));
            };
            for (size_t fi = 0; fi < faceCount; ++fi) {
              const auto &f = meshEditAsset.faces[fi];
              uint32_t tri[3] = {f.x, f.y, f.z};
              for (int e = 0; e < 3; ++e) {
                edgeToFaces[edgeKey(tri[e], tri[(e + 1) % 3])].push_back(
                    (int)fi);
              }
            }

            std::vector<char> visited(faceCount, 0);
            std::vector<int> stack;
            visited[seed] = 1;
            stack.push_back(seed);
            group.push_back(seed);

            const auto &seedFace = meshEditAsset.faces[seed];
            glm::vec3 seedPoint = meshEditAsset.positions[seedFace.x];
            float seedD = glm::dot(seedNormal, seedPoint);
            const float normalThreshold = 0.995f;
            const float planeEpsilon = 1e-3f;

            while (!stack.empty()) {
              int current = stack.back();
              stack.pop_back();
              const auto &f = meshEditAsset.faces[current];
              uint32_t tri[3] = {f.x, f.y, f.z};
              for (int e = 0; e < 3; ++e) {
                auto it = edgeToFaces.find(edgeKey(tri[e], tri[(e + 1) % 3]));
                if (it == edgeToFaces.end())
                  continue;
                for (int neighbor : it->second) {
                  if (neighbor < 0 || neighbor >= (int)faceCount)
                    continue;
                  if (visited[neighbor])
                    continue;
                  glm::vec3 n(0.0f);
                  if (!computeFaceNormal(meshEditAsset.faces[neighbor], n))
                    continue;
                  if (glm::dot(seedNormal, n) < normalThreshold)
                    continue;
                  const auto &nf = meshEditAsset.faces[neighbor];
                  const glm::vec3 &na = meshEditAsset.positions[nf.x];
                  const glm::vec3 &nb = meshEditAsset.positions[nf.y];
                  const glm::vec3 &nc = meshEditAsset.positions[nf.z];
                  if (std::abs(glm::dot(seedNormal, na) - seedD) >
                          planeEpsilon ||
                      std::abs(glm::dot(seedNormal, nb) - seedD) >
                          planeEpsilon ||
                      std::abs(glm::dot(seedNormal, nc) - seedD) >
                          planeEpsilon) {
                    continue;
                  }
                  visited[neighbor] = 1;
                  stack.push_back(neighbor);
                  group.push_back(neighbor);
                }
              }
            }
            std::sort(group.begin(), group.end());
            group.erase(std::unique(group.begin(), group.end()), group.end());
            return group;
          };
          auto gatherFaceGroup = [&](int seed) {
            std::vector<int> group;
            const size_t faceCount = meshEditAsset.faces.size();
            if (seed < 0 || seed >= (int)faceCount)
              return group;
            group.push_back(seed);
            if (meshEditTriangleSelection) {
              return group;
            }

            glm::vec3 seedNormal(0.0f);
            if (!computeFaceNormal(meshEditAsset.faces[seed], seedNormal)) {
              return group;
            }
            const auto &seedFace = meshEditAsset.faces[seed];
            const glm::vec3 seedPoint = meshEditAsset.positions[seedFace.x];
            const float seedPlaneD = glm::dot(seedNormal, seedPoint);
            const float positionEps2 = 1e-10f;
            const float planeEps = 1e-3f;
            auto sharePosition = [&](uint32_t a, uint32_t b) -> bool {
              if (a >= meshEditAsset.positions.size() ||
                  b >= meshEditAsset.positions.size()) {
                return false;
              }
              glm::vec3 d =
                  meshEditAsset.positions[a] - meshEditAsset.positions[b];
              return glm::dot(d, d) <= positionEps2;
            };
            auto edgeSharedLength = [&](const glm::u32vec3 &a,
                                        const glm::u32vec3 &b) {
              uint32_t aTri[3] = {a.x, a.y, a.z};
              uint32_t bTri[3] = {b.x, b.y, b.z};
              float bestLen = 0.0f;
              for (int ea = 0; ea < 3; ++ea) {
                uint32_t a0 = aTri[ea];
                uint32_t a1 = aTri[(ea + 1) % 3];
                for (int eb = 0; eb < 3; ++eb) {
                  uint32_t b0 = bTri[eb];
                  uint32_t b1 = bTri[(eb + 1) % 3];
                  bool sameDir =
                      (a0 == b0 && a1 == b1) || (a0 == b1 && a1 == b0);
                  bool samePos =
                      (sharePosition(a0, b0) && sharePosition(a1, b1)) ||
                      (sharePosition(a0, b1) && sharePosition(a1, b0));
                  if (!sameDir && !samePos)
                    continue;
                  glm::vec3 p0 = meshEditAsset.positions[a0];
                  glm::vec3 p1 = meshEditAsset.positions[a1];
                  bestLen = std::max(bestLen, glm::length(p1 - p0));
                }
              }
              return bestLen;
            };

            int bestNeighbor = -1;
            float bestScore = -FLT_MAX;

            for (size_t fi = 0; fi < faceCount; ++fi) {
              if ((int)fi == seed)
                continue;
              const auto &f = meshEditAsset.faces[fi];
              glm::vec3 n(0.0f);
              if (!computeFaceNormal(f, n))
                continue;
              const float align = std::abs(glm::dot(seedNormal, n));
              if (align < 0.995f)
                continue;
              if (std::abs(glm::dot(seedNormal, meshEditAsset.positions[f.x]) -
                           seedPlaneD) > planeEps ||
                  std::abs(glm::dot(seedNormal, meshEditAsset.positions[f.y]) -
                           seedPlaneD) > planeEps ||
                  std::abs(glm::dot(seedNormal, meshEditAsset.positions[f.z]) -
                           seedPlaneD) > planeEps) {
                continue;
              }
              float sharedLen = edgeSharedLength(seedFace, f);
              if (sharedLen <= 1e-6f)
                continue;
              float score = sharedLen * 1000.0f + align;
              if (score > bestScore) {
                bestScore = score;
                bestNeighbor = static_cast<int>(fi);
              }
            }

            if (bestNeighbor >= 0) {
              group.push_back(bestNeighbor);
            }
            if (group.size() > 1) {
              std::sort(group.begin(), group.end());
              group.erase(std::unique(group.begin(), group.end()), group.end());
            }
            return group;
          };

          for (int fi : meshEditSelectedFaces) {
            if (fi < 0 || fi >= (int)meshEditAsset.faces.size())
              continue;
            const auto &f = meshEditAsset.faces[fi];
            glm::vec3 a = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[f.x], 1.0f));
            glm::vec3 b = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[f.y], 1.0f));
            glm::vec3 c = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[f.z], 1.0f));
            auto sa = projectToScreen(a);
            auto sb = projectToScreen(b);
            auto sc = projectToScreen(c);
            if (!sa || !sb || !sc)
              continue;
            dl->AddTriangleFilled(*sa, *sb, *sc, faceSelFillCol);
            dl->AddTriangle(*sa, *sb, *sc, selCol, 2.0f);
          }

          int hoveredFaceIndex = -1;
          if (mouseOverViewportImage) {
            auto ray = makeRay(mouse);
            glm::vec3 localOrigin =
                glm::vec3(invModel * glm::vec4(ray.first, 1.0f));
            glm::vec3 localDir = glm::normalize(
                glm::vec3(invModel * glm::vec4(ray.second, 0.0f)));
            float bestT = FLT_MAX;
            for (size_t fi = 0; fi < meshEditAsset.faces.size(); ++fi) {
              const auto &f = meshEditAsset.faces[fi];
              if (f.x >= meshEditAsset.positions.size() ||
                  f.y >= meshEditAsset.positions.size() ||
                  f.z >= meshEditAsset.positions.size())
                continue;
              float tHit = 0.0f;
              if (rayTriangle(localOrigin, localDir,
                              meshEditAsset.positions[f.x],
                              meshEditAsset.positions[f.y],
                              meshEditAsset.positions[f.z], tHit)) {
                if (tHit < bestT) {
                  bestT = tHit;
                  hoveredFaceIndex = static_cast<int>(fi);
                }
              }
            }
          }
          std::vector<int> hoverGroup = (hoveredFaceIndex >= 0)
                                            ? gatherFaceGroup(hoveredFaceIndex)
                                            : std::vector<int>{};
          for (int fi : hoverGroup) {
            if (fi < 0 || fi >= static_cast<int>(meshEditAsset.faces.size()))
              continue;
            if (std::find(meshEditSelectedFaces.begin(),
                          meshEditSelectedFaces.end(),
                          fi) != meshEditSelectedFaces.end())
              continue;
            const auto &f = meshEditAsset.faces[fi];
            glm::vec3 a = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[f.x], 1.0f));
            glm::vec3 b = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[f.y], 1.0f));
            glm::vec3 c = glm::vec3(
                modelMatrix * glm::vec4(meshEditAsset.positions[f.z], 1.0f));
            auto sa = projectToScreen(a);
            auto sb = projectToScreen(b);
            auto sc = projectToScreen(c);
            if (!sa || !sb || !sc)
              continue;
            dl->AddTriangleFilled(*sa, *sb, *sc, faceHoverFillCol);
            dl->AddTriangle(*sa, *sb, *sc, hoverCol, 1.6f);
          }

          if (clicked || doubleClicked) {
            meshSelectionChangedThisFrame = true;
            int clickedIndex = hoveredFaceIndex;
            if (clickedIndex >= 0) {
              std::vector<int> group = gatherFaceGroup(clickedIndex);
              if (group.empty())
                group.push_back(clickedIndex);
              if (additiveClick) {
                bool allSelected = true;
                for (int fi : group) {
                  if (std::find(meshEditSelectedFaces.begin(),
                                meshEditSelectedFaces.end(),
                                fi) == meshEditSelectedFaces.end()) {
                    allSelected = false;
                    break;
                  }
                }
                if (allSelected) {
                  for (int fi : group) {
                    auto itSel = std::find(meshEditSelectedFaces.begin(),
                                           meshEditSelectedFaces.end(), fi);
                    if (itSel != meshEditSelectedFaces.end()) {
                      meshEditSelectedFaces.erase(itSel);
                    }
                  }
                } else {
                  for (int fi : group) {
                    if (std::find(meshEditSelectedFaces.begin(),
                                  meshEditSelectedFaces.end(),
                                  fi) == meshEditSelectedFaces.end()) {
                      meshEditSelectedFaces.push_back(fi);
                    }
                  }
                }
              } else {
                meshEditSelectedFaces.clear();
                meshEditSelectedFaces = std::move(group);
              }
            } else if (!additiveClick) {
              meshEditSelectedFaces.clear();
            }
            meshEditSelectedVertices.clear();
            meshEditSelectedEdges.clear();
          }
        }

        // Compute affected vertices from selection
        std::vector<int> baseAffectedVerts;
        if (meshEditSelectionMode == MeshEditSelectionMode::Vertex) {
          baseAffectedVerts = meshEditSelectedVertices;
        }
        auto pushUnique = [&](int idx) {
          if (idx < 0)
            return;
          if (std::find(baseAffectedVerts.begin(), baseAffectedVerts.end(),
                        idx) == baseAffectedVerts.end()) {
            baseAffectedVerts.push_back(idx);
          }
        };
        if (meshEditSelectionMode == MeshEditSelectionMode::Edge) {
          for (int ei : meshEditSelectedEdges) {
            if (ei < 0 || ei >= (int)edges.size())
              continue;
            pushUnique(edges[ei].x);
            pushUnique(edges[ei].y);
          }
        } else if (meshEditSelectionMode == MeshEditSelectionMode::Face ||
                   meshEditSelectionMode == MeshEditSelectionMode::UV) {
          for (int fi : meshEditSelectedFaces) {
            if (fi < 0 || fi >= (int)meshEditAsset.faces.size())
              continue;
            const auto &f = meshEditAsset.faces[fi];
            pushUnique(f.x);
            pushUnique(f.y);
            pushUnique(f.z);
          }
          constexpr float coincidentEps2 = 1e-10f;
          std::vector<int> seamSeeds = baseAffectedVerts;
          for (int seedIdx : seamSeeds) {
            if (seedIdx < 0 ||
                seedIdx >= static_cast<int>(meshEditAsset.positions.size()))
              continue;
            const glm::vec3 seedPos = meshEditAsset.positions[seedIdx];
            for (size_t vi = 0; vi < meshEditAsset.positions.size(); ++vi) {
              const glm::vec3 delta = meshEditAsset.positions[vi] - seedPos;
              if (glm::dot(delta, delta) <= coincidentEps2) {
                pushUnique(static_cast<int>(vi));
              }
            }
          }
        }
        auto recalcMesh = [&]() {
          meshEditAsset.boundsMin = glm::vec3(FLT_MAX);
          meshEditAsset.boundsMax = glm::vec3(-FLT_MAX);
          for (const auto &p : meshEditAsset.positions) {
            meshEditAsset.boundsMin.x =
                std::min(meshEditAsset.boundsMin.x, p.x);
            meshEditAsset.boundsMin.y =
                std::min(meshEditAsset.boundsMin.y, p.y);
            meshEditAsset.boundsMin.z =
                std::min(meshEditAsset.boundsMin.z, p.z);
            meshEditAsset.boundsMax.x =
                std::max(meshEditAsset.boundsMax.x, p.x);
            meshEditAsset.boundsMax.y =
                std::max(meshEditAsset.boundsMax.y, p.y);
            meshEditAsset.boundsMax.z =
                std::max(meshEditAsset.boundsMax.z, p.z);
          }

          meshEditAsset.normals.assign(meshEditAsset.positions.size(),
                                       glm::vec3(0.0f));
          for (const auto &f : meshEditAsset.faces) {
            if (f.x >= meshEditAsset.positions.size() ||
                f.y >= meshEditAsset.positions.size() ||
                f.z >= meshEditAsset.positions.size())
              continue;
            const glm::vec3 &a = meshEditAsset.positions[f.x];
            const glm::vec3 &b = meshEditAsset.positions[f.y];
            const glm::vec3 &c = meshEditAsset.positions[f.z];
            glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
            meshEditAsset.normals[f.x] += n;
            meshEditAsset.normals[f.y] += n;
            meshEditAsset.normals[f.z] += n;
          }
          for (auto &n : meshEditAsset.normals) {
            if (glm::length(n) > 1e-6f)
              n = glm::normalize(n);
          }
          meshEditAsset.hasNormals = true;
          if (meshEditAsset.materialSlots.empty()) {
            meshEditAsset.materialSlots.push_back("Default");
          }
          if (meshEditAsset.faceMaterialIndices.size() !=
              meshEditAsset.faces.size()) {
            meshEditAsset.faceMaterialIndices.resize(
                meshEditAsset.faces.size(),
                static_cast<uint32_t>(meshEditActiveMaterialSlot));
          }
        };

        auto ensureFaceMaterials = [&]() {
          if (meshEditAsset.materialSlots.empty()) {
            meshEditAsset.materialSlots.push_back("Default");
          }
          meshEditActiveMaterialSlot = std::clamp(
              meshEditActiveMaterialSlot, 0,
              static_cast<int>(meshEditAsset.materialSlots.size()) - 1);
          if (meshEditAsset.faceMaterialIndices.size() !=
              meshEditAsset.faces.size()) {
            meshEditAsset.faceMaterialIndices.resize(
                meshEditAsset.faces.size(),
                static_cast<uint32_t>(meshEditActiveMaterialSlot));
          }
          const uint32_t maxMat =
              static_cast<uint32_t>(meshEditAsset.materialSlots.size() - 1);
          for (auto &idx : meshEditAsset.faceMaterialIndices) {
            idx = std::min(idx, maxMat);
          }
        };

        auto ensureUvs = [&]() {
          if (meshEditAsset.uvs.size() < meshEditAsset.positions.size()) {
            meshEditAsset.uvs.resize(meshEditAsset.positions.size(),
                                     glm::vec2(0.0f));
          }
        };

        auto applyPlanarUvToFace = [&](int faceIndex) -> bool {
          if (faceIndex < 0 ||
              faceIndex >= static_cast<int>(meshEditAsset.faces.size())) {
            return false;
          }
          ensureUvs();
          const auto &face = meshEditAsset.faces[faceIndex];
          if (face.x >= meshEditAsset.positions.size() ||
              face.y >= meshEditAsset.positions.size() ||
              face.z >= meshEditAsset.positions.size()) {
            return false;
          }

          const glm::vec3 &a = meshEditAsset.positions[face.x];
          const glm::vec3 &b = meshEditAsset.positions[face.y];
          const glm::vec3 &c = meshEditAsset.positions[face.z];
          glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
          if (glm::length(n) < 1e-6f) {
            n = glm::vec3(0.0f, 0.0f, 1.0f);
          }

          glm::vec2 ua(a.x, a.y), ub(b.x, b.y), uc(c.x, c.y);
          if (std::abs(n.x) >= std::abs(n.y) &&
              std::abs(n.x) >= std::abs(n.z)) {
            ua = glm::vec2(a.y, a.z);
            ub = glm::vec2(b.y, b.z);
            uc = glm::vec2(c.y, c.z);
          } else if (std::abs(n.y) >= std::abs(n.z)) {
            ua = glm::vec2(a.x, a.z);
            ub = glm::vec2(b.x, b.z);
            uc = glm::vec2(c.x, c.z);
          }

          glm::vec2 minUV = glm::min(glm::min(ua, ub), uc);
          glm::vec2 maxUV = glm::max(glm::max(ua, ub), uc);
          glm::vec2 span = maxUV - minUV;
          auto mapUv = [&](const glm::vec2 &v) {
            return glm::vec2(span.x > 1e-6f ? (v.x - minUV.x) / span.x : 0.0f,
                             span.y > 1e-6f ? (v.y - minUV.y) / span.y : 0.0f);
          };
          meshEditAsset.uvs[face.x] = mapUv(ua);
          meshEditAsset.uvs[face.y] = mapUv(ub);
          meshEditAsset.uvs[face.z] = mapUv(uc);
          meshEditAsset.hasUVs = true;
          return true;
        };

        auto compactMesh = [&]() {
          std::vector<glm::u32vec3> compactFaces;
          std::vector<uint32_t> compactMaterials;
          compactFaces.reserve(meshEditAsset.faces.size());
          compactMaterials.reserve(meshEditAsset.faces.size());

          std::vector<uint8_t> used(meshEditAsset.positions.size(), 0u);
          for (size_t fi = 0; fi < meshEditAsset.faces.size(); ++fi) {
            const auto &f = meshEditAsset.faces[fi];
            if (f.x >= meshEditAsset.positions.size() ||
                f.y >= meshEditAsset.positions.size() ||
                f.z >= meshEditAsset.positions.size() || f.x == f.y ||
                f.y == f.z || f.z == f.x) {
              continue;
            }
            compactFaces.push_back(f);
            uint32_t mat = 0u;
            if (fi < meshEditAsset.faceMaterialIndices.size()) {
              mat = meshEditAsset.faceMaterialIndices[fi];
            }
            compactMaterials.push_back(mat);
            used[f.x] = 1u;
            used[f.y] = 1u;
            used[f.z] = 1u;
          }

          std::vector<uint32_t> remap(meshEditAsset.positions.size(),
                                      UINT32_MAX);
          std::vector<glm::vec3> newPositions;
          std::vector<glm::vec3> newNormals;
          std::vector<glm::vec2> newUvs;
          newPositions.reserve(meshEditAsset.positions.size());
          newNormals.reserve(meshEditAsset.positions.size());
          newUvs.reserve(meshEditAsset.positions.size());

          for (size_t i = 0; i < meshEditAsset.positions.size(); ++i) {
            if (!used[i])
              continue;
            remap[i] = static_cast<uint32_t>(newPositions.size());
            newPositions.push_back(meshEditAsset.positions[i]);
            if (i < meshEditAsset.normals.size())
              newNormals.push_back(meshEditAsset.normals[i]);
            else
              newNormals.push_back(glm::vec3(0.0f));
            if (i < meshEditAsset.uvs.size())
              newUvs.push_back(meshEditAsset.uvs[i]);
            else
              newUvs.push_back(glm::vec2(0.0f));
          }

          for (auto &f : compactFaces) {
            f.x = remap[f.x];
            f.y = remap[f.y];
            f.z = remap[f.z];
          }

          meshEditAsset.positions = std::move(newPositions);
          meshEditAsset.normals = std::move(newNormals);
          meshEditAsset.uvs = std::move(newUvs);
          meshEditAsset.faces = std::move(compactFaces);
          meshEditAsset.faceMaterialIndices = std::move(compactMaterials);

          auto remapSelection = [&](std::vector<int> &selection) {
            std::vector<int> newSel;
            newSel.reserve(selection.size());
            for (int idx : selection) {
              if (idx < 0 || idx >= static_cast<int>(remap.size()))
                continue;
              uint32_t mapped = remap[idx];
              if (mapped == UINT32_MAX)
                continue;
              int mappedInt = static_cast<int>(mapped);
              if (std::find(newSel.begin(), newSel.end(), mappedInt) ==
                  newSel.end()) {
                newSel.push_back(mappedInt);
              }
            }
            selection = std::move(newSel);
          };
          remapSelection(meshEditSelectedVertices);
        };

        SceneSnapshot meshEditCommandSnapshot;
        bool meshEditCommandSnapshotValid = false;
        auto commitMeshEdit = [&](const char *actionName,
                                  bool generateAutomaticUvs = true) {
          if (meshEditCommandSnapshotValid) {
            pushUndoSnapshot(std::move(meshEditCommandSnapshot), actionName);
            meshEditCommandSnapshotValid = false;
          } else {
            recordState(actionName ? actionName : "meshEdit");
          }
          compactMesh();
          ensureFaceMaterials();
          recalcMesh();
          if (meshEditAutoUV && generateAutomaticUvs) {
            if (!meshEditSelectedFaces.empty()) {
              for (int fi : meshEditSelectedFaces) {
                applyPlanarUvToFace(fi);
              }
            } else {
              for (int fi = 0;
                   fi < static_cast<int>(meshEditAsset.faces.size()); ++fi) {
                applyPlanarUvToFace(fi);
              }
            }
          }
          meshEditDirty = true;
          if (selectedObj) {
            syncMeshEditToGPU(selectedObj);
          }
          if (actionName && *actionName) {
            addConsoleMessage(std::string("Mesh edit: ") + actionName,
                              ConsoleMessageType::Info);
          }
          {
            const std::vector<std::string> meshProblems =
                MapMaker::ValidateMeshAsset(meshEditAsset);
            for (const std::string &problem : meshProblems) {
              addConsoleMessage(std::string("Mesh check (") +
                                    (actionName ? actionName : "edit") +
                                    "): " + problem,
                                ConsoleMessageType::Warning);
            }
          }
        };

        auto reportMeshEditFailure = [&](const char *actionName,
                                         const std::string &error) {
          addConsoleMessage(std::string("Mesh edit failed (") + actionName +
                                "): " +
                                (error.empty() ? "unsupported topology" : error),
                            ConsoleMessageType::Warning);
        };

        auto addFaceWithMaterial = [&](const glm::u32vec3 &tri,
                                       uint32_t matIdx) {
          meshEditAsset.faces.push_back(tri);
          meshEditAsset.faceMaterialIndices.push_back(matIdx);
        };

        if (meshEditActionsPopupRequested) {
          ImGui::SetNextWindowPos(meshEditActionsPopupPos, ImGuiCond_Always);
          ImGui::OpenPopup("##mesh_edit_context_menu");
          meshEditActionsPopupRequested = false;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 7.0f));
        if (ImGui::BeginPopup("##mesh_edit_context_menu")) {
          ensureFaceMaterials();

          if (ImGui::BeginMenu("Tool Settings")) {
            ImGui::DragFloat("Weld Distance", &meshEditWeldDistance, 0.0001f,
                             0.000001f, 100.0f, "%.6f");
            ImGui::SliderFloat("Smooth Factor", &meshEditSmoothFactor, 0.01f,
                               1.0f, "%.2f");
            ImGui::DragInt("Smooth Iterations", &meshEditSmoothIterations,
                           0.1f, 1, 100);
            ImGui::SliderFloat("Similar Face Angle",
                               &meshEditSimilarFaceAngle, 0.0f, 180.0f,
                               "%.1f deg");
            ImGui::SeparatorText("Proportional Editing");
            ImGui::Checkbox("Enabled", &meshEditProportionalEditing);
            ImGui::DragFloat("Radius", &meshEditProportionalRadius, 0.01f,
                             0.0001f, 10000.0f, "%.3f");
            ImGui::SliderFloat("Falloff", &meshEditProportionalFalloff, 0.25f,
                               8.0f, "%.2f");
            ImGui::SeparatorText("Transform Pivot");
            ImGui::RadioButton("Median", &meshEditPivotMode, 0);
            ImGui::RadioButton("Bounds Center", &meshEditPivotMode, 1);
            ImGui::RadioButton("Active Element", &meshEditPivotMode, 2);
            ImGui::EndMenu();
          }
          ImGui::Separator();

          auto selectedFaceMaterial = [&]() -> uint32_t {
            if (!meshEditSelectedFaces.empty()) {
              int fi = meshEditSelectedFaces.front();
              if (fi >= 0 &&
                  fi < static_cast<int>(
                           meshEditAsset.faceMaterialIndices.size())) {
                return meshEditAsset.faceMaterialIndices[fi];
              }
            }
            return static_cast<uint32_t>(meshEditActiveMaterialSlot);
          };

          auto computeFaceNormal = [&](const glm::u32vec3 &f,
                                       glm::vec3 &out) -> bool {
            if (f.x >= meshEditAsset.positions.size() ||
                f.y >= meshEditAsset.positions.size() ||
                f.z >= meshEditAsset.positions.size()) {
              return false;
            }
            glm::vec3 n = glm::cross(
                meshEditAsset.positions[f.y] - meshEditAsset.positions[f.x],
                meshEditAsset.positions[f.z] - meshEditAsset.positions[f.x]);
            if (glm::length(n) < 1e-6f)
              return false;
            out = glm::normalize(n);
            return true;
          };

          auto deleteSelectedFaces = [&]() {
            if (meshEditSelectedFaces.empty())
              return false;
            std::vector<uint8_t> remove(meshEditAsset.faces.size(), 0u);
            for (int fi : meshEditSelectedFaces) {
              if (fi >= 0 && fi < static_cast<int>(remove.size()))
                remove[fi] = 1u;
            }
            std::vector<glm::u32vec3> nextFaces;
            std::vector<uint32_t> nextMats;
            nextFaces.reserve(meshEditAsset.faces.size());
            nextMats.reserve(meshEditAsset.faces.size());
            for (size_t fi = 0; fi < meshEditAsset.faces.size(); ++fi) {
              if (remove[fi])
                continue;
              nextFaces.push_back(meshEditAsset.faces[fi]);
              uint32_t mat = (fi < meshEditAsset.faceMaterialIndices.size())
                                 ? meshEditAsset.faceMaterialIndices[fi]
                                 : 0u;
              nextMats.push_back(mat);
            }
            meshEditAsset.faces = std::move(nextFaces);
            meshEditAsset.faceMaterialIndices = std::move(nextMats);
            meshEditSelectedFaces.clear();
            return true;
          };

          auto extrudeSelectedFaces = [&](float amount, bool branchFromEdges) {
            if (meshEditSelectedFaces.empty())
              return false;
            ensureUvs();
            std::unordered_map<uint32_t, uint32_t> duplicateMap;
            std::unordered_map<uint32_t, glm::vec3> moveDirs;
            duplicateMap.reserve(meshEditSelectedFaces.size() * 3);

            std::vector<glm::u32vec3> selectedFaces;
            selectedFaces.reserve(meshEditSelectedFaces.size());
            for (int fi : meshEditSelectedFaces) {
              if (fi >= 0 &&
                  fi < static_cast<int>(meshEditAsset.faces.size())) {
                selectedFaces.push_back(meshEditAsset.faces[fi]);
              }
            }
            if (selectedFaces.empty())
              return false;

            glm::vec3 regionCenter(0.0f);
            int regionVertCount = 0;
            for (const auto &f : selectedFaces) {
              regionCenter += meshEditAsset.positions[f.x];
              regionCenter += meshEditAsset.positions[f.y];
              regionCenter += meshEditAsset.positions[f.z];
              regionVertCount += 3;
            }
            regionCenter /= std::max(1, regionVertCount);

            auto duplicateVertex = [&](uint32_t idx) -> uint32_t {
              auto it = duplicateMap.find(idx);
              if (it != duplicateMap.end())
                return it->second;
              uint32_t newIdx =
                  static_cast<uint32_t>(meshEditAsset.positions.size());
              duplicateMap[idx] = newIdx;
              meshEditAsset.positions.push_back(meshEditAsset.positions[idx]);
              meshEditAsset.normals.push_back(idx < meshEditAsset.normals.size()
                                                  ? meshEditAsset.normals[idx]
                                                  : glm::vec3(0.0f));
              meshEditAsset.uvs.push_back(idx < meshEditAsset.uvs.size()
                                              ? meshEditAsset.uvs[idx]
                                              : glm::vec2(0.0f));
              return newIdx;
            };

            auto buildDirForVertex = [&](uint32_t oldIdx,
                                         const glm::vec3 &faceNormal) {
              glm::vec3 dir = faceNormal;
              if (branchFromEdges) {
                glm::vec3 radial =
                    meshEditAsset.positions[oldIdx] - regionCenter;
                if (glm::length(radial) > 1e-6f) {
                  dir += glm::normalize(radial) * 0.35f;
                }
              }
              if (glm::length(dir) < 1e-6f)
                dir = glm::vec3(0.0f, 0.0f, 1.0f);
              return glm::normalize(dir);
            };

            uint32_t defaultMat = selectedFaceMaterial();
            for (const auto &f : selectedFaces) {
              glm::vec3 n(0.0f);
              computeFaceNormal(f, n);
              const uint32_t oldIdx[3] = {f.x, f.y, f.z};
              uint32_t newIdx[3];
              for (int i = 0; i < 3; ++i) {
                newIdx[i] = duplicateVertex(oldIdx[i]);
                moveDirs[newIdx[i]] += buildDirForVertex(oldIdx[i], n);
              }

              addFaceWithMaterial(glm::u32vec3(newIdx[0], newIdx[1], newIdx[2]),
                                  defaultMat);
              addFaceWithMaterial(glm::u32vec3(oldIdx[0], oldIdx[1], newIdx[1]),
                                  defaultMat);
              addFaceWithMaterial(glm::u32vec3(oldIdx[0], newIdx[1], newIdx[0]),
                                  defaultMat);
              addFaceWithMaterial(glm::u32vec3(oldIdx[1], oldIdx[2], newIdx[2]),
                                  defaultMat);
              addFaceWithMaterial(glm::u32vec3(oldIdx[1], newIdx[2], newIdx[1]),
                                  defaultMat);
              addFaceWithMaterial(glm::u32vec3(oldIdx[2], oldIdx[0], newIdx[0]),
                                  defaultMat);
              addFaceWithMaterial(glm::u32vec3(oldIdx[2], newIdx[0], newIdx[2]),
                                  defaultMat);
            }

            for (const auto &it : moveDirs) {
              uint32_t idx = it.first;
              glm::vec3 dir = it.second;
              if (glm::length(dir) < 1e-6f)
                continue;
              meshEditAsset.positions[idx] += glm::normalize(dir) * amount;
            }

            return true;
          };

          auto subdivideSelectedFaces = [&]() {
            if (meshEditSelectedFaces.empty())
              return false;
            ensureUvs();
            std::unordered_map<uint64_t, uint32_t> edgeMidpoints;
            edgeMidpoints.reserve(meshEditSelectedFaces.size() * 3);
            auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
              return (static_cast<uint64_t>(std::min(a, b)) << 32) |
                     static_cast<uint64_t>(std::max(a, b));
            };

            std::vector<int> selected = meshEditSelectedFaces;
            std::sort(selected.begin(), selected.end());
            std::reverse(selected.begin(), selected.end());

            for (int fi : selected) {
              if (fi < 0 || fi >= static_cast<int>(meshEditAsset.faces.size()))
                continue;
              const auto f = meshEditAsset.faces[fi];
              uint32_t mat =
                  (fi <
                   static_cast<int>(meshEditAsset.faceMaterialIndices.size()))
                      ? meshEditAsset.faceMaterialIndices[fi]
                      : static_cast<uint32_t>(meshEditActiveMaterialSlot);

              auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
                uint64_t key = edgeKey(a, b);
                auto it = edgeMidpoints.find(key);
                if (it != edgeMidpoints.end())
                  return it->second;
                uint32_t idx =
                    static_cast<uint32_t>(meshEditAsset.positions.size());
                meshEditAsset.positions.push_back(
                    (meshEditAsset.positions[a] + meshEditAsset.positions[b]) *
                    0.5f);
                meshEditAsset.normals.push_back(
                    (meshEditAsset.normals[a] + meshEditAsset.normals[b]) *
                    0.5f);
                meshEditAsset.uvs.push_back(
                    (meshEditAsset.uvs[a] + meshEditAsset.uvs[b]) * 0.5f);
                edgeMidpoints[key] = idx;
                return idx;
              };

              uint32_t ab = midpoint(f.x, f.y);
              uint32_t bc = midpoint(f.y, f.z);
              uint32_t ca = midpoint(f.z, f.x);

              meshEditAsset.faces.erase(meshEditAsset.faces.begin() + fi);
              if (fi <
                  static_cast<int>(meshEditAsset.faceMaterialIndices.size())) {
                meshEditAsset.faceMaterialIndices.erase(
                    meshEditAsset.faceMaterialIndices.begin() + fi);
              }

              addFaceWithMaterial(glm::u32vec3(f.x, ab, ca), mat);
              addFaceWithMaterial(glm::u32vec3(ab, f.y, bc), mat);
              addFaceWithMaterial(glm::u32vec3(ca, bc, f.z), mat);
              addFaceWithMaterial(glm::u32vec3(ab, bc, ca), mat);
            }
            return true;
          };

          auto insetSelectedFaces = [&]() {
            if (meshEditSelectedFaces.empty())
              return false;
            ensureUvs();
            std::vector<int> selected = meshEditSelectedFaces;
            std::sort(selected.begin(), selected.end());
            std::reverse(selected.begin(), selected.end());
            const float t = std::clamp(meshEditInsetAmount, 0.01f, 0.95f);

            for (int fi : selected) {
              if (fi < 0 || fi >= static_cast<int>(meshEditAsset.faces.size()))
                continue;
              const auto f = meshEditAsset.faces[fi];
              uint32_t mat =
                  (fi <
                   static_cast<int>(meshEditAsset.faceMaterialIndices.size()))
                      ? meshEditAsset.faceMaterialIndices[fi]
                      : static_cast<uint32_t>(meshEditActiveMaterialSlot);

              glm::vec3 center =
                  (meshEditAsset.positions[f.x] + meshEditAsset.positions[f.y] +
                   meshEditAsset.positions[f.z]) /
                  3.0f;
              glm::vec2 uvCenter =
                  (meshEditAsset.uvs[f.x] + meshEditAsset.uvs[f.y] +
                   meshEditAsset.uvs[f.z]) /
                  3.0f;

              uint32_t ia =
                  static_cast<uint32_t>(meshEditAsset.positions.size());
              uint32_t ib = ia + 1u;
              uint32_t ic = ia + 2u;
              meshEditAsset.positions.push_back(
                  glm::mix(meshEditAsset.positions[f.x], center, t));
              meshEditAsset.positions.push_back(
                  glm::mix(meshEditAsset.positions[f.y], center, t));
              meshEditAsset.positions.push_back(
                  glm::mix(meshEditAsset.positions[f.z], center, t));
              meshEditAsset.normals.push_back(meshEditAsset.normals[f.x]);
              meshEditAsset.normals.push_back(meshEditAsset.normals[f.y]);
              meshEditAsset.normals.push_back(meshEditAsset.normals[f.z]);
              meshEditAsset.uvs.push_back(
                  glm::mix(meshEditAsset.uvs[f.x], uvCenter, t));
              meshEditAsset.uvs.push_back(
                  glm::mix(meshEditAsset.uvs[f.y], uvCenter, t));
              meshEditAsset.uvs.push_back(
                  glm::mix(meshEditAsset.uvs[f.z], uvCenter, t));

              meshEditAsset.faces.erase(meshEditAsset.faces.begin() + fi);
              if (fi <
                  static_cast<int>(meshEditAsset.faceMaterialIndices.size())) {
                meshEditAsset.faceMaterialIndices.erase(
                    meshEditAsset.faceMaterialIndices.begin() + fi);
              }

              addFaceWithMaterial(glm::u32vec3(ia, ib, ic), mat);
              addFaceWithMaterial(glm::u32vec3(f.x, f.y, ib), mat);
              addFaceWithMaterial(glm::u32vec3(f.x, ib, ia), mat);
              addFaceWithMaterial(glm::u32vec3(f.y, f.z, ic), mat);
              addFaceWithMaterial(glm::u32vec3(f.y, ic, ib), mat);
              addFaceWithMaterial(glm::u32vec3(f.z, f.x, ia), mat);
              addFaceWithMaterial(glm::u32vec3(f.z, ia, ic), mat);
            }
            return true;
          };

          auto separateSelectedFaces = [&]() {
            if (meshEditSelectedFaces.empty())
              return false;
            ensureUvs();
            for (int fi : meshEditSelectedFaces) {
              if (fi < 0 || fi >= static_cast<int>(meshEditAsset.faces.size()))
                continue;
              auto &f = meshEditAsset.faces[fi];
              uint32_t ids[3] = {f.x, f.y, f.z};
              uint32_t outIdx[3];
              for (int i = 0; i < 3; ++i) {
                outIdx[i] =
                    static_cast<uint32_t>(meshEditAsset.positions.size());
                meshEditAsset.positions.push_back(
                    meshEditAsset.positions[ids[i]]);
                meshEditAsset.normals.push_back(meshEditAsset.normals[ids[i]]);
                meshEditAsset.uvs.push_back(meshEditAsset.uvs[ids[i]]);
              }
              f = glm::u32vec3(outIdx[0], outIdx[1], outIdx[2]);
            }
            return true;
          };

          auto flipSelectedFaces = [&]() {
            std::string error;
            if (!MeshEditOperations::FlipFaces(
                    meshEditAsset, meshEditSelectedFaces, error)) {
              reportMeshEditFailure("Flip Face", error);
              return false;
            }
            return true;
          };

          auto deleteSelectedVertices = [&]() {
            if (meshEditSelectedVertices.empty())
              return false;
            std::unordered_set<uint32_t> removeSet;
            for (int vi : meshEditSelectedVertices) {
              if (vi >= 0 &&
                  vi < static_cast<int>(meshEditAsset.positions.size())) {
                removeSet.insert(static_cast<uint32_t>(vi));
              }
            }
            if (removeSet.empty())
              return false;
            std::vector<glm::u32vec3> nextFaces;
            std::vector<uint32_t> nextMats;
            for (size_t fi = 0; fi < meshEditAsset.faces.size(); ++fi) {
              const auto &f = meshEditAsset.faces[fi];
              if (removeSet.count(f.x) || removeSet.count(f.y) ||
                  removeSet.count(f.z))
                continue;
              nextFaces.push_back(f);
              uint32_t mat = (fi < meshEditAsset.faceMaterialIndices.size())
                                 ? meshEditAsset.faceMaterialIndices[fi]
                                 : 0u;
              nextMats.push_back(mat);
            }
            meshEditAsset.faces = std::move(nextFaces);
            meshEditAsset.faceMaterialIndices = std::move(nextMats);
            meshEditSelectedVertices.clear();
            return true;
          };

          auto snapSelectedVertices = [&]() {
            if (meshEditSelectedVertices.empty())
              return false;
            float step = std::max(0.001f, meshEditGridSnap);
            for (int vi : meshEditSelectedVertices) {
              if (vi < 0 ||
                  vi >= static_cast<int>(meshEditAsset.positions.size()))
                continue;
              glm::vec3 &p = meshEditAsset.positions[vi];
              p.x = std::round(p.x / step) * step;
              p.y = std::round(p.y / step) * step;
              p.z = std::round(p.z / step) * step;
            }
            return true;
          };

          auto weldSelectedVertices = [&]() {
            if (meshEditSelectedVertices.size() < 2)
              return false;
            glm::vec3 center(0.0f);
            int valid = 0;
            for (int vi : meshEditSelectedVertices) {
              if (vi < 0 ||
                  vi >= static_cast<int>(meshEditAsset.positions.size()))
                continue;
              center += meshEditAsset.positions[vi];
              ++valid;
            }
            if (valid < 2)
              return false;
            center /= static_cast<float>(valid);
            int keep = meshEditSelectedVertices.front();
            if (keep < 0 ||
                keep >= static_cast<int>(meshEditAsset.positions.size()))
              return false;
            meshEditAsset.positions[keep] = center;
            std::unordered_set<int> mergeSet(meshEditSelectedVertices.begin(),
                                             meshEditSelectedVertices.end());
            for (auto &f : meshEditAsset.faces) {
              if (mergeSet.count(static_cast<int>(f.x)))
                f.x = static_cast<uint32_t>(keep);
              if (mergeSet.count(static_cast<int>(f.y)))
                f.y = static_cast<uint32_t>(keep);
              if (mergeSet.count(static_cast<int>(f.z)))
                f.z = static_cast<uint32_t>(keep);
            }
            meshEditSelectedVertices = {keep};
            return true;
          };

          auto splitSelectedVertex = [&]() {
            if (meshEditSelectedVertices.size() != 1)
              return false;
            int vi = meshEditSelectedVertices.front();
            if (vi < 0 ||
                vi >= static_cast<int>(meshEditAsset.positions.size()))
              return false;
            std::vector<int> connectedFaces;
            for (int fi = 0; fi < static_cast<int>(meshEditAsset.faces.size());
                 ++fi) {
              const auto &f = meshEditAsset.faces[fi];
              if (static_cast<int>(f.x) == vi || static_cast<int>(f.y) == vi ||
                  static_cast<int>(f.z) == vi) {
                connectedFaces.push_back(fi);
              }
            }
            if (connectedFaces.size() < 2)
              return false;
            size_t splitStart = connectedFaces.size() / 2;
            for (size_t i = splitStart; i < connectedFaces.size(); ++i) {
              int fi = connectedFaces[i];
              uint32_t newIdx =
                  static_cast<uint32_t>(meshEditAsset.positions.size());
              meshEditAsset.positions.push_back(meshEditAsset.positions[vi]);
              meshEditAsset.normals.push_back(meshEditAsset.normals[vi]);
              meshEditAsset.uvs.push_back(meshEditAsset.uvs[vi]);
              auto &f = meshEditAsset.faces[fi];
              if (static_cast<int>(f.x) == vi)
                f.x = newIdx;
              if (static_cast<int>(f.y) == vi)
                f.y = newIdx;
              if (static_cast<int>(f.z) == vi)
                f.z = newIdx;
            }
            return true;
          };

          auto deleteSelectedEdges = [&]() {
            if (meshEditSelectedEdges.empty())
              return false;
            std::unordered_set<uint64_t> selected;
            selected.reserve(meshEditSelectedEdges.size() * 2);
            auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
              return (static_cast<uint64_t>(std::min(a, b)) << 32) |
                     static_cast<uint64_t>(std::max(a, b));
            };
            for (int ei : meshEditSelectedEdges) {
              if (ei < 0 || ei >= static_cast<int>(edges.size()))
                continue;
              selected.insert(edgeKey(edges[ei].x, edges[ei].y));
            }
            std::vector<glm::u32vec3> nextFaces;
            std::vector<uint32_t> nextMats;
            for (size_t fi = 0; fi < meshEditAsset.faces.size(); ++fi) {
              const auto &f = meshEditAsset.faces[fi];
              uint64_t keys[3] = {edgeKey(f.x, f.y), edgeKey(f.y, f.z),
                                  edgeKey(f.z, f.x)};
              if (selected.count(keys[0]) || selected.count(keys[1]) ||
                  selected.count(keys[2])) {
                continue;
              }
              nextFaces.push_back(f);
              uint32_t mat = (fi < meshEditAsset.faceMaterialIndices.size())
                                 ? meshEditAsset.faceMaterialIndices[fi]
                                 : 0u;
              nextMats.push_back(mat);
            }
            meshEditAsset.faces = std::move(nextFaces);
            meshEditAsset.faceMaterialIndices = std::move(nextMats);
            meshEditSelectedEdges.clear();
            return true;
          };

          auto bridgeSelectedEdges = [&]() {
            if (meshEditSelectedEdges.size() != 2)
              return false;
            ensureUvs();
            int e0 = meshEditSelectedEdges[0];
            int e1 = meshEditSelectedEdges[1];
            if (e0 < 0 || e0 >= static_cast<int>(edges.size()) || e1 < 0 ||
                e1 >= static_cast<int>(edges.size())) {
              return false;
            }
            const auto a = edges[e0];
            const auto b = edges[e1];
            if (a.x == b.x || a.x == b.y || a.y == b.x || a.y == b.y) {
              return false;
            }

            const float mapCost0 = glm::length(meshEditAsset.positions[a.x] -
                                               meshEditAsset.positions[b.x]) +
                                   glm::length(meshEditAsset.positions[a.y] -
                                               meshEditAsset.positions[b.y]);
            const float mapCost1 = glm::length(meshEditAsset.positions[a.x] -
                                               meshEditAsset.positions[b.y]) +
                                   glm::length(meshEditAsset.positions[a.y] -
                                               meshEditAsset.positions[b.x]);

            glm::u32vec3 tri0(0u), tri1(0u);
            if (mapCost0 <= mapCost1) {
              tri0 = glm::u32vec3(a.x, a.y, b.y);
              tri1 = glm::u32vec3(a.x, b.y, b.x);
            } else {
              tri0 = glm::u32vec3(a.x, a.y, b.x);
              tri1 = glm::u32vec3(a.x, b.x, b.y);
            }

            auto edgeKey = [](uint32_t u, uint32_t v) -> uint64_t {
              return (static_cast<uint64_t>(std::min(u, v)) << 32) |
                     static_cast<uint64_t>(std::max(u, v));
            };
            auto triNormal = [&](const glm::u32vec3 &t) {
              if (t.x >= meshEditAsset.positions.size() ||
                  t.y >= meshEditAsset.positions.size() ||
                  t.z >= meshEditAsset.positions.size()) {
                return glm::vec3(0.0f);
              }
              glm::vec3 n = glm::cross(
                  meshEditAsset.positions[t.y] - meshEditAsset.positions[t.x],
                  meshEditAsset.positions[t.z] - meshEditAsset.positions[t.x]);
              float len = glm::length(n);
              return (len > 1e-6f) ? (n / len) : glm::vec3(0.0f);
            };

            glm::vec3 bridgeNormal = triNormal(tri0) + triNormal(tri1);
            if (glm::length(bridgeNormal) < 1e-6f) {
              return false;
            }

            glm::vec3 adjacentNormal(0.0f);
            const uint64_t k0 = edgeKey(a.x, a.y);
            const uint64_t k1 = edgeKey(b.x, b.y);
            for (const auto &f : meshEditAsset.faces) {
              uint64_t keys[3] = {edgeKey(f.x, f.y), edgeKey(f.y, f.z),
                                  edgeKey(f.z, f.x)};
              if (keys[0] == k0 || keys[1] == k0 || keys[2] == k0 ||
                  keys[0] == k1 || keys[1] == k1 || keys[2] == k1) {
                adjacentNormal += triNormal(f);
              }
            }

            if (glm::length(adjacentNormal) > 1e-6f &&
                glm::dot(glm::normalize(bridgeNormal),
                         glm::normalize(adjacentNormal)) < 0.0f) {
              std::swap(tri0.y, tri0.z);
              std::swap(tri1.y, tri1.z);
            }

            uint32_t mat = static_cast<uint32_t>(meshEditActiveMaterialSlot);
            int faceStart = static_cast<int>(meshEditAsset.faces.size());
            addFaceWithMaterial(tri0, mat);
            addFaceWithMaterial(tri1, mat);
            if (meshEditAutoUV || !meshEditAsset.hasUVs) {
              applyPlanarUvToFace(faceStart);
              applyPlanarUvToFace(faceStart + 1);
              meshEditAsset.hasUVs = true;
            }
            meshEditSelectedFaces = {faceStart, faceStart + 1};
            meshEditSelectedEdges.clear();
            meshEditSelectedVertices.clear();
            return true;
          };

          auto fillSelectedEdgeBoundary = [&]() {
            if (meshEditSelectedEdges.size() < 3)
              return false;
            ensureUvs();
            ensureFaceMaterials();

            auto edgeKey = [](uint32_t u, uint32_t v) -> uint64_t {
              return (static_cast<uint64_t>(std::min(u, v)) << 32) |
                     static_cast<uint64_t>(std::max(u, v));
            };
            auto directedEdgeInFace = [](const glm::u32vec3 &f, uint32_t a,
                                         uint32_t b) -> int {
              if ((f.x == a && f.y == b) || (f.y == a && f.z == b) ||
                  (f.z == a && f.x == b))
                return 1;
              if ((f.x == b && f.y == a) || (f.y == b && f.z == a) ||
                  (f.z == b && f.x == a))
                return -1;
              return 0;
            };
            auto cross2 = [](const glm::vec2 &a, const glm::vec2 &b,
                             const glm::vec2 &c) -> float {
              return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            };
            auto pointInTriangle2 = [&](const glm::vec2 &p, const glm::vec2 &a,
                                        const glm::vec2 &b, const glm::vec2 &c,
                                        float eps) -> bool {
              float c1 = cross2(a, b, p);
              float c2 = cross2(b, c, p);
              float c3 = cross2(c, a, p);
              bool hasNeg = (c1 < -eps) || (c2 < -eps) || (c3 < -eps);
              bool hasPos = (c1 > eps) || (c2 > eps) || (c3 > eps);
              return !(hasNeg && hasPos);
            };
            auto projectTo2 = [](const glm::vec3 &p,
                                 const glm::vec3 &n) -> glm::vec2 {
              glm::vec3 an = glm::abs(n);
              if (an.x >= an.y && an.x >= an.z)
                return glm::vec2(p.y, p.z);
              if (an.y >= an.z)
                return glm::vec2(p.x, p.z);
              return glm::vec2(p.x, p.y);
            };

            std::vector<glm::u32vec2> selectedBoundaryEdges;
            selectedBoundaryEdges.reserve(meshEditSelectedEdges.size());
            std::unordered_set<uint64_t> seenEdges;
            seenEdges.reserve(meshEditSelectedEdges.size() * 2);
            for (int ei : meshEditSelectedEdges) {
              if (ei < 0 || ei >= static_cast<int>(edges.size()))
                continue;
              const auto &e = edges[ei];
              if (e.x == e.y)
                continue;
              uint64_t key = edgeKey(e.x, e.y);
              if (!seenEdges.insert(key).second)
                continue;
              selectedBoundaryEdges.push_back(e);
            }
            if (selectedBoundaryEdges.size() < 3)
              return false;

            const int originalFaceCount =
                static_cast<int>(meshEditAsset.faces.size());
            std::unordered_map<uint64_t, std::vector<int>> edgeFaces;
            edgeFaces.reserve(static_cast<size_t>(originalFaceCount) * 3);
            for (int fi = 0; fi < originalFaceCount; ++fi) {
              const auto &f = meshEditAsset.faces[fi];
              edgeFaces[edgeKey(f.x, f.y)].push_back(fi);
              edgeFaces[edgeKey(f.y, f.z)].push_back(fi);
              edgeFaces[edgeKey(f.z, f.x)].push_back(fi);
            }

            std::unordered_map<uint32_t, std::vector<uint32_t>> adjacency;
            adjacency.reserve(selectedBoundaryEdges.size() * 2);
            for (const auto &e : selectedBoundaryEdges) {
              adjacency[e.x].push_back(e.y);
              adjacency[e.y].push_back(e.x);
            }

            std::unordered_set<uint32_t> visitedVerts;
            visitedVerts.reserve(adjacency.size() * 2);
            std::vector<int> createdFaces;
            createdFaces.reserve(selectedBoundaryEdges.size());
            const uint32_t mat =
                static_cast<uint32_t>(meshEditActiveMaterialSlot);

            for (const auto &kv : adjacency) {
              uint32_t startVert = kv.first;
              if (visitedVerts.count(startVert))
                continue;

              std::vector<uint32_t> stack = {startVert};
              std::vector<uint32_t> componentVerts;
              visitedVerts.insert(startVert);
              while (!stack.empty()) {
                uint32_t v = stack.back();
                stack.pop_back();
                componentVerts.push_back(v);
                const auto itAdj = adjacency.find(v);
                if (itAdj == adjacency.end())
                  continue;
                for (uint32_t n : itAdj->second) {
                  if (visitedVerts.insert(n).second) {
                    stack.push_back(n);
                  }
                }
              }
              if (componentVerts.size() < 3)
                continue;

              std::unordered_set<uint32_t> componentSet(componentVerts.begin(),
                                                        componentVerts.end());
              std::vector<glm::u32vec2> componentEdges;
              componentEdges.reserve(componentVerts.size() * 2);
              for (const auto &e : selectedBoundaryEdges) {
                if (componentSet.count(e.x) && componentSet.count(e.y)) {
                  componentEdges.push_back(e);
                }
              }
              if (componentEdges.size() < 3)
                continue;

              bool validCycle =
                  (componentEdges.size() == componentVerts.size());
              if (!validCycle)
                continue;
              for (uint32_t v : componentVerts) {
                int degree = 0;
                const auto &nbrs = adjacency[v];
                for (uint32_t n : nbrs) {
                  if (componentSet.count(n)) {
                    ++degree;
                  }
                }
                if (degree != 2) {
                  validCycle = false;
                  break;
                }
              }
              if (!validCycle)
                continue;

              bool openBoundary = true;
              for (const auto &e : componentEdges) {
                auto itFaces = edgeFaces.find(edgeKey(e.x, e.y));
                if (itFaces == edgeFaces.end() || itFaces->second.size() != 1) {
                  openBoundary = false;
                  break;
                }
              }
              if (!openBoundary)
                continue;

              std::vector<uint32_t> loop;
              loop.reserve(componentVerts.size());
              uint32_t loopStart = *std::min_element(componentVerts.begin(),
                                                     componentVerts.end());
              uint32_t prev = UINT32_MAX;
              uint32_t curr = loopStart;
              for (size_t guard = 0; guard <= componentVerts.size(); ++guard) {
                loop.push_back(curr);
                const auto &allNbrs = adjacency[curr];
                std::vector<uint32_t> nbrs;
                nbrs.reserve(2);
                for (uint32_t n : allNbrs) {
                  if (componentSet.count(n))
                    nbrs.push_back(n);
                }
                if (nbrs.size() != 2) {
                  loop.clear();
                  break;
                }
                uint32_t next = (nbrs[0] != prev) ? nbrs[0] : nbrs[1];
                prev = curr;
                curr = next;
                if (curr == loopStart)
                  break;
                if (std::find(loop.begin(), loop.end(), curr) != loop.end()) {
                  loop.clear();
                  break;
                }
              }
              if (loop.size() != componentVerts.size() || curr != loopStart)
                continue;

              int windingScore = 0;
              for (size_t i = 0; i < loop.size(); ++i) {
                uint32_t a = loop[i];
                uint32_t b = loop[(i + 1) % loop.size()];
                auto itFaces = edgeFaces.find(edgeKey(a, b));
                if (itFaces == edgeFaces.end() || itFaces->second.size() != 1)
                  continue;
                int fi = itFaces->second[0];
                if (fi < 0 || fi >= originalFaceCount)
                  continue;
                windingScore +=
                    directedEdgeInFace(meshEditAsset.faces[fi], a, b);
              }
              if (windingScore > 0) {
                std::reverse(loop.begin(), loop.end());
              }

              glm::vec3 polyNormal(0.0f);
              for (size_t i = 0; i < loop.size(); ++i) {
                const glm::vec3 &p = meshEditAsset.positions[loop[i]];
                const glm::vec3 &q =
                    meshEditAsset.positions[loop[(i + 1) % loop.size()]];
                polyNormal.x += (p.y - q.y) * (p.z + q.z);
                polyNormal.y += (p.z - q.z) * (p.x + q.x);
                polyNormal.z += (p.x - q.x) * (p.y + q.y);
              }
              float polyLen = glm::length(polyNormal);
              if (polyLen < 1e-6f)
                continue;
              polyNormal /= polyLen;

              std::vector<glm::vec2> loop2;
              loop2.reserve(loop.size());
              for (uint32_t v : loop) {
                if (v >= meshEditAsset.positions.size()) {
                  loop2.clear();
                  break;
                }
                loop2.push_back(
                    projectTo2(meshEditAsset.positions[v], polyNormal));
              }
              if (loop2.size() != loop.size())
                continue;

              std::vector<int> ring(loop.size());
              std::iota(ring.begin(), ring.end(), 0);
              auto ringArea = [&](const std::vector<int> &idxs) {
                float a = 0.0f;
                for (size_t i = 0; i < idxs.size(); ++i) {
                  const glm::vec2 &p = loop2[idxs[i]];
                  const glm::vec2 &q = loop2[idxs[(i + 1) % idxs.size()]];
                  a += (p.x * q.y) - (q.x * p.y);
                }
                return a * 0.5f;
              };
              const float area = ringArea(ring);
              if (std::abs(area) < 1e-7f)
                continue;

              std::vector<glm::u32vec3> tris;
              tris.reserve(loop.size() - 2);
              const float eps = 1e-6f;
              bool triangulationFailed = false;
              while (ring.size() > 3) {
                bool foundEar = false;
                for (size_t i = 0; i < ring.size(); ++i) {
                  int iPrev = ring[(i + ring.size() - 1) % ring.size()];
                  int iCurr = ring[i];
                  int iNext = ring[(i + 1) % ring.size()];

                  float turn = cross2(loop2[iPrev], loop2[iCurr], loop2[iNext]);
                  bool convex = (area > 0.0f) ? (turn > eps) : (turn < -eps);
                  if (!convex)
                    continue;

                  bool hasPointInside = false;
                  for (int r : ring) {
                    if (r == iPrev || r == iCurr || r == iNext)
                      continue;
                    if (pointInTriangle2(loop2[r], loop2[iPrev], loop2[iCurr],
                                         loop2[iNext], eps)) {
                      hasPointInside = true;
                      break;
                    }
                  }
                  if (hasPointInside)
                    continue;

                  tris.emplace_back(loop[iPrev], loop[iCurr], loop[iNext]);
                  ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                  foundEar = true;
                  break;
                }
                if (!foundEar) {
                  triangulationFailed = true;
                  break;
                }
              }
              if (triangulationFailed || ring.size() != 3)
                continue;
              tris.emplace_back(loop[ring[0]], loop[ring[1]], loop[ring[2]]);

              int faceStart = static_cast<int>(meshEditAsset.faces.size());
              for (const auto &tri : tris) {
                addFaceWithMaterial(tri, mat);
              }
              for (int fi = faceStart;
                   fi < static_cast<int>(meshEditAsset.faces.size()); ++fi) {
                applyPlanarUvToFace(fi);
                createdFaces.push_back(fi);
              }
              meshEditAsset.hasUVs = true;
            }

            if (createdFaces.empty())
              return false;
            meshEditSelectedFaces = std::move(createdFaces);
            meshEditSelectedEdges.clear();
            meshEditSelectedVertices.clear();
            return true;
          };

          auto splitSelectedEdges = [&]() {
            if (meshEditSelectedEdges.empty())
              return false;
            ensureUvs();
            if (meshEditAsset.normals.size() < meshEditAsset.positions.size()) {
              meshEditAsset.normals.resize(meshEditAsset.positions.size(),
                                           glm::vec3(0.0f));
            }
            std::unordered_set<uint64_t> selected;
            selected.reserve(meshEditSelectedEdges.size() * 2);
            auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
              return (static_cast<uint64_t>(std::min(a, b)) << 32) |
                     static_cast<uint64_t>(std::max(a, b));
            };
            for (int ei : meshEditSelectedEdges) {
              if (ei < 0 || ei >= static_cast<int>(edges.size()))
                continue;
              selected.insert(edgeKey(edges[ei].x, edges[ei].y));
            }
            if (selected.empty())
              return false;
            for (const glm::u32vec3 &face : meshEditAsset.faces) {
              const uint32_t tri[3] = {face.x, face.y, face.z};
              int selectedEdgesInFace = 0;
              for (int edge = 0; edge < 3; ++edge) {
                selectedEdgesInFace +=
                    selected.count(
                        edgeKey(tri[edge], tri[(edge + 1) % 3])) != 0;
              }
              if (selectedEdgesInFace > 1) {
                reportMeshEditFailure(
                    "Subdivide Edge",
                    "Adjacent selected edges in the same triangle are not "
                    "supported by the incremental edge splitter.");
                return false;
              }
            }

            bool changed = false;
            std::unordered_map<uint64_t, uint32_t> midpoints;
            std::vector<int> createdVertices;
            std::vector<glm::u32vec3> rebuiltFaces;
            std::vector<uint32_t> rebuiltMaterials;
            rebuiltFaces.reserve(meshEditAsset.faces.size() * 2);
            rebuiltMaterials.reserve(meshEditAsset.faces.size() * 2);

            auto midpointFor = [&](uint32_t a, uint32_t b) {
              const uint64_t key = edgeKey(a, b);
              auto existing = midpoints.find(key);
              if (existing != midpoints.end())
                return existing->second;
              const uint32_t midpoint =
                  static_cast<uint32_t>(meshEditAsset.positions.size());
              meshEditAsset.positions.push_back(
                  (meshEditAsset.positions[a] + meshEditAsset.positions[b]) *
                  0.5f);
              meshEditAsset.normals.push_back(
                  (meshEditAsset.normals[a] + meshEditAsset.normals[b]) *
                  0.5f);
              meshEditAsset.uvs.push_back(
                  (meshEditAsset.uvs[a] + meshEditAsset.uvs[b]) * 0.5f);
              midpoints.emplace(key, midpoint);
              createdVertices.push_back(static_cast<int>(midpoint));
              return midpoint;
            };

            for (int fi = 0;
                 fi < static_cast<int>(meshEditAsset.faces.size()); ++fi) {
              const glm::u32vec3 f = meshEditAsset.faces[fi];
              const uint32_t tri[3] = {f.x, f.y, f.z};
              const uint32_t mat =
                  (fi <
                   static_cast<int>(meshEditAsset.faceMaterialIndices.size()))
                      ? meshEditAsset.faceMaterialIndices[fi]
                      : static_cast<uint32_t>(meshEditActiveMaterialSlot);
              bool split = false;
              for (int e = 0; e < 3; ++e) {
                const uint32_t a = tri[e];
                const uint32_t b = tri[(e + 1) % 3];
                if (!selected.count(edgeKey(a, b)))
                  continue;
                const uint32_t c = tri[(e + 2) % 3];
                const uint32_t mid = midpointFor(a, b);
                rebuiltFaces.emplace_back(a, mid, c);
                rebuiltMaterials.push_back(mat);
                rebuiltFaces.emplace_back(mid, b, c);
                rebuiltMaterials.push_back(mat);
                changed = true;
                split = true;
                break;
              }
              if (!split) {
                rebuiltFaces.push_back(f);
                rebuiltMaterials.push_back(mat);
              }
            }
            if (changed) {
              meshEditAsset.faces = std::move(rebuiltFaces);
              meshEditAsset.faceMaterialIndices =
                  std::move(rebuiltMaterials);
              meshEditSelectedVertices = std::move(createdVertices);
              meshEditSelectedEdges.clear();
              meshEditSelectedFaces.clear();
            }
            return changed;
          };

          auto transformSelectedUvs = [&](const glm::vec2 &move, float scale,
                                          float rotateDeg, bool flipU,
                                          bool flipV, bool reset, bool fit) {
            if (meshEditSelectedFaces.empty())
              return false;
            ensureUvs();
            std::vector<uint32_t> verts;
            verts.reserve(meshEditSelectedFaces.size() * 3);
            auto pushUnique = [&](uint32_t v) {
              if (std::find(verts.begin(), verts.end(), v) == verts.end())
                verts.push_back(v);
            };
            for (int fi : meshEditSelectedFaces) {
              if (fi < 0 || fi >= static_cast<int>(meshEditAsset.faces.size()))
                continue;
              const auto &f = meshEditAsset.faces[fi];
              pushUnique(f.x);
              pushUnique(f.y);
              pushUnique(f.z);
            }
            if (verts.empty())
              return false;

            if (reset) {
              for (int fi : meshEditSelectedFaces) {
                applyPlanarUvToFace(fi);
              }
              return true;
            }

            glm::vec2 pivot(0.0f);
            for (uint32_t v : verts)
              pivot += meshEditAsset.uvs[v];
            pivot /= static_cast<float>(verts.size());

            const float radians = glm::radians(rotateDeg);
            const float cs = std::cos(radians);
            const float sn = std::sin(radians);

            for (uint32_t v : verts) {
              glm::vec2 uv = meshEditAsset.uvs[v];
              uv -= pivot;
              uv *= scale;
              if (flipU)
                uv.x *= -1.0f;
              if (flipV)
                uv.y *= -1.0f;
              uv = glm::vec2(uv.x * cs - uv.y * sn, uv.x * sn + uv.y * cs);
              uv += pivot + move;
              meshEditAsset.uvs[v] = uv;
            }

            if (fit) {
              glm::vec2 minUv(FLT_MAX), maxUv(-FLT_MAX);
              for (uint32_t v : verts) {
                minUv = glm::min(minUv, meshEditAsset.uvs[v]);
                maxUv = glm::max(maxUv, meshEditAsset.uvs[v]);
              }
              glm::vec2 span = maxUv - minUv;
              for (uint32_t v : verts) {
                glm::vec2 uv = meshEditAsset.uvs[v];
                uv.x = span.x > 1e-6f ? (uv.x - minUv.x) / span.x : 0.0f;
                uv.y = span.y > 1e-6f ? (uv.y - minUv.y) / span.y : 0.0f;
                meshEditAsset.uvs[v] = uv;
              }
            }
            meshEditAsset.hasUVs = true;
            return true;
          };

          bool changed = false;
          meshEditCommandSnapshot = captureSceneSnapshot();
          meshEditCommandSnapshotValid = true;
          if (meshEditSelectionMode == MeshEditSelectionMode::Face) {
            if (ImGui::MenuItem("Extrude")) {
              changed = extrudeSelectedFaces(
                  std::max(0.001f, meshEditExtrudeAmount), false);
              if (changed)
                commitMeshEdit("Face Extrude");
            }
            if (ImGui::MenuItem("Inset Face")) {
              changed = insetSelectedFaces();
              if (changed)
                commitMeshEdit("Face Inset");
            }
            if (ImGui::MenuItem("Subdivide")) {
              changed = subdivideSelectedFaces();
              if (changed)
                commitMeshEdit("Face Subdivide");
            }
            if (ImGui::MenuItem("Merge Faces")) {
              if (meshEditSelectedFaces.size() == 2) {
                // Lightweight merge: flip triangulation across shared quad.
                int fa = meshEditSelectedFaces[0];
                int fb = meshEditSelectedFaces[1];
                if (fa >= 0 && fb >= 0 &&
                    fa < static_cast<int>(meshEditAsset.faces.size()) &&
                    fb < static_cast<int>(meshEditAsset.faces.size())) {
                  auto a = meshEditAsset.faces[fa];
                  auto b = meshEditAsset.faces[fb];
                  std::vector<uint32_t> all = {a.x, a.y, a.z, b.x, b.y, b.z};
                  std::sort(all.begin(), all.end());
                  all.erase(std::unique(all.begin(), all.end()), all.end());
                  if (all.size() == 4) {
                    uint32_t mat =
                        (fa < static_cast<int>(
                                  meshEditAsset.faceMaterialIndices.size()))
                            ? meshEditAsset.faceMaterialIndices[fa]
                            : static_cast<uint32_t>(meshEditActiveMaterialSlot);
                    meshEditAsset.faces[fa] =
                        glm::u32vec3(all[0], all[1], all[2]);
                    meshEditAsset.faces[fb] =
                        glm::u32vec3(all[0], all[2], all[3]);
                    if (fa < static_cast<int>(
                                 meshEditAsset.faceMaterialIndices.size()))
                      meshEditAsset.faceMaterialIndices[fa] = mat;
                    if (fb < static_cast<int>(
                                 meshEditAsset.faceMaterialIndices.size()))
                      meshEditAsset.faceMaterialIndices[fb] = mat;
                    changed = true;
                  }
                }
              }
              if (changed)
                commitMeshEdit("Face Merge");
            }
            if (ImGui::MenuItem("Separate Face")) {
              changed = separateSelectedFaces();
              if (changed)
                commitMeshEdit("Face Separate");
            }
            if (ImGui::MenuItem("Flip Face")) {
              changed = flipSelectedFaces();
              if (changed)
                commitMeshEdit("Face Flip", false);
            }
            if (ImGui::MenuItem("Recalculate Normals Outside")) {
              std::string error;
              changed = MeshEditOperations::RecalculateNormals(
                  meshEditAsset, false, error);
              if (changed)
                commitMeshEdit("Normals Outside", false);
              else
                reportMeshEditFailure("Recalculate Normals Outside", error);
            }
            if (ImGui::MenuItem("Recalculate Normals Inside")) {
              std::string error;
              changed = MeshEditOperations::RecalculateNormals(
                  meshEditAsset, true, error);
              if (changed)
                commitMeshEdit("Normals Inside", false);
              else
                reportMeshEditFailure("Recalculate Normals Inside", error);
            }
            if (ImGui::MenuItem("Triangulate Selected Faces")) {
              addConsoleMessage(
                  "Mesh edit: selected faces are already triangles; the "
                  "current RMesh format stores only triangle faces.",
                  ConsoleMessageType::Info);
            }
            if (ImGui::BeginMenu("Flatten Selected")) {
              auto flattenSelectedFaces =
                  [&](MeshEditOperations::FlattenMode mode,
                      const char *actionName) {
                    std::vector<int> vertices;
                    for (int fi : meshEditSelectedFaces) {
                      if (fi < 0 ||
                          fi >= static_cast<int>(meshEditAsset.faces.size()))
                        continue;
                      const glm::u32vec3 &face = meshEditAsset.faces[fi];
                      const uint32_t tri[3] = {face.x, face.y, face.z};
                      for (uint32_t vertex : tri) {
                        if (std::find(vertices.begin(), vertices.end(),
                                      static_cast<int>(vertex)) ==
                            vertices.end()) {
                          vertices.push_back(static_cast<int>(vertex));
                        }
                      }
                    }
                    std::string error;
                    changed = MeshEditOperations::FlattenVertices(
                        meshEditAsset, vertices, mode, error);
                    if (changed)
                      commitMeshEdit(actionName, false);
                    else
                      reportMeshEditFailure(actionName, error);
                  };
              if (ImGui::MenuItem("X Axis"))
                flattenSelectedFaces(MeshEditOperations::FlattenMode::X,
                                     "Face Flatten X");
              if (ImGui::MenuItem("Y Axis"))
                flattenSelectedFaces(MeshEditOperations::FlattenMode::Y,
                                     "Face Flatten Y");
              if (ImGui::MenuItem("Z Axis"))
                flattenSelectedFaces(MeshEditOperations::FlattenMode::Z,
                                     "Face Flatten Z");
              if (ImGui::MenuItem("Average Plane"))
                flattenSelectedFaces(
                    MeshEditOperations::FlattenMode::AveragePlane,
                    "Face Flatten Plane");
              ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Branch Face Edges")) {
              changed = extrudeSelectedFaces(
                  std::max(0.001f, meshEditExtrudeAmount), true);
              if (changed)
                commitMeshEdit("Face Branch");
            }
            if (ImGui::MenuItem("Duplicate Faces")) {
              if (!meshEditSelectedFaces.empty()) {
                ensureUvs();
                const bool copyNormals =
                    meshEditAsset.hasNormals &&
                    meshEditAsset.normals.size() == meshEditAsset.positions.size();
                std::unordered_map<uint32_t, uint32_t> duplicateMap;
                std::vector<int> duplicatedFaces;
                duplicatedFaces.reserve(meshEditSelectedFaces.size());
                auto duplicateVert = [&](uint32_t v) -> uint32_t {
                  auto found = duplicateMap.find(v);
                  if (found != duplicateMap.end()) return found->second;
                  const uint32_t next =
                      static_cast<uint32_t>(meshEditAsset.positions.size());
                  meshEditAsset.positions.push_back(meshEditAsset.positions[v]);
                  if (copyNormals) {
                    meshEditAsset.normals.push_back(meshEditAsset.normals[v]);
                  }
                  meshEditAsset.uvs.push_back(
                      v < meshEditAsset.uvs.size() ? meshEditAsset.uvs[v]
                                                   : glm::vec2(0.0f));
                  duplicateMap.emplace(v, next);
                  return next;
                };
                for (int fi : meshEditSelectedFaces) {
                  if (fi < 0 ||
                      fi >= static_cast<int>(meshEditAsset.faces.size())) {
                    continue;
                  }
                  const glm::u32vec3 face = meshEditAsset.faces[fi];
                  if (face.x >= meshEditAsset.positions.size() ||
                      face.y >= meshEditAsset.positions.size() ||
                      face.z >= meshEditAsset.positions.size()) {
                    continue;
                  }
                  const glm::u32vec3 copy(duplicateVert(face.x),
                                          duplicateVert(face.y),
                                          duplicateVert(face.z));
                  const uint32_t mat =
                      (fi < static_cast<int>(
                                meshEditAsset.faceMaterialIndices.size()))
                          ? meshEditAsset.faceMaterialIndices[fi]
                          : static_cast<uint32_t>(meshEditActiveMaterialSlot);
                  duplicatedFaces.push_back(
                      static_cast<int>(meshEditAsset.faces.size()));
                  addFaceWithMaterial(copy, mat);
                }
                if (!duplicatedFaces.empty()) {
                  meshEditSelectedFaces = std::move(duplicatedFaces);
                  changed = true;
                }
              }
              if (changed)
                commitMeshEdit("Face Duplicate");
            }
            if (ImGui::MenuItem("Delete Face")) {
              changed = deleteSelectedFaces();
              if (changed)
                commitMeshEdit("Face Delete");
            }
          } else if (meshEditSelectionMode == MeshEditSelectionMode::Edge) {
            if (ImGui::MenuItem("Bevel Edge")) {
              reportMeshEditFailure(
                  "Bevel Edge",
                  "The current triangle-only RMesh topology cannot represent "
                  "a stable bevel strip yet. No geometry was changed.");
            }
            if (ImGui::MenuItem("Subdivide Edge")) {
              changed = splitSelectedEdges();
              if (changed) {
                meshEditSelectionMode = MeshEditSelectionMode::Vertex;
                commitMeshEdit("Edge Subdivide");
              }
            }
            if (ImGui::MenuItem("Bridge Edges")) {
              changed = bridgeSelectedEdges();
              if (changed)
                commitMeshEdit("Edge Bridge");
            }
            if (ImGui::MenuItem("Fill Face")) {
              changed = fillSelectedEdgeBoundary();
              if (changed)
                commitMeshEdit("Edge Fill");
            }
            if (ImGui::MenuItem("Split Edge")) {
              changed = splitSelectedEdges();
              if (changed)
                commitMeshEdit("Edge Split");
            }
            if (ImGui::MenuItem("Delete Edge")) {
              changed = deleteSelectedEdges();
              if (changed)
                commitMeshEdit("Edge Delete");
            }
          } else if (meshEditSelectionMode == MeshEditSelectionMode::Vertex) {
            if (ImGui::MenuItem("Merge Vertices at Center")) {
              changed = weldSelectedVertices();
              if (changed)
                commitMeshEdit("Vertex Merge Center", false);
            }
            if (ImGui::MenuItem("Weld Vertices by Distance")) {
              std::string error;
              std::vector<int> weldedSelection;
              changed = MeshEditOperations::WeldVerticesByDistance(
                  meshEditAsset, meshEditSelectedVertices,
                  meshEditWeldDistance, weldedSelection, error);
              if (changed) {
                meshEditSelectedVertices = std::move(weldedSelection);
                meshEditSelectedEdges.clear();
                meshEditSelectedFaces.clear();
                commitMeshEdit("Vertex Weld Distance", false);
              } else {
                reportMeshEditFailure("Weld Vertices by Distance", error);
              }
            }
            if (ImGui::MenuItem("Dissolve Vertices")) {
              std::string error;
              changed = MeshEditOperations::DissolveVertices(
                  meshEditAsset, meshEditSelectedVertices, error);
              if (changed) {
                meshEditSelectedVertices.clear();
                meshEditSelectedEdges.clear();
                meshEditSelectedFaces.clear();
                commitMeshEdit("Vertex Dissolve", false);
              } else {
                reportMeshEditFailure("Dissolve Vertices", error);
              }
            }
            if (ImGui::MenuItem("Split Vertex")) {
              changed = splitSelectedVertex();
              if (changed)
                commitMeshEdit("Vertex Split");
            }
            if (ImGui::MenuItem("Delete Vertex")) {
              changed = deleteSelectedVertices();
              if (changed)
                commitMeshEdit("Vertex Delete");
            }
            if (ImGui::MenuItem("Snap To Grid")) {
              changed = snapSelectedVertices();
              if (changed)
                commitMeshEdit("Vertex Snap");
            }
            if (ImGui::MenuItem("Smooth Vertices")) {
              std::string error;
              changed = MeshEditOperations::SmoothVertices(
                  meshEditAsset, meshEditSelectedVertices,
                  meshEditSmoothFactor, meshEditSmoothIterations, error);
              if (changed)
                commitMeshEdit("Vertex Smooth", false);
              else
                reportMeshEditFailure("Smooth Vertices", error);
            }
            if (ImGui::BeginMenu("Flatten Selected")) {
              auto flattenSelectedVertices =
                  [&](MeshEditOperations::FlattenMode mode,
                      const char *actionName) {
                    std::string error;
                    changed = MeshEditOperations::FlattenVertices(
                        meshEditAsset, meshEditSelectedVertices, mode, error);
                    if (changed)
                      commitMeshEdit(actionName, false);
                    else
                      reportMeshEditFailure(actionName, error);
                  };
              if (ImGui::MenuItem("X Axis"))
                flattenSelectedVertices(MeshEditOperations::FlattenMode::X,
                                        "Vertex Flatten X");
              if (ImGui::MenuItem("Y Axis"))
                flattenSelectedVertices(MeshEditOperations::FlattenMode::Y,
                                        "Vertex Flatten Y");
              if (ImGui::MenuItem("Z Axis"))
                flattenSelectedVertices(MeshEditOperations::FlattenMode::Z,
                                        "Vertex Flatten Z");
              if (ImGui::MenuItem("Average Plane"))
                flattenSelectedVertices(
                    MeshEditOperations::FlattenMode::AveragePlane,
                    "Vertex Flatten Plane");
              ImGui::EndMenu();
            }
          } else if (meshEditSelectionMode == MeshEditSelectionMode::UV) {
            if (ImGui::MenuItem("Move UVs")) {
              changed =
                  transformSelectedUvs(glm::vec2(meshEditUvMoveStep, 0.0f),
                                       1.0f, 0.0f, false, false, false, false);
              if (changed)
                commitMeshEdit("UV Move");
            }
            if (ImGui::MenuItem("Scale UVs")) {
              changed =
                  transformSelectedUvs(glm::vec2(0.0f), meshEditUvScaleStep,
                                       0.0f, false, false, false, false);
              if (changed)
                commitMeshEdit("UV Scale");
            }
            if (ImGui::MenuItem("Rotate UVs")) {
              changed = transformSelectedUvs(glm::vec2(0.0f), 1.0f,
                                             meshEditUvRotateStep, false, false,
                                             false, false);
              if (changed)
                commitMeshEdit("UV Rotate");
            }
            if (ImGui::MenuItem("Flip UVs U")) {
              changed = transformSelectedUvs(glm::vec2(0.0f), 1.0f, 0.0f, true,
                                             false, false, false);
              if (changed)
                commitMeshEdit("UV Flip U");
            }
            if (ImGui::MenuItem("Flip UVs V")) {
              changed = transformSelectedUvs(glm::vec2(0.0f), 1.0f, 0.0f, false,
                                             true, false, false);
              if (changed)
                commitMeshEdit("UV Flip V");
            }
            if (ImGui::MenuItem("Reset UVs")) {
              changed = transformSelectedUvs(glm::vec2(0.0f), 1.0f, 0.0f, false,
                                             false, true, false);
              if (changed)
                commitMeshEdit("UV Reset");
            }
            if (ImGui::MenuItem("Fit UVs to Region")) {
              changed = transformSelectedUvs(glm::vec2(0.0f), 1.0f, 0.0f, false,
                                             false, false, true);
              if (changed)
                commitMeshEdit("UV Fit");
            }
          }

          if (meshEditSelectionMode == MeshEditSelectionMode::Vertex ||
              meshEditSelectionMode == MeshEditSelectionMode::Edge ||
              meshEditSelectionMode == MeshEditSelectionMode::Face) {
            ImGui::Separator();
            const bool vertexMode =
                meshEditSelectionMode == MeshEditSelectionMode::Vertex;
            const bool edgeMode =
                meshEditSelectionMode == MeshEditSelectionMode::Edge;
            std::vector<int> &selection =
                vertexMode ? meshEditSelectedVertices
                : edgeMode ? meshEditSelectedEdges
                           : meshEditSelectedFaces;
            const int elementCount =
                vertexMode ? static_cast<int>(meshEditAsset.positions.size())
                : edgeMode ? static_cast<int>(edges.size())
                           : static_cast<int>(meshEditAsset.faces.size());
            auto elementVerts = [&](int idx, uint32_t out[3]) -> int {
              if (idx < 0 || idx >= elementCount) return 0;
              if (vertexMode) {
                out[0] = static_cast<uint32_t>(idx);
                return 1;
              }
              if (edgeMode) {
                out[0] = edges[idx].x;
                out[1] = edges[idx].y;
                return 2;
              }
              const glm::u32vec3 &f = meshEditAsset.faces[idx];
              if (f.x >= meshEditAsset.positions.size() ||
                  f.y >= meshEditAsset.positions.size() ||
                  f.z >= meshEditAsset.positions.size()) {
                return 0;
              }
              out[0] = f.x;
              out[1] = f.y;
              out[2] = f.z;
              return 3;
            };
            auto selectedVertexSet = [&]() {
              std::unordered_set<uint32_t> verts;
              uint32_t buffer[3];
              for (int idx : selection) {
                const int count = elementVerts(idx, buffer);
                for (int i = 0; i < count; ++i) verts.insert(buffer[i]);
              }
              return verts;
            };
            auto buildVertexAdjacency = [&]() {
              std::vector<std::vector<uint32_t>> adjacency(
                  meshEditAsset.positions.size());
              for (const glm::u32vec2 &edge : edges) {
                if (edge.x < adjacency.size() && edge.y < adjacency.size()) {
                  adjacency[edge.x].push_back(edge.y);
                  adjacency[edge.y].push_back(edge.x);
                }
              }
              return adjacency;
            };

            if (ImGui::MenuItem("Select All")) {
              selection.clear();
              selection.reserve(elementCount);
              for (int i = 0; i < elementCount; ++i) selection.push_back(i);
            }
            if (ImGui::MenuItem("Deselect All")) {
              selection.clear();
            }
            if (edgeMode &&
                ImGui::MenuItem("Select Boundary Edges", nullptr, false,
                                !edges.empty())) {
              std::string error;
              if (!MeshEditOperations::SelectBoundaryEdges(
                      meshEditAsset, edges, meshEditSelectedEdges, error)) {
                reportMeshEditFailure("Select Boundary Edges", error);
              }
            }
            if (!vertexMode && !edgeMode &&
                ImGui::MenuItem("Select Coplanar Faces", nullptr, false,
                                !meshEditSelectedFaces.empty())) {
              std::string error;
              std::vector<int> matches;
              if (MeshEditOperations::SelectSimilarFaces(
                      meshEditAsset, meshEditSelectedFaces,
                      meshEditSimilarFaceAngle, true, matches, error)) {
                meshEditSelectedFaces = std::move(matches);
              } else {
                reportMeshEditFailure("Select Coplanar Faces", error);
              }
            }
            if (!vertexMode && !edgeMode &&
                ImGui::MenuItem("Select Similar Faces", nullptr, false,
                                !meshEditSelectedFaces.empty())) {
              std::string error;
              std::vector<int> matches;
              if (MeshEditOperations::SelectSimilarFaces(
                      meshEditAsset, meshEditSelectedFaces,
                      meshEditSimilarFaceAngle, false, matches, error)) {
                meshEditSelectedFaces = std::move(matches);
              } else {
                reportMeshEditFailure("Select Similar Faces", error);
              }
            }
            if (ImGui::MenuItem("Select Connected", nullptr, false,
                                !selection.empty())) {
              const auto adjacency = buildVertexAdjacency();
              std::unordered_set<uint32_t> reachable = selectedVertexSet();
              std::vector<uint32_t> stack(reachable.begin(), reachable.end());
              while (!stack.empty()) {
                const uint32_t current = stack.back();
                stack.pop_back();
                if (current >= adjacency.size()) continue;
                for (uint32_t next : adjacency[current]) {
                  if (reachable.insert(next).second) stack.push_back(next);
                }
              }
              selection.clear();
              uint32_t buffer[3];
              for (int i = 0; i < elementCount; ++i) {
                const int count = elementVerts(i, buffer);
                if (count == 0) continue;
                if (reachable.count(buffer[0]) > 0) selection.push_back(i);
              }
            }
            if (ImGui::MenuItem("Grow Selection", nullptr, false,
                                !selection.empty())) {
              const auto adjacency = buildVertexAdjacency();
              std::unordered_set<uint32_t> verts = selectedVertexSet();
              std::unordered_set<uint32_t> grown = verts;
              for (uint32_t v : verts) {
                if (v >= adjacency.size()) continue;
                for (uint32_t next : adjacency[v]) grown.insert(next);
              }
              selection.clear();
              uint32_t buffer[3];
              for (int i = 0; i < elementCount; ++i) {
                const int count = elementVerts(i, buffer);
                if (count == 0) continue;
                bool anyIn = false;
                for (int k = 0; k < count; ++k) {
                  if (grown.count(buffer[k]) > 0) {
                    anyIn = true;
                    break;
                  }
                }
                if (vertexMode ? grown.count(buffer[0]) > 0 : anyIn) {
                  selection.push_back(i);
                }
              }
            }
            if (ImGui::MenuItem("Shrink Selection", nullptr, false,
                                !selection.empty())) {
              const auto adjacency = buildVertexAdjacency();
              const std::unordered_set<uint32_t> verts = selectedVertexSet();
              std::vector<int> kept;
              kept.reserve(selection.size());
              uint32_t buffer[3];
              for (int idx : selection) {
                const int count = elementVerts(idx, buffer);
                if (count == 0) continue;
                bool interior = true;
                for (int k = 0; k < count && interior; ++k) {
                  if (buffer[k] >= adjacency.size()) continue;
                  for (uint32_t next : adjacency[buffer[k]]) {
                    if (verts.count(next) == 0) {
                      interior = false;
                      break;
                    }
                  }
                }
                if (interior) kept.push_back(idx);
              }
              selection = std::move(kept);
            }
          }

          ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);

        static bool meshEditHistoryCaptured = false;
        static bool meshEditWasUsing = false;
        static bool meshEditExtruding = false;
        static bool meshEditAwaitMouseRelease = false;
        static std::vector<int> meshEditExtrudeVerts;
        static std::vector<int> meshEditExtrudeFaces;

        if (!baseAffectedVerts.empty()) {
          glm::vec3 pivotWorld(0.0f);
          if (meshEditPivotMode == 1) {
            glm::vec3 boundsMin(FLT_MAX);
            glm::vec3 boundsMax(-FLT_MAX);
            for (int idx : baseAffectedVerts) {
              const glm::vec3 world = glm::vec3(
                  modelMatrix *
                  glm::vec4(meshEditAsset.positions[idx], 1.0f));
              boundsMin = glm::min(boundsMin, world);
              boundsMax = glm::max(boundsMax, world);
            }
            pivotWorld = (boundsMin + boundsMax) * 0.5f;
          } else if (meshEditPivotMode == 2) {
            std::vector<int> activeVertices;
            if (meshEditSelectionMode == MeshEditSelectionMode::Vertex &&
                !meshEditSelectedVertices.empty()) {
              activeVertices.push_back(meshEditSelectedVertices.back());
            } else if (meshEditSelectionMode == MeshEditSelectionMode::Edge &&
                       !meshEditSelectedEdges.empty()) {
              const int edgeIndex = meshEditSelectedEdges.back();
              if (edgeIndex >= 0 &&
                  edgeIndex < static_cast<int>(edges.size())) {
                activeVertices = {static_cast<int>(edges[edgeIndex].x),
                                  static_cast<int>(edges[edgeIndex].y)};
              }
            } else if ((meshEditSelectionMode ==
                            MeshEditSelectionMode::Face ||
                        meshEditSelectionMode == MeshEditSelectionMode::UV) &&
                       !meshEditSelectedFaces.empty()) {
              const int faceIndex = meshEditSelectedFaces.back();
              if (faceIndex >= 0 &&
                  faceIndex <
                      static_cast<int>(meshEditAsset.faces.size())) {
                const glm::u32vec3 &face = meshEditAsset.faces[faceIndex];
                activeVertices = {static_cast<int>(face.x),
                                  static_cast<int>(face.y),
                                  static_cast<int>(face.z)};
              }
            }
            for (int idx : activeVertices) {
              pivotWorld += glm::vec3(
                  modelMatrix *
                  glm::vec4(meshEditAsset.positions[idx], 1.0f));
            }
            if (!activeVertices.empty()) {
              pivotWorld /= static_cast<float>(activeVertices.size());
            } else {
              meshEditPivotMode = 0;
            }
          }
          if (meshEditPivotMode == 0) {
            for (int idx : baseAffectedVerts) {
              glm::vec3 wp = glm::vec3(
                  modelMatrix *
                  glm::vec4(meshEditAsset.positions[idx], 1.0f));
              pivotWorld += wp;
            }
            pivotWorld /= static_cast<float>(baseAffectedVerts.size());
          }

          glm::mat4 gizmoMat = glm::translate(glm::mat4(1.0f), pivotWorld);
          if (mCurrentGizmoMode == ImGuizmo::LOCAL) {
            gizmoMat = glm::rotate(gizmoMat,
                                   glm::radians(selectedObj->rotation.x),
                                   glm::vec3(1, 0, 0));
            gizmoMat = glm::rotate(gizmoMat,
                                   glm::radians(selectedObj->rotation.y),
                                   glm::vec3(0, 1, 0));
            gizmoMat = glm::rotate(gizmoMat,
                                   glm::radians(selectedObj->rotation.z),
                                   glm::vec3(0, 0, 1));
          }

          ImGuizmo::OPERATION componentOperation = mCurrentGizmoOperation;
          if (componentOperation != ImGuizmo::TRANSLATE &&
              componentOperation != ImGuizmo::ROTATE &&
              componentOperation != ImGuizmo::SCALE) {
            componentOperation = ImGuizmo::TRANSLATE;
          }
          float componentSnap[3] = {snapValue[0], snapValue[1], snapValue[2]};
          float componentRotateSnap[3] = {
              rotationSnapValue, rotationSnapValue, rotationSnapValue};
          const float *componentSnapPtr = nullptr;
          if (useSnap) {
            componentSnapPtr = componentOperation == ImGuizmo::ROTATE
                                   ? componentRotateSnap
                                   : componentSnap;
          }
          glm::mat4 componentDelta(1.0f);

          ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                               componentOperation, mCurrentGizmoMode,
                               glm::value_ptr(gizmoMat),
                               glm::value_ptr(componentDelta),
                               componentSnapPtr);

          if (meshSelectionChangedThisFrame && !meshEditWasUsing &&
              ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Prevent a select click from being interpreted as an immediate
            // transform start.
            meshEditAwaitMouseRelease = true;
          }
          if (meshEditAwaitMouseRelease &&
              !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            meshEditAwaitMouseRelease = false;
          }

          const bool suppressTransformOnSelectionFrame =
              meshSelectionChangedThisFrame && !meshEditWasUsing;
          const bool suppressTransformUntilRelease =
              meshEditAwaitMouseRelease && !meshEditWasUsing;
          bool usingNow = (suppressTransformOnSelectionFrame ||
                           suppressTransformUntilRelease)
                              ? false
                              : ImGuizmo::IsUsing();
          if (usingNow && !meshEditWasUsing) {
            if (!meshEditHistoryCaptured) {
              recordState("meshEdit");
              meshEditHistoryCaptured = true;
            }
            ensureFaceMaterials();
            bool wantsExtrude =
                componentOperation == ImGuizmo::TRANSLATE &&
                (meshEditExtrudeMode || ImGui::GetIO().KeyShift);
            bool seams = ImGui::GetIO().KeyShift && ImGui::GetIO().KeyCtrl;
            meshEditExtruding = false;
            meshEditExtrudeVerts.clear();
            meshEditExtrudeFaces.clear();
            int originalVertexCount =
                static_cast<int>(meshEditAsset.positions.size());
            int newFaceStart = -1;

            auto duplicateVertex = [&](uint32_t idx) -> uint32_t {
              uint32_t newIdx =
                  static_cast<uint32_t>(meshEditAsset.positions.size());
              meshEditAsset.positions.push_back(meshEditAsset.positions[idx]);
              if (idx < meshEditAsset.normals.size()) {
                meshEditAsset.normals.push_back(meshEditAsset.normals[idx]);
              } else {
                meshEditAsset.normals.push_back(glm::vec3(0.0f));
              }
              if (idx < meshEditAsset.uvs.size()) {
                meshEditAsset.uvs.push_back(meshEditAsset.uvs[idx]);
              } else {
                meshEditAsset.uvs.push_back(glm::vec2(0.0f));
              }
              return newIdx;
            };
            [[maybe_unused]] auto rebuildAffectedVerts = [&]() {
              baseAffectedVerts.clear();
              if (meshEditSelectionMode == MeshEditSelectionMode::Vertex) {
                baseAffectedVerts = meshEditSelectedVertices;
              }
              auto pushUnique = [&](int idx) {
                if (idx < 0)
                  return;
                if (std::find(baseAffectedVerts.begin(),
                              baseAffectedVerts.end(),
                              idx) == baseAffectedVerts.end()) {
                  baseAffectedVerts.push_back(idx);
                }
              };
              if (meshEditSelectionMode == MeshEditSelectionMode::Edge) {
                for (int ei : meshEditSelectedEdges) {
                  if (ei < 0 || ei >= (int)edges.size())
                    continue;
                  pushUnique(edges[ei].x);
                  pushUnique(edges[ei].y);
                }
              } else if (meshEditSelectionMode == MeshEditSelectionMode::Face ||
                         meshEditSelectionMode == MeshEditSelectionMode::UV) {
                for (int fi : meshEditSelectedFaces) {
                  if (fi < 0 || fi >= (int)meshEditAsset.faces.size())
                    continue;
                  const auto &f = meshEditAsset.faces[fi];
                  pushUnique(f.x);
                  pushUnique(f.y);
                  pushUnique(f.z);
                }
              }
            };
            auto pushExtrudeVert = [&](int idx) {
              if (std::find(meshEditExtrudeVerts.begin(),
                            meshEditExtrudeVerts.end(),
                            idx) == meshEditExtrudeVerts.end()) {
                meshEditExtrudeVerts.push_back(idx);
              }
            };
            auto ensureUvs = [&]() {
              if (meshEditAsset.uvs.size() < meshEditAsset.positions.size()) {
                meshEditAsset.uvs.resize(meshEditAsset.positions.size(),
                                         glm::vec2(0.0f));
              }
            };
            auto applyPlanarUV = [&](const glm::u32vec3 &face) -> bool {
              if (face.x >= meshEditAsset.positions.size() ||
                  face.y >= meshEditAsset.positions.size() ||
                  face.z >= meshEditAsset.positions.size()) {
                return false;
              }
              const glm::vec3 &a = meshEditAsset.positions[face.x];
              const glm::vec3 &b = meshEditAsset.positions[face.y];
              const glm::vec3 &c = meshEditAsset.positions[face.z];
              glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
              glm::vec2 ua(a.x, a.y), ub(b.x, b.y), uc(c.x, c.y);
              if (std::abs(n.x) >= std::abs(n.y) &&
                  std::abs(n.x) >= std::abs(n.z)) {
                ua = glm::vec2(a.y, a.z);
                ub = glm::vec2(b.y, b.z);
                uc = glm::vec2(c.y, c.z);
              } else if (std::abs(n.y) >= std::abs(n.z)) {
                ua = glm::vec2(a.x, a.z);
                ub = glm::vec2(b.x, b.z);
                uc = glm::vec2(c.x, c.z);
              }
              glm::vec2 minUV = glm::min(glm::min(ua, ub), uc);
              glm::vec2 maxUV = glm::max(glm::max(ua, ub), uc);
              glm::vec2 span = maxUV - minUV;
              auto toUv = [&](const glm::vec2 &v) {
                return glm::vec2(
                    span.x > 1e-5f ? (v.x - minUV.x) / span.x : 0.0f,
                    span.y > 1e-5f ? (v.y - minUV.y) / span.y : 0.0f);
              };
              meshEditAsset.uvs[face.x] = toUv(ua);
              meshEditAsset.uvs[face.y] = toUv(ub);
              meshEditAsset.uvs[face.z] = toUv(uc);
              return true;
            };

            std::unordered_set<int> customExtrudeUvFaces;
            auto addSideQuadWithUv = [&](uint32_t a, uint32_t b, uint32_t aNew,
                                         uint32_t bNew, uint32_t matIdx) {
              uint32_t sideA = duplicateVertex(a);
              uint32_t sideB = duplicateVertex(b);
              uint32_t sideANew = duplicateVertex(aNew);
              uint32_t sideBNew = duplicateVertex(bNew);

              ensureUvs();
              const float edgeLen =
                  std::max(1e-4f, glm::length(meshEditAsset.positions[sideB] -
                                              meshEditAsset.positions[sideA]));
              const float heightA =
                  glm::length(meshEditAsset.positions[sideANew] -
                              meshEditAsset.positions[sideA]);
              const float heightB =
                  glm::length(meshEditAsset.positions[sideBNew] -
                              meshEditAsset.positions[sideB]);
              const float sideHeight =
                  std::max(1e-4f, 0.5f * (heightA + heightB));

              meshEditAsset.uvs[sideA] = glm::vec2(0.0f, 0.0f);
              meshEditAsset.uvs[sideB] = glm::vec2(edgeLen, 0.0f);
              meshEditAsset.uvs[sideBNew] = glm::vec2(edgeLen, sideHeight);
              meshEditAsset.uvs[sideANew] = glm::vec2(0.0f, sideHeight);

              int firstFace = static_cast<int>(meshEditAsset.faces.size());
              meshEditAsset.faces.push_back(
                  glm::u32vec3(sideA, sideB, sideBNew));
              meshEditAsset.faceMaterialIndices.push_back(matIdx);
              meshEditAsset.faces.push_back(
                  glm::u32vec3(sideA, sideBNew, sideANew));
              meshEditAsset.faceMaterialIndices.push_back(matIdx);
              customExtrudeUvFaces.insert(firstFace);
              customExtrudeUvFaces.insert(firstFace + 1);
              pushExtrudeVert(static_cast<int>(sideANew));
              pushExtrudeVert(static_cast<int>(sideBNew));
              meshEditAsset.hasUVs = true;
            };

            if (wantsExtrude &&
                meshEditSelectionMode == MeshEditSelectionMode::Face &&
                !meshEditSelectedFaces.empty()) {
              newFaceStart = (int)meshEditAsset.faces.size();
              const size_t faceCount = meshEditAsset.faces.size();
              std::vector<glm::u32vec3> originalFaces = meshEditAsset.faces;
              std::vector<uint32_t> originalFaceMaterials =
                  meshEditAsset.faceMaterialIndices;
              if (originalFaceMaterials.size() < faceCount) {
                originalFaceMaterials.resize(
                    faceCount,
                    static_cast<uint32_t>(meshEditActiveMaterialSlot));
              }
              auto matForFace = [&](int fi) -> uint32_t {
                if (fi >= 0 &&
                    fi < static_cast<int>(originalFaceMaterials.size())) {
                  return originalFaceMaterials[fi];
                }
                return static_cast<uint32_t>(meshEditActiveMaterialSlot);
              };
              std::vector<bool> faceSelected(faceCount, false);
              for (int fi : meshEditSelectedFaces) {
                if (fi >= 0 && fi < (int)faceCount)
                  faceSelected[fi] = true;
              }

              std::unordered_map<uint32_t, uint32_t> vertexMap;
              std::unordered_map<int, glm::u32vec3> newFaceVerts;
              std::vector<int> newFaceSelection;
              vertexMap.reserve(meshEditSelectedFaces.size() * 3);
              newFaceVerts.reserve(meshEditSelectedFaces.size());
              newFaceSelection.reserve(meshEditSelectedFaces.size());
              for (int fi : meshEditSelectedFaces) {
                if (fi < 0 || fi >= (int)faceCount)
                  continue;
                const auto f = originalFaces[fi];
                uint32_t idx[3] = {f.x, f.y, f.z};
                uint32_t newIdx[3];
                for (int k = 0; k < 3; ++k) {
                  if (seams) {
                    newIdx[k] = duplicateVertex(idx[k]);
                  } else {
                    auto it = vertexMap.find(idx[k]);
                    if (it == vertexMap.end()) {
                      uint32_t created = duplicateVertex(idx[k]);
                      vertexMap[idx[k]] = created;
                      newIdx[k] = created;
                    } else {
                      newIdx[k] = it->second;
                    }
                  }
                  pushExtrudeVert((int)newIdx[k]);
                }
                meshEditAsset.faces.push_back(
                    glm::u32vec3(newIdx[0], newIdx[1], newIdx[2]));
                meshEditAsset.faceMaterialIndices.push_back(matForFace(fi));
                int newFaceIndex = (int)meshEditAsset.faces.size() - 1;
                newFaceVerts[fi] =
                    glm::u32vec3(newIdx[0], newIdx[1], newIdx[2]);
                newFaceSelection.push_back(newFaceIndex);
              }

              if (seams) {
                for (int fi : meshEditSelectedFaces) {
                  if (fi < 0 || fi >= (int)faceCount)
                    continue;
                  auto itFace = newFaceVerts.find(fi);
                  if (itFace == newFaceVerts.end())
                    continue;
                  const auto f = itFace->second;
                  const auto oldF = originalFaces[fi];
                  uint32_t oldIdx[3] = {oldF.x, oldF.y, oldF.z};
                  uint32_t newIdx[3] = {f.x, f.y, f.z};
                  uint32_t mat = matForFace(fi);
                  addSideQuadWithUv(oldIdx[0], oldIdx[1], newIdx[0], newIdx[1],
                                    mat);
                  addSideQuadWithUv(oldIdx[1], oldIdx[2], newIdx[1], newIdx[2],
                                    mat);
                  addSideQuadWithUv(oldIdx[2], oldIdx[0], newIdx[2], newIdx[0],
                                    mat);
                }
              } else {
                struct EdgeInfo {
                  int total = 0;
                  int selected = 0;
                };
                std::unordered_map<uint64_t, EdgeInfo> edgeInfo;
                edgeInfo.reserve(faceCount * 3);
                auto edgeKey = [](uint32_t a, uint32_t b) {
                  return (static_cast<uint64_t>(std::min(a, b)) << 32) |
                         static_cast<uint64_t>(std::max(a, b));
                };
                for (size_t fi = 0; fi < faceCount; ++fi) {
                  const auto &f = originalFaces[fi];
                  uint32_t tri[3] = {f.x, f.y, f.z};
                  for (int e = 0; e < 3; ++e) {
                    uint32_t a = tri[e];
                    uint32_t b = tri[(e + 1) % 3];
                    auto &info = edgeInfo[edgeKey(a, b)];
                    info.total += 1;
                    if (faceSelected[fi])
                      info.selected += 1;
                  }
                }
                for (int fi : meshEditSelectedFaces) {
                  if (fi < 0 || fi >= (int)faceCount)
                    continue;
                  const auto &f = originalFaces[fi];
                  uint32_t tri[3] = {f.x, f.y, f.z};
                  for (int e = 0; e < 3; ++e) {
                    uint32_t a = tri[e];
                    uint32_t b = tri[(e + 1) % 3];
                    auto it = edgeInfo.find(edgeKey(a, b));
                    if (it == edgeInfo.end())
                      continue;
                    uint32_t mat = matForFace(fi);
                    if (it->second.selected == 1 &&
                        it->second.selected < it->second.total) {
                      uint32_t aNew = vertexMap[a];
                      uint32_t bNew = vertexMap[b];
                      addSideQuadWithUv(a, b, aNew, bNew, mat);
                    } else if (it->second.total == 1) {
                      uint32_t aNew = vertexMap[a];
                      uint32_t bNew = vertexMap[b];
                      addSideQuadWithUv(a, b, aNew, bNew, mat);
                    }
                  }
                }
              }

              std::vector<uint8_t> removeOriginalFaces(faceCount, 0u);
              int removedOriginalCount = 0;
              for (int fi : meshEditSelectedFaces) {
                if (fi >= 0 && fi < static_cast<int>(faceCount) &&
                    !removeOriginalFaces[fi]) {
                  removeOriginalFaces[fi] = 1u;
                  ++removedOriginalCount;
                }
              }
              if (removedOriginalCount > 0) {
                std::vector<int> faceRemap(meshEditAsset.faces.size(), -1);
                std::vector<glm::u32vec3> rebuiltFaces;
                std::vector<uint32_t> rebuiltMaterials;
                rebuiltFaces.reserve(meshEditAsset.faces.size() -
                                     static_cast<size_t>(removedOriginalCount));
                rebuiltMaterials.reserve(
                    meshEditAsset.faceMaterialIndices.size());

                for (int fi = 0;
                     fi < static_cast<int>(meshEditAsset.faces.size()); ++fi) {
                  if (fi < static_cast<int>(faceCount) &&
                      removeOriginalFaces[fi]) {
                    continue;
                  }
                  faceRemap[fi] = static_cast<int>(rebuiltFaces.size());
                  rebuiltFaces.push_back(meshEditAsset.faces[fi]);
                  uint32_t mat =
                      (fi < static_cast<int>(
                                meshEditAsset.faceMaterialIndices.size()))
                          ? meshEditAsset.faceMaterialIndices[fi]
                          : static_cast<uint32_t>(meshEditActiveMaterialSlot);
                  rebuiltMaterials.push_back(mat);
                }

                meshEditAsset.faces = std::move(rebuiltFaces);
                meshEditAsset.faceMaterialIndices = std::move(rebuiltMaterials);

                auto remapFaceSelection = [&](std::vector<int> &selection) {
                  std::vector<int> remapped;
                  remapped.reserve(selection.size());
                  for (int fi : selection) {
                    if (fi < 0 || fi >= static_cast<int>(faceRemap.size()))
                      continue;
                    int mapped = faceRemap[fi];
                    if (mapped < 0)
                      continue;
                    if (std::find(remapped.begin(), remapped.end(), mapped) ==
                        remapped.end()) {
                      remapped.push_back(mapped);
                    }
                  }
                  selection = std::move(remapped);
                };
                remapFaceSelection(newFaceSelection);

                std::unordered_set<int> remappedCustomUvFaces;
                remappedCustomUvFaces.reserve(customExtrudeUvFaces.size() * 2);
                for (int fi : customExtrudeUvFaces) {
                  if (fi < 0 || fi >= static_cast<int>(faceRemap.size()))
                    continue;
                  int mapped = faceRemap[fi];
                  if (mapped >= 0) {
                    remappedCustomUvFaces.insert(mapped);
                  }
                }
                customExtrudeUvFaces = std::move(remappedCustomUvFaces);

                if (newFaceStart >= 0 &&
                    newFaceStart < static_cast<int>(faceRemap.size())) {
                  newFaceStart = faceRemap[newFaceStart];
                } else {
                  newFaceStart = -1;
                }
                if (newFaceStart < 0 && !newFaceSelection.empty()) {
                  newFaceStart = *std::min_element(newFaceSelection.begin(),
                                                   newFaceSelection.end());
                }
              }

              if (!newFaceSelection.empty()) {
                meshEditSelectedFaces = newFaceSelection;
                meshEditSelectedVertices.clear();
                meshEditSelectedEdges.clear();
              }

              meshEditExtruding = !meshEditExtrudeVerts.empty();
            } else if (wantsExtrude &&
                       meshEditSelectionMode == MeshEditSelectionMode::Edge &&
                       !meshEditSelectedEdges.empty()) {
              newFaceStart = (int)meshEditAsset.faces.size();
              std::unordered_map<uint32_t, uint32_t> vertexMap;
              if (!seams) {
                vertexMap.reserve(meshEditSelectedEdges.size() * 2);
              }
              uint32_t activeMat =
                  static_cast<uint32_t>(meshEditActiveMaterialSlot);

              for (int ei : meshEditSelectedEdges) {
                if (ei < 0 || ei >= (int)edges.size())
                  continue;
                uint32_t a = edges[ei].x;
                uint32_t b = edges[ei].y;
                uint32_t aNew = 0;
                uint32_t bNew = 0;
                if (seams) {
                  aNew = duplicateVertex(a);
                  bNew = duplicateVertex(b);
                } else {
                  auto ita = vertexMap.find(a);
                  if (ita == vertexMap.end()) {
                    aNew = duplicateVertex(a);
                    vertexMap[a] = aNew;
                  } else {
                    aNew = ita->second;
                  }
                  auto itb = vertexMap.find(b);
                  if (itb == vertexMap.end()) {
                    bNew = duplicateVertex(b);
                    vertexMap[b] = bNew;
                  } else {
                    bNew = itb->second;
                  }
                }
                pushExtrudeVert((int)aNew);
                pushExtrudeVert((int)bNew);
                addSideQuadWithUv(a, b, aNew, bNew, activeMat);
              }

              meshEditExtruding = !meshEditExtrudeVerts.empty();
            }

            if (newFaceStart >= 0 &&
                newFaceStart < (int)meshEditAsset.faces.size()) {
              meshEditExtrudeFaces.clear();
              meshEditExtrudeFaces.reserve(static_cast<size_t>(
                  meshEditAsset.faces.size() - newFaceStart));
              for (int fi = newFaceStart; fi < (int)meshEditAsset.faces.size();
                   ++fi) {
                meshEditExtrudeFaces.push_back(fi);
              }
              ensureUvs();
              bool wroteUvs = false;
              for (int fi = newFaceStart; fi < (int)meshEditAsset.faces.size();
                   ++fi) {
                if (customExtrudeUvFaces.count(fi) != 0) {
                  continue;
                }
                const auto &f = meshEditAsset.faces[fi];
                bool shouldWrite = !meshEditAsset.hasUVs ||
                                   f.x >= (uint32_t)originalVertexCount ||
                                   f.y >= (uint32_t)originalVertexCount ||
                                   f.z >= (uint32_t)originalVertexCount;
                if (shouldWrite) {
                  wroteUvs |= applyPlanarUV(f);
                }
              }
              if (wroteUvs) {
                meshEditAsset.hasUVs = true;
              }
            }
          }

          std::vector<int> affectedVerts = baseAffectedVerts;
          std::vector<float> transformWeights(meshEditAsset.positions.size(),
                                              0.0f);
          for (int idx : baseAffectedVerts) {
            if (idx >= 0 &&
                idx < static_cast<int>(transformWeights.size())) {
              transformWeights[idx] = 1.0f;
            }
          }
          if (!meshEditExtruding && meshEditProportionalEditing &&
              meshEditProportionalRadius > 1e-6f) {
            affectedVerts.clear();
            const float radius = meshEditProportionalRadius;
            const float exponent =
                std::max(0.25f, meshEditProportionalFalloff);
            for (int vi = 0;
                 vi < static_cast<int>(meshEditAsset.positions.size()); ++vi) {
              float nearest = FLT_MAX;
              for (int selectedVertex : baseAffectedVerts) {
                nearest = std::min(
                    nearest,
                    glm::length(meshEditAsset.positions[vi] -
                                meshEditAsset.positions[selectedVertex]));
              }
              if (nearest > radius)
                continue;
              const float linear =
                  std::clamp(1.0f - nearest / radius, 0.0f, 1.0f);
              transformWeights[vi] = std::pow(linear, exponent);
              if (transformWeights[vi] > 1e-4f) {
                affectedVerts.push_back(vi);
              }
            }
          }
          if (meshEditExtruding && !meshEditExtrudeVerts.empty()) {
            affectedVerts = meshEditExtrudeVerts;
            transformWeights.resize(meshEditAsset.positions.size(), 0.0f);
            for (int idx : affectedVerts) {
              if (idx >= 0 &&
                  idx < static_cast<int>(transformWeights.size())) {
                transformWeights[idx] = 1.0f;
              }
            }
          }

          if (usingNow) {
            for (int idx : affectedVerts) {
              glm::vec3 wp = glm::vec3(
                  modelMatrix * glm::vec4(meshEditAsset.positions[idx], 1.0f));
              const glm::vec3 transformed =
                  glm::vec3(componentDelta * glm::vec4(wp, 1.0f));
              const float weight =
                  idx < static_cast<int>(transformWeights.size())
                      ? transformWeights[idx]
                      : 1.0f;
              wp = glm::mix(wp, transformed, weight);
              glm::vec3 newLocal = glm::vec3(invModel * glm::vec4(wp, 1.0f));
              meshEditAsset.positions[idx] = newLocal;
            }

            recalcMesh();
            if (meshEditExtruding && meshEditAutoUV &&
                !meshEditExtrudeFaces.empty()) {
              bool wroteUvs = false;
              for (int fi : meshEditExtrudeFaces) {
                if (fi < 0 ||
                    fi >= static_cast<int>(meshEditAsset.faces.size()))
                  continue;
                wroteUvs |= applyPlanarUvToFace(fi);
              }
              if (wroteUvs) {
                meshEditAsset.hasUVs = true;
              }
            }
            meshEditDirty = true;

            syncMeshEditToGPU(selectedObj);
          } else {
            meshEditHistoryCaptured = false;
            meshEditExtruding = false;
            meshEditExtrudeVerts.clear();
            meshEditExtrudeFaces.clear();
          }

          meshEditWasUsing = usingNow;
        } else {
          meshEditHistoryCaptured = false;
          meshEditExtruding = false;
          meshEditExtrudeVerts.clear();
          meshEditExtrudeFaces.clear();
          meshEditWasUsing = false;
          meshEditAwaitMouseRelease = false;
        }
      } else if (meshModeActive &&
                 meshEditSelectionMode == MeshEditSelectionMode::Carve) {
      } else {
        // Object transform mode
        float *snapPtr = nullptr;
        float snapRot[3] = {rotationSnapValue, rotationSnapValue,
                            rotationSnapValue};

        if (useSnap) {
          if (mCurrentGizmoOperation == ImGuizmo::ROTATE) {
            snapPtr = snapRot;
          } else {
            snapPtr = snapValue;
          }
        }

        glm::vec3 gizmoBoundsMin(-0.5f);
        glm::vec3 gizmoBoundsMax(0.5f);

        switch (selectedObj->type) {
        case ObjectType::Cube:
          gizmoBoundsMin = glm::vec3(-0.5f);
          gizmoBoundsMax = glm::vec3(0.5f);
          break;
        case ObjectType::Sphere:
          gizmoBoundsMin = glm::vec3(-0.5f);
          gizmoBoundsMax = glm::vec3(0.5f);
          break;
        case ObjectType::Capsule:
          gizmoBoundsMin = glm::vec3(-0.35f, -0.9f, -0.35f);
          gizmoBoundsMax = glm::vec3(0.35f, 0.9f, 0.35f);
          break;
        case ObjectType::Plane:
          gizmoBoundsMin = glm::vec3(-0.5f, -0.5f, -0.02f);
          gizmoBoundsMax = glm::vec3(0.5f, 0.5f, 0.02f);
          break;
        case ObjectType::Mirror:
        case ObjectType::Sprite:
        case ObjectType::Sprite25D:
        case ObjectType::ParticleSystem2D:
          gizmoBoundsMin = glm::vec3(-0.5f, -0.5f, -0.02f);
          gizmoBoundsMax = glm::vec3(0.5f, 0.5f, 0.02f);
          break;
        case ObjectType::Torus:
          gizmoBoundsMin = glm::vec3(-0.5f);
          gizmoBoundsMax = glm::vec3(0.5f);
          break;
        case ObjectType::OBJMesh: {
          const auto *info = g_objLoader.getMeshInfo(selectedObj->meshId);
          if (info && info->boundsMin.x < info->boundsMax.x) {
            gizmoBoundsMin = info->boundsMin;
            gizmoBoundsMax = info->boundsMax;
          }
          break;
        }
        case ObjectType::Model: {
          const auto *info = getModelLoader().getMeshInfo(selectedObj->meshId);
          if (info && info->boundsMin.x < info->boundsMax.x) {
            gizmoBoundsMin = info->boundsMin;
            gizmoBoundsMax = info->boundsMax;
          }
          break;
        }
        case ObjectType::Camera:
          gizmoBoundsMin = glm::vec3(-0.3f);
          gizmoBoundsMax = glm::vec3(0.3f);
          break;
        case ObjectType::DirectionalLight:
        case ObjectType::PointLight:
        case ObjectType::SpotLight:
        case ObjectType::AreaLight:
          gizmoBoundsMin = glm::vec3(-0.3f);
          gizmoBoundsMax = glm::vec3(0.3f);
          break;
        case ObjectType::PostFXNode:
          gizmoBoundsMin = glm::vec3(-0.25f);
          gizmoBoundsMax = glm::vec3(0.25f);
          break;
        case ObjectType::Empty:
          gizmoBoundsMin = glm::vec3(-0.2f);
          gizmoBoundsMax = glm::vec3(0.2f);
          break;
        case ObjectType::Sprite2D:
        case ObjectType::Canvas:
        case ObjectType::UIImage:
        case ObjectType::UISlider:
        case ObjectType::UIButton:
        case ObjectType::UIText:
          gizmoBoundsMin = glm::vec3(-0.5f, -0.5f, -0.01f);
          gizmoBoundsMax = glm::vec3(0.5f, 0.5f, 0.01f);
          break;
        default:
          break;
        }

        float bounds[6] = {gizmoBoundsMin.x, gizmoBoundsMin.y,
                           gizmoBoundsMin.z, gizmoBoundsMax.x,
                           gizmoBoundsMax.y, gizmoBoundsMax.z};
        float boundsSnap[3] = {snapValue[0], snapValue[1], snapValue[2]};
        const float *boundsPtr =
            (mCurrentGizmoOperation == ImGuizmo::BOUNDS) ? bounds : nullptr;
        const float *boundsSnapPtr =
            (useSnap && mCurrentGizmoOperation == ImGuizmo::BOUNDS) ? boundsSnap
                                                                    : nullptr;

        if (gizmoDragActive) {
          for (SceneObject &o : sceneObjects) {
            auto startIt = gizmoDragStartTransforms.find(o.id);
            auto lastIt = gizmoDragLastAppliedTransforms.find(o.id);
            if (startIt == gizmoDragStartTransforms.end() ||
                lastIt == gizmoDragLastAppliedTransforms.end()) {
              continue;
            }

            const GizmoDragTransform live{o.position, o.rotation, o.scale};
            const GizmoDragTransform &last = lastIt->second;
            const glm::vec3 positionDelta = live.position - last.position;
            const glm::vec3 rotationDelta = live.rotation - last.rotation;
            const glm::vec3 scaleDelta = live.scale - last.scale;
            const bool moved =
                glm::dot(positionDelta, positionDelta) > 1e-12f ||
                glm::dot(rotationDelta, rotationDelta) > 1e-10f ||
                glm::dot(scaleDelta, scaleDelta) > 1e-12f;
            if (!moved) {
              continue;
            }

            const glm::mat4 liveMatrix =
                ComposeTransform(live.position, live.rotation, live.scale);
            const glm::mat4 lastMatrix =
                ComposeTransform(last.position, last.rotation, last.scale);
            const glm::mat4 externalDelta =
                liveMatrix * glm::inverse(lastMatrix);

            GizmoDragTransform &start = startIt->second;
            const glm::mat4 shiftedStart =
                externalDelta *
                ComposeTransform(start.position, start.rotation, start.scale);
            glm::vec3 shiftedPosition(0.0f), shiftedRotation(0.0f),
                shiftedScale(1.0f);
            DecomposeMatrix(shiftedStart, shiftedPosition, shiftedRotation,
                            shiftedScale, &start.scale, &start.rotation);
            if (selectedObj && o.id == selectedObj->id) {
              gizmoDragPivotShift += positionDelta;
            }

            start.position = shiftedPosition;
            start.rotation =
                NormalizeEulerDegrees(glm::degrees(shiftedRotation));
            start.scale = shiftedScale;
          }

          modelMatrix = gizmoDragLiveModel;
        }

        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                             mCurrentGizmoOperation, mCurrentGizmoMode,
                             glm::value_ptr(modelMatrix), nullptr, snapPtr,
                             boundsPtr, boundsSnapPtr);

        if (mCurrentGizmoOperation == ImGuizmo::BOUNDS) {
          std::array<glm::vec3, 8> corners = {
              glm::vec3(gizmoBoundsMin.x, gizmoBoundsMin.y, gizmoBoundsMin.z),
              glm::vec3(gizmoBoundsMax.x, gizmoBoundsMin.y, gizmoBoundsMin.z),
              glm::vec3(gizmoBoundsMax.x, gizmoBoundsMax.y, gizmoBoundsMin.z),
              glm::vec3(gizmoBoundsMin.x, gizmoBoundsMax.y, gizmoBoundsMin.z),
              glm::vec3(gizmoBoundsMin.x, gizmoBoundsMin.y, gizmoBoundsMax.z),
              glm::vec3(gizmoBoundsMax.x, gizmoBoundsMin.y, gizmoBoundsMax.z),
              glm::vec3(gizmoBoundsMax.x, gizmoBoundsMax.y, gizmoBoundsMax.z),
              glm::vec3(gizmoBoundsMin.x, gizmoBoundsMax.y, gizmoBoundsMax.z),
          };

          std::array<ImVec2, 8> projected{};
          bool allProjected = true;
          for (size_t i = 0; i < corners.size(); ++i) {
            glm::vec3 world =
                glm::vec3(modelMatrix * glm::vec4(corners[i], 1.0f));
            auto p = projectToScreen(world);
            if (!p.has_value()) {
              allProjected = false;
              break;
            }
            projected[i] = *p;
          }

          if (allProjected) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImU32 col = ImGui::GetColorU32(ImVec4(1.0f, 0.93f, 0.35f, 0.45f));
            const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},
                                      {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                      {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (auto &e : edges) {
              dl->AddLine(projected[e[0]], projected[e[1]], col, 2.0f);
            }
          }
        }

        if (ImGuizmo::IsUsing()) {
          if (!gizmoHistoryCaptured) {
            recordState("gizmo");
            gizmoHistoryCaptured = true;
          }
          if (!gizmoDragActive) {
            gizmoDragStartModel = originalModel;
            gizmoDragPivotShift = glm::vec3(0.0f);
            gizmoDragStartTransforms.clear();
            gizmoDragLastAppliedTransforms.clear();
            for (const SceneObject &o : sceneObjects) {
              gizmoDragStartTransforms[o.id] = {o.position, o.rotation, o.scale};
              gizmoDragLastAppliedTransforms[o.id] = {
                  o.position, o.rotation, o.scale};
            }
            gizmoDragActive = true;
          }
          gizmoDragLiveModel = modelMatrix;

          glm::mat4 delta = modelMatrix * glm::inverse(gizmoDragStartModel);

          auto dragStartOf = [&](const SceneObject &o) {
            auto it = gizmoDragStartTransforms.find(o.id);
            if (it != gizmoDragStartTransforms.end()) {
              return it->second;
            }
            return GizmoDragTransform{o.position, o.rotation, o.scale};
          };

          auto unwrapNear = [](float angle, float reference) {
            float result = angle;
            while (result - reference > 180.0f)
              result -= 360.0f;
            while (reference - result > 180.0f)
              result += 360.0f;
            return result;
          };
          auto quatFromEulerXYZ = [](const glm::vec3 &deg) {
            glm::mat4 m(1.0f);
            m = glm::rotate(m, glm::radians(deg.x),
                            glm::vec3(1.0f, 0.0f, 0.0f));
            m = glm::rotate(m, glm::radians(deg.y),
                            glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::rotate(m, glm::radians(deg.z),
                            glm::vec3(0.0f, 0.0f, 1.0f));
            return glm::normalize(glm::quat_cast(glm::mat3(m)));
          };
          auto quatFromMatrixNoScale = [](const glm::mat4 &m) {
            glm::vec3 x = glm::vec3(m[0]);
            glm::vec3 y = glm::vec3(m[1]);
            if (glm::length(x) < 1e-6f || glm::length(y) < 1e-6f) {
              return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            x = glm::normalize(x);
            y = glm::normalize(y - x * glm::dot(x, y));
            glm::vec3 z = glm::cross(x, y);
            if (glm::length(z) < 1e-6f) {
              z = glm::vec3(0.0f, 0.0f, 1.0f);
            } else {
              z = glm::normalize(z);
            }
            glm::mat3 rot(1.0f);
            rot[0] = x;
            rot[1] = y;
            rot[2] = z;
            return glm::normalize(glm::quat_cast(rot));
          };

          auto commitDraggedTransform = [&](SceneObject &o) {
            syncLocalTransform(o);
            if (physics && physics->isReady() && o.hasRigidbody &&
                o.rigidbody.enabled && (isPlaying || specMode || testMode)) {
              physics->setActorPose(o.id, o.position, o.rotation);
            }
          };

          auto applyDelta = [&](SceneObject &o) {
            if (o.IsInvariable) return;
            const GizmoDragTransform start = dragStartOf(o);
            glm::mat4 m = ComposeTransform(start.position, start.rotation, start.scale);
            glm::mat4 newM = delta * m;
            glm::vec3 t, r, s;
            DecomposeMatrix(newM, t, r, s, &start.scale, &start.rotation);

            glm::vec3 rotDeg = glm::degrees(r);
            if (mCurrentGizmoOperation == ImGuizmo::ROTATE ||
                mCurrentGizmoOperation == ImGuizmo::UNIVERSAL) {
              glm::quat beforeQ = quatFromMatrixNoScale(m);
              glm::quat afterQ = quatFromMatrixNoScale(newM);
              glm::quat deltaQ = glm::normalize(afterQ * glm::inverse(beforeQ));
              glm::quat finalQ =
                  glm::normalize(deltaQ * quatFromEulerXYZ(start.rotation));
              glm::vec3 tmpPos(0.0f), tmpRot(0.0f), tmpScale(1.0f);
              DecomposeMatrix(
                  ComposeTransform(glm::vec3(0.0f), finalQ, glm::vec3(1.0f)),
                  tmpPos, tmpRot, tmpScale, nullptr, &start.rotation);
              rotDeg = glm::degrees(tmpRot);
            }

            if (mCurrentGizmoOperation == ImGuizmo::ROTATE ||
                mCurrentGizmoOperation == ImGuizmo::UNIVERSAL) {
              const glm::vec3 pivotStart = glm::vec3(gizmoDragStartModel[3]);
              const glm::vec3 pivotNow = pivotStart + gizmoDragPivotShift;
              const glm::mat3 deltaRotation = glm::mat3(delta);
              const glm::vec3 deltaTranslation = glm::vec3(delta[3]);
              const glm::vec3 userTranslation =
                  deltaTranslation - pivotStart + deltaRotation * pivotStart;
              t = deltaRotation * (start.position - pivotNow) + pivotNow +
                  userTranslation;
            }

            o.position = t;
            if (o.parentId != -1) {
              rotDeg.x = unwrapNear(rotDeg.x, start.rotation.x);
              rotDeg.y = unwrapNear(rotDeg.y, start.rotation.y);
              rotDeg.z = unwrapNear(rotDeg.z, start.rotation.z);
              o.rotation = rotDeg;
            } else {
              o.rotation = NormalizeEulerDegrees(rotDeg);
            }
            o.scale = s;
            commitDraggedTransform(o);
          };

          std::vector<int> selectedRoots;
          if (selectedObjectIds.size() <= 1) {
            selectedRoots.push_back(selectedObj->id);
          } else {
            std::unordered_set<int> selectedSet;
            for (int id : selectedObjectIds) {
              SceneObject *candidate = findObjectById(id);
              if (candidate && !candidate->IsInvariable) {
                selectedSet.insert(id);
              }
            }
            auto getParentId = [&](int id) -> int {
              auto it = std::find_if(
                  sceneObjects.begin(), sceneObjects.end(),
                  [id](const SceneObject &o) { return o.id == id; });
              if (it == sceneObjects.end())
                return -1;
              return it->parentId;
            };
            auto hasSelectedAncestor = [&](int id) -> bool {
              int parentId = getParentId(id);
              while (parentId != -1) {
                if (selectedSet.count(parentId)) {
                  return true;
                }
                parentId = getParentId(parentId);
              }
              return false;
            };
            for (int id : selectedObjectIds) {
              SceneObject *candidate = findObjectById(id);
              if (!candidate || candidate->IsInvariable) continue;
              if (!hasSelectedAncestor(id)) {
                selectedRoots.push_back(id);
              }
            }
          }

          if (selectedRoots.size() <= 1) {
            SceneObject *rootObj = selectedObj;
            if (!selectedRoots.empty()) {
              auto rootIt = std::find_if(
                  sceneObjects.begin(), sceneObjects.end(),
                  [&](const SceneObject &o) { return o.id == selectedRoots[0]; });
              if (rootIt != sceneObjects.end()) {
                rootObj = &*rootIt;
              }
            }
            if (rootObj) {
              applyDelta(*rootObj);
            }
          } else if (mCurrentGizmoMode == ImGuizmo::LOCAL &&
                     mCurrentGizmoOperation != ImGuizmo::BOUNDS) {
            const bool applyTranslation =
                (mCurrentGizmoOperation == ImGuizmo::TRANSLATE ||
                 mCurrentGizmoOperation == ImGuizmo::UNIVERSAL);
            const bool applyRotation =
                (mCurrentGizmoOperation == ImGuizmo::ROTATE ||
                 mCurrentGizmoOperation == ImGuizmo::UNIVERSAL);
            const bool applyScale =
                (mCurrentGizmoOperation == ImGuizmo::SCALE ||
                 mCurrentGizmoOperation == ImGuizmo::UNIVERSAL);

            const GizmoDragTransform selectedStart = dragStartOf(*selectedObj);

            glm::vec3 deltaTranslationWorld(delta[3][0], delta[3][1],
                                            delta[3][2]);
            glm::quat selectedBeforeQ = quatFromEulerXYZ(selectedStart.rotation);
            glm::vec3 selectedLocalTranslation =
                glm::inverse(selectedBeforeQ) * deltaTranslationWorld;

            glm::mat4 selectedBeforeM = ComposeTransform(
                selectedStart.position, selectedStart.rotation, selectedStart.scale);
            glm::mat4 selectedAfterM = delta * selectedBeforeM;
            glm::quat selectedAfterQ = quatFromMatrixNoScale(selectedAfterM);
            glm::quat localDeltaQ =
                glm::normalize(glm::inverse(selectedBeforeQ) * selectedAfterQ);

            glm::vec3 selectedAfterT(0.0f), selectedAfterR(0.0f),
                selectedAfterS(1.0f);
            DecomposeMatrix(selectedAfterM, selectedAfterT, selectedAfterR,
                            selectedAfterS, &selectedStart.scale,
                            &selectedStart.rotation);
            auto safeScaleRatio = [](float after, float before) {
              constexpr float kEps = 1e-5f;
              return (std::abs(before) > kEps) ? (after / before) : 1.0f;
            };
            glm::vec3 scaleRatio(
                safeScaleRatio(selectedAfterS.x, selectedStart.scale.x),
                safeScaleRatio(selectedAfterS.y, selectedStart.scale.y),
                safeScaleRatio(selectedAfterS.z, selectedStart.scale.z));

            for (int id : selectedRoots) {
              auto itObj = std::find_if(
                  sceneObjects.begin(), sceneObjects.end(),
                  [id](const SceneObject &o) { return o.id == id; });
              if (itObj == sceneObjects.end())
                continue;

              SceneObject &o = *itObj;
              if (o.IsInvariable)
                continue;
              const GizmoDragTransform objectStart = dragStartOf(o);
              bool changed = false;

              if (applyTranslation) {
                glm::quat objectQ = quatFromEulerXYZ(objectStart.rotation);
                glm::vec3 objectWorldMove = objectQ * selectedLocalTranslation;
                o.position = objectStart.position + objectWorldMove;
                changed = true;
              }

              if (applyRotation) {
                glm::quat objectBeforeQ = quatFromEulerXYZ(objectStart.rotation);
                glm::quat objectAfterQ =
                    glm::normalize(objectBeforeQ * localDeltaQ);
                glm::vec3 tmpPos(0.0f), tmpRot(0.0f), tmpScale(1.0f);
                DecomposeMatrix(ComposeTransform(glm::vec3(0.0f), objectAfterQ,
                                                 glm::vec3(1.0f)),
                                tmpPos, tmpRot, tmpScale, nullptr,
                                &objectStart.rotation);
                glm::vec3 rotDeg = glm::degrees(tmpRot);
                if (o.parentId != -1) {
                  rotDeg.x = unwrapNear(rotDeg.x, objectStart.rotation.x);
                  rotDeg.y = unwrapNear(rotDeg.y, objectStart.rotation.y);
                  rotDeg.z = unwrapNear(rotDeg.z, objectStart.rotation.z);
                  o.rotation = rotDeg;
                } else {
                  o.rotation = NormalizeEulerDegrees(rotDeg);
                }
                changed = true;
              }

              if (applyScale) {
                o.scale.x = objectStart.scale.x * scaleRatio.x;
                o.scale.y = objectStart.scale.y * scaleRatio.y;
                o.scale.z = objectStart.scale.z * scaleRatio.z;
                changed = true;
              }

              if (changed) {
                commitDraggedTransform(o);
              }
            }
          } else {
            for (int id : selectedRoots) {
              auto itObj = std::find_if(
                  sceneObjects.begin(), sceneObjects.end(),
                  [id](const SceneObject &o) { return o.id == id; });
              if (itObj != sceneObjects.end()) {
                applyDelta(*itObj);
              }
            }
          }

          updateHierarchyWorldTransforms();

          for (const SceneObject &o : sceneObjects) {
            if (gizmoDragStartTransforms.find(o.id) !=
                gizmoDragStartTransforms.end()) {
              gizmoDragLastAppliedTransforms[o.id] = {
                  o.position, o.rotation, o.scale};
            }
          }

          projectManager.currentProject.hasUnsavedChanges = true;
        } else {
          gizmoHistoryCaptured = false;
          gizmoDragActive = false;
          gizmoDragPivotShift = glm::vec3(0.0f);
          gizmoDragStartTransforms.clear();
          gizmoDragLastAppliedTransforms.clear();
        }
      }
    } else {
      gizmoHistoryCaptured = false;
      gizmoDragActive = false;
      gizmoDragPivotShift = glm::vec3(0.0f);
      gizmoDragStartTransforms.clear();
      gizmoDragLastAppliedTransforms.clear();
    }

    const float cameraOverlayAspect =
        std::max(0.1f, activeGameResolutionAspect);
    const float gizmoOverlayScaleClamped =
        std::clamp(sceneGizmoOverlayScale, 0.4f, 3.0f);
    const float gizmoIconScaleClamped =
        std::clamp(sceneGizmoIconScale, 0.4f, 3.0f);

    auto drawCameraDirection = [&](const SceneObject &camObj) {
      if (!IsObjectEnabledInHierarchy(camObj))
        return;

      glm::quat q = EulerXYZDegreesToQuat(camObj.rotation);
      glm::mat3 rot = glm::mat3_cast(q);
      glm::vec3 forward = glm::normalize(rot * glm::vec3(0.0f, 0.0f, -1.0f));
      glm::vec3 upDir = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
      glm::vec3 rightDir = glm::normalize(rot * glm::vec3(1.0f, 0.0f, 0.0f));
      if (!std::isfinite(forward.x) || !std::isfinite(upDir.x) ||
          !std::isfinite(rightDir.x) || glm::length(forward) < 1e-3f)
        return;
      const float alpha = 1.0f;

      auto start = projectToScreen(camObj.position);
      auto end = projectToScreen(camObj.position +
                                 forward * (1.4f * gizmoOverlayScaleClamped));
      auto upTip = projectToScreen(camObj.position +
                                   upDir * (0.6f * gizmoOverlayScaleClamped));
      ImDrawList *dl = ImGui::GetWindowDrawList();
      if (start && end) {
        ImU32 lineCol =
            ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 1.0f, 0.9f * alpha));
        ImU32 headCol =
            ImGui::GetColorU32(ImVec4(0.9f, 1.0f, 1.0f, 0.95f * alpha));
        dl->AddLine(*start, *end, lineCol, 2.5f * gizmoOverlayScaleClamped);
        ImVec2 dir = ImVec2(end->x - start->x, end->y - start->y);
        float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
        if (len > 1.0f) {
          ImVec2 normDir = ImVec2(dir.x / len, dir.y / len);
          ImVec2 left = ImVec2(-normDir.y, normDir.x);
          float head = 10.0f * gizmoOverlayScaleClamped;
          ImVec2 tip = *end;
          ImVec2 p1 = ImVec2(tip.x - normDir.x * head + left.x * head * 0.6f,
                             tip.y - normDir.y * head + left.y * head * 0.6f);
          ImVec2 p2 = ImVec2(tip.x - normDir.x * head - left.x * head * 0.6f,
                             tip.y - normDir.y * head - left.y * head * 0.6f);
          dl->AddTriangleFilled(tip, p1, p2, headCol);
        }
        if (upTip) {
          dl->AddCircleFilled(
              *upTip, 3.0f * gizmoOverlayScaleClamped,
              ImGui::GetColorU32(ImVec4(0.8f, 1.0f, 0.6f, 0.8f * alpha)));
        }
      }

      auto drawWorldLine = [&](const glm::vec3 &a, const glm::vec3 &b,
                               ImU32 color, float thickness) {
        auto sa = projectToScreen(a);
        auto sb = projectToScreen(b);
        if (sa && sb) {
          dl->AddLine(*sa, *sb, color, thickness);
        }
      };

      const bool cameraIs2D = project2DPipeline || camObj.camera.use2D;
      if (cameraIs2D) {
        const float pixelsPerUnit = std::max(1.0f, camObj.camera.pixelsPerUnit);
        const float halfWidth = static_cast<float>(activeGameResolutionWidth) /
                                (2.0f * pixelsPerUnit);
        const float halfHeight =
            static_cast<float>(activeGameResolutionHeight) /
            (2.0f * pixelsPerUnit);
        glm::vec3 center = camObj.position;
        std::array<glm::vec3, 4> corners = {
            center - rightDir * halfWidth + upDir * halfHeight,
            center + rightDir * halfWidth + upDir * halfHeight,
            center + rightDir * halfWidth - upDir * halfHeight,
            center - rightDir * halfWidth - upDir * halfHeight};

        ImU32 boundsCol =
            ImGui::GetColorU32(ImVec4(0.28f, 0.88f, 1.0f, 0.92f * alpha));
        ImU32 diagCol =
            ImGui::GetColorU32(ImVec4(0.28f, 0.88f, 1.0f, 0.45f * alpha));
        for (int i = 0; i < 4; ++i) {
          drawWorldLine(corners[i], corners[(i + 1) % 4], boundsCol,
                        2.0f * gizmoOverlayScaleClamped);
        }
        drawWorldLine(corners[0], corners[2], diagCol,
                      1.2f * gizmoOverlayScaleClamped);
        drawWorldLine(corners[1], corners[3], diagCol,
                      1.2f * gizmoOverlayScaleClamped);
        drawWorldLine(
            center, center + upDir * std::max(0.1f, halfHeight * 0.45f),
            ImGui::GetColorU32(ImVec4(0.94f, 0.98f, 1.0f, 0.88f * alpha)),
            2.0f * gizmoOverlayScaleClamped);

        auto labelAnchor = projectToScreen(
            corners[1] + upDir * (0.08f * gizmoOverlayScaleClamped));
        if (gizmoShowCameraFrustumLabels && labelAnchor) {
          char label[96];
          std::snprintf(label, sizeof(label), "2D %dx%d | %.2fx%.2f",
                        activeGameResolutionWidth, activeGameResolutionHeight,
                        halfWidth * 2.0f, halfHeight * 2.0f);
          ImVec2 textSize = ImGui::CalcTextSize(label);
          ImVec2 pad(4.0f * gizmoOverlayScaleClamped,
                     2.0f * gizmoOverlayScaleClamped);
          ImVec2 bgMin(labelAnchor->x,
                       labelAnchor->y - textSize.y * 0.5f - pad.y);
          ImVec2 bgMax(bgMin.x + textSize.x + pad.x * 2.0f,
                       bgMin.y + textSize.y + pad.y * 2.0f);
          dl->AddRectFilled(
              bgMin, bgMax,
              IM_COL32(16, 24, 34, static_cast<int>(195.0f * alpha)),
              4.0f * gizmoOverlayScaleClamped);
          dl->AddRect(bgMin, bgMax,
                      IM_COL32(120, 200, 240, static_cast<int>(180.0f * alpha)),
                      4.0f * gizmoOverlayScaleClamped, 0, 1.0f);
          dl->AddText(ImVec2(bgMin.x + pad.x, bgMin.y + pad.y),
                      IM_COL32(214, 242, 255, static_cast<int>(235.0f * alpha)),
                      label);
        }
        return;
      }

      const float nearPlane = std::max(0.01f, camObj.camera.nearClip);
      const float farPlane = std::max(nearPlane + 0.05f, camObj.camera.farClip);
      const float drawDepth = std::clamp(farPlane, nearPlane + 0.05f,
                                         14.0f * gizmoOverlayScaleClamped);
      const float fovDeg = std::clamp(
          ResolveCameraVerticalFovDeg(camObj.camera, cameraOverlayAspect), 5.0f,
          170.0f);
      const float tanHalfFov = std::tan(glm::radians(fovDeg) * 0.5f);
      const bool ortho3D =
          camObj.camera.projection == SceneCameraProjection::Orthographic &&
          !camObj.camera.use2D;
      const float orthoHalfH = std::max(0.01f, camObj.camera.orthoSize);
      const float nearHalfH = ortho3D ? orthoHalfH : tanHalfFov * nearPlane;
      const float nearHalfW = nearHalfH * cameraOverlayAspect;
      const float farHalfH = ortho3D ? orthoHalfH : tanHalfFov * drawDepth;
      const float farHalfW = farHalfH * cameraOverlayAspect;

      glm::vec3 nearC = camObj.position + forward * nearPlane;
      glm::vec3 farC = camObj.position + forward * drawDepth;
      std::array<glm::vec3, 4> nearCorners = {
          nearC + upDir * nearHalfH - rightDir * nearHalfW,
          nearC + upDir * nearHalfH + rightDir * nearHalfW,
          nearC - upDir * nearHalfH + rightDir * nearHalfW,
          nearC - upDir * nearHalfH - rightDir * nearHalfW};
      std::array<glm::vec3, 4> farCorners = {
          farC + upDir * farHalfH - rightDir * farHalfW,
          farC + upDir * farHalfH + rightDir * farHalfW,
          farC - upDir * farHalfH + rightDir * farHalfW,
          farC - upDir * farHalfH - rightDir * farHalfW};

      ImU32 frustumLineCol =
          ImGui::GetColorU32(ImVec4(0.55f, 0.9f, 1.0f, 0.78f * alpha));
      ImU32 frustumFaintCol =
          ImGui::GetColorU32(ImVec4(0.55f, 0.9f, 1.0f, 0.42f * alpha));
      for (int i = 0; i < 4; ++i) {
        drawWorldLine(nearCorners[i], nearCorners[(i + 1) % 4], frustumFaintCol,
                      1.4f * gizmoOverlayScaleClamped);
        drawWorldLine(farCorners[i], farCorners[(i + 1) % 4], frustumLineCol,
                      1.6f * gizmoOverlayScaleClamped);
        drawWorldLine(camObj.position, farCorners[i], frustumFaintCol,
                      1.25f * gizmoOverlayScaleClamped);
      }
      drawWorldLine(camObj.position, farC, frustumLineCol,
                    1.8f * gizmoOverlayScaleClamped);

      auto labelAnchor = projectToScreen(
          farC + upDir * (farHalfH + 0.05f * gizmoOverlayScaleClamped) +
          rightDir * (farHalfW + 0.05f * gizmoOverlayScaleClamped));
      if (gizmoShowCameraFrustumLabels && labelAnchor) {
        char label[96];
        std::snprintf(label, sizeof(label), "FOV %.0f | %.2fx%.2f @ %.1f",
                      fovDeg, farHalfW * 2.0f, farHalfH * 2.0f, drawDepth);
        ImVec2 textSize = ImGui::CalcTextSize(label);
        ImVec2 pad(4.0f * gizmoOverlayScaleClamped,
                   2.0f * gizmoOverlayScaleClamped);
        ImVec2 bgMin(labelAnchor->x,
                     labelAnchor->y - textSize.y * 0.5f - pad.y);
        ImVec2 bgMax(bgMin.x + textSize.x + pad.x * 2.0f,
                     bgMin.y + textSize.y + pad.y * 2.0f);
        dl->AddRectFilled(
            bgMin, bgMax,
            IM_COL32(16, 24, 34, static_cast<int>(195.0f * alpha)),
            4.0f * gizmoOverlayScaleClamped);
        dl->AddRect(bgMin, bgMax,
                    IM_COL32(120, 200, 240, static_cast<int>(180.0f * alpha)),
                    4.0f * gizmoOverlayScaleClamped, 0, 1.0f);
        dl->AddText(ImVec2(bgMin.x + pad.x, bgMin.y + pad.y),
                    IM_COL32(214, 242, 255, static_cast<int>(235.0f * alpha)),
                    label);
      }
    };

    if (showSceneGizmos && gizmoShowCameraOverlays && !worldUiEditing) {
      for (const auto &obj : sceneObjects) {
        if (obj.hasCamera) {
          drawCameraDirection(obj);
        }
      }
    }

    // Light visualization overlays
    auto drawLightOverlays = [&](const SceneObject &lightObj) {
      if (!IsObjectEnabledInHierarchy(lightObj) || !lightObj.light.enabled)
        return;
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImU32 col = ImGui::GetColorU32(ImVec4(1.0f, 0.9f, 0.4f, 0.7f));
      ImU32 faint = ImGui::GetColorU32(ImVec4(1.0f, 0.9f, 0.4f, 0.25f));
      ImU32 handleCol = ImGui::GetColorU32(ImVec4(1.0f, 0.9f, 0.4f, 0.9f));
      const float lw = 1.5f * gizmoOverlayScaleClamped;
      const float lw2 = 2.0f * gizmoOverlayScaleClamped;

      auto forwardFromRotation = [](const SceneObject &obj) {
        glm::mat4 rotation(1.0f);
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.x),
                               glm::vec3(1.0f, 0.0f, 0.0f));
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.y),
                               glm::vec3(0.0f, 1.0f, 0.0f));
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.z),
                               glm::vec3(0.0f, 0.0f, 1.0f));
        glm::vec3 f = glm::normalize(
            glm::vec3(rotation * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
        if (glm::length(f) < 1e-3f || !std::isfinite(f.x) ||
            !std::isfinite(f.y) || !std::isfinite(f.z)) {
          f = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        return f;
      };

      // Helper: draw a circle in 3D space (world-space center + two basis
      // vectors + radius)
      auto drawWorldCircle = [&](const glm::vec3 &center, const glm::vec3 &ax1,
                                 const glm::vec3 &ax2, float radius,
                                 ImU32 color, float thickness, int segs = 32) {
        ImVec2 prev;
        bool first = true;
        for (int i = 0; i <= segs; ++i) {
          float a = (float)i / segs * 2.0f * PI;
          glm::vec3 p =
              center + ax1 * std::cos(a) * radius + ax2 * std::sin(a) * radius;
          auto sp = projectToScreen(p);
          if (!sp) {
            first = true;
            continue;
          }
          if (first) {
            prev = *sp;
            first = false;
            continue;
          }
          dl->AddLine(prev, *sp, color, thickness);
          prev = *sp;
        }
      };

      if (lightObj.light.type == LightType::Directional) {
        glm::vec3 dir = forwardFromRotation(lightObj);
        glm::vec3 up =
            glm::abs(dir.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 right = glm::normalize(glm::cross(dir, up));
        glm::vec3 u = glm::normalize(glm::cross(right, dir));

        const float radius = 0.5f;
        const float length = 1.6f;
        glm::vec3 origin = lightObj.position;
        glm::vec3 end = origin + dir * length;

        drawWorldCircle(origin, right, u, radius, col, lw);
        drawWorldCircle(end, right, u, radius, col, lw);

        const int numRays = 8;
        for (int i = 0; i < numRays; ++i) {
          float a = (float)i / numRays * 2.0f * PI;
          glm::vec3 offset =
              right * std::cos(a) * radius + u * std::sin(a) * radius;
          auto sStart = projectToScreen(origin + offset);
          auto sEnd = projectToScreen(end + offset);
          if (sStart && sEnd)
            dl->AddLine(*sStart, *sEnd, col, lw);
        }

        auto sc = projectToScreen(origin);
        auto se = projectToScreen(end);
        if (sc && se)
          dl->AddLine(*sc, *se, faint, lw);
      } else if (lightObj.light.type == LightType::Point) {
        const glm::vec3 c = lightObj.position;
        const float r = lightObj.light.range;
        drawWorldCircle(c, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), r, col, lw);
        drawWorldCircle(c, glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), r, col, lw);
        drawWorldCircle(c, glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), r, col, lw);

        const float hSz = 4.0f * gizmoOverlayScaleClamped;
        const glm::vec3 axes[6] = {
            glm::vec3(r, 0, 0),  glm::vec3(-r, 0, 0), glm::vec3(0, r, 0),
            glm::vec3(0, -r, 0), glm::vec3(0, 0, r),  glm::vec3(0, 0, -r),
        };
        for (const auto &a : axes) {
          auto sp = projectToScreen(c + a);
          if (sp)
            dl->AddRectFilled(ImVec2(sp->x - hSz * 0.5f, sp->y - hSz * 0.5f),
                              ImVec2(sp->x + hSz * 0.5f, sp->y + hSz * 0.5f),
                              handleCol);
        }
      } else if (lightObj.light.type == LightType::Spot) {
        glm::vec3 dir = forwardFromRotation(lightObj);
        glm::vec3 tip = lightObj.position;
        glm::vec3 end = tip + dir * lightObj.light.range;
        float innerRad = glm::tan(glm::radians(lightObj.light.innerAngle)) *
                         lightObj.light.range;
        float outerRad = glm::tan(glm::radians(lightObj.light.outerAngle)) *
                         lightObj.light.range;

        glm::vec3 up =
            glm::abs(dir.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 right = glm::normalize(glm::cross(dir, up));
        up = glm::normalize(glm::cross(right, dir));

        drawWorldCircle(end, right, up, outerRad, faint, lw);
        drawWorldCircle(end, right, up, innerRad, col, lw);

        auto sTip = projectToScreen(tip);
        const float hSz = 4.0f * gizmoOverlayScaleClamped;
        if (sTip) {
          const glm::vec3 spokes[4] = {right, -right, up, -up};
          for (const auto &s : spokes) {
            glm::vec3 edgePt = end + s * outerRad;
            auto sEdge = projectToScreen(edgePt);
            if (sEdge) {
              dl->AddLine(*sTip, *sEdge, col, lw2);
              dl->AddRectFilled(
                  ImVec2(sEdge->x - hSz * 0.5f, sEdge->y - hSz * 0.5f),
                  ImVec2(sEdge->x + hSz * 0.5f, sEdge->y + hSz * 0.5f),
                  handleCol);
            }
          }
        }

        auto sEnd = projectToScreen(end);
        if (sTip && sEnd) {
          dl->AddLine(*sTip, *sEnd, faint, lw);
          dl->AddRectFilled(ImVec2(sEnd->x - hSz * 0.5f, sEnd->y - hSz * 0.5f),
                            ImVec2(sEnd->x + hSz * 0.5f, sEnd->y + hSz * 0.5f),
                            handleCol);
        }

      } else if (lightObj.light.type == LightType::Area) {
        glm::vec3 n = forwardFromRotation(lightObj);
        glm::vec3 up =
            glm::abs(n.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 tangent = glm::normalize(glm::cross(up, n));
        glm::vec3 bitangent = glm::cross(n, tangent);
        glm::vec2 half = lightObj.light.size * 0.5f;
        glm::vec3 c = lightObj.position;
        glm::vec3 corners[4] = {c + tangent * half.x + bitangent * half.y,
                                c - tangent * half.x + bitangent * half.y,
                                c - tangent * half.x - bitangent * half.y,
                                c + tangent * half.x - bitangent * half.y};
        ImVec2 projected[4];
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
          auto p = projectToScreen(corners[i]);
          if (!p) {
            ok = false;
            break;
          }
          projected[i] = *p;
        }
        if (ok) {
          for (int i = 0; i < 4; ++i) {
            dl->AddLine(projected[i], projected[(i + 1) % 4], col,
                        2.0f * gizmoOverlayScaleClamped);
          }
          // normal indicator
          auto cproj = projectToScreen(c);
          auto nproj =
              projectToScreen(c + n * glm::max(lightObj.light.range, 0.5f));
          if (cproj && nproj) {
            dl->AddLine(*cproj, *nproj, col, 2.0f * gizmoOverlayScaleClamped);
            dl->AddCircleFilled(*nproj, 4.0f * gizmoOverlayScaleClamped, col);
          }
        }
      }
    };

    if (showSceneGizmos && gizmoShowLightOverlays && !worldUiEditing) {
      for (const auto &obj : sceneObjects) {
        if (!obj.hasLight)
          continue;
        if (obj.light.type == LightType::Directional ||
            obj.light.type == LightType::Point ||
            obj.light.type == LightType::Spot ||
            obj.light.type == LightType::Area) {
          drawLightOverlays(obj);
        }
      }
    }

    auto drawArmatureOverlays =
        [&](const SceneObject &skinnedObj,
            const std::unordered_map<int, const SceneObject *> &idLookup) {
          if (!IsObjectEnabledInHierarchy(skinnedObj) ||
              !skinnedObj.hasSkeletalAnimation || !skinnedObj.skeletal.enabled)
            return;
          if (skinnedObj.skeletal.boneNodeIds.empty() &&
              skinnedObj.skeletal.armatureNodeIds.empty())
            return;

          std::unordered_set<int> boneIds;
          const std::vector<int> &overlayNodeIds =
              skinnedObj.skeletal.armatureNodeIds.empty()
                  ? skinnedObj.skeletal.boneNodeIds
                  : skinnedObj.skeletal.armatureNodeIds;
          for (int id : overlayNodeIds) {
            if (id >= 0)
              boneIds.insert(id);
          }
          if (boneIds.empty())
            return;

          if (boneIds.size() <= 2 && skinnedObj.skeletal.skeletonRootId >= 0) {
            std::vector<int> stack;
            stack.push_back(skinnedObj.skeletal.skeletonRootId);
            while (!stack.empty()) {
              int currentId = stack.back();
              stack.pop_back();
              auto it = idLookup.find(currentId);
              if (it == idLookup.end() || !it->second)
                continue;
              const SceneObject *node = it->second;
              if (node->type == ObjectType::Empty) {
                boneIds.insert(node->id);
              }
              for (int childId : node->childIds) {
                if (childId >= 0) {
                  stack.push_back(childId);
                }
              }
            }
          }

          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 lineCol = ImGui::GetColorU32(ImVec4(0.55f, 0.9f, 0.8f, 0.75f));
          ImU32 nodeCol = ImGui::GetColorU32(ImVec4(0.85f, 0.95f, 0.9f, 0.9f));
          ImU32 rootCol = ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.45f, 0.95f));

          for (int id : boneIds) {
            auto it = idLookup.find(id);
            if (it == idLookup.end() || !it->second)
              continue;
            const SceneObject *boneObj = it->second;
            if (!IsObjectEnabledInHierarchy(*boneObj))
              continue;
            auto boneScreen = projectToScreen(boneObj->position);
            if (!boneScreen)
              continue;

            bool isRoot = boneObj->parentId < 0 ||
                          boneIds.find(boneObj->parentId) == boneIds.end();
            float radius = isRoot ? 4.5f : 3.0f;
            dl->AddCircleFilled(*boneScreen, radius,
                                isRoot ? rootCol : nodeCol);

            if (boneObj->parentId >= 0) {
              auto parentIt = idLookup.find(boneObj->parentId);
              if (parentIt != idLookup.end() && parentIt->second &&
                  IsObjectEnabledInHierarchy(*parentIt->second) &&
                  boneIds.find(boneObj->parentId) != boneIds.end()) {
                auto parentScreen = projectToScreen(parentIt->second->position);
                if (parentScreen) {
                  dl->AddLine(*parentScreen, *boneScreen, lineCol, 2.0f);
                }
              }
            }
          }
        };

    auto drawRig25DOverlays =
        [&](const SceneObject &obj,
            const std::unordered_map<int, const SceneObject *> &idLookup) {
          if (!project25DPipeline || !IsObjectEnabledInHierarchy(obj)) {
            return;
          }
          if (!obj.hasRig25DRoot && !obj.hasRig25DNode) {
            return;
          }

          auto screen = projectToScreen(obj.position);
          if (!screen) {
            return;
          }

          ImDrawList *dl = ImGui::GetWindowDrawList();
          const bool isRoot = obj.hasRig25DRoot;
          const ImU32 fillCol = isRoot
                                    ? ImGui::GetColorU32(ImVec4(1.0f, 0.78f,
                                                                0.34f, 0.95f))
                                    : ImGui::GetColorU32(ImVec4(0.38f, 0.84f,
                                                                1.0f, 0.95f));
          const ImU32 outlineCol = isRoot
                                       ? ImGui::GetColorU32(ImVec4(1.0f, 0.92f,
                                                                   0.62f, 0.95f))
                                       : ImGui::GetColorU32(ImVec4(0.76f, 0.94f,
                                                                   1.0f, 0.92f));
          const float radius = isRoot ? 5.5f : 4.5f;
          dl->AddCircleFilled(*screen, radius, fillCol, 16);
          dl->AddCircle(*screen, radius + 3.5f, outlineCol, 20, 1.4f);

          if (obj.parentId >= 0) {
            auto parentIt = idLookup.find(obj.parentId);
            if (parentIt != idLookup.end() && parentIt->second) {
              const SceneObject *parent = parentIt->second;
              if (parent->hasRig25DRoot || parent->hasRig25DNode) {
                auto parentScreen = projectToScreen(parent->position);
                if (parentScreen) {
                  dl->AddLine(*parentScreen, *screen, outlineCol, 1.8f);
                }
              }
            }
          }

          const char *marker = isRoot ? "R" : "N";
          dl->AddText(ImVec2(screen->x + 8.0f, screen->y - 9.0f), outlineCol,
                      marker);
        };

    auto resolveMainObjectType = [](const SceneObject &obj) -> ObjectType {
      if (obj.hasRenderer) {
        const ObjectType mappedType =
            MapEnumToObjectType(obj.renderType, kRenderTypeMainObjectMap);
        if (mappedType != ObjectType::Empty) {
          return mappedType;
        }
      }
      if (obj.hasUI) {
        ObjectType mappedType =
            MapEnumToObjectType(obj.ui.type, kUiTypeMainObjectMap);
        if (mappedType == ObjectType::Sprite2D &&
            obj.type == ObjectType::Sprite25D) {
          mappedType = ObjectType::Sprite25D;
        }
        if (mappedType != ObjectType::Empty) {
          return mappedType;
        }
      }
      if (obj.hasLight) {
        const ObjectType mappedType =
            MapEnumToObjectType(obj.light.type, kLightTypeMainObjectMap);
        if (mappedType != ObjectType::Empty) {
          return mappedType;
        }
      }
      if (obj.hasCamera)
        return ObjectType::Camera;
      if (obj.hasPostFX)
        return ObjectType::PostFXNode;
      return ObjectType::Empty;
    };

    auto isMainTypeGizmoEnabled = [&](const SceneObject &obj) {
      if (!IsObjectEnabledInHierarchy(obj)) {
        return false;
      }
      const ObjectType type = resolveMainObjectType(obj);
      switch (type) {
      case ObjectType::DirectionalLight:
      case ObjectType::PointLight:
      case ObjectType::SpotLight:
      case ObjectType::AreaLight:
        // Disabled lights stay visible as the unlit bulb.
        return obj.hasLight;
      case ObjectType::PostFXNode:
        return obj.hasPostFX && obj.postFx.enabled;
      case ObjectType::Camera:
      case ObjectType::UIText:
        return true;
      default:
        return false;
      }
    };

    struct GizmoIconImage {
      ImTextureID id = static_cast<ImTextureID>(0);
      bool flipY = false;
    };

    auto getMainTypeGizmoIconPath = [&](const SceneObject &obj) -> const char * {
      switch (resolveMainObjectType(obj)) {
      case ObjectType::Camera:
        return "Resources/Engine-Root/Gizmos/Camera view.png";
      case ObjectType::DirectionalLight:
      case ObjectType::PointLight:
      case ObjectType::SpotLight:
      case ObjectType::AreaLight:
        return obj.light.enabled
                   ? "Resources/Engine-Root/Gizmos/Light bulb On.png"
                   : "Resources/Engine-Root/Gizmos/Light bulb Off.png";
      case ObjectType::UIText:
        return "Resources/Engine-Root/Gizmos/Dynamic Text.png";
      default:
        return nullptr;
      }
    };

    auto loadGizmoIcon = [&](const char *path) -> GizmoIconImage {
      if (!path || !*path)
        return {};
      if (rendererInitialized) {
        if (Texture *icon = renderer.getTexture(path); icon && icon->GetID()) {
          return {static_cast<ImTextureID>(icon->GetID()), true};
        }
        return {};
      }
      if (hasVulkanSceneTexture && vulkanRenderer) {
        ImTextureID vkIcon = vulkanRenderer->getOrCreateUIImage(path);
        if (vkIcon != static_cast<ImTextureID>(0)) {
          return {vkIcon, false};
        }
      }
      return {};
    };

    auto drawMainTypeGizmoIcon = [&](const SceneObject &obj) {
      if (!IsObjectEnabledInHierarchy(obj))
        return;
      const bool wantsCustomIcon =
          obj.editorIconShowInViewport && !obj.editorIconPath.empty();
      GizmoIconImage icon;
      if (wantsCustomIcon) {
        icon = loadGizmoIcon(obj.editorIconPath.c_str());
      } else if (isMainTypeGizmoEnabled(obj)) {
        icon = loadGizmoIcon(getMainTypeGizmoIconPath(obj));
      }
      const bool hasIcon = icon.id != static_cast<ImTextureID>(0);
      const GizmoIconImage lockIcon =
          gizmoShowInvariableLocks && obj.IsInvariable
              ? loadGizmoIcon(
                    "Resources/Engine-Root/Gizmos/Invariable Object Lock.png")
              : GizmoIconImage{};
      const bool hasLock = lockIcon.id != static_cast<ImTextureID>(0);
      if (!hasIcon && !hasLock)
        return;

      auto screen = projectToScreen(obj.position);
      if (!screen)
        return;

      const bool isSelected =
          std::find(selectedObjectIds.begin(), selectedObjectIds.end(),
                    obj.id) != selectedObjectIds.end();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      auto addIconImage = [&](const GizmoIconImage &image, const ImVec2 &min,
                              const ImVec2 &max, ImU32 tint) {
        const ImVec2 uvMin = image.flipY ? ImVec2(0, 1) : ImVec2(0, 0);
        const ImVec2 uvMax = image.flipY ? ImVec2(1, 0) : ImVec2(1, 1);
        dl->AddImage(image.id, min, max, uvMin, uvMax, tint);
      };
      const ImU32 lockTint = IM_COL32(255, 255, 255, 232);

      if (!hasIcon) {
        // Objects without a type icon (meshes, empties) get the lock alone.
        const float lockHalf =
            (isSelected ? 22.0f : 17.0f) * gizmoIconScaleClamped;
        const ImVec2 lockMin(screen->x - lockHalf, screen->y - lockHalf);
        const ImVec2 lockMax(screen->x + lockHalf, screen->y + lockHalf);
        addIconImage(lockIcon, lockMin, lockMax, lockTint);
        if (isSelected) {
          dl->AddRect(lockMin, lockMax, IM_COL32(255, 255, 255, 170),
                      4.0f * gizmoIconScaleClamped, 0, 1.5f);
        }
        return;
      }

      const float size = (isSelected ? 74.0f : 56.0f) * gizmoIconScaleClamped;
      const float half = size * 0.5f;
      const ImVec2 min(screen->x - half, screen->y - half);
      const ImVec2 max(screen->x + half, screen->y + half);
      const ImU32 tint = ImGui::ColorConvertFloat4ToU32(
          ImVec4(obj.editorIconTint.r, obj.editorIconTint.g, obj.editorIconTint.b,
                 obj.editorIconTint.a * 0.91f));

      addIconImage(icon, min, max, tint);
      if (isSelected) {
        dl->AddRect(min, max, IM_COL32(255, 255, 255, 170),
                    5.0f * gizmoIconScaleClamped, 0, 1.8f);
      }
      if (hasLock) {
        // Bottom-left badge; the top-right corner holds the intensity label.
        const float lockSize = size * 0.42f;
        addIconImage(lockIcon, ImVec2(min.x, max.y - lockSize),
                     ImVec2(min.x + lockSize, max.y), lockTint);
      }
      if (obj.hasLight && gizmoShowLightIntensityLabels) {
        char brightness[24];
        std::snprintf(brightness, sizeof(brightness), "%.3g",
                      obj.light.intensity);
        ImVec2 textSize = ImGui::CalcTextSize(brightness);
        const ImVec2 pad(5.0f * gizmoIconScaleClamped,
                         2.0f * gizmoIconScaleClamped);
        ImVec2 badgeMin(max.x - (textSize.x + pad.x * 2.0f) -
                            1.5f * gizmoIconScaleClamped,
                        min.y + 1.5f * gizmoIconScaleClamped);
        ImVec2 badgeMax(max.x - 1.5f, badgeMin.y + textSize.y + pad.y * 2.0f);
        int bgA = 212;
        int fgA = 242;
        dl->AddRectFilled(badgeMin, badgeMax, IM_COL32(24, 28, 34, bgA),
                          5.0f * gizmoIconScaleClamped);
        dl->AddRect(badgeMin, badgeMax, IM_COL32(255, 214, 118, fgA),
                    5.0f * gizmoIconScaleClamped, 0, 1.0f);
        dl->AddText(ImVec2(badgeMin.x + pad.x, badgeMin.y + pad.y),
                    IM_COL32(255, 238, 180, fgA), brightness);
      }
    };
    auto drawPlayerControllerGroundProbe = [&]() {
      if (activePlayerId < 0 ||
          playerControllerGroundProbeDebug.playerId != activePlayerId) {
        return;
      }

      const SceneObject *playerObj = nullptr;
      for (const auto &obj : sceneObjects) {
        if (obj.id == activePlayerId) {
          playerObj = &obj;
          break;
        }
      }
      if (!playerObj || !IsObjectEnabledInHierarchy(*playerObj) ||
          !playerObj->hasPlayerController ||
          !playerObj->playerController.enabled) {
        return;
      }

      auto screenStart = projectToScreen(playerControllerGroundProbeDebug.rayStart);
      auto screenEnd = projectToScreen(playerControllerGroundProbeDebug.hasHit
                                           ? playerControllerGroundProbeDebug.hitPos
                                           : playerControllerGroundProbeDebug.rayEnd);
      if (!screenStart || !screenEnd) {
        return;
      }

      ImDrawList *dl = ImGui::GetWindowDrawList();
      const ImU32 lineColor = playerControllerGroundProbeDebug.hasHit
                                  ? IM_COL32(92, 255, 174, 220)
                                  : IM_COL32(255, 183, 92, 220);
      const ImU32 startColor = IM_COL32(210, 245, 255, 230);
      const ImU32 endColor = playerControllerGroundProbeDebug.hasHit
                                 ? IM_COL32(92, 255, 174, 240)
                                 : IM_COL32(255, 183, 92, 240);
      dl->AddLine(*screenStart, *screenEnd, lineColor, 2.4f);
      dl->AddCircleFilled(*screenStart, 3.5f, startColor);
      dl->AddCircleFilled(*screenEnd, 3.5f, endColor);
    };
    auto drawWireCube = [&](const glm::mat4 &worldFromLocal,
                            const glm::vec3 &localMin,
                            const glm::vec3 &localMax, ImU32 color,
                            float thickness) {
      std::array<glm::vec3, 8> corners = {
          glm::vec3(localMin.x, localMin.y, localMin.z),
          glm::vec3(localMax.x, localMin.y, localMin.z),
          glm::vec3(localMax.x, localMax.y, localMin.z),
          glm::vec3(localMin.x, localMax.y, localMin.z),
          glm::vec3(localMin.x, localMin.y, localMax.z),
          glm::vec3(localMax.x, localMin.y, localMax.z),
          glm::vec3(localMax.x, localMax.y, localMax.z),
          glm::vec3(localMin.x, localMax.y, localMax.z)};
      std::array<ImVec2, 8> projected = {};
      for (size_t i = 0; i < corners.size(); ++i) {
        glm::vec3 world =
            glm::vec3(worldFromLocal * glm::vec4(corners[i], 1.0f));
        auto p = projectToScreen(world);
        if (!p.has_value())
          return;
        projected[i] = *p;
      }
      const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      ImDrawList *dl = ImGui::GetWindowDrawList();
      for (const auto &e : edges) {
        dl->AddLine(projected[e[0]], projected[e[1]], color, thickness);
      }
    };
    auto drawSelectionColliderBounds = [&](const SceneObject &obj, ImU32 color,
                                           float thickness) {
      auto composeNoScale = [](const SceneObject &o) {
        glm::mat4 m(1.0f);
        m = glm::translate(m, o.position);
        m = glm::rotate(m, glm::radians(o.rotation.x),
                        glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(o.rotation.y),
                        glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, glm::radians(o.rotation.z),
                        glm::vec3(0.0f, 0.0f, 1.0f));
        return m;
      };
      auto composeWithScale = [](const SceneObject &o) {
        glm::mat4 m(1.0f);
        m = glm::translate(m, o.position);
        m = glm::rotate(m, glm::radians(o.rotation.x),
                        glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(o.rotation.y),
                        glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, glm::radians(o.rotation.z),
                        glm::vec3(0.0f, 0.0f, 1.0f));
        m = glm::scale(m, o.scale);
        return m;
      };

      if (obj.hasCollider && obj.collider.enabled) {
        glm::vec3 localMin(-0.5f);
        glm::vec3 localMax(0.5f);
        glm::mat4 world = composeNoScale(obj);
        world = glm::translate(world, obj.collider.offset);
        switch (obj.collider.type) {
        case ColliderType::Box:
          world = glm::scale(world, obj.collider.boxSize);
          break;
        case ColliderType::Capsule: {
          const float capsuleRadius = std::max(
              0.01f, std::max(obj.collider.boxSize.x, obj.collider.boxSize.z) * 0.5f);
          const float capsuleHalfHeight =
              std::max(0.05f, obj.collider.boxSize.y - capsuleRadius * 2.0f) * 0.5f +
              capsuleRadius;
          localMin = glm::vec3(-capsuleRadius, -capsuleHalfHeight, -capsuleRadius);
          localMax = glm::vec3(capsuleRadius, capsuleHalfHeight, capsuleRadius);
          break;
        }
        case ColliderType::Sphere:
          world = glm::scale(world, glm::vec3(std::max({obj.collider.boxSize.x,
                                                        obj.collider.boxSize.y,
                                                        obj.collider.boxSize.z})));
          break;
        case ColliderType::Mesh:
        case ColliderType::ConvexMesh: {
          if (obj.hasRenderer && obj.renderType == RenderType::OBJMesh &&
              obj.meshId >= 0) {
            if (const auto *info = g_objLoader.getMeshInfo(obj.meshId)) {
              if (info->boundsMin.x < info->boundsMax.x) {
                localMin = info->boundsMin;
                localMax = info->boundsMax;
              }
            }
          } else if (obj.hasRenderer && obj.renderType == RenderType::Model &&
                     obj.meshId >= 0) {
            if (const auto *info = getModelLoader().getMeshInfo(obj.meshId)) {
              if (info->boundsMin.x < info->boundsMax.x) {
                localMin = info->boundsMin;
                localMax = info->boundsMax;
              }
            }
          }
          world = glm::scale(world, obj.scale);
          break;
        }
        }
        drawWireCube(world, localMin, localMax, color, thickness);
      }

      if (obj.hasCollider2D && obj.collider2D.enabled) {
        glm::mat4 world = composeWithScale(obj);
        auto drawPolylineLocal = [&](const std::vector<glm::vec3> &pts,
                                     bool closed) {
          if (pts.size() < 2)
            return;
          std::vector<ImVec2> projected;
          projected.reserve(pts.size());
          for (const auto &p : pts) {
            auto sp = projectToScreen(glm::vec3(world * glm::vec4(p, 1.0f)));
            if (!sp.has_value())
              return;
            projected.push_back(*sp);
          }
          ImDrawList *dl = ImGui::GetWindowDrawList();
          for (size_t i = 1; i < projected.size(); ++i) {
            dl->AddLine(projected[i - 1], projected[i], color, thickness);
          }
          if (closed && projected.size() > 2) {
            dl->AddLine(projected.back(), projected.front(), color, thickness);
          }
        };

        if (obj.collider2D.type == Collider2DType::Box) {
          glm::vec2 half = obj.collider2D.boxSize * 0.5f;
          std::vector<glm::vec3> pts = {
              glm::vec3(-half.x + obj.collider2D.offset.x,
                        -half.y + obj.collider2D.offset.y, 0.0f),
              glm::vec3(half.x + obj.collider2D.offset.x,
                        -half.y + obj.collider2D.offset.y, 0.0f),
              glm::vec3(half.x + obj.collider2D.offset.x,
                        half.y + obj.collider2D.offset.y, 0.0f),
              glm::vec3(-half.x + obj.collider2D.offset.x,
                        half.y + obj.collider2D.offset.y, 0.0f)};
          drawPolylineLocal(pts, true);
        } else if (!obj.collider2D.points.empty()) {
          std::vector<glm::vec3> pts;
          pts.reserve(obj.collider2D.points.size());
          for (const auto &p : obj.collider2D.points) {
            pts.emplace_back(p.x + obj.collider2D.offset.x,
                             p.y + obj.collider2D.offset.y, 0.0f);
          }
          drawPolylineLocal(pts,
                            obj.collider2D.type == Collider2DType::Polygon ||
                                obj.collider2D.closed);
        }
      }
    };

    const float toolbarPadding = 6.0f;
    const float toolbarSpacing = 5.0f;
    const ImVec2 gizmoIconButtonSize(32.0f, 24.0f);
    std::unordered_map<int, const SceneObject *> idLookup;
    idLookup.reserve(sceneObjects.size());
    for (const auto &obj : sceneObjects) {
      idLookup.emplace(obj.id, &obj);
    }
    std::vector<int> colliderPreviewRoots = selectedObjectIds;
    if (colliderPreviewRoots.empty() && selectedObjectId >= 0) {
      colliderPreviewRoots.push_back(selectedObjectId);
    }
    if (!worldUiEditing && !colliderPreviewRoots.empty()) {
      for (int id : colliderPreviewRoots) {
        auto it = idLookup.find(id);
        if (it == idLookup.end() || !it->second)
          continue;
        const SceneObject &node = *it->second;
        if (!(node.hasCollider2D && node.collider2D.enabled))
          continue;
        drawSelectionColliderBounds(
            node, ImGui::GetColorU32(ImVec4(0.24f, 0.95f, 1.0f, 0.95f)), 2.4f);
      }
    }
    if (collisionWireframe && !worldUiEditing) {
      std::unordered_set<int> rootSet(colliderPreviewRoots.begin(),
                                      colliderPreviewRoots.end());
      std::unordered_set<int> visited;
      std::vector<int> stack = colliderPreviewRoots;
      while (!stack.empty()) {
        int id = stack.back();
        stack.pop_back();
        if (!visited.insert(id).second)
          continue;
        auto it = idLookup.find(id);
        if (it == idLookup.end() || !it->second)
          continue;
        const SceneObject *node = it->second;

        const bool isRoot = rootSet.find(id) != rootSet.end();
        if (!isRoot) {
          drawSelectionColliderBounds(
              *node, ImGui::GetColorU32(ImVec4(0.38f, 1.0f, 0.78f, 0.85f)),
              1.8f);
        }

        for (int childId : node->childIds) {
          if (childId >= 0) {
            stack.push_back(childId);
          }
        }
      }
    }
    if (showSceneGizmos && !worldUiEditing) {
      for (const auto &obj : sceneObjects) {
        drawArmatureOverlays(obj, idLookup);
      }
      if (project25DPipeline) {
        for (const auto &obj : sceneObjects) {
          drawRig25DOverlays(obj, idLookup);
        }
      }
      for (const auto &obj : sceneObjects) {
        drawMainTypeGizmoIcon(obj);
      }
      drawPlayerControllerGroundProbe();
    }

    auto clearMeshEditSelection = [&]() {
      meshEditSelectedVertices.clear();
      meshEditSelectedEdges.clear();
      meshEditSelectedFaces.clear();
    };
    auto leaveMeshEditMode = [&]() {
      meshEditMode = false;
      meshEditLoaded = false;
      meshEditPath.clear();
      meshEditDirty = false;
      meshEditExtrudeMode = false;
      meshEditSelectionMode = MeshEditSelectionMode::Object;
      meshEditTriangleSelection = false;
      clearMeshEditSelection();
      meshEditAutoObjectId = -1;
      resetCarveToolState();
    };

    if (worldUiEditing) {
      if (meshEditMode) {
        leaveMeshEditMode();
      }
    } else if (selectedRMeshObject) {
      if (meshEditAutoObjectId != selectedObj->id) {
        meshEditLoaded = false;
        meshEditPath.clear();
        meshEditDirty = false;
        meshEditExtrudeMode = false;
        meshEditSelectionMode = MeshEditSelectionMode::Object;
        meshEditTriangleSelection = false;
        clearMeshEditSelection();
        meshEditAutoObjectId = selectedObj->id;
        resetCarveToolState();
        if (selectedObj->hasMapMesh && selectedObj->mapMesh.gridSize > 0.0f) {
          meshEditGridSnap = selectedObj->mapMesh.gridSize;
        }
      }
      meshEditMode = true;
    } else if (meshEditMode || meshEditAutoObjectId != -1) {
      leaveMeshEditMode();
    }

    viewportDrawList->PopClipRect();

    const ImGuiStyle &style = ImGui::GetStyle();
    const ImVec4 panelTone(style.Colors[ImGuiCol_WindowBg].x,
                           style.Colors[ImGuiCol_WindowBg].y,
                           style.Colors[ImGuiCol_WindowBg].z, 1.0f);
    auto flattenOverPanel = [&](ImGuiCol idx, float alpha) {
      const ImVec4 c = style.Colors[idx];
      return ImVec4(panelTone.x + (c.x - panelTone.x) * c.w,
                    panelTone.y + (c.y - panelTone.y) * c.w,
                    panelTone.z + (c.z - panelTone.z) * c.w, alpha);
    };
    ImVec4 bgCol = flattenOverPanel(ImGuiCol_PopupBg, 0.78f);
    ImVec4 baseCol = flattenOverPanel(ImGuiCol_FrameBg, 0.85f);
    ImVec4 hoverCol = flattenOverPanel(ImGuiCol_ButtonHovered, 0.95f);
    ImVec4 activeCol = flattenOverPanel(ImGuiCol_ButtonActive, 1.0f);
    ImVec4 accentCol = style.Colors[ImGuiCol_SliderGrabActive];
    accentCol.w = 1.0f;
    ImVec4 textCol = style.Colors[ImGuiCol_Text];

    ImU32 baseBtn = ImGui::GetColorU32(baseCol);
    ImU32 hoverBtn =
        ImGui::GetColorU32(GizmoToolbar::ScaleColor(hoverCol, 1.05f));
    ImU32 activeBtn =
        ImGui::GetColorU32(GizmoToolbar::ScaleColor(activeCol, 1.08f));
    ImU32 accent = ImGui::GetColorU32(accentCol);
    ImU32 iconColor = ImGui::GetColorU32(ImVec4(0.95f, 0.98f, 1.0f, 0.95f));
    ImU32 toolbarBg = ImGui::GetColorU32(bgCol);
    ImU32 toolbarOutline = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.0f));
    const float toolbarFrameBleed = 2.0f;

    if (showViewportToolbar) {
      const ImGuiWindowFlags toolbarWindowFlags =
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
          ImGuiWindowFlags_NoScrollWithMouse |
          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
          ImGuiWindowFlags_NoNav;
      ImGui::SetCursorScreenPos(ImVec2(toolbarRectMin.x - toolbarFrameBleed,
                                       toolbarRectMin.y - toolbarFrameBleed));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                          ImVec2(toolbarPadding + toolbarFrameBleed,
                                 toolbarPadding + toolbarFrameBleed));
      ImGui::BeginChild("##SceneViewportToolbar",
                        ImVec2(toolbarSizeCache.x + toolbarFrameBleed * 2.0f,
                               toolbarSizeCache.y + toolbarFrameBleed * 2.0f),
                        false, toolbarWindowFlags);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                          ImVec2(toolbarSpacing, toolbarSpacing));
      // The custom buttons below flatten their fills over the panel tone; the stock widgets
      // sharing this toolbar (the snap drag field, the settings arrow, the mode buttons) used
      // the global style colors instead and read as unshaded holes next to them. Hand them
      // the same palette so the shade theme draws them like every other button and frame.
      ImGui::PushStyleColor(ImGuiCol_Button, baseCol);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverCol);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
      // Input frames sit a shade below the buttons so they read as recessed, the way the
      // rest of the editor's fields do.
      ImGui::PushStyleColor(ImGuiCol_FrameBg, GizmoToolbar::ScaleColor(baseCol, 0.80f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, GizmoToolbar::ScaleColor(hoverCol, 0.86f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, GizmoToolbar::ScaleColor(activeCol, 0.92f));
      ImGui::PushStyleColor(ImGuiCol_Text, textCol);
      ImGui::PushClipRect(imageMin, imageMax, false);

      ImDrawList *toolbarDrawList = ImGui::GetWindowDrawList();
      ImDrawListSplitter splitter;
      splitter.Split(toolbarDrawList, 2);
      splitter.SetCurrentChannel(toolbarDrawList, 1);

      ImGui::BeginGroup();

      // Authored toolbar art, point-sampled so IconButton can draw it pixel-for-pixel.
      // Anything without a PNG, or whose PNG fails to load, falls through to the vector
      // drawing inside GizmoToolbar.
      auto resolveToolbarIcon =
          [&](GizmoToolbar::Icon icon) -> GizmoToolbar::IconTexture {
        const char *iconPath = GizmoToolbar::IconTexturePath(icon);
        if (!iconPath || !*iconPath) {
          return {};
        }
        if (rendererInitialized) {
          if (Texture *tex = renderer.getTexture(
                  iconPath, MaterialProperties::TextureFilter::Point);
              tex && tex->GetID()) {
            return {static_cast<ImTextureID>(tex->GetID()),
                    ImVec2(static_cast<float>(tex->GetWidth()),
                           static_cast<float>(tex->GetHeight())),
                    true};
          }
        }
        if (hasVulkanSceneTexture && vulkanRenderer) {
          ImTextureID vkIcon = vulkanRenderer->getOrCreateUIImage(iconPath);
          if (vkIcon != static_cast<ImTextureID>(0)) {
            return {vkIcon, ImVec2(0.0f, 0.0f), false};
          }
        }
        return {};
      };

      auto toolbarIconButton = [&](const char *id, GizmoToolbar::Icon icon,
                                   bool active) {
        return GizmoToolbar::IconButton(id, icon, active, gizmoIconButtonSize,
                                        baseBtn, hoverBtn, activeBtn, accent,
                                        iconColor, resolveToolbarIcon(icon));
      };

      auto gizmoButton = [&](const char *id, GizmoToolbar::Icon icon,
                             ImGuizmo::OPERATION op, const char *tooltip) {
        if (toolbarIconButton(id, icon, mCurrentGizmoOperation == op)) {
          mCurrentGizmoOperation = op;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", tooltip);
        }
      };
      const bool use2DGizmos = worldUiEditing;
      if (use2DGizmos) {
        if (meshEditMode) {
          meshEditMode = false;
          meshEditLoaded = false;
          meshEditPath.clear();
          meshEditDirty = false;
          meshEditExtrudeMode = false;
          meshEditSelectedVertices.clear();
          meshEditSelectedEdges.clear();
          meshEditSelectedFaces.clear();
        }
        if (mCurrentGizmoOperation != ImGuizmo::TRANSLATE &&
            mCurrentGizmoOperation != ImGuizmo::ROTATE &&
            mCurrentGizmoOperation != ImGuizmo::SCALE &&
            mCurrentGizmoOperation != ImGuizmo::BOUNDS) {
          mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
        }
        mCurrentGizmoMode = ImGuizmo::LOCAL;
      }

      gizmoButton("##gizmo_move", GizmoToolbar::Icon::Translate,
                  ImGuizmo::TRANSLATE, "Translate");
      ImGui::SameLine(0.0f, toolbarSpacing);
      gizmoButton("##gizmo_rotate", GizmoToolbar::Icon::Rotate,
                  ImGuizmo::ROTATE, "Rotate");
      ImGui::SameLine(0.0f, toolbarSpacing);
      gizmoButton("##gizmo_scale", GizmoToolbar::Icon::Scale, ImGuizmo::SCALE,
                  "Scale");
      if (use2DGizmos) {
        ImGui::SameLine(0.0f, toolbarSpacing);
        gizmoButton("##gizmo_bounds_2d", GizmoToolbar::Icon::Bounds,
                    ImGuizmo::BOUNDS, "Rect scale");
      }
      if (!use2DGizmos) {
        ImGui::SameLine(0.0f, toolbarSpacing);
        if (meshEditMode) {
          if (GizmoToolbar::ModeButton(
                  "UV", meshEditSelectionMode == MeshEditSelectionMode::UV,
                  ImVec2(38, 24), baseCol, accentCol, textCol)) {
            meshEditSelectionMode = MeshEditSelectionMode::UV;
            meshEditTriangleSelection = false;
          }
          ImGui::SameLine(0.0f, toolbarSpacing * 0.6f);
          if (GizmoToolbar::ModeButton("Extrude", meshEditExtrudeMode,
                                       ImVec2(68, 24), baseCol, accentCol,
                                       textCol)) {
            meshEditExtrudeMode = !meshEditExtrudeMode;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Toggle extrude mode (Shift to extrude, Shift+Ctrl for seams)");
          }
          ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
          if (GizmoToolbar::ModeButton("AutoUV", meshEditAutoUV, ImVec2(62, 24),
                                       baseCol, accentCol, textCol)) {
            meshEditAutoUV = !meshEditAutoUV;
          }
          ImGui::SameLine(0.0f, toolbarSpacing * 0.6f);
          if (GizmoToolbar::TextButton("Mats", false, ImVec2(46, 24), baseBtn,
                                       hoverBtn, activeBtn, accent,
                                       iconColor)) {
            ImGui::OpenPopup("##mesh_edit_material_slots");
          }
          if (ImGui::BeginPopup("##mesh_edit_material_slots")) {
            if (meshEditAsset.materialSlots.empty()) {
              meshEditAsset.materialSlots.push_back("Default");
            }
            if (meshEditAsset.faceMaterialIndices.size() !=
                meshEditAsset.faces.size()) {
              meshEditAsset.faceMaterialIndices.resize(
                  meshEditAsset.faces.size(), 0u);
            }
            meshEditActiveMaterialSlot = std::clamp(
                meshEditActiveMaterialSlot, 0,
                static_cast<int>(meshEditAsset.materialSlots.size()) - 1);

            ImGui::TextUnformatted("Material Slots");
            ImGui::Separator();
            for (size_t slot = 0; slot < meshEditAsset.materialSlots.size();
                 ++slot) {
              char slotName[128];
              std::snprintf(slotName, sizeof(slotName), "%s",
                            meshEditAsset.materialSlots[slot].c_str());
              ImGui::PushID(static_cast<int>(slot));
              if (ImGui::Selectable("##slot_sel",
                                    meshEditActiveMaterialSlot ==
                                        static_cast<int>(slot),
                                    0, ImVec2(18.0f, 18.0f))) {
                meshEditActiveMaterialSlot = static_cast<int>(slot);
              }
              ImGui::SameLine();
              ImGui::SetNextItemWidth(200.0f);
              if (ImGui::InputText("##slot_name", slotName, sizeof(slotName))) {
                meshEditAsset.materialSlots[slot] = slotName;
                meshEditDirty = true;
              }
              ImGui::SameLine();
              if (ImGui::ArrowButton("##up", ImGuiDir_Up) && slot > 0) {
                std::swap(meshEditAsset.materialSlots[slot],
                          meshEditAsset.materialSlots[slot - 1]);
                for (auto &idx : meshEditAsset.faceMaterialIndices) {
                  if (idx == slot)
                    idx = static_cast<uint32_t>(slot - 1);
                  else if (idx == slot - 1)
                    idx = static_cast<uint32_t>(slot);
                }
                meshEditActiveMaterialSlot = static_cast<int>(slot - 1);
                meshEditDirty = true;
              }
              ImGui::SameLine();
              if (ImGui::ArrowButton("##down", ImGuiDir_Down) &&
                  slot + 1 < meshEditAsset.materialSlots.size()) {
                std::swap(meshEditAsset.materialSlots[slot],
                          meshEditAsset.materialSlots[slot + 1]);
                for (auto &idx : meshEditAsset.faceMaterialIndices) {
                  if (idx == slot)
                    idx = static_cast<uint32_t>(slot + 1);
                  else if (idx == slot + 1)
                    idx = static_cast<uint32_t>(slot);
                }
                meshEditActiveMaterialSlot = static_cast<int>(slot + 1);
                meshEditDirty = true;
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("X") &&
                  meshEditAsset.materialSlots.size() > 1) {
                meshEditAsset.materialSlots.erase(
                    meshEditAsset.materialSlots.begin() +
                    static_cast<long>(slot));
                for (auto &idx : meshEditAsset.faceMaterialIndices) {
                  if (idx == slot)
                    idx = 0u;
                  else if (idx > slot)
                    idx -= 1u;
                }
                meshEditActiveMaterialSlot = std::clamp(
                    meshEditActiveMaterialSlot, 0,
                    static_cast<int>(meshEditAsset.materialSlots.size()) - 1);
                meshEditDirty = true;
                ImGui::PopID();
                break;
              }
              ImGui::PopID();
            }
            if (ImGui::Button("Add Slot")) {
              meshEditAsset.materialSlots.push_back(
                  "Material_" +
                  std::to_string(meshEditAsset.materialSlots.size()));
              meshEditActiveMaterialSlot =
                  static_cast<int>(meshEditAsset.materialSlots.size()) - 1;
              meshEditDirty = true;
            }

            ImGui::Spacing();
            if (ImGui::Button("Assign Selected Faces")) {
              if (meshEditAsset.faceMaterialIndices.size() !=
                  meshEditAsset.faces.size()) {
                meshEditAsset.faceMaterialIndices.resize(
                    meshEditAsset.faces.size(), 0u);
              }
              for (int fi : meshEditSelectedFaces) {
                if (fi >= 0 &&
                    fi < static_cast<int>(
                             meshEditAsset.faceMaterialIndices.size())) {
                  meshEditAsset.faceMaterialIndices[fi] =
                      static_cast<uint32_t>(meshEditActiveMaterialSlot);
                }
              }
              meshEditDirty = true;
              if (selectedObj) {
                syncMeshEditToGPU(selectedObj);
              }
            }
            ImGui::SameLine();
            if (ImGui::Button("Select Faces By Slot")) {
              meshEditSelectedFaces.clear();
              if (meshEditAsset.faceMaterialIndices.size() !=
                  meshEditAsset.faces.size()) {
                meshEditAsset.faceMaterialIndices.resize(
                    meshEditAsset.faces.size(), 0u);
              }
              for (size_t fi = 0; fi < meshEditAsset.faceMaterialIndices.size();
                   ++fi) {
                if (meshEditAsset.faceMaterialIndices[fi] ==
                    static_cast<uint32_t>(meshEditActiveMaterialSlot)) {
                  meshEditSelectedFaces.push_back(static_cast<int>(fi));
                }
              }
              meshEditSelectedVertices.clear();
              meshEditSelectedEdges.clear();
              meshEditSelectionMode = MeshEditSelectionMode::Face;
            }
            ImGui::EndPopup();
          }
          if (meshEditSelectionMode == MeshEditSelectionMode::UV) {
            ImGui::SameLine(0.0f, toolbarSpacing * 0.6f);
            if (GizmoToolbar::TextButton("UV Move", false, ImVec2(62, 24),
                                         baseBtn, hoverBtn, activeBtn, accent,
                                         iconColor)) {
              ImGui::OpenPopup("##mesh_edit_uv_tools");
            }
            if (ImGui::BeginPopup("##mesh_edit_uv_tools")) {
              ImGui::TextUnformatted("UV Tools (selected faces)");
              ImGui::Separator();
              ImGui::DragFloat("Move Step", &meshEditUvMoveStep, 0.01f, -10.0f,
                               10.0f, "%.3f");
              ImGui::DragFloat("Scale Step", &meshEditUvScaleStep, 0.01f, 0.01f,
                               10.0f, "%.3f");
              ImGui::DragFloat("Rotate Step", &meshEditUvRotateStep, 1.0f,
                               -180.0f, 180.0f, "%.1f");
              ImGui::TextDisabled(
                  "Right-click in UV mode also shows these tools.");
              ImGui::EndPopup();
            }
          }
          ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
          ImGui::BeginDisabled(!meshEditLoaded || meshEditPath.empty());
          if (GizmoToolbar::TextButton("Save", meshEditDirty, ImVec2(52, 24),
                                       baseBtn, hoverBtn, activeBtn, accent,
                                       iconColor)) {
            std::string err;
            if (!saveMeshEditAsset(err)) {
              addConsoleMessage("Mesh save failed: " + err,
                                ConsoleMessageType::Error);
            } else {
              addConsoleMessage("Saved mesh: " + meshEditPath,
                                ConsoleMessageType::Success);
            }
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(meshEditDirty ? "Save edited mesh to disk"
                                            : "Mesh is up to date");
          }
          ImGui::EndDisabled();
        }
      }
      if (!use2DGizmos) {
        ImGui::SameLine(0.0f, toolbarSpacing);
        gizmoButton("##gizmo_bounds", GizmoToolbar::Icon::Bounds,
                    ImGuizmo::BOUNDS, "Rect scale");
        ImGui::SameLine(0.0f, toolbarSpacing);
        gizmoButton("##gizmo_universal", GizmoToolbar::Icon::Universal,
                    ImGuizmo::UNIVERSAL, "Universal");

        ImGui::SameLine(0.0f, toolbarSpacing * 1.25f);
        if (toolbarIconButton("##mode_local", GizmoToolbar::Icon::LocalMode,
                              mCurrentGizmoMode == ImGuizmo::LOCAL)) {
          mCurrentGizmoMode = ImGuizmo::LOCAL;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Local");
        }
        ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
        if (toolbarIconButton("##mode_world", GizmoToolbar::Icon::WorldMode,
                              mCurrentGizmoMode == ImGuizmo::WORLD)) {
          mCurrentGizmoMode = ImGuizmo::WORLD;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("World");
        }
      }

      if (use2DGizmos) {
        ImGui::SameLine(0.0f, toolbarSpacing * 1.25f);
        const bool freeform = world2DEditMode == World2DEditMode::Freeform;
        if (toolbarIconButton("##mode_freeform",
                              GizmoToolbar::Icon::FreeformMode, freeform)) {
          world2DEditMode = World2DEditMode::Freeform;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Freeform: place and transform sprites and SceneOBJs anywhere.");
        }
        ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
        if (toolbarIconButton("##mode_assemblage",
                              GizmoToolbar::Icon::AssemblageMode, !freeform)) {
          world2DEditMode = World2DEditMode::Assemblage;
          showAssemblageWindow = true;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Assemblage: paint tiles onto a grid.");
        }
      }

      ImGui::SameLine(0.0f, toolbarSpacing);
      bool snapActive = use2DGizmos ? pixelGridSnapEnabled : useSnap;
      if (toolbarIconButton("##snap_toggle", GizmoToolbar::Icon::SnapToggle,
                            snapActive)) {
        if (use2DGizmos) {
          pixelGridSnapEnabled = !pixelGridSnapEnabled;
        } else {
          useSnap = !useSnap;
        }
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(use2DGizmos ? "Pixel snap" : "Snap");
      }

      if (use2DGizmos && pixelGridSnapEnabled) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(86.0f);
        if (ImGui::DragInt("##pixelSnapStep", &pixelGridSnapStep, 0.5f, 1, 64,
                           "%d px")) {
          pixelGridSnapStep = std::clamp(pixelGridSnapStep, 1, 64);
        }
      } else if (!use2DGizmos && useSnap) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (mCurrentGizmoOperation == ImGuizmo::ROTATE) {
          ImGui::DragFloat("##snapAngle", &rotationSnapValue, 1.0f, 1.0f, 90.0f,
                           "%.0f deg");
        } else {
          ImGui::DragFloat("##snapVal", &snapValue[0], 0.1f, 0.1f, 10.0f,
                           "%.1f");
          snapValue[1] = snapValue[2] = snapValue[0];
        }
      }

      bool toolbarEditorSettingsChanged = false;
      ImGui::SameLine(0.0f, toolbarSpacing * 1.25f);
      if (toolbarIconButton("##gizmo_toggle", GizmoToolbar::Icon::GizmoToggle,
                            showSceneGizmos)) {
        showSceneGizmos = !showSceneGizmos;
        toolbarEditorSettingsChanged = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle light/camera scene symbols");
      }
      ImGui::SameLine(0.0f, toolbarSpacing * 0.35f);
      if (ImGui::ArrowButton("##scene_gizmo_settings_arrow", ImGuiDir_Down)) {
        ImGui::OpenPopup("##scene_gizmo_settings_popup");
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Scene gizmo settings");
      }
      if (ImGui::BeginPopup("##scene_gizmo_settings_popup")) {
        ImGui::TextUnformatted("Scene Gizmo Settings");
        ImGui::Separator();

        if (ImGui::Checkbox("Camera Overlays", &gizmoShowCameraOverlays)) {
          toolbarEditorSettingsChanged = true;
        }
        ImGui::BeginDisabled(!gizmoShowCameraOverlays);
        if (ImGui::Checkbox("Camera Frustum Labels",
                            &gizmoShowCameraFrustumLabels)) {
          toolbarEditorSettingsChanged = true;
        }
        ImGui::EndDisabled();

        if (ImGui::Checkbox("Light Overlays", &gizmoShowLightOverlays)) {
          toolbarEditorSettingsChanged = true;
        }
        ImGui::BeginDisabled(!gizmoShowLightOverlays);
        if (ImGui::Checkbox("Light Intensity Labels",
                            &gizmoShowLightIntensityLabels)) {
          toolbarEditorSettingsChanged = true;
        }
        ImGui::EndDisabled();

        if (ImGui::Checkbox("Invariable Locks", &gizmoShowInvariableLocks)) {
          toolbarEditorSettingsChanged = true;
        }

        ImGui::Separator();
        if (ImGui::Checkbox("Viewport Hint Overlay",
                            &showViewportHintOverlay)) {
          toolbarEditorSettingsChanged = true;
        }
        if (ImGui::Checkbox("2D Light Stats Overlay",
                            &showLight2DStatsOverlay)) {
          toolbarEditorSettingsChanged = true;
        }
        if (ImGui::Checkbox("UI Canvas Preview", &uiCanvasPreviewEnabled)) {
          toolbarEditorSettingsChanged = true;
        }

        const char *renderModeLabels[] = {
            "Normal", "Shaded Wireframe", "Wireframe"};
        int renderMode = static_cast<int>(sceneViewportRenderMode);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Viewport Draw Mode", &renderMode, renderModeLabels,
                         IM_ARRAYSIZE(renderModeLabels))) {
          sceneViewportRenderMode =
              static_cast<SceneRenderMode>(std::clamp(renderMode, 0, 2));
          toolbarEditorSettingsChanged = true;
        }

        ImGui::Separator();
        if (ImGui::SliderFloat("Gizmo Icon Size", &sceneGizmoIconScale, 0.4f,
                               3.0f, "%.2fx")) {
          toolbarEditorSettingsChanged = true;
        }
        if (ImGui::SliderFloat("Overlay Scale", &sceneGizmoOverlayScale, 0.4f,
                               3.0f, "%.2fx")) {
          toolbarEditorSettingsChanged = true;
        }
        if (ImGui::Button("Reset Gizmo Defaults")) {
          gizmoShowCameraOverlays = true;
          gizmoShowCameraFrustumLabels = true;
          gizmoShowLightOverlays = true;
          gizmoShowLightIntensityLabels = true;
          gizmoShowInvariableLocks = true;
          showViewportHintOverlay = true;
          showLight2DStatsOverlay = true;
          sceneViewportRenderMode = SceneRenderMode::Normal;
          sceneGizmoIconScale = 1.0f;
          sceneGizmoOverlayScale = 1.0f;
          toolbarEditorSettingsChanged = true;
        }

        ImGui::EndPopup();
      }
      ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
      if (use2DGizmos) {
        if (toolbarIconButton("##grid_toggle_2d",
                              GizmoToolbar::Icon::GridToggle,
                              showUIWorldGrid)) {
          showUIWorldGrid = !showUIWorldGrid;
          toolbarEditorSettingsChanged = true;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Toggle 2D grid");
        }
      } else {
        if (toolbarIconButton("##grid_toggle", GizmoToolbar::Icon::GridToggle,
                              showSceneGrid3D)) {
          showSceneGrid3D = !showSceneGrid3D;
          toolbarEditorSettingsChanged = true;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Toggle 3D grid");
        }
      }
      ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
      const bool uiWorldToggleActive =
          project2DPipeline ? uiCanvasPreviewEnabled
                            : (uiCanvasPreviewEnabled && uiWorldMode);
      if (toolbarIconButton("##ui_world_toggle",
                            GizmoToolbar::Icon::UiWorldToggle,
                            uiWorldToggleActive)) {
        if (project2DPipeline) {
          uiCanvasPreviewEnabled = !uiCanvasPreviewEnabled;
        } else {
          if (!uiCanvasPreviewEnabled) {
            uiCanvasPreviewEnabled = true;
            uiWorldMode = true;
          } else {
            uiWorldMode = !uiWorldMode;
          }
        }
        toolbarEditorSettingsChanged = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(project2DPipeline ? "Toggle UI canvas preview"
                                            : "Toggle 2D UI world overlay");
      }
      if (toolbarEditorSettingsChanged) {
        saveEditorUserSettings();
      }

      ImGui::EndGroup();

      ImVec2 groupMin = ImGui::GetItemRectMin();
      ImVec2 groupMax = ImGui::GetItemRectMax();
      ImVec2 bgMin =
          ImVec2(groupMin.x - toolbarPadding, groupMin.y - toolbarPadding);
      ImVec2 bgMax =
          ImVec2(groupMax.x + toolbarPadding, groupMax.y + toolbarPadding);

      splitter.SetCurrentChannel(toolbarDrawList, 0);
      float rounding = 10.0f;
      ImGui::ShadeRect(toolbarDrawList, bgMin, bgMax, toolbarBg,
                       ImGuiShadeClass_MenuBar, ImGuiShadeState_Normal, rounding,
                       ImDrawFlags_RoundCornersAll);
      toolbarDrawList->AddRect(bgMin, bgMax, toolbarOutline, rounding,
                               ImDrawFlags_RoundCornersAll, 1.5f);

      splitter.Merge(toolbarDrawList);

      toolbarSizeCache = ImVec2(bgMax.x - bgMin.x, bgMax.y - bgMin.y);
      toolbarRectMin = bgMin;
      toolbarRectMax = bgMax;

      const bool toolbarWindowHovered = ImGui::IsWindowHovered(
          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
      if (toolbarWindowHovered) {
        blockSelection = true;
      }
      ImGui::PopClipRect();
      ImGui::PopStyleColor(7);
      ImGui::PopStyleVar();
      ImGui::EndChild();
      ImGui::PopStyleVar();
    }

    if (toolbarAllowed && selectedRMeshObject) {
      struct RMeshModeIcon {
        ImTextureID id = static_cast<ImTextureID>(0);
        bool flipY = false;
      };
      auto resolveRMeshModeIcon = [&](const char *iconPath) -> RMeshModeIcon {
        if (!iconPath || !*iconPath) {
          return {};
        }
        if (rendererInitialized) {
          if (Texture *icon = renderer.getTexture(
                  iconPath, MaterialProperties::TextureFilter::Point);
              icon && icon->GetID()) {
            return {static_cast<ImTextureID>(icon->GetID()), true};
          }
        }
        if (hasVulkanSceneTexture && vulkanRenderer) {
          ImTextureID icon = vulkanRenderer->getOrCreateUIImage(iconPath);
          if (icon != static_cast<ImTextureID>(0)) {
            return {icon, false};
          }
        }
        return {};
      };

      const ImVec2 modeButtonSize = rmeshModeButtonSize;
      const ImVec2 actionsButtonSize = rmeshActionsButtonSize;
      const float modePadding = rmeshModePadding;
      const float modeSpacing = rmeshModeSpacing;
      const ImVec2 modeToolbarSize = rmeshModeToolbarSize;
      const ImVec2 modeToolbarMin = rmeshModeToolbarMin;
      const ImGuiWindowFlags modeToolbarWindowFlags =
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
          ImGuiWindowFlags_NoScrollWithMouse |
          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
          ImGuiWindowFlags_NoNav;

      ImGui::SetCursorScreenPos(modeToolbarMin);
      ImGui::BeginChild("##SceneViewportRMeshModeToolbar", modeToolbarSize,
                        false, modeToolbarWindowFlags);
      ImDrawList *modeDrawList = ImGui::GetWindowDrawList();
      const ImVec2 modeBgMin = modeToolbarMin;
      const ImVec2 modeBgMax(modeToolbarMin.x + modeToolbarSize.x,
                             modeToolbarMin.y + modeToolbarSize.y);
      ImGui::ShadeRect(modeDrawList, modeBgMin, modeBgMax, toolbarBg,
                       ImGuiShadeClass_MenuBar, ImGuiShadeState_Normal, 10.0f,
                       ImDrawFlags_RoundCornersAll);
      modeDrawList->AddRect(modeBgMin, modeBgMax, toolbarOutline, 10.0f,
                            ImDrawFlags_RoundCornersAll, 1.5f);

      auto drawRMeshModeButton =
          [&](const char *id, const char *iconPath, const char *fallback,
              const char *tooltip, bool active) -> bool {
        ImGui::PushID(id);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##btn", modeButtonSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool pressed = ImGui::IsItemClicked();
        if (pressed) ImGui::ReportWidgetFeedback(ImGuiWidgetFeedback_Button);
        ImGui::ReportItemHoverFeedback();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();

        const ImU32 bg = active ? activeBtn : (hovered ? hoverBtn : baseBtn);
        ImGui::ShadeRect(modeDrawList, min, max, bg, ImGuiShadeClass_Button,
                         ImGui::ShadeStateFromInteraction(hovered, ImGui::IsItemActive(), active),
                         8.0f);
        modeDrawList->AddRect(
            min, max,
            ImGui::GetColorU32(
                ImVec4(1.0f, 1.0f, 1.0f, active ? 0.35f : 0.18f)),
            8.0f);

        const RMeshModeIcon icon = resolveRMeshModeIcon(iconPath);
        const float iconInset = 5.0f;
        const ImVec2 iconMin(min.x + iconInset, min.y + iconInset);
        const ImVec2 iconMax(max.x - iconInset, max.y - iconInset);
        const int alpha = active ? 255 : hovered ? 240 : 218;
        if (icon.id != static_cast<ImTextureID>(0)) {
          const ImVec2 uvMin =
              icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
          const ImVec2 uvMax =
              icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
          modeDrawList->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax,
                                 IM_COL32(255, 255, 255, alpha));
        } else if (fallback && *fallback) {
          const ImVec2 textSize = ImGui::CalcTextSize(fallback);
          modeDrawList->AddText(
              ImVec2(min.x + (modeButtonSize.x - textSize.x) * 0.5f,
                     min.y + (modeButtonSize.y - textSize.y) * 0.5f),
              IM_COL32(255, 255, 255, alpha), fallback);
        }
        if (hovered && tooltip && *tooltip) {
          ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::PopID();
        return pressed;
      };

      auto selectRMeshMode = [&](MeshEditSelectionMode mode,
                                 bool triangleSelection) {
        meshEditSelectionMode = mode;
        meshEditTriangleSelection = triangleSelection;
        if (mode == MeshEditSelectionMode::Object) {
          clearMeshEditSelection();
        }
      };

      ImGui::SetCursorScreenPos(ImVec2(modeToolbarMin.x + modePadding,
                                       modeToolbarMin.y + modePadding));
      if (drawRMeshModeButton(
              "##rmesh_select_object",
              "Resources/Engine-Root/RMesh Builder/Object Selection.png", "O",
              "Object Selection", meshEditSelectionMode ==
                                      MeshEditSelectionMode::Object)) {
        selectRMeshMode(MeshEditSelectionMode::Object, false);
      }
      ImGui::SameLine(0.0f, modeSpacing);
      if (drawRMeshModeButton(
              "##rmesh_select_vertex",
              "Resources/Engine-Root/RMesh Builder/Vertex Selection.png", "V",
              "Vertex Selection", meshEditSelectionMode ==
                                      MeshEditSelectionMode::Vertex)) {
        selectRMeshMode(MeshEditSelectionMode::Vertex, false);
      }
      ImGui::SameLine(0.0f, modeSpacing);
      if (drawRMeshModeButton(
              "##rmesh_select_edge",
              "Resources/Engine-Root/RMesh Builder/Edge Selection.png", "E",
              "Edge Selection", meshEditSelectionMode ==
                                    MeshEditSelectionMode::Edge)) {
        selectRMeshMode(MeshEditSelectionMode::Edge, false);
      }
      ImGui::SameLine(0.0f, modeSpacing);
      if (drawRMeshModeButton(
              "##rmesh_select_face",
              "Resources/Engine-Root/RMesh Builder/Face Selection.png", "F",
              "Face Selection",
              meshEditSelectionMode == MeshEditSelectionMode::Face &&
                  !meshEditTriangleSelection)) {
        selectRMeshMode(MeshEditSelectionMode::Face, false);
      }
      ImGui::SameLine(0.0f, modeSpacing);
      if (drawRMeshModeButton(
              "##rmesh_select_triangle",
              "Resources/Engine-Root/RMesh Builder/Tri Selection.png", "T",
              "Triangle Selection",
              meshEditSelectionMode == MeshEditSelectionMode::Face &&
                  meshEditTriangleSelection)) {
        selectRMeshMode(MeshEditSelectionMode::Face, true);
      }
      ImGui::SameLine(0.0f, modeSpacing);
      if (drawRMeshModeButton(
              "##rmesh_carve",
              "Resources/Engine-Root/RMesh Builder/Carve.png", "C",
              "Carve Mode (Shift+C): cut doorways and openings into walls",
              meshEditSelectionMode == MeshEditSelectionMode::Carve)) {
        selectRMeshMode(MeshEditSelectionMode::Carve, false);
        resetCarveToolState();
      }

      const ImVec2 actionsButtonPos(
          modeToolbarMin.x + (modeToolbarSize.x - actionsButtonSize.x) * 0.5f,
          modeToolbarMin.y + modePadding + modeButtonSize.y + 3.0f);
      ImGui::SetCursorScreenPos(actionsButtonPos);
      ImGui::PushID("##rmesh_actions_dropdown");
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton("##btn", actionsButtonSize);
      const bool actionsHovered = ImGui::IsItemHovered();
      const bool actionsPressed = ImGui::IsItemClicked();
      if (actionsPressed) ImGui::ReportWidgetFeedback(ImGuiWidgetFeedback_Button);
      ImGui::ReportItemHoverFeedback();
      const ImVec2 actionsMin = ImGui::GetItemRectMin();
      const ImVec2 actionsMax = ImGui::GetItemRectMax();
      const ImU32 actionsBg =
          actionsHovered ? hoverBtn : ImGui::GetColorU32(baseCol);
      modeDrawList->AddRectFilled(actionsMin, actionsMax, actionsBg, 6.0f);
      modeDrawList->AddRect(
          actionsMin, actionsMax,
          ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.16f)), 6.0f);
      const ImVec2 arrowCenter((actionsMin.x + actionsMax.x) * 0.5f,
                               (actionsMin.y + actionsMax.y) * 0.5f + 1.0f);
      modeDrawList->AddTriangleFilled(
          ImVec2(arrowCenter.x - 5.0f, arrowCenter.y - 2.0f),
          ImVec2(arrowCenter.x + 5.0f, arrowCenter.y - 2.0f),
          ImVec2(arrowCenter.x, arrowCenter.y + 4.0f), iconColor);
      if (actionsHovered) {
        ImGui::SetTooltip("Mesh edit actions");
      }
      if (actionsPressed) {
        meshEditActionsPopupPos =
            ImVec2(actionsMin.x, actionsMax.y + 5.0f);
        meshEditActionsPopupRequested = true;
      }
      ImGui::PopID();

      const bool modeToolbarHovered = ImGui::IsWindowHovered(
          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
      if (modeToolbarHovered) {
        blockSelection = true;
      }
      ImGui::EndChild();
    }

    if (worldUiEditing) {
      blockSelection = true;
    }
    // RMB input routing: pressing RMB over the viewport immediately enters
    // freelook.
    static bool viewportRightPending = false;
    static bool viewportRightConsumedByLook = false;
    static ImVec2 viewportRightPressPos(0.0f, 0.0f);
    const bool rightPressCandidate =
        mouseOverViewportImage &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGuizmo::IsUsing() &&
        !ImGuizmo::IsOver() && !blockSelection;
    if (rightPressCandidate) {
      viewportRightPending = true;
      viewportRightConsumedByLook = true;
      viewportRightPressPos = ImGui::GetMousePos();
      viewportController.setFocused(true);
      cursorLocked = true;
      camera.velocity = glm::vec3(0.0f);
      camera.firstMouse = true;
      ImGui::ClearActiveID();
    }

    if (cursorLocked && viewportController.isViewportFocused()) {
      const float wheel = ImGui::GetIO().MouseWheel;
      if (std::abs(wheel) > 0.0001f) {
        const float factor = std::pow(1.12f, wheel);
        const float ratio = (camera.moveSpeed > 0.001f)
                                ? (camera.sprintSpeed / camera.moveSpeed)
                                : 2.0f;
        camera.moveSpeed = std::clamp(camera.moveSpeed * factor, 0.5f, 100.0f);
        camera.sprintSpeed =
            std::clamp(camera.moveSpeed * ratio, 0.5f, 200.0f);
        viewportMoveSpeedHudValue = camera.moveSpeed;
        viewportMoveSpeedHudTime = glfwGetTime();
      }
    }

    bool rightPickRelease = false;
    if (viewportRightPending &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      ImVec2 now = ImGui::GetMousePos();
      float dx = now.x - viewportRightPressPos.x;
      float dy = now.y - viewportRightPressPos.y;
      bool isClick = (dx * dx + dy * dy) < (6.0f * 6.0f);
      rightPickRelease = isClick && !viewportRightConsumedByLook &&
                         mouseOverViewportImage && !ImGuizmo::IsUsing() &&
                         !ImGuizmo::IsOver() && !blockSelection &&
                         !(meshEditMode && meshEditSelectionMode !=
                                               MeshEditSelectionMode::Object);
      viewportRightPending = false;
    }

    // Viewport object picking (left click select, RMB click-release for context
    // selection)
    const bool leftPickClick = mouseOverViewportImage &&
                               ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                               !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
                               !blockSelection;
    bool rightPickHitObject = false;
    std::vector<int> rig25DSelectionBeforeContext;
    if (project25DPipeline) {
      rig25DSelectionBeforeContext = selectedObjectIds;
      if (rig25DSelectionBeforeContext.empty() && selectedObjectId >= 0) {
        rig25DSelectionBeforeContext.push_back(selectedObjectId);
      }
    }
    if (leftPickClick || rightPickRelease) {
      const int hitId = pickSceneObjectAtScreen(view, proj, imageMin, imageMax, ImGui::GetMousePos());

      viewportController.setFocused(true);
      if (hitId != -1) {
        if (leftPickClick) {
          bool additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
          setPrimarySelection(hitId, additive);
        } else {
          setPrimarySelection(hitId, false);
          rightPickHitObject = true;
        }
      } else if (leftPickClick) {
        clearSelection();
      }
    }

    selectedObj = getSelectedObject();
    const bool selectedMeshContextObject =
        selectedObj && selectedObj->hasRenderer &&
        (selectedObj->renderType == RenderType::Model ||
         selectedObj->renderType == RenderType::OBJMesh ||
         IsRawMeshPath(selectedObj->meshPath)) &&
        (!meshEditMode ||
         meshEditSelectionMode == MeshEditSelectionMode::Object);
    const bool meshObjectContextMode = selectedMeshContextObject;
    const bool rig25DContextMode = project25DPipeline && selectedObj != nullptr;

    if ((meshObjectContextMode || rig25DContextMode) && rightPickHitObject &&
        !cursorLocked && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver()) {
      ImGui::OpenPopup("##mesh_object_context_menu");
    }

    if (ImGui::BeginPopup("##mesh_object_context_menu")) {
      if (selectedObj) {
        if (ImGui::MenuItem("Delete Object")) {
          setPrimarySelection(selectedObj->id);
          deleteSelected();
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Reset Object")) {
          recordState("meshObjectReset");
          selectedObj->position = glm::vec3(0.0f);
          selectedObj->rotation = glm::vec3(0.0f);
          selectedObj->scale = glm::vec3(1.0f);
          updateHierarchyWorldTransforms();
          projectManager.currentProject.hasUnsavedChanges = true;
          addConsoleMessage("Mesh object reset", ConsoleMessageType::Success);
        }
        if (ImGui::MenuItem("Duplicate Object")) {
          setPrimarySelection(selectedObj->id);
          duplicateSelected();
          ImGui::CloseCurrentPopup();
        }

        if (selectedMeshContextObject) {
          if (ImGui::MenuItem("Convert to RMesh")) {
            if (!selectedObj->meshPath.empty()) {
              fs::path src = selectedObj->meshPath;
              fs::path outPath = src;
              outPath.replace_extension(".rmesh");
              std::string err;
              if (getModelLoader().exportRawMesh(src.string(), outPath.string(),
                                                 err)) {
                ModelLoadResult converted =
                    getModelLoader().loadModel(outPath.string());
                if (converted.success && converted.meshIndex >= 0) {
                  selectedObj->hasRenderer = true;
                  selectedObj->renderType = RenderType::Model;
                  selectedObj->type = ObjectType::Model;
                  selectedObj->meshPath = outPath.string();
                  selectedObj->meshId = converted.meshIndex;
                  meshEditPath.clear();
                  meshEditLoaded = false;
                  meshEditDirty = false;
                  fileBrowser.needsRefresh = true;
                  projectManager.currentProject.hasUnsavedChanges = true;
                  addConsoleMessage("Converted object to RMesh: " +
                                        outPath.string(),
                                    ConsoleMessageType::Success);
                } else {
                  addConsoleMessage("Convert to RMesh failed: " +
                                        converted.errorMessage,
                                    ConsoleMessageType::Error);
                }
              } else {
                addConsoleMessage("Convert to RMesh failed: " + err,
                                  ConsoleMessageType::Error);
              }
            }
          }
          if (ImGui::MenuItem("Rebuild Mesh Data")) {
            if (IsMeshEditablePath(selectedObj->meshPath)) {
              RawMeshAsset rebuilt;
              std::string err;
              if (getModelLoader().loadRawMesh(selectedObj->meshPath, rebuilt,
                                               err)) {
                if (selectedObj->meshId < 0) {
                  ModelLoadResult loaded =
                      getModelLoader().loadModel(selectedObj->meshPath);
                  if (loaded.success) {
                    selectedObj->meshId = loaded.meshIndex;
                  }
                }
                if (selectedObj->meshId >= 0 &&
                    getModelLoader().updateRawMesh(selectedObj->meshId,
                                                   rebuilt, err)) {
                  addConsoleMessage("Rebuilt RMesh data for object",
                                    ConsoleMessageType::Success);
                } else {
                  addConsoleMessage("Rebuild mesh data failed: " + err,
                                    ConsoleMessageType::Error);
                }
              } else {
                addConsoleMessage("Rebuild mesh data failed: " + err,
                                  ConsoleMessageType::Error);
              }
            }
          }
        }

        if (project25DPipeline) {
          ImGui::Separator();
          ImGui::TextDisabled("2.5D Rig");
          ImGui::TextDisabled(
              "Rig nodes are Empty objects; animation uses normal transform tracks.");

          const bool selectedIsEmpty = selectedObj->type == ObjectType::Empty;
          const bool canAttachSelection =
              selectedIsEmpty && !rig25DSelectionBeforeContext.empty();
          if (ImGui::MenuItem("Attach Previous Selection Here (preserve world)",
                              nullptr, false, canAttachSelection)) {
            std::vector<int> sourceRoots =
                collectSelectionRoots(rig25DSelectionBeforeContext);
            const int targetId = selectedObj->id;
            bool changed = false;
            for (int childId : sourceRoots) {
              if (childId == targetId) {
                continue;
              }
              setParent(childId, targetId);
              changed = true;
            }
            if (changed) {
              updateHierarchyWorldTransforms();
              projectManager.currentProject.hasUnsavedChanges = true;
              addConsoleMessage("Attached selected objects to rig node",
                                ConsoleMessageType::Success);
            }
            ImGui::CloseCurrentPopup();
          }

          if (ImGui::MenuItem("Create Child Rig Node (Empty)", nullptr, false,
                              selectedIsEmpty)) {
            const int parentId = selectedObj->id;
            const glm::vec3 parentPos = selectedObj->position;
            const glm::vec3 parentRot = selectedObj->rotation;
            const glm::vec3 parentScale = selectedObj->scale;
            addObject(ObjectType::Empty, "Rig Node");
            if (SceneObject *created = getSelectedObject()) {
              created->position = parentPos;
              created->rotation = parentRot;
              created->scale = parentScale;
              created->localPosition = created->position;
              created->localRotation = NormalizeEulerDegrees(created->rotation);
              created->localScale = created->scale;
              created->localInitialized = true;
              setRig25DMetadata(*created, false);
              setParent(created->id, parentId);
              updateHierarchyWorldTransforms();
              projectManager.currentProject.hasUnsavedChanges = true;
            }
            ImGui::CloseCurrentPopup();
          }

          if (ImGui::MenuItem("Detach From Parent (preserve world)", nullptr,
                              false, selectedObj->parentId != -1)) {
            setParent(selectedObj->id, -1);
            updateHierarchyWorldTransforms();
            projectManager.currentProject.hasUnsavedChanges = true;
            addConsoleMessage("Detached object from hierarchy",
                              ConsoleMessageType::Success);
            ImGui::CloseCurrentPopup();
          }

          if (ImGui::MenuItem("Zero Local Transform")) {
            zeroLocalTransform(*selectedObj);
            updateHierarchyWorldTransforms();
            projectManager.currentProject.hasUnsavedChanges = true;
            addConsoleMessage("Zeroed local transform",
                              ConsoleMessageType::Success);
            ImGui::CloseCurrentPopup();
          }

          if (ImGui::MenuItem("Mirror Local X")) {
            mirrorLocalX(*selectedObj);
            updateHierarchyWorldTransforms();
            projectManager.currentProject.hasUnsavedChanges = true;
            addConsoleMessage("Mirrored local X transform",
                              ConsoleMessageType::Success);
            ImGui::CloseCurrentPopup();
          }

          ImGui::TextDisabled(
              "Future note: rig-root clips should target node IDs.");
        }
      }
      ImGui::EndPopup();
    }

    if (cursorLocked && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      cursorLocked = false;
      camera.velocity = glm::vec3(0.0f);
      camera.firstMouse = true;
    }
    if (cursorLocked) {
      viewportController.setFocused(true);
    }

    if (isPlaying && showViewOutput && rendererInitialized) {
      static unsigned int cachedViewOutputTex = 0;
      static int cachedViewOutputWidth = 0;
      static int cachedViewOutputHeight = 0;
      static int cachedViewOutputCameraId = -1;
      static double nextViewOutputRefreshTime = 0.0;
      constexpr double kViewOutputRefreshInterval = 1.0 / 30.0;

      std::vector<const SceneObject *> playerCams;
      for (const auto &obj : sceneObjects) {
        if (IsObjectEnabledInHierarchy(obj) && obj.hasCamera &&
            obj.camera.type == SceneCameraType::Player) {
          playerCams.push_back(&obj);
        }
      }

      if (playerCams.empty()) {
        previewCameraId = -1;
        cachedViewOutputTex = 0;
        cachedViewOutputCameraId = -1;
        nextViewOutputRefreshTime = 0.0;
      } else {
        auto findCamById = [&](int id) -> const SceneObject * {
          auto it =
              std::find_if(playerCams.begin(), playerCams.end(),
                           [id](const SceneObject *o) { return o->id == id; });
          return (it != playerCams.end()) ? *it : nullptr;
        };
        const SceneObject *previewCam = findCamById(previewCameraId);
        if (!previewCam) {
          previewCam = playerCams.front();
          previewCameraId = previewCam->id;
        }

        int previewWidth = static_cast<int>(imageSize.x * 0.28f);
        previewWidth = std::clamp(previewWidth, 180, 420);
        int previewHeight = static_cast<int>(previewWidth / 16.0f * 9.0f);
        const double now = glfwGetTime();
        const bool sizeChanged = previewWidth != cachedViewOutputWidth ||
                                 previewHeight != cachedViewOutputHeight;
        const bool cameraChanged = previewCam->id != cachedViewOutputCameraId;
        const bool forceRefresh = cachedViewOutputTex == 0 || sizeChanged ||
                                  cameraChanged ||
                                  now >= nextViewOutputRefreshTime;

        if (forceRefresh) {
          cachedViewOutputTex = renderer.renderScenePreview(
              makeCameraFromObject(*previewCam), sceneObjects, previewWidth,
              previewHeight, previewCam->camera.fov,
              previewCam->camera.nearClip, previewCam->camera.farClip,
              previewCam->camera.use2D || previewCam->camera.applyPostFX);
          cachedViewOutputWidth = previewWidth;
          cachedViewOutputHeight = previewHeight;
          cachedViewOutputCameraId = previewCam->id;
          nextViewOutputRefreshTime = now + kViewOutputRefreshInterval;
        }
        unsigned int previewTex = cachedViewOutputTex;

        if (previewTex != 0) {
          ImVec2 overlaySize(previewWidth + 20.0f, previewHeight + 64.0f);
          ImVec2 overlayPos = ImVec2(imageMax.x - overlaySize.x - 12.0f,
                                     imageMax.y - overlaySize.y - 12.0f);
          ImVec2 winPos = ImGui::GetWindowPos();
          ImVec2 localPos =
              ImVec2(overlayPos.x - winPos.x, overlayPos.y - winPos.y);
          ImGui::SetCursorPos(localPos);
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
          ImGui::BeginChild("ViewOutputOverlay", overlaySize, true,
                            ImGuiWindowFlags_NoScrollbar);
          ImGui::TextDisabled("View Output");
          ImGuiID comboId = ImGui::GetID("##ViewOutputCamera");
          UIAnimationState &comboAnim = editorUiAnimationStates[comboId];
          float comboAnimSpeed = 0.0f;
          if (uiAnimationMode == UIAnimationMode::Fluid) {
            comboAnimSpeed = 8.0f;
          } else if (uiAnimationMode == UIAnimationMode::Snappy) {
            comboAnimSpeed = 18.0f;
          }
          float comboAnimStep =
              (uiAnimationMode == UIAnimationMode::Off)
                  ? 1.0f
                  : (1.0f -
                     std::exp(-comboAnimSpeed * ImGui::GetIO().DeltaTime));
          bool comboOpen = ImGui::IsPopupOpen(comboId, ImGuiPopupFlags_None);
          if (uiAnimationMode == UIAnimationMode::Off) {
            comboAnim.active = comboOpen ? 1.0f : 0.0f;
          } else {
            float target = comboOpen ? 1.0f : 0.0f;
            comboAnim.active += (target - comboAnim.active) * comboAnimStep;
          }
          ImGui::SetNextWindowBgAlpha(0.85f *
                                      std::clamp(comboAnim.active, 0.0f, 1.0f));
          if (ImGui::BeginCombo("##ViewOutputCamera",
                                previewCam->name.c_str())) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                std::clamp(comboAnim.active, 0.0f, 1.0f));
            for (const auto *cam : playerCams) {
              bool selected = cam->id == previewCameraId;
              if (ImGui::Selectable(cam->name.c_str(), selected)) {
                previewCameraId = cam->id;
              }
              if (selected)
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopStyleVar();
            ImGui::EndCombo();
          }
          ImGui::Image((void *)(intptr_t)previewTex,
                       ImVec2((float)previewWidth, (float)previewHeight),
                       ImVec2(0, 1), ImVec2(1, 0));
          ImGui::EndChild();
          ImGui::PopStyleVar();
        }
      }
    } else {
      previewCameraId = -1;
    }

    if (sceneInteractionOverlayActive) {
      ImGui::EndChild();
      ImGui::PopStyleVar();
    }
  }

  // Draw viewport hint/status on the foreground layer so it always stays above
  // scene sprites/gizmos.
  if (hasViewportImageRect) {
    ImDrawList *fg = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
    fg->PushClipRect(viewportImageMin, viewportImageMax, true);
    const ImU32 hintShadow = IM_COL32(0, 0, 0, 180);
    if (showViewportHintOverlay) {
      /*const char *hintText =
          worldUiEditing ? "MMB/Space+LMB: Pan | Wheel: Zoom | LMB: Select | "
                           "Gizmo: Move/Rotate/Scale"
                         : "Hold RMB: Look & Move | LMB: Select | WASD+QE: Move "
                           "| ESC: Release | F11: Fullscreen";
      const ImU32 hintColor = worldUiEditing ? IM_COL32(228, 236, 246, 196)
                                             : IM_COL32(228, 236, 246, 172);
      ImVec2 hintPos(viewportImageMin.x + 10.0f, viewportImageMin.y + 10.0f);
      fg->AddText(ImVec2(hintPos.x + 1.0f, hintPos.y + 1.0f), hintShadow,
                  hintText);
      fg->AddText(hintPos, hintColor, hintText);*/

      if (cursorLocked) {
        ImVec2 statusPos(viewportImageMin.x + 10.0f, viewportImageMin.y + 10.0f);
        fg->AddText(ImVec2(statusPos.x + 1.0f, statusPos.y + 1.0f), hintShadow,
                    "Freelook Active");
        fg->AddText(statusPos, IM_COL32(120, 255, 120, 255), "Freelook Active");
      } else if (viewportController.isViewportFocused()) {
        ImVec2 statusPos(viewportImageMin.x + 10.0f, viewportImageMin.y + 10.0f);
        fg->AddText(ImVec2(statusPos.x + 1.0f, statusPos.y + 1.0f), hintShadow,
                    "Viewport Focused");
        fg->AddText(statusPos, IM_COL32(180, 226, 255, 255), "Viewport Focused");
      }
    }

    const double speedHudAge = glfwGetTime() - viewportMoveSpeedHudTime;
    if (speedHudAge >= 0.0 && speedHudAge < 1.35) {
      const float fadeStart = 0.85f;
      const float alpha = speedHudAge <= fadeStart
                              ? 1.0f
                              : std::clamp(
                                    1.0f - static_cast<float>(
                                               (speedHudAge - fadeStart) /
                                               (1.35 - fadeStart)),
                                    0.0f, 1.0f);
      char speedLabel[64];
      std::snprintf(speedLabel, sizeof(speedLabel), "Editor Speed %.2fx",
                    viewportMoveSpeedHudValue);
      const ImVec2 textSize = ImGui::CalcTextSize(speedLabel);
      const ImVec2 pad(14.0f, 7.0f);
      const float centerX = (viewportImageMin.x + viewportImageMax.x) * 0.5f;
      const float bottomY = viewportImageMax.y - 42.0f;
      const ImVec2 boxMin(centerX - textSize.x * 0.5f - pad.x,
                          bottomY - textSize.y - pad.y * 2.0f);
      const ImVec2 boxMax(centerX + textSize.x * 0.5f + pad.x, bottomY);
      const int bgA = static_cast<int>(190.0f * alpha);
      const int borderA = static_cast<int>(175.0f * alpha);
      const int textA = static_cast<int>(245.0f * alpha);
      fg->AddRectFilled(boxMin, boxMax, IM_COL32(18, 22, 30, bgA), 8.0f);
      fg->AddRect(boxMin, boxMax, IM_COL32(120, 190, 255, borderA), 8.0f, 0,
                  1.3f);
      fg->AddText(ImVec2(centerX - textSize.x * 0.5f,
                         boxMin.y + pad.y),
                  IM_COL32(226, 242, 255, textA), speedLabel);
    }

    fg->PopClipRect();
  }

  bool windowFocused =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  viewportController.updateFocusFromImGui(windowFocused, cursorLocked);

  ImGui::End();
}
#pragma endregion
