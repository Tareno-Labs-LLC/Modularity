#pragma once

// ModuInput.XR - the script-facing XR input API.
//
//     add ModuCPP;
//     add ModuInput;
//     add ModuInput.XR;
//
//     void TickUpdate() {
//         if (InputXR.Right.ButtonDown("Trigger")) {
//             InputXR.Right.HapticPulse(0.4f, 0.05f);
//         }
//         Vector2 stick = InputXR.Left.Axis2D("Thumbstick");
//         Vector3 aim   = InputXR.Right.AimPosition;
//     }
//
// A script using this never sees XrAction, XrPath, XrSession or XrSpace
// (section 48). Everything routes through ScriptContext, which reads a snapshot
// the engine takes once per XR frame.
//
// About the string form. `ButtonDown("Trigger")` was the API asked for, and it is
// kept, but no string is ever searched against OpenXR: XRButtonFromName is a scan
// over a fixed 15-entry table of `const char*` with no allocation, resolving to an
// enum that is then passed across the ABI as an int (section 13). The typed form
// `ButtonDown(XRButton::Trigger)` is available for anyone who wants the lookup
// gone entirely, and both call the exact same accessor underneath.
//
// With no XR session running, every accessor reports a neutral value - buttons
// false, axes zero, poses untracked. A VR script therefore still runs in a flat
// build without needing an `if (xr)` around every line.

#include "ModuCPPScriptApi.h"
#include "XR/XRInputTypes.h"

namespace ModuCPP {

// Re-exported so scripts can write XRButton::Trigger without naming the
// Modularity::XR namespace, matching how the rest of the script API reads.
using XRButton = ::Modularity::XR::XRButton;
using XRAxis = ::Modularity::XR::XRAxisKind;
using XRAxis2D = ::Modularity::XR::XRAxis2DKind;
using XRPoseKind = ::Modularity::XR::XRPoseKind;
using XRTrackingState = ::Modularity::XR::XRTrackingState;
using XRDevice = ::Modularity::XR::XRDevice;

namespace detail {

// Every accessor funnels through here so the "no context" case is written once.
// ctxPtr() is null outside a running script (editor preview, tooling), which is
// exactly when reporting neutral values is the right answer.
inline int xrDeviceId(XRDevice device) { return static_cast<int>(device); }

// Property proxies.
//
// The API these serve reads as fields - `InputXR.Left.Position`,
// `InputXR.Right.Tracked` - but every one of them has to run code to read the
// current frame's snapshot, so they cannot be plain members. Each proxy is a
// two-field value carrying only what the read needs, with a conversion operator
// that performs it. That keeps the requested syntax without the transpiler
// needing a single special case for it.
//
// One consequence worth knowing: because these convert rather than *are* the
// value, `InputXR.Left.Position.x` does not compile. Assign it first
// (`Vector3 p = InputXR.Left.Position;`) or call `.Value()`.

// Which pose-derived vector a proxy reads.
enum class XRVectorSource { PosePosition, Velocity, AngularVelocity, Forward };

struct XRVec3Property {
    XRDevice device = XRDevice::Left;
    XRPoseKind kind = XRPoseKind::Grip;
    XRVectorSource source = XRVectorSource::PosePosition;

    Vector3 Value() const;
    operator Vector3() const { return Value(); }
};

struct XRQuatProperty {
    XRDevice device = XRDevice::Left;
    XRPoseKind kind = XRPoseKind::Grip;

    Quaternion Value() const;
    operator Quaternion() const { return Value(); }
};

struct XRTrackedProperty {
    XRDevice device = XRDevice::Left;

    bool Value() const {
        if (ScriptContext* c = ctxPtr()) return c->IsXRDeviceTracked(xrDeviceId(device));
        return false;
    }
    operator bool() const { return Value(); }
};

struct XRTrackingStateProperty {
    XRDevice device = XRDevice::Left;

    XRTrackingState Value() const {
        if (ScriptContext* c = ctxPtr()) {
            return static_cast<XRTrackingState>(c->GetXRTrackingState(xrDeviceId(device)));
        }
        return XRTrackingState::NotTracked;
    }
    operator XRTrackingState() const { return Value(); }
};

struct XRProfileProperty {
    XRDevice device = XRDevice::Left;

    std::string Value() const {
        if (ScriptContext* c = ctxPtr()) return c->GetXRInteractionProfile(xrDeviceId(device));
        return {};
    }
    operator std::string() const { return Value(); }
};

inline Vector3 XRVec3Property::Value() const {
    ScriptContext* c = ctxPtr();
    if (!c) return Vector3(0.0f);

    switch (source) {
        case XRVectorSource::Velocity:
        case XRVectorSource::AngularVelocity: {
            Vector3 linear(0.0f);
            Vector3 angular(0.0f);
            c->GetXRVelocity(xrDeviceId(device), linear, angular);
            return (source == XRVectorSource::Velocity) ? linear : angular;
        }
        case XRVectorSource::Forward: {
            Vector3 position(0.0f);
            Quaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
            if (!c->GetXRPose(xrDeviceId(device), static_cast<int>(kind), position, rotation)) {
                return Vector3(0.0f, 0.0f, -1.0f);
            }
            // -Z is forward in OpenXR's pose convention, matching a Modularity camera.
            return glm::normalize(rotation * Vector3(0.0f, 0.0f, -1.0f));
        }
        case XRVectorSource::PosePosition:
        default: {
            Vector3 position(0.0f);
            Quaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
            c->GetXRPose(xrDeviceId(device), static_cast<int>(kind), position, rotation);
            return position;
        }
    }
}

inline Quaternion XRQuatProperty::Value() const {
    Vector3 position(0.0f);
    Quaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
    if (ScriptContext* c = ctxPtr()) {
        c->GetXRPose(xrDeviceId(device), static_cast<int>(kind), position, rotation);
    }
    return rotation;
}

} // namespace detail

// One tracked device: InputXR.Head, InputXR.Left or InputXR.Right.
//
// Constructed as a constexpr value per device, so `InputXR.Left` is a compile-time
// constant with no lookup of its own.
struct XRDeviceFacade {
    XRDevice device = XRDevice::Left;

    constexpr explicit XRDeviceFacade(XRDevice d) : device(d) {}

    // --- digital ---------------------------------------------------------

    // Held down right now.
    bool Button(XRButton button) const {
        if (ScriptContext* c = ctxPtr()) {
            return c->GetXRButton(detail::xrDeviceId(device), static_cast<int>(button));
        }
        return false;
    }
    bool Button(const char* name) const { return Button(::Modularity::XR::XRButtonFromName(name)); }
    bool Button(const std::string& name) const { return Button(name.c_str()); }

    // True only on the frame it goes down.
    bool ButtonDown(XRButton button) const {
        if (ScriptContext* c = ctxPtr()) {
            return c->GetXRButtonDown(detail::xrDeviceId(device), static_cast<int>(button));
        }
        return false;
    }
    bool ButtonDown(const char* name) const {
        return ButtonDown(::Modularity::XR::XRButtonFromName(name));
    }
    bool ButtonDown(const std::string& name) const { return ButtonDown(name.c_str()); }

    // True only on the frame it is released.
    bool ButtonUp(XRButton button) const {
        if (ScriptContext* c = ctxPtr()) {
            return c->GetXRButtonUp(detail::xrDeviceId(device), static_cast<int>(button));
        }
        return false;
    }
    bool ButtonUp(const char* name) const {
        return ButtonUp(::Modularity::XR::XRButtonFromName(name));
    }
    bool ButtonUp(const std::string& name) const { return ButtonUp(name.c_str()); }

    // Does the active controller even have this control? Meta Quest Touch has no
    // Menu button on the right hand, and this is how a script tells that apart
    // from "the button is not pressed".
    bool HasButton(XRButton button) const {
        if (ScriptContext* c = ctxPtr()) {
            return c->IsXRButtonBound(detail::xrDeviceId(device), static_cast<int>(button));
        }
        return false;
    }
    bool HasButton(const char* name) const {
        return HasButton(::Modularity::XR::XRButtonFromName(name));
    }

    // --- analog ----------------------------------------------------------

    // Trigger or Grip, 0..1.
    float Axis(XRAxis axis) const {
        if (ScriptContext* c = ctxPtr()) {
            return c->GetXRAxis(detail::xrDeviceId(device), static_cast<int>(axis));
        }
        return 0.0f;
    }
    float Axis(const char* name) const { return Axis(::Modularity::XR::XRAxisFromName(name)); }
    float Axis(const std::string& name) const { return Axis(name.c_str()); }

    // Thumbstick, each axis -1..1, +y up.
    Vector2 Axis2D(XRAxis2D axis) const {
        float x = 0.0f;
        float y = 0.0f;
        if (ScriptContext* c = ctxPtr()) {
            c->GetXRAxis2D(detail::xrDeviceId(device), static_cast<int>(axis), x, y);
        }
        return Vector2(x, y);
    }
    Vector2 Axis2D(const char* name) const {
        return Axis2D(::Modularity::XR::XRAxis2DFromName(name));
    }
    Vector2 Axis2D(const std::string& name) const { return Axis2D(name.c_str()); }

    // --- poses -----------------------------------------------------------
    //
    // Grip and Aim are different transforms and both are exposed (section 15):
    // Grip is where the controller physically is, Aim is where it points. Use
    // Grip for held objects and hand models, Aim for rays, weapons and UI.
    //
    // Positions are in world space, with the XR Origin's transform already
    // applied, so a script can use them directly against scene objects.

    // Explicit form, for when you want to know whether the pose was valid rather
    // than accepting the neutral value the properties below return.
    bool TryPose(XRPoseKind kind, Vector3& outPosition, Quaternion& outRotation) const {
        if (ScriptContext* c = ctxPtr()) {
            return c->GetXRPose(detail::xrDeviceId(device), static_cast<int>(kind), outPosition,
                                outRotation);
        }
        return false;
    }

    // Properties, not calls: `InputXR.Left.Position`, not `InputXR.Left.Position()`.
    // Position/Rotation default to the grip pose, because "where is the
    // controller" is what most gameplay code means.
    detail::XRVec3Property Position{ device, XRPoseKind::Grip,
                                     detail::XRVectorSource::PosePosition };
    detail::XRQuatProperty Rotation{ device, XRPoseKind::Grip };

    detail::XRVec3Property GripPosition{ device, XRPoseKind::Grip,
                                         detail::XRVectorSource::PosePosition };
    detail::XRQuatProperty GripRotation{ device, XRPoseKind::Grip };
    detail::XRVec3Property AimPosition{ device, XRPoseKind::Aim,
                                        detail::XRVectorSource::PosePosition };
    detail::XRQuatProperty AimRotation{ device, XRPoseKind::Aim };

    // Direction the aim pose points, ready to hand straight to a raycast.
    detail::XRVec3Property AimDirection{ device, XRPoseKind::Aim,
                                         detail::XRVectorSource::Forward };

    // --- motion ----------------------------------------------------------
    // Grip-pose velocities, which is what throwing a held object needs.

    detail::XRVec3Property Velocity{ device, XRPoseKind::Grip,
                                     detail::XRVectorSource::Velocity };
    detail::XRVec3Property AngularVelocity{ device, XRPoseKind::Grip,
                                            detail::XRVectorSource::AngularVelocity };

    // --- tracking --------------------------------------------------------

    detail::XRTrackedProperty Tracked{ device };
    detail::XRTrackingStateProperty TrackingState{ device };

    // OpenXR interaction profile path currently bound to this device, e.g.
    // "/interaction_profiles/oculus/touch_controller". Empty when nothing is bound.
    detail::XRProfileProperty InteractionProfile{ device };

    // --- output ----------------------------------------------------------

    // amplitude 0..1, duration in seconds. frequency 0 lets the runtime pick,
    // which is what you want on Quest. Silently does nothing on the head device
    // or with no session.
    void HapticPulse(float amplitude = 0.5f, float duration = 0.1f,
                     float frequency = 0.0f) const {
        if (ScriptContext* c = ctxPtr()) {
            c->XRHapticPulse(detail::xrDeviceId(device), amplitude, duration, frequency);
        }
    }
};

// The XR input root. `InputXR.Left`, `InputXR.Right`, `InputXR.Head`.
struct XRInputFacade {
    XRDeviceFacade Head{ XRDevice::Head };
    XRDeviceFacade Left{ XRDevice::Left };
    XRDeviceFacade Right{ XRDevice::Right };

    // True while an XR session is running. Worth checking before switching a
    // whole control scheme; individual accessors already degrade safely.
    bool Active() const {
        if (ScriptContext* c = ctxPtr()) return c->IsXRActive();
        return false;
    }

    // Head pose convenience, since the head has no buttons to hang them off.
    // Properties, matching the per-device ones above.
    detail::XRVec3Property HeadPosition{ XRDevice::Head, XRPoseKind::Grip,
                                         detail::XRVectorSource::PosePosition };
    detail::XRQuatProperty HeadRotation{ XRDevice::Head, XRPoseKind::Grip };
    // Direction the player is looking, in world space.
    detail::XRVec3Property HeadForward{ XRDevice::Head, XRPoseKind::Grip,
                                        detail::XRVectorSource::Forward };

    // Hand tracking is not implemented. These exist so the shape of the API is
    // settled and a script can check for it, rather than so it can be faked
    // (section 31): both report false today and will start reporting true when
    // XR_EXT_hand_tracking is actually wired up.
    bool HandTrackingSupported() const { return false; }
    bool HandTrackingActive() const { return false; }
};

// The single instance scripts use. inline so every translation unit shares it.
inline const XRInputFacade InputXR{};

// Interaction state for the script's own object, when it carries an XR Grab
// Interactable. Poll these in TickUpdate:
//
//     void TickUpdate() {
//         if (XRInteraction.SelectEntered) print("grabbed!");
//         if (XRInteraction.SelectExited)  print("released!");
//         if (XRInteraction.Activated)     Fire();
//     }
//
// The *Entered / *Exited / Activated flags are true for exactly one frame, so a
// script sees each transition once no matter how often it asks.
struct XRInteractionFacade {
    // Every member below is a property, not a call, matching the input facade:
    // `XRInteraction.Selected`, never `XRInteraction.Selected()`. Each is a
    // one-field proxy holding the ScriptContext getter, with a conversion
    // operator that runs it.
    struct BoolProperty {
        bool (ScriptContext::*getter)() const;
        bool Value() const {
            if (ScriptContext* c = ctxPtr()) return (c->*getter)();
            return false;
        }
        operator bool() const { return Value(); }
    };

    // Continuous state.
    BoolProperty Hovered{ &ScriptContext::IsXRHovered };
    BoolProperty Selected{ &ScriptContext::IsXRSelected };

    // One-frame transitions.
    BoolProperty HoverEntered{ &ScriptContext::XRHoverEntered };
    BoolProperty HoverExited{ &ScriptContext::XRHoverExited };
    BoolProperty SelectEntered{ &ScriptContext::XRSelectEntered };
    BoolProperty SelectExited{ &ScriptContext::XRSelectExited };
    BoolProperty Activated{ &ScriptContext::XRActivated };
    BoolProperty Deactivated{ &ScriptContext::XRDeactivated };

    // Scene id of the controller object interacting with this one, or -1.
    // Held wins over hovered.
    struct InteractorIdProperty {
        int Value() const {
            if (ScriptContext* c = ctxPtr()) return c->GetXRInteractorId();
            return -1;
        }
        operator int() const { return Value(); }
    };
    InteractorIdProperty InteractorId{};

    // Which hand is interacting, resolved from the interactor object's own
    // components. Head means "nothing is interacting", which no controller is.
    struct InteractorHandProperty {
        XRDevice Value() const {
            ScriptContext* c = ctxPtr();
            if (!c) return XRDevice::Head;
            const int id = c->GetXRInteractorId();
            if (id < 0) return XRDevice::Head;
            if (const SceneObject* interactor = c->FindObjectById(id)) {
                // The action-based controller is authoritative when present: it
                // is the component that actually drove the selection.
                if (interactor->hasXRActionBasedController) {
                    return (interactor->xrActionBasedController.hand == XRHand::Right)
                               ? XRDevice::Right
                               : XRDevice::Left;
                }
                if (interactor->hasXRController) {
                    return (interactor->xrController.hand == XRHand::Right) ? XRDevice::Right
                                                                            : XRDevice::Left;
                }
            }
            return XRDevice::Head;
        }
        operator XRDevice() const { return Value(); }
    };
    InteractorHandProperty InteractorHand{};
};

inline const XRInteractionFacade XRInteraction{};

} // namespace ModuCPP
