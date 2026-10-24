#pragma once
#include "Rendering.h"
#include "ThirdParty/ModuGUI/imgui.h"

namespace VREditorWidgets {
// Use the same shade classes as the desktop chrome, including disabled/held states.
inline bool IconButton(Renderer& renderer, const char* id, const char* path,
                       const char* label, bool selected, ImVec2 size = {72,72}) {
    const ImVec2 min=ImGui::GetCursorScreenPos(), max(min.x+size.x,min.y+size.y);
    const bool clicked=ImGui::InvisibleButton(id,size);
    const bool hovered=ImGui::IsItemHovered(), held=ImGui::IsItemActive();
    const auto state=ImGui::ShadeStateFromInteraction(hovered,held,selected);
    const auto color=selected?ImGuiCol_ButtonActive:hovered?ImGuiCol_ButtonHovered:ImGuiCol_Button;
    auto* draw=ImGui::GetWindowDrawList();
    ImGui::ShadeRect(draw,min,max,ImGui::GetColorU32(color),ImGuiShadeClass_Button,state,5);
    const ImU32 tint=ImGui::GetColorU32(ImVec4(1,1,1,1)); // Includes disabled alpha.
    if(auto* texture=path?renderer.getTexture(path,MaterialProperties::TextureFilter::Point):nullptr;
       texture && texture->GetID() && texture->GetWidth()>0 && texture->GetHeight()>0) {
        const float scale=std::min((size.x-16)/texture->GetWidth(),(size.y-16)/texture->GetHeight());
        const ImVec2 extent(texture->GetWidth()*scale,texture->GetHeight()*scale);
        const ImVec2 at(min.x+(size.x-extent.x)*.5f,min.y+(size.y-extent.y)*.5f);
        draw->AddImage((ImTextureID)(intptr_t)texture->GetID(),at,{at.x+extent.x,at.y+extent.y},{0,1},{1,0},tint);
    } else {
        const auto extent=ImGui::CalcTextSize(label);
        draw->AddText({min.x+(size.x-extent.x)*.5f,min.y+(size.y-extent.y)*.5f},ImGui::GetColorU32(ImGuiCol_Text),label);
    }
    ImGui::ReportItemHoverFeedback();
    if(clicked)ImGui::ReportWidgetFeedback(ImGuiWidgetFeedback_Button);
    if(hovered)ImGui::SetTooltip("%s",label);
    return clicked;
}
}
