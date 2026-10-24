#pragma once

#include "ThirdParty/glm/glm.hpp"

namespace PhysicsContacts {

// Input points from the first callback body toward the second. Preserve the
// solver's direction: actor centres do not describe mesh faces or their edges.
inline glm::vec3 LowToHighNormal(int firstId, int secondId,
                               const glm::vec3& firstToSecond) {
    return firstId < secondId ? firstToSecond : -firstToSecond;
}

} // namespace PhysicsContacts
