#include "PhysicsSystem.h"
#ifdef MODULARITY_ENABLE_PHYSX
#include "PxPhysicsAPI.h"
#include "ModelLoader.h"
#include "Rendering.h"
#include <numeric>
#include <algorithm>
#include <new>
#include "extensions/PxRigidBodyExt.h"
using namespace physx;
class PhysicsSimulationEvents final : public PxSimulationEventCallback {
public:
    explicit PhysicsSimulationEvents(PhysicsSystem& owner) : mOwner(owner) {}

    void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
    void onWake(PxActor**, PxU32) override {}
    void onSleep(PxActor**, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}

    void onContact(const PxContactPairHeader& pairHeader,
                   const PxContactPair* pairs, PxU32 pairCount) override {
        const int a = actorId(pairHeader.actors[0]);
        const int b = actorId(pairHeader.actors[1]);
        if (a < 0 || b < 0) return;
        for (PxU32 i = 0; i < pairCount; ++i) {
            if (pairs[i].events.isSet(PxPairFlag::eNOTIFY_TOUCH_FOUND)) {
                mOwner.queueCollisionEvent(a, b, PhysicsCollisionPhase::Enter);
            }
            if (pairs[i].events.isSet(PxPairFlag::eNOTIFY_TOUCH_LOST)) {
                mOwner.queueCollisionEvent(a, b, PhysicsCollisionPhase::Exit);
            }
            if (pairs[i].events.isSet(PxPairFlag::eNOTIFY_TOUCH_FOUND) ||
                pairs[i].events.isSet(PxPairFlag::eNOTIFY_TOUCH_PERSISTS)) {
                recordNormal(pairHeader, pairs[i], a, b);
            }
        }
    }

    void recordNormal(const PxContactPairHeader& pairHeader, const PxContactPair& pair,
                      int idA, int idB) {
        constexpr PxU32 kMaxPoints = 16;
        PxContactPairPoint points[kMaxPoints];
        const PxU32 count = pair.extractContacts(points, kMaxPoints);
        if (count == 0) return;
        PxVec3 sum(0.0f);
        for (PxU32 i = 0; i < count; ++i) sum += points[i].normal;
        glm::vec3 normal(sum.x, sum.y, sum.z);
        const float lengthSq = glm::dot(normal, normal);
        if (lengthSq < 0.000001f) return;
        normal /= std::sqrt(lengthSq);

        const PxRigidActor* actor0 = pairHeader.actors[0]->is<PxRigidActor>();
        const PxRigidActor* actor1 = pairHeader.actors[1]->is<PxRigidActor>();
        if (!actor0 || !actor1) return;
        const PxVec3 centre0 = actor0->getGlobalPose().p;
        const PxVec3 centre1 = actor1->getGlobalPose().p;
        glm::vec3 lowToHigh(centre1.x - centre0.x, centre1.y - centre0.y, centre1.z - centre0.z);
        if (idA > idB) lowToHigh = -lowToHigh;
        if (glm::dot(normal, lowToHigh) < 0.0f) normal = -normal;
        mOwner.noteContactNormal(idA, idB, normal);
    }

    void onTrigger(PxTriggerPair* pairs, PxU32 count) override {
        for (PxU32 i = 0; i < count; ++i) {
            if (pairs[i].flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER |
                                  PxTriggerPairFlag::eREMOVED_SHAPE_OTHER)) {
                continue;
            }
            const int a = actorId(pairs[i].triggerActor);
            const int b = actorId(pairs[i].otherActor);
            if (a < 0 || b < 0) continue;
            if (pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_FOUND) {
                mOwner.queueCollisionEvent(a, b, PhysicsCollisionPhase::Enter);
            }
            if (pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_LOST) {
                mOwner.queueCollisionEvent(a, b, PhysicsCollisionPhase::Exit);
            }
        }
    }

private:
    static int actorId(const PxActor* actor) {
        if (!actor || !actor->userData) return -1;
        return static_cast<int>(reinterpret_cast<intptr_t>(actor->userData) - 1);
    }
    PhysicsSystem& mOwner;
};

class PhysicsPairFilter final : public PxSimulationFilterCallback {
public:
    explicit PhysicsPairFilter(PhysicsSystem& owner) : mOwner(owner) {}

    PxFilterFlags pairFound(PxU64,
                            PxFilterObjectAttributes, PxFilterData,
                            const PxActor* actor0, const PxShape*,
                            PxFilterObjectAttributes, PxFilterData,
                            const PxActor* actor1, const PxShape*,
                            PxPairFlags&) override {
        if (mOwner.isCollisionPairIgnored(actorId(actor0), actorId(actor1))) {
            return PxFilterFlag::eSUPPRESS;
        }
        return PxFilterFlags();
    }

    void pairLost(PxU64, PxFilterObjectAttributes, PxFilterData,
                  PxFilterObjectAttributes, PxFilterData, bool) override {}

    bool statusChange(PxU64&, PxPairFlags&, PxFilterFlags&) override {
        return false;
    }

private:
    static int actorId(const PxActor* actor) {
        if (!actor || !actor->userData) return -1;
        return static_cast<int>(reinterpret_cast<intptr_t>(actor->userData) - 1);
    }

    PhysicsSystem& mOwner;
};

namespace {
    uint64_t CollisionPairKey(int a, int b) {
        const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
        const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
        return (static_cast<uint64_t>(lo) << 32u) | static_cast<uint64_t>(hi);
    }
    PxFilterFlags ModularitySimulationFilterShader(
        PxFilterObjectAttributes attributes0, PxFilterData filterData0,
        PxFilterObjectAttributes attributes1, PxFilterData filterData1,
        PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize) {
        PxFilterFlags result = PxDefaultSimulationFilterShader(
            attributes0, filterData0, attributes1, filterData1,
            pairFlags, constantBlock, constantBlockSize);
        if (!result.isSet(PxFilterFlag::eKILL) && !result.isSet(PxFilterFlag::eSUPPRESS)) {
            pairFlags |= PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_TOUCH_LOST |
                         PxPairFlag::eNOTIFY_TOUCH_PERSISTS | PxPairFlag::eNOTIFY_CONTACT_POINTS;
            result |= PxFilterFlag::eCALLBACK;
        }
        return result;
    }
    PxVec3 ToPxVec3(const glm::vec3& v) {
        return PxVec3(v.x, v.y, v.z);
    }
    PxQuat ToPxQuat(const glm::vec3& eulerDeg) {
        glm::vec3 r = glm::radians(eulerDeg);
        glm::mat4 m(1.0f);
        m = glm::rotate(m, r.x, glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, r.y, glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, r.z, glm::vec3(0.0f, 0.0f, 1.0f));
        glm::quat q = glm::quat_cast(glm::mat3(m));
        return PxQuat(q.x, q.y, q.z, q.w);
    }
    glm::vec3 ToGlmVec3(const PxVec3& v) {return glm::vec3(v.x, v.y, v.z);}
    glm::vec3 ToGlmVec3(const glm::vec3& v) { return v; }
    glm::vec3 ExtractEulerXYZ(const glm::mat3& m) {
        float T1 = std::atan2(m[2][1], m[2][2]);
        float C2 = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
        float T2 = std::atan2(-m[2][0], C2);
        float S1 = std::sin(T1);
        float C1 = std::cos(T1);
        float T3 = std::atan2(S1 * m[0][2] - C1 * m[0][1], C1 * m[1][1] - S1 * m[1][2]);
        return glm::vec3(-T1, -T2, -T3);
    }
    glm::vec3 ToGlmEulerDeg(const PxQuat& q) {
        glm::quat gq(q.w, q.x, q.y, q.z);
        glm::mat3 m = glm::mat3_cast(gq);
        return glm::degrees(ExtractEulerXYZ(m));
    }
    float EffectiveMassKilograms(const SceneObject& obj, const ProjectPhysicsSettings& settings) {
        const float authoredMass = std::max(0.0001f, obj.rigidbody.mass);
        const float unitScale = std::max(0.000001f, ProjectMassUnitToKilograms(settings.massUnit));
        return std::max(0.01f, authoredMass * unitScale);
    }
    PxVec3 SceneGravity(const ProjectPhysicsSettings& settings) {
        return PxVec3(0.0f, -9.81f * std::max(0.0f, settings.globalGravityScale), 0.0f);
    }
    uint32_t BuildAngularLockMask(const SceneObject& obj) {
        uint32_t mask = 0;
        if (obj.rigidbody.lockRotationX) mask |= 1u << 0u;
        if (obj.rigidbody.lockRotationY) mask |= 1u << 1u;
        if (obj.rigidbody.lockRotationZ) mask |= 1u << 2u;
        return mask;
    }
    PxRigidDynamicLockFlags BuildAngularLockFlags(const SceneObject& obj) {
        PxRigidDynamicLockFlags lockFlags;
        if (obj.rigidbody.lockRotationX) lockFlags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
        if (obj.rigidbody.lockRotationY) lockFlags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
        if (obj.rigidbody.lockRotationZ) lockFlags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
        return lockFlags;
    }
    PxMaterial* CreateShapeMaterial(PxPhysics* physics, PxMaterial* fallback, const SceneObject& obj, bool& outOwned) {
        outOwned = false;
        if (!physics || !fallback) return fallback;
        if (!obj.hasCollider || !obj.collider.enabled) {
            return fallback;
        }
        const float staticFriction = std::clamp(obj.collider.staticFriction, 0.0f, 4.0f);
        const float dynamicFriction = std::clamp(obj.collider.dynamicFriction, 0.0f, 4.0f);
        const float restitution = std::clamp(obj.collider.restitution, 0.0f, 1.0f);
        PxMaterial* material = physics->createMaterial(staticFriction, dynamicFriction, restitution);
        if (!material) {
            return fallback;
        }
        outOwned = true;
        return material;
    }
    bool BuildTriangleMeshFromVertexStream(const std::vector<float>& source, std::vector<PxVec3>& vertices, std::vector<uint32_t>& indices) {
        constexpr size_t kStrideFloats = 8;
        if (source.size() < kStrideFloats * 3 || (source.size() % kStrideFloats) != 0) return false;
        const size_t vertexCount = source.size() / kStrideFloats;
        vertices.reserve(vertexCount);
        indices.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) {
            const size_t base = i * kStrideFloats;
            vertices.emplace_back(source[base + 0], source[base + 1], source[base + 2]);
            indices[i] = static_cast<uint32_t>(i);
        }
        return !vertices.empty() && (indices.size() % 3 == 0);
    }

    bool PoseChangedSignificantly(const glm::vec3& prevPosition, const glm::vec3& prevRotation,
                                  const glm::vec3& nextPosition, const glm::vec3& nextRotation) {
        const glm::vec3 positionDelta = nextPosition - prevPosition;
        const glm::vec3 rotationDelta = nextRotation - prevRotation;
        return glm::dot(positionDelta, positionDelta) > 1e-8f || glm::dot(rotationDelta, rotationDelta) > 1e-8f;
    }
    void BakeScaleIntoVertices(std::vector<PxVec3>& vertices, const glm::vec3& scale) {
        const PxVec3 bakedScale(std::abs(scale.x), std::abs(scale.y), std::abs(scale.z));
        for (PxVec3& v : vertices) {
            v.x *= bakedScale.x;
            v.y *= bakedScale.y;
            v.z *= bakedScale.z;
        }
    }
    void BakeTransformIntoVertices(std::vector<PxVec3>& vertices, const SceneObject& obj) {
        glm::mat4 transform(1.0f);
        transform = glm::translate(transform, obj.position);
        transform = glm::rotate(transform, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        transform = glm::translate(transform, obj.collider.offset);
        transform = glm::scale(transform, obj.scale);
        for (PxVec3& v : vertices) {
            glm::vec4 world = transform * glm::vec4(v.x, v.y, v.z, 1.0f);
            v = PxVec3(world.x, world.y, world.z);
        }
    }
    glm::mat4 BuildColliderVertexTransform(const SceneObject& obj) {
        glm::mat4 transform(1.0f);
        transform = glm::translate(transform, obj.position);
        transform = glm::rotate(transform, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        transform = glm::translate(transform, obj.collider.offset);
        transform = glm::scale(transform, obj.scale);
        return transform;
    }
    template <typename VertexContainer>
    void ComputeColliderBounds(const VertexContainer& vertices, const SceneObject& obj, bool bakeWorldSpaceStaticMesh,
                               glm::vec3& boundsMin, glm::vec3& boundsMax) {
        boundsMin = glm::vec3(FLT_MAX);
        boundsMax = glm::vec3(-FLT_MAX);
        const glm::mat4 transform = bakeWorldSpaceStaticMesh ? BuildColliderVertexTransform(obj) : glm::mat4(1.0f);
        for (const auto& vertex : vertices) {
            glm::vec3 p = ToGlmVec3(vertex);
            if (bakeWorldSpaceStaticMesh) {
                glm::vec4 world = transform * glm::vec4(p, 1.0f); p = glm::vec3(world);
            }
            else p *= obj.scale;
            boundsMin = glm::min(boundsMin, p);
            boundsMax = glm::max(boundsMax, p);
        }
    }
    const std::vector<float>* GetPrimitiveTriangleStream(RenderType type) {
        static const std::vector<float> cubeVerts(std::begin(vertices), std::end(vertices));
        static const std::vector<float> planeVerts(std::begin(mirrorPlaneVertices), std::end(mirrorPlaneVertices));
        static const std::vector<float> sphereVerts = generateSphere();
        static const std::vector<float> capsuleVerts = generateCapsule();
        static const std::vector<float> torusVerts = generateTorus();
        switch (type) {
            case RenderType::Cube: return &cubeVerts;
            case RenderType::Sphere: return &sphereVerts;
            case RenderType::Capsule: return &capsuleVerts;
            case RenderType::Plane: return &planeVerts;
            case RenderType::Torus: return &torusVerts;
            default: return nullptr;
        }
    }
}
namespace {
    struct IgnoreActorFilter : PxQueryFilterCallback {
        PxRigidActor* ignore = nullptr;
        explicit IgnoreActorFilter(PxRigidActor* actor) : ignore(actor) {}

        PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor, PxHitFlags&) override {
            if (actor == ignore) return PxQueryHitType::eNONE;
            if (shape && shape->getFlags().isSet(PxShapeFlag::eTRIGGER_SHAPE)) return PxQueryHitType::eNONE;
            return PxQueryHitType::eBLOCK;
        }
        PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override {
            return PxQueryHitType::eBLOCK;
        }
    };
}
PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() = default;

bool PhysicsSystem::init() {
    if (isReady()) return true;
    mFoundation = PxCreateFoundation(PX_PHYSICS_VERSION, mAllocator, mErrorCallback);
    if (!mFoundation) return false;
    mPhysics = PxCreatePhysics(PX_PHYSICS_VERSION, *mFoundation, PxTolerancesScale(), true, nullptr);
    if (!mPhysics) return false;
    mDispatcher = PxDefaultCpuDispatcherCreate(2);
    if (!mDispatcher) return false;
    PxTolerancesScale scale = mPhysics->getTolerancesScale();
    mCookParams = PxCookingParams(scale);
    mCookParams.meshPreprocessParams |= PxMeshPreprocessingFlag::eDISABLE_ACTIVE_EDGES_PRECOMPUTE;
    mCookParams.meshPreprocessParams |= PxMeshPreprocessingFlag::eWELD_VERTICES;
    mCookParams.meshWeldTolerance = std::max(0.0001f, 0.001f * scale.length);
    PxSceneDesc sceneDesc(mPhysics->getTolerancesScale());
    sceneDesc.gravity = SceneGravity(mProjectSettings);
    sceneDesc.cpuDispatcher = mDispatcher;
    sceneDesc.filterShader = ModularitySimulationFilterShader;
    mSimulationEvents = std::make_unique<PhysicsSimulationEvents>(*this);
    mPairFilter = std::make_unique<PhysicsPairFilter>(*this);
    sceneDesc.simulationEventCallback = mSimulationEvents.get();
    sceneDesc.filterCallback = mPairFilter.get();
    sceneDesc.flags |= PxSceneFlag::eENABLE_CCD;
    mScene = mPhysics->createScene(sceneDesc);
    if (!mScene) return false;
    mDefaultMaterial = mPhysics->createMaterial(0.9f, 0.9f, 0.0f);
    return mDefaultMaterial != nullptr;
}
bool PhysicsSystem::isReady() const {
    return mFoundation && mPhysics && mScene && mDefaultMaterial;
}
void PhysicsSystem::setProjectSettings(const ProjectPhysicsSettings& settings) {
    mProjectSettings = settings;
    applySceneGravity();
}
void PhysicsSystem::applySceneGravity() {
    if (!mScene) return;
    mScene->setGravity(SceneGravity(mProjectSettings));
}
bool PhysicsSystem::gatherMeshData(const SceneObject& obj, std::vector<PxVec3>& vertices, std::vector<uint32_t>& indices) const {
    auto buildFromPrimitiveStream = [&]() {
        const std::vector<float>* sourceVertices = GetPrimitiveTriangleStream(obj.renderType);
        if (!sourceVertices) return false;
        vertices.clear();
        indices.clear();
        return BuildTriangleMeshFromVertexStream(*sourceVertices, vertices, indices);
    };
    const OBJLoader::LoadedMesh* meshInfo = nullptr;
    if (obj.hasRenderer && obj.renderType == RenderType::OBJMesh && obj.meshId >= 0) {
        meshInfo = g_objLoader.getMeshInfo(obj.meshId);
    } else if (obj.hasRenderer && obj.renderType == RenderType::Model && obj.meshId >= 0) {
        meshInfo = getModelLoader().getMeshInfo(obj.meshId);
    }
    if (!meshInfo) return buildFromPrimitiveStream();
    if (!meshInfo->positions.empty() && meshInfo->triangleIndices.size() >= 3) {
        vertices.reserve(meshInfo->positions.size());
        indices.reserve(meshInfo->triangleIndices.size());
        for (const auto& v : meshInfo->positions) {
            vertices.emplace_back(v.x, v.y, v.z);
        }
        indices.insert(indices.end(), meshInfo->triangleIndices.begin(), meshInfo->triangleIndices.end());
        return !vertices.empty() && (indices.size() % 3 == 0);
    }
    if (meshInfo->triangleVertices.empty()) return false;
    vertices.reserve(meshInfo->triangleVertices.size());
    indices.resize(meshInfo->triangleVertices.size());
    for (size_t i = 0; i < meshInfo->triangleVertices.size(); ++i) {
        const glm::vec3& v = meshInfo->triangleVertices[i];
        vertices.emplace_back(v.x, v.y, v.z);
        indices[i] = static_cast<uint32_t>(i);
    }
    if (!vertices.empty() && (indices.size() % 3 == 0)) return true;
    return buildFromPrimitiveStream();
}
PxTriangleMesh* PhysicsSystem::cookTriangleMesh(const std::vector<PxVec3>& vertices, const std::vector<uint32_t>& indices) const {
    if (vertices.empty() || indices.size() < 3) return nullptr;
    try {
        PxTriangleMeshDesc desc;
        desc.points.count = static_cast<uint32_t>(vertices.size());
        desc.points.stride = sizeof(PxVec3);
        desc.points.data = vertices.data();
        desc.triangles.count = static_cast<uint32_t>(indices.size() / 3);
        desc.triangles.stride = 3 * sizeof(uint32_t);
        desc.triangles.data = indices.data();
        PxDefaultMemoryOutputStream buf;
        if (!PxCookTriangleMesh(mCookParams, desc, buf)) return nullptr;
        PxDefaultMemoryInputData input(buf.getData(), buf.getSize());
        return mPhysics->createTriangleMesh(input);
    }
    catch (const std::bad_alloc&) {return nullptr;}
}
PxConvexMesh* PhysicsSystem::cookConvexMesh(const std::vector<PxVec3>& vertices) const {
    if (vertices.size() < 4) return nullptr;
    try {
        PxConvexMeshDesc desc;
        desc.points.count = static_cast<uint32_t>(vertices.size());
        desc.points.stride = sizeof(PxVec3);
        desc.points.data = vertices.data();
        desc.flags = PxConvexFlag::eCOMPUTE_CONVEX | PxConvexFlag::eCHECK_ZERO_AREA_TRIANGLES;
        desc.vertexLimit = 255;
        PxDefaultMemoryOutputStream buf;
        if (!PxCookConvexMesh(mCookParams, desc, buf)) return nullptr;
        PxDefaultMemoryInputData input(buf.getData(), buf.getSize());
        return mPhysics->createConvexMesh(input);
    }
    catch (const std::bad_alloc&) {return nullptr;}
}
bool PhysicsSystem::attachPrimitiveShape(PxRigidActor* actor, const SceneObject& obj, bool isDynamic) const {
    (void)isDynamic;
    if (!actor) return false;
    bool ownsMaterial = false;
    PxMaterial* shapeMaterial = CreateShapeMaterial(mPhysics, mDefaultMaterial, obj, ownsMaterial);
    PxShape* shape = nullptr;
    auto tuneShape = [](PxShape* s, float minDim, bool /*swept*/) {
        if (!s) return;
        float contact = std::clamp(minDim * 0.005f, 0.0005f, 0.01f);
        s->setContactOffset(contact);
        s->setRestOffset(0.0f);
    };
    switch (obj.renderType) {
        case RenderType::Cube: {
            PxVec3 halfExtents = ToPxVec3(glm::max(obj.scale * 0.5f, glm::vec3(0.01f)));
            shape = mPhysics->createShape(PxBoxGeometry(halfExtents), *shapeMaterial, true);
            tuneShape(shape, std::min({halfExtents.x, halfExtents.y, halfExtents.z}) * 2.0f, isDynamic);
            break;
        }
        case RenderType::Sphere: {
            float radius = std::max({obj.scale.x, obj.scale.y, obj.scale.z}) * 0.5f;
            radius = std::max(radius, 0.01f);
            shape = mPhysics->createShape(PxSphereGeometry(radius), *shapeMaterial, true);
            tuneShape(shape, radius * 2.0f, isDynamic);
            break;
        }
        case RenderType::Capsule: {
        float radius = std::max(obj.scale.x, obj.scale.z) * 0.5f;
        radius = std::max(radius, 0.01f);
        float cylHeight = std::max(0.05f, obj.scale.y - radius * 2.0f);
        float halfHeight = cylHeight * 0.5f;
        shape = mPhysics->createShape(PxCapsuleGeometry(radius, halfHeight), *shapeMaterial, true);
        if (shape) {
            // PhysX capsules default to the X axis; rotate to align with Y (character up)
            shape->setLocalPose(PxTransform(PxQuat(PxHalfPi, PxVec3(0, 0, 1))));
        }
        tuneShape(shape, std::min(radius * 2.0f, halfHeight * 2.0f), isDynamic);
        break;
    }
        case RenderType::Plane: {
            glm::vec3 halfExtents = glm::max(obj.scale * 0.5f, glm::vec3(0.01f));
            halfExtents.z = std::max(halfExtents.z, 0.01f);
            shape = mPhysics->createShape(PxBoxGeometry(ToPxVec3(halfExtents)), *shapeMaterial, true);
            tuneShape(shape, std::min({halfExtents.x, halfExtents.y, halfExtents.z}) * 2.0f, isDynamic);
            break;
        }
        case RenderType::Sprite: {
            glm::vec3 halfExtents = glm::max(obj.scale * 0.5f, glm::vec3(0.01f));
            halfExtents.z = std::max(halfExtents.z, 0.01f);
            shape = mPhysics->createShape(PxBoxGeometry(ToPxVec3(halfExtents)), *shapeMaterial, true);
            tuneShape(shape, std::min({halfExtents.x, halfExtents.y, halfExtents.z}) * 2.0f, isDynamic);
            break;
        }
        case RenderType::Torus: {
            float radius = std::max({obj.scale.x, obj.scale.y, obj.scale.z}) * 0.5f;
            radius = std::max(radius, 0.01f);
            shape = mPhysics->createShape(PxSphereGeometry(radius), *shapeMaterial, true);
            tuneShape(shape, radius * 2.0f, isDynamic);
            break;
        }
        default: break;
    }
    if (ownsMaterial && shapeMaterial) shapeMaterial->release();
    if (!shape) return false;
    if (obj.hasCollider && obj.collider.isTrigger) {
        shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
        shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, true);
    }
    actor->attachShape(*shape);
    shape->release();
    return true;
}

bool PhysicsSystem::attachColliderShape(PxRigidActor* actor, const SceneObject& obj, bool isDynamic, bool bakeWorldSpaceStaticMesh) const {
    if (!actor || !obj.hasCollider || !obj.collider.enabled) return false;
    bool ownsMaterial = false;
    PxMaterial* shapeMaterial = CreateShapeMaterial(mPhysics, mDefaultMaterial, obj, ownsMaterial);
    PxShape* shape = nullptr;
    const PxVec3 localOffset = ToPxVec3(obj.collider.offset);
    auto tuneShape = [](PxShape* s, float minDim, bool /*swept*/) {
        if (!s) return;
        float contact = std::clamp(minDim * 0.005f, 0.0005f, 0.01f);
        s->setContactOffset(contact);
        s->setRestOffset(0.0f);
    };
    float minDim = 0.1f;
    if (obj.collider.type == ColliderType::Box) {
        glm::vec3 half = glm::max(obj.collider.boxSize * 0.5f, glm::vec3(0.01f));
        shape = mPhysics->createShape(PxBoxGeometry(ToPxVec3(half)), *shapeMaterial, true);
        if (shape) {
            shape->setLocalPose(PxTransform(localOffset, PxQuat(PxIdentity)));
        }
        minDim = std::min({half.x, half.y, half.z}) * 2.0f;
    } else if (obj.collider.type == ColliderType::Capsule) {
        float radius = std::max({obj.collider.boxSize.x, obj.collider.boxSize.z}) * 0.5f;
        radius = std::max(radius, 0.01f);
        float cylHeight = std::max(0.05f, obj.collider.boxSize.y - radius * 2.0f);
        float halfHeight = cylHeight * 0.5f;
        shape = mPhysics->createShape(PxCapsuleGeometry(radius, halfHeight), *shapeMaterial, true);
        if (shape) {
            // Rotate capsule so its axis matches the engine's Y-up expectation
            shape->setLocalPose(PxTransform(localOffset, PxQuat(PxHalfPi, PxVec3(0, 0, 1))));
        }
        minDim = std::min(radius * 2.0f, halfHeight * 2.0f);
    } else if (obj.collider.type == ColliderType::Sphere) {
        float radius = std::max({obj.collider.boxSize.x, obj.collider.boxSize.y, obj.collider.boxSize.z}) * 0.5f;
        radius = std::max(radius, 0.01f);
        shape = mPhysics->createShape(PxSphereGeometry(radius), *shapeMaterial, true);
        if (shape) {
            shape->setLocalPose(PxTransform(localOffset, PxQuat(PxIdentity)));
        }
        minDim = radius * 2.0f;
    } else {
        const OBJLoader::LoadedMesh* meshInfo = nullptr;
        if (obj.hasRenderer && obj.renderType == RenderType::OBJMesh && obj.meshId >= 0) {
            meshInfo = g_objLoader.getMeshInfo(obj.meshId);
        } else if (obj.hasRenderer && obj.renderType == RenderType::Model && obj.meshId >= 0) {
            meshInfo = getModelLoader().getMeshInfo(obj.meshId);
        }
        const bool hasIndexed = meshInfo && !meshInfo->positions.empty() && meshInfo->triangleIndices.size() >= 3;
        const bool hasTriVerts = meshInfo && !meshInfo->triangleVertices.empty();
        if (meshInfo && !hasIndexed && !hasTriVerts) return false;
        auto makeBoundsShape = [&](const glm::vec3& boundsMin, const glm::vec3& boundsMax) {
            glm::vec3 halfExtents = glm::max((boundsMax - boundsMin) * 0.5f, glm::vec3(0.01f));
            glm::vec3 center = (boundsMax + boundsMin) * 0.5f;
            PxShape* box = mPhysics->createShape(PxBoxGeometry(ToPxVec3(halfExtents)), *shapeMaterial, true);
            if (box) {
                const PxVec3 poseCenter = bakeWorldSpaceStaticMesh ? ToPxVec3(center) : ToPxVec3(center) + localOffset;
                box->setLocalPose(PxTransform(poseCenter, PxQuat(PxIdentity)));
            }
            return box;
        };

        constexpr size_t kMaxCookVertices = 1000000;
        size_t cookVertices = 0;
        if (meshInfo) cookVertices = hasIndexed ? meshInfo->positions.size() : meshInfo->triangleVertices.size();
        if (cookVertices > kMaxCookVertices) {
            glm::vec3 boundsMin;
            glm::vec3 boundsMax;
            if (hasIndexed) {ComputeColliderBounds(meshInfo->positions, obj, bakeWorldSpaceStaticMesh, boundsMin, boundsMax);}
            else {ComputeColliderBounds(meshInfo->triangleVertices, obj, bakeWorldSpaceStaticMesh, boundsMin, boundsMax);}
            minDim = std::max(0.01f, std::min({boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z}));
            shape = makeBoundsShape(boundsMin, boundsMax);
        } else {
            std::vector<PxVec3> verts;
            std::vector<uint32_t> indices;
            bool hasMeshData = false;
            try {hasMeshData = gatherMeshData(obj, verts, indices);}
            catch (const std::bad_alloc&) {hasMeshData = false;}
            if (!hasMeshData) {
                if (!meshInfo) return false;
                glm::vec3 boundsMin;
                glm::vec3 boundsMax;
                if (hasIndexed) {ComputeColliderBounds(meshInfo->positions, obj, bakeWorldSpaceStaticMesh, boundsMin, boundsMax);}
                else {ComputeColliderBounds(meshInfo->triangleVertices, obj, bakeWorldSpaceStaticMesh, boundsMin, boundsMax);}
                minDim = std::max(0.01f, std::min({boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z}));
                shape = makeBoundsShape(boundsMin, boundsMax);
            } else {
                bool useConvex = isDynamic || obj.collider.type == ColliderType::ConvexMesh || obj.collider.isTrigger;
                if (bakeWorldSpaceStaticMesh) {BakeTransformIntoVertices(verts, obj);}
                else {BakeScaleIntoVertices(verts, obj.scale);}
                glm::vec3 boundsMin;
                glm::vec3 boundsMax;
                ComputeColliderBounds(verts, obj, false, boundsMin, boundsMax);
                minDim = std::max(0.01f, std::min({boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z}));
                if (useConvex) {
                    PxConvexMesh* convex = cookConvexMesh(verts);
                    if (convex) {
                        PxConvexMeshGeometry geom(convex);
                        shape = mPhysics->createShape(geom, *shapeMaterial, true);
                        if (shape) {
                            shape->setLocalPose(PxTransform(bakeWorldSpaceStaticMesh ? PxVec3(0.0f) : localOffset, PxQuat(PxIdentity)));
                        }
                        convex->release();
                    }
                } else {
                    PxTriangleMesh* tri = cookTriangleMesh(verts, indices);
                    if (tri) {
                        PxTriangleMeshGeometry geom(tri);
                        shape = mPhysics->createShape(geom, *shapeMaterial, true);
                        if (shape) {
                            shape->setLocalPose(PxTransform(bakeWorldSpaceStaticMesh ? PxVec3(0.0f) : localOffset, PxQuat(PxIdentity)));
                        }
                        tri->release();
                    }
                }
                if (!shape) shape = makeBoundsShape(boundsMin, boundsMax);
            }
        }
    }
    tuneShape(shape, std::max(0.01f, minDim), isDynamic || obj.hasPlayerController);
    if (ownsMaterial && shapeMaterial) shapeMaterial->release();
    if (!shape) return false;
    if (obj.collider.isTrigger) {
        shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
        shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, true);
    }
    actor->attachShape(*shape);
    shape->release();
    return true;
}

PhysicsSystem::ActorRecord PhysicsSystem::createActorFor(const SceneObject& obj) const {
    ActorRecord record;
    const bool wantsDynamic = obj.hasRigidbody && obj.rigidbody.enabled;
    const bool wantsCollider = obj.hasCollider && obj.collider.enabled;
    const bool bakeWorldSpaceStaticMesh = !wantsDynamic && wantsCollider && obj.collider.type == ColliderType::Mesh;
    if (!wantsDynamic && !wantsCollider) return record;
    PxTransform transform = bakeWorldSpaceStaticMesh
        ? PxTransform(PxIdentity) : PxTransform(ToPxVec3(obj.position), ToPxQuat(obj.rotation));
    PxRigidActor* actor = wantsDynamic
        ? static_cast<PxRigidActor*>(mPhysics->createRigidDynamic(transform)) : static_cast<PxRigidActor*>(mPhysics->createRigidStatic(transform));
    if (!actor) return record;
    record.actor = actor;
    actor->userData = reinterpret_cast<void*>(static_cast<intptr_t>(obj.id) + 1);
    record.isDynamic = wantsDynamic;
    const bool transformLocked = obj.rigidbody.isKinematic || obj.IsInvariable;
    record.isKinematic = wantsDynamic && transformLocked;
    record.hasWorldBakedStaticMesh = bakeWorldSpaceStaticMesh;
    record.hasAuthoringPose = true;
    record.lastAuthoringPosition = obj.position;
    record.lastAuthoringRotation = obj.rotation;
    bool attached = false;
    if (wantsCollider) attached = attachColliderShape(actor, obj, wantsDynamic, bakeWorldSpaceStaticMesh);
    if (!attached) attached = attachPrimitiveShape(actor, obj, wantsDynamic);
    if (!attached) {
        actor->release(); record.actor = nullptr; return record;
    }
    if (PxRigidDynamic* dyn = actor->is<PxRigidDynamic>()) {
        dyn->setAngularDamping(obj.rigidbody.angularDamping);
        dyn->setLinearDamping(obj.rigidbody.linearDamping);
        dyn->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, transformLocked);
        dyn->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !obj.rigidbody.useGravity);
        PxRigidDynamicLockFlags lockFlags = BuildAngularLockFlags(obj);
        dyn->setRigidDynamicLockFlags(lockFlags);
        if (obj.hasPlayerController) {
            dyn->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
            dyn->setMaxDepenetrationVelocity(1.5f);
        }
        if (!transformLocked) {
            const float massKg = EffectiveMassKilograms(obj, mProjectSettings);
            const PxVec3 com = ToPxVec3(obj.rigidbody.centerOfMass);
            const PxVec3* comPtr = obj.rigidbody.useCustomCenterOfMass ? &com : nullptr;
            PxRigidBodyExt::setMassAndUpdateInertia(*dyn, massKg, comPtr);
            record.massKg = massKg;
            record.useCustomCenterOfMass = obj.rigidbody.useCustomCenterOfMass;
            record.centerOfMass = obj.rigidbody.centerOfMass;
        }
        record.gravityDisabled = !obj.rigidbody.useGravity;
        record.linearDamping = obj.rigidbody.linearDamping;
        record.angularDamping = obj.rigidbody.angularDamping;
        record.angularLockMask = BuildAngularLockMask(obj);
    }
    record.isTrigger = wantsCollider && obj.collider.isTrigger;
    return record;
}
void PhysicsSystem::clearActors() {
    for (auto& [id, rec] : mActors) {
        if (rec.actor && mScene) {
            mScene->removeActor(*rec.actor); rec.actor->release();
        }
    }
    mActors.clear();
    mActorIdsByPtr.clear();
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    mPendingCollisionEvents.clear();
    mActiveCollisionPairs.clear();
    mIgnoredCollisionPairs.clear();
}
void PhysicsSystem::onPlayStart(const std::vector<SceneObject>& objects) {
    if (!isReady()) return;
    clearActors();
    mActors.reserve(objects.size());
    mActorIdsByPtr.reserve(objects.size());
    for (const auto& obj : objects) {
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        ActorRecord rec = createActorFor(obj);
        if (!rec.actor) continue;
        mScene->addActor(*rec.actor);
        mActors[obj.id] = rec;
        mActorIdsByPtr[rec.actor] = obj.id;
    }
}
void PhysicsSystem::onPlayStop() {clearActors();}
bool PhysicsSystem::setLinearVelocity(int id, const glm::vec3& velocity) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic) return false;
    if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        // Re-enable simulation in case the actor was put to sleep / disabled
        // while its parent or component was off, then force-wake so the
        // velocity actually takes effect on the next step.
        dyn->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, false);
        dyn->setLinearVelocity(ToPxVec3(velocity));
        if (!dyn->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC)) {
            dyn->wakeUp();
        }
        return true;
    }
#endif
    return false;
}
bool PhysicsSystem::setAngularVelocity(int id, const glm::vec3& velocity) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic || rec.isKinematic) return false;
    if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        dyn->setAngularVelocity(ToPxVec3(velocity));
        return true;
    }
#endif
    return false;
}
bool PhysicsSystem::setContinuousCollisionDetection(int id, bool enabled) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic || rec.isKinematic) return false;
    if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        dyn->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, enabled);
        return true;
    }
#endif
    return false;
}
bool PhysicsSystem::setActorYaw(int id, float yawDegrees) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor) return false;
    PxTransform pose = rec.actor->getGlobalPose();
    PxQuat yawQuat(static_cast<float>(glm::radians(yawDegrees)), PxVec3(0, 1, 0));
    pose.q = yawQuat;
    rec.actor->setGlobalPose(pose);
    rec.hasAuthoringPose = true;
    rec.lastAuthoringPosition = ToGlmVec3(pose.p);
    rec.lastAuthoringRotation = ToGlmEulerDeg(pose.q);
    return true;
#endif
    return false;
}
bool PhysicsSystem::getLinearVelocity(int id, glm::vec3& outVelocity) const {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    const ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic) return false;
    if (const PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        PxVec3 v = dyn->getLinearVelocity();
        outVelocity = glm::vec3(v.x, v.y, v.z);
        return true;
    }
#endif
    return false;
}
bool PhysicsSystem::getAngularVelocity(int id, glm::vec3& outVelocity) const {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    const ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic || rec.isKinematic) return false;
    if (const PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        PxVec3 v = dyn->getAngularVelocity();
        outVelocity = glm::vec3(v.x, v.y, v.z); return true;
    }
#endif
    return false;
}
bool PhysicsSystem::getActorPose(int id, glm::vec3& outPosition,
                                 glm::vec3& outRotationDeg) const {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    const ActorRecord& rec = it->second;
    if (!rec.actor) return false;
    const PxTransform pose = rec.actor->getGlobalPose();
    outPosition = ToGlmVec3(pose.p);
    outRotationDeg = ToGlmEulerDeg(pose.q);
    return true;
#else
    (void)id;
    (void)outPosition;
    (void)outRotationDeg;
    return false;
#endif
}
bool PhysicsSystem::setActorPose(int id, const glm::vec3& position, const glm::vec3& rotationDeg) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor) return false;
    PxTransform pose(ToPxVec3(position), ToPxQuat(rotationDeg));
    rec.actor->setGlobalPose(pose);
    rec.hasAuthoringPose = true;
    rec.lastAuthoringPosition = position;
    rec.lastAuthoringRotation = rotationDeg;
    return true;
#else
    (void)id; (void)position; (void)rotationDeg;
    return false;
#endif
}
bool PhysicsSystem::addForce(int id, const glm::vec3& force) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic || rec.isKinematic) return false;
    if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        dyn->addForce(ToPxVec3(force), PxForceMode::eFORCE); return true;
    }
#endif
    return false;
}
bool PhysicsSystem::addImpulse(int id, const glm::vec3& impulse) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic || rec.isKinematic) return false;
    if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        dyn->addForce(ToPxVec3(impulse), PxForceMode::eIMPULSE);
        return true;
    }
#endif
    return false;
}
bool PhysicsSystem::addTorque(int id, const glm::vec3& torque) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic || rec.isKinematic) return false;
    if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        dyn->addTorque(ToPxVec3(torque), PxForceMode::eFORCE);
        return true;
    }
#endif
    return false;
}
bool PhysicsSystem::addAngularImpulse(int id, const glm::vec3& impulse) {
#ifdef MODULARITY_ENABLE_PHYSX
    auto it = mActors.find(id);
    if (it == mActors.end()) return false;
    ActorRecord& rec = it->second;
    if (!rec.actor || !rec.isDynamic || rec.isKinematic) return false;
    if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
        dyn->addTorque(ToPxVec3(impulse), PxForceMode::eIMPULSE);
        return true;
    }
#endif
    return false;
}
bool PhysicsSystem::refreshObject(const SceneObject& obj) {
#ifdef MODULARITY_ENABLE_PHYSX
    if (!isReady()) return false;
    auto existing = mActors.find(obj.id);
    if (existing == mActors.end()) {
        return true;
    }

    glm::vec3 linearVelocity(0.0f);
    glm::vec3 angularVelocity(0.0f);
    const bool hadLinearVelocity = getLinearVelocity(obj.id, linearVelocity);
    const bool hadAngularVelocity = getAngularVelocity(obj.id, angularVelocity);

    endCollisionPairsForObject(obj.id);
    ActorRecord& oldRecord = existing->second;
    if (oldRecord.actor) {
        mActorIdsByPtr.erase(oldRecord.actor);
        if (mScene) mScene->removeActor(*oldRecord.actor);
        oldRecord.actor->release();
    }
    mActors.erase(existing);

    ActorRecord refreshed = createActorFor(obj);
    if (!refreshed.actor) return false;
    mScene->addActor(*refreshed.actor);
    mActorIdsByPtr[refreshed.actor] = obj.id;
    mActors[obj.id] = refreshed;
    if (hadLinearVelocity) setLinearVelocity(obj.id, linearVelocity);
    if (hadAngularVelocity) setAngularVelocity(obj.id, angularVelocity);
    return true;
#else
    (void)obj;
    return false;
#endif
}
bool PhysicsSystem::raycastClosest(const glm::vec3& origin, const glm::vec3& dir, float distance,
                                   int ignoreId, glm::vec3* hitPos, glm::vec3* hitNormal, float* hitDistance,
                                   int* hitActorId, glm::vec3* hitActorVelocity, float* hitStaticFriction, float* hitDynamicFriction) const {
#ifdef MODULARITY_ENABLE_PHYSX
    if (!isReady() || distance <= 0.0f) return false;
    if (hitActorId) *hitActorId = -1;
    if (hitActorVelocity) *hitActorVelocity = glm::vec3(0.0f);
    if (hitStaticFriction) *hitStaticFriction = 0.9f;
    if (hitDynamicFriction) *hitDynamicFriction = 0.9f;
    PxVec3 unitDir = ToPxVec3(glm::normalize(dir));
    if (!unitDir.isFinite()) return false;
    PxRaycastBuffer hit;
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
    IgnoreActorFilter cb(nullptr);
    auto it = mActors.find(ignoreId);
    if (it != mActors.end()) {
        cb.ignore = it->second.actor;
    }
    bool result = mScene->raycast(ToPxVec3(origin), unitDir, distance, hit, PxHitFlag::ePOSITION | PxHitFlag::eNORMAL,
                                  fd, cb.ignore ? &cb : nullptr);
    if (!result || !hit.hasBlock) return false;
    if (hitPos) *hitPos = ToGlmVec3(hit.block.position);
    if (hitNormal) *hitNormal = ToGlmVec3(hit.block.normal);
    if (hitDistance) *hitDistance = hit.block.distance;
    if (hitStaticFriction || hitDynamicFriction) {
        PxShape* shape = hit.block.shape;
        if (shape) {
            PxMaterial* materials[1] = {nullptr};
            PxU16 count = shape->getMaterials(materials, 1);
            if (count > 0 && materials[0]) {
                if (hitStaticFriction) *hitStaticFriction = materials[0]->getStaticFriction();
                if (hitDynamicFriction) *hitDynamicFriction = materials[0]->getDynamicFriction();
            }
        }
    }
    if (hitActorId || hitActorVelocity) {
        PxRigidActor* hitActor = hit.block.actor;
        if (hitActor) {
            auto idIt = mActorIdsByPtr.find(hitActor);
            if (idIt != mActorIdsByPtr.end()) {
                if (hitActorId) *hitActorId = idIt->second;
                auto actorIt = mActors.find(idIt->second);
                if (hitActorVelocity && actorIt != mActors.end() &&
                    actorIt->second.isDynamic) {
                    if (const PxRigidDynamic* dyn = actorIt->second.actor->is<PxRigidDynamic>()) {*hitActorVelocity = ToGlmVec3(dyn->getLinearVelocity());}
                }
            }
        }
    }
    return true;
#else
    (void)origin; (void)dir; (void)distance; (void)ignoreId; (void)hitPos; (void)hitNormal; (void)hitDistance;
    (void)hitActorId; (void)hitActorVelocity; (void)hitStaticFriction; (void)hitDynamicFriction;
    return false;
#endif
}
void PhysicsSystem::simulate(float deltaTime, std::vector<SceneObject>& objects) {
    if (!isReady() || deltaTime <= 0.0f) return;
    std::unordered_map<int, SceneObject*> objectsById;
    objectsById.reserve(objects.size());
    for (SceneObject& obj : objects) {objectsById[obj.id] = &obj;}

    // Sync actors to authoring transforms before stepping.
    // Zeroth pass: any SceneObject that just became hierarchyEnabled mid-play
    // (e.g. a parent group flipped on, or a script toggled the object back on)
    for (SceneObject& obj : objects) {
        if (mActors.find(obj.id) != mActors.end()) continue;
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        const bool wantsDyn = obj.hasRigidbody && obj.rigidbody.enabled;
        const bool wantsCol = obj.hasCollider && obj.collider.enabled;
        if (!wantsDyn && !wantsCol) continue;
        ActorRecord rec = createActorFor(obj);
        if (!rec.actor) continue;
        mScene->addActor(*rec.actor);
        mActors[obj.id] = rec;
        mActorIdsByPtr[rec.actor] = obj.id;
    }

    // First pass: detect actors whose dynamic/static type no longer matches the
    // SceneObject's current rigidbody.enabled state (e.g. a rigidbody toggled
    // on after the actor was first spawned as static). PhysX actor identity
    // can't change in place, so we release and re-create.
    {
        std::vector<int> idsToRecreate;
        for (auto& [id, rec] : mActors) {
            if (!rec.actor) continue;
            auto objIt = objectsById.find(id);
            if (objIt == objectsById.end()) continue;
            const SceneObject& obj = *objIt->second;
            const bool wantsDynamicNow = obj.hasRigidbody && obj.rigidbody.enabled;
            const bool wantsTriggerNow = obj.hasCollider && obj.collider.enabled && obj.collider.isTrigger;
            if (rec.isDynamic != wantsDynamicNow || rec.isTrigger != wantsTriggerNow) {
                idsToRecreate.push_back(id);
            }
        }
        for (int id : idsToRecreate) {
            auto recIt = mActors.find(id);
            if (recIt == mActors.end()) continue;
            ActorRecord& rec = recIt->second;
            endCollisionPairsForObject(id);
            if (rec.actor) {
                mActorIdsByPtr.erase(rec.actor);
                if (mScene) mScene->removeActor(*rec.actor);
                rec.actor->release();
                rec.actor = nullptr;
            }
            const SceneObject& obj = *objectsById[id];
            ActorRecord fresh = createActorFor(obj);
            if (fresh.actor) {
                mScene->addActor(*fresh.actor);
                mActorIdsByPtr[fresh.actor] = id;
                recIt->second = fresh;
            }
            else mActors.erase(recIt);
        }
    }
    for (auto& [id, rec] : mActors) {
        if (!rec.actor) continue;
        auto objIt = objectsById.find(id);
        if (objIt == objectsById.end()) continue;
        SceneObject& obj = *objIt->second;
        const bool enabledInHierarchy = IsObjectEnabledInHierarchy(obj);
        if (enabledInHierarchy == rec.simulationDisabled) {
            rec.actor->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, !enabledInHierarchy);
            rec.simulationDisabled = !enabledInHierarchy;
        }
        if (!enabledInHierarchy) continue;
        if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
            const bool isKinematic = obj.rigidbody.isKinematic || obj.IsInvariable;
            if (rec.isKinematic != isKinematic) {
                if (isKinematic) {
                    dyn->setLinearVelocity(PxVec3(0.0f));
                    dyn->setAngularVelocity(PxVec3(0.0f));
                }
                dyn->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, isKinematic);
                rec.isKinematic = isKinematic;
            }
            const bool gravityDisabled = !obj.rigidbody.useGravity;
            if (rec.gravityDisabled != gravityDisabled) {
                dyn->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, gravityDisabled);
                rec.gravityDisabled = gravityDisabled;
            }
            if (std::abs(rec.linearDamping - obj.rigidbody.linearDamping) > 0.0001f) {
                dyn->setLinearDamping(obj.rigidbody.linearDamping);
                rec.linearDamping = obj.rigidbody.linearDamping;
            }
            if (std::abs(rec.angularDamping - obj.rigidbody.angularDamping) > 0.0001f) {
                dyn->setAngularDamping(obj.rigidbody.angularDamping);
                rec.angularDamping = obj.rigidbody.angularDamping;
            }
            const uint32_t angularLockMask = BuildAngularLockMask(obj);
            if (rec.angularLockMask != angularLockMask) {
                dyn->setRigidDynamicLockFlags(BuildAngularLockFlags(obj));
                rec.angularLockMask = angularLockMask;
            }
            const float massKg = EffectiveMassKilograms(obj, mProjectSettings);
            const bool useCustomCenterOfMass = obj.rigidbody.useCustomCenterOfMass;
            const glm::vec3 centerOfMassDelta = rec.centerOfMass - obj.rigidbody.centerOfMass;
            const bool centerOfMassChanged = glm::dot(centerOfMassDelta, centerOfMassDelta) > 0.000001f;
            const bool massChanged = std::abs(rec.massKg - massKg) > 0.0001f;
            const bool customCenterStateChanged = rec.useCustomCenterOfMass != useCustomCenterOfMass;
            if (!isKinematic && (massChanged || centerOfMassChanged || customCenterStateChanged)) {
                const PxVec3 com = ToPxVec3(obj.rigidbody.centerOfMass);
                const PxVec3* comPtr = useCustomCenterOfMass ? &com : nullptr;
                PxRigidBodyExt::setMassAndUpdateInertia(*dyn, massKg, comPtr);
                rec.massKg = massKg;
                rec.useCustomCenterOfMass = useCustomCenterOfMass;
                rec.centerOfMass = obj.rigidbody.centerOfMass;
            }
            if (rec.isKinematic) {
                const bool poseChanged =
                    !rec.hasAuthoringPose ||
                    PoseChangedSignificantly(rec.lastAuthoringPosition, rec.lastAuthoringRotation, obj.position, obj.rotation);
                if (poseChanged) {
                    dyn->setKinematicTarget(PxTransform(ToPxVec3(obj.position), ToPxQuat(obj.rotation)));
                    rec.hasAuthoringPose = true;
                    rec.lastAuthoringPosition = obj.position;
                    rec.lastAuthoringRotation = obj.rotation;
                }
            }
        } else {
            if (rec.hasWorldBakedStaticMesh) continue;
            const bool poseChanged =
                !rec.hasAuthoringPose ||
                PoseChangedSignificantly(rec.lastAuthoringPosition, rec.lastAuthoringRotation, obj.position, obj.rotation);
            if (poseChanged) {
                // Static actors follow their authoring transform so scripted
                // moves/rotations take effect without a full actor rebuild.
                rec.actor->setGlobalPose(PxTransform(ToPxVec3(obj.position), ToPxQuat(obj.rotation)));
                rec.hasAuthoringPose = true;
                rec.lastAuthoringPosition = obj.position;
                rec.lastAuthoringRotation = obj.rotation;
            }
        }
    }
    mScene->simulate(deltaTime);
    mScene->fetchResults(true);
    for (auto& [id, rec] : mActors) {
        if (!rec.actor || !rec.isDynamic || rec.isKinematic) continue;
        PxTransform pose = rec.actor->getGlobalPose();
        auto objIt = objectsById.find(id);
        if (objIt == objectsById.end() || !IsObjectEnabledInHierarchy(*objIt->second)) continue;
        SceneObject& obj = *objIt->second;
        if (std::clamp(obj.rigidbody.interpolationMode, 0, 2) == 2) {
            if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
                const float predictionTime = std::min(
                    std::max(0.0f, deltaTime),
                    std::max(0.0f, obj.rigidbody.maxExtrapolationTime));
                pose.p += dyn->getLinearVelocity() * predictionTime;
                const PxVec3 angularVelocity = dyn->getAngularVelocity();
                const float angularSpeed = angularVelocity.magnitude();
                if (angularSpeed > 1.0e-5f && predictionTime > 0.0f) {
                    pose.q = (PxQuat(angularSpeed * predictionTime,
                                     angularVelocity / angularSpeed) * pose.q).getNormalized();
                }
            }
        }
        obj.position = ToGlmVec3(pose.p);
        if (obj.hasPlayerController && obj.playerController.enabled) continue;
        glm::vec3 euler = ToGlmEulerDeg(pose.q);
        if (PxRigidDynamic* dyn = rec.actor->is<PxRigidDynamic>()) {
            PxRigidDynamicLockFlags lockFlags = dyn->getRigidDynamicLockFlags();
            bool lockX = lockFlags.isSet(PxRigidDynamicLockFlag::eLOCK_ANGULAR_X);
            bool lockY = lockFlags.isSet(PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y);
            bool lockZ = lockFlags.isSet(PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z);
            if (lockX && lockZ && !lockY) obj.rotation.y = euler.y;
            else obj.rotation = euler;
        }
        else obj.rotation.y = euler.y;
    }
}

void PhysicsSystem::queueCollisionEvent(int objectAId, int objectBId, PhysicsCollisionPhase phase) {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return;
    const uint64_t key = CollisionPairKey(objectAId, objectBId);
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    if (phase == PhysicsCollisionPhase::Enter) {
        if (++mActiveCollisionPairs[key].manifolds != 1) return;
    } else if (phase == PhysicsCollisionPhase::Exit) {
        auto it = mActiveCollisionPairs.find(key);
        if (it == mActiveCollisionPairs.end()) return;
        if (--it->second.manifolds > 0) return;
        mActiveCollisionPairs.erase(it);
    }
    mPendingCollisionEvents.push_back({objectAId, objectBId, phase});
}

void PhysicsSystem::endCollisionPairsForObject(int objectId) {
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    for (auto it = mActiveCollisionPairs.begin(); it != mActiveCollisionPairs.end();) {
        const int a = static_cast<int>(it->first >> 32u);
        const int b = static_cast<int>(it->first & 0xffffffffu);
        if (a == objectId || b == objectId) {
            mPendingCollisionEvents.push_back({a, b, PhysicsCollisionPhase::Exit});
            it = mActiveCollisionPairs.erase(it);
        } else {
            ++it;
        }
    }
}

bool PhysicsSystem::areObjectsTouching(int objectAId, int objectBId) const {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return false;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    return mActiveCollisionPairs.find(CollisionPairKey(objectAId, objectBId)) !=
           mActiveCollisionPairs.end();
}

bool PhysicsSystem::isCollisionPairIgnored(int objectAId, int objectBId) const {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return false;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    return mIgnoredCollisionPairs.find(CollisionPairKey(objectAId, objectBId)) !=
           mIgnoredCollisionPairs.end();
}

bool PhysicsSystem::setObjectsCollisionIgnored(int objectAId, int objectBId, bool ignored) {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return false;
    const uint64_t key = CollisionPairKey(objectAId, objectBId);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mCollisionEventMutex);
        if (ignored) {
            changed = mIgnoredCollisionPairs.insert(key).second;
            if (mActiveCollisionPairs.erase(key) != 0) {
                mPendingCollisionEvents.push_back({objectAId, objectBId,
                                                   PhysicsCollisionPhase::Exit});
            }
        } else {
            changed = mIgnoredCollisionPairs.erase(key) != 0;
        }
    }

    if (!changed || !mScene) return true;

    auto actorIt = mActors.find(objectAId);
    if (actorIt == mActors.end()) actorIt = mActors.find(objectBId);
    if (actorIt != mActors.end() && actorIt->second.actor) {
        mScene->resetFiltering(*actorIt->second.actor);
    }
    return true;
}

void PhysicsSystem::noteContactNormal(int objectAId, int objectBId,
                                      const glm::vec3& lowToHighNormal) {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    auto it = mActiveCollisionPairs.find(CollisionPairKey(objectAId, objectBId));
    if (it == mActiveCollisionPairs.end()) return;
    it->second.normal = lowToHighNormal;
}

bool PhysicsSystem::getContactNormal(int objectId, glm::vec3& outNormal) const {
    if (objectId < 0) return false;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    glm::vec3 sum(0.0f);
    for (const auto& [key, state] : mActiveCollisionPairs) {
        const int low = static_cast<int>(key >> 32u);
        const int high = static_cast<int>(key & 0xffffffffu);
        if (low != objectId && high != objectId) continue;
        sum += (objectId == high) ? state.normal : -state.normal;
    }
    const float length = glm::length(sum);
    if (length < 0.0001f) return false;
    outNormal = sum / length;
    return true;
}

int PhysicsSystem::countActiveContacts(int objectId) const {
    if (objectId < 0) return 0;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    int count = 0;
    for (const auto& [key, state] : mActiveCollisionPairs) {
        const int a = static_cast<int>(key >> 32u);
        const int b = static_cast<int>(key & 0xffffffffu);
        if (a == objectId || b == objectId) ++count;
    }
    return count;
}

void PhysicsSystem::drainCollisionEvents(std::vector<PhysicsCollisionEvent>& outEvents) {
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    outEvents.insert(outEvents.end(), mPendingCollisionEvents.begin(), mPendingCollisionEvents.end());
    std::unordered_set<uint64_t> changedPairs;
    changedPairs.reserve(mPendingCollisionEvents.size());
    for (const PhysicsCollisionEvent& event : mPendingCollisionEvents) {
        changedPairs.insert(CollisionPairKey(event.objectAId, event.objectBId));
    }
    for (const auto& [key, state] : mActiveCollisionPairs) {
        if (changedPairs.find(key) != changedPairs.end()) continue;
        outEvents.push_back({static_cast<int>(key >> 32u),
                             static_cast<int>(key & 0xffffffffu),
                             PhysicsCollisionPhase::Stay});
    }
    mPendingCollisionEvents.clear();
}

void PhysicsSystem::shutdown() {
    clearActors();
    if (mScene) { mScene->release(); mScene = nullptr; }
    if (mDispatcher) { mDispatcher->release(); mDispatcher = nullptr; }
    if (mPhysics) { mPhysics->release(); mPhysics = nullptr; }
    if (mFoundation) { mFoundation->release(); mFoundation = nullptr; }
    mDefaultMaterial = nullptr;
    mSimulationEvents.reset();
    mPairFilter.reset();
}

#else // MODULARITY_ENABLE_PHYSX
PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() = default;
bool PhysicsSystem::init() { return false; }
void PhysicsSystem::shutdown() {}
bool PhysicsSystem::isReady() const { return false; }
void PhysicsSystem::setProjectSettings(const ProjectPhysicsSettings&) {}
bool PhysicsSystem::setLinearVelocity(int, const glm::vec3&) { return false; }
bool PhysicsSystem::setAngularVelocity(int, const glm::vec3&) { return false; }
bool PhysicsSystem::setContinuousCollisionDetection(int, bool) { return false; }
bool PhysicsSystem::setActorYaw(int, float) { return false; }
bool PhysicsSystem::getLinearVelocity(int, glm::vec3&) const { return false; }
bool PhysicsSystem::getAngularVelocity(int, glm::vec3&) const { return false; }
bool PhysicsSystem::getActorPose(int, glm::vec3&, glm::vec3&) const { return false; }
bool PhysicsSystem::setActorPose(int, const glm::vec3&, const glm::vec3&) { return false; }
bool PhysicsSystem::addForce(int, const glm::vec3&) { return false; }
bool PhysicsSystem::addImpulse(int, const glm::vec3&) { return false; }
bool PhysicsSystem::addTorque(int, const glm::vec3&) { return false; }
bool PhysicsSystem::addAngularImpulse(int, const glm::vec3&) { return false; }
bool PhysicsSystem::refreshObject(const SceneObject&) { return false; }
bool PhysicsSystem::raycastClosest(const glm::vec3&, const glm::vec3&, float, int,
                                   glm::vec3*, glm::vec3*, float*, int*,
                                   glm::vec3*, float*, float*) const { return false; }
void PhysicsSystem::onPlayStart(const std::vector<SceneObject>&) {}
void PhysicsSystem::onPlayStop() {}
void PhysicsSystem::simulate(float, std::vector<SceneObject>&) {}
void PhysicsSystem::drainCollisionEvents(std::vector<PhysicsCollisionEvent>&) {}
#endif
