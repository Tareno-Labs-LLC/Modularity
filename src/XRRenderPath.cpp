
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

        const Modularity::XR::ProjectOpenXRSettings& xr =
            projectManager.currentProject.openXRSettings;

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

    Camera xrCamera = camera;
    float nearPlane = kDefaultXRNearPlane;
    float farPlane = kDefaultXRFarPlane;
    const SceneObject* runtimeCam = findXRCameraObject();
    if (!runtimeCam) runtimeCam = findPlayerCameraObject();
    if (runtimeCam) {
        xrCamera = makeCameraFromObject(*runtimeCam);
        nearPlane = std::max(0.01f, runtimeCam->camera.nearClip);
        farPlane = std::max(nearPlane + 0.01f, runtimeCam->camera.farClip);
    }
    xrSystem.setClipPlanes(nearPlane, farPlane);

    const bool render = xrSystem.beginFrame();
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
                                         farPlane);
            renderer.endRender();

            xrSystem.endView(viewIndex);
        }
    }

    xrSystem.endFrame();
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
