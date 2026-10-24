
#include "../Engine.h"

#include "XRInput.h"
#include "XRSystem.h"

#include <algorithm>
#include <cmath>

namespace {

using Modularity::XR::XRDevice;
using Modularity::XR::XRPoseKind;

XRDevice ToXRDevice(XRHand hand) {
    return (hand == XRHand::Right) ? XRDevice::Right : XRDevice::Left;
}

XRPoseKind ToXRPoseKind(XRControllerPoseSource source) {
    return (source == XRControllerPoseSource::Aim) ? XRPoseKind::Aim : XRPoseKind::Grip;
}

void ApplyPoseToLocalTransform(SceneObject& obj, const Modularity::XR::XRPose& pose,
                               bool trackPosition, bool trackRotation, float scale) {
    if (trackPosition && pose.positionValid) {
        obj.localPosition = pose.position * scale;
    }
    if (trackRotation && pose.orientationValid) {
        obj.localRotation = NormalizeEulerDegrees(QuatToEulerXYZDegrees(pose.rotation));
    }
    obj.localInitialized = true;
}

float DistanceSquared(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = a - b;
    return glm::dot(d, d);
}

} // namespace

const SceneObject* Engine::findXROriginObject() const {
    const SceneObject* found = nullptr;
    int extras = 0;
    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasXROrigin || !obj.xrOrigin.enabled) continue;
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        if (!found) {
            found = &obj;
        } else {
            ++extras;
        }
    }
    if (extras > 0) {
        static int warnedForCount = -1;
        if (warnedForCount != extras) {
            warnedForCount = extras;
            std::fprintf(stderr,
                         "[OpenXR Warning] Scene has %d enabled XR Origins; using the first "
                         "and ignoring the rest.\n",
                         extras + 1);
        }
    }
    return found;
}

const SceneObject* Engine::findXRCameraObject() const {
    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasXRCamera || !obj.xrCamera.enabled) continue;
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        if (!obj.hasCamera) continue;
        return &obj;
    }
    return nullptr;
}

void Engine::updateXRComponents(float deltaTime) {
#if !MODULARITY_HAS_OPENXR
    (void)deltaTime;
#else
    if (!isXRRequestedByProject()) return;
#if !MODULARITY_RUNTIME_ONLY
    // Editing must never write tracked poses into the saved scene rig.
    if (vrEditor.requested && !playerMode && !isPlaying) {
        xrSystem.setOriginToWorld(vrEditor.sceneFromTracking);
        return;
    }
#endif

    const Modularity::XR::XRInputState& input = Modularity::XR::InputState();

    glm::mat4 originToWorld(1.0f);
    float rigScale = 1.0f;
    float cameraYOffset = 0.0f;
    if (const SceneObject* origin = findXROriginObject()) {
        rigScale = std::max(0.01f, origin->xrOrigin.rigScale);
        cameraYOffset = origin->xrOrigin.cameraYOffset;
        originToWorld = glm::translate(glm::mat4(1.0f), origin->position) *
                        glm::mat4_cast(glm::quat(glm::radians(origin->rotation))) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(rigScale));
    }
    xrSystem.setOriginToWorld(originToWorld);

    if (!input.sessionActive) {
        return;
    }

    for (SceneObject& obj : sceneObjects) {
        if (!obj.hasXRCamera || !obj.xrCamera.enabled) continue;
        if (!obj.xrCamera.applyTracking) continue;
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        ApplyPoseToLocalTransform(obj, input.headPose, obj.xrCamera.trackPosition,
                                  obj.xrCamera.trackRotation, rigScale);
        if (obj.xrCamera.trackPosition) {
            obj.localPosition.y += cameraYOffset;
        }
    }

    for (SceneObject& obj : sceneObjects) {
        if (!obj.hasXRController || !obj.xrController.enabled) continue;
        const XRControllerComponent& controller = obj.xrController;
        const Modularity::XR::XRControllerState& state =
            input.controller(ToXRDevice(controller.hand));

        if (controller.hideWhenNotTracked) {
            const bool tracked = state.active && state.gripPose.isTracked();
            if (obj.enabled != tracked) obj.enabled = tracked;
            if (!tracked) continue;
        }
        if (!IsObjectEnabledInHierarchy(obj)) continue;

        ApplyPoseToLocalTransform(obj, state.pose(ToXRPoseKind(controller.poseSource)),
                                  controller.trackPosition, controller.trackRotation, rigScale);
    }

    for (SceneObject& obj : sceneObjects) {
        if (!obj.hasXRActionBasedController || !obj.xrActionBasedController.enabled) continue;
        XRActionBasedControllerComponent& c = obj.xrActionBasedController;
        if (!IsObjectEnabledInHierarchy(obj)) {
            c.selectHeld = c.selectStarted = c.selectEnded = false;
            c.activateHeld = c.activateStarted = c.activateEnded = false;
            c.uiPressHeld = false;
            continue;
        }

        const Modularity::XR::XRControllerState& state = input.controller(ToXRDevice(c.hand));
        const auto readButton = [&state](int button) {
            if (button < 0 || button >= static_cast<int>(Modularity::XR::XRButton::Count)) {
                return std::pair<bool, bool>(false, false);
            }
            const size_t index = static_cast<size_t>(button);
            return std::pair<bool, bool>(state.buttons[index], state.previousButtons[index]);
        };

        const auto [selectNow, selectPrev] = readButton(c.selectButton);
        c.selectHeld = selectNow;
        c.selectStarted = selectNow && !selectPrev;
        c.selectEnded = !selectNow && selectPrev;

        const auto [activateNow, activatePrev] = readButton(c.activateButton);
        c.activateHeld = activateNow;
        c.activateStarted = activateNow && !activatePrev;
        c.activateEnded = !activateNow && activatePrev;

        c.uiPressHeld = readButton(c.uiPressButton).first;
    }

    // Tracking writes local poses. Publish their world poses before interaction
    // and scripts read them, rather than leaving them one tracking sample behind.
    updateHierarchyWorldTransforms();
    updateXRInteraction(deltaTime);
#endif
}

void Engine::updateXRInteraction(float deltaTime) {
#if !MODULARITY_HAS_OPENXR
    (void)deltaTime;
#else
    if (!projectManager.currentProject.openXRSettings.interaction.enabled) return;

    xrInteractableScratch.clear();
    for (SceneObject& obj : sceneObjects) {
        if (!obj.hasXRGrabInteractable) continue;
        XRGrabInteractableComponent& grab = obj.xrGrabInteractable;
        grab.hoverEnteredThisFrame = false;
        grab.hoverExitedThisFrame = false;
        grab.selectEnteredThisFrame = false;
        grab.selectExitedThisFrame = false;
        grab.activatedThisFrame = false;
        grab.deactivatedThisFrame = false;
        if (!grab.enabled || !IsObjectEnabledInHierarchy(obj)) continue;
        xrInteractableScratch.push_back(obj.id);
    }

    const auto layerAllowed = [](uint32_t mask, int layer) {
        if (layer < 0 || layer > 31) return true;
        return (mask & (1u << static_cast<uint32_t>(layer))) != 0;
    };

    const auto findController = [this](const SceneObject& interactor) -> XRActionBasedControllerComponent* {
        if (interactor.hasXRActionBasedController) {
            return &const_cast<SceneObject&>(interactor).xrActionBasedController;
        }
        int parentId = interactor.parentId;
        for (int depth = 0; depth < 8 && parentId >= 0; ++depth) {
            SceneObject* parent = findObjectById(parentId);
            if (!parent) break;
            if (parent->hasXRActionBasedController) return &parent->xrActionBasedController;
            parentId = parent->parentId;
        }
        return nullptr;
    };

    for (SceneObject& interactorObj : sceneObjects) {
        const bool isRay = interactorObj.hasXRRayInteractor && interactorObj.xrRayInteractor.enabled;
        const bool isDirect =
            interactorObj.hasXRDirectInteractor && interactorObj.xrDirectInteractor.enabled;
        if (!isRay && !isDirect) continue;
        if (!IsObjectEnabledInHierarchy(interactorObj)) continue;

        XRActionBasedControllerComponent* controller = findController(interactorObj);

        int& hoveredId = isRay ? interactorObj.xrRayInteractor.hoveredObjectId
                               : interactorObj.xrDirectInteractor.hoveredObjectId;
        int& selectedId = isRay ? interactorObj.xrRayInteractor.selectedObjectId
                                : interactorObj.xrDirectInteractor.selectedObjectId;
        const uint32_t mask = isRay ? interactorObj.xrRayInteractor.interactionMask
                                    : interactorObj.xrDirectInteractor.interactionMask;

        int newHover = -1;
        if (isRay) {
            XRRayInteractorComponent& ray = interactorObj.xrRayInteractor;
            ray.hasHit = false;
            const glm::vec3 origin = interactorObj.position;
            const glm::quat rotation = glm::quat(glm::radians(interactorObj.rotation));
            const glm::vec3 direction = glm::normalize(rotation * glm::vec3(0.0f, 0.0f, -1.0f));

            float bestDistance = ray.maxDistance;
            for (int candidateId : xrInteractableScratch) {
                SceneObject* candidate = findObjectById(candidateId);
                if (!candidate || !layerAllowed(mask, candidate->layer)) continue;
                const float largestAxis = std::max(candidate->scale.x,
                                                   std::max(candidate->scale.y, candidate->scale.z));
                const float radius = std::max(0.05f, largestAxis * 0.5f);
                const glm::vec3 toTarget = candidate->position - origin;
                const float along = glm::dot(toTarget, direction);
                if (along < 0.0f || along > bestDistance) continue;
                const glm::vec3 closest = origin + direction * along;
                if (DistanceSquared(closest, candidate->position) > radius * radius) continue;
                bestDistance = along;
                newHover = candidateId;
                ray.hitPoint = closest;
                ray.hasHit = true;
            }
        } else {
            const XRDirectInteractorComponent& direct = interactorObj.xrDirectInteractor;
            const float radius = direct.interactionRadius;
            float bestDistanceSq = radius * radius;
            for (int candidateId : xrInteractableScratch) {
                SceneObject* candidate = findObjectById(candidateId);
                if (!candidate || !layerAllowed(mask, candidate->layer)) continue;
                const float distanceSq = DistanceSquared(candidate->position, interactorObj.position);
                if (distanceSq > bestDistanceSq) continue;
                bestDistanceSq = distanceSq;
                newHover = candidateId;
            }
        }

        if (newHover != hoveredId) {
            if (SceneObject* previous = findObjectById(hoveredId)) {
                if (previous->hasXRGrabInteractable) {
                    previous->xrGrabInteractable.hoverExitedThisFrame = true;
                    previous->xrGrabInteractable.hoveredByObjectId = -1;
                }
            }
            if (SceneObject* current = findObjectById(newHover)) {
                if (current->hasXRGrabInteractable) {
                    current->xrGrabInteractable.hoverEnteredThisFrame = true;
                    current->xrGrabInteractable.hoveredByObjectId = interactorObj.id;
                }
            }
            hoveredId = newHover;
        }

        const bool selectStarted = controller && controller->selectStarted;
        const bool selectEnded = controller && controller->selectEnded;

        if (selectStarted && selectedId < 0 && hoveredId >= 0) {
            if (SceneObject* target = findObjectById(hoveredId)) {
                if (beginXRGrab(*target, interactorObj, controller)) {
                    selectedId = hoveredId;
                    target->xrGrabInteractable.selectEnteredThisFrame = true;
                }
            }
        } else if (selectEnded && selectedId >= 0) {
            if (SceneObject* target = findObjectById(selectedId)) {
                endXRGrab(*target, interactorObj);
                target->xrGrabInteractable.selectExitedThisFrame = true;
            }
            selectedId = -1;
        }

        if (controller && selectedId >= 0) {
            if (SceneObject* target = findObjectById(selectedId)) {
                if (controller->activateStarted) {
                    target->xrGrabInteractable.activatedThisFrame = true;
                } else if (controller->activateEnded) {
                    target->xrGrabInteractable.deactivatedThisFrame = true;
                }
            }
        }

        if (selectedId >= 0) {
            if (SceneObject* target = findObjectById(selectedId)) {
                updateXRGrabFollow(*target, interactorObj, deltaTime);
            } else {
                selectedId = -1;
            }
        }
    }
#endif
}

bool Engine::beginXRGrab(SceneObject& interactable, SceneObject& interactor,
                         XRActionBasedControllerComponent* controller) {
    XRGrabInteractableComponent& grab = interactable.xrGrabInteractable;
    if (grab.heldByObjectId >= 0) return false;

    const bool isRight = controller && controller->hand == XRHand::Right;
    if (isRight && !grab.allowRightHand) return false;
    if (!isRight && !grab.allowLeftHand) return false;

    grab.heldByObjectId = interactor.id;

    if (interactable.hasRigidbody) {
        grab.wasKinematic = interactable.rigidbody.isKinematic;
        grab.hadGravity = interactable.rigidbody.useGravity;
        grab.stateSaved = true;
        if (grab.movementType != XRGrabInteractableComponent::MovementType::VelocityTracking) {
            interactable.rigidbody.isKinematic = true;
            interactable.rigidbody.useGravity = false;
        } else {
            interactable.rigidbody.useGravity = false;
        }
    }

    if (controller && controller->enableHaptics) {
        Modularity::XR::XRHapticRequest pulse;
        pulse.device = (controller->hand == XRHand::Right) ? Modularity::XR::XRDevice::Right
                                                           : Modularity::XR::XRDevice::Left;
        pulse.amplitude = controller->hapticAmplitude;
        pulse.duration = controller->hapticDuration;
        Modularity::XR::RequestHapticPulse(pulse);
    }
    return true;
}

void Engine::updateXRGrabFollow(SceneObject& interactable, SceneObject& interactor,
                                float deltaTime) {
    const XRGrabInteractableComponent& grab = interactable.xrGrabInteractable;

    glm::vec3 targetPosition = interactor.position;
    glm::quat targetRotation = glm::quat(glm::radians(interactor.rotation));
    if (grab.attachTransformId >= 0) {
        if (const SceneObject* attach = findObjectById(grab.attachTransformId)) {
            const glm::vec3 offset = attach->position - interactable.position;
            targetPosition -= targetRotation * offset;
        }
    }

    switch (grab.movementType) {
        case XRGrabInteractableComponent::MovementType::VelocityTracking: {
            if (interactable.hasRigidbody && deltaTime > 1e-5f) {
                const glm::vec3 delta = targetPosition - interactable.position;
                const glm::vec3 velocity = delta / deltaTime;
                setRigidbodyVelocityFromScript(interactable.id, velocity);
                if (grab.trackRotation) {
                    interactable.rotation =
                        NormalizeEulerDegrees(QuatToEulerXYZDegrees(targetRotation));
                    syncLocalTransform(interactable);
                }
                break;
            }
            [[fallthrough]];
        }
        case XRGrabInteractableComponent::MovementType::Instant:
        case XRGrabInteractableComponent::MovementType::Kinematic:
        default: {
            if (grab.trackPosition) interactable.position = targetPosition;
            if (grab.trackRotation) {
                interactable.rotation = NormalizeEulerDegrees(QuatToEulerXYZDegrees(targetRotation));
            }
            syncLocalTransform(interactable);
            if (interactable.hasRigidbody) {
                teleportPhysicsActorFromScript(interactable.id, interactable.position,
                                               interactable.rotation);
            }
            break;
        }
    }
}

void Engine::endXRGrab(SceneObject& interactable, SceneObject& interactor) {
    XRGrabInteractableComponent& grab = interactable.xrGrabInteractable;
    grab.heldByObjectId = -1;

    if (interactable.hasRigidbody && grab.stateSaved) {
        interactable.rigidbody.isKinematic = grab.wasKinematic;
        interactable.rigidbody.useGravity = grab.hadGravity;
        grab.stateSaved = false;
    }

    if (!grab.throwOnDetach || !interactable.hasRigidbody) return;

#if MODULARITY_HAS_OPENXR
    const Modularity::XR::XRInputState& input = Modularity::XR::InputState();
    XRHand hand = XRHand::Right;
    if (interactor.hasXRActionBasedController) {
        hand = interactor.xrActionBasedController.hand;
    } else if (interactor.hasXRController) {
        hand = interactor.xrController.hand;
    }
    const Modularity::XR::XRControllerState& state =
        input.controller((hand == XRHand::Right) ? Modularity::XR::XRDevice::Right
                                                 : Modularity::XR::XRDevice::Left);
    if (state.hasVelocity) {
        setRigidbodyVelocityFromScript(interactable.id,
                                       state.velocity * grab.throwVelocityScale);
        setRigidbodyAngularVelocityFromScript(
            interactable.id, state.angularVelocity * grab.throwAngularVelocityScale);
    }
#else
    (void)interactor;
#endif
}
