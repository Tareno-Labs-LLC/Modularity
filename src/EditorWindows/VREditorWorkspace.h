#pragma once

#if !MODULARITY_RUNTIME_ONLY
#include "XR/XRInput.h"
#include "Rendering.h"
#include "ThirdParty/ModuGuizmo/ImGuizmo.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <unordered_map>

struct ImGuiContext;
struct ImGuiWindow;

// A separate ModuGUI workspace. Desktop docking/ini state never enters this context.
class VREditorWorkspace {
public:
    VREditorWorkspace() = default;
    ~VREditorWorkspace();
    bool requested = false;
    bool preview = false;
    bool resetLayout = false;
    bool recenterPending = true;
    bool keyboardVisible = false;
    bool initializedLayout = false;
    bool previousPlaying = false;
    bool wristToolsVisible = false;
    bool navigationInitialized = false;
    bool viewportPanelVisible = false;
    float grabForce = .36f;
    bool smoothHands = true;
    std::string pendingLayoutLoad;
    bool previewPointerActive = false;
    bool previewTrigger = false;
    bool previewGrip = false;
    glm::vec3 previewDirection{0,0,-1};
    int pendingAction = 0; // 1 play/stop, 2 pause, 3 desktop, 4 project selector, 5 spec
    std::string pendingProject;
    // Locates the moduengine.vr-controllers ModuPAK (controller models and their
    // .controller material files). Engine wires it to the PackageManager; it is
    // polled every few seconds so installing the package mid-session just works.
    std::function<std::filesystem::path()> findControllerPackage;
    glm::mat4 sceneFromTracking{1.0f};
    void updateNavigation(const Modularity::XR::XRInputState& state, float dt);

    void beginUI(const Modularity::XR::XRInputState& input, float dt, bool launcher);
    void endUI();
    void capturePanels();
    void renderEye(Renderer& renderer, const glm::mat4& trackingViewProjection,
                   const glm::vec3& trackingEye);
    void shutdown();
    void save();
    bool saveLayout(const std::string& name);
    bool loadLayout(const std::string& name);
    std::vector<std::string> layoutNames() const;
    void setStorage(const std::filesystem::path& path);
    void recenter(const Modularity::XR::XRInputState& input);
    bool returnGesture(const Modularity::XR::XRInputState& input, float dt, bool playing);
    int takeToolStep() { const int step=toolStep;toolStep=0;return step; }
    bool takeScenePick(glm::vec3& origin, glm::vec3& direction);
    void renderPreviewImage(ImVec2 size);
    Modularity::XR::XRInputState previewInputState() const;
    void renderKeyboard();
    void renderPreview(Renderer& renderer, const Camera* camera = nullptr,
                       const std::vector<SceneObject>* scene = nullptr,
                       float nearPlane = .03f, float farPlane = 1000.f);
    void captureMirror(unsigned int source, int width, int height);
    void updateSceneHover(const std::vector<SceneObject>& scene, const glm::mat4& view, float dt);
    bool manipulateGizmo(glm::mat4& model, const glm::mat4& view, ImGuizmo::OPERATION operation,
                         ImGuizmo::MODE mode, const float* snap, const float* bounds);
    void clearGizmo();
    void drawSceneOverlays(Renderer& renderer, const Camera& camera, const std::vector<SceneObject>& scene,
                           const glm::mat4& projection, unsigned int target, int width, int height, float nearPlane);
    std::vector<int> selection;
    bool gizmoUsing() const { return gizmoWasUsing; }

    unsigned int mirrorTexture() const { return mirror.texture; }
    size_t panelCount() const { return panels.size(); }
    ImGuiContext* context() const { return ui; }

    struct Surface {
        glm::vec3 position{0.0f};
        glm::quat rotation{1,0,0,0};
        glm::vec2 size{1.0f};
    };
    static bool intersect(const Surface& surface, glm::vec3 origin, glm::vec3 direction,
                          float& distance, glm::vec2& uv, bool bounded = true);

private:
    struct Target {
        unsigned int fbo = 0, texture = 0, depth = 0;
        int width = 0, height = 0;
    };
    struct Panel {
        unsigned int id = 0;
        std::string name;
        Surface surface;
        ImVec2 uiPosition{0,0}, uiSize{600,500};
        Target target;
        bool visible = false;
        bool popup = false;
        bool wrist = false;
    };
    std::vector<Panel> panels;
    std::vector<Panel*> visibleScratch;
    std::vector<std::pair<ImDrawList*, ImGuiWindow*>> ownerScratch;
    ImDrawData drawSlice;
    std::unordered_map<unsigned int, Surface> savedSurfaces;
    ImGuiContext* ui = nullptr;
    ImGuiContext* desktop = nullptr;
    Modularity::XR::XRInputState input;
    struct HandFilter {
        Modularity::XR::XRPose aim,grip,clickAnchor,gripAnchor;
        bool valid=false,clickHeld=false,gripHeld=false,clickLocked=false,gripLocked=false;
    };
    std::array<HandFilter,2> handFilters{};
    std::array<bool,2> editorGrips{};
    bool restoreLayoutPending=false;
    bool savedAnchorValid=false;
    glm::vec3 savedAnchor{0};
    float savedAnchorYaw=0;
    bool rightDown=false,padTouched=false;
    glm::vec2 padPosition{0};
    std::filesystem::path storage;
    std::string iniPath;
    Target mirror;
    unsigned int program = 0, vao = 0, vbo = 0;
    std::unique_ptr<Shader> controllerShader;
    std::filesystem::path controllerPackageRoot;
    std::chrono::steady_clock::time_point nextControllerPackageLookup{};
    int pointerHand = 1;
    int capturedHand = -1;
    unsigned int hoveredPanel = 0, draggedPanel = 0, mousePanel = 0;
    glm::mat4 gripFromPanel{1.0f};
    Surface previewDragPlane;
    glm::vec3 previewDragAnchor{0};
    glm::vec3 scenePickOrigin{0}, scenePickDirection{0,0,-1};
    bool scenePickPending = false;
    glm::vec3 hitPoint{0};
    glm::vec4 rayColor{1,1,1,1};
    int hoverObject = -1, hoverVisual = -1;
    float hoverOpacity = 0, hoverAge = 0;
    bool disabledItem = false, gizmoVisible = false, gizmoWasUsing = false, gizmoOver = false;
    glm::mat4 gizmoModel{1};
    ImGuizmo::OPERATION gizmoOperation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;
    std::array<float,6> gizmoBounds{};
    bool gizmoHasBounds = false;
    bool pointerRay(glm::vec3& origin, glm::vec3& direction) const;

    bool hit = false, mouseDown = false, gripDown = false;
    bool keyboardPress = false;
    float gestureTime = 0.0f;
    bool gestureLatched = false;
    float saveClock = 0.0f;
    bool shifted = false;
    bool toolStickLatched = false;
    int toolStep = 0;
    float anchorYaw = 0;
    glm::vec3 anchor{0};

    Panel* findPanel(unsigned int id);
    Surface defaultSurface(const ImGuiWindow* root) const;
    void feedPointer(float dt);
    void stabilizeInput(float dt);
    void updateWristTools();
    void updateWristSurfaces();
    void ensureTarget(Target& target, int width, int height, bool depth = false);
    void releaseTarget(Target& target);
    void ensureGraphics();
    void drawQuad(const glm::mat4& mvp, unsigned int texture, const glm::vec4& tint);
    void drawControllers(Renderer& renderer, const glm::mat4& vp, glm::vec3 eye);
};
#endif
