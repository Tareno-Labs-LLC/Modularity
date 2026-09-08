#pragma once
#include "IPhysicsBackend.h"
#ifdef MODULARITY_ENABLE_JOLT
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
class JoltContactEvents;
class JoltPhysicsBackend final : public IPhysicsBackend {
public:
    JoltPhysicsBackend();
    ~JoltPhysicsBackend() override;
    bool init() override;
    void shutdown() override;
    bool isReady() const override;
    void setProjectSettings(const ProjectPhysicsSettings& settings) override;
    bool setLinearVelocity(int id, const glm::vec3& velocity) override;
    bool setAngularVelocity(int id, const glm::vec3& velocity) override;
    bool setContinuousCollisionDetection(int id, bool enabled) override;
    bool setActorYaw(int id, float yawDegrees) override;
    bool getLinearVelocity(int id, glm::vec3& outVelocity) const override;
    bool getAngularVelocity(int id, glm::vec3& outVelocity) const override;
    bool getActorPose(int id, glm::vec3& outPosition,
                      glm::vec3& outRotationDeg) const override;
    bool setActorPose(int id, const glm::vec3& position, const glm::vec3& rotationDeg) override;
    bool addForce(int id, const glm::vec3& force) override;
    bool addImpulse(int id, const glm::vec3& impulse) override;
    bool addTorque(int id, const glm::vec3& torque) override;
    bool addAngularImpulse(int id, const glm::vec3& impulse) override;
    bool refreshObject(const SceneObject& obj) override;
    bool raycastClosest(const glm::vec3& origin, const glm::vec3& dir, float distance, int ignoreId, glm::vec3* hitPos = nullptr, glm::vec3* hitNormal = nullptr, float* hitDistance = nullptr, int* hitActorId = nullptr, glm::vec3* hitActorVelocity = nullptr, float* hitStaticFriction = nullptr, float* hitDynamicFriction = nullptr) const override;
    void onPlayStart(const std::vector<SceneObject>& objects) override;
    void onPlayStop() override;
    void simulate(float deltaTime, std::vector<SceneObject>& objects) override;
    void drainCollisionEvents(std::vector<PhysicsCollisionEvent>& outEvents) override;
    int countActiveContacts(int objectId) const override;
    bool areObjectsTouching(int objectAId, int objectBId) const override;
    bool getContactNormal(int objectId, glm::vec3& outNormal) const override;
    void noteContactNormal(int objectAId, int objectBId, const glm::vec3& lowToHighNormal);
    bool setObjectsCollisionIgnored(int objectAId, int objectBId, bool ignored) override;
private:
    friend class JoltContactEvents;
    // A per-body authoring/cache state stuff
    struct BodyRecord {
        JPH::BodyID bodyId;
        bool isDynamic = false;
        bool isKinematic = false;
        bool hasWorldBakedStaticMesh = false;
        bool simulationDisabled = false;
        bool hasAuthoringPose = false;
        bool gravityDisabled = false;
        float linearDamping = 0.0f;
        float angularDamping = 0.0f;
        float massKg = 0.0f;
        bool useCustomCenterOfMass = false;
        glm::vec3 centerOfMass = glm::vec3(0.0f);
        glm::vec3 lastAuthoringPosition = glm::vec3(0.0f);
        glm::vec3 lastAuthoringRotation = glm::vec3(0.0f);
        uint32_t angularLockMask = 0;
        bool isTrigger = false;
        JPH::Vec3 prevPos = JPH::Vec3::sZero();
        JPH::Quat prevRot = JPH::Quat::sIdentity();
        JPH::Vec3 currPos = JPH::Vec3::sZero();
        JPH::Quat currRot = JPH::Quat::sIdentity();
    };
    struct JoltFilters;
    BodyRecord createBodyFor(const SceneObject& obj);
    void clearBodies();
    void applySceneGravity();
    bool mReady = false;
    ProjectPhysicsSettings mProjectSettings;
    float mTimeAccumulator = 0.0f;
    std::unique_ptr<JPH::TempAllocator> mTempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> mJobSystem;
    std::unique_ptr<JoltFilters> mFilters;
    std::unique_ptr<JPH::PhysicsSystem> mPhysicsSystem;
    std::unique_ptr<JoltContactEvents> mContactEvents;

    std::unordered_map<int, BodyRecord> mBodies;
    std::unordered_map<uint32_t, int> mIdsByBody;
    mutable std::mutex mCollisionEventMutex;
    std::vector<PhysicsCollisionEvent> mPendingCollisionEvents;
    std::unordered_map<uint64_t, ContactPairState> mActiveCollisionPairs;
    std::unordered_set<uint64_t> mIgnoredCollisionPairs;
    void queueCollisionEvent(int objectAId, int objectBId, PhysicsCollisionPhase phase);
    void endCollisionPairsForObject(int objectId);
    bool isCollisionPairIgnored(int objectAId, int objectBId) const;
};
#endif // MODULARITY_ENABLE_JOLT
