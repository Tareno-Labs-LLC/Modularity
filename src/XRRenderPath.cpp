
#include "Engine.h"

#include "XR/XRDiagnostics.h"
#include "XR/XRInput.h"
#include "XR/XRRuntimeSelection.h"
#include "XR/XRSystem.h"

#ifdef __ANDROID__
#include "AndroidRuntime/AndroidRuntime.h"
#endif

#include <string>

namespace {

constexpr float kDefaultXRNearPlane = 0.05f;
constexpr float kDefaultXRFarPlane = 1000.0f;

} // namespace

bool Engine::isXRRequestedByProject() const {
#if !MODULARITY_RUNTIME_ONLY
    if (!playerMode && vrEditor.preview) return false;
#endif
#if !MODULARITY_RUNTIME_ONLY && MODULARITY_HAS_OPENXR
    if (!playerMode && vrEditor.requested && !usingVulkan()) return true;
#endif
    if (!projectManager.currentProject.isLoaded) return false;
    const Modularity::XR::ProjectOpenXRSettings& xr =
        projectManager.currentProject.openXRSettings;
    if (!xr.isEnabledForActiveTarget()) return false;
#if !MODULARITY_HAS_OPENXR
    return false;
#else
    if (graphicsBackend == Modularity::GraphicsBackend::Vulkan) return false;

#if !MODULARITY_RUNTIME_ONLY
    if (!playerMode) {
        if (!isPlaying) return false;
        if (Modularity::XR::ActivePlatformTarget() == Modularity::XR::XRPlatformTarget::PC &&
            !xr.pc.startOnPlay) {
            return false;
        }
    }
#endif
    return true;
#endif
}

void Engine::updateXRSystem() {
    xrFrameSubmitted = false;

    if (!isXRRequestedByProject()) {
        if (xrSystem.isCreated()) shutdownXRSystem();
        xrStartupAttempted = false;
        return;
    }

    if (!xrSystem.isCreated()) {
        if (xrStartupAttempted) return;
        xrStartupAttempted = true;

#if !MODULARITY_RUNTIME_ONLY
        // The launcher has not opened a scene and normally has no renderer yet.
        // A VR session still needs rendered frames to leave the compositor's
        // application-waiting screen and show the spatial project selector.
        if (vrEditor.requested && !playerMode && !initRenderer()) {
            xrStartupError = "VR editor could not initialize the OpenGL renderer. See Console for the missing resource or shader error.";
            return;
        }
#endif
        Modularity::XR::ProjectOpenXRSettings xr = projectManager.currentProject.openXRSettings;
#if !MODULARITY_RUNTIME_ONLY
        if (vrEditor.requested && !playerMode) {
            xr.enabled = true;
            xr.interaction.editorWorkspace = true;
            xr.activeTarget().enabled = true;
            xr.activeTarget().renderMode = Modularity::XR::XRRenderMode::MultiPass;
            xr.activeTarget().interactionProfiles = Modularity::XR::DefaultInteractionProfiles(
                Modularity::XR::ActivePlatformTarget());
        } else
#endif
        {
            // Start in the space the scene's XR Origin asks for, so the first frame is
            // already right; updateXRComponents keeps it in step after that.
            xr.trackingOrigin = resolveXRTrackingOrigin();
        }

        if (Modularity::XR::ActivePlatformTarget() == Modularity::XR::XRPlatformTarget::PC) {
            std::string overrideError;
            if (!Modularity::XR::ApplyRuntimeOverride(xr.pc.runtimeManifestPath, overrideError) &&
                !overrideError.empty()) {
                addConsoleMessage("OpenXR runtime selection: " + overrideError,
                                  ConsoleMessageType::Warning);
            }
            Modularity::XR::MutableDiagnostics().runtimeOverridePath =
                Modularity::XR::ActiveRuntimeOverride();
        }

        Modularity::XR::XRSystem::PlatformHandles platform;
#ifdef __ANDROID__
        platform.applicationVM = Modularity::AndroidRuntime::GetJavaVM();
        platform.applicationActivity = Modularity::AndroidRuntime::GetActivityObject();
        platform.graphics.gles.eglDisplay = Modularity::AndroidRuntime::GetEglDisplay();
        platform.graphics.gles.eglConfig = Modularity::AndroidRuntime::GetEglConfig();
        platform.graphics.gles.eglContext = Modularity::AndroidRuntime::GetEglContext();
#else
        platform.graphics.desktop.window = editorWindow;
#endif

        xrStartupError.clear();
        if (!xrSystem.startup(xr, platform, xrStartupError)) {
            addConsoleMessage("OpenXR unavailable: " + xrStartupError,
                              ConsoleMessageType::Warning);
            return;
        }
        addConsoleMessage("OpenXR session started (" + xrSystem.runtime().runtimeName() + ")",
                          ConsoleMessageType::Success);
        applyPresentationSettings();
        return;
    }

    if (!xrSystem.update()) {
        addConsoleMessage("OpenXR session ended.", ConsoleMessageType::Info);
        shutdownXRSystem();
    }
}

bool Engine::renderXRFrame() {
#if !MODULARITY_HAS_OPENXR
    return false;
#else
    if (!xrSystem.isRunning() || !rendererInitialized) return false;

#if MODULARITY_XR_HAS_ANY_GL_TYPES
    // The desktop monitor renders after XR. Never leave a released eye image
    // bound as its target, including when an XR frame cannot submit any views.
    struct FrameTargetRestore {
        GLint draw = 0, read = 0, viewport[4]{};
        FrameTargetRestore() {
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
            glGetIntegerv(GL_VIEWPORT, viewport);
        }
        ~FrameTargetRestore() {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        }
    } restoreTarget;
#endif

    Camera xrCamera = camera;
    float nearPlane = kDefaultXRNearPlane;
    float farPlane = kDefaultXRFarPlane;
    const SceneObject* runtimeCam = nullptr;
#if !MODULARITY_RUNTIME_ONLY
    const bool editingWorkspace = vrEditor.requested && !playerMode && !isPlaying;
    if (editingWorkspace) {
        nearPlane = std::max(0.01f, buildSettings.editorCameraNear);
        farPlane = std::max(nearPlane + 0.01f, buildSettings.editorCameraFar);
    } else
#endif
    {
        runtimeCam = findXRCameraObject();
        if (!runtimeCam) runtimeCam = findPlayerCameraObject();
        // updateXRComponents set this before scripts and physics moved the rig. Rebuild it
        // from where the origin ended up, so the eyes land where the XR Camera object (and
        // the audio listener on it) already are instead of one physics step behind.
        xrSystem.setOriginToWorld(computeXROriginToWorld(findXROriginObject()));
    }
    if (runtimeCam) {
        xrCamera = makeCameraFromObject(*runtimeCam);
        nearPlane = std::max(0.01f, runtimeCam->camera.nearClip);
        farPlane = std::max(nearPlane + 0.01f, runtimeCam->camera.farClip);
    }
    xrSystem.setClipPlanes(nearPlane, farPlane);

    const bool render = xrSystem.beginFrame();
#if !MODULARITY_RUNTIME_ONLY
    if (vrEditor.requested && !playerMode) renderVREditorUI(Modularity::XR::InputState());
    if (editingWorkspace) xrCamera = camera;
#endif
    if (render) {
        const uint32_t viewCount = xrSystem.viewCount();
        for (uint32_t viewIndex = 0; viewIndex < viewCount; ++viewIndex) {
            Modularity::XR::XRRenderView view;
            if (!xrSystem.beginView(viewIndex, view)) continue;

            const glm::mat4 viewMatrix = xrSystem.viewMatrix(viewIndex);
            const glm::mat4 projection = xrSystem.projectionMatrix(viewIndex);

            const glm::mat4 eyeToWorld = glm::inverse(viewMatrix);
            xrCamera.position = glm::vec3(eyeToWorld[3]);
            xrCamera.front = glm::normalize(-glm::vec3(eyeToWorld[2]));
            xrCamera.up = glm::normalize(glm::vec3(eyeToWorld[1]));

            renderer.beginRender(viewMatrix, projection, xrCamera.position);
            renderer.renderSceneToTarget(xrCamera, sceneObjects, view.framebuffer,
                                         static_cast<int>(view.width),
                                         static_cast<int>(view.height), projection, nearPlane,
                                         farPlane, true, true);
            renderer.endRender();

#if !MODULARITY_RUNTIME_ONLY
            if (editingWorkspace) vrEditor.drawSceneOverlays(renderer,xrCamera,sceneObjects,projection,
                view.framebuffer,static_cast<int>(view.width),static_cast<int>(view.height),nearPlane);
#endif
#if !MODULARITY_RUNTIME_ONLY
            if (vrEditor.requested && !playerMode) {
                glBindFramebuffer(GL_FRAMEBUFFER, view.framebuffer);
                glViewport(0, 0, static_cast<int>(view.width), static_cast<int>(view.height));
                const glm::mat4 trackingToWorld = xrSystem.originToWorld();
                const glm::vec3 trackingEye = glm::vec3(glm::inverse(trackingToWorld) * glm::vec4(xrCamera.position, 1));
                vrEditor.renderEye(renderer, projection * viewMatrix * trackingToWorld, trackingEye);
                if (viewIndex == 0) {
                    const bool firstMirror = !vrEditor.mirrorTexture();
                    vrEditor.captureMirror(view.framebuffer, view.width, view.height);
                    if (firstMirror && vrEditor.mirrorTexture())
                        addConsoleMessage("VR editor submitted its first view; desktop mirror ready.", ConsoleMessageType::Info);
                }
            }
#endif

            xrSystem.endView(viewIndex);
        }
    }

    xrSystem.endFrame();
#if !MODULARITY_RUNTIME_ONLY
    if (vrEditor.requested && !playerMode) applyVREditorActions();
#endif
    return render;
#endif
}

void Engine::shutdownXRSystem() {
    if (!xrSystem.isCreated()) {
        Modularity::XR::ResetInputState();
        return;
    }
    xrSystem.shutdown();
    xrFrameSubmitted = false;
    Modularity::XR::ResetInputState();
    applyPresentationSettings();
}
