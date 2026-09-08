#pragma once

#include "XRSettings.h"

#include <vector>

namespace Modularity::XR {

enum class XRActionSlot {
    GripPose = 0,
    AimPose,
    TriggerValue,
    GripValue,
    Thumbstick,
    ThumbstickClick,
    PrimaryButton,
    SecondaryButton,
    MenuButton,
    Haptic,
    Count
};

struct XRBindingEntry {
    XRActionSlot slot;
    const char* leftPath;
    const char* rightPath;
};

struct XRProfileBindings {
    XRInteractionProfile profile;
    const char* profilePath;
    const XRBindingEntry* entries;
    size_t entryCount;
};

const XRProfileBindings* GetProfileBindings(XRInteractionProfile profile);

}
