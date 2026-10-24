#include "VREditorWorkspace.h"
#if !MODULARITY_RUNTIME_ONLY
#include "ModelLoader.h"
#include "ScenePicking.h"
#include "Camera.h"
#include "ThirdParty/ModuGUI/imgui_internal.h"
#include "ThirdParty/ModuGUI/backends/imgui_impl_opengl3.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cfloat>
#include <cctype>
#include "../../include/Platform/AssetSource.h"

namespace {
using namespace Modularity::XR;
struct GLState {
    GLint draw, read, viewport[4], shader, array, buffer, textureUnit, depthFunc, cullMode, frontFace, renderbuffer;
    GLboolean blend, depth, cull, scissor, depthMask;
    GLState() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
        glGetIntegerv(GL_VIEWPORT,viewport); glGetIntegerv(GL_CURRENT_PROGRAM,&shader);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&array); glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE,&textureUnit);
        glGetIntegerv(GL_DEPTH_FUNC,&depthFunc);glGetIntegerv(GL_CULL_FACE_MODE,&cullMode);
        glGetIntegerv(GL_FRONT_FACE,&frontFace);glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);
        blend=glIsEnabled(GL_BLEND); depth=glIsEnabled(GL_DEPTH_TEST);
        cull=glIsEnabled(GL_CULL_FACE); scissor=glIsEnabled(GL_SCISSOR_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK,&depthMask);
    }
    ~GLState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw); glBindFramebuffer(GL_READ_FRAMEBUFFER,read);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]); glUseProgram(shader);
        glBindVertexArray(array); glBindBuffer(GL_ARRAY_BUFFER,buffer); glActiveTexture(textureUnit);
        auto restore=[](GLenum flag,GLboolean value){ if(value)glEnable(flag);else glDisable(flag); };
        restore(GL_BLEND,blend); restore(GL_DEPTH_TEST,depth); restore(GL_CULL_FACE,cull); restore(GL_SCISSOR_TEST,scissor);
        glDepthMask(depthMask);
        glDepthFunc(depthFunc);glCullFace(cullMode);glFrontFace(frontFace);glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
    }
};
bool pressed(const XRControllerState& hand, XRButton button) {
    return hand.active && hand.buttons[static_cast<size_t>(button)];
}
bool editorGrip(const XRControllerState& hand,float threshold,bool wasPressed) {
    if(hand.interactionProfile!="/interaction_profiles/valve/index_controller")return pressed(hand,XRButton::Grip);
    return hand.active && hand.axes[static_cast<size_t>(XRAxisKind::Grip)] >= threshold*(wasPressed?.65f:1.f);
}
bool validLayoutName(const std::string& name) {
    return !name.empty() && name.size()<=48 && name.front()!=' ' && name.back()!=' ' && name!="." && name!=".." &&
        std::all_of(name.begin(),name.end(),[](unsigned char c){return std::isalnum(c) || c==' ' || c=='_' || c=='-';});
}
constexpr const char* layoutFiles[]={"vr-docking.ini","vr-surfaces.txt","vr-anchor.txt","vr-input.txt"};
glm::mat4 poseMatrix(const XRPose& pose) {
    return glm::translate(glm::mat4(1),pose.position)*glm::mat4_cast(pose.rotation);
}
glm::mat4 surfaceMatrix(const VREditorWorkspace::Surface& surface) {
    return glm::translate(glm::mat4(1),surface.position)*glm::mat4_cast(surface.rotation);
}
ImGuiWindow* displayRoot(ImGuiWindow* window) {
    return window->RootWindowDockTree ? window->RootWindowDockTree : window->RootWindow;
}
}

VREditorWorkspace::~VREditorWorkspace() { shutdown(); }

void VREditorWorkspace::renderPreviewImage(ImVec2 size) {
    size={std::max(1.f,size.x),std::max(1.f,size.y)};
    const auto top=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##VRMirror",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)mirror.texture,top,{top.x+size.x,top.y+size.y},{0,1},{1,0});
    if(!preview)return;
    const auto& io=ImGui::GetIO();
    const bool over=!io.AppFocusLost && (ImGui::IsItemHovered() || ImGui::IsItemActive());
    if(over) {
        const auto mouse=ImGui::GetMousePos();
        const float x=(mouse.x-top.x)/size.x*2-1,y=1-(mouse.y-top.y)/size.y*2;
        const float tangent=std::tan(glm::radians(95.f)*.5f);
        previewDirection=glm::normalize(glm::vec3(x*tangent*1.6f,y*tangent,-1));
    }
    previewPointerActive=over;
    previewTrigger=over && ImGui::IsMouseDown(0);
    previewGrip=over && ImGui::IsMouseDown(1);
}

Modularity::XR::XRInputState VREditorWorkspace::previewInputState() const {
    Modularity::XR::XRInputState state;
    state.sessionActive=state.hasFocus=!ImGui::GetIO().AppFocusLost;
    state.headPose.position={0,1.6f,0};state.headPose.positionValid=state.headPose.orientationValid=true;
    auto& hand=state.controllers[1];hand.active=previewPointerActive;
    hand.aimPose=state.headPose;
    hand.aimPose.rotation=glm::quatLookAt(previewDirection,glm::vec3(0,1,0));
    hand.gripPose=hand.aimPose;hand.gripPose.position+=previewDirection*.35f;
    hand.buttons[static_cast<size_t>(XRButton::Trigger)]=previewTrigger;
    hand.buttons[static_cast<size_t>(XRButton::Grip)]=previewGrip;
    return state;
}

bool VREditorWorkspace::intersect(const Surface& surface, glm::vec3 origin, glm::vec3 direction,
                                 float& distance, glm::vec2& uv, bool bounded) {
    const glm::quat inverse=glm::inverse(surface.rotation);
    const glm::vec3 p=inverse*(origin-surface.position), d=inverse*direction;
    if (d.z>=-0.0001f) return false; // Only the readable front face accepts input.
    distance=-p.z/d.z;
    if (distance<0 || distance>20.0f || surface.size.x<=0 || surface.size.y<=0) return false;
    const glm::vec3 at=p+d*distance;
    uv={at.x/surface.size.x+0.5f,0.5f-at.y/surface.size.y};
    return !bounded || (uv.x>=0 && uv.y>=0 && uv.x<=1 && uv.y<=1);
}

void VREditorWorkspace::setStorage(const std::filesystem::path& path) {
    if(storage==path)return;
    save(); storage=path; iniPath=(storage/"vr-docking.ini").string();
    savedSurfaces.clear();
    grabForce=.36f;smoothHands=true;
    std::ifstream settings(storage/"vr-input.txt");
    float force=.36f;int smoothing=1;
    if(settings>>force>>smoothing && std::isfinite(force)) {grabForce=std::clamp(force,.1f,1.f);smoothHands=smoothing!=0;}
    std::ifstream anchorFile(storage/"vr-anchor.txt");
    savedAnchorValid=bool(anchorFile>>savedAnchor.x>>savedAnchor.y>>savedAnchor.z>>savedAnchorYaw) &&
        std::isfinite(savedAnchor.x) && std::isfinite(savedAnchor.y) && std::isfinite(savedAnchor.z) && std::isfinite(savedAnchorYaw);
    std::ifstream file(storage/"vr-surfaces.txt");
    unsigned int id; Surface s;
    while(file>>id>>s.position.x>>s.position.y>>s.position.z>>s.rotation.w>>s.rotation.x>>s.rotation.y>>s.rotation.z>>s.size.x>>s.size.y) {
        if(!std::isfinite(s.position.x)||!std::isfinite(s.position.y)||!std::isfinite(s.position.z) ||
           !std::isfinite(s.size.x)||!std::isfinite(s.size.y)||s.size.x<0.15f||s.size.x>5||s.size.y<0.03f||s.size.y>5 ||
           !std::isfinite(glm::length(s.rotation))||glm::length(s.rotation)<0.5f) continue;
        s.rotation=glm::normalize(s.rotation); savedSurfaces[id]=s;
    }
    if(ui) { auto* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(ui);
        ImGui::DockBuilderRemoveNode(0); ImGui::ClearIniSettings();
        ImGui::GetIO().IniFilename=iniPath.c_str(); ImGui::LoadIniSettingsFromDisk(iniPath.c_str()); ImGui::SetCurrentContext(previous); }
    for(auto& panel:panels)releaseTarget(panel.target);
    panels.clear();visibleScratch.clear();ownerScratch.clear();
    draggedPanel=hoveredPanel=mousePanel=0;
    initializedLayout=!savedSurfaces.empty() && std::filesystem::exists(storage/"vr-docking.ini");
    navigationInitialized=false;
    restoreLayoutPending=true;handFilters={};editorGrips={};
    mouseDown=gripDown=rightDown=padTouched=false;capturedHand=-1;
}

void VREditorWorkspace::save() {
    if(storage.empty() || !ui)return;
    std::error_code ec; std::filesystem::create_directories(storage,ec);
    if(ec)return;
    auto* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(ui);
    ImGui::SaveIniSettingsToDisk(iniPath.c_str()); ImGui::SetCurrentContext(previous);
    for(const auto& panel:panels) if(!panel.popup && !panel.wrist)savedSurfaces[panel.id]=panel.surface;
    std::ofstream file(storage/"vr-surfaces.txt");
    file<<std::setprecision(8);
    for(const auto& [id,s]:savedSurfaces)
        file<<id<<' '<<s.position.x<<' '<<s.position.y<<' '<<s.position.z<<' '
            <<s.rotation.w<<' '<<s.rotation.x<<' '<<s.rotation.y<<' '<<s.rotation.z<<' '<<s.size.x<<' '<<s.size.y<<'\n';
    std::ofstream anchorFile(storage/"vr-anchor.txt");
    anchorFile<<std::setprecision(8)<<anchor.x<<' '<<anchor.y<<' '<<anchor.z<<' '<<anchorYaw<<'\n';
    std::ofstream settings(storage/"vr-input.txt");settings<<grabForce<<' '<<int(smoothHands)<<'\n';
}

std::vector<std::string> VREditorWorkspace::layoutNames() const {
    std::vector<std::string> names;std::error_code ec;
    for(const auto& entry:std::filesystem::directory_iterator(storage/"Layouts",ec)) {
        if(entry.is_directory(ec) && std::filesystem::exists(entry.path()/"vr-docking.ini",ec))names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(),names.end());return names;
}

bool VREditorWorkspace::saveLayout(const std::string& name) {
    if(storage.empty() || !ui || !validLayoutName(name))return false;
    save();std::error_code ec;const auto path=storage/"Layouts"/name;
    std::filesystem::create_directories(path,ec);if(ec)return false;
    for(const auto* file:layoutFiles) {
        std::filesystem::copy_file(storage/file,path/file,std::filesystem::copy_options::overwrite_existing,ec);
        if(ec)return false;
    }
    return true;
}

bool VREditorWorkspace::loadLayout(const std::string& name) {
    if(storage.empty() || !validLayoutName(name))return false;
    const auto path=storage/"Layouts"/name;std::error_code ec;
    for(const auto* file:layoutFiles)if(!std::filesystem::exists(path/file,ec) || ec)return false;
    save();
    for(const auto* file:layoutFiles) {
        std::filesystem::copy_file(path/file,storage/file,std::filesystem::copy_options::overwrite_existing,ec);
        if(ec)return false;
    }
    clearGizmo();const auto previous=storage;storage.clear();setStorage(previous);return true;
}

void VREditorWorkspace::stabilizeInput(float dt) {
    for(int h=0;h<2;++h) {
        auto& hand=input.controllers[h];auto& filter=handFilters[h];
        editorGrips[h]=editorGrip(hand,grabForce,editorGrips[h]);
        if(!preview)hand.buttons[static_cast<size_t>(XRButton::Grip)]=editorGrips[h];
        if(preview || !smoothHands || !input.hasFocus || !input.sessionActive || !hand.active || !hand.aimPose.isValid() || !hand.gripPose.isValid()) {
            filter={};continue;
        }
        const bool click=pressed(hand,XRButton::Trigger) || hand.editorTouchpadClicked,grip=pressed(hand,XRButton::Grip);
        if(!filter.valid) {filter.aim=hand.aimPose;filter.grip=hand.gripPose;filter.valid=true;}
        const auto previousAim=filter.aim,previousGrip=filter.grip;
        auto smooth=[&](XRPose& filtered,const XRPose& raw,float tau) {
            const float a=1.f-std::exp(-std::clamp(dt,.001f,.1f)/tau);
            filtered.position=glm::mix(filtered.position,raw.position,a);
            filtered.rotation=glm::normalize(glm::slerp(filtered.rotation,raw.rotation,a));
        };
        smooth(filter.aim,hand.aimPose,.022f);smooth(filter.grip,hand.gripPose,.018f);
        if(click && !filter.clickHeld){filter.clickAnchor=previousAim;filter.clickLocked=true;}
        if(grip && !filter.gripHeld){filter.gripAnchor=previousGrip;filter.gripLocked=true;}
        auto moved=[](const XRPose& pose,const XRPose& anchor) {
            return glm::distance(pose.position,anchor.position)>.006f ||
                std::abs(glm::dot(pose.rotation,anchor.rotation))<std::cos(glm::radians(.45f)*.5f);
        };
        if(!click || moved(hand.aimPose,filter.clickAnchor))filter.clickLocked=false;
        if(!grip || moved(hand.gripPose,filter.gripAnchor))filter.gripLocked=false;
        const auto aim=filter.clickLocked?filter.clickAnchor:filter.aim;
        const auto heldGrip=filter.gripLocked?filter.gripAnchor:filter.grip;
        hand.aimPose.position=aim.position;hand.aimPose.rotation=aim.rotation;
        hand.gripPose.position=heldGrip.position;hand.gripPose.rotation=heldGrip.rotation;
        filter.clickHeld=click;filter.gripHeld=grip;
    }
}

void VREditorWorkspace::recenter(const XRInputState& state) {
    if(!state.headPose.isValid())return;
    anchor=state.headPose.position;
    glm::vec3 forward=state.headPose.rotation*glm::vec3(0,0,-1);
    anchorYaw=std::atan2(-forward.x,-forward.z);
    savedSurfaces.clear();
    for(auto& panel:panels) {
        if(auto* root=ImGui::FindWindowByID(panel.id))panel.surface=defaultSurface(root);
    }
    recenterPending=false;
}

VREditorWorkspace::Surface VREditorWorkspace::defaultSurface(const ImGuiWindow* root) const {
    Surface s; glm::vec3 offset(0,0,-1.5f); glm::vec3 rotation(0);
    s.size={std::clamp(root->Size.x/760.0f,0.25f,2.1f),std::clamp(root->Size.y/760.0f,0.08f,1.4f)};
    const ImGuiDockNode* dock=root->DockNodeAsHost;
    const ImGuiID dockId=dock?dock->ID:0;
    if(dockId==0x56520001) {offset={-1.02f,0,-1.05f};rotation.y=40; s.size={0.82f,1.08f};}
    else if(dockId==0x56520002){offset={1.02f,0,-1.05f};rotation.y=-40;s.size={0.82f,1.08f};}
    else if(dockId==0x56520003){offset={0,-0.58f,-1.25f};rotation.x=-23;s.size={1.5f,0.54f};}
    else if(dockId==0x56520004){offset={0,0.53f,-1.6f};rotation.x=12;s.size={1.35f,0.6f};}
    else if(std::string(root->Name).find("VR Management")!=std::string::npos){offset={0,-0.98f,-1.0f};rotation.x=-35;s.size={1.30f,0.30f};}
    else if(std::string(root->Name)=="Launcher"){offset={0,0,-1.6f};s.size={2.1f,1.3125f};}
    else if(std::string(root->Name).find("VR Keyboard")!=std::string::npos){offset={0,-0.70f,-0.8f};rotation.x=-35;s.size={1.05f,0.38f};}
    else {offset={0.18f,0.04f,-1.15f};}
    const glm::quat yaw=glm::angleAxis(anchorYaw,glm::vec3(0,1,0));
    s.position=anchor+yaw*offset;
    s.rotation=yaw*glm::quat(glm::radians(rotation));
    const float density=std::min(s.size.x/std::max(1.f,root->Size.x),s.size.y/std::max(1.f,root->Size.y));
    s.size=glm::vec2(root->Size.x,root->Size.y)*density;
    return s;
}

void VREditorWorkspace::beginUI(const XRInputState& state, float dt, bool launcher) {
    desktop=ImGui::GetCurrentContext();
    if(!ui) {
        ui=ImGui::CreateContext(desktop->IO.Fonts);
        ImGui::SetCurrentContext(ui);
        auto& io=ImGui::GetIO();
        io.ConfigFlags=ImGuiConfigFlags_DockingEnable;
        io.BackendFlags=desktop->IO.BackendFlags & (ImGuiBackendFlags_RendererHasVtxOffset|ImGuiBackendFlags_RendererHasTextures);
        io.BackendRendererUserData=desktop->IO.BackendRendererUserData;
        io.BackendRendererName=desktop->IO.BackendRendererName;
        io.FontDefault=desktop->IO.FontDefault;
        io.IniFilename=iniPath.empty()?nullptr:iniPath.c_str();
        io.ConfigDockingAlwaysTabBar=true; io.ConfigDockingWithShift=false;
        io.ConfigWindowsMoveFromTitleBarOnly=true;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth=desktop->PlatformIO.Renderer_TextureMaxWidth;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight=desktop->PlatformIO.Renderer_TextureMaxHeight;
    } else ImGui::SetCurrentContext(ui);
    ImGui::GetStyle()=desktop->Style;
    ImGui::SetShadeTheme(desktop->ShadeTheme);
    input=state;
    stabilizeInput(dt);
    auto& io=ImGui::GetIO();
    io.DisplaySize=launcher?ImVec2(1600,1000):ImVec2(4096,3072);
    io.DisplayFramebufferScale={1,1}; io.DeltaTime=std::clamp(dt,0.001f,0.1f);
    io.AddFocusEvent(state.hasFocus);
    // Physical keyboard remains usable while the headset is worn.
    for(const auto& event:desktop->InputEventsTrail) {
        if(event.Type==ImGuiInputEventType_Text) io.AddInputCharacter(event.Text.Char);
        else if(event.Type==ImGuiInputEventType_Key) io.AddKeyEvent(event.Key.Key,event.Key.Down);
    }
    if(restoreLayoutPending && state.headPose.isValid()) {
        anchor=state.headPose.position;
        const auto forward=state.headPose.rotation*glm::vec3(0,0,-1);
        anchorYaw=std::atan2(-forward.x,-forward.z);
        if(savedAnchorValid) {
            const auto rotation=glm::angleAxis(anchorYaw-savedAnchorYaw,glm::vec3(0,1,0));
            for(auto& [id,surface]:savedSurfaces) {
                surface.position=anchor+rotation*(surface.position-savedAnchor);
                surface.rotation=glm::normalize(rotation*surface.rotation);
            }
        }
        for(auto& panel:panels) {
            if(auto saved=savedSurfaces.find(panel.id);saved!=savedSurfaces.end())panel.surface=saved->second;
            else if(auto* root=ImGui::FindWindowByID(panel.id))panel.surface=defaultSurface(root);
        }
        restoreLayoutPending=false;recenterPending=false;
    } else if(recenterPending)recenter(state);
    // Auto-docked optional panels can inherit a 32px-wide first-use window.
    // Repair the dock's pixel size rather than widening its texture in 3D.
    for(auto& entry:ui->DockContext.Nodes.Data)if(auto* node=static_cast<ImGuiDockNode*>(entry.val_p)) {
        if(node->ParentNode || node->IsDockSpace())continue;
        if(node->Size.x<360 || node->Size.y<220)
            ImGui::DockBuilderSetNodeSize(node->ID,{std::max(360.f,node->Size.x),std::max(220.f,node->Size.y)});
    }
    updateWristTools();
    updateWristSurfaces();
    feedPointer(dt);
    ImGui::NewFrame();
    saveClock+=dt;
}

void VREditorWorkspace::updateWristTools() {
    const auto& left=input.controllers[0];
    const bool tracked=wristToolsVisible && input.hasFocus && input.sessionActive && left.active && left.gripPose.isValid();
    toolStep=0;
    if(!tracked) { toolStickLatched=false;return; }
    const float x=left.axes2D[static_cast<size_t>(XRAxis2DKind::Thumbstick)].x;
    if(std::abs(x)<.25f)toolStickLatched=false;
    // Grip+stick remains the panel distance/size gesture. Never cycle while dragging.
    if(pressed(left,XRButton::PrimaryButton) && !pressed(left,XRButton::Grip) && !pressed(left,XRButton::Trigger) && !draggedPanel &&
       !toolStickLatched && std::abs(x)>.65f) {
        toolStep=x>0?1:-1;toolStickLatched=true;
        RequestHapticPulse({XRDevice::Left,.18f,.025f,0});
    }
}

void VREditorWorkspace::updateNavigation(const XRInputState& state,float dt) {
    if(!navigationInitialized || !wristToolsVisible || !state.hasFocus || !state.sessionActive || !state.headPose.isValid())return;
    const auto& left=state.controllers[0];const auto& right=state.controllers[1];
    // Holding a panel or choosing a tool owns the sticks until that gesture ends.
    if(draggedPanel || editorGrip(left,grabForce,editorGrips[0]) || editorGrip(right,grabForce,editorGrips[1]) ||
       pressed(left,XRButton::Trigger) || pressed(right,XRButton::Trigger) || pressed(left,XRButton::PrimaryButton))return;
    dt=std::clamp(dt,0.f,.05f);
    auto axis=[](float value) { const float magnitude=std::abs(value);return magnitude<.2f?0.f:std::copysign(std::min(1.f,(magnitude-.2f)/.8f),value); };
    const glm::vec2 move=left.active?left.axes2D[0]:glm::vec2(0);
    glm::vec2 stick(axis(move.x),axis(move.y));
    if(glm::length(stick)>1)stick=glm::normalize(stick);
    // Fly in the direction of the headset, including pitch and roll. Flattening
    // this vector onto Y=0 made looking up/down have no effect on movement.
    const auto heading=glm::mat3(sceneFromTracking)*glm::mat3_cast(state.headPose.rotation);
    const auto forward=glm::normalize(heading*glm::vec3(0,0,-1));
    const auto strafe=glm::normalize(heading*glm::vec3(1,0,0));
    sceneFromTracking[3]+=glm::vec4((strafe*stick.x+forward*stick.y)*(2.f*dt),0);
    const float turn=right.active?axis(right.axes2D[0].x):0;
    if(turn!=0) {
        const glm::vec3 pivot=glm::vec3(sceneFromTracking*glm::vec4(state.headPose.position,1));
        sceneFromTracking=glm::translate(glm::mat4(1),pivot)*glm::rotate(glm::mat4(1),-turn*glm::radians(90.f)*dt,glm::vec3(0,1,0))*
            glm::translate(glm::mat4(1),-pivot)*sceneFromTracking;
    }
}

void VREditorWorkspace::updateWristSurfaces() {
    const auto& left=input.controllers[0];
    const bool tracked=wristToolsVisible && input.hasFocus && input.sessionActive && left.active && left.gripPose.isValid();
    for(auto& panel:panels)if(panel.wrist) {
        panel.visible=panel.visible && tracked;
        if(!tracked)continue;
        // Position follows the grip; face the wearer so the strip stays readable.
        panel.surface.position=left.gripPose.position+glm::vec3(0,.16f,0);
        const auto facing=input.headPose.position-panel.surface.position;
        if(input.headPose.isValid() && glm::length(facing)>.05f) {
            const auto forward=glm::normalize(facing);
            const auto up=std::abs(forward.y)>.97f?glm::vec3(0,0,1):glm::vec3(0,1,0);
            panel.surface.rotation=glm::quatLookAt(-forward,up);
        }
        panel.surface.size={.40f,.40f*panel.uiSize.y/std::max(1.f,panel.uiSize.x)};
    }
}

VREditorWorkspace::Panel* VREditorWorkspace::findPanel(unsigned int id) {
    for(auto& panel:panels)if(panel.id==id)return &panel;
    return nullptr;
}

void VREditorWorkspace::feedPointer(float dt) {
    auto& io=ImGui::GetIO();
    keyboardPress=false;
    scenePickPending=false;
    hit=false;hoveredPanel=0;
    ImGui::SetMouseHitTestRoot(0);
    if(!input.hasFocus || !input.sessionActive) {
        io.AddMouseButtonEvent(0,false);io.AddMouseButtonEvent(1,false); io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);
        mouseDown=gripDown=rightDown=padTouched=false; draggedPanel=0; capturedHand=-1; return;
    }
    // Keep one hand in charge until its click/drag ends. No cross-hand chatter.
    const int previousHand=pointerHand;
    if(capturedHand>=0)pointerHand=capturedHand;
    else for(int h=0;h<2;++h) if(pressed(input.controllers[h],XRButton::Trigger)||pressed(input.controllers[h],XRButton::Grip)||input.controllers[h].editorTouchpadTouched||input.controllers[h].editorTouchpadClicked)pointerHand=h;
    if(!input.controllers[pointerHand].active)pointerHand=1-pointerHand;
    if(previousHand!=pointerHand)padTouched=false;
    const auto& hand=input.controllers[pointerHand];
    if(!hand.active || !hand.aimPose.isValid()) {
        io.AddMouseButtonEvent(0,false);io.AddMouseButtonEvent(1,false);io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);
        mouseDown=gripDown=rightDown=padTouched=false;draggedPanel=0;capturedHand=-1;return;
    }
    const glm::vec3 origin=hand.aimPose.position, direction=hand.aimPose.rotation*glm::vec3(0,0,-1);
    const bool trigger=pressed(hand,XRButton::Trigger), grip=pressed(hand,XRButton::Grip);
    const bool right=hand.editorTouchpadClicked;
    if((trigger||grip||right)&&capturedHand<0)capturedHand=pointerHand;
    if(!trigger&&!grip&&!right)capturedHand=-1;
    hit=false; hoveredPanel=0;
    float nearest=20;glm::vec2 uv(0);
    // Reverse order gives popups priority over their parent at near-equal depth.
    for(auto it=panels.rbegin();it!=panels.rend();++it) {
        // Tooltips must not steal their source widget's hover on the next frame.
        const auto* window=ImGui::FindWindowByID(it->id);
        if(window && (window->Flags&ImGuiWindowFlags_NoMouseInputs))continue;
        float distance;glm::vec2 candidate;
        if(!gizmoWasUsing && it->visible && intersect(it->surface,origin,direction,distance,candidate) && distance<nearest) {
            nearest=distance;hoveredPanel=it->id;uv=candidate;hit=true;
        }
    }
    hitPoint=origin+direction*(hit?nearest:2.0f);
    if(wristToolsVisible && trigger && !mouseDown && !grip && !hit && !draggedPanel && ui->OpenPopupStack.empty()) {
        scenePickPending=true;
        scenePickOrigin=glm::vec3(sceneFromTracking*glm::vec4(origin,1));
        scenePickDirection=glm::normalize(glm::mat3(sceneFromTracking)*direction);
    }
    if(grip && !gripDown && hit && hand.gripPose.isValid() && !findPanel(hoveredPanel)->wrist && !findPanel(hoveredPanel)->popup) {
        draggedPanel=hoveredPanel;
        gripFromPanel=glm::inverse(poseMatrix(hand.gripPose))*surfaceMatrix(findPanel(draggedPanel)->surface);
        previewDragPlane=findPanel(draggedPanel)->surface;
        previewDragAnchor=hitPoint;
        RequestHapticPulse({pointerHand?XRDevice::Right:XRDevice::Left,0.2f,0.03f,0});
    }
    if(draggedPanel && grip && hand.gripPose.isValid()) {
        if(auto* panel=findPanel(draggedPanel)) {
            const glm::vec2 stick=hand.axes2D[static_cast<size_t>(XRAxis2DKind::Thumbstick)];
            if(std::abs(stick.y)>0.18f)gripFromPanel[3].z-=stick.y*dt*0.8f;
            if(preview) {
                float distance;glm::vec2 at;
                if(intersect(previewDragPlane,origin,direction,distance,at,false))
                    panel->surface.position=previewDragPlane.position+(origin+direction*distance-previewDragAnchor);
                panel->surface.rotation=previewDragPlane.rotation;
            } else {
                const glm::mat4 world=poseMatrix(hand.gripPose)*gripFromPanel;
                panel->surface.position=glm::vec3(world[3]);panel->surface.rotation=glm::normalize(glm::quat_cast(glm::mat3(world)));
            }
            if(std::abs(stick.x)>0.18f) {
                const float minimum=.25f/std::max(panel->surface.size.x,panel->surface.size.y);
                const float maximum=std::min(3.5f/panel->surface.size.x,2.5f/panel->surface.size.y);
                panel->surface.size*=std::clamp(std::exp(stick.x*dt*0.7f),std::min(minimum,maximum),maximum);
            }
        }
    }
    if(!grip && draggedPanel){draggedPanel=0;save();}
    // Only a gesture that started on UI may keep its panel's unbounded plane.
    // A previous hover/click must never reclaim a later scene gizmo drag.
    if((!trigger&&!right) || (!mouseDown&&!rightDown))mousePanel=0;
    if(hit && (trigger||right))mousePanel=hoveredPanel;
    else if(((trigger && mouseDown)||(right && rightDown)) && !gizmoWasUsing) {
        if(auto* panel=findPanel(mousePanel)) {
            float distance; hit=intersect(panel->surface,origin,direction,distance,uv,false);
            if(hit)hoveredPanel=mousePanel;
        }
    }
    if(hit && !draggedPanel) {
        if(auto* panel=findPanel(hoveredPanel))io.AddMousePosEvent(panel->uiPosition.x+uv.x*panel->uiSize.x,panel->uiPosition.y+uv.y*panel->uiSize.y);
        const auto stick=hand.axes2D[0];
        if(!trigger && !grip && !right) {
            if(hand.editorTouchpadTouched && padTouched) {
                const auto delta=glm::clamp(hand.editorTouchpad-padPosition,glm::vec2(-.5f),glm::vec2(.5f));
                io.AddMouseWheelEvent(delta.x*3.f,delta.y*5.f);
            } else if(!hand.editorTouchpadTouched && (pointerHand==1 || !wristToolsVisible) && std::abs(stick.y)>0.18f)
                io.AddMouseWheelEvent(0,stick.y*dt*7);
        }
    } else io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);
    const auto* pointed=findPanel(hoveredPanel);
    if(hit && pointed)ImGui::SetMouseHitTestRoot(pointed->id);
    const bool overKeyboard=pointed && pointed->name.find("VR Keyboard")!=std::string::npos &&
        uv.y*pointed->uiSize.y > ImGui::GetFrameHeight()+2;
    keyboardPress=overKeyboard && trigger && !mouseDown;
    // Keyboard keys inject text without clicking away from the active text field.
    io.AddMouseButtonEvent(0,trigger && !grip && !overKeyboard);
    io.AddMouseButtonEvent(1,right && !grip && !gizmoWasUsing);
    mouseDown=trigger;gripDown=grip;rightDown=right;
    padTouched=hand.editorTouchpadTouched;padPosition=hand.editorTouchpad;
}

bool VREditorWorkspace::takeScenePick(glm::vec3& origin,glm::vec3& direction) {
    if(!scenePickPending)return false;
    scenePickPending=false;origin=scenePickOrigin;direction=scenePickDirection;return true;
}

void VREditorWorkspace::endUI() {
    disabledItem=ui->HoveredIdIsDisabled;
    ImGui::Render(); capturePanels();
    if(saveClock>10){saveClock=0;save();}
    ImGui::SetCurrentContext(desktop);
}

void VREditorWorkspace::ensureTarget(Target& target,int width,int height,bool depth) {
    width=std::clamp(width,1,4096);height=std::clamp(height,1,3072);
    if(target.fbo && target.width==width && target.height==height && (target.depth!=0)==depth)return;
    releaseTarget(target);target.width=width;target.height=height;
    glGenFramebuffers(1,&target.fbo);glGenTextures(1,&target.texture);
    glBindTexture(GL_TEXTURE_2D,target.texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER,target.fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target.texture,0);
    if(depth) {
        glGenRenderbuffers(1,&target.depth);glBindRenderbuffer(GL_RENDERBUFFER,target.depth);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,width,height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,target.depth);
    }
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)releaseTarget(target);
}
void VREditorWorkspace::releaseTarget(Target& target) {
    if(target.texture)glDeleteTextures(1,&target.texture);if(target.depth)glDeleteRenderbuffers(1,&target.depth);if(target.fbo)glDeleteFramebuffers(1,&target.fbo);target={};
}

void VREditorWorkspace::capturePanels() {
    GLState restore;
    const auto* data=ImGui::GetDrawData(); if(!data || !data->Valid)return;
    for(auto& panel:panels)panel.visible=false;
    ownerScratch.clear();
    for(auto* window:ui->Windows)if(window->Active && !window->Hidden &&
        std::find(data->CmdLists.begin(),data->CmdLists.end(),window->DrawList)!=data->CmdLists.end())
        ownerScratch.emplace_back(window->DrawList,displayRoot(window));
    for(const auto& [list,root]:ownerScratch) {
        if(!root || root->Size.x<2 || root->Size.y<2 || (std::string(root->Name)=="Debug##Default" || std::string(root->Name)=="VR Scene Gizmo"))continue;
        // Loading-screen dimmers fill the virtual desktop but are not windows
        // in the room. Keep their actual loading card as the spatial surface.
        if((root->Flags&ImGuiWindowFlags_NoInputs)==ImGuiWindowFlags_NoInputs &&
           (root->Flags&ImGuiWindowFlags_NoSavedSettings) && !(root->Flags&(ImGuiWindowFlags_Popup|ImGuiWindowFlags_Tooltip)) &&
           root->Size.x>=ImGui::GetIO().DisplaySize.x*.9f && root->Size.y>=ImGui::GetIO().DisplaySize.y*.9f)continue;
        auto* panel=findPanel(root->ID);
        if(panel && panel->visible)continue;
        const bool newlyCaptured=panel==nullptr;
        const bool moving=ui->MovingWindow && displayRoot(ui->MovingWindow)==root && mouseDown && !draggedPanel;
        if(!panel) {
            Panel added;added.id=root->ID;added.name=root->Name;added.surface=defaultSurface(root);
            added.popup=(root->Flags&(ImGuiWindowFlags_Popup|ImGuiWindowFlags_Tooltip))!=0;
            added.wrist=std::string(root->Name)=="VR Hand Tools";
            if(added.popup) {
                // Popup transforms are resolved after all parent surfaces below.
            } else if(moving) {
                // A torn-off tab inherits its source panel's physical location
                // and pixel density before following the native dock drag.
                if(auto* source=findPanel(mousePanel)) {
                    const float density=source->surface.size.x/source->uiSize.x;
                    added.surface.rotation=source->surface.rotation;
                    added.surface.size=glm::vec2(root->Size.x,root->Size.y)*density;
                    const ImVec2 delta(root->Pos.x+root->Size.x*.5f-source->uiPosition.x-source->uiSize.x*.5f,
                                       root->Pos.y+root->Size.y*.5f-source->uiPosition.y-source->uiSize.y*.5f);
                    added.surface.position=source->surface.position+source->surface.rotation*glm::vec3(delta.x*density,-delta.y*density,.004f);
                }
            } else if(!added.wrist)if(auto it=savedSurfaces.find(root->ID);it!=savedSurfaces.end())added.surface=it->second;
            added.uiSize=root->Size;
            added.uiPosition=root->Pos;
            panels.push_back(std::move(added)); panel=&panels.back();
        }
        if(moving && !panel->wrist && !panel->popup) {
            const auto& pose=input.controllers[pointerHand].aimPose;
            const auto direction=pose.rotation*glm::vec3(0,0,-1);
            float distance;glm::vec2 uv;
            if(newlyCaptured || ui->ActiveIdIsJustActivated) {
                previewDragPlane=panel->surface;
                // Native docking coordinates can jump between unrelated panel
                // rectangles. Keep the physical drag on a head-facing plane.
                if(input.headPose.isValid())previewDragPlane.rotation=input.headPose.rotation;
                if(intersect(previewDragPlane,pose.position,direction,distance,uv,false))
                    previewDragAnchor=pose.position+direction*distance;
            }
            if(intersect(previewDragPlane,pose.position,direction,distance,uv,false))
                panel->surface.position=previewDragPlane.position+(pose.position+direction*distance-previewDragAnchor);
        }
        if(!panel->popup && panel->uiSize.x>0 && panel->uiSize.y>0) {
            // Use one pixels-to-meters ratio, including for older saved poses.
            // Independent axis clamps used to stretch narrow/oversized windows.
            const float density=std::min(panel->surface.size.x/panel->uiSize.x,panel->surface.size.y/panel->uiSize.y);
            panel->surface.size=glm::vec2(root->Size.x,root->Size.y)*density;
            const float fit=std::min({1.f,3.5f/panel->surface.size.x,2.5f/panel->surface.size.y});
            panel->surface.size*=fit;
        }
        panel->visible=true;panel->uiPosition=root->Pos;panel->uiSize=root->Size;
    }
    // Auto-fit popups change size for several frames after opening. Derive
    // their pose from their actual Begin parent every frame, never from whichever
    // panel was last clicked. Nested menus inherit the same pixel density.
    for(int pass=0;pass<ui->OpenPopupStack.Size+2;++pass)for(auto& panel:panels)if(panel.visible && panel.popup) {
        auto* window=ImGui::FindWindowByID(panel.id);
        Panel* parent=nullptr;
        for(auto* at=window?window->ParentWindowInBeginStack:nullptr;at;at=at->ParentWindowInBeginStack) {
            auto* candidate=findPanel(displayRoot(at)->ID);
            if(candidate && candidate!=&panel && candidate->visible){parent=candidate;break;}
        }
        if(!parent && window && !(window->Flags&ImGuiWindowFlags_Modal)) {
            parent=findPanel(mousePanel);
            if(parent==&panel)parent=nullptr;
        }
        if(!parent)continue;
        const float density=parent->surface.size.x/std::max(1.f,parent->uiSize.x);
        panel.surface.rotation=parent->surface.rotation;
        panel.surface.size=glm::vec2(panel.uiSize.x,panel.uiSize.y)*density;
        const glm::vec2 delta(panel.uiPosition.x+panel.uiSize.x*.5f-parent->uiPosition.x-parent->uiSize.x*.5f,
                              parent->uiPosition.y+parent->uiSize.y*.5f-panel.uiPosition.y-panel.uiSize.y*.5f);
        panel.surface.position=parent->surface.position+parent->surface.rotation*glm::vec3(delta*density,.004f);
    }
    // Also place a newly created hand strip before its first rendered frame.
    updateWristSurfaces();
    // Each root uses its own draw lists, including child windows and dock tabs.
    // Overlapping 2D windows can never bleed into another spatial surface.
    for(auto& panel:panels)if(panel.visible) {
        ensureTarget(panel.target,static_cast<int>(std::ceil(panel.uiSize.x)),static_cast<int>(std::ceil(panel.uiSize.y)));
        if(!panel.target.fbo)continue;
        auto& slice=drawSlice;slice.Clear();slice.Valid=true;slice.DisplayPos=panel.uiPosition;slice.DisplaySize=panel.uiSize;
        slice.FramebufferScale={panel.target.width/panel.uiSize.x,panel.target.height/panel.uiSize.y};
        slice.OwnerViewport=data->OwnerViewport;slice.Textures=data->Textures;
        for(auto* list:data->CmdLists) {
            auto it=std::find_if(ownerScratch.begin(),ownerScratch.end(),[&](const auto& owner){return owner.first==list;});
            if(it!=ownerScratch.end() && it->second && it->second->ID==panel.id)slice.AddDrawList(list);
            else if(it==ownerScratch.end() && panel.id==hoveredPanel)slice.AddDrawList(list);
        }
        glBindFramebuffer(GL_FRAMEBUFFER,panel.target.fbo);glViewport(0,0,panel.target.width,panel.target.height);
        glDisable(GL_SCISSOR_TEST);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(&slice);
    }
    // Closed tool windows release GPU memory; keep their persisted poses.
    for(auto& panel:panels)if(!panel.visible && panel.target.fbo)releaseTarget(panel.target);
}

bool VREditorWorkspace::returnGesture(const XRInputState& state,float dt,bool playing) {
    const bool held=state.sessionActive && state.hasFocus &&
        ((pressed(state.controllers[0],XRButton::ThumbstickClick) && pressed(state.controllers[1],XRButton::ThumbstickClick)) ||
          pressed(state.controllers[0],XRButton::Menu));
    if(!held){gestureTime=0;gestureLatched=false;return false;}
    if(!playing){gestureTime=0;return false;}
    gestureTime+=std::clamp(dt,0.0f,0.1f);
    if(gestureTime<0.7f || gestureLatched)return false;
    gestureLatched=true;
    RequestHapticPulse({XRDevice::Left,.45f,.08f,0});RequestHapticPulse({XRDevice::Right,.45f,.08f,0});
    return true;
}

bool VREditorWorkspace::pointerRay(glm::vec3& origin,glm::vec3& direction) const {
    const auto& hand=input.controllers[pointerHand];
    if(!wristToolsVisible || !input.hasFocus || !input.sessionActive || !hand.active ||
       !hand.aimPose.isValid() || hit || draggedPanel || (gripDown && !gizmoWasUsing) || !ui || !ui->OpenPopupStack.empty())return false;
    origin=glm::vec3(sceneFromTracking*glm::vec4(hand.aimPose.position,1));
    direction=glm::normalize(glm::mat3(sceneFromTracking)*(hand.aimPose.rotation*glm::vec3(0,0,-1)));
    return true;
}

void VREditorWorkspace::updateSceneHover(const std::vector<SceneObject>& scene,const glm::mat4& view,float dt) {
    glm::vec3 origin,direction;float distance=0;
    const int hovered=pointerRay(origin,direction) && !gizmoOver && !gizmoWasUsing
        ? EditorScenePicking::PickRay(scene,origin,direction,view,{},&distance) : -1;
    if(hovered!=hoverObject) {
        hoverObject=hovered;
        if(hovered>=0){hoverVisual=hovered;hoverAge=0;hoverOpacity=0;}
    }
    dt=std::clamp(dt,0.f,.1f);hoverAge+=dt;
    hoverOpacity=glm::mix(hoverOpacity,hovered>=0?1.f:0.f,1.f-std::exp(-dt*18.f));
    if(hovered>=0)hitPoint=glm::vec3(glm::inverse(sceneFromTracking)*glm::vec4(origin+direction*distance,1));
    disabledItem=ui && ui->HoveredIdIsDisabled;
    const glm::vec4 color=hit?(disabledItem?glm::vec4(.45f,.45f,.45f,1):glm::vec4(1)):
        ((hovered>=0 || gizmoOver || gizmoWasUsing)?glm::vec4(.65f,.35f,1,1):glm::vec4(1));
    rayColor=glm::mix(rayColor,color,1.f-std::exp(-dt*18.f));
}

void VREditorWorkspace::clearGizmo() {
    gizmoVisible=false;gizmoOver=false;
    if(gizmoWasUsing) { ImGuizmo::Enable(false);ImGuizmo::Enable(true); }
    gizmoWasUsing=false;
}

bool VREditorWorkspace::manipulateGizmo(glm::mat4& model,const glm::mat4& view,
    ImGuizmo::OPERATION operation,ImGuizmo::MODE mode,const float* snap,const float* bounds) {
    ImGuizmo::BeginFrame(); // Reset per-frame handle hover ownership.
    gizmoVisible=true;gizmoOperation=operation;gizmoMode=mode;gizmoHasBounds=bounds!=nullptr;
    if(bounds)std::copy_n(bounds,6,gizmoBounds.begin());
    auto& io=ImGui::GetIO();const auto savedMouse=io.MousePos;const bool savedDown=io.MouseDown[0];
    glm::vec3 origin,direction;
    const bool canPoint=pointerRay(origin,direction);
    const auto projection=glm::perspective(glm::radians(95.f),1.6f,.03f,1000.f);
    // Screen-space center handles still need a projected pointer. Project the
    // controller ray onto the head-facing plane through the gizmo, while axes
    // use the actual controller ray rather than a ray starting at the camera.
    io.MousePos={-FLT_MAX,-FLT_MAX};
    if(canPoint) {
        const auto eyeFromWorld=glm::inverse(view);
        const auto normal=glm::normalize(glm::vec3(eyeFromWorld[2]));
        const float denominator=glm::dot(direction,normal);
        if(std::abs(denominator)>.0001f) {
            const float distance=glm::dot(glm::vec3(model[3])-origin,normal)/denominator;
            const auto clip=projection*view*glm::vec4(origin+direction*distance,1);
            if(distance>0 && clip.w>.001f)io.MousePos={(clip.x/clip.w*.5f+.5f)*1280,(.5f-clip.y/clip.w*.5f)*800};
        }
    }
    io.MouseDown[0]=canPoint && mouseDown;
    // NoInputs prevents this invisible interaction canvas taking clicks from UI.
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({1280,800});
    ImGui::Begin("VR Scene Gizmo",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|
        ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGuizmo::SetDrawlist();ImGuizmo::SetRect(0,0,1280,800);ImGuizmo::SetOrthographic(false);
    ImGuizmo::Enable(canPoint);ImGuizmo::SetID(0x565247);
    // A null owner is never dereferenced while spatial input is enabled.
    const glm::vec3 fallbackOrigin(0),fallbackDirection(0,0,-1);
    ImGuizmo::SetInputRay(glm::value_ptr(canPoint?origin:fallbackOrigin),glm::value_ptr(canPoint?direction:fallbackDirection));
    const bool changed=ImGuizmo::Manipulate(glm::value_ptr(view),glm::value_ptr(projection),operation,mode,
        glm::value_ptr(model),nullptr,snap,bounds,snap);
    gizmoOver=canPoint && ImGuizmo::IsOver();gizmoWasUsing=ImGuizmo::IsUsing();gizmoModel=model;
    if(gizmoOver || gizmoWasUsing) {
        scenePickPending=false;
        const float distance=std::max(.03f,glm::dot(glm::vec3(model[3])-origin,direction));
        hitPoint=glm::vec3(glm::inverse(sceneFromTracking)*glm::vec4(origin+direction*distance,1));
    }
    ImGuizmo::SetInputRay(nullptr,nullptr);ImGuizmo::Enable(true);ImGui::End();
    io.MousePos=savedMouse;io.MouseDown[0]=savedDown;
    return changed;
}

void VREditorWorkspace::drawSceneOverlays(Renderer& renderer,const Camera& camera,
    const std::vector<SceneObject>& scene,const glm::mat4& projection,unsigned int target,int width,int height,float nearPlane) {
    if(!wristToolsVisible)return;
    GLState restore;
    renderer.renderSelectionOutlineToTarget(camera,scene,selection,projection,target,width,height,nearPlane);
    if(hoverVisual>=0 && hoverOpacity>.005f && std::find(selection.begin(),selection.end(),hoverVisual)==selection.end()) {
        static std::vector<int> hovered(1);hovered[0]=hoverVisual;
        const float pop=std::sin(glm::pi<float>()*std::min(1.f,hoverAge/.28f));
        renderer.renderSelectionOutlineToTarget(camera,scene,hovered,projection,target,width,height,nearPlane,
            2.7f+2.5f*pop,.9f*hoverOpacity,false);
    }
    if(!gizmoVisible || !ui)return;
    auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(ui);
    // Reuse the excluded interaction window's draw list; no extra GL targets or
    // ImGui frames are needed, and neither eye can apply a second transform.
    auto* window=ImGui::FindWindowByName("VR Scene Gizmo");
    if(window) {
        auto* list=window->DrawList;list->_ResetForNewFrame();
        list->PushTexture(ui->IO.Fonts->TexRef);list->PushClipRect({0,0},{float(width),float(height)});
        ImGuizmo::SetDrawlist(list);ImGuizmo::SetRect(0,0,float(width),float(height));
        const auto view=camera.getViewMatrix();
        ImGuizmo::DrawGizmo(glm::value_ptr(view),glm::value_ptr(projection),
                           gizmoOperation,gizmoMode,glm::value_ptr(gizmoModel),gizmoHasBounds?gizmoBounds.data():nullptr);
        list->PopClipRect();list->PopTexture();
        auto& data=drawSlice;data.Clear();data.Valid=true;data.DisplayPos={0,0};data.DisplaySize={float(width),float(height)};
        data.FramebufferScale={1,1};data.OwnerViewport=ui->Viewports[0];
        data.Textures=ImGui::GetDrawData()->Textures;data.AddDrawList(list);
        glBindFramebuffer(GL_FRAMEBUFFER,target);glViewport(0,0,width,height);
        ImGui_ImplOpenGL3_RenderDrawData(&data);
    }
    ImGui::SetCurrentContext(previous);
}

void VREditorWorkspace::renderKeyboard() {
    if(ImGui::GetIO().WantTextInput)keyboardVisible=true;
    if(!keyboardVisible)return;
    ImGui::SetNextWindowPos({1050,1800},ImGuiCond_FirstUseEver);ImGui::SetNextWindowSize({840,290},ImGuiCond_FirstUseEver);
    if(ImGui::Begin("VR Keyboard",&keyboardVisible,ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNavInputs|ImGuiWindowFlags_NoDocking)) {
        const char* rows[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm_-./:"};
        auto key=[&](const char* label,ImVec2 size) {
            ImGui::Button(label,size);
            return keyboardPress && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        };
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav,true);
        for(const char* row:rows){
            for(const char* c=row;*c;++c){char label[2]={static_cast<char>(shifted?std::toupper(*c):*c),0};
                if(c!=row)ImGui::SameLine();if(key(label,{60,38}))ImGui::GetIO().AddInputCharacter(label[0]);}
        }
        if(key("Shift",{90,38}))shifted=!shifted;
        ImGui::SameLine();if(key("Space",{220,38}))ImGui::GetIO().AddInputCharacter(' ');
        ImGui::SameLine();if(key("Backspace",{130,38})){ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace,true);ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace,false);}
        ImGui::SameLine();if(key("Enter",{90,38})){ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);}
        ImGui::PopItemFlag();
    }ImGui::End();
}

void VREditorWorkspace::ensureGraphics() {
    if(program)return;
    // The editor compositor is deliberately after scene post FX so text stays sharp.
    const char* vertex=
#if MODULARITY_OPENGL_ES
        "#version 300 es\n"
#else
        "#version 330 core\n"
#endif
        "layout(location=0)in vec3 p;layout(location=1)in vec2 t;uniform mat4 mvp;out vec2 uv;void main(){uv=t;gl_Position=mvp*vec4(p,1);}";
    const char* fragment=
#if MODULARITY_OPENGL_ES
        "#version 300 es\nprecision mediump float;\n"
#else
        "#version 330 core\n"
#endif
        "in vec2 uv;out vec4 c;uniform sampler2D image;uniform vec4 tint;uniform bool textured;void main(){c=tint*(textured?texture(image,uv):vec4(1));}";
    GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs,1,&vertex,nullptr);glCompileShader(vs);glShaderSource(fs,1,&fragment,nullptr);glCompileShader(fs);
    program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    glDeleteShader(vs);glDeleteShader(fs);
    GLint valid=0;glGetProgramiv(program,GL_LINK_STATUS,&valid);
    if(!valid){glDeleteProgram(program);program=0;return;}
    const float quad[]={-.5f,-.5f,0,0,0, .5f,-.5f,0,1,0, .5f,.5f,0,1,1, -.5f,-.5f,0,0,0, .5f,.5f,0,1,1, -.5f,.5f,0,0,1};
    glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,5*sizeof(float),nullptr);
    glEnableVertexAttribArray(1);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,5*sizeof(float),reinterpret_cast<void*>(3*sizeof(float)));
}
void VREditorWorkspace::drawQuad(const glm::mat4& mvp,unsigned int texture,const glm::vec4& tint) {
    glUseProgram(program);glUniformMatrix4fv(glGetUniformLocation(program,"mvp"),1,GL_FALSE,glm::value_ptr(mvp));
    glUniform4fv(glGetUniformLocation(program,"tint"),1,glm::value_ptr(tint));
    glUniform1i(glGetUniformLocation(program,"image"),0);glUniform1i(glGetUniformLocation(program,"textured"),texture!=0);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,6);
}

void VREditorWorkspace::renderEye(Renderer& renderer,const glm::mat4& vp,const glm::vec3& eye) {
    GLState restore;ensureGraphics();if(!program)return;
    glDisable(GL_SCISSOR_TEST);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
    // Back to front for transparent edges and popup surfaces.
    visibleScratch.clear();
    for(auto& panel:panels)if(panel.visible && panel.target.texture)visibleScratch.push_back(&panel);
    std::sort(visibleScratch.begin(),visibleScratch.end(),[&](const Panel* a,const Panel* b){
        // A submenu near the edge may have a more distant center than its
        // parent. Menus still composite above that parent and in Begin order.
        if(a->popup!=b->popup)return !a->popup;
        if(a->popup) {
            const auto window=[&](ImGuiID id)->const ImGuiWindow* {
                for(const auto* candidate:ui->Windows)if(candidate->ID==id)return candidate;
                return nullptr;
            };
            const auto* aw=window(a->id);const auto* bw=window(b->id);
            if(aw && bw && aw->BeginOrderWithinContext!=bw->BeginOrderWithinContext)
                return aw->BeginOrderWithinContext<bw->BeginOrderWithinContext;
        }
        return glm::distance(a->surface.position,eye)>glm::distance(b->surface.position,eye);
    });
    for(const auto* panel:visibleScratch) {
        const auto model=surfaceMatrix(panel->surface)*glm::scale(glm::mat4(1),glm::vec3(panel->surface.size,1));
        drawQuad(vp*model,panel->target.texture,glm::vec4(1));
    }
    // The compositor's 2D state cannot be used for opaque geometry: without
    // depth, rear triangles overwrite the shell and controllers look inside out.
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);glEnable(GL_CULL_FACE);glCullFace(GL_BACK);glFrontFace(GL_CCW);
    drawControllers(renderer,vp,eye);
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);
    const auto& hand=input.controllers[pointerHand];
    if(hand.active && hand.aimPose.isValid() && input.hasFocus) {
        const glm::vec3 start=hand.aimPose.position,end=hitPoint;
        glm::vec3 beam=end-start;const float length=glm::length(beam);
        if(length>0.01f) {
            const glm::vec3 direction=beam/length;
            const auto rotation=direction.y < -0.9999f ? glm::angleAxis(glm::pi<float>(),glm::vec3(1,0,0)) :
                glm::normalize(glm::quat(1+direction.y,direction.z,0,-direction.x));
            auto model=glm::translate(glm::mat4(1),(start+end)*.5f)*glm::mat4_cast(rotation)*glm::scale(glm::mat4(1),glm::vec3(.0025f,length,1));
            drawQuad(vp*model,0,rayColor);
            if(hit || hoverObject>=0 || gizmoOver || gizmoWasUsing) {
                auto marker=glm::translate(glm::mat4(1),end);
                marker*=glm::mat4_cast(hand.aimPose.rotation);marker=glm::scale(marker,glm::vec3(.012f,.012f,1));
                drawQuad(vp*marker,0,rayColor);
            }
        }
    }
}

void VREditorWorkspace::captureMirror(unsigned int source,int width,int height) {
    GLState restore;ensureTarget(mirror,1280,800,true);if(!mirror.fbo)return;
    glDisable(GL_SCISSOR_TEST);glBindFramebuffer(GL_READ_FRAMEBUFFER,source);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,mirror.fbo);
    glBlitFramebuffer(0,0,width,height,0,0,mirror.width,mirror.height,GL_COLOR_BUFFER_BIT,GL_LINEAR);
}

void VREditorWorkspace::drawControllers(Renderer& renderer,const glm::mat4& vp,glm::vec3 eye) {
    if(!controllerShader)controllerShader=std::make_unique<Shader>(
        "Resources/Shaders/vr_editor_controller_vert.glsl","Resources/Shaders/vr_editor_controller_frag.glsl");
    if(!controllerShader->ID)return;
    struct Visual {
        std::string mesh,albedo,specular,normal,orm;
        float scale=1,metallic=.391f,roughness=.323f,ambient=.28f;
        glm::vec3 offset{0},rotation{0};
    };
    static std::unordered_map<std::string,Visual> visuals;
    for(int h=0;h<2;++h) {
        const auto& hand=input.controllers[h];if(!hand.active || !hand.gripPose.isValid())continue;
        const bool touch=hand.interactionProfile.find("oculus")!=std::string::npos || hand.interactionProfile.find("meta")!=std::string::npos;
        const std::string config=std::string("Resources/VR-Engine-Root/ControllerMaterials/")+(touch?"quest-touch-plus":"valve-index")+(h?"-right.controller":"-left.controller");
        auto it=visuals.find(config);
        if(it==visuals.end()) {
            Visual visual;
            auto bytes=Modularity::Platform::GetAssetSource().ReadAll(config);
            std::istringstream stream(std::string(bytes.begin(),bytes.end()));std::string line;
            while(std::getline(stream,line)) {
                while(!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))line.pop_back();
                const auto split=line.find('=');if(split==std::string::npos || line[0]=='#')continue;
                const auto key=line.substr(0,split),value=line.substr(split+1);
                if(key=="mesh")visual.mesh=value;else if(key=="albedo")visual.albedo=value;
                else if(key=="specular")visual.specular=value;else if(key=="normal")visual.normal=value;else if(key=="orm")visual.orm=value;
                else if(key=="scale"||key=="metallic"||key=="roughness"||key=="ambient") {
                    float number;std::istringstream numberStream(value);if(!(numberStream>>number)||!std::isfinite(number))continue;
                    if(key=="scale")visual.scale=std::clamp(number,.001f,10.f);
                    if(key=="metallic")visual.metallic=std::clamp(number,0.f,1.f);
                    if(key=="roughness")visual.roughness=std::clamp(number,.04f,1.f);
                    if(key=="ambient")visual.ambient=std::clamp(number,0.f,1.f);
                } else if(key=="offset"||key=="rotation") {
                    std::string components=value;std::replace(components.begin(),components.end(),',',' ');
                    glm::vec3 v;std::istringstream values(components);if(values>>v.x>>v.y>>v.z) {
                        if(key=="offset")visual.offset=v;else visual.rotation=v;
                    }
                }
            }
            it=visuals.emplace(config,std::move(visual)).first;
        }
        const auto& visual=it->second;if(visual.mesh.empty())continue;
        const auto loaded=getModelLoader().loadModel(visual.mesh);if(!loaded.success)continue;
        const auto model=poseMatrix(hand.gripPose)*glm::translate(glm::mat4(1),visual.offset)*
            glm::mat4_cast(glm::quat(glm::radians(visual.rotation)))*glm::scale(glm::mat4(1),glm::vec3(visual.scale));
        controllerShader->use();controllerShader->setMat4("model",model);controllerShader->setMat4("vp",vp);
        controllerShader->setVec3("eye",eye);controllerShader->setFloat("metallic",visual.metallic);
        controllerShader->setFloat("roughness",visual.roughness);controllerShader->setFloat("ambient",visual.ambient);
        auto texture=[&](const std::string& path,const char* uniform,int unit) {
            auto* texture=path.empty()?nullptr:renderer.getTexture(path);
            glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,texture?texture->GetID():0);
            controllerShader->setInt(uniform,unit);return texture!=nullptr;
        };
        controllerShader->setBool("hasAlbedo",texture(visual.albedo,"albedoMap",0));
        controllerShader->setBool("hasSpecular",texture(visual.specular,"specularMap",1));
        controllerShader->setBool("hasNormal",texture(visual.normal,"normalMap",2));
        controllerShader->setBool("hasOrm",texture(visual.orm,"ormMap",3));
        glDisable(GL_BLEND);
        for(int mesh:loaded.meshIndices)if(auto* geometry=getModelLoader().getMesh(mesh))geometry->draw();
        if(loaded.meshIndices.empty())if(auto* geometry=getModelLoader().getMesh(loaded.meshIndex))geometry->draw();
        glEnable(GL_BLEND);
    }
}
void VREditorWorkspace::renderPreview(Renderer& renderer,const Camera* camera,const std::vector<SceneObject>* scene,
                                    float nearPlane,float farPlane) {
    GLState restore;ensureTarget(mirror,1280,800,true);if(!mirror.fbo)return;
    glBindFramebuffer(GL_FRAMEBUFFER,mirror.fbo);glViewport(0,0,mirror.width,mirror.height);
    glDisable(GL_SCISSOR_TEST);glClearColor(.035f,.045f,.065f,1);glClear(GL_COLOR_BUFFER_BIT);
    const auto& head=input.headPose;
    const auto view=glm::inverse(poseMatrix(head));
    nearPlane=std::max(.01f,nearPlane);farPlane=std::max(nearPlane+.01f,farPlane);
    const auto projection=glm::perspective(glm::radians(95.f),1.6f,nearPlane,farPlane);
    if(camera && scene) {
        glDepthMask(GL_TRUE);
        const auto worldFromHead=sceneFromTracking*poseMatrix(head);
        const auto worldView=glm::inverse(worldFromHead);
        Camera sceneCamera=*camera;
        sceneCamera.position=glm::vec3(worldFromHead[3]);
        sceneCamera.front=glm::normalize(-glm::vec3(worldFromHead[2]));
        sceneCamera.up=glm::normalize(glm::vec3(worldFromHead[1]));
        sceneCamera.orthographic=false;
        renderer.beginRender(worldView,projection,sceneCamera.position);
        renderer.renderSceneToTarget(sceneCamera,*scene,mirror.fbo,mirror.width,mirror.height,projection,nearPlane,farPlane,true,true);
        renderer.endRender();
        drawSceneOverlays(renderer,sceneCamera,*scene,projection,mirror.fbo,mirror.width,mirror.height,nearPlane);
        glBindFramebuffer(GL_FRAMEBUFFER,mirror.fbo);glViewport(0,0,mirror.width,mirror.height);
    }
    renderEye(renderer,projection*view,head.position);
}

void VREditorWorkspace::shutdown() {
    if(!ui && !program && !mirror.fbo)return;
    save();
    if(ui) {
        auto* previous=ImGui::GetCurrentContext();
        // Backend objects and font atlas are borrowed from the desktop context.
        ui->IO.BackendRendererUserData=nullptr;ui->IO.BackendRendererName=nullptr;
        auto* destroyed=ui;ImGui::DestroyContext(ui);ui=nullptr;ImGui::SetCurrentContext(previous==destroyed?nullptr:previous);
    }
    for(auto& panel:panels)releaseTarget(panel.target);panels.clear();
    releaseTarget(mirror);controllerShader.reset();
    if(program)glDeleteProgram(program);if(vbo)glDeleteBuffers(1,&vbo);if(vao)glDeleteVertexArrays(1,&vao);
    program=vao=vbo=0;
}
#endif
