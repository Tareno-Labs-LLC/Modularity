#pragma once

#include "ScriptSdkCommon.h"
#include "Lighting2DTypes.h"
#include "AudioFX.h"
#include <algorithm>
#include <unordered_set>

enum class ObjectType {
    Cube = 0,
    Sphere = 1,
    Capsule = 2,
    OBJMesh = 3,
    Model = 4,        // New type for Assimp-loaded models (FBX, GLTF, etc.)
    DirectionalLight = 5,
    PointLight = 6,
    SpotLight = 7,
    AreaLight = 8,
    Camera = 9,
    PostFXNode = 10,
    Mirror = 11,
    Plane = 12,
    Torus = 13,
    Sprite = 14,      // 3D quad sprite (lit/unlit with material)
    Sprite2D = 15,    // Screen-space sprite
    Canvas = 16,      // UI canvas root
    UIImage = 17,
    UISlider = 18,
    UIButton = 19,
    UIText = 20,
    Empty = 21,
    Sprite25D = 22,
    Light2D = 23,
    ShadowCaster2D = 24,
    ParticleSystem2D = 25,
    ReflectionCast = 26
};

enum class RenderType {
    None = 0,
    Cube = 1,
    Sphere = 2,
    Capsule = 3,
    OBJMesh = 4,
    Model = 5,
    Mirror = 6,
    Plane = 7,
    Torus = 8,
    Sprite = 9
};

enum class UIElementType {
    None = 0,
    Canvas = 1,
    Image = 2,
    Slider = 3,
    Button = 4,
    Text = 5,
    Sprite2D = 6
};

struct MaterialProperties {
    enum class TextureFilter {
        Bilinear = 0,
        Point = 1
    };

    glm::vec3 color = glm::vec3(1.0f);
    float alpha = 1.0f;
    float ambientStrength = 0.2f;
    float specularStrength = 0.5f;
    float shininess = 32.0f;
    float normalMapIntensity = 1.0f;
    float textureMix = 0.3f;  // Blend factor between albedo and overlay
    glm::vec2 uvTiling = glm::vec2(1.0f);
    glm::vec2 uvOffset = glm::vec2(0.0f);
    TextureFilter textureFilter = TextureFilter::Bilinear;
    float scrollSpeed = 0.5f;
    glm::vec2 scrollDirection = glm::vec2(1.0f, 0.3f);
    bool uvScrollEnabled = false;

    glm::vec3 cloudColor = glm::vec3(0.85f, 0.36f, 0.96f);
    glm::vec3 cloudSkyColor = glm::vec3(0.10f, 0.02f, 0.14f);
    float cloudScale = 3.0f;
    float cloudCoverage = 0.5f;
    float cloudSoftness = 0.35f;
    int cloudDetail = 5;
    float cloudSpeed = 0.15f;
    float cloudWarp = 0.35f;
    float cloudHighlight = 1.35f;
    float cloudStars = 0.0f;
    float cloudHorizon = 0.0f;

    bool pbrEnabled = false;
    float metallic = 0.0f;
    float roughness = 0.5f;
    float occlusionStrength = 1.0f;

    glm::vec3 emissionColor = glm::vec3(1.0f);
    float emissionStrength = 0.0f;

    float alphaCutoff = 0.0f;

    float heightScale = 0.0f;
    int parallaxSteps = 32;
    bool parallaxInvertHeight = false;
};

enum class LightType {
    Directional = 0,
    Point = 1,
    Spot = 2,
    Area = 3
};

enum class ReflectionCastUpdateMode {
    EveryFrame = 0,
    FirstFrame = 1
};

enum class UIAnchor {
    Center = 0,
    TopLeft = 1,
    TopRight = 2,
    BottomLeft = 3,
    BottomRight = 4
};

enum class UITextHAlign {
    Left = 0,
    Center = 1,
    Right = 2
};

enum class UITextVAlign {
    Top = 0,
    Middle = 1,
    Bottom = 2
};

enum class UISliderStyle {
    ImGui = 0,
    Fill = 1,
    Circle = 2,
    Vertical = 3,
    Ring = 4,
    Stepped = 5
};

enum class UIButtonStyle {
    ImGui = 0,
    Outline = 1
};

enum class ReverbPreset {
    Room = 0,
    LivingRoom = 1,
    Hall = 2,
    Forest = 3,
    Custom = 4
};

enum class ReverbZoneShape {
    Box = 0,
    Sphere = 1
};

enum class AudioRolloffMode {
    Logarithmic = 0,
    Linear = 1,
    Exponential = 2,
    Custom = 3
};

enum class AnimationInterpolation {
    Linear = 0,
    SmoothStep = 1,
    EaseIn = 2,
    EaseOut = 3,
    EaseInOut = 4,
    Step = 5,
    Cubic = 6
};

enum class AnimationCurveMode {
    Preset = 0,
    Bezier = 1
};

struct AnimationKeyframe {
    float time = 0.0f;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
    AnimationInterpolation interpolation = AnimationInterpolation::SmoothStep;
    AnimationCurveMode curveMode = AnimationCurveMode::Preset;
    glm::vec2 bezierIn = glm::vec2(0.25f, 0.0f);
    glm::vec2 bezierOut = glm::vec2(0.75f, 1.0f);
};

struct AnimationEvent {
    float time = 0.0f;
    std::string eventId;
    std::string payload;
};

struct AnimationPropertyKeyframe {
    float time = 0.0f;
    float value = 0.0f;
    AnimationInterpolation interpolation = AnimationInterpolation::SmoothStep;
    AnimationCurveMode curveMode = AnimationCurveMode::Preset;
    glm::vec2 bezierIn = glm::vec2(0.25f, 0.0f);
    glm::vec2 bezierOut = glm::vec2(0.75f, 1.0f);
};

struct AnimationPropertyTrack {
    bool enabled = true;
    std::string path;
    std::string label;
    float defaultValue = 0.0f;
    std::vector<AnimationPropertyKeyframe> keyframes;
};

struct AnimationClipSlot {
    std::string name;
    std::string assetPath;
};

struct AnimationComponent {
    bool enabled = true;
    std::string clipAssetPath;
    std::vector<AnimationClipSlot> clips;
    int activeClipIndex = -1;
    float clipLength = 2.0f;
    float playSpeed = 1.0f;
    bool loop = true;
    bool playOnAwake = true;
    bool applyOnScrub = true;
    bool runtimePlaying = false;
    bool runtimePaused = false;
    float runtimeTime = 0.0f;
    float runtimeDirection = 1.0f;
    bool runtimeInitialized = false;
    std::string runtimeClipPath;
    std::vector<AnimationKeyframe> keyframes;
    std::vector<AnimationEvent> events;
    std::vector<AnimationPropertyTrack> tracks;
};

inline std::string AnimationClipNameFromPath(const std::string& assetPath) {
    if (assetPath.empty()) return "Animation";
    fs::path path(assetPath);
    std::string stem = path.stem().string();
    if (!stem.empty()) return stem;
    std::string fileName = path.filename().string();
    if (!fileName.empty()) return fileName;
    return "Animation";
}

inline int AnimationGetActiveClipIndex(const AnimationComponent& animation) {
    if (animation.clips.empty()) return -1;
    if (animation.activeClipIndex >= 0 &&
        animation.activeClipIndex < static_cast<int>(animation.clips.size())) {
        return animation.activeClipIndex;
    }
    if (!animation.clipAssetPath.empty()) {
        for (int i = 0; i < static_cast<int>(animation.clips.size()); ++i) {
            if (animation.clips[i].assetPath == animation.clipAssetPath) {
                return i;
            }
        }
    }
    return 0;
}

inline const AnimationClipSlot* AnimationGetActiveClip(const AnimationComponent& animation) {
    const int index = AnimationGetActiveClipIndex(animation);
    if (index < 0 || index >= static_cast<int>(animation.clips.size())) return nullptr;
    return &animation.clips[index];
}

inline AnimationClipSlot* AnimationGetActiveClip(AnimationComponent& animation) {
    const int index = AnimationGetActiveClipIndex(animation);
    if (index < 0 || index >= static_cast<int>(animation.clips.size())) return nullptr;
    return &animation.clips[index];
}

inline std::string AnimationGetActiveClipAssetPath(const AnimationComponent& animation) {
    const AnimationClipSlot* clip = AnimationGetActiveClip(animation);
    if (clip) return clip->assetPath;
    return animation.clipAssetPath;
}

inline std::string AnimationGetActiveClipName(const AnimationComponent& animation) {
    const AnimationClipSlot* clip = AnimationGetActiveClip(animation);
    if (clip == nullptr) {
        if (!animation.clipAssetPath.empty()) {
            return AnimationClipNameFromPath(animation.clipAssetPath);
        }
        return {};
    }
    if (!clip->name.empty()) return clip->name;
    return AnimationClipNameFromPath(clip->assetPath);
}

inline void NormalizeAnimationClipSlots(AnimationComponent& animation) {
    if (!animation.clipAssetPath.empty()) {
        bool foundLegacyPath = false;
        for (AnimationClipSlot& clip : animation.clips) {
            if (clip.assetPath == animation.clipAssetPath) {
                foundLegacyPath = true;
            }
            if (clip.name.empty()) {
                clip.name = AnimationClipNameFromPath(clip.assetPath);
            }
        }
        if (!foundLegacyPath) {
            AnimationClipSlot clip;
            clip.assetPath = animation.clipAssetPath;
            clip.name = AnimationClipNameFromPath(clip.assetPath);
            animation.clips.push_back(std::move(clip));
        }
    } else {
        for (AnimationClipSlot& clip : animation.clips) {
            if (clip.name.empty()) {
                clip.name = AnimationClipNameFromPath(clip.assetPath);
            }
        }
    }

    if (animation.clips.empty()) {
        animation.activeClipIndex = -1;
        animation.clipAssetPath.clear();
        return;
    }

    int resolvedIndex = AnimationGetActiveClipIndex(animation);
    if (resolvedIndex < 0 || resolvedIndex >= static_cast<int>(animation.clips.size())) {
        resolvedIndex = 0;
    }
    animation.activeClipIndex = resolvedIndex;
    animation.clipAssetPath = animation.clips[resolvedIndex].assetPath;
}

struct SkeletalAnimationComponent {
    bool enabled = true;
    bool useGpuSkinning = true;
    bool allowCpuFallback = true;
    bool useAnimation = false;
    int clipIndex = 0;
    float time = 0.0f;
    float playSpeed = 1.0f;
    bool loop = true;
    int skeletonRootId = -1;
    int maxBones = 128;
    std::vector<std::string> boneNames;
    std::vector<int> boneNodeIds;
    std::vector<int> armatureNodeIds;
    std::vector<glm::mat4> inverseBindMatrices;
    std::vector<glm::mat4> finalMatrices;
};

struct LightComponent {
    LightType type = LightType::Point;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float range = 10.0f;
    float edgeFade = 0.2f; // 0 = sharp cutoff, 1 = fully softened edges (area lights)
    // Spot
    float innerAngle = 15.0f;
    float outerAngle = 25.0f;
    // Area (rect) size in world units
    glm::vec2 size = glm::vec2(1.0f, 1.0f);
    bool castShadows = false;
    bool softShadows = true;
    float shadowBias = 0.02f;
    float shadowSoftness = 0.04f;
    int shadowResolution = 0; // 0 = use renderer default
    bool enabled = true;
};

struct ReflectionCastComponent {
    bool enabled = true;
    ReflectionCastUpdateMode updateMode = ReflectionCastUpdateMode::FirstFrame;
    glm::vec3 boxSize = glm::vec3(6.0f);
    float blendDistance = 8.0f;
    float intensity = 1.0f;
    int resolution = 128;
    bool baked = false;
};

enum class SceneCameraType {
    Scene = 0,
    Player = 1
};

enum class SceneCameraProjection {
    Perspective = 0,
    Orthographic = 1
};

enum class SceneCameraFovAxis {
    Vertical = 0,
    Horizontal = 1
};

enum class SceneCameraBackground {
    Skybox = 0,
    SolidColor = 1
};

enum class PostFXToneMapper {
    None = 0,
    Reinhard = 1,
    ACES = 2
};

enum class PostFXDitherPalette {
    FullColor = 0,
    PS1Warm = 1,
    PS1Cool = 2,
    Mono = 3,
    Sepia = 4
};

enum class PostFXDitherPattern {
    Classic4x4 = 0,
    Bayer8x8 = 1,
    Bayer16x16 = 2,
    Checker = 3,
    HybridPS1 = 4
};

enum class PostFXVhsSignalMode {
    Composite = 0,
    SVideo = 1,
    RF = 2,
    VhsSP = 3,
    VhsLP = 4,
    VhsEP = 5
};

enum class PostFX2DScopeMode {
    AtOrBelow = 0,
    Band = 1
};

struct CameraComponent {
    SceneCameraType type = SceneCameraType::Player;
    float fov = FOV;
    float nearClip = NEAR_PLANE;
    float farClip = FAR_PLANE;
    bool applyPostFX = true;
    bool use2D = false;
    float pixelsPerUnit = 100.0f;
    SceneCameraProjection projection = SceneCameraProjection::Perspective;
    SceneCameraFovAxis fovAxis = SceneCameraFovAxis::Vertical;
    float orthoSize = 5.0f;
    bool renderShadows = true;
    SceneCameraBackground background = SceneCameraBackground::Skybox;
    glm::vec3 backgroundColor = glm::vec3(0.10f, 0.10f, 0.12f);
    uint32_t cullingMask = 0xFFFFFFFFu;
};

inline float ResolveCameraVerticalFovDeg(const CameraComponent& cam, float aspectWidthOverHeight) {
    float fovDeg = glm::clamp(cam.fov, 1.0f, 179.0f);
    if (cam.fovAxis != SceneCameraFovAxis::Horizontal) return fovDeg;
    const float aspect = glm::max(aspectWidthOverHeight, 0.0001f);
    const float halfH = glm::radians(fovDeg) * 0.5f;
    return glm::clamp(glm::degrees(2.0f * std::atan(std::tan(halfH) / aspect)), 1.0f, 179.0f);
}

struct PostFXSettings {
    bool enabled = true;
    bool isGlobal = true;
    float priority = 0.0f;
    float blendWeight = 1.0f;
    float blendRadius = 4.0f;
    bool hdrEnabled = true;
    PostFXToneMapper toneMapper = PostFXToneMapper::ACES;
    float whitePoint = 4.0f;
    float gamma = 2.2f;
    bool bloomEnabled = true;
    float bloomThreshold = 1.1f;
    float bloomSoftKnee = 0.25f;
    float bloomIntensity = 0.8f;
    float bloomRadius = 1.5f;
    bool colorAdjustEnabled = false;
    float exposure = 0.0f;      // in EV stops
    float contrast = 1.0f;
    bool eyeAdaptationEnabled = false;
    float eyeAdaptationMinEv = -3.0f;
    float eyeAdaptationMaxEv = 3.0f;
    float eyeAdaptationSpeedUp = 3.0f;
    float eyeAdaptationSpeedDown = 0.6f;
    float eyeAdaptationKeyValue = 0.18f;
    bool purkinjeEnabled = false;
    float purkinjeStrength = 0.7f;
    float purkinjeThreshold = 0.06f;
    float purkinjeRange = 0.25f;
    glm::vec3 purkinjeTint = glm::vec3(0.60f, 0.76f, 1.0f);
    float saturation = 1.0f;
    glm::vec3 colorFilter = glm::vec3(1.0f);
    bool motionBlurEnabled = false;
    float motionBlurStrength = 0.15f; // 0..1 blend with previous frame
    float motionBlurThreshold = 0.04f;
    float motionBlurClamp = 0.35f;
    bool vignetteEnabled = false;
    float vignetteIntensity = 0.35f;
    float vignetteSmoothness = 0.25f;
    bool lensFlareEnabled = false;
    float lensFlareIntensity = 0.6f;
    float lensFlareThreshold = 0.75f;
    int lensFlareGhosts = 4;
    float lensFlareGhostSpacing = 0.3f;
    float lensFlareHaloRadius = 0.4f;
    float lensFlareStreak = 0.5f;
    glm::vec3 lensFlareTint = glm::vec3(1.0f, 0.92f, 0.78f);
    bool lensFlareGhostsEnabled = true;
    bool lensFlareHaloEnabled = true;
    bool lensFlareStreakEnabled = true;
    bool lensFlareGlowEnabled = true;
    float lensFlareGlowSize = 0.18f;
    float lensFlareGlowIntensity = 0.7f;
    bool chromaticAberrationEnabled = false;
    float chromaticAmount = 0.0025f;
    bool sharpenEnabled = false;
    float sharpenStrength = 0.15f;
    bool ambientOcclusionEnabled = false;
    float aoRadius = 0.0035f;
    float aoStrength = 0.6f;
    bool ditherEnabled = false;
    float ditherIntensity = 0.65f;
    int ditherColorBits = 5;
    float ditherDarkAdjustment = 0.35f;
    float ditherPixelation = 0.0f;
    float ditherSize = 1.0f;
    float ditherContrast = 0.35f;
    float ditherOffset = 0.0f;
    PostFXDitherPalette ditherPalette = PostFXDitherPalette::FullColor;
    PostFXDitherPattern ditherPattern = PostFXDitherPattern::HybridPS1;
    bool staticEnabled = false;
    float staticIntensity = 0.15f;
    float staticGrainScale = 2.0f;
    float staticDarkAreaInfluence = 0.5f;
    float staticSpeed = 1.0f;
    bool staticMonochrome = false;
    float staticSparkle = 0.1f;
    bool staticDistortionEnabled = false;
    float staticDistortionHorizontalJitterAmount = 0.003f;
    float staticDistortionLineDensity = 128.0f;
    float staticDistortionGlitchFrequency = 1.5f;
    float staticDistortionStrength = 0.2f;
    bool lensDistortionEnabled = false;
    float lensDistortionAmount = 0.08f;
    float lensDistortionEdgeFalloff = 0.75f;
    glm::vec2 lensDistortionCenterOffset = glm::vec2(0.0f);
    bool lensDistortionEdgeVignetteEnabled = false;
    float lensDistortionEdgeVignetteIntensity = 1.0f;
    float lensDistortionEdgeVignetteRadius = 0.9f;
    float lensDistortionEdgeVignetteSoftness = 0.25f;
    glm::vec3 lensDistortionEdgeVignetteColor = glm::vec3(0.0f);
    bool pixelationEnabled = false;
    float pixelationSize = 4.0f;
    bool posterizeEnabled = false;
    int posterizeLevels = 8;
    bool scanlinesEnabled = false;
    float scanlinesIntensity = 0.25f;
    float scanlinesDensity = 1.0f;
    float scanlinesSpeed = 0.0f;
    bool vhsOverlayEnabled = false;
    float vhsOverlayOpacity = 0.6f;
    float vhsOverlayScanlineStrength = 0.55f;
    float vhsOverlayTapeNoise = 0.45f;
    float vhsOverlayChromaBleed = 0.15f;
    float vhsOverlayBottomNoiseBandHeight = 0.18f;
    float vhsOverlayBottomNoiseBandIntensity = 0.85f;
    float vhsOverlayDistortionStrength = 0.35f;
    float vhsOverlayAnimationSpeed = 1.0f;
    float vhsOverlayColorBleed = 0.4f;
    float vhsOverlayBanding = 0.25f;
    PostFXVhsSignalMode vhsOverlaySignalMode = PostFXVhsSignalMode::VhsSP;
    float vhsOverlayDropouts = 0.35f;
    bool wavyEnabled = false;
    float wavyAmplitude = 0.006f;
    float wavyFrequency = 16.0f;
    float wavySpeed = 1.0f;
    bool wavyVertical = false;
    float distortionPolarization = 1.0f;
    bool scope2DEnabled = false;
    PostFX2DScopeMode scope2DMode = PostFX2DScopeMode::AtOrBelow;
    int scope2DMinOrder = -4096;
    int scope2DMaxOrder = 0;
};

inline bool PostFX2DScopeContains(const PostFXSettings& fx, int sortOrder) {
    if (fx.scope2DMode == PostFX2DScopeMode::Band && sortOrder < fx.scope2DMinOrder) {
        return false;
    }
    return sortOrder <= fx.scope2DMaxOrder;
}

enum class ConsoleMessageType {
    Info,
    Warning,
    Error,
    Success
};

struct ScriptSetting {
    std::string key;
    std::string value;
};

enum class ScriptLanguage {
    Cpp = 0,
    CSharp = 1,
    C = 2
};

struct ScriptComponent {
    bool enabled = true;
    ScriptLanguage language = ScriptLanguage::Cpp;
    std::string path;
    std::string managedType;
    int inspectorId = 0;
    std::vector<ScriptSetting> settings;
    std::string lastBinaryPath;
    bool lastBinaryVerified = false;
    std::vector<void*> activeIEnums; // function pointers registered via IEnum_Start
};

struct RigidbodyComponent {
    bool enabled = true;
    float mass = 1.0f;
    bool useCustomCenterOfMass = false;
    glm::vec3 centerOfMass = glm::vec3(0.0f);
    bool useGravity = true;
    bool isKinematic = false;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    int interpolationMode = 1;
    float maxExtrapolationTime = 0.05f;
    bool lockRotationX = false;
    bool lockRotationY = false;
    bool lockRotationZ = false;
};

enum class ColliderType {
    Box = 0,
    Mesh = 1,
    ConvexMesh = 2,
    Capsule = 3,
    Sphere = 4
};

struct ColliderComponent {
    bool enabled = true;
    ColliderType type = ColliderType::Box;
    glm::vec3 boxSize = glm::vec3(1.0f);
    glm::vec3 offset = glm::vec3(0.0f);
    bool convex = true; // For mesh colliders: true = convex hull, false = triangle mesh (static only)
    bool isTrigger = false;
    float staticFriction = 0.9f;
    float dynamicFriction = 0.8f;
    float restitution = 0.0f;
};

struct PlayerControllerComponent {
    bool enabled = true;
    float moveSpeed = 6.0f;
    float runSpeed = 9.0f;
    float lookSensitivity = 0.12f;
    float groundAcceleration = 24.0f;
    float airAcceleration = 8.0f;
    float braking = 16.0f;
    float minSurfaceControl = 0.2f;
    float slideGravity = 40.0f;
    float platformCarry = 1.0f;
    float height = 1.8f;
    float radius = 0.4f;
    float jumpStrength = 6.5f;
    float verticalVelocity = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
};

struct PlayerViewMotionSettings {
    bool enabled = false;

    float bobFrequency = 0.95f;
    float bobVertical = 0.035f;
    float bobHorizontal = 0.025f;
    float bobRoll = 0.55f;
    float runMultiplier = 1.6f;
    float gaitBlend = 4.0f;
    float runLean = 1.2f;

    float lookSway = 0.9f;
    float lookSwayStiffness = 26.0f;
    float lookSwayDamping = 9.0f;
    float lookSwayMax = 3.5f;

    float strafeRoll = 1.6f;
    float turnRoll = 0.9f;
    float rollSmoothing = 7.0f;

    float idleAmount = 0.5f;
    float idleFrequency = 0.25f;
    float idleBlend = 2.2f;

    float jumpCrouchDip = 0.05f;

    float landingDip = 0.06f;
    float landingStiffness = 55.0f;
    float landingDamping = 9.0f;

    bool attachedSwayEnabled = true;
    float attachedSway = 1.3f;
    float attachedStiffness = 18.0f;
    float attachedDamping = 7.0f;
    float attachedMax = 4.0f;

    // A direct child that carries a light (a flashlight model with its spot light inside)
    // is animated as though a hand holds it. Each value scales one part of that motion.
    bool heldItemEnabled = true;
    float heldItemBob = 1.0f;
    float heldItemIdle = 1.0f;
    float heldItemJump = 1.0f;
    float heldItemRunPose = 1.0f;
};

struct PlayerMovementAudioSettings {
    bool enabled = false;
    std::vector<std::string> footstepClips;
    std::string jumpClip;
    std::string landClip;
    float volume = 0.8f;
    float runVolumeScale = 1.25f;
    float landVolumeScale = 1.0f;
    float pitchVariance = 0.08f;
};

struct PlayerControlFeelSettings {
    float lookSmoothing = 0.0f;

    bool chargedJump = false;
    float jumpChargeTime = 0.35f;
    float jumpChargeMinScale = 0.62f;
};

struct UIElementComponent {
    UIElementType type = UIElementType::None;
    UIAnchor anchor = UIAnchor::Center;
    glm::vec2 position = glm::vec2(0.0f); // offset in pixels from anchor
    glm::vec2 size = glm::vec2(160.0f, 40.0f);
    bool maskChildren = true; // Canvas-only: clip descendants to this canvas rect.
    float rotation = 0.0f;
    float sliderValue = 0.5f;
    float sliderMin = 0.0f;
    float sliderMax = 1.0f;
    std::string label = "UI Element";
    bool buttonPressed = false;
    glm::vec4 color = glm::vec4(1.0f);
    bool interactable = true;
    UISliderStyle sliderStyle = UISliderStyle::ImGui;
    UIButtonStyle buttonStyle = UIButtonStyle::ImGui;
    std::string stylePreset = "Default";
    float textScale = 1.0f;
    std::string textFont;
    bool textAutoWrap = true;
    UITextHAlign textHAlign = UITextHAlign::Left;
    UITextVAlign textVAlign = UITextVAlign::Top;
    int textEffectFlags = 0;
    float textEffectSpeed = 1.0f;
    float textEffectIntensity = 1.0f;
    bool renderIn3D = false;
    glm::ivec2 renderTargetSize = glm::ivec2(512, 512);
    MaterialProperties::TextureFilter renderTargetFilter = MaterialProperties::TextureFilter::Bilinear;
    bool pseudo3DEnabled = false;
    bool pseudo3DUseOffscreenSurface = true;
    glm::vec2 pseudo3DPanelSize = glm::vec2(0.0f); // <= 0 uses ui.size
    glm::vec2 pseudo3DTopLeftOffset = glm::vec2(0.0f);
    glm::vec2 pseudo3DTopRightOffset = glm::vec2(0.0f);
    glm::vec2 pseudo3DBottomRightOffset = glm::vec2(0.0f);
    glm::vec2 pseudo3DBottomLeftOffset = glm::vec2(0.0f);
    glm::vec2 pseudo3DPivot = glm::vec2(0.5f, 0.5f);
    float pseudo3DPerspectiveIntensity = 0.0f;
    float pseudo3DSkewAmount = 0.0f;
    float pseudo3DCurvatureAmount = 0.0f;
    int pseudo3DAnchorTargetId = -1;
    bool pseudo3DDistanceScalingEnabled = false;
    bool pseudo3DAdjustPerspectiveWithDistance = false;
    float pseudo3DMinDistance = 1.0f;
    float pseudo3DMaxDistance = 20.0f;
    float pseudo3DInteractionDistance = 0.0f; // 0 disables distance gating
    int pseudo3DDepthSort = 0;
    bool pseudo3DAllowInteraction = true;
    bool spriteSheetEnabled = false;
    int spriteSheetColumns = 1;
    int spriteSheetRows = 1;
    int spriteSheetFrame = 0;
    float spriteSheetFps = 12.0f;
    bool spriteSheetLoop = true;
    bool spriteCustomFramesEnabled = false;
    int spriteSourceWidth = 0;
    int spriteSourceHeight = 0;
    std::vector<glm::ivec4> spriteCustomFrames;
    std::vector<std::string> spriteCustomFrameNames;
    std::vector<glm::vec2> spriteCustomFrameScales;
    std::string spriteSheetAssetPath;
    bool nineSliceEnabled = false;
    glm::vec4 nineSliceBorder = glm::vec4(12.0f, 12.0f, 12.0f, 12.0f); // left, right, top, bottom in source pixels
    bool nineSliceTileEdges = true;
    bool nineSliceTileCenter = false;
    bool receiveLighting2D = true;
    bool unlitLighting2D = false;
    float emissiveLighting2D = 0.0f;
    // Per-component color overrides (alpha == 0 means "use derived default")
    glm::vec4 fillColor = glm::vec4(0.0f);        // slider fill / image tint override
    glm::vec4 backgroundColor = glm::vec4(0.0f);  // slider/button background override
    glm::vec4 borderColor = glm::vec4(0.0f);      // slider/button border override
    glm::vec4 textColor = glm::vec4(0.0f);        // label/text color override
    float fontSize = 0.0f;                         // explicit font size in px (0 = inherit textScale)
    int sortingOrder = 0;                          // draw order within layer
    float backdropBlur = 0.0f;
    float backdropRounding = 0.0f;
    bool uiHovered = false;
    bool uiActive = false;
    float textRevealChars = -1.0f;
    float textRevealPopScale = 1.0f;
    float textRevealSoftness = 1.0f;
};

struct Rigidbody2DComponent {
    bool enabled = true;
    bool useGravity = false;
    bool lockRotation = false;
    float gravityScale = 1.0f;
    float linearDamping = 0.0f;
    glm::vec2 velocity = glm::vec2(0.0f);
};

enum class Collider2DType {
    Box = 0,
    Polygon = 1,
    Edge = 2,
    Circle = 3,
    Line = 4
};

struct Collider2DComponent {
    bool enabled = true;
    bool isTrigger = false;
    Collider2DType type = Collider2DType::Box;
    glm::vec2 boxSize = glm::vec2(1.0f);
    glm::vec2 offset = glm::vec2(0.0f);
    std::vector<glm::vec2> points;
    bool closed = false;
    float edgeThickness = 0.05f;
    float radius = 0.5f;
    float outlineAlphaThreshold = 0.5f;
    float outlineTolerance = 1.5f;
    int outlineMaxVertices = 32;
    bool outlineClosed = true;
    std::string outlineSourcePath;
};

struct ParallaxLayer2DComponent {
    bool enabled = true;
    int order = 0;
    float factor = 1.0f; // 1 = world locked, 0 = camera locked
    bool repeatX = false;
    bool repeatY = false;
    bool disableCulling = false;
    glm::vec2 repeatSpacing = glm::vec2(0.0f);
};

struct CameraFollow2DComponent {
    bool enabled = true;
    int targetId = -1;
    glm::vec2 offset = glm::vec2(0.0f);
    float smoothTime = 0.0f; // seconds; 0 snaps to target
};

struct AudioSourceComponent {
    bool enabled = true;
    std::string clipPath;
    float volume = 1.0f;
    bool loop = true;
    bool playOnStart = true;
    bool spatial = true;
    float spatialBlend = 1.0f; // 0 = fully 2D/centered, 1 = fully placed in world
    float minDistance = 1.0f;
    float maxDistance = 25.0f;
    AudioRolloffMode rolloffMode = AudioRolloffMode::Logarithmic;
    float rolloff = 1.0f;
    float customMidDistance = 0.5f;
    float customMidGain = 0.6f;
    float customEndGain = 0.0f;
};

struct AudioFXComponent {
    bool enabled = true;
    bool global = false;
    AudioFXChain chain;
};

struct VideoPlayerComponent {
    bool enabled = true;
    std::string videoPath;
    bool playOnAwake = true;
    bool loop = true;
    bool flipX = false;
    bool flipY = false;
    float playbackSpeed = 1.0f;
    bool playAudioFromVideo = true;
    bool routeAudioToSource = false;
    int outputAudioSourceObjectId = -1;
    float videoAudioVolume = 1.0f;
    bool videoAudioMuted = false;
    bool syncAudioToVideo = true;
    float audioSyncTolerance = 0.05f;
};

struct ParticleSystem2DComponent {
    struct MinMaxFloat {
        float min = 1.0f;
        float max = 1.0f;
        bool random = false;

        bool operator==(const MinMaxFloat& other) const {
            return min == other.min && max == other.max && random == other.random;
        }

        bool operator!=(const MinMaxFloat& other) const {
            return !(*this == other);
        }
    };

    struct Particle {
        bool alive = false;
        float age = 0.0f;
        float lifetime = 1.0f;
        glm::vec3 position = glm::vec3(0.0f);
        glm::vec3 velocity = glm::vec3(0.0f);
        float size = 1.0f;
        float rotation = 0.0f;
        float angularVelocity = 0.0f;
        glm::vec4 startColor = glm::vec4(1.0f);
        uint32_t seed = 1u;
        int sheetFrameOffset = 0; // texture sheet start frame, rolled at emission
    };

    // Values of the int-backed settings below. Kept as ints so the scene format and the
    // Inspector combos stay plain; these name them for code.
    enum Shape { ShapePoint = 0, ShapeCircle = 1, ShapeBox = 2, ShapeCone = 3,
                 ShapeHemisphere = 4, ShapeEdge = 5 };
    enum SimulationSpace { SpaceLocal = 0, SpaceWorld = 1 };
    enum RenderAlignment { AlignBillboard = 0, AlignStretched = 1, AlignHorizontal = 2,
                           AlignVertical = 3, AlignLocal = 4 };
    enum BlendMode { BlendAlpha = 0, BlendAdditive = 1, BlendPremultiplied = 2, BlendMultiply = 3 };
    enum SheetMode { SheetGrid = 0, SheetSpriteClips = 1 };
    enum SheetTiming { SheetOverLifetime = 0, SheetFps = 1 };

    bool enabled = true;
    bool looping = true;
    bool prewarm = false;
    bool playOnAwake = true;
    bool playing = true;
    bool paused = false;
    bool autoRandomSeed = true;
    uint32_t randomSeed = 1u;
    float startDelay = 0.0f;
    MinMaxFloat startLifetime{5.0f, 5.0f, false};
    MinMaxFloat startSpeed{2.0f, 5.0f, true};
    MinMaxFloat startSize{0.18f, 0.35f, true};
    MinMaxFloat startRotation{0.0f, 360.0f, true};
    glm::vec4 startColor = glm::vec4(1.0f);
    float gravityModifier = 0.0f;
    float simulationSpeed = 1.0f;
    int maxParticles = 1000;
    float emissionRate = 20.0f;
    int burstCount = 0;
    float burstTime = 0.0f;
    bool burstLoop = false;
    int shape = 0;
    float shapeRadius = 0.5f;
    glm::vec3 shapeBox = glm::vec3(1.0f, 1.0f, 0.0f);
    bool emitIn3D = false;
    bool velocityOverLifetimeEnabled = false;
    glm::vec3 velocityOverLifetime = glm::vec3(0.0f);
    bool colorOverLifetimeEnabled = true;
    glm::vec4 colorOverLifetime = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    bool sizeOverLifetimeEnabled = true;
    float sizeOverLifetime = 0.0f;
    bool rotationOverLifetimeEnabled = false;
    float rotationOverLifetime = 0.0f;
    bool noiseEnabled = false;
    float noiseStrength = 0.0f;
    float noiseFrequency = 1.0f;
    std::string texturePath;
    std::string materialPath;
    bool receiveLighting2D = true;
    bool unlitLighting2D = false;
    float emissiveLighting2D = 0.0f;
    // World: particles keep the position they were born at when the emitter moves
    // (trails, smoke). Local: they ride along with the emitter.
    int simulationSpace = SpaceLocal;
    float shapeAngle = 25.0f;          // cone half-angle, degrees
    // How each particle's quad faces the camera in 3D views.
    int renderAlignment = AlignBillboard;
    float stretchLengthScale = 2.0f;   // stretched: length in multiples of size...
    float stretchSpeedScale = 0.1f;    // ...plus this much per unit of speed
    int blendMode = BlendAlpha;
    // Texture sheet animation: a grid of equal tiles, or the clips of the texture's
    // .spritesheet sidecar (the same clips a UI Image picks up from that texture).
    bool textureSheetEnabled = false;
    int textureSheetMode = SheetGrid;
    int textureSheetTilesX = 1;
    int textureSheetTilesY = 1;
    int textureSheetTiming = SheetOverLifetime;
    float textureSheetCycles = 1.0f;   // over lifetime: passes through the frames per life
    float textureSheetFps = 12.0f;
    bool textureSheetLoop = true;      // fps: wrap, or hold the last frame
    int textureSheetStartFrame = 0;
    int textureSheetFrameCount = 0;    // 0 = every frame from the start frame on
    bool textureSheetRandomStart = false;
    float runtimeAccumulator = 0.0f;
    float runtimeTime = 0.0f;
    double runtimeLastUpdateTime = 0.0;
    bool runtimeInitialized = false;
    std::vector<Particle> particles;
    // Resolved by Engine::updateParticleSystems so renderers never touch the disk. Not saved.
    std::string runtimeMaterialKey;           // materialPath these were read from
    std::string runtimeMaterialTexture;       // the material's albedo
    glm::vec4 runtimeMaterialTint = glm::vec4(1.0f);
    glm::vec3 runtimeMaterialEmission = glm::vec3(0.0f);
    int runtimeMaterialFilter = -1;           // -1: the object's own filter
    long long runtimeMaterialStamp = 0;
    std::string runtimeSheetKey;              // texture whose .spritesheet supplied the clips
    std::vector<glm::ivec4> runtimeSheetRects;
    long long runtimeSheetStamp = 0;
    double runtimeAssetCheckTime = -1.0;
    // Script requests (Particles.Emit / Particles.Play), consumed by the next simulation step.
    int runtimePendingEmit = 0;
    // Bumped on every scripted Play/Emit and mixed into the auto random seed, so replaying a
    // burst does not replay the exact same particles.
    uint32_t runtimeEmitSalt = 0;
};

struct ReverbZoneComponent {
    bool enabled = true;
    ReverbPreset preset = ReverbPreset::Room;
    ReverbZoneShape shape = ReverbZoneShape::Box;
    glm::vec3 boxSize = glm::vec3(6.0f);
    float radius = 6.0f;
    float blendDistance = 1.0f;
    float minDistance = 1.0f;
    float maxDistance = 15.0f;
    float room = -1000.0f;          // dB
    float roomHF = -100.0f;         // dB
    float roomLF = 0.0f;            // dB
    float decayTime = 1.49f;        // s
    float decayHFRatio = 0.83f;     // 0.1..2
    float reflections = -2602.0f;   // dB
    float reflectionsDelay = 0.007f; // s
    float reverb = 200.0f;          // dB
    float reverbDelay = 0.011f;     // s
    float hfReference = 5000.0f;    // Hz
    float lfReference = 250.0f;     // Hz
    float roomRolloffFactor = 0.0f;
    float diffusion = 100.0f;       // 0..100
    float density = 100.0f;         // 0..100
};

struct GroundBakedTypeComponent {
    bool enabled = true;
    bool includeInBake = true;
    float areaCost = 1.0f;
};

struct ObsticleObjectComponent {
    bool enabled = true;
    bool carve = true;
    float padding = 0.2f;
};

struct AIAgentComponent {
    bool enabled = true;
    bool useTargetObject = true;
    int targetId = -1;
    glm::vec3 destination = glm::vec3(0.0f);
    float speed = 2.5f;
    float stoppingDistance = 0.2f;
    float repathInterval = 0.5f;
    bool autoRepath = true;
    bool alignToPath = true;
    bool debugDrawPath = true;
    float turnSpeed = 540.0f;          // deg/sec when aligning to path; 0 = snap instantly.
    float avoidancePadding = 0.3f;     // extra clearance from obstacles/walls during pathing (world units).
};

struct OffMeshLinkComponent {
    bool enabled = true;
    glm::vec3 startPoint = glm::vec3(0.0f);
    glm::vec3 endPoint = glm::vec3(0.0f);
    bool bidirectional = true;
    float costOverride = 0.0f; // 0 = use planar distance
};

struct Rig25DRootComponent {
    bool enabled = true;
};

enum class MapTransitionKind {
    Door = 0,
    OpenPassage = 1,
    Elevator = 2,
    Ladder = 3,
    Teleport = 4,
    Loading = 5,
    Custom = 6
};

struct MapRootComponent {
    bool enabled = true;
    std::string mapId;
    std::string startSectorId;
    std::string activeSectorId;
    int sectorVisibilityMode = 0;
    std::string notes;
};

struct MapSectorComponent {
    bool enabled = true;
    std::string sectorId;
    glm::vec2 graphPosition = glm::vec2(0.0f);
    glm::vec3 color = glm::vec3(0.31f, 0.55f, 0.90f);
    bool useCustomBounds = false;
    glm::vec3 boundsCenter = glm::vec3(0.0f);
    glm::vec3 boundsSize = glm::vec3(8.0f, 4.0f, 8.0f);
    std::string streamingTag;
    std::string notes;
    float estimatedMemoryMB = 0.0f;
};

struct MapTransitionComponent {
    bool enabled = true;
    std::string transitionId;
    std::string sourceSectorId;
    std::string destinationSectorId;
    std::string sourcePortalId;
    std::string destinationPortalId;
    bool bidirectional = true;
    bool locked = false;
    std::string condition;
    MapTransitionKind kind = MapTransitionKind::Door;
    std::string editorLabel;
    bool hasEntryTransform = false;
    glm::vec3 entryPosition = glm::vec3(0.0f);
    float entryYawDeg = 0.0f;
};

struct MapPortalComponent {
    bool enabled = true;
    std::string portalId;
    std::string transitionId;
    std::string sectorId;
    glm::vec2 openingSize = glm::vec2(1.2f, 2.2f);
};

struct MapMeshComponent {
    bool enabled = true;
    float gridSize = 0.5f;
    bool snapToGrid = true;
    bool vertexSnapping = false;
    bool surfaceSnapping = false;
    bool autoCollision = true;
};

struct AssemblageComponent {
    bool enabled = true;
    std::string assemblageId;
    std::string assetPath;
    bool showGrid = true;
    bool snapToGrid = true;
    glm::vec4 gridColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.12f);
};

struct AssemblageLayerComponent {
    bool enabled = true;
    int layerId = -1;
    int sortingOrder = 0;
    bool locked = false;
    float opacity = 1.0f;
    glm::vec4 tint = glm::vec4(1.0f);
    bool receiveLighting2D = true;
    bool unlitLighting2D = false;
    float emissiveLighting2D = 0.0f;
    bool collisionEnabled = true;
};

struct Rig25DNodeComponent {
    bool enabled = true;
    int nodeId = -1;
    std::string nodeName;
};

struct ModuObjInstanceRef {
    std::string assetId;
    std::string instanceId;
    std::string lastKnownPath;
    int sourceLocalId = -1;
    bool isRoot = false;
    bool sourceMissing = false;
};

enum class XRHand { Left = 0, Right = 1 };

enum class XRControllerPoseSource { Grip = 0, Aim = 1 };

struct XROriginComponent {
    bool enabled = true;
    enum class Mode { FollowProjectSettings = 0, Floor = 1, EyeLevel = 2 };
    Mode trackingOriginMode = Mode::FollowProjectSettings;
    float rigScale = 1.0f;
    float cameraYOffset = 0.0f;
};

struct XRCameraComponent {
    bool enabled = true;
    bool trackPosition = true;
    bool trackRotation = true;
    bool applyTracking = true;
};

struct XRControllerComponent {
    bool enabled = true;
    XRHand hand = XRHand::Left;
    XRControllerPoseSource poseSource = XRControllerPoseSource::Grip;
    bool trackPosition = true;
    bool trackRotation = true;
    bool hideWhenNotTracked = false;
};

struct XRActionBasedControllerComponent {
    bool enabled = true;
    XRHand hand = XRHand::Left;

    int selectButton = 0;
    int activateButton = 2;
    int uiPressButton = 0;

    bool enableHaptics = true;
    float hapticAmplitude = 0.5f;
    float hapticDuration = 0.1f;

    bool selectHeld = false;
    bool selectStarted = false;
    bool selectEnded = false;
    bool activateHeld = false;
    bool activateStarted = false;
    bool activateEnded = false;
    bool uiPressHeld = false;
};

struct XRRayInteractorComponent {
    bool enabled = true;
    enum class RayType { Straight = 0 };
    RayType rayType = RayType::Straight;
    float maxDistance = 20.0f;
    uint32_t interactionMask = 0xFFFFFFFFu;
    bool uiInteraction = true;
    bool showLineVisual = true;
    glm::vec4 lineColor = glm::vec4(0.35f, 0.75f, 1.0f, 0.85f);

    int hoveredObjectId = -1;
    int selectedObjectId = -1;
    glm::vec3 hitPoint = glm::vec3(0.0f);
    bool hasHit = false;
};

struct XRDirectInteractorComponent {
    bool enabled = true;
    float interactionRadius = 0.1f;
    uint32_t interactionMask = 0xFFFFFFFFu;

    int hoveredObjectId = -1;
    int selectedObjectId = -1;
};

struct XRGrabInteractableComponent {
    bool enabled = true;
    enum class MovementType { Instant = 0, Kinematic = 1, VelocityTracking = 2 };
    MovementType movementType = MovementType::VelocityTracking;

    bool allowLeftHand = true;
    bool allowRightHand = true;
    int attachTransformId = -1;
    bool trackPosition = true;
    bool trackRotation = true;
    bool throwOnDetach = true;
    float throwVelocityScale = 1.0f;
    float throwAngularVelocityScale = 1.0f;

    int heldByObjectId = -1;
    int hoveredByObjectId = -1;
    bool wasKinematic = false;
    bool hadGravity = true;
    bool stateSaved = false;

    bool hoverEnteredThisFrame = false;
    bool hoverExitedThisFrame = false;
    bool selectEnteredThisFrame = false;
    bool selectExitedThisFrame = false;
    bool activatedThisFrame = false;
    bool deactivatedThisFrame = false;
};

struct NetworkIdentityComponent {
    bool enabled = true;
    std::string assetId;
    bool syncPosition = true;
    bool syncRotation = true;
    bool syncScale = false;
    bool syncVelocity = false;
    int syncMode = 1;
    int sendRateHz = 0;
    float interpolationDelay = 0.1f;
    float maxExtrapolation = 0.25f;
    bool ownerOnlyWrites = true;
    bool spawnerOwns = true;

    uint32_t runtimeNetworkId = 0;
    int runtimeOwner = 0;
    bool runtimeHasAuthority = false;
    bool runtimeSpawned = false;
};

struct NetworkManagerComponent {
    bool enabled = true;
    std::string appId;
    std::string appVersion = "1.0";
    std::string region;
    std::string nickname = "Player";
    bool autoConnect = false;
    bool offlineMode = false;
    int maxPlayers = 0;
    bool autoJoinLobby = true;
    std::string defaultRoomName;
    int sendRateHz = 20;
    int serializationRateHz = 10;
    int maxOutboundBytesPerSecond = 0;
};

class SceneObject {
public:
    std::string name;
    ObjectType type;
    bool enabled = true;
    bool IsInvariable = false;
    // Derived each hierarchy update: true when all ancestors are locally enabled.
    bool hierarchyEnabled = true;
    int layer = 0;
    std::string tag = "Untagged";
    glm::vec4 editorIconTint = glm::vec4(1.0f);
    std::string editorIconPath;
    bool editorIconShowInViewport = false;
    bool hasRenderer = false;
    RenderType renderType = RenderType::None;
    bool faceCamera = false;
    bool hasLight = false;
    bool hasLight2D = false;
    bool hasReflectionCast = false;
    bool hasCamera = false;
    bool hasPostFX = false;
    bool hasUI = false;
    bool hasShadowCaster2D = false;
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;
    glm::vec3 localPosition;
    glm::vec3 localRotation;
    glm::vec3 localScale;
    bool localInitialized = false;
    int id;
    int parentId = -1;
    std::vector<int> childIds;
    bool hasModuObjInstance = false;
    ModuObjInstanceRef moduObjInstance;
    bool hasNetworkIdentity = false;
    NetworkIdentityComponent networkIdentity;
    bool hasNetworkManager = false;
    NetworkManagerComponent networkManager;
    bool isExpanded = true;
    std::string meshPath;  // Path to imported model file
    int meshId = -1;       // Index into loaded mesh caches (OBJLoader / ModelLoader)
    int meshSourceIndex = -1; // Source mesh index for multi-mesh models
    MaterialProperties material;
    std::string materialPath;       // Optional external material asset
    std::string albedoTexturePath;
    std::string overlayTexturePath;
    std::string normalMapPath;
    std::string shaderPackPath;
    std::string vertexShaderPath;
    std::string fragmentShaderPath;
    bool useOverlay = false;
    LightComponent light;  // Only used when type is a light
    ReflectionCastComponent reflectionCast;
    Light2DComponent light2D;
    ShadowCaster2DComponent shadowCaster2D;
    CameraComponent camera; // Only used when type is camera
    PostFXSettings postFx;  // Only used when type is PostFXNode
    std::vector<ScriptComponent> scripts;
    std::vector<std::string> additionalMaterialPaths;
    bool hasRigidbody = false;
    RigidbodyComponent rigidbody;
    bool hasRigidbody2D = false;
    Rigidbody2DComponent rigidbody2D;
    bool hasCollider2D = false;
    Collider2DComponent collider2D;
    bool hasParallaxLayer2D = false;
    ParallaxLayer2DComponent parallaxLayer2D;
    bool hasCameraFollow2D = false;
    CameraFollow2DComponent cameraFollow2D;
    bool hasCollider = false;
    ColliderComponent collider;
    bool hasPlayerController = false;
    PlayerControllerComponent playerController;
    bool hasAudioSource = false;
    AudioSourceComponent audioSource;
    bool hasAudioFX = false;
    AudioFXComponent audioFX;
    bool hasVideoPlayer = false;
    VideoPlayerComponent videoPlayer;
    bool hasParticleSystem2D = false;
    ParticleSystem2DComponent particleSystem2D;
    bool hasReverbZone = false;
    ReverbZoneComponent reverbZone;
    bool hasXROrigin = false;
    XROriginComponent xrOrigin;
    bool hasXRCamera = false;
    XRCameraComponent xrCamera;
    bool hasXRController = false;
    XRControllerComponent xrController;
    bool hasXRActionBasedController = false;
    XRActionBasedControllerComponent xrActionBasedController;
    bool hasXRRayInteractor = false;
    XRRayInteractorComponent xrRayInteractor;
    bool hasXRDirectInteractor = false;
    XRDirectInteractorComponent xrDirectInteractor;
    bool hasXRGrabInteractable = false;
    XRGrabInteractableComponent xrGrabInteractable;
    bool hasGroundBakedType = false;
    GroundBakedTypeComponent groundBakedType;
    bool hasObsticleObject = false;
    ObsticleObjectComponent obsticleObject;
    bool hasAIAgent = false;
    AIAgentComponent aiAgent;
    bool hasOffMeshLink = false;
    OffMeshLinkComponent offMeshLink;
    bool hasAnimation = false;
    AnimationComponent animation;
    bool hasSkeletalAnimation = false;
    SkeletalAnimationComponent skeletal;
    bool hasRig25DRoot = false;
    Rig25DRootComponent rig25DRoot;
    bool hasRig25DNode = false;
    Rig25DNodeComponent rig25DNode;
    UIElementComponent ui;
    bool runtimeHasAlbedoTextureOverride = false;
    unsigned int runtimeAlbedoTextureOverrideId = 0;
    bool runtimeAlbedoTextureFlipX = false;
    bool runtimeAlbedoTextureFlipY = false;
    std::vector<std::string> inspectorComponentOrder;
    int nextInspectorScriptId = 1;
    bool hasMapRoot = false;
    MapRootComponent mapRoot;
    bool hasMapSector = false;
    MapSectorComponent mapSector;
    bool hasMapTransition = false;
    MapTransitionComponent mapTransition;
    bool hasMapPortal = false;
    MapPortalComponent mapPortal;
    bool hasMapMesh = false;
    MapMeshComponent mapMesh;
    bool editorSectorHidden = false;
    bool hasAssemblage = false;
    AssemblageComponent assemblage;
    bool hasAssemblageLayer = false;
    AssemblageLayerComponent assemblageLayer;
    PlayerViewMotionSettings playerViewMotion;
    PlayerControlFeelSettings playerControlFeel;
    PlayerMovementAudioSettings playerMovementAudio;
    std::string metallicRoughnessMapPath;
    std::string metallicMapPath;
    std::string roughnessMapPath;
    std::string opacityMapPath;
    std::string emissiveMapPath;
    std::string occlusionMapPath;
    std::string heightMapPath;

    SceneObject(const std::string& name, ObjectType type, int id)
        : name(name),
          type(type),
          position(0.0f),
          rotation(0.0f),
          scale(1.0f),
          localPosition(0.0f),
          localRotation(0.0f),
          localScale(1.0f),
          localInitialized(true),
          id(id) {}
};

inline int CountNetworkManagers(const std::vector<SceneObject>& objects, int* outFirstId = nullptr) {
    int count = 0;
    for (const SceneObject& obj : objects) {
        if (!obj.hasNetworkManager || !obj.networkManager.enabled) continue;
        if (count == 0 && outFirstId) *outFirstId = obj.id;
        ++count;
    }
    return count;
}

inline int FindActiveNetworkManager(const std::vector<SceneObject>& objects) {
    int firstId = -1;
    return CountNetworkManagers(objects, &firstId) == 1 ? firstId : -1;
}

inline bool HasRendererComponent(const SceneObject& obj) {
    return obj.hasRenderer && obj.renderType != RenderType::None;
}

inline bool IsRawMeshPath(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".rmesh";
}

inline bool IsRawMeshPath(const std::string& path) {
    if (path.empty()) return false;
    return IsRawMeshPath(fs::path(path));
}

inline bool IsMMeshPath(const fs::path& path) {
    if (path.empty()) return false;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mmesh";
}

inline bool IsMeshEditablePath(const fs::path& path) {
    return IsRawMeshPath(path) || IsMMeshPath(path);
}

inline bool IsMeshEditablePath(const std::string& path) {
    if (path.empty()) return false;
    return IsMeshEditablePath(fs::path(path));
}

inline bool IsMMeshPath(const std::string& path) {
    if (path.empty()) return false;
    return IsMMeshPath(fs::path(path));
}

inline bool IsObjectEnabledInHierarchy(const SceneObject& obj) {
    return obj.enabled && obj.hierarchyEnabled && !obj.editorSectorHidden;
}

inline bool HasUIComponent(const SceneObject& obj) {
    return obj.hasUI && obj.ui.type != UIElementType::None;
}

inline bool UsesUIOnly2DPhysics(const SceneObject& obj) {
    return HasUIComponent(obj) && obj.type != ObjectType::Sprite25D;
}

inline bool HasRig25DRootComponent(const SceneObject& obj) {
    return obj.hasRig25DRoot;
}

inline bool HasRig25DNodeComponent(const SceneObject& obj) {
    return obj.hasRig25DNode;
}

inline bool IsRig25DObject(const SceneObject& obj) {
    return HasRig25DRootComponent(obj) || HasRig25DNodeComponent(obj);
}

inline bool IsRig25DNodeTargetable(const SceneObject& obj) {
    return HasRig25DNodeComponent(obj) && obj.rig25DNode.nodeId >= 0;
}

inline bool IsPipeline25DOnlyRigObject(const SceneObject& obj) {
    return IsRig25DObject(obj);
}

inline float GetAudioSpatialBlend(const AudioSourceComponent& src) {
    return std::clamp(src.spatial ? src.spatialBlend : 0.0f, 0.0f, 1.0f);
}

inline bool AudioSourceUsesSpatialization(const AudioSourceComponent& src) {
    return GetAudioSpatialBlend(src) > 0.001f;
}

inline std::string MakeInspectorScriptComponentKey(int inspectorId) {
    return "script:" + std::to_string(inspectorId);
}

inline bool IsInspectorScriptComponentKey(const std::string& key) {
    return key.rfind("script:", 0) == 0;
}

inline int ParseInspectorScriptComponentId(const std::string& key) {
    if (!IsInspectorScriptComponentKey(key)) return -1;
    try {
        return std::stoi(key.substr(7));
    } catch (...) {
        return -1;
    }
}

inline const std::vector<std::string>& GetDefaultInspectorComponentOrderTemplate() {
    static const std::vector<std::string> order = {
        "map_root",
        "map_sector",
        "map_transition",
        "map_portal",
        "map_mesh",
        "assemblage",
        "assemblage_layer",
        "ui",
        "collider",
        "player_controller",
        "rigidbody3d",
        "rigidbody2d",
        "collider2d",
        "parallax2d",
        "audio_source",
        "audio_fx",
        "video_player",
        "network_manager",
        "network_identity",
        "particle_system2d",
        "ground_baked",
        "obstacle",
        "ai_agent",
        "rig25d_root",
        "rig25d_node",
        "animation",
        "skeletal_animation",
        "reverb_zone",
        "xr_origin",
        "xr_camera",
        "xr_controller",
        "xr_action_controller",
        "xr_ray_interactor",
        "xr_direct_interactor",
        "xr_grab_interactable",
        "camera",
        "camera_follow2d",
        "post_fx",
        "reflection_cast",
        "script",
        "renderer",
        "light",
        "light2d",
        "shadow_caster2d"
    };
    return order;
}

inline int RuntimeUiSortOrderOf(const SceneObject& obj) {
    if (obj.hasAssemblageLayer) return obj.assemblageLayer.sortingOrder;
    return obj.hasUI ? obj.ui.sortingOrder : 0;
}

inline void EnsureInspectorComponentMetadata(SceneObject& obj) {
    int nextScriptId = std::max(1, obj.nextInspectorScriptId);
    std::unordered_set<int> usedScriptIds;
    for (ScriptComponent& script : obj.scripts) {
        if (script.inspectorId > 0 && usedScriptIds.insert(script.inspectorId).second) {
            nextScriptId = std::max(nextScriptId, script.inspectorId + 1);
            continue;
        }
        while (usedScriptIds.count(nextScriptId) > 0) {
            ++nextScriptId;
        }
        script.inspectorId = nextScriptId++;
        usedScriptIds.insert(script.inspectorId);
    }
    obj.nextInspectorScriptId = nextScriptId;

    std::vector<std::string> presentKeys;
    presentKeys.reserve(30 + obj.scripts.size());
    if (obj.hasMapRoot) presentKeys.push_back("map_root");
    if (obj.hasMapSector) presentKeys.push_back("map_sector");
    if (obj.hasMapTransition) presentKeys.push_back("map_transition");
    if (obj.hasMapPortal) presentKeys.push_back("map_portal");
    if (obj.hasMapMesh) presentKeys.push_back("map_mesh");
    if (obj.hasAssemblage) presentKeys.push_back("assemblage");
    if (obj.hasAssemblageLayer) presentKeys.push_back("assemblage_layer");
    if (HasUIComponent(obj)) presentKeys.push_back("ui");
    if (obj.hasCollider) presentKeys.push_back("collider");
    if (obj.hasPlayerController) presentKeys.push_back("player_controller");
    if (obj.hasRigidbody) presentKeys.push_back("rigidbody3d");
    if (obj.hasRigidbody2D) presentKeys.push_back("rigidbody2d");
    if (obj.hasCollider2D) presentKeys.push_back("collider2d");
    if (obj.hasParallaxLayer2D) presentKeys.push_back("parallax2d");
    if (obj.hasAudioSource) presentKeys.push_back("audio_source");
    if (obj.hasAudioFX) presentKeys.push_back("audio_fx");
    if (obj.hasVideoPlayer) presentKeys.push_back("video_player");
    if (obj.hasNetworkManager) presentKeys.push_back("network_manager");
    if (obj.hasNetworkIdentity) presentKeys.push_back("network_identity");
    if (obj.hasParticleSystem2D) presentKeys.push_back("particle_system2d");
    if (obj.hasGroundBakedType) presentKeys.push_back("ground_baked");
    if (obj.hasObsticleObject) presentKeys.push_back("obstacle");
    if (obj.hasAIAgent) presentKeys.push_back("ai_agent");
    if (obj.hasOffMeshLink) presentKeys.push_back("offmesh_link");
    if (obj.hasRig25DRoot) presentKeys.push_back("rig25d_root");
    if (obj.hasRig25DNode) presentKeys.push_back("rig25d_node");
    if (obj.hasAnimation) presentKeys.push_back("animation");
    if (obj.hasSkeletalAnimation) presentKeys.push_back("skeletal_animation");
    if (obj.hasReverbZone) presentKeys.push_back("reverb_zone");
    if (obj.hasXROrigin) presentKeys.push_back("xr_origin");
    if (obj.hasXRCamera) presentKeys.push_back("xr_camera");
    if (obj.hasXRController) presentKeys.push_back("xr_controller");
    if (obj.hasXRActionBasedController) presentKeys.push_back("xr_action_controller");
    if (obj.hasXRRayInteractor) presentKeys.push_back("xr_ray_interactor");
    if (obj.hasXRDirectInteractor) presentKeys.push_back("xr_direct_interactor");
    if (obj.hasXRGrabInteractable) presentKeys.push_back("xr_grab_interactable");
    if (obj.hasCamera) presentKeys.push_back("camera");
    if (obj.hasCameraFollow2D) presentKeys.push_back("camera_follow2d");
    if (obj.hasPostFX) presentKeys.push_back("post_fx");
    if (obj.hasReflectionCast) presentKeys.push_back("reflection_cast");
    if (obj.hasRenderer) presentKeys.push_back("renderer");
    if (obj.hasLight) presentKeys.push_back("light");
    if (obj.hasLight2D) presentKeys.push_back("light2d");
    if (obj.hasShadowCaster2D) presentKeys.push_back("shadow_caster2d");
    for (const ScriptComponent& script : obj.scripts) {
        presentKeys.push_back(MakeInspectorScriptComponentKey(script.inspectorId));
    }

    std::unordered_set<std::string> presentSet(presentKeys.begin(), presentKeys.end());
    std::vector<std::string> sanitizedOrder;
    sanitizedOrder.reserve(presentKeys.size());
    std::unordered_set<std::string> seenKeys;
    for (const std::string& key : obj.inspectorComponentOrder) {
        if (presentSet.count(key) == 0) continue;
        if (!seenKeys.insert(key).second) continue;
        sanitizedOrder.push_back(key);
    }

    const auto appendIfMissing = [&](const std::string& key) {
        if (presentSet.count(key) == 0) return;
        if (!seenKeys.insert(key).second) return;
        sanitizedOrder.push_back(key);
    };

    for (const std::string& templateKey : GetDefaultInspectorComponentOrderTemplate()) {
        if (templateKey == "script") {
            for (const ScriptComponent& script : obj.scripts) {
                appendIfMissing(MakeInspectorScriptComponentKey(script.inspectorId));
            }
            continue;
        }
        appendIfMissing(templateKey);
    }

    obj.inspectorComponentOrder = std::move(sanitizedOrder);
}
