#pragma once
#include "Common.h"
#include "IPhysicsBackend.h"
#include "ProjectManager.h"
#include "SceneObject.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef MODULARITY_ENABLE_PHYSX
#include "PxPhysicsAPI.h"
#include "cooking/PxCooking.h"
class PhysicsSimulationEvents;
class PhysicsPairFilter;
#endif

class PhysicsSystem : public IPhysicsBackend {
    public:
        PhysicsSystem();
        ~PhysicsSystem() override;
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
        bool raycastClosest(const glm::vec3& origin, const glm::vec3& dir, float distance, int ignoreId, glm::vec3* hitPos = nullptr,
                            glm::vec3* hitNormal = nullptr, float* hitDistance = nullptr, int* hitActorId = nullptr, glm::vec3* hitActorVelocity = nullptr,
                            float* hitStaticFriction = nullptr, float* hitDynamicFriction = nullptr) const override;
        void onPlayStart(const std::vector<SceneObject>& objects) override;
        void onPlayStop() override;
        void simulate(float deltaTime, std::vector<SceneObject>& objects) override;
        void drainCollisionEvents(std::vector<PhysicsCollisionEvent>& outEvents) override;
    private:
#ifdef MODULARITY_ENABLE_PHYSX
        friend class PhysicsSimulationEvents;
        friend class PhysicsPairFilter;
        struct ActorRecord {
            physx::PxRigidActor* actor = nullptr;
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
        };
        physx::PxDefaultAllocator mAllocator;
        physx::PxDefaultErrorCallback mErrorCallback;
        physx::PxFoundation* mFoundation = nullptr;
        physx::PxPhysics* mPhysics = nullptr;
        physx::PxDefaultCpuDispatcher* mDispatcher = nullptr;
        physx::PxScene* mScene = nullptr;
        physx::PxMaterial* mDefaultMaterial = nullptr;
        physx::PxCookingParams mCookParams{physx::PxTolerancesScale()};
        ProjectPhysicsSettings mProjectSettings;
        std::unordered_map<int, ActorRecord> mActors;
        std::unordered_map<const physx::PxRigidActor*, int> mActorIdsByPtr;
        std::unique_ptr<PhysicsSimulationEvents> mSimulationEvents;
        std::unique_ptr<PhysicsPairFilter> mPairFilter;
        mutable std::mutex mCollisionEventMutex;
        std::vector<PhysicsCollisionEvent> mPendingCollisionEvents;
        std::unordered_map<uint64_t, ContactPairState> mActiveCollisionPairs;
        std::unordered_set<uint64_t> mIgnoredCollisionPairs;
        void queueCollisionEvent(int objectAId, int objectBId, PhysicsCollisionPhase phase);
        void endCollisionPairsForObject(int objectId);
        bool isCollisionPairIgnored(int objectAId, int objectBId) const;
    public:
        int countActiveContacts(int objectId) const override;
        bool areObjectsTouching(int objectAId, int objectBId) const override;
        bool getContactNormal(int objectId, glm::vec3& outNormal) const override;
        void noteContactNormal(int objectAId, int objectBId, const glm::vec3& lowToHighNormal);
        bool setObjectsCollisionIgnored(int objectAId, int objectBId, bool ignored) override;
    private:
        void clearActors();
        void applySceneGravity();
        ActorRecord createActorFor(const SceneObject& obj) const;
        bool attachColliderShape(physx::PxRigidActor* actor, const SceneObject& obj, bool isDynamic, bool bakeWorldSpaceStaticMesh) const;
        bool attachPrimitiveShape(physx::PxRigidActor* actor, const SceneObject& obj, bool isDynamic) const;
        bool gatherMeshData(const SceneObject& obj, std::vector<physx::PxVec3>& vertices, std::vector<uint32_t>& indices) const;
        physx::PxTriangleMesh* cookTriangleMesh(const std::vector<physx::PxVec3>& vertices, const std::vector<uint32_t>& indices) const;
        physx::PxConvexMesh* cookConvexMesh(const std::vector<physx::PxVec3>& vertices) const;
#endif
};
