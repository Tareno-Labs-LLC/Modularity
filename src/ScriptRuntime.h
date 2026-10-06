#pragma once

#include "ScriptSdkCommon.h"
#include "SceneObject.h"
#include "IPhysicsBackend.h"
#include "imgui.h"
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32) && defined(DeleteFile)
#undef DeleteFile
#endif

class Engine;

#if defined(_WIN32)
    #if defined(MODULARITY_SCRIPT_HOST)
        #define MODULARITY_SCRIPT_API __declspec(dllexport)
    #elif defined(MODULARITY_SCRIPT_IMPORTS)
        #define MODULARITY_SCRIPT_API __declspec(dllimport)
    #else
        #define MODULARITY_SCRIPT_API
    #endif
    #define MODULARITY_SCRIPT_EXPORT __declspec(dllexport)
#else
    #define MODULARITY_SCRIPT_API
    #define MODULARITY_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

#define MODULARITY_NATIVE_SCRIPT_ABI_VERSION 58

#define MODULARITY_SCRIPT_LAYOUT_SIGNATURE() \
    ((static_cast<unsigned long long>(sizeof(SceneObject)) << 40) ^ \
     (static_cast<unsigned long long>(sizeof(ScriptComponent)) << 20) ^ \
     static_cast<unsigned long long>(sizeof(ScriptContext)))

struct MODULARITY_SCRIPT_API ScriptContext {
    Engine* engine = nullptr;
    SceneObject* object = nullptr;
    ScriptComponent* script = nullptr;
    enum class AutoSettingType { Bool, Float, Int, Vec3, StringBuf, String };
    struct AutoSettingEntry {
        AutoSettingType type;
        std::string key;
        void* ptr = nullptr;
        size_t bufSize = 0;
        bool initialBool = false;
        float initialFloat = 0.0f;
        int initialInt = 0;
        glm::vec3 initialVec3 = glm::vec3(0.0f);
        std::string initialString;
    };
    std::vector<AutoSettingEntry> autoSettings;

    // Convenience helpers for scripts
    /// @summary Resolve the first scene object with an exact name match.
    /// @usage Useful for cross-object links in gameplay scripts.
    /// @howto Call once (for example in Begin) and cache the id/name you need.
    /// @param name Scene object name to match exactly.
    /// @returns Matching object pointer, or nullptr when not found.
    SceneObject* FindObjectByName(const std::string& name);
    SceneObject* FindObjectById(int id);
    // First descendant of parentObjectId with this exact name, or -1. Scene objects
    // only store parentId, so this walks each candidate's parent chain rather than a
    // child list. Needed because ModuObj::Instantiate remaps ids, parentId and
    // children when it spawns a packaged object but does NOT remap [ObjectRef]
    // strings - so a prefab cannot reference its own parts by ref and has to find
    // them by name at runtime.
    int FindChildByName(int parentObjectId, const std::string& name) const;
    SceneObject* ResolveObjectRef(const std::string& ref);
    void SyncObjectLocalTransform(int objectId);
    int GetSceneObjectCount() const;
    int GetSceneObjectIdAt(int index) const;
    const SceneObject* GetSceneObjectAt(int index) const;
    bool IsObjectEnabled() const;
    void SetObjectEnabled(bool enabled);
    int GetLayer() const;
    void SetLayer(int layer);
    std::string GetTag() const;
    void SetTag(const std::string& tag);
    bool HasTag(const std::string& tag) const;
    bool IsInLayer(int layer) const;
    void SetPosition(const glm::vec3& pos);
    void SetPosition2D(const glm::vec2& pos);
    void SetRotation(const glm::vec3& rot);
    void SetScale(const glm::vec3& scl);
    void GetPlanarYawPitchVectors(float pitchDeg, float yawDeg, glm::vec3& outForward, glm::vec3& outRight) const;
    glm::vec3 GetMoveInputWASD(float pitchDeg, float yawDeg) const;
    // Also keeps the mouse captured while it is applying (see RequestMouseCapture).
    bool ApplyMouseLook(float& pitchDeg, float& yawDeg, float sensitivity, float maxDelta, float deltaTime,
                        bool requireMouseButton) const;
    // The game only captures the mouse (hidden + locked) while something asks for it. Call
    // this every frame the mouse steers the camera/character; stop and the pointer returns.
    void RequestMouseCapture();
    bool IsMouseCaptured() const;
    // GLFW button index: 0 = left, 1 = right, 2 = middle.
    bool IsMouseButtonDown(int glfwButton) const;
    // Show a project image as the pointer while it is visible over the game. Hotspot is a
    // 0..1 fraction of the image (0,0 = top-left); height is pixels at 100% display scale.
    void SetCursorImage(const std::string& path, float hotspotX, float hotspotY, float heightPx);
    void ClearCursorImage();
    int GetSelectedObjectId() const;
    bool IsSprintDown() const;
    bool IsJumpDown() const;
    bool IsKeyDown(int glfwKey, ImGuiKey imguiKey = ImGuiKey_None) const;
    bool IsKeyPressed(int glfwKey, ImGuiKey imguiKey = ImGuiKey_None) const;
    int GetTouchCount() const;
    bool GetTouch(int index, float& x, float& y) const;
    bool GetPrimaryTouch(float& x, float& y) const;
    bool IsTouchActive() const;
    bool IsXRActive() const;
    bool IsXRDeviceTracked(int device) const;
    int GetXRTrackingState(int device) const;
    bool GetXRButton(int device, int button) const;
    bool GetXRButtonDown(int device, int button) const;
    bool GetXRButtonUp(int device, int button) const;
    bool IsXRButtonBound(int device, int button) const;
    float GetXRAxis(int device, int axis) const;
    bool GetXRAxis2D(int device, int axis, float& x, float& y) const;
    bool GetXRPose(int device, int poseKind, glm::vec3& position, glm::quat& rotation) const;
    bool GetXRVelocity(int device, glm::vec3& linear, glm::vec3& angular) const;
    void XRHapticPulse(int device, float amplitude, float duration, float frequency = 0.0f) const;
    std::string GetXRInteractionProfile(int device) const;

    // --- Networking (ModuNetwork) -------------------------------------------
    //
    // Flat by design: every parameter and return is an int, float, string or glm
    // vector, so no Net:: type ever crosses into a compiled script and the ABI does
    // not move when the networking internals do. ModuNetworkScriptApi.h is the
    // facade that makes these read like an API.
    //
    // With no session running every query reports a neutral value - disconnected,
    // no players, no authority - and every command is a no-op returning false, so a
    // networked script still runs in a single-player scene without guards.
    bool IsNetworkActive() const;
    bool IsNetworkOffline() const;
    int GetNetworkState() const;
    int GetNetworkPing() const;
    std::string GetNetworkRegion() const;
    std::string GetNetworkBackendName() const;
    bool NetworkConnect();
    void NetworkDisconnect();

    bool IsInNetworkRoom() const;
    bool NetworkJoinOrCreateRoom(const std::string& name, int maxPlayers);
    bool NetworkJoinRandomRoom();
    bool NetworkLeaveRoom();

    int GetLocalNetworkPlayerId() const;
    int GetNetworkHostPlayerId() const;
    bool IsNetworkHost() const;
    int GetNetworkPlayerCount() const;
    int GetNetworkPlayerIdAt(int index) const;
    std::string GetNetworkPlayerNickname(int playerId) const;
    bool SetNetworkNickname(const std::string& nickname);

    // NetworkId is unsigned on the wire; 0 is kInvalidNetworkId and is what every
    // one of these returns when it fails, so a caller can test the result directly.
    unsigned int NetworkSpawn(const std::string& assetId,
                              const glm::vec3& position,
                              const glm::vec3& rotationDegrees,
                              const glm::vec3& scale);
    bool NetworkDespawn(unsigned int networkId);
    bool NetworkHasAuthority(unsigned int networkId) const;
    bool NetworkTransferOwnership(unsigned int networkId, int newOwner);
    int GetNetworkObjectOwner(unsigned int networkId) const;
    int GetNetworkObjectSceneId(unsigned int networkId) const;
    unsigned int FindNetworkIdForObject(int sceneObjectId) const;
    int GetNetworkObjectCount() const;
    unsigned int GetNetworkObjectIdAt(int index) const;

    // Avatar pose channel - the per-player stream a rig uses for parts that move
    // independently of the object root. Rotations are quaternions here because a
    // headset orientation does not survive a Euler round trip.
    void SetLocalAvatarPose(int slot, const glm::vec3& position, const glm::quat& rotation);
    void ClearLocalAvatarPose(int slot);
    bool GetAvatarPose(int playerId, int slot, glm::vec3& position, glm::quat& rotation) const;

    // Rotation conversion in the engine's own convention (Rx*Ry*Rz, degrees), which
    // is NOT glm's default quat(vec3) composition - the two agree only while a
    // single axis is non-zero. Exposed here rather than as free functions so a
    // compiled script picks up the engine's implementation instead of linking its
    // own, and so scripts converting XR poses cannot get the convention wrong.
    glm::vec3 QuatToEulerDegrees(const glm::quat& rotation) const;
    glm::quat EulerDegreesToQuat(const glm::vec3& degrees) const;
    bool IsXRHovered() const;
    bool IsXRSelected() const;
    bool XRHoverEntered() const;
    bool XRHoverExited() const;
    bool XRSelectEntered() const;
    bool XRSelectExited() const;
    bool XRActivated() const;
    bool XRDeactivated() const;
    int GetXRInteractorId() const;
    bool ResolveGround(float capsuleHalf, float probeExtra, float groundSnap, float verticalVelocity,
                       glm::vec3* outHitPos = nullptr, bool* outHitGround = nullptr,
                       glm::vec3* outHitNormal = nullptr, int* outHitActorId = nullptr,
                       glm::vec3* outHitActorVelocity = nullptr,
                       float* outHitStaticFriction = nullptr,
                       float* outHitDynamicFriction = nullptr) const;
    void ApplyVelocity(const glm::vec3& velocity, float deltaTime);
    struct StandaloneMovementSettings {
        glm::vec3 moveTuning = glm::vec3(4.5f, 7.5f, 6.5f);
        glm::vec3 lookTuning = glm::vec3(0.12f, 200.0f, 0.0f);
        glm::vec3 capsuleTuning = glm::vec3(1.8f, 0.4f, 0.2f);
        glm::vec3 gravityTuning = glm::vec3(-9.81f, 0.4f, 30.0f);
        glm::vec3 locomotionTuning = glm::vec3(24.0f, 8.0f, 16.0f);
        glm::vec3 surfaceTuning = glm::vec3(0.2f, 40.0f, 1.0f);
        bool enableMouseLook = true;
        bool requireMouseButton = false;
        bool enforceCollider = true;
        bool enforceRigidbody = true;
    };
    struct StandaloneMovementState {
        float pitch = 0.0f;
        float yaw = 0.0f;
        float verticalVelocity = 0.0f;
        glm::vec2 localVelocity = glm::vec2(0.0f);
        glm::vec3 slideVelocity = glm::vec3(0.0f);
        glm::vec3 lastGroundHitPos = glm::vec3(0.0f);
        bool hasGroundSample = false;
    };
    struct StandaloneMovementDebug {
        glm::vec3 velocity = glm::vec3(0.0f);
        glm::vec2 localVelocity = glm::vec2(0.0f);
        glm::vec3 platformVelocity = glm::vec3(0.0f);
        float surfaceFriction = 0.0f;
        float slopeDegrees = 0.0f;
        bool grounded = false;
    };
    void BindStandaloneMovementSettings(StandaloneMovementSettings& settings);
    void DrawStandaloneMovementInspector(StandaloneMovementSettings& settings, bool* showDebug = nullptr);
    void TickStandaloneMovement(StandaloneMovementState& state, StandaloneMovementSettings& settings,
                                float deltaTime, StandaloneMovementDebug* debug = nullptr);
    // UI helpers
    bool IsUIButtonPressed() const;
    bool IsUIHovered() const;
    bool IsUIActive() const;
    bool IsUIInteractable() const;
    void SetUIInteractable(bool interactable);
    float GetUISliderValue() const;
    void SetUISliderValue(float value);
    void SetUISliderRange(float minValue, float maxValue);
    void SetUILabel(const std::string& label);
    void SetUIColor(const glm::vec4& color);
    int GetSpriteClipCount() const;
    int GetSpriteClipIndex() const;
    std::string GetSpriteClipName() const;
    std::string GetSpriteClipNameAt(int index) const;
    bool SetSpriteClipIndex(int index);
    bool SetSpriteClipName(const std::string& name);
    bool SetObjectSprite(int objectId, const Sprite& sprite);
    bool SetSpriteTexture(const std::string& path);
    float GetSpriteAlpha() const;
    void SetSpriteAlpha(float alpha);
    bool FadeSpriteAlpha(float targetAlpha, float duration, float deltaTime);
    bool FadeSpriteToClipIndex(int clipIndex, float fadeOutDuration, float fadeInDuration, float deltaTime);
    bool FadeSpriteToClipName(const std::string& clipName,
                              float fadeOutDuration, float fadeInDuration, float deltaTime);
    float GetUITextScale() const;
    void SetUITextScale(float scale);
    void SetUISliderStyle(UISliderStyle style);
    void SetUIButtonStyle(UIButtonStyle style);
    void SetUIStylePreset(const std::string& name);
    void RegisterUIStylePreset(const std::string& name, const ImGuiStyle& style, bool replace = false);
    void SetFPSCap(bool enabled, float cap = 120.0f);
    bool HasRigidbody() const;
    bool HasRigidbody2D() const;
    bool EnsureCapsuleCollider(float height, float radius);
    bool EnsureRigidbody(bool useGravity = true, bool kinematic = false);
    bool SetRigidbody2DVelocity(const glm::vec2& velocity);
    bool GetRigidbody2DVelocity(glm::vec2& outVelocity) const;
    bool SetRigidbodyVelocity(const glm::vec3& velocity);
    bool GetRigidbodyVelocity(glm::vec3& outVelocity) const;
    bool GetRigidbodyPose(glm::vec3& outPosition, glm::vec3& outRotationDeg) const;
    bool SetContinuousCollisionDetection(bool enabled = true);
    bool AddRigidbodyVelocity(const glm::vec3& deltaVelocity);
    bool SetRigidbodyAngularVelocity(const glm::vec3& velocity);
    bool GetRigidbodyAngularVelocity(glm::vec3& outVelocity) const;
    bool AddRigidbodyForce(const glm::vec3& force);
    bool AddRigidbodyImpulse(const glm::vec3& impulse);
    bool AddRigidbodyTorque(const glm::vec3& torque);
    bool AddRigidbodyAngularImpulse(const glm::vec3& impulse);
    bool SetObjectRigidbodyVelocity(int objectId, const glm::vec3& velocity);
    bool GetObjectRigidbodyVelocity(int objectId, glm::vec3& outVelocity) const;
    bool GetObjectRigidbodyPose(int objectId, glm::vec3& outPosition,
                                glm::vec3& outRotationDeg) const;
    int GetContactCount() const;
    int GetObjectContactCount(int objectId) const;
    bool IsTouchingObject(int otherObjectId) const;
    bool AreObjectsTouching(int objectAId, int objectBId) const;
    bool GetContactNormal(glm::vec3& outNormal) const;
    bool GetObjectContactNormal(int objectId, glm::vec3& outNormal) const;
    bool SetObjectsCollisionIgnored(int objectAId, int objectBId, bool ignored = true);
    bool AddObjectRigidbodyImpulse(int objectId, const glm::vec3& impulse);
    bool TeleportObjectRigidbody(int objectId, const glm::vec3& pos, const glm::vec3& rotDeg);
    bool SetRigidbodyYaw(float yawDegrees);
    float GetProjectGravityScale() const;
    float FixedDeltaTime() const;
    void SetProjectGravityScale(float scale);
    bool RaycastClosest(const glm::vec3& origin, const glm::vec3& dir, float distance,
                        glm::vec3* hitPos = nullptr, glm::vec3* hitNormal = nullptr,
                        float* hitDistance = nullptr) const;
    bool RaycastClosestDetailed(const glm::vec3& origin, const glm::vec3& dir, float distance,
                                glm::vec3* hitPos = nullptr, glm::vec3* hitNormal = nullptr,
                                float* hitDistance = nullptr, int* hitObjectId = nullptr,
                                glm::vec3* hitObjectVelocity = nullptr,
                                float* hitStaticFriction = nullptr,
                                float* hitDynamicFriction = nullptr) const;
    bool SetRigidbodyRotation(const glm::vec3& rotDeg);
    bool TeleportRigidbody(const glm::vec3& pos, const glm::vec3& rotDeg);
    // Audio helpers
    bool HasAudioSource() const;
    bool PlayAudio();
    bool StopAudio();
    bool PlayObjectAudio(int objectId);
    bool StopObjectAudio(int objectId);
    bool SetObjectAudioLoop(int objectId, bool loop);
    bool PlayObjectAudioOneShot(int objectId, const std::string& clipPath, float volumeScale = 1.0f);
    bool SetAudioLoop(bool loop);
    bool SetAudioVolume(float volume);
    bool SetAudioClip(const std::string& path);
    bool PlayAudioOneShot(const std::string& clipPath = "", float volumeScale = 1.0f);
    // Animation helpers
    bool HasAnimation() const;
    bool PlayAnimation(bool restart = true);
    bool StopAnimation(bool resetTime = true);
    bool PauseAnimation(bool pause = true);
    bool ReverseAnimation(bool restartIfStopped = true);
    bool SetAnimationTime(float timeSeconds);
    float GetAnimationTime() const;
    bool IsAnimationPlaying() const;
    bool SetAnimationLoop(bool loop);
    bool SetAnimationPlaySpeed(float speed);
    bool SetAnimationPlayOnAwake(bool playOnAwake);
    // Settings helpers (auto-mark dirty)
    std::string GetSetting(const std::string& key, const std::string& fallback = "") const;
    void SetSetting(const std::string& key, const std::string& value);
    bool GetSettingBool(const std::string& key, bool fallback = false) const;
    void SetSettingBool(const std::string& key, bool value);
    float GetSettingFloat(const std::string& key, float fallback = 0.0f) const;
    void SetSettingFloat(const std::string& key, float value);
    glm::vec3 GetSettingVec3(const std::string& key, const glm::vec3& fallback = glm::vec3(0.0f)) const;
    void SetSettingVec3(const std::string& key, const glm::vec3& value);
    // Utility I/O helpers
    std::string HttpPost(const std::string& url, const std::string& contentType,
                         const std::string& body, const std::string& headers = "");
    int StartHttpPost(const std::string& url, const std::string& contentType,
                      const std::string& body, const std::string& headers = "",
                      bool stream = false);
    bool PollHttpPost(int requestId, std::string& outChunk, bool& outDone, bool& outSuccess);
    void CancelHttpPost(int requestId);
    int StartProcess(const std::string& command, const std::string& workingDir = "",
                     bool interactive = false);
    bool WriteProcessStdin(int processId, const std::string& data);
    bool PollProcess(int processId, std::string& outChunk, bool& outDone,
                     bool& outSuccess, int& outExitCode);
    void CancelProcess(int processId);
    std::string RunProcess(const std::string& command, const std::string& workingDir = "",
                           int timeoutMs = 15000, int* exitCode = nullptr);
    bool ReadProcessLine(int processId, std::string& outLine, int timeoutMs = 15000);
    std::string ReadFileText(const std::string& path) const;
    std::string ReadFileBase64(const std::string& path, size_t maxBytes = 16 * 1024 * 1024) const;
    bool WriteFileText(const std::string& path, const std::string& content);
    bool DeleteFile(const std::string& path);
    std::string ListFiles(const std::string& path, bool recursive = false, int maxEntries = 200) const;
    std::string SearchFiles(const std::string& root, const std::string& query, int maxResults = 50) const;
    std::string GetProgramRootPath() const;
    std::string GetPersistentDataPath(const std::string& subFolder = "") const;
    std::string GetEngineDocsRootPath() const;
    ImTextureID GetUIImageTexture(const std::string& path, int* outWidth = nullptr, int* outHeight = nullptr) const;
    bool SaveProject();
    std::string GetSelectedFilePath() const;
    std::string GetSelectedObjectInfo() const;
    std::string GetProjectName() const;
    std::string GetCurrentSceneName() const;
    std::string GetSceneHierarchy(int maxObjects = 0) const;
    int  CreateSceneObject(const std::string& type, const std::string& name, int parentId = -1);
    bool DeleteSceneObject(int objectId);
    bool RenameSceneObject(int objectId, const std::string& name);
    bool SetSceneObjectParent(int objectId, int parentId);
    bool SetSceneObjectTransform(int objectId, const glm::vec3& position,
                                 const glm::vec3& rotationDeg, const glm::vec3& scale);
    bool SetSceneObjectEnabled(int objectId, bool enabled);
    bool AddObjectComponent(int objectId, const std::string& component);
    bool AttachObjectScript(int objectId, const std::string& scriptPath);
    // Console helper
    void AddConsoleMessage(const std::string& message, ConsoleMessageType type = ConsoleMessageType::Info);
    // Auto-binding helpers: bind once per call, optionally load stored value.
    void AutoSetting(const std::string& key, bool& value);
    void AutoSetting(const std::string& key, float& value);
    void AutoSetting(const std::string& key, int& value);
    void AutoSetting(const std::string& key, glm::vec3& value);
    void AutoSetting(const std::string& key, char* buffer, size_t bufferSize);
    void AutoSetting(const std::string& key, std::string& value);
    void SaveAutoSettings();
    // IEnum helpers
    void StartIEnum(void(*fn)(ScriptContext&, float));
    void StopIEnum(void(*fn)(ScriptContext&, float));
    void EnsureIEnum(void(*fn)(ScriptContext&, float));
    bool IsIEnumRunning(void(*fn)(ScriptContext&, float)) const;
    void StopAllIEnums();
    void MarkDirty();
};

enum class ScriptLoadFailure {
    None,
    Unreadable,
    AbiMismatch,
    LayoutMismatch,
    NoExports
};

class ScriptRuntime {
public:
    using AbiVersionFn = int(*)();
    using LayoutSignatureFn = unsigned long long(*)();
    using BeginFn = void(*)(ScriptContext&, float);
    using SpecFn = void(*)(ScriptContext&, float);
    using TestEditorFn = void(*)(ScriptContext&, float);
    using UpdateFn = void(*)(ScriptContext&, float);
    using TickUpdateFn = void(*)(ScriptContext&, float);
    using InspectorFn = void(*)(ScriptContext&);
    using EditorRenderFn = void(*)(ScriptContext&);
    using EditorExitFn = void(*)(ScriptContext&);
    using IEnumFn = void(*)(ScriptContext&, float);
    using CollisionFn = void(*)(ScriptContext&, SceneObject*);
    using CollisionHoldDurationFn = float(*)();
    using ResetStateFn = void(*)();

    InspectorFn getInspector(const fs::path& binaryPath);
    void tickModule(const fs::path& binaryPath, ScriptContext& ctx, float deltaTime,
                    bool runSpec, bool runTest);
    void dispatchCollision(const fs::path& binaryPath, ScriptContext& ctx,
                           SceneObject* other, PhysicsCollisionPhase phase, float deltaTime);
    void unloadAll();
    const std::string& getLastError() const { return lastError; }
    ScriptLoadFailure getLastFailure() const { return lastFailure; }
    // Editor extension hooks: load RenderEditorWindow/ExitRenderEditorWindow from a script binary.
    bool hasEditorWindow(const fs::path& binaryPath);
    void callEditorWindow(const fs::path& binaryPath, ScriptContext& ctx);
    void callExitEditorWindow(const fs::path& binaryPath, ScriptContext& ctx);

    void setCompiledScriptsRoot(const fs::path& root) { compiledScriptsRoot_ = root; }

private:
    struct Module {
        void* handle = nullptr;
        fs::path loadedPath;
        fs::file_time_type binaryWriteTime{};
        uintmax_t binarySize = 0;
        bool loadedFromShadowCopy = false;
        InspectorFn inspector = nullptr;
        BeginFn begin = nullptr;
        SpecFn spec = nullptr;
        TestEditorFn testEditor = nullptr;
        UpdateFn update = nullptr;
        TickUpdateFn tickUpdate = nullptr;
        EditorRenderFn editorRender = nullptr;
        EditorExitFn editorExit = nullptr;
        ResetStateFn resetState = nullptr;
        CollisionFn collideEnter = nullptr;
        CollisionFn collideHold = nullptr;
        CollisionFn collideExit = nullptr;
        CollisionHoldDurationFn collideHoldDuration = nullptr;
        struct CollisionHoldState {
            float elapsed = 0.0f;
            bool fired = false;
        };
        std::unordered_map<uint64_t, CollisionHoldState> collisionHoldStates;
        std::unordered_set<int> beginCalledObjects;
    };
    Module* getModule(const fs::path& binaryPath);
    std::unordered_map<std::string, Module> loaded;
    std::string lastError;
    ScriptLoadFailure lastFailure = ScriptLoadFailure::None;
    fs::path compiledScriptsRoot_;
};

// Lightweight coroutine-style helpers (opt-in and no-ops unless used by scripts).
#ifndef IEnum
#define IEnum
#endif

#ifndef IEnum_Start
#define IEnum_Start(fn) ctx.StartIEnum(fn)
#endif
#ifndef IEnum_Stop
#define IEnum_Stop(fn) ctx.StopIEnum(fn)
#endif
#ifndef IEnum_Ensure
#define IEnum_Ensure(fn) ctx.EnsureIEnum(fn)
#endif

#undef MODULARITY_SCRIPT_API
