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
};
