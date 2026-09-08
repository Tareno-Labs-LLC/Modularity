#pragma once

#include "../Common.h"

#include <cmath>

#if MODULARITY_HAS_OPENXR
#include "XRPlatform.h"
#endif

namespace Modularity::XR {

struct XRPose {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    bool positionValid = false;
    bool orientationValid = false;
    bool positionTracked = false;
    bool orientationTracked = false;

    bool isTracked() const { return positionTracked && orientationTracked; }
    bool isValid() const { return positionValid && orientationValid; }

    glm::mat4 toMatrix() const {
        return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation);
    }
};

struct XRView {
    XRPose pose;
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleUp = 0.0f;
    float angleDown = 0.0f;
};

#if MODULARITY_HAS_OPENXR

inline glm::vec3 ToGlm(const XrVector3f& v) { return glm::vec3(v.x, v.y, v.z); }

inline glm::quat ToGlm(const XrQuaternionf& q) {
    return glm::quat(q.w, q.x, q.y, q.z);
}

inline XrVector3f ToXr(const glm::vec3& v) { return XrVector3f{ v.x, v.y, v.z }; }

inline XrQuaternionf ToXr(const glm::quat& q) { return XrQuaternionf{ q.x, q.y, q.z, q.w }; }

inline XRPose ToXRPose(const XrPosef& pose, XrSpaceLocationFlags flags) {
    XRPose out;
    out.positionValid = (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    out.orientationValid = (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    out.positionTracked = (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
    out.orientationTracked = (flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
    if (out.positionValid) out.position = ToGlm(pose.position);
    if (out.orientationValid) out.rotation = ToGlm(pose.orientation);
    return out;
}

inline XRView ToXRView(const XrView& view, XrViewStateFlags stateFlags) {
    XRView out;
    out.pose.positionValid = (stateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    out.pose.orientationValid = (stateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    out.pose.positionTracked = (stateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
    out.pose.orientationTracked = (stateFlags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT) != 0;
    if (out.pose.positionValid) out.pose.position = ToGlm(view.pose.position);
    if (out.pose.orientationValid) out.pose.rotation = ToGlm(view.pose.orientation);
    out.angleLeft = view.fov.angleLeft;
    out.angleRight = view.fov.angleRight;
    out.angleUp = view.fov.angleUp;
    out.angleDown = view.fov.angleDown;
    return out;
}

#endif // MODULARITY_HAS_OPENXR

inline glm::mat4 XRProjectionMatrix(const XRView& view, float nearPlane, float farPlane) {
    const float tanLeft = std::tan(view.angleLeft);
    const float tanRight = std::tan(view.angleRight);
    const float tanDown = std::tan(view.angleDown);
    const float tanUp = std::tan(view.angleUp);

    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;
    if (std::abs(tanWidth) < 1e-6f || std::abs(tanHeight) < 1e-6f) {
        return glm::mat4(1.0f);
    }

    const float safeNear = std::max(0.001f, nearPlane);
    const float safeFar = std::max(safeNear + 0.001f, farPlane);

    glm::mat4 proj(0.0f);
    proj[0][0] = 2.0f / tanWidth;
    proj[1][1] = 2.0f / tanHeight;
    proj[2][0] = (tanRight + tanLeft) / tanWidth;
    proj[2][1] = (tanUp + tanDown) / tanHeight;
    proj[2][2] = -(safeFar + safeNear) / (safeFar - safeNear);
    proj[2][3] = -1.0f;
    proj[3][2] = -(2.0f * safeFar * safeNear) / (safeFar - safeNear);
    return proj;
}

inline glm::mat4 XRViewMatrix(const XRPose& eyePose, const glm::mat4& originToWorld) {
    return glm::inverse(originToWorld * eyePose.toMatrix());
}

}
