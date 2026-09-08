#pragma once

#include "IPhysicsBackend.h"
#include "ProjectManager.h"

#include <memory>

std::unique_ptr<IPhysicsBackend> CreatePhysicsBackend(PhysicsBackendType type);
