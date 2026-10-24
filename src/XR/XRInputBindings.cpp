#include "XRInputBindings.h"
#include <array>

namespace Modularity::XR {

namespace {

constexpr XRBindingEntry kMetaQuestTouchEntries[] = {
    { XRActionSlot::GripPose,        "/user/hand/left/input/grip/pose",
                                     "/user/hand/right/input/grip/pose" },
    { XRActionSlot::AimPose,         "/user/hand/left/input/aim/pose",
                                     "/user/hand/right/input/aim/pose" },
    { XRActionSlot::TriggerValue,    "/user/hand/left/input/trigger/value",
                                     "/user/hand/right/input/trigger/value" },
    { XRActionSlot::GripValue,       "/user/hand/left/input/squeeze/value",
                                     "/user/hand/right/input/squeeze/value" },
    { XRActionSlot::Thumbstick,      "/user/hand/left/input/thumbstick",
                                     "/user/hand/right/input/thumbstick" },
    { XRActionSlot::ThumbstickClick, "/user/hand/left/input/thumbstick/click",
                                     "/user/hand/right/input/thumbstick/click" },
    { XRActionSlot::PrimaryButton,   "/user/hand/left/input/x/click",
                                     "/user/hand/right/input/a/click" },
    { XRActionSlot::SecondaryButton, "/user/hand/left/input/y/click",
                                     "/user/hand/right/input/b/click" },
    { XRActionSlot::MenuButton,      "/user/hand/left/input/menu/click",
                                     nullptr },
    { XRActionSlot::Haptic,          "/user/hand/left/output/haptic",
                                     "/user/hand/right/output/haptic" },
};

constexpr XRBindingEntry kValveIndexEntries[] = {
    { XRActionSlot::GripPose,        "/user/hand/left/input/grip/pose",
                                     "/user/hand/right/input/grip/pose" },
    { XRActionSlot::AimPose,         "/user/hand/left/input/aim/pose",
                                     "/user/hand/right/input/aim/pose" },
    { XRActionSlot::TriggerValue,    "/user/hand/left/input/trigger/value",
                                     "/user/hand/right/input/trigger/value" },
    { XRActionSlot::GripValue,       "/user/hand/left/input/squeeze/value",
                                     "/user/hand/right/input/squeeze/value" },
    { XRActionSlot::Thumbstick,      "/user/hand/left/input/thumbstick",
                                     "/user/hand/right/input/thumbstick" },
    { XRActionSlot::ThumbstickClick, "/user/hand/left/input/thumbstick/click",
                                     "/user/hand/right/input/thumbstick/click" },
    { XRActionSlot::PrimaryButton,   "/user/hand/left/input/a/click",
                                     "/user/hand/right/input/a/click" },
    { XRActionSlot::SecondaryButton, "/user/hand/left/input/b/click",
                                     "/user/hand/right/input/b/click" },
    { XRActionSlot::MenuButton,      nullptr, nullptr },
    { XRActionSlot::Haptic,          "/user/hand/left/output/haptic",
                                     "/user/hand/right/output/haptic" },
};

constexpr XRBindingEntry kHtcViveEntries[] = {
    { XRActionSlot::GripPose,        "/user/hand/left/input/grip/pose",
                                     "/user/hand/right/input/grip/pose" },
    { XRActionSlot::AimPose,         "/user/hand/left/input/aim/pose",
                                     "/user/hand/right/input/aim/pose" },
    { XRActionSlot::TriggerValue,    "/user/hand/left/input/trigger/value",
                                     "/user/hand/right/input/trigger/value" },
    { XRActionSlot::GripValue,       "/user/hand/left/input/squeeze/click",
                                     "/user/hand/right/input/squeeze/click" },
    { XRActionSlot::Thumbstick,      "/user/hand/left/input/trackpad",
                                     "/user/hand/right/input/trackpad" },
    { XRActionSlot::ThumbstickClick, "/user/hand/left/input/trackpad/click",
                                     "/user/hand/right/input/trackpad/click" },
    { XRActionSlot::PrimaryButton,   nullptr, nullptr },
    { XRActionSlot::SecondaryButton, nullptr, nullptr },
    { XRActionSlot::MenuButton,      "/user/hand/left/input/menu/click",
                                     "/user/hand/right/input/menu/click" },
    { XRActionSlot::Haptic,          "/user/hand/left/output/haptic",
                                     "/user/hand/right/output/haptic" },
};

constexpr XRBindingEntry kMicrosoftMotionEntries[] = {
    { XRActionSlot::GripPose,        "/user/hand/left/input/grip/pose",
                                     "/user/hand/right/input/grip/pose" },
    { XRActionSlot::AimPose,         "/user/hand/left/input/aim/pose",
                                     "/user/hand/right/input/aim/pose" },
    { XRActionSlot::TriggerValue,    "/user/hand/left/input/trigger/value",
                                     "/user/hand/right/input/trigger/value" },
    { XRActionSlot::GripValue,       "/user/hand/left/input/squeeze/click",
                                     "/user/hand/right/input/squeeze/click" },
    { XRActionSlot::Thumbstick,      "/user/hand/left/input/thumbstick",
                                     "/user/hand/right/input/thumbstick" },
    { XRActionSlot::ThumbstickClick, "/user/hand/left/input/thumbstick/click",
                                     "/user/hand/right/input/thumbstick/click" },
    { XRActionSlot::PrimaryButton,   "/user/hand/left/input/trackpad/click",
                                     "/user/hand/right/input/trackpad/click" },
    { XRActionSlot::SecondaryButton, nullptr, nullptr },
    { XRActionSlot::MenuButton,      "/user/hand/left/input/menu/click",
                                     "/user/hand/right/input/menu/click" },
    { XRActionSlot::Haptic,          "/user/hand/left/output/haptic",
                                     "/user/hand/right/output/haptic" },
};

constexpr XRBindingEntry kKhronosSimpleEntries[] = {
    { XRActionSlot::GripPose,        "/user/hand/left/input/grip/pose",
                                     "/user/hand/right/input/grip/pose" },
    { XRActionSlot::AimPose,         "/user/hand/left/input/aim/pose",
                                     "/user/hand/right/input/aim/pose" },
    { XRActionSlot::TriggerValue,    "/user/hand/left/input/select/click",
                                     "/user/hand/right/input/select/click" },
    { XRActionSlot::GripValue,       nullptr, nullptr },
    { XRActionSlot::Thumbstick,      nullptr, nullptr },
    { XRActionSlot::ThumbstickClick, nullptr, nullptr },
    { XRActionSlot::PrimaryButton,   nullptr, nullptr },
    { XRActionSlot::SecondaryButton, nullptr, nullptr },
    { XRActionSlot::MenuButton,      "/user/hand/left/input/menu/click",
                                     "/user/hand/right/input/menu/click" },
    { XRActionSlot::Haptic,          "/user/hand/left/output/haptic",
                                     "/user/hand/right/output/haptic" },
};

constexpr XRProfileBindings kProfileTables[] = {
    { XRInteractionProfile::MetaQuestTouch,
      "/interaction_profiles/oculus/touch_controller",
      kMetaQuestTouchEntries,
      sizeof(kMetaQuestTouchEntries) / sizeof(kMetaQuestTouchEntries[0]) },
    { XRInteractionProfile::ValveIndex,
      "/interaction_profiles/valve/index_controller",
      kValveIndexEntries,
      sizeof(kValveIndexEntries) / sizeof(kValveIndexEntries[0]) },
    { XRInteractionProfile::HtcVive,
      "/interaction_profiles/htc/vive_controller",
      kHtcViveEntries,
      sizeof(kHtcViveEntries) / sizeof(kHtcViveEntries[0]) },
    { XRInteractionProfile::MicrosoftMotionController,
      "/interaction_profiles/microsoft/motion_controller",
      kMicrosoftMotionEntries,
      sizeof(kMicrosoftMotionEntries) / sizeof(kMicrosoftMotionEntries[0]) },
    { XRInteractionProfile::KhronosSimpleController,
      "/interaction_profiles/khr/simple_controller",
      kKhronosSimpleEntries,
      sizeof(kKhronosSimpleEntries) / sizeof(kKhronosSimpleEntries[0]) },
};

} // namespace

const XRProfileBindings* GetProfileBindings(XRInteractionProfile profile, bool editorWorkspace) {
    // Index squeeze/value measures finger closure. Editor grabs need deliberate
    // pressure, while the project's gameplay bindings keep their original input.
    static constexpr auto editorIndexEntries = [] {
        auto entries = std::to_array(kValveIndexEntries);
        for (auto& entry : entries)if(entry.slot == XRActionSlot::GripValue) {
            entry.leftPath = "/user/hand/left/input/squeeze/force";
            entry.rightPath = "/user/hand/right/input/squeeze/force";
        }
        return entries;
    }();
    static const XRProfileBindings editorIndex = { XRInteractionProfile::ValveIndex,
        "/interaction_profiles/valve/index_controller", editorIndexEntries.data(), editorIndexEntries.size() };
    if(editorWorkspace && profile == XRInteractionProfile::ValveIndex)return &editorIndex;
    for (const XRProfileBindings& table : kProfileTables) {
        if (table.profile == profile) return &table;
    }
    return nullptr;
}

}
