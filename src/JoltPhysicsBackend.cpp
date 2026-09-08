#include "JoltPhysicsBackend.h"
#ifdef MODULARITY_ENABLE_JOLT
#include "ModelLoader.h"
#include "Rendering.h"
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <iostream>
#include <new>

class JoltContactEvents final : public JPH::ContactListener {
public:
    explicit JoltContactEvents(JoltPhysicsBackend& owner) : mOwner(owner) {}

    JPH::ValidateResult OnContactValidate(
            const JPH::Body& body1, const JPH::Body& body2,
            JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
        if (mOwner.isCollisionPairIgnored(objectId(body1), objectId(body2))) {
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        mOwner.queueCollisionEvent(objectId(body1), objectId(body2), PhysicsCollisionPhase::Enter);
        recordNormal(body1, body2, manifold);
    }

    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        recordNormal(body1, body2, manifold);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        const auto aIt = mOwner.mIdsByBody.find(pair.GetBody1ID().GetIndexAndSequenceNumber());
        const auto bIt = mOwner.mIdsByBody.find(pair.GetBody2ID().GetIndexAndSequenceNumber());
        if (aIt != mOwner.mIdsByBody.end() && bIt != mOwner.mIdsByBody.end()) {
            mOwner.queueCollisionEvent(aIt->second, bIt->second, PhysicsCollisionPhase::Exit);
        }
    }

private:
    void recordNormal(const JPH::Body& body1, const JPH::Body& body2,
                      const JPH::ContactManifold& manifold) {
        const int idA = objectId(body1);
        const int idB = objectId(body2);
        if (idA < 0 || idB < 0 || idA == idB) return;
        const JPH::Vec3 centre1 = body1.GetCenterOfMassPosition();
        const JPH::Vec3 centre2 = body2.GetCenterOfMassPosition();
        glm::vec3 lowToHigh(centre2.GetX() - centre1.GetX(),
                            centre2.GetY() - centre1.GetY(),
                            centre2.GetZ() - centre1.GetZ());
        if (idA > idB) lowToHigh = -lowToHigh;
        const JPH::Vec3 n = manifold.mWorldSpaceNormal;
        glm::vec3 normal(n.GetX(), n.GetY(), n.GetZ());
        const float lengthSq = glm::dot(normal, normal);
        if (lengthSq < 0.000001f) return;
        normal /= std::sqrt(lengthSq);
        if (glm::dot(normal, lowToHigh) < 0.0f) normal = -normal;
        mOwner.noteContactNormal(idA, idB, normal);
    }

    static int objectId(const JPH::Body& body) {
        const uint64_t encoded = body.GetUserData();
        return encoded == 0 ? -1 : static_cast<int>(encoded - 1u);
    }
    JoltPhysicsBackend& mOwner;
};

namespace {
    int g_joltGlobalsRefcount = 0;

    uint64_t JoltCollisionPairKey(int a, int b) {
        const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
        const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
        return (static_cast<uint64_t>(lo) << 32u) | static_cast<uint64_t>(hi);
    }

    void EnsureJoltGlobals() {
        if (g_joltGlobalsRefcount++ != 0) return;
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    void ReleaseJoltGlobals() {
        if (--g_joltGlobalsRefcount != 0) return;
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

}

namespace JoltLayers {
    constexpr JPH::ObjectLayer NON_MOVING = 0;
    constexpr JPH::ObjectLayer MOVING = 1;
    constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

namespace JoltBPLayers {
    constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    constexpr JPH::BroadPhaseLayer MOVING(1);
    constexpr JPH::uint NUM_LAYERS = 2;
}

class JoltBroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JoltBroadPhaseLayerInterface() {
        mObjectToBroadPhase[JoltLayers::NON_MOVING] = JoltBPLayers::NON_MOVING;
        mObjectToBroadPhase[JoltLayers::MOVING] = JoltBPLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return JoltBPLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return mObjectToBroadPhase[inLayer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        if (inLayer == JoltBPLayers::NON_MOVING) return "NON_MOVING";
        if (inLayer == JoltBPLayers::MOVING) return "MOVING";
        return "INVALID";
    }
#endif
private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[JoltLayers::NUM_LAYERS];
};

class JoltObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
            case JoltLayers::NON_MOVING: return inLayer2 == JoltBPLayers::MOVING;
            case JoltLayers::MOVING: return true;
            default: return false;
        }
    }
};

class JoltObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        switch (a) {
            case JoltLayers::NON_MOVING: return b == JoltLayers::MOVING;
            case JoltLayers::MOVING: return true;
            default: return false;
        }
    }
};

struct JoltPhysicsBackend::JoltFilters {
    JoltBroadPhaseLayerInterface bpInterface;
    JoltObjectVsBroadPhaseLayerFilter objVsBpFilter;
    JoltObjectLayerPairFilter objLayerPairFilter;
};

namespace {
    using namespace JPH;

    Vec3 ToJVec3(const glm::vec3& v) { return Vec3(v.x, v.y, v.z); }
    glm::vec3 FromJVec3(Vec3Arg v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }

    Quat ToJQuat(const glm::vec3& eulerDeg) {
        glm::vec3 r = glm::radians(eulerDeg);
        glm::mat4 m(1.0f);
        m = glm::rotate(m, r.x, glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, r.y, glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, r.z, glm::vec3(0.0f, 0.0f, 1.0f));
        glm::quat q = glm::quat_cast(glm::mat3(m));
        return Quat(q.x, q.y, q.z, q.w);
    }

    glm::vec3 ExtractEulerXYZ(const glm::mat3& m) {
        float T1 = std::atan2(m[2][1], m[2][2]);
        float C2 = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
        float T2 = std::atan2(-m[2][0], C2);
        float S1 = std::sin(T1);
        float C1 = std::cos(T1);
        float T3 = std::atan2(S1 * m[0][2] - C1 * m[0][1], C1 * m[1][1] - S1 * m[1][2]);
        return glm::vec3(-T1, -T2, -T3);
    }
    glm::vec3 FromJQuatEulerDeg(QuatArg q) {
        glm::quat gq(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
        glm::mat3 m = glm::mat3_cast(gq);
        return glm::degrees(ExtractEulerXYZ(m));
    }

    float EffectiveMassKilograms(const SceneObject& obj, const ProjectPhysicsSettings& s) {
        const float authoredMass = std::max(0.0001f, obj.rigidbody.mass);
        const float unitScale = std::max(0.000001f, ProjectMassUnitToKilograms(s.massUnit));
        return std::max(0.01f, authoredMass * unitScale);
    }

    Vec3 SceneGravity(const ProjectPhysicsSettings& s) {
        return Vec3(0.0f, -9.81f * std::max(0.0f, s.globalGravityScale), 0.0f);
    }

    uint32_t BuildAngularLockMask(const SceneObject& obj) {
        uint32_t mask = 0;
        if (obj.rigidbody.lockRotationX) mask |= 1u << 0u;
        if (obj.rigidbody.lockRotationY) mask |= 1u << 1u;
        if (obj.rigidbody.lockRotationZ) mask |= 1u << 2u;
        return mask;
    }

    EAllowedDOFs BuildAllowedDOFs(const SceneObject& obj) {
        EAllowedDOFs dofs = EAllowedDOFs::All;
        if (obj.rigidbody.lockRotationX) dofs &= ~EAllowedDOFs::RotationX;
        if (obj.rigidbody.lockRotationY) dofs &= ~EAllowedDOFs::RotationY;
        if (obj.rigidbody.lockRotationZ) dofs &= ~EAllowedDOFs::RotationZ;
        return dofs;
    }

    bool PoseChangedSignificantly(const glm::vec3& prevPos, const glm::vec3& prevRot,
                                  const glm::vec3& nextPos, const glm::vec3& nextRot) {
        const glm::vec3 dp = nextPos - prevPos;
        const glm::vec3 dr = nextRot - prevRot;
        return glm::dot(dp, dp) > 1e-8f || glm::dot(dr, dr) > 1e-8f;
    }

    void BakeScaleIntoVertices(std::vector<Float3>& vertices, const glm::vec3& scale) {
        const glm::vec3 abs = glm::abs(scale);
        for (Float3& v : vertices) { v.x *= abs.x; v.y *= abs.y; v.z *= abs.z; }
    }

    glm::mat4 BuildColliderVertexTransform(const SceneObject& obj) {
        glm::mat4 t(1.0f);
        t = glm::translate(t, obj.position);
        t = glm::rotate(t, glm::radians(obj.rotation.x), glm::vec3(1, 0, 0));
        t = glm::rotate(t, glm::radians(obj.rotation.y), glm::vec3(0, 1, 0));
        t = glm::rotate(t, glm::radians(obj.rotation.z), glm::vec3(0, 0, 1));
        t = glm::translate(t, obj.collider.offset);
        t = glm::scale(t, obj.scale);
        return t;
    }

    void BakeTransformIntoVertices(std::vector<Float3>& vertices, const SceneObject& obj) {
        const glm::mat4 m = BuildColliderVertexTransform(obj);
        for (Float3& v : vertices) {
            glm::vec4 w = m * glm::vec4(v.x, v.y, v.z, 1.0f);
            v.x = w.x; v.y = w.y; v.z = w.z;
        }
    }

    void ComputeBounds(const std::vector<Float3>& verts, glm::vec3& outMin, glm::vec3& outMax) {
        outMin = glm::vec3(FLT_MAX);
        outMax = glm::vec3(-FLT_MAX);
        for (const Float3& f : verts) {
            outMin = glm::min(outMin, glm::vec3(f.x, f.y, f.z));
            outMax = glm::max(outMax, glm::vec3(f.x, f.y, f.z));
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

    bool GatherMeshData(const SceneObject& obj, std::vector<Float3>& verts, std::vector<IndexedTriangle>& tris) {
        verts.clear();
        tris.clear();
        const OBJLoader::LoadedMesh* meshInfo = nullptr;
        if (obj.hasRenderer && obj.renderType == RenderType::OBJMesh && obj.meshId >= 0) {
            meshInfo = g_objLoader.getMeshInfo(obj.meshId);
        } else if (obj.hasRenderer && obj.renderType == RenderType::Model && obj.meshId >= 0) {
            meshInfo = getModelLoader().getMeshInfo(obj.meshId);
        }

        auto pushFromStream = [&](const std::vector<float>& stream) {
            constexpr size_t kStride = 8;
            if (stream.size() < kStride * 3 || stream.size() % kStride != 0) return false;
            const size_t vCount = stream.size() / kStride;
            verts.reserve(vCount);
            for (size_t i = 0; i < vCount; ++i) {
                const size_t b = i * kStride;
                verts.emplace_back(stream[b + 0], stream[b + 1], stream[b + 2]);
            }
            if (vCount % 3 != 0) return false;
            tris.reserve(vCount / 3);
            for (uint32_t i = 0; i < (uint32_t)vCount; i += 3) {
                tris.emplace_back(i, i + 1, i + 2, 0);
            }
            return true;
        };

        if (meshInfo && !meshInfo->positions.empty() && meshInfo->triangleIndices.size() >= 3) {
            verts.reserve(meshInfo->positions.size());
            for (const auto& p : meshInfo->positions) verts.emplace_back(p.x, p.y, p.z);
            tris.reserve(meshInfo->triangleIndices.size() / 3);
            for (size_t i = 0; i + 2 < meshInfo->triangleIndices.size(); i += 3) {
                tris.emplace_back(meshInfo->triangleIndices[i],
                                  meshInfo->triangleIndices[i + 1],
                                  meshInfo->triangleIndices[i + 2], 0);
            }
            return !verts.empty() && !tris.empty();
        }
        if (meshInfo && !meshInfo->triangleVertices.empty()) {
            const size_t n = meshInfo->triangleVertices.size();
            verts.reserve(n);
            for (const auto& v : meshInfo->triangleVertices) verts.emplace_back(v.x, v.y, v.z);
            if (n % 3 == 0) {
                tris.reserve(n / 3);
                for (uint32_t i = 0; i < (uint32_t)n; i += 3) tris.emplace_back(i, i + 1, i + 2, 0);
                return true;
            }
            verts.clear();
        }
        const std::vector<float>* prim = GetPrimitiveTriangleStream(obj.renderType);
        if (!prim) return false;
        return pushFromStream(*prim);
    }
} // namespace

JoltPhysicsBackend::JoltPhysicsBackend() {
    mFilters = std::make_unique<JoltFilters>();
}

JoltPhysicsBackend::~JoltPhysicsBackend() {
    shutdown();
}

bool JoltPhysicsBackend::init() {
    if (mReady) return true;
    EnsureJoltGlobals();

    mTempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    mJobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
        std::max<int>(1, (int)std::thread::hardware_concurrency() - 1));

    constexpr JPH::uint cMaxBodies = 65536;
    constexpr JPH::uint cNumBodyMutexes = 0;
    constexpr JPH::uint cMaxBodyPairs = 65536;
    constexpr JPH::uint cMaxContactConstraints = 10240;

    mPhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
    mPhysicsSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
                         mFilters->bpInterface, mFilters->objVsBpFilter, mFilters->objLayerPairFilter);
    mContactEvents = std::make_unique<JoltContactEvents>(*this);
    mPhysicsSystem->SetContactListener(mContactEvents.get());
    mPhysicsSystem->SetGravity(SceneGravity(mProjectSettings));

    mReady = true;
    return true;
}

void JoltPhysicsBackend::shutdown() {
    if (!mReady) return;
    clearBodies();
    if (mPhysicsSystem) mPhysicsSystem->SetContactListener(nullptr);
    mContactEvents.reset();
    mPhysicsSystem.reset();
    mJobSystem.reset();
    mTempAllocator.reset();
    mReady = false;
    ReleaseJoltGlobals();
}

bool JoltPhysicsBackend::isReady() const {
    return mReady;
}

void JoltPhysicsBackend::setProjectSettings(const ProjectPhysicsSettings& settings) {
    mProjectSettings = settings;
    applySceneGravity();
    if (mPhysicsSystem) {
        JPH::PhysicsSettings sim = mPhysicsSystem->GetPhysicsSettings();
        sim.mNumVelocitySteps = (JPH::uint)std::clamp(settings.solverIterations, 1, 64);
        sim.mNumPositionSteps = std::max(1u, sim.mNumVelocitySteps / 4u);
        mPhysicsSystem->SetPhysicsSettings(sim);
    }
}

void JoltPhysicsBackend::applySceneGravity() {
    if (!mPhysicsSystem) return;
    mPhysicsSystem->SetGravity(SceneGravity(mProjectSettings));
}

namespace {
    using namespace JPH;

    float BoxConvexRadius(const glm::vec3& halfExtent) {
        const float smallest = std::min({halfExtent.x, halfExtent.y, halfExtent.z});
        return std::clamp(smallest * 0.1f, 0.0005f, cDefaultConvexRadius);
    }

    ShapeRefC BuildShapeForObject(const SceneObject& obj, bool isDynamic, bool bakeWorldSpaceStaticMesh) {
        const bool wantsCollider = obj.hasCollider && obj.collider.enabled;
        const Vec3 localOffset = ToJVec3(obj.collider.offset);

        auto wrapWithOffset = [&](ShapeRefC inner, bool addOffset) -> ShapeRefC {
            if (!addOffset) return inner;
            if (obj.collider.offset == glm::vec3(0.0f)) return inner;
            RotatedTranslatedShapeSettings rts(localOffset, Quat::sIdentity(), inner);
            ShapeSettings::ShapeResult r = rts.Create();
            if (!r.IsValid()) return inner;
            return r.Get();
        };

        if (wantsCollider && obj.collider.type == ColliderType::Box) {
            glm::vec3 half = glm::max(obj.collider.boxSize * 0.5f, glm::vec3(0.01f));
            BoxShapeSettings s(ToJVec3(half), BoxConvexRadius(half));
            s.SetEmbedded();
            ShapeSettings::ShapeResult r = s.Create();
            if (!r.IsValid()) return ShapeRefC();
            return wrapWithOffset(r.Get(), true);
        }
        if (wantsCollider && obj.collider.type == ColliderType::Capsule) {
            float radius = std::max({obj.collider.boxSize.x, obj.collider.boxSize.z}) * 0.5f;
            radius = std::max(radius, 0.01f);
            float cylHeight = std::max(0.05f, obj.collider.boxSize.y - radius * 2.0f);
            CapsuleShapeSettings s(cylHeight * 0.5f, radius);
            s.SetEmbedded();
            ShapeSettings::ShapeResult r = s.Create();
            if (!r.IsValid()) return ShapeRefC();
            return wrapWithOffset(r.Get(), true);
        }
        if (wantsCollider && obj.collider.type == ColliderType::Sphere) {
            float radius = std::max({obj.collider.boxSize.x, obj.collider.boxSize.y, obj.collider.boxSize.z}) * 0.5f;
            radius = std::max(radius, 0.01f);
            SphereShapeSettings s(radius);
            s.SetEmbedded();
            ShapeSettings::ShapeResult r = s.Create();
            if (!r.IsValid()) return ShapeRefC();
            return wrapWithOffset(r.Get(), true);
        }

        if (wantsCollider && (obj.collider.type == ColliderType::Mesh || obj.collider.type == ColliderType::ConvexMesh)) {
            std::vector<Float3> verts;
            std::vector<IndexedTriangle> tris;
            bool ok = false;
            try { ok = GatherMeshData(obj, verts, tris); }
            catch (const std::bad_alloc&) { ok = false; }

            auto fallbackBoxFromBounds = [&](const std::vector<Float3>& v) -> ShapeRefC {
                glm::vec3 bmin, bmax;
                ComputeBounds(v, bmin, bmax);
                glm::vec3 half = glm::max((bmax - bmin) * 0.5f, glm::vec3(0.01f));
                glm::vec3 center = (bmax + bmin) * 0.5f;
                BoxShapeSettings bs(ToJVec3(half), BoxConvexRadius(half));
                bs.SetEmbedded();
                ShapeSettings::ShapeResult r = bs.Create();
                if (!r.IsValid()) return ShapeRefC();
                RotatedTranslatedShapeSettings rts(ToJVec3(bakeWorldSpaceStaticMesh ? center : center + glm::vec3(obj.collider.offset)),
                                                   Quat::sIdentity(), r.Get());
                ShapeSettings::ShapeResult r2 = rts.Create();
                return r2.IsValid() ? r2.Get() : r.Get();
            };

            if (!ok || verts.empty()) return ShapeRefC();

            constexpr size_t kMaxCookVerts = 1000000;
            if (verts.size() > kMaxCookVerts) return fallbackBoxFromBounds(verts);

            if (bakeWorldSpaceStaticMesh) BakeTransformIntoVertices(verts, obj);
            else BakeScaleIntoVertices(verts, obj.scale);

            const bool useConvex = isDynamic || obj.collider.type == ColliderType::ConvexMesh;
            if (useConvex) {
                Array<Vec3> hullPoints;
                hullPoints.reserve(verts.size());
                for (const Float3& f : verts) hullPoints.push_back(Vec3(f.x, f.y, f.z));
                ConvexHullShapeSettings s(hullPoints, 0.0f);
                s.SetEmbedded();
                ShapeSettings::ShapeResult r = s.Create();
                if (!r.IsValid()) return fallbackBoxFromBounds(verts);
                return wrapWithOffset(r.Get(), !bakeWorldSpaceStaticMesh);
            } else {
                VertexList jVerts;
                jVerts.reserve(verts.size());
                for (const Float3& f : verts) jVerts.push_back(f);
                IndexedTriangleList jTris;
                jTris.reserve(tris.size());
                for (const IndexedTriangle& t : tris) jTris.push_back(t);
                MeshShapeSettings s(std::move(jVerts), std::move(jTris));
                s.SetEmbedded();
                ShapeSettings::ShapeResult r = s.Create();
                if (!r.IsValid()) return ShapeRefC();
                return wrapWithOffset(r.Get(), !bakeWorldSpaceStaticMesh);
            }
        }

        switch (obj.renderType) {
            case RenderType::Cube:
            case RenderType::Plane:
            case RenderType::Sprite: {
                glm::vec3 half = glm::max(obj.scale * 0.5f, glm::vec3(0.01f));
                BoxShapeSettings s(ToJVec3(half), BoxConvexRadius(half));
                s.SetEmbedded();
                ShapeSettings::ShapeResult r = s.Create();
                return r.IsValid() ? r.Get() : ShapeRefC();
            }
            case RenderType::Sphere:
            case RenderType::Torus: {
                float radius = std::max({obj.scale.x, obj.scale.y, obj.scale.z}) * 0.5f;
                radius = std::max(radius, 0.01f);
                SphereShapeSettings s(radius);
                s.SetEmbedded();
                ShapeSettings::ShapeResult r = s.Create();
                return r.IsValid() ? r.Get() : ShapeRefC();
            }
            case RenderType::Capsule: {
                float radius = std::max(obj.scale.x, obj.scale.z) * 0.5f;
                radius = std::max(radius, 0.01f);
                float cylHeight = std::max(0.05f, obj.scale.y - radius * 2.0f);
                CapsuleShapeSettings s(cylHeight * 0.5f, radius);
                s.SetEmbedded();
                ShapeSettings::ShapeResult r = s.Create();
                return r.IsValid() ? r.Get() : ShapeRefC();
            }
            default: return ShapeRefC();
        }
    }

    float ColliderFriction(const SceneObject& obj) {
        if (!obj.hasCollider || !obj.collider.enabled) return 0.9f;
        return std::clamp(0.5f * (obj.collider.staticFriction + obj.collider.dynamicFriction), 0.0f, 4.0f);
    }
    float ColliderRestitution(const SceneObject& obj) {
        if (!obj.hasCollider || !obj.collider.enabled) return 0.0f;
        return std::clamp(obj.collider.restitution, 0.0f, 1.0f);
    }
} // namespace

JoltPhysicsBackend::BodyRecord JoltPhysicsBackend::createBodyFor(const SceneObject& obj) {
    BodyRecord rec;
    const bool wantsDynamic = obj.hasRigidbody && obj.rigidbody.enabled;
    const bool wantsCollider = obj.hasCollider && obj.collider.enabled;
    const bool bakeWorldStaticMesh = !wantsDynamic && wantsCollider && obj.collider.type == ColliderType::Mesh;
    if (!wantsDynamic && !wantsCollider) return rec;

    JPH::ShapeRefC shape = BuildShapeForObject(obj, wantsDynamic, bakeWorldStaticMesh);
    if (!shape) return rec;

    const JPH::Vec3 pos = bakeWorldStaticMesh ? JPH::Vec3::sZero() : ToJVec3(obj.position);
    const JPH::Quat rot = bakeWorldStaticMesh ? JPH::Quat::sIdentity() : ToJQuat(obj.rotation);
    const JPH::ObjectLayer layer = wantsDynamic ? JoltLayers::MOVING : JoltLayers::NON_MOVING;
    const JPH::EMotionType motionType = wantsDynamic
        ? (obj.rigidbody.isKinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic)
        : JPH::EMotionType::Static;

    JPH::BodyCreationSettings settings(shape, pos, rot, motionType, layer);
    settings.mIsSensor = wantsCollider && obj.collider.isTrigger;
    settings.mUserData = static_cast<uint64_t>(obj.id) + 1u;
    settings.mFriction = ColliderFriction(obj);
    settings.mRestitution = ColliderRestitution(obj);

    if (wantsDynamic) {
        settings.mLinearDamping = std::max(0.0f, obj.rigidbody.linearDamping);
        settings.mAngularDamping = std::max(0.0f, obj.rigidbody.angularDamping);
        settings.mGravityFactor = obj.rigidbody.useGravity ? 1.0f : 0.0f;
        settings.mAllowedDOFs = BuildAllowedDOFs(obj);
        if (!obj.rigidbody.isKinematic) {
            const float massKg = EffectiveMassKilograms(obj, mProjectSettings);
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = massKg;
            rec.massKg = massKg;
            rec.useCustomCenterOfMass = obj.rigidbody.useCustomCenterOfMass;
            rec.centerOfMass = obj.rigidbody.centerOfMass;
        }
        if (obj.hasPlayerController) {
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
        }
    }

    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    JPH::BodyID bodyId = bi.CreateAndAddBody(settings,
        wantsDynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (bodyId.IsInvalid()) return rec;

    rec.bodyId = bodyId;
    rec.isDynamic = wantsDynamic;
    rec.isKinematic = wantsDynamic && obj.rigidbody.isKinematic;
    rec.hasWorldBakedStaticMesh = bakeWorldStaticMesh;
    rec.hasAuthoringPose = true;
    rec.lastAuthoringPosition = obj.position;
    rec.lastAuthoringRotation = obj.rotation;
    rec.linearDamping = obj.rigidbody.linearDamping;
    rec.angularDamping = obj.rigidbody.angularDamping;
    rec.gravityDisabled = !obj.rigidbody.useGravity;
    rec.angularLockMask = BuildAngularLockMask(obj);
    rec.isTrigger = wantsCollider && obj.collider.isTrigger;
    rec.prevPos = pos;
    rec.currPos = pos;
    rec.prevRot = rot;
    rec.currRot = rot;
    return rec;
}

void JoltPhysicsBackend::clearBodies() {
    if (mPhysicsSystem) {
        JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
        for (auto& [id, rec] : mBodies) {
            if (!rec.bodyId.IsInvalid()) {
                bi.RemoveBody(rec.bodyId);
                bi.DestroyBody(rec.bodyId);
            }
        }
    }
    mBodies.clear();
    mIdsByBody.clear();
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    mPendingCollisionEvents.clear();
    mActiveCollisionPairs.clear();
    mIgnoredCollisionPairs.clear();
}

void JoltPhysicsBackend::onPlayStart(const std::vector<SceneObject>& objects) {
    if (!isReady()) return;
    clearBodies();
    mTimeAccumulator = 0.0f;
    mBodies.reserve(objects.size());
    mIdsByBody.reserve(objects.size());
    for (const auto& obj : objects) {
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        BodyRecord rec = createBodyFor(obj);
        if (rec.bodyId.IsInvalid()) continue;
        mBodies[obj.id] = rec;
        mIdsByBody[rec.bodyId.GetIndexAndSequenceNumber()] = obj.id;
    }
    if (mPhysicsSystem) mPhysicsSystem->OptimizeBroadPhase();
}

void JoltPhysicsBackend::onPlayStop() {
    clearBodies();
    mTimeAccumulator = 0.0f;
}

bool JoltPhysicsBackend::setLinearVelocity(int id, const glm::vec3& velocity) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic) return false;
    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    bi.ActivateBody(rec.bodyId);
    bi.SetLinearVelocity(rec.bodyId, ToJVec3(velocity));
    return true;
}

bool JoltPhysicsBackend::setAngularVelocity(int id, const glm::vec3& velocity) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) return false;
    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    bi.SetAngularVelocity(rec.bodyId, ToJVec3(velocity));
    return true;
}

bool JoltPhysicsBackend::setContinuousCollisionDetection(int id, bool enabled) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) return false;
    mPhysicsSystem->GetBodyInterface().SetMotionQuality(
        rec.bodyId,
        enabled ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
    return true;
}

bool JoltPhysicsBackend::setActorYaw(int id, float yawDegrees) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid()) return false;
    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    JPH::Vec3 pos = bi.GetPosition(rec.bodyId);
    JPH::Quat q = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), glm::radians(yawDegrees));
    bi.SetPositionAndRotation(rec.bodyId, pos, q, JPH::EActivation::Activate);
    rec.hasAuthoringPose = true;
    rec.lastAuthoringPosition = FromJVec3(pos);
    rec.lastAuthoringRotation = FromJQuatEulerDeg(q);
    return true;
}

bool JoltPhysicsBackend::getLinearVelocity(int id, glm::vec3& outVelocity) const {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    const BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic) return false;
    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    outVelocity = FromJVec3(bi.GetLinearVelocity(rec.bodyId));
    return true;
}

bool JoltPhysicsBackend::getAngularVelocity(int id, glm::vec3& outVelocity) const {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    const BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) return false;
    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    outVelocity = FromJVec3(bi.GetAngularVelocity(rec.bodyId));
    return true;
}

bool JoltPhysicsBackend::getActorPose(int id, glm::vec3& outPosition,
                                     glm::vec3& outRotationDeg) const {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    const BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid()) return false;
    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    outPosition = FromJVec3(bi.GetPosition(rec.bodyId));
    outRotationDeg = FromJQuatEulerDeg(bi.GetRotation(rec.bodyId));
    return true;
}

bool JoltPhysicsBackend::setActorPose(int id, const glm::vec3& position, const glm::vec3& rotationDeg) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid()) return false;
    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();
    const JPH::Vec3 joltPosition = ToJVec3(position);
    const JPH::Quat joltRotation = ToJQuat(rotationDeg);
    bi.SetPositionAndRotation(rec.bodyId, joltPosition, joltRotation, JPH::EActivation::Activate);
    rec.prevPos = joltPosition;
    rec.currPos = joltPosition;
    rec.prevRot = joltRotation;
    rec.currRot = joltRotation;
    rec.hasAuthoringPose = true;
    rec.lastAuthoringPosition = position;
    rec.lastAuthoringRotation = rotationDeg;
    return true;
}

bool JoltPhysicsBackend::addForce(int id, const glm::vec3& force) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) return false;
    mPhysicsSystem->GetBodyInterfaceNoLock().AddForce(rec.bodyId, ToJVec3(force));
    return true;
}

bool JoltPhysicsBackend::addImpulse(int id, const glm::vec3& impulse) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) return false;
    mPhysicsSystem->GetBodyInterfaceNoLock().AddImpulse(rec.bodyId, ToJVec3(impulse));
    return true;
}

bool JoltPhysicsBackend::addTorque(int id, const glm::vec3& torque) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) return false;
    mPhysicsSystem->GetBodyInterfaceNoLock().AddTorque(rec.bodyId, ToJVec3(torque));
    return true;
}

bool JoltPhysicsBackend::addAngularImpulse(int id, const glm::vec3& impulse) {
    auto it = mBodies.find(id);
    if (it == mBodies.end()) return false;
    BodyRecord& rec = it->second;
    if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) return false;
    mPhysicsSystem->GetBodyInterfaceNoLock().AddAngularImpulse(rec.bodyId, ToJVec3(impulse));
    return true;
}

bool JoltPhysicsBackend::refreshObject(const SceneObject& obj) {
    if (!isReady()) return false;
    auto existing = mBodies.find(obj.id);
    if (existing == mBodies.end()) {
        return true;
    }

    glm::vec3 linearVelocity(0.0f);
    glm::vec3 angularVelocity(0.0f);
    const bool hadLinearVelocity = getLinearVelocity(obj.id, linearVelocity);
    const bool hadAngularVelocity = getAngularVelocity(obj.id, angularVelocity);

    endCollisionPairsForObject(obj.id);
    JPH::BodyInterface& bodies = mPhysicsSystem->GetBodyInterfaceNoLock();
    const JPH::BodyID oldBody = existing->second.bodyId;
    if (!oldBody.IsInvalid()) {
        mIdsByBody.erase(oldBody.GetIndexAndSequenceNumber());
        bodies.RemoveBody(oldBody);
        bodies.DestroyBody(oldBody);
    }
    mBodies.erase(existing);

    BodyRecord refreshed = createBodyFor(obj);
    if (refreshed.bodyId.IsInvalid()) return false;
    mIdsByBody[refreshed.bodyId.GetIndexAndSequenceNumber()] = obj.id;
    mBodies[obj.id] = refreshed;
    if (hadLinearVelocity) setLinearVelocity(obj.id, linearVelocity);
    if (hadAngularVelocity) setAngularVelocity(obj.id, angularVelocity);
    return true;
}

namespace {
    using namespace JPH;

    class IgnoreBodyFilter final : public BodyFilter {
    public:
        BodyID ignore;
        bool ShouldCollide(const BodyID& inBodyID) const override {
            return inBodyID != ignore;
        }
        bool ShouldCollideLocked(const Body& inBody) const override {
            return inBody.GetID() != ignore;
        }
    };
} // namespace

bool JoltPhysicsBackend::raycastClosest(const glm::vec3& origin,
                                        const glm::vec3& dir,
                                        float distance,
                                        int ignoreId,
                                        glm::vec3* hitPos,
                                        glm::vec3* hitNormal,
                                        float* hitDistance,
                                        int* hitActorId,
                                        glm::vec3* hitActorVelocity,
                                        float* hitStaticFriction,
                                        float* hitDynamicFriction) const {
    if (!isReady() || distance <= 0.0f) return false;
    if (hitActorId) *hitActorId = -1;
    if (hitActorVelocity) *hitActorVelocity = glm::vec3(0.0f);
    if (hitStaticFriction) *hitStaticFriction = 0.9f;
    if (hitDynamicFriction) *hitDynamicFriction = 0.9f;

    glm::vec3 normDir = glm::normalize(dir);
    if (!std::isfinite(normDir.x) || !std::isfinite(normDir.y) || !std::isfinite(normDir.z)) return false;

    JPH::RRayCast ray(ToJVec3(origin), ToJVec3(normDir * distance));
    JPH::RayCastResult result;

    IgnoreBodyFilter bodyFilter;
    auto it = mBodies.find(ignoreId);
    if (it != mBodies.end()) bodyFilter.ignore = it->second.bodyId;

    const JPH::NarrowPhaseQuery& q = mPhysicsSystem->GetNarrowPhaseQuery();
    if (!q.CastRay(ray, result, {}, {}, bodyFilter)) return false;

    JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), result.mBodyID);
    if (!lock.Succeeded()) return false;
    const JPH::Body& body = lock.GetBody();

    const JPH::Vec3 hitPoint = ray.GetPointOnRay(result.mFraction);
    if (hitPos) *hitPos = FromJVec3(hitPoint);
    if (hitNormal) {
        JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, hitPoint);
        *hitNormal = FromJVec3(n);
    }
    if (hitDistance) *hitDistance = result.mFraction * distance;

    const float friction = body.GetFriction();
    if (hitStaticFriction) *hitStaticFriction = friction;
    if (hitDynamicFriction) *hitDynamicFriction = friction;

    if (hitActorId || hitActorVelocity) {
        auto idIt = mIdsByBody.find(result.mBodyID.GetIndexAndSequenceNumber());
        if (idIt != mIdsByBody.end()) {
            if (hitActorId) *hitActorId = idIt->second;
            if (hitActorVelocity && body.IsDynamic()) {
                *hitActorVelocity = FromJVec3(body.GetLinearVelocity());
            }
        }
    }
    return true;
}

void JoltPhysicsBackend::simulate(float deltaTime, std::vector<SceneObject>& objects) {
    if (!isReady() || deltaTime <= 0.0f) return;

    std::unordered_map<int, SceneObject*> objectsById;
    objectsById.reserve(objects.size());
    for (SceneObject& obj : objects) objectsById[obj.id] = &obj;

    JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterfaceNoLock();

    for (SceneObject& obj : objects) {
        if (mBodies.find(obj.id) != mBodies.end()) continue;
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        const bool wantsDyn = obj.hasRigidbody && obj.rigidbody.enabled;
        const bool wantsCol = obj.hasCollider && obj.collider.enabled;
        if (!wantsDyn && !wantsCol) continue;
        BodyRecord rec = createBodyFor(obj);
        if (rec.bodyId.IsInvalid()) continue;
        mBodies[obj.id] = rec;
        mIdsByBody[rec.bodyId.GetIndexAndSequenceNumber()] = obj.id;
    }

    {
        std::vector<int> idsToRecreate;
        for (auto& [id, rec] : mBodies) {
            if (rec.bodyId.IsInvalid()) continue;
            auto objIt = objectsById.find(id);
            if (objIt == objectsById.end()) continue;
            const bool wantsDynamicNow = objIt->second->hasRigidbody && objIt->second->rigidbody.enabled;
            const bool wantsTriggerNow = objIt->second->hasCollider &&
                objIt->second->collider.enabled && objIt->second->collider.isTrigger;
            if (rec.isDynamic != wantsDynamicNow || rec.isTrigger != wantsTriggerNow) {
                idsToRecreate.push_back(id);
            }
        }
        for (int id : idsToRecreate) {
            auto recIt = mBodies.find(id);
            if (recIt == mBodies.end()) continue;
            BodyRecord& rec = recIt->second;
            endCollisionPairsForObject(id);
            if (!rec.bodyId.IsInvalid()) {
                mIdsByBody.erase(rec.bodyId.GetIndexAndSequenceNumber());
                bi.RemoveBody(rec.bodyId);
                bi.DestroyBody(rec.bodyId);
                rec.bodyId = JPH::BodyID();
            }
            BodyRecord fresh = createBodyFor(*objectsById[id]);
            if (!fresh.bodyId.IsInvalid()) {
                mIdsByBody[fresh.bodyId.GetIndexAndSequenceNumber()] = id;
                recIt->second = fresh;
            } else {
                mBodies.erase(recIt);
            }
        }
    }

    for (auto& [id, rec] : mBodies) {
        if (rec.bodyId.IsInvalid()) continue;
        auto objIt = objectsById.find(id);
        if (objIt == objectsById.end()) continue;
        SceneObject& obj = *objIt->second;
        const bool enabledInHierarchy = IsObjectEnabledInHierarchy(obj);
        if (enabledInHierarchy == rec.simulationDisabled) {
            if (enabledInHierarchy) bi.AddBody(rec.bodyId, JPH::EActivation::Activate);
            else bi.RemoveBody(rec.bodyId);
            rec.simulationDisabled = !enabledInHierarchy;
        }
        if (!enabledInHierarchy) continue;

        if (rec.isDynamic) {
            const bool isKinematic = obj.rigidbody.isKinematic;
            if (rec.isKinematic != isKinematic) {
                bi.SetMotionType(rec.bodyId,
                    isKinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic,
                    JPH::EActivation::Activate);
                rec.isKinematic = isKinematic;
            }
            const bool gravityDisabled = !obj.rigidbody.useGravity;
            if (rec.gravityDisabled != gravityDisabled) {
                bi.SetGravityFactor(rec.bodyId, gravityDisabled ? 0.0f : 1.0f);
                rec.gravityDisabled = gravityDisabled;
            }
            const bool dampingChanged =
                std::abs(rec.linearDamping - obj.rigidbody.linearDamping) > 0.0001f ||
                std::abs(rec.angularDamping - obj.rigidbody.angularDamping) > 0.0001f;
            const uint32_t newLockMask = BuildAngularLockMask(obj);
            const bool locksChanged = rec.angularLockMask != newLockMask;
            const float massKg = EffectiveMassKilograms(obj, mProjectSettings);
            const bool massChanged = !isKinematic && std::abs(rec.massKg - massKg) > 0.0001f;
            if (dampingChanged || locksChanged || massChanged) {
                JPH::BodyLockWrite lock(mPhysicsSystem->GetBodyLockInterface(), rec.bodyId);
                if (lock.Succeeded()) {
                    JPH::Body& body = lock.GetBody();
                    if (dampingChanged) {
                        if (auto* mp = body.GetMotionPropertiesUnchecked()) {
                            mp->SetLinearDamping(std::max(0.0f, obj.rigidbody.linearDamping));
                            mp->SetAngularDamping(std::max(0.0f, obj.rigidbody.angularDamping));
                        }
                        rec.linearDamping = obj.rigidbody.linearDamping;
                        rec.angularDamping = obj.rigidbody.angularDamping;
                    }
                    if (massChanged) {
                        if (auto* mp = body.GetMotionPropertiesUnchecked()) {
                            mp->ScaleToMass(std::max(0.01f, massKg));
                        }
                        rec.massKg = massKg;
                    }
                    rec.angularLockMask = newLockMask;
                    (void)locksChanged;
                }
            }
            if (rec.isKinematic) {
                const bool poseChanged = !rec.hasAuthoringPose ||
                    PoseChangedSignificantly(rec.lastAuthoringPosition, rec.lastAuthoringRotation, obj.position, obj.rotation);
                if (poseChanged) {
                    bi.MoveKinematic(rec.bodyId, ToJVec3(obj.position), ToJQuat(obj.rotation), deltaTime);
                    rec.hasAuthoringPose = true;
                    rec.lastAuthoringPosition = obj.position;
                    rec.lastAuthoringRotation = obj.rotation;
                }
            }
        } else {
            if (rec.hasWorldBakedStaticMesh) continue;
            const bool poseChanged = !rec.hasAuthoringPose ||
                PoseChangedSignificantly(rec.lastAuthoringPosition, rec.lastAuthoringRotation, obj.position, obj.rotation);
            if (poseChanged) {
                bi.SetPositionAndRotation(rec.bodyId, ToJVec3(obj.position), ToJQuat(obj.rotation), JPH::EActivation::DontActivate);
                rec.hasAuthoringPose = true;
                rec.lastAuthoringPosition = obj.position;
                rec.lastAuthoringRotation = obj.rotation;
            }
        }
    }

    const float fixedDt = std::clamp(mProjectSettings.fixedTimestep, 0.001f, 0.1f);
    mTimeAccumulator += deltaTime;
    constexpr int kMaxSubsteps = 4;
    constexpr int kCollisionSteps = 1;

    const bool willStep = mTimeAccumulator >= fixedDt;
    if (willStep) {
        for (auto& [id, rec] : mBodies) {
            if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) continue;
            rec.prevPos = rec.currPos;
            rec.prevRot = rec.currRot;
        }
    }

    int substeps = 0;
    while (mTimeAccumulator >= fixedDt && substeps < kMaxSubsteps) {
        mPhysicsSystem->Update(fixedDt, kCollisionSteps, mTempAllocator.get(), mJobSystem.get());
        mTimeAccumulator -= fixedDt;
        ++substeps;
    }
    if (substeps == kMaxSubsteps) mTimeAccumulator = 0.0f;

    if (substeps > 0) {
        for (auto& [id, rec] : mBodies) {
            if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) continue;
            rec.currPos = bi.GetPosition(rec.bodyId);
            rec.currRot = bi.GetRotation(rec.bodyId);
        }
    }

    const float alpha = std::clamp(mTimeAccumulator / fixedDt, 0.0f, 1.0f);

    for (auto& [id, rec] : mBodies) {
        if (rec.bodyId.IsInvalid() || !rec.isDynamic || rec.isKinematic) continue;
        auto objIt = objectsById.find(id);
        if (objIt == objectsById.end() || !IsObjectEnabledInHierarchy(*objIt->second)) continue;
        SceneObject& obj = *objIt->second;

        const int interpolationMode = std::clamp(obj.rigidbody.interpolationMode, 0, 2);
        JPH::Vec3 renderPos = rec.currPos;
        JPH::Quat renderRot = rec.currRot;
        if (interpolationMode == 1) {
            renderPos = rec.prevPos * (1.0f - alpha) + rec.currPos * alpha;
            renderRot = rec.prevRot.SLERP(rec.currRot, alpha);
        } else if (interpolationMode == 2) {
            const float predictionTime = std::min(
                alpha * fixedDt,
                std::max(0.0f, obj.rigidbody.maxExtrapolationTime));
            renderPos += bi.GetLinearVelocity(rec.bodyId) * predictionTime;
            const JPH::Vec3 angularVelocity = bi.GetAngularVelocity(rec.bodyId);
            const float angularSpeed = angularVelocity.Length();
            if (angularSpeed > 1.0e-5f && predictionTime > 0.0f) {
                const JPH::Quat deltaRotation = JPH::Quat::sRotation(
                    angularVelocity / angularSpeed,
                    angularSpeed * predictionTime);
                renderRot = (deltaRotation * renderRot).Normalized();
            }
        }
        obj.position = FromJVec3(renderPos);
        if (obj.hasPlayerController && obj.playerController.enabled) continue;
        glm::vec3 euler = FromJQuatEulerDeg(renderRot);
        const bool lockX = (rec.angularLockMask & (1u << 0u)) != 0;
        const bool lockY = (rec.angularLockMask & (1u << 1u)) != 0;
        const bool lockZ = (rec.angularLockMask & (1u << 2u)) != 0;
        if (lockX && lockZ && !lockY) obj.rotation.y = euler.y;
        else obj.rotation = euler;
    }
}

void JoltPhysicsBackend::queueCollisionEvent(int objectAId, int objectBId,
                                             PhysicsCollisionPhase phase) {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return;
    const uint64_t key = JoltCollisionPairKey(objectAId, objectBId);
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

void JoltPhysicsBackend::endCollisionPairsForObject(int objectId) {
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

bool JoltPhysicsBackend::areObjectsTouching(int objectAId, int objectBId) const {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return false;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    return mActiveCollisionPairs.find(JoltCollisionPairKey(objectAId, objectBId)) !=
           mActiveCollisionPairs.end();
}

bool JoltPhysicsBackend::isCollisionPairIgnored(int objectAId, int objectBId) const {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return false;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    return mIgnoredCollisionPairs.find(JoltCollisionPairKey(objectAId, objectBId)) !=
           mIgnoredCollisionPairs.end();
}

bool JoltPhysicsBackend::setObjectsCollisionIgnored(int objectAId, int objectBId, bool ignored) {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return false;
    const uint64_t key = JoltCollisionPairKey(objectAId, objectBId);
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

    if (!changed || !mPhysicsSystem) return true;

    auto bodyIt = mBodies.find(objectAId);
    if (bodyIt == mBodies.end()) bodyIt = mBodies.find(objectBId);
    if (bodyIt != mBodies.end() && !bodyIt->second.bodyId.IsInvalid()) {
        JPH::BodyInterface& bi = mPhysicsSystem->GetBodyInterface();
        JPH::RefConst<JPH::Shape> shape = bi.GetShape(bodyIt->second.bodyId);
        if (shape) {
            bi.NotifyShapeChanged(bodyIt->second.bodyId, shape->GetCenterOfMass(), false,
                                  JPH::EActivation::Activate);
        }
    }
    return true;
}

void JoltPhysicsBackend::noteContactNormal(int objectAId, int objectBId,
                                           const glm::vec3& lowToHighNormal) {
    if (objectAId < 0 || objectBId < 0 || objectAId == objectBId) return;
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    auto it = mActiveCollisionPairs.find(JoltCollisionPairKey(objectAId, objectBId));
    if (it == mActiveCollisionPairs.end()) return;
    it->second.normal = lowToHighNormal;
}

bool JoltPhysicsBackend::getContactNormal(int objectId, glm::vec3& outNormal) const {
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

int JoltPhysicsBackend::countActiveContacts(int objectId) const {
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

void JoltPhysicsBackend::drainCollisionEvents(std::vector<PhysicsCollisionEvent>& outEvents) {
    std::lock_guard<std::mutex> lock(mCollisionEventMutex);
    outEvents.insert(outEvents.end(), mPendingCollisionEvents.begin(), mPendingCollisionEvents.end());
    std::unordered_set<uint64_t> changedPairs;
    changedPairs.reserve(mPendingCollisionEvents.size());
    for (const PhysicsCollisionEvent& event : mPendingCollisionEvents) {
        changedPairs.insert(JoltCollisionPairKey(event.objectAId, event.objectBId));
    }
    for (const auto& [key, state] : mActiveCollisionPairs) {
        if (changedPairs.find(key) != changedPairs.end()) continue;
        outEvents.push_back({static_cast<int>(key >> 32u),
                             static_cast<int>(key & 0xffffffffu),
                             PhysicsCollisionPhase::Stay});
    }
    mPendingCollisionEvents.clear();
}

#endif // MODULARITY_ENABLE_JOLT
