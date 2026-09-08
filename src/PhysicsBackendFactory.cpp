#include "PhysicsBackendFactory.h"

#ifdef MODULARITY_ENABLE_JOLT
#include "JoltPhysicsBackend.h"
#endif
#ifdef MODULARITY_ENABLE_PHYSX
#include "PhysicsSystem.h"
#endif

#include <iostream>

std::unique_ptr<IPhysicsBackend> CreatePhysicsBackend(PhysicsBackendType type) {
    auto makeJolt = []() -> std::unique_ptr<IPhysicsBackend> {
#ifdef MODULARITY_ENABLE_JOLT
        return std::make_unique<JoltPhysicsBackend>();
#else
        return nullptr;
#endif
    };
    auto makePhysX = []() -> std::unique_ptr<IPhysicsBackend> {
#ifdef MODULARITY_ENABLE_PHYSX
        return std::make_unique<PhysicsSystem>();
#else
        return nullptr;
#endif
    };

    std::unique_ptr<IPhysicsBackend> backend;
    if (type == PhysicsBackendType::PhysX) {
        backend = makePhysX();
        if (!backend) {
            std::cerr << "[Physics] PhysX backend requested but not compiled in; "
                         "falling back to Jolt." << std::endl;
            backend = makeJolt();
        }
    } else {
        backend = makeJolt();
        if (!backend) {
            std::cerr << "[Physics] Jolt backend requested but not compiled in; "
                         "falling back to PhysX." << std::endl;
            backend = makePhysX();
        }
    }
    return backend;
}
