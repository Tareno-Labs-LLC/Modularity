#pragma once
#include <cmath>
#include <string>
#if defined(_MSVC_LANG)
#define MODULARITY_CPLUSPLUS _MSVC_LANG
#else
#define MODULARITY_CPLUSPLUS __cplusplus
#endif
#if defined(_MSC_VER) && MODULARITY_CPLUSPLUS < 201703L && !defined(_SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING)
#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
#endif
#if MODULARITY_CPLUSPLUS >= 201703L
#include <filesystem>
#elif defined(__has_include)
#if __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
#else
#error "Modularity script SDK requires <filesystem> or <experimental/filesystem>."
#endif
#else
#include <experimental/filesystem>
#endif
#include "ThirdParty/glm/glm.hpp"
#include "ThirdParty/glm/gtc/quaternion.hpp"
#if MODULARITY_CPLUSPLUS < 201703L
namespace std {
    template <typename T>
    constexpr const T& clamp(const T& value, const T& low, const T& high) {return value < low ? low : (high < value ? high : value);}
}
#endif
#if MODULARITY_CPLUSPLUS >= 201703L
namespace fs = std::filesystem;
#else
namespace fs = std::experimental::filesystem;
#endif
#ifndef MODULARITY_COMMON_SHARED_DECLS
#define MODULARITY_COMMON_SHARED_DECLS
constexpr float SENSITIVITY = 0.1f;
constexpr float CAMERA_SPEED = 2.5f;
constexpr float FOV = 45.0f;
constexpr float NEAR_PLANE = 0.1f;
constexpr float FAR_PLANE = 100.0f;
constexpr float PI = 3.14159265359f;
inline glm::vec3 NormalizeEulerDegrees(const glm::vec3& deg) {
    auto wrap = [](float a) {float r = std::fmod(a, 360.0f); if (r < 0.0f) r += 360.0f; return r;};
    return glm::vec3(wrap(deg.x), wrap(deg.y), wrap(deg.z));
}
class Mesh;
class OBJLoader;
class Renderer;
class Camera;
class ViewportController;
class SceneObject;
class Project;
class ProjectManager;
class Engine;
extern OBJLoader g_objLoader;

struct Sprite {
    std::string sheetAssetPath;
    std::string clipName;
    mutable int clipIndex = -1;

    Sprite() = default;
    Sprite(int clip) : clipIndex(clip) {}
    Sprite& operator=(int clip) { sheetAssetPath.clear(); clipName.clear(); clipIndex = clip; return *this; }
    operator int() const { return clipIndex; }

    bool IsValid() const { return !clipName.empty() || clipIndex >= 0; }
    bool IsAssigned() const { return IsValid(); }
    explicit operator bool() const { return IsValid(); }
};
#endif
