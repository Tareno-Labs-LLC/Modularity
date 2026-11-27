#pragma once
#include "Common.h"
#include "ProjectManager.h"
#include "SceneObject.h"
#include <vector>

enum class PhysicsCollisionPhase {
    Enter,
    Stay,
    Exit
};

struct PhysicsCollisionEvent {
    int objectAId = -1;
    int objectBId = -1;
    PhysicsCollisionPhase phase = PhysicsCollisionPhase::Enter;
};

struct ContactPairState {
    int manifolds = 0;
    glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);
};

class IPhysicsBackend {
public:
    virtual ~IPhysicsBackend() = default;
    virtual bool init() = 0;
    virtual void shutdown() = 0;
    virtual bool isReady() const = 0;
    virtual void setProjectSettings(const ProjectPhysicsSettings& settings) = 0;
    virtual bool setLinearVelocity(int id, const glm::vec3& velocity) = 0;
    virtual bool setAngularVelocity(int id, const glm::vec3& velocity) = 0;
    virtual bool setContinuousCollisionDetection(int id, bool enabled) = 0;
    virtual bool setActorYaw(int id, float yawDegrees) = 0;
    virtual bool getLinearVelocity(int id, glm::vec3& outVelocity) const = 0;
    virtual bool getAngularVelocity(int id, glm::vec3& outVelocity) const = 0;
    virtual bool getActorPose(int id, glm::vec3& outPosition,
                              glm::vec3& outRotationDeg) const = 0;
    virtual bool setActorPose(int id, const glm::vec3& position, const glm::vec3& rotationDeg) = 0;
    virtual bool addForce(int id, const glm::vec3& force) = 0;
    virtual bool addImpulse(int id, const glm::vec3& impulse) = 0;
    virtual bool addTorque(int id, const glm::vec3& torque) = 0;
    virtual bool addAngularImpulse(int id, const glm::vec3& impulse) = 0;
    virtual bool refreshObject(const SceneObject& obj) = 0;
    virtual bool raycastClosest(const glm::vec3& origin, const glm::vec3& dir, float distance, int ignoreId, glm::vec3* hitPos = nullptr,glm::vec3* hitNormal = nullptr, float* hitDistance = nullptr, int* hitActorId = nullptr, glm::vec3* hitActorVelocity = nullptr, float* hitStaticFriction = nullptr, float* hitDynamicFriction = nullptr) const = 0;
    virtual void onPlayStart(const std::vector<SceneObject>& objects) = 0;
    virtual void onPlayStop() = 0;
    virtual void simulate(float deltaTime, std::vector<SceneObject>& objects) = 0;
    virtual void drainCollisionEvents(std::vector<PhysicsCollisionEvent>& outEvents) = 0;

    virtual int countActiveContacts(int objectId) const = 0;

    virtual bool areObjectsTouching(int objectAId, int objectBId) const = 0;
    virtual bool getContactNormal(int objectId, glm::vec3& outNormal) const = 0;

    virtual bool setObjectsCollisionIgnored(int objectAId, int objectBId, bool ignored) = 0;

    // Point-based queries/forces. Points are world space. Defaults report "unsupported" so a
    // backend that does not implement them keeps compiling and fails cleanly at runtime.
    virtual bool addForceAtPosition(int id, const glm::vec3& force, const glm::vec3& worldPoint) {
        (void)id; (void)force; (void)worldPoint; return false;
    }
    virtual bool addImpulseAtPosition(int id, const glm::vec3& impulse, const glm::vec3& worldPoint) {
        (void)id; (void)impulse; (void)worldPoint; return false;
    }
    virtual bool getPointVelocity(int id, const glm::vec3& worldPoint, glm::vec3& outVelocity) const {
        (void)id; (void)worldPoint; (void)outVelocity; return false;
    }
    virtual bool getCenterOfMassPosition(int id, glm::vec3& outWorldCenterOfMass) const {
        (void)id; (void)outWorldCenterOfMass; return false;
    }
    // Live simulated pose of the body origin (not the interpolated render pose).
    virtual bool getActorPoseQuat(int id, glm::vec3& outPosition, glm::quat& outRotation) const {
        (void)id; (void)outPosition; (void)outRotation; return false;
    }
    virtual bool getMass(int id, float& outMassKg) const {
        (void)id; (void)outMassKg; return false;
    }
    // Like raycastClosest, but ignores ignoreId explicitly (-1 = none) and can skip sensors.
    virtual bool raycastClosestFiltered(const glm::vec3& origin, const glm::vec3& dir, float distance,
                                        int ignoreId, bool skipSensors,
                                        glm::vec3* hitPos, glm::vec3* hitNormal, float* hitDistance,
                                        int* hitActorId, float* hitFriction) const {
        (void)skipSensors;
        return raycastClosest(origin, dir, distance, ignoreId, hitPos, hitNormal, hitDistance,
                              hitActorId, nullptr, hitFriction, nullptr);
    }

    // Called by simulate() before every fixed physics step with that step's dt, so the engine
    // can run script FixedUpdate hooks. Forces added from inside the callback apply to exactly
    // that step. Plain function pointer + user data: no allocation, cheap to re-set per frame.
    using PreStepCallback = void (*)(void* user, float stepDeltaTime);
    void setPreStepCallback(PreStepCallback callback, void* user) {
        mPreStepCallback = callback;
        mPreStepUser = user;
    }

protected:
    void invokePreStepCallback(float stepDeltaTime) {
        if (mPreStepCallback) mPreStepCallback(mPreStepUser, stepDeltaTime);
    }
    PreStepCallback mPreStepCallback = nullptr;
    void* mPreStepUser = nullptr;
};
