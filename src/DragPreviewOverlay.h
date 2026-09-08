#pragma once
#include "Common.h"
// Unified editor drag/drop ghost preview.
//     if (DragPreview::BeginSource(flags)) {
//         ImGui::SetDragDropPayload(type, data, size);
//         DragPreview::SubmitMeta(displayName, icon, type);
//         DragPreview::EndSource();
//     }
namespace DragPreview {
    struct Settings {
        bool  enabled = true;
        float followStiffness       = 280.0f;
        float followDamping         = 22.0f;
        float cursorOffsetX         = 0.0f;
        float cursorOffsetY         = 10.0f;
        float rotationStiffness     = 70.0f;
        float rotationDamping       = 7.5f;
        float rotationDrive         = 0.018f;  // degrees of tilt per pixels per second of lateral velocity (I keep forgetting this lol.)
        float maxRotationDeg        = 14.0f;
        float scaleResponse         = 0.08f;
        float scaleSmoothing        = 11.0f;
        float popInStartScale       = 0.55f;
        float popInRiseDistance     = 14.0f;
        float popInFadeRate         = 9.0f;
        float floatAmplitude        = 1.4f;    // px
        float floatFrequency        = 2.4f;    // Hz
        float dropFallDistance      = 90.0f;
        float dropFadeDuration      = 0.50f;
        float dropExtraRotationDeg  = 22.0f;
        float dropEndScale          = 0.82f;
        float dropGravity           = 220.0f;
        float opacity               = 0.85f;
        float iconSize              = 18.0f;
        float cardPaddingX          = 10.0f;
        float cardPaddingY          = 6.0f;
        float iconGap               = 6.0f;
        float shadowOffset          = 4.0f;
        ImU32 cardColor             = IM_COL32( 34,  36,  54, 240);
        ImU32 cardBorderColor       = IM_COL32(110, 124, 196, 220);
        ImU32 cardShadowColor       = IM_COL32(  0,   0,   0, 110);
        ImU32 textColor             = IM_COL32(236, 238, 250, 255);
    };
    Settings& GetSettings();
    bool BeginSource(ImGuiDragDropFlags userFlags = 0);
    void EndSource();
    void SubmitMeta(const char* displayName, ImTextureID icon = (ImTextureID)0, const char* payloadType = nullptr,
                    ImVec2 iconUv0 = ImVec2(0.0f, 1.0f), ImVec2 iconUv1 = ImVec2(1.0f, 0.0f));
    void UpdateAndRender();
    void Cancel();
}