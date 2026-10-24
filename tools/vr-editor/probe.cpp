#include "EditorWindows/VREditorWorkspace.h"
#include "EditorWindows/VREditorWidgets.h"
#include "EditorWindows/VREditorMouseInput.h"
#include "ModelLoader.h"
#include "Camera.h"
#include "EditorWindows/ScenePicking.h"
#include "XR/XRInputBindings.h"
#include "ThirdParty/ModuGUI/imgui_internal.h"
#include "ThirdParty/ModuGUI/backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <fstream>
#include <map>

using namespace Modularity::XR;
static int failures=0;
static void check(bool value,const char* message) {
    std::printf("[%s] %s\n",value?"PASS":"FAIL",message);std::fflush(stdout);if(!value)++failures;
}
static std::map<ImGuiID,VREditorWorkspace::Surface> readSurfaces(const std::filesystem::path& folder) {
    std::map<ImGuiID,VREditorWorkspace::Surface> out;std::ifstream file(folder/"vr-surfaces.txt");
    ImGuiID id;VREditorWorkspace::Surface s;
    while(file>>id>>s.position.x>>s.position.y>>s.position.z>>s.rotation.w>>s.rotation.x>>s.rotation.y>>s.rotation.z>>s.size.x>>s.size.y)out[id]=s;
    return out;
}

static void checkEditorBindings() {
    const auto* gameplay=GetProfileBindings(XRInteractionProfile::ValveIndex);
    const auto* editor=GetProfileBindings(XRInteractionProfile::ValveIndex,true);
    bool force=false,unchanged=gameplay && editor && gameplay->entryCount==editor->entryCount;
    if(unchanged)for(size_t i=0;i<editor->entryCount;++i) {
        const auto& original=gameplay->entries[i];const auto& edited=editor->entries[i];
        auto same=[](const char* a,const char* b){return a==b || (a && b && std::string(a)==b);};
        if(edited.slot==XRActionSlot::GripValue) {
            force=same(edited.leftPath,"/user/hand/left/input/squeeze/force") && same(edited.rightPath,"/user/hand/right/input/squeeze/force");
            unchanged=unchanged && same(original.leftPath,"/user/hand/left/input/squeeze/value") && same(original.rightPath,"/user/hand/right/input/squeeze/value");
        } else unchanged=unchanged && original.slot==edited.slot && same(original.leftPath,edited.leftPath) && same(original.rightPath,edited.rightPath);
    }
    check(force,"Index editor grip uses deliberate squeeze force for both hands");
    check(unchanged,"Index gameplay bindings and all other editor controls retain their original paths");
    for(auto profile:{XRInteractionProfile::MetaQuestTouch,XRInteractionProfile::HtcVive,XRInteractionProfile::MicrosoftMotionController,XRInteractionProfile::KhronosSimpleController})
        check(GetProfileBindings(profile)==GetProfileBindings(profile,true),"editor grip policy preserves non-Index controller profiles");
    check(!ProjectOpenXRSettings{}.interaction.editorWorkspace,"project XR settings default to gameplay input policy");
}

static void checkMirrorIsolation() {
    VREditorWorkspace workspace;
    GLuint textures[2]{},targets[2]{};glGenTextures(2,textures);glGenFramebuffers(2,targets);
    for(int eye=0;eye<2;++eye) {
        glBindTexture(GL_TEXTURE_2D,textures[eye]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER,targets[eye]);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[eye],0);
        glDisable(GL_SCISSOR_TEST);glClearColor(eye?0.f:1.f,eye?1.f:0.f,0,1);glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER,targets[0]);glViewport(17,19,320,240);
    workspace.captureMirror(targets[0],64,64);
    GLint draw=0,read=0,viewport[4]{};glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_VIEWPORT,viewport);
    check(draw==GLint(targets[1]) && read==GLint(targets[0]) && viewport[0]==17 && viewport[1]==19 && viewport[2]==320 && viewport[3]==240,
          "headset mirror capture preserves distinct read/draw targets and viewport");
    std::vector<unsigned char> mirrored(1280*800*4);glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,mirrored.data());
    check(mirrored[0]>250 && mirrored[1]<5,"headset mirror copies the intended eye before desktop presentation");
    // Desktop presentation explicitly selects the window instead of the last eye.
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,1280,800);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
    std::array<unsigned char,4> pixel{};glBindFramebuffer(GL_READ_FRAMEBUFFER,targets[1]);glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
    check(pixel[0]<5 && pixel[1]>250 && pixel[2]<5,"desktop window presentation leaves the VR eye image intact");
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(2,targets);glDeleteTextures(2,textures);
    workspace.shutdown();check(glGetError()==GL_NO_ERROR,"headset mirror and desktop target isolation has no GL errors");
}

static void checkSavedLayoutsAndHands(ImGuiContext* desktop,const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    for(const auto* name:{"vr-docking.ini","vr-surfaces.txt","vr-anchor.txt","vr-input.txt"})std::filesystem::remove(folder/name);
    VREditorWorkspace workspace;workspace.wristToolsVisible=true;workspace.setStorage(folder);
    XRInputState state;state.sessionActive=state.hasFocus=true;
    state.headPose.position={0,1.6f,0};state.headPose.positionValid=state.headPose.orientationValid=true;
    auto& hand=state.controllers[1];hand.active=true;hand.aimPose=state.headPose;hand.gripPose=state.headPose;
    hand.gripPose.position={.2f,1.3f,-.2f};hand.interactionProfile="/interaction_profiles/valve/index_controller";
    ImGuiID rootID=0;ImVec2 button{},scrollPoint{},pointer{},rootPosition{};int clicks=0;float scroll=0;
    bool contextOpened=false,closePopup=false;
    auto frame=[&]() {
        ImGui::SetCurrentContext(desktop);auto& io=ImGui::GetIO();io.DisplaySize={1280,800};io.DeltaTime=1.f/90;
        ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();ImGui::Render();
        workspace.beginUI(state,1.f/90,false);pointer=ImGui::GetIO().MousePos;
        ImGui::SetNextWindowPos({1000,900},ImGuiCond_FirstUseEver);ImGui::SetNextWindowSize({400,330},ImGuiCond_FirstUseEver);
        ImGui::Begin("Saved layout input fixture",nullptr,ImGuiWindowFlags_NoDocking);
        rootID=ImGui::GetCurrentWindow()->ID;rootPosition=ImGui::GetWindowPos();
        if(ImGui::Button("Stable target",{220,46}))++clicks;
        const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();button={(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f};
        if(ImGui::BeginPopupContextItem("Native context menu")) {
            contextOpened=true;ImGui::MenuItem("A native menu action");if(closePopup)ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        ImGui::BeginChild("Scrollable fixture",{300,200},true);
        const auto top=ImGui::GetWindowPos();scrollPoint={top.x+120,top.y+100};scroll=ImGui::GetScrollY();
        for(int i=0;i<70;++i)ImGui::Text("Scrollable row %d",i);
        ImGui::EndChild();ImGui::End();workspace.endUI();
    };
    auto targetAt=[&](ImVec2 pixel) {
        workspace.save();const auto surface=readSurfaces(folder).at(rootID);
        ImGui::SetCurrentContext(workspace.context());const auto* window=ImGui::FindWindowByID(rootID);
        const auto point=surface.position+surface.rotation*glm::vec3(((pixel.x-window->Pos.x)/window->Size.x-.5f)*surface.size.x,
            (.5f-(pixel.y-window->Pos.y)/window->Size.y)*surface.size.y,0);
        ImGui::SetCurrentContext(desktop);return point;
    };
    auto aim=[&](glm::vec3 point){hand.aimPose.rotation=glm::quatLookAt(glm::normalize(point-hand.aimPose.position),glm::vec3(0,1,0));};
    frame();frame();aim(targetAt(button));for(int i=0;i<25;++i)frame();
    const auto stable=pointer;const auto rotation=hand.aimPose.rotation;
    hand.buttons[(size_t)XRButton::Trigger]=true;
    for(int i=0;i<5;++i){hand.aimPose.rotation=rotation*glm::angleAxis(glm::radians(i%2?.12f:-.12f),glm::vec3(0,1,0));frame();}
    check(std::abs(pointer.x-stable.x)<.1f && std::abs(pointer.y-stable.y)<.1f,"trigger click suppresses sub-threshold controller shake");
    hand.aimPose.rotation=rotation*glm::angleAxis(glm::radians(1.5f),glm::vec3(0,1,0));for(int i=0;i<8;++i)frame();
    check(std::abs(pointer.x-stable.x)>8,"intentional controller movement unlocks click stabilization for dragging");
    hand.buttons.fill(false);frame();frame();
    check(clicks==1,"stabilized trigger click still activates a native button exactly once");
    aim(targetAt(button));for(int i=0;i<25;++i)frame();
    hand.editorTouchpadTouched=hand.editorTouchpadClicked=true;frame();frame();
    hand.editorTouchpadClicked=false;frame();frame();
    check(contextOpened && clicks==1,"pressing the Index touchpad opens a native right-click context menu without a left click");
    closePopup=true;frame();frame();hand.editorTouchpadTouched=false;frame();
    aim(targetAt(scrollPoint));for(int i=0;i<25;++i)frame();const auto beforeScroll=scroll;
    hand.editorTouchpadTouched=true;hand.editorTouchpad={0,.5f};frame();frame();
    check(std::abs(scroll-beforeScroll)<.1f,"initial touchpad contact does not jump the scroll position");
    hand.editorTouchpad.y=.25f;frame();frame();frame();
    check(scroll>beforeScroll+1,"touchpad swipe scrolls the pointed native child panel");
    hand.editorTouchpadTouched=false;aim(targetAt(button));for(int i=0;i<25;++i)frame();
    workspace.save();const auto beforeGrab=readSurfaces(folder).at(rootID);
    hand.axes[(size_t)XRAxisKind::Grip]=.35f;hand.gripPose.position.x+=.1f;for(int i=0;i<12;++i)frame();
    workspace.save();check(glm::distance(readSurfaces(folder).at(rootID).position,beforeGrab.position)<.001f,"Index panel grab ignores force below the reduced 0.36 threshold");
    hand.axes[(size_t)XRAxisKind::Grip]=.36f;frame();hand.gripPose.position.x+=.2f;for(int i=0;i<20;++i)frame();
    hand.axes[(size_t)XRAxisKind::Grip]=.23f;frame();workspace.save();
    const auto savedSurface=readSurfaces(folder).at(rootID);
    check(glm::distance(savedSurface.position,beforeGrab.position)>.15f,"Index panel grab activates at 60 percent of the previous effort and follows smoothed movement");
    const auto savedRootPosition=rootPosition;
    check(workspace.saveLayout("Regression desk"),"named VR layout saves docking, physical poses and input preferences");
    check(!workspace.saveLayout("../Escape") && !workspace.loadLayout(".."),"layout names cannot escape the project layout directory");
    workspace.grabForce=.9f;workspace.smoothHands=false;workspace.save();
    ImGui::SetCurrentContext(workspace.context());ImGui::SetWindowPos("Saved layout input fixture",{1700,1200});ImGui::SetCurrentContext(desktop);frame();
    check(workspace.loadLayout("Regression desk"),"saved VR layout can be reloaded in the existing UI context");
    frame();frame();workspace.save();
    check(std::abs(workspace.grabForce-.36f)<.001f && workspace.smoothHands && std::abs(rootPosition.x-savedRootPosition.x)<1 &&
          glm::distance(readSurfaces(folder).at(rootID).position,savedSurface.position)<.001f,
          "loading a layout restores native window coordinates, physical panel poses and input settings");
    workspace.shutdown();
    VREditorWorkspace restored;restored.setStorage(folder);
    check(restored.initializedLayout,"restored floating layouts do not recreate default dock groups over saved windows");
    state.headPose.position+=glm::vec3(.7f,0,.4f);state.headPose.rotation=glm::angleAxis(glm::radians(45.f),glm::vec3(0,1,0));
    for(int i=0;i<3;++i) {
        ImGui::SetCurrentContext(desktop);ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();ImGui::Render();
        restored.beginUI(state,1.f/90,false);ImGui::Begin("Saved layout input fixture");ImGui::TextUnformatted("Restored workspace");ImGui::End();restored.endUI();
    }
    restored.save();const auto expected=state.headPose.position+state.headPose.rotation*(savedSurface.position-glm::vec3(0,1.6f,0));
    check(glm::distance(readSurfaces(folder).at(rootID).position,expected)<.005f,"VR layout survives a fresh workspace and recenters relative to the new headset pose");
    restored.shutdown();
    VREditorWorkspace gizmos;gizmos.wristToolsVisible=true;
    state.headPose.position={0,1.6f,0};state.headPose.rotation=glm::quat(1,0,0,0);
    hand.aimPose=state.headPose;hand.gripPose=state.headPose;hand.axes[(size_t)XRAxisKind::Grip]=0;hand.buttons.fill(false);
    glm::mat4 model=glm::translate(glm::mat4(1),glm::vec3(0,1.6f,-3));
    const auto view=glm::lookAt(state.headPose.position,state.headPose.position+glm::vec3(0,0,-1),glm::vec3(0,1,0));
    auto gizmoFrame=[&]() {
        ImGui::SetCurrentContext(desktop);ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();ImGui::Render();
        gizmos.beginUI(state,1.f/90,false);gizmos.manipulateGizmo(model,view,ImGuizmo::TRANSLATE,ImGuizmo::WORLD,nullptr,nullptr);gizmos.endUI();
    };
    for(int i=0;i<4;++i)gizmoFrame();
    hand.buttons[(size_t)XRButton::Trigger]=true;gizmoFrame();const auto beforeJitter=model;
    for(int i=0;i<5;++i){hand.aimPose.rotation=glm::angleAxis(glm::radians(i%2?.1f:-.1f),glm::vec3(0,1,0));gizmoFrame();}
    check(gizmos.gizmoUsing() && glm::distance(glm::vec3(model[3]),glm::vec3(beforeJitter[3]))<.001f,"stabilization prevents small trigger-pull jitter from moving a selected object");
    hand.aimPose.rotation=glm::quatLookAt(glm::normalize(glm::vec3(.3f,.15f,-3)),glm::vec3(0,1,0));
    for(int i=0;i<10;++i)gizmoFrame();
    check(gizmos.gizmoUsing() && glm::distance(glm::vec3(model[3]),glm::vec3(beforeJitter[3]))>.1f,"intentional movement keeps the filtered scene gizmo captured and moves the selection");
    const auto rawRotation=hand.aimPose.rotation;hand.buttons.fill(false);gizmoFrame();
    check(!gizmos.gizmoUsing() && hand.aimPose.rotation==rawRotation,"filtered editor dragging releases normally and never changes source tracking poses");
    gizmos.shutdown();check(glGetError()==GL_NO_ERROR,"saved layout, smoothing, scrolling and context-menu checks have no GL errors");
}

static void checkPreviewInteraction(Renderer& renderer,ImGuiContext* desktop,const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    std::filesystem::remove(folder/"vr-docking.ini");std::filesystem::remove(folder/"vr-surfaces.txt");
    VREditorWorkspace workspace;workspace.preview=true;workspace.wristToolsVisible=true;workspace.setStorage(folder);
    SceneObject cube("Selectable preview cube",ObjectType::Cube,901);cube.hasRenderer=true;cube.renderType=RenderType::Cube;cube.position={-1,1.6f,-3};
    std::vector<SceneObject> objects{cube};Camera camera;
    int widgetClicks=0,pickRequests=0,selected=-1;ImGuiID rootID=0,mirrorID=0;ImVec2 button{},imageTop{},imageSize{1248,780},monitorPosition{};
    auto frame=[&](ImVec2 mouse,bool left=false,bool right=false) {
        ImGui::SetCurrentContext(desktop);auto& io=ImGui::GetIO();io.DeltaTime=1.f/90;io.DisplaySize={1280,800};
        io.AddMousePosEvent(mouse.x,mouse.y);io.AddMouseButtonEvent(0,left);io.AddMouseButtonEvent(1,right);
        ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({1280,800});
        ImGui::Begin("Preview monitor fixture",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        monitorPosition=ImGui::GetWindowPos();imageTop=ImGui::GetCursorScreenPos();mirrorID=ImGui::GetID("##VRMirror");
        workspace.renderPreviewImage(imageSize);ImGui::End();ImGui::Render();
        const auto state=workspace.previewInputState();workspace.beginUI(state,1.f/90,false);
        ImGui::SetNextWindowPos({1000,1000},ImGuiCond_FirstUseEver);ImGui::SetNextWindowSize({500,300},ImGuiCond_FirstUseEver);
        ImGui::Begin("Preview fixture",nullptr,ImGuiWindowFlags_NoDocking);
        rootID=ImGui::GetCurrentWindow()->ID;
        if(ImGui::Button("Select UI widget",{180,42}))++widgetClicks;
        const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();button={(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f};
        ImGui::End();glm::vec3 origin,direction;
        if(workspace.takeScenePick(origin,direction)){++pickRequests;selected=EditorScenePicking::PickRay(objects,origin,direction,glm::lookAt(glm::vec3(0,1.6f,0),glm::vec3(0,1.6f,-1),glm::vec3(0,1,0)));}
        workspace.endUI();workspace.renderPreview(renderer,&camera,&objects);
    };
    auto project=[&](glm::vec3 world) {
        const auto p=world-glm::vec3(0,1.6f,0);const float tangent=std::tan(glm::radians(95.f)*.5f);
        return ImVec2(imageTop.x+(p.x/(-p.z)/(tangent*1.6f)+1)*.5f*imageSize.x,
                      imageTop.y+(1-p.y/(-p.z)/tangent)*.5f*imageSize.y);
    };
    auto pointAtUI=[&](ImVec2 point) {
        workspace.save();const auto surface=readSurfaces(folder).at(rootID);
        ImGui::SetCurrentContext(workspace.context());const auto* root=ImGui::FindWindowByID(rootID);
        const auto world=surface.position+surface.rotation*glm::vec3(((point.x-root->Pos.x)/root->Size.x-.5f)*surface.size.x,
                                                                    (.5f-(point.y-root->Pos.y)/root->Size.y)*surface.size.y,0);
        ImGui::SetCurrentContext(desktop);return world;
    };
    for(int i=0;i<4;++i)frame({10,10});
    auto mouse=project(pointAtUI(button));frame(mouse);frame(mouse);frame(mouse,true);frame(mouse,true);
    check(desktop->ActiveId==mirrorID && !desktop->MovingWindow,"preview image owns the desktop mouse gesture instead of moving the monitor");
    frame(mouse);frame(mouse);
    check(widgetClicks==1 && pickRequests==0,"desktop preview mouse selects a spatial UI widget exactly once without picking through it");
    ImGui::SetCurrentContext(workspace.context());const auto* root=ImGui::FindWindowByID(rootID);
    const ImVec2 title(root->Pos.x+root->Size.x*.5f,root->Pos.y+root->TitleBarHeight*.5f);ImGui::SetCurrentContext(desktop);
    const auto titleWorld=pointAtUI(title);workspace.save();const auto titleBefore=readSurfaces(folder).at(rootID);
    mouse=project(titleWorld);frame(mouse);frame(mouse,true);frame(mouse,true);
    mouse=project(titleWorld+glm::vec3(.18f,.04f,0));for(int i=0;i<4;++i)frame(mouse,true);frame(mouse);frame(mouse);
    workspace.save();const auto titleAfter=readSurfaces(folder).at(rootID);
    check(glm::distance(titleBefore.position,titleAfter.position)>.12f,"left-dragging a title moves the panel in world space through the real preview input path");
    const auto gripTarget=titleAfter.position;mouse=project(gripTarget);frame(mouse);frame(mouse,false,true);frame(mouse,false,true);
    ImGui::SetCurrentContext(workspace.context());const auto pixelBefore=ImGui::FindWindowByID(rootID)->Pos;ImGui::SetCurrentContext(desktop);
    mouse=project(gripTarget+glm::vec3(.20f,0,0));for(int i=0;i<3;++i)frame(mouse,false,true);frame(mouse);frame(mouse);
    workspace.save();const auto gripAfter=readSurfaces(folder).at(rootID);
    ImGui::SetCurrentContext(workspace.context());const auto pixelAfter=ImGui::FindWindowByID(rootID)->Pos;ImGui::SetCurrentContext(desktop);
    check(glm::distance(titleAfter.position,gripAfter.position)>.15f && std::abs(pixelBefore.x-pixelAfter.x)<1 && std::abs(pixelBefore.y-pixelAfter.y)<1,
          "right-dragging in preview translates the 3D panel while its virtual desktop rectangle stays fixed");
    check(std::abs(glm::dot(titleAfter.rotation,gripAfter.rotation))>.999f && monitorPosition.x==0 && monitorPosition.y==0,"preview panel dragging preserves orientation and leaves the monitor fixed");
    mouse=project(cube.position);frame(mouse);frame(mouse,true);for(int i=0;i<4;++i)frame(mouse,true);frame(mouse);frame(mouse);
    check(selected==901 && pickRequests==1,"desktop preview click selects the visible scene object once through shared editor ray picking");
    mouse=project({4,3,-3});frame(mouse);frame(mouse,true);frame(mouse,true);frame(mouse);frame(mouse);
    check(selected==-1 && pickRequests==2,"clicking empty scene space clears selection");
    workspace.shutdown();
}
static void checkPopupPlacement(Renderer& renderer,ImGuiContext* desktop,const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    std::filesystem::remove(folder/"vr-docking.ini");std::filesystem::remove(folder/"vr-surfaces.txt");
    VREditorWorkspace workspace;workspace.preview=true;workspace.setStorage(folder);
    XRInputState state;state.hasFocus=state.sessionActive=true;state.headPose.position={0,1.6f,0};
    state.headPose.positionValid=state.headPose.orientationValid=true;
    ImGuiID parentId=0;ImVec2 parentPos{},parentSize{},popupPos{},popupSize{};
    for(int frame=0;frame<8;++frame) {
        ImGui::SetCurrentContext(desktop);auto& io=ImGui::GetIO();io.DisplaySize={1280,800};io.DeltaTime=1.f/90;
        ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();ImGui::Render();workspace.beginUI(state,1.f/90,false);
        ImGui::SetNextWindowPos({1200,900});ImGui::SetNextWindowSize({480,400});
        ImGui::Begin("Popup parent fixture",nullptr,ImGuiWindowFlags_NoDocking);
        parentId=ImGui::GetCurrentWindow()->ID;parentPos=ImGui::GetWindowPos();parentSize=ImGui::GetWindowSize();
        if(frame==0)ImGui::OpenPopup("Component fixture");
        ImGui::SetNextWindowPos({parentPos.x+440,parentPos.y+60});
        if(ImGui::BeginPopup("Component fixture")) {
            ImGui::TextUnformatted("Add component");
            if(!ImGui::IsPopupOpen("Nested fixture"))ImGui::OpenPopup("Nested fixture");
            ImGui::SetNextWindowPos({parentPos.x+500,parentPos.y+100});
            ImGui::SetNextWindowSize(frame<3?ImVec2(64,64):ImVec2(224,144));
            if(ImGui::BeginPopup("Nested fixture")) {
                popupPos=ImGui::GetWindowPos();popupSize=ImGui::GetWindowSize();
                ImGui::GetWindowDrawList()->AddRectFilled(popupPos,{popupPos.x+popupSize.x,popupPos.y+popupSize.y},IM_COL32(220,35,190,255));
                ImGui::EndPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();workspace.endUI();workspace.renderPreview(renderer);
    }
    workspace.save();const auto surfaces=readSurfaces(folder);const auto parent=surfaces.at(parentId);
    const float density=parent.size.x/parentSize.x;
    const auto vp=glm::perspective(glm::radians(95.f),1.6f,.03f,1000.f)*
        glm::lookAt(state.headPose.position,state.headPose.position+glm::vec3(0,0,-1),glm::vec3(0,1,0));
    std::vector<unsigned char> image(1280*800*4);glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,image.data());
    bool aligned=popupSize.x==224 && popupSize.y==144;
    for(float x:{.2f,.8f})for(float y:{.2f,.8f}) {
        const glm::vec3 offset((popupPos.x+popupSize.x*x-parentPos.x-parentSize.x*.5f)*density,
            (parentPos.y+parentSize.y*.5f-popupPos.y-popupSize.y*y)*density,.008f);
        const auto clip=vp*glm::vec4(parent.position+parent.rotation*offset,1);
        const int px=int((clip.x/clip.w*.5f+.5f)*1280),py=int((clip.y/clip.w*.5f+.5f)*800);
        if(px<0 || px>=1280 || py<0 || py>=800){aligned=false;continue;}
        const auto i=(py*1280+px)*4;
        aligned=aligned && image[i]>140 && image[i+2]>100 && image[i+1]<100;
    }
    check(aligned,"resized nested popup follows its actual parent with equal horizontal and vertical pixel density");
    check(surfaces.size()==1,"transient nested menus do not persist as independent window poses");
    check(glGetError()==GL_NO_ERROR,"nested popup capture and placement has no GL errors");
    workspace.shutdown();
}

static void checkSpatialTools(Renderer& renderer,ImGuiContext* desktop) {
    VREditorWorkspace workspace;workspace.preview=true;workspace.wristToolsVisible=true;
    XRInputState state;state.sessionActive=state.hasFocus=true;
    state.headPose.position={0,1.6f,0};state.headPose.positionValid=state.headPose.orientationValid=true;
    auto& hand=state.controllers[1];hand.active=true;hand.aimPose=state.headPose;
    hand.aimPose.position={.35f,1.4f,-.25f};
    const glm::vec3 target(0,1.6f,-3);
    auto aim=[&](glm::vec3 point){hand.aimPose.rotation=glm::quatLookAt(glm::normalize(point-hand.aimPose.position),glm::vec3(0,1,0));};
    aim(target); // Aim is offset from the camera: exercise actual controller rays.
    Camera camera;camera.position=state.headPose.position;camera.front={0,0,-1};camera.solidBackground=true;camera.backgroundColor={.01f,.01f,.01f};
    SceneObject cube("Spatial tools cube",ObjectType::Cube,777);cube.hasRenderer=true;cube.renderType=RenderType::Cube;cube.position=target;
    cube.material.color={1,.01f,.01f};cube.material.ambientStrength=1;
    std::vector<SceneObject> scene{cube};
    glm::mat4 model=glm::translate(glm::mat4(1),target);
    bool useGizmo=false,passiveOverlay=false,capturePanel=false;ImGuizmo::OPERATION operation=ImGuizmo::TRANSLATE;
    const float bounds[]={-.5f,-.5f,-.5f,.5f,.5f,.5f};
    auto frame=[&]() {
        ImGui::SetCurrentContext(desktop);auto& io=ImGui::GetIO();io.DisplaySize={1280,800};io.DeltaTime=1.f/90;
        ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();ImGui::Render();
        workspace.beginUI(state,1.f/90,false);
        if(capturePanel) {
            ImGui::SetNextWindowPos({2000,1000});ImGui::SetNextWindowSize({200,120});
            ImGui::Begin("Gizmo capture fixture",nullptr,ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings);
            ImGui::Button("Previously selected tool");ImGui::End();
        }
        if(passiveOverlay) {
            ImGui::SetNextWindowPos({1300,900});ImGui::SetNextWindowSize({400,400});
            ImGui::Begin("Passive tooltip-style overlay",nullptr,ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("A passive overlay must not capture the controller ray.");ImGui::End();
        }
        if(useGizmo)workspace.manipulateGizmo(model,camera.getViewMatrix(),operation,ImGuizmo::WORLD,nullptr,operation==ImGuizmo::BOUNDS?bounds:nullptr);
        else workspace.clearGizmo();
        workspace.updateSceneHover(scene,camera.getViewMatrix(),1.f/90);
        workspace.endUI();workspace.renderPreview(renderer,&camera,&scene);
    };
    auto pixels=[&](){std::vector<unsigned char> out(1280*800*4);glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,out.data());return out;};
    auto purple=[](const auto& image){size_t count=0;for(size_t i=0;i<image.size();i+=4)if(image[i+2]>90 && image[i]>40 && int(image[i+2])>int(image[i+1])+25)++count;return count;};
    for(int i=0;i<30;++i)frame();
    const auto hovered=pixels();
    check(purple(hovered)>100,"controller hover renders a purple scene outline and ray without selection");
    float distance=0;const auto ray=glm::normalize(target-hand.aimPose.position);
    check(EditorScenePicking::PickRay(scene,hand.aimPose.position,ray,camera.getViewMatrix(),{},&distance)==777 && distance>2 && distance<4,
          "shared scene ray returns the actual surface distance for the beam endpoint");
    workspace.selection={777};aim({5,1.6f,-3});for(int i=0;i<30;++i)frame();
    check(purple(pixels())>100,"selected scene outline remains visible when the controller points away");
    aim(target);useGizmo=true;for(int i=0;i<3;++i)frame();
    {
        const auto image=pixels();std::ofstream file("../vr-editor-check/spatial-tools.ppm",std::ios::binary);
        file<<"P6\n1280 800\n255\n";
        for(int y=799;y>=0;--y)for(int x=0;x<1280;++x)file.write((const char*)&image[(y*1280+x)*4],3);
    }
    const auto noDrag=model;
    hand.buttons[(size_t)XRButton::Trigger]=true;frame();
    check(workspace.gizmoUsing(),"controller trigger captures the ImGuizmo center handle");
    aim(target+glm::vec3(.3f,.15f,0));frame();
    check(glm::length(glm::vec3(model[3]-noDrag[3]))>.1f,"controller ray dragging transforms the ImGuizmo model");
    const auto beforeStereo=model;workspace.renderPreview(renderer,&camera,&scene);
    check(model==beforeStereo,"drawing the gizmo for another view never applies input twice");
    state.hasFocus=false;const auto beforeLoss=model;frame();
    check(!workspace.gizmoUsing() && model==beforeLoss,"focus loss cancels a gizmo drag without moving the model");
    state.hasFocus=true;hand.buttons.fill(false);frame();
    check(workspace.panelCount()==0,"gizmo interaction canvas does not become a floating window");
    for(const auto tool:{ImGuizmo::ROTATE,ImGuizmo::SCALE,ImGuizmo::BOUNDS,ImGuizmo::UNIVERSAL}) {
        operation=tool;frame();check(glGetError()==GL_NO_ERROR,"stereo gizmo tool and outline rendering has no GL errors");
    }
    // Reprojection uses the asymmetric eye projection, rather than the desktop FOV.
    const auto projection=glm::perspective(glm::radians(95.f),1.6f,.03f,1000.f);
    for(float eye:{-.032f,.032f}) {
        Camera stereo=camera;stereo.position.x+=eye;
        auto asymmetric=projection;asymmetric[2][0]=eye*2;
        renderer.beginRender(stereo.getViewMatrix(),asymmetric,stereo.position);
        renderer.renderSceneToTarget(stereo,scene,0,1280,800,asymmetric,.03f,1000,true,true);renderer.endRender();
        workspace.drawSceneOverlays(renderer,stereo,scene,asymmetric,0,1280,800,.03f);
        check(glGetError()==GL_NO_ERROR,"asymmetric stereo eye renders scene outlines and gizmos without GL errors");
    }
    useGizmo=false;passiveOverlay=true;aim(target);hand.buttons.fill(false);frame();frame();
    hand.buttons[(size_t)XRButton::Trigger]=true;frame();
    glm::vec3 pickOrigin,pickDirection;
    check(workspace.takeScenePick(pickOrigin,pickDirection) &&
          EditorScenePicking::PickRay(scene,pickOrigin,pickDirection,camera.getViewMatrix())==777,
          "passive tooltip-style overlays do not steal controller hover or scene clicks");
    // A previous UI target must not reclaim a later scene drag through its
    // unbounded surface plane. Keep moving for many frames, as in a real edit.
    passiveOverlay=false;capturePanel=true;hand.buttons.fill(false);frame();frame();
    aim({.18f,1.64f,-1.15f});frame();frame();
    hand.buttons[(size_t)XRButton::Trigger]=true;frame();
    hand.buttons.fill(false);frame();frame();
    operation=ImGuizmo::TRANSLATE;model=glm::translate(glm::mat4(1),target);
    aim(target);useGizmo=true;frame();frame();
    hand.buttons[(size_t)XRButton::Trigger]=true;frame();
    check(workspace.gizmoUsing(),"gizmo captures after using a floating panel");
    bool held=true;
    for(int i=1;i<=12;++i) {
        aim(target+glm::vec3(i*.025f,i*.01f,0));frame();
        held=held && workspace.gizmoUsing();
    }
    check(held && glm::distance(glm::vec3(model[3]),target)>.2f,
          "previous panel cannot reclaim a sustained controller gizmo drag");
    hand.buttons[(size_t)XRButton::Grip]=true;
    aim(target+glm::vec3(.45f,.2f,0));const auto beforeGrip=model;frame();
    check(workspace.gizmoUsing() && glm::distance(glm::vec3(model[3]),glm::vec3(beforeGrip[3]))>.05f,
          "Index grip pressure does not cancel an active trigger gizmo drag");
    hand.buttons[(size_t)XRButton::Trigger]=false;frame();
    check(!workspace.gizmoUsing(),"releasing the trigger ends the gizmo drag even while grip is held");
    hand.buttons.fill(false);frame();
    model=glm::translate(glm::mat4(1),target);operation=ImGuizmo::TRANSLATE_X;
    const auto arrow=target+glm::vec3(.3f,0,0);aim(arrow);frame();frame();
    hand.buttons[(size_t)XRButton::Trigger]=true;frame();
    bool arrowHeld=workspace.gizmoUsing();
    for(int i=1;i<=12;++i) {
        aim(arrow+glm::vec3(i*.025f,i*.015f,0));frame();
        arrowHeld=arrowHeld && workspace.gizmoUsing();
    }
    check(arrowHeld && model[3].x>.2f && std::abs(model[3].y-target.y)<.001f,
          "translation arrow stays captured across frames and preserves its axis constraint");
    hand.buttons.fill(false);frame();
    workspace.shutdown();
}

int main() {
    checkEditorBindings();
    if(!glfwInit())return 2;
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,4);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,5);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    auto* window=glfwCreateWindow(1280,800,"VR Editor Probe",nullptr,nullptr);if(!window)return 3;
    glfwMakeContextCurrent(window);gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    auto* desktop=ImGui::CreateContext();ImGui_ImplOpenGL3_Init("#version 330 core");
    ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().DisplaySize={1280,800};
    checkMirrorIsolation();
    ImGuiShadeTheme shade;ImGui::StyleShadeThemeDesktop(&shade);ImGui::SetShadeTheme(shade);
    {
        Renderer renderer;renderer.initialize();renderer.resize(1280,800);
        VREditorWorkspace workspace;
        const auto folder=std::filesystem::absolute("../vr-editor-check/Workspace");
        std::filesystem::create_directories(folder);
        std::filesystem::remove(folder/"vr-docking.ini");
        std::filesystem::remove(folder/"vr-surfaces.txt");
        workspace.setStorage(folder);
        XRInputState state;state.sessionActive=state.hasFocus=true;
        state.headPose.position={0,1.6f,0};state.headPose.positionValid=state.headPose.orientationValid=true;
        int clicks=0,wristClicks=0;ImVec2 buttonCenter{};ImGuiID hierarchyRoot=0;bool initialized=false,narrowVisible=false,showBackdrop=false;
        auto frame=[&]() {
            ImGui::SetCurrentContext(desktop);auto& io=ImGui::GetIO();io.DeltaTime=1.f/90;io.DisplaySize={1280,800};
            ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();ImGui::Begin("Desktop");ImGui::TextUnformatted("Desktop stays independent");ImGui::End();ImGui::Render();
            workspace.beginUI(state,1.f/90,false);
            if(!initialized) {
                const char* names[]={"Hierarchy","Inspector","Project","Viewport"};
                const ImVec2 sizes[]={{620,820},{620,820},{1140,410},{1030,460}};
                const ImVec2 positions[]={{40,120},{2450,120},{900,1500},{900,80}};
                for(int i=0;i<4;++i){auto id=0x56520001u+i;ImGui::DockBuilderRemoveNode(id);ImGui::DockBuilderAddNode(id);
                    ImGui::DockBuilderSetNodePos(id,positions[i]);ImGui::DockBuilderSetNodeSize(id,sizes[i]);ImGui::DockBuilderDockWindow(names[i],id);ImGui::DockBuilderFinish(id);}
                initialized=true;
            }
            if(ImGui::Begin("Hierarchy")) {
                ImGui::TextUnformatted("Scene hierarchy");
                if(ImGui::Button("Select object",{180,42}))++clicks;
                const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();buttonCenter={(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f};
                hierarchyRoot=ImGui::GetCurrentWindow()->RootWindowDockTree->ID;
                ImGui::TextUnformatted("SCL\n  XR Camera\n  Left hand\n  Right hand\nPlane\nDirectional Light");
            }ImGui::End();
            ImGui::Begin("Inspector");ImGui::TextUnformatted("Transform\nPosition\nRotation\nScale\nMaterial\nRigidbody");ImGui::End();
            ImGui::Begin("Project");ImGui::TextUnformatted("Assets / Scenes   Scripts   Materials   Models");ImGui::End();
            ImGui::Begin("Viewport");ImGui::TextUnformatted("Viewport / Console tabs");ImGui::End();
            ImGui::SetNextWindowPos({1020,1050},ImGuiCond_FirstUseEver);ImGui::SetNextWindowSize({1000,214},ImGuiCond_FirstUseEver);
            ImGui::Begin("VR Management",nullptr,ImGuiWindowFlags_NoDocking);
            const char* files[]={"Engine Button On.png","Play Button Off.png","Test Mode Off.png","Pause Button Off.png","Time Slider.png"};
            for(int i=0;i<5;++i){if(i)ImGui::SameLine();ImGui::PushID(i);
                const std::string path=std::string("Resources/VR-Engine-Root/ActionBar/")+files[i];
                VREditorWidgets::IconButton(renderer,"##Action",path.c_str(),files[i],i==0);ImGui::PopID();}
            ImGui::TextUnformatted("Trigger: widgets and tabs   Grip: move windows");ImGui::End();
            if(workspace.wristToolsVisible && state.controllers[0].active && state.hasFocus) {
                ImGui::SetNextWindowPos({300,2500},ImGuiCond_Always);ImGui::SetNextWindowSize({414,116},ImGuiCond_Always);
                ImGui::Begin("VR Hand Tools",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings);
                if(ImGui::Button("Wrist select",{398,100}))++wristClicks;
                ImGui::End();
            }
            if(narrowVisible) {
                if(!ImGui::DockBuilderGetNode(0x56520005)) {
                    ImGui::DockBuilderAddNode(0x56520005);ImGui::DockBuilderSetNodePos(0x56520005,{100,200});
                    ImGui::DockBuilderSetNodeSize(0x56520005,{32,568});
                    ImGui::DockBuilderDockWindow("Optional narrow panel",0x56520005);ImGui::DockBuilderFinish(0x56520005);
                }
                ImGui::Begin("Optional narrow panel");ImGui::TextUnformatted("Optional editor panel");ImGui::End();
            }
            if(showBackdrop) {
                ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
                ImGui::Begin("ProjectLoadOverlay",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
                    ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoInputs);
                ImGui::End();
            }
            workspace.endUI();workspace.renderPreview(renderer);
        };
        for(int i=0;i<4;++i)frame();
        check(ImGui::GetCurrentContext()==desktop,"spatial frame restores desktop context");
        check(workspace.panelCount()>=5,"five independent root surfaces captured");
        workspace.save();auto surfaces=readSurfaces(folder);
        check(surfaces.size()>=5,"spatial poses persist separately from docking settings");
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(workspace.context());
        auto* hierarchy=ImGui::FindWindowByName("Hierarchy");
        const ImVec2 rootPos=hierarchy->RootWindowDockTree->Pos,rootSize=hierarchy->RootWindowDockTree->Size;
        ImGui::SetCurrentContext(previous);
        const auto original=surfaces[hierarchyRoot];
        glm::vec2 uv((buttonCenter.x-rootPos.x)/rootSize.x,(buttonCenter.y-rootPos.y)/rootSize.y);
        const auto target=original.position+original.rotation*glm::vec3((uv.x-.5f)*original.size.x,(.5f-uv.y)*original.size.y,0);
        auto& hand=state.controllers[1];hand.active=true;hand.aimPose=state.headPose;
        hand.aimPose.rotation=glm::quatLookAt(glm::normalize(target-state.headPose.position),glm::vec3(0,1,0));hand.gripPose=hand.aimPose;
        hand.gripPose.position+=glm::vec3(.2f,-.3f,-.2f);
        frame();hand.buttons[(size_t)XRButton::Trigger]=true;frame();frame();hand.buttons[(size_t)XRButton::Trigger]=false;frame();
        check(clicks==1,"controller ray activates a native ModuGUI widget exactly once");
        hand.buttons[(size_t)XRButton::Grip]=true;frame();hand.gripPose.position.x+=.25f;frame();
        hand.buttons[(size_t)XRButton::Grip]=false;frame();workspace.save();surfaces=readSurfaces(folder);
        check(glm::distance(original.position,surfaces[hierarchyRoot].position)>.20f,"grip moves a whole floating window in 3D");
        auto aimAtUI=[&](const char* name,ImVec2 point) {
            workspace.save();auto saved=readSurfaces(folder);
            ImGui::SetCurrentContext(workspace.context());auto* root=ImGui::FindWindowByName(name)->RootWindowDockTree;
            const auto surface=saved.at(root->ID);
            glm::vec2 at((point.x-root->Pos.x)/root->Size.x,(point.y-root->Pos.y)/root->Size.y);
            ImGui::SetCurrentContext(desktop);
            const auto world=surface.position+surface.rotation*glm::vec3((at.x-.5f)*surface.size.x,(.5f-at.y)*surface.size.y,0);
            hand.aimPose.rotation=glm::quatLookAt(glm::normalize(world-state.headPose.position),glm::vec3(0,1,0));
        };
        auto tabPoint=[&](const char* name) {
            ImGui::SetCurrentContext(workspace.context());auto* window=ImGui::FindWindowByName(name);
            auto* bar=window->DockNode->TabBar;ImVec2 point{};
            for(const auto& tab:bar->Tabs)if(tab.ID==window->TabId)point={bar->BarRect.Min.x+tab.Offset+tab.Width*.5f,(bar->BarRect.Min.y+bar->BarRect.Max.y)*.5f};
            ImGui::SetCurrentContext(desktop);return point;
        };
        const auto sourceTab=tabPoint("Hierarchy");aimAtUI("Hierarchy",sourceTab);frame();
        hand.buttons[(size_t)XRButton::Trigger]=true;frame();
        aimAtUI("Hierarchy",{sourceTab.x,sourceTab.y+120});frame();frame();
        auto targetTab=tabPoint("Inspector");aimAtUI("Inspector",targetTab);
        for(int i=0;i<18;++i)frame();
        hand.buttons[(size_t)XRButton::Trigger]=false;frame();frame();
        ImGui::SetCurrentContext(workspace.context());
        const bool docked=ImGui::FindWindowByName("Hierarchy")->DockId==ImGui::FindWindowByName("Inspector")->DockId;
        std::printf("[DockState] %08X %08X\n",ImGui::FindWindowByName("Hierarchy")->DockId,ImGui::FindWindowByName("Inspector")->DockId);
        ImGui::SetCurrentContext(desktop);
        check(docked,"controller trigger drag docks a tab onto another spatial window");
        if(docked) {
            const auto tab=tabPoint("Hierarchy");aimAtUI("Hierarchy",tab);frame();hand.buttons[(size_t)XRButton::Trigger]=true;frame();
            aimAtUI("Hierarchy",{tab.x-950,tab.y+500});for(int i=0;i<12;++i)frame();
            hand.buttons[(size_t)XRButton::Trigger]=false;frame();frame();
            ImGui::SetCurrentContext(workspace.context());
            const bool separated=ImGui::FindWindowByName("Hierarchy")->RootWindowDockTree!=ImGui::FindWindowByName("Inspector")->RootWindowDockTree;
            ImGui::SetCurrentContext(desktop);check(separated,"dragging a tab into free space creates an independent spatial window");
        }
        state.hasFocus=false;hand.buttons[(size_t)XRButton::Trigger]=true;frame();
        ImGui::SetCurrentContext(workspace.context());check(!ImGui::GetIO().MouseDown[0],"focus loss releases the virtual pointer");ImGui::SetCurrentContext(desktop);
        state.hasFocus=true;hand.buttons[(size_t)XRButton::Trigger]=false;
        // Root hit-test routing must work even for identical 2D window rectangles.
        auto& io=ImGui::GetIO();
        for(int i=0;i<2;++i){ImGui_ImplOpenGL3_NewFrame();io.DeltaTime=1.f/90;ImGui::NewFrame();
            for(const char* name:{"Overlap A","Overlap B"}){ImGui::SetNextWindowPos({100,100});ImGui::SetNextWindowSize({260,220});ImGui::Begin(name);ImGui::TextUnformatted(name);ImGui::End();}ImGui::Render();}
        auto* a=ImGui::FindWindowByName("Overlap A");io.AddMousePosEvent(180,180);
        ImGui_ImplOpenGL3_NewFrame();VREditorMouseInput::NewFrame(a->ID);check(ImGui::GetCurrentContext()->HoveredWindow==a,"compositor hit root overrides overlapping 2D stacking");ImGui::EndFrame();
        VREditorWorkspace::Surface s;s.position={0,0,-1};s.size={2,2};float distance;glm::vec2 result;
        check(VREditorWorkspace::intersect(s,{0,0,0},{0,0,-1},distance,result)&&std::abs(distance-1)<.001f&&glm::length(result-glm::vec2(.5f))<.001f,"ray/quad intersection maps UVs correctly");
        check(!VREditorWorkspace::intersect(s,{0,0,-2},{0,0,1},distance,result),"back faces reject pointer input");
        check(!VREditorWorkspace::intersect(s,{3,0,0},{0,0,-1},distance,result),"off-panel rays reject bounded input");
        check(VREditorWorkspace::intersect(s,{3,0,0},{0,0,-1},distance,result,false),"captured drags can leave a panel");
        state.controllers[0].active=true;state.controllers[1].active=true;
        state.controllers[0].buttons[(size_t)XRButton::ThumbstickClick]=state.controllers[1].buttons[(size_t)XRButton::ThumbstickClick]=true;
        int fired=0;for(int i=0;i<180;++i)fired+=workspace.returnGesture(state,1.f/90,true);
        check(fired==1,"return-to-editor chord fires once after a deliberate hold");
        state.hasFocus=false;fired=0;for(int i=0;i<90;++i)fired+=workspace.returnGesture(state,1.f/90,true);
        check(fired==0,"return shortcut ignores unfocused runtime input");
        state.hasFocus=true;state.controllers[0].buttons[(size_t)XRButton::ThumbstickClick]=false;
        state.controllers[1].buttons[(size_t)XRButton::ThumbstickClick]=false;workspace.returnGesture(state,1.f/90,true);
        state.controllers[0].buttons[(size_t)XRButton::Menu]=true;fired=0;
        for(int i=0;i<90;++i)fired+=workspace.returnGesture(state,1.f/90,true);
        check(fired==1,"left Menu fallback returns on simple and Vive controllers");
        for(const char* side:{"left","right"}) {
            const std::string path="Resources/VR-Engine-Root/Valve-Index-Knuckles/valve-index-knuckle-"+std::string(side)+"/source/valve_controller_knu_1_0_"+side+"/valve_controller_knu_1_0_"+side+".obj";
            auto loaded=getModelLoader().loadModel(path);check(loaded.success,"Index controller imports through the actual model loader");
        }
        for(const char* side:{"Left","Right"}) {
            auto loaded=getModelLoader().loadModel("Resources/VR-Engine-Root/Meta-Quest-Touch-Plus/models/MetaQuestTouchPlus_"+std::string(side)+".fbx");
            if(auto* mesh=getModelLoader().getMeshInfo(loaded.meshIndex)) {
                const auto size=mesh->boundsMax-mesh->boundsMin;
                std::printf("[TouchBounds] %s %.5f %.5f %.5f\n",side,size.x,size.y,size.z);
            }
            check(loaded.success,"Touch Plus controller imports through the actual model loader");
        }
        state.controllers[0].buttons.fill(false);state.controllers[1].buttons.fill(false);
        auto& left=state.controllers[0];left.gripPose=state.headPose;left.gripPose.position={-.25f,1.1f,-.5f};
        workspace.wristToolsVisible=true;frame();frame();
        left.axes2D[0].x=.9f;frame();check(workspace.takeToolStep()==0,"unmodified left stick is reserved for navigation");
        left.axes2D[0].x=0;frame();left.buttons[(size_t)XRButton::PrimaryButton]=true;
        left.axes2D[0].x=.9f;frame();check(workspace.takeToolStep()==1,"left stick flick selects the next editor tool");
        frame();check(workspace.takeToolStep()==0,"held stick does not repeatedly cycle tools");
        left.axes2D[0].x=0;frame();left.axes2D[0].x=-.9f;frame();check(workspace.takeToolStep()==-1,"neutral then left flick selects the previous tool");
        left.axes2D[0].x=0;frame();left.buttons[(size_t)XRButton::Grip]=true;left.axes2D[0].x=.9f;frame();
        check(workspace.takeToolStep()==0,"panel grip/resize gesture does not change tools");
        left.buttons.fill(false);left.axes2D[0].x=0;frame();
        state.hasFocus=false;left.axes2D[0].x=.9f;frame();check(workspace.takeToolStep()==0,"unfocused left stick cannot change tools");
        state.hasFocus=true;left.axes2D[0].x=0;frame();
        auto clickWrist=[&]() {
            const auto target=left.gripPose.position+glm::vec3(0,.16f,0);
            hand.aimPose.rotation=glm::quatLookAt(glm::normalize(target-hand.aimPose.position),glm::vec3(0,1,0));
            frame();hand.buttons[(size_t)XRButton::Trigger]=true;frame();frame();hand.buttons[(size_t)XRButton::Trigger]=false;frame();
        };
        clickWrist();left.gripPose.position.x+=.35f;frame();clickWrist();
        check(wristClicks==2,"right pointer can click hand strip before and after left-hand movement");
        workspace.save();ImGui::SetCurrentContext(workspace.context());const auto wristID=ImGui::FindWindowByName("VR Hand Tools")->ID;ImGui::SetCurrentContext(desktop);
        check(!readSurfaces(folder).contains(wristID),"tracked hand strip is excluded from saved room poses");
        workspace.navigationInitialized=true;workspace.sceneFromTracking=glm::mat4(1);
        left.buttons.fill(false);hand.buttons.fill(false);left.axes2D[0]={0,1};hand.axes2D[0]={0,0};
        for(int i=0;i<90;++i)workspace.updateNavigation(state,1.f/90);
        check(glm::distance(glm::vec3(workspace.sceneFromTracking[3]),glm::vec3(0,0,-2))<.001f,"left stick moves the immersive editor at a consistent speed without changing height");
        workspace.sceneFromTracking=glm::mat4(1);left.axes2D[0]={0,0};hand.axes2D[0]={1,0};
        state.headPose.position={.4f,1.6f,.25f};const auto pivot=state.headPose.position;
        for(int i=0;i<90;++i)workspace.updateNavigation(state,1.f/90);
        const auto turnedForward=glm::mat3(workspace.sceneFromTracking)*glm::vec3(0,0,-1);
        check(glm::distance(glm::vec3(workspace.sceneFromTracking*glm::vec4(pivot,1)),pivot)<.001f && turnedForward.x>.999f,"right stick turns smoothly to the right around the headset rather than the room origin");
        workspace.sceneFromTracking=glm::mat4(1);state.headPose.rotation=glm::quat(glm::vec3(0,glm::radians(90.f),0));
        hand.axes2D[0]={0,0};left.axes2D[0]={0,1};
        for(int i=0;i<90;++i)workspace.updateNavigation(state,1.f/90);
        check(glm::vec3(workspace.sceneFromTracking[3]).x< -1.99f,"editor movement follows headset heading");
        workspace.sceneFromTracking=glm::mat4(1);
        state.headPose.rotation=glm::angleAxis(glm::radians(45.f),glm::vec3(1,0,0));
        for(int i=0;i<90;++i)workspace.updateNavigation(state,1.f/90);
        check(std::abs(workspace.sceneFromTracking[3].y-std::sqrt(2.f))<.01f && std::abs(workspace.sceneFromTracking[3].z+std::sqrt(2.f))<.01f,
              "head-relative movement includes headset pitch instead of flattening onto the floor");
        workspace.sceneFromTracking=glm::mat4(1);left.buttons[(size_t)XRButton::PrimaryButton]=true;
        workspace.updateNavigation(state,.05f);check(glm::length(glm::vec3(workspace.sceneFromTracking[3]))==0,"tool-selection modifier suppresses movement");
        left.buttons.fill(false);state.hasFocus=false;workspace.updateNavigation(state,.05f);
        check(glm::length(glm::vec3(workspace.sceneFromTracking[3]))==0,"unfocused input cannot navigate the editor");state.hasFocus=true;
        left.buttons[(size_t)XRButton::Grip]=true;workspace.updateNavigation(state,.05f);
        check(glm::length(glm::vec3(workspace.sceneFromTracking[3]))==0,"panel grip manipulation suppresses movement");
        left.buttons.fill(false);workspace.wristToolsVisible=false;workspace.updateNavigation(state,.05f);
        check(glm::length(glm::vec3(workspace.sceneFromTracking[3]))==0,"editor navigation does not run during Play or project selection");
        workspace.wristToolsVisible=true;left.axes2D[0]={0,0};state.headPose.position={0,1.6f,0};state.headPose.rotation=glm::quat(1,0,0,0);
        for(const char* file:{"Engine Button Off.png","Engine Button On.png","Play Button Off.png","Play Button On.png","Test Mode Off.png","Test Mode On.png","Pause Button Off.png","Pause Button On.png","Time Slider.png"}) {
            auto* tex=renderer.getTexture(std::string("Resources/VR-Engine-Root/ActionBar/")+file,MaterialProperties::TextureFilter::Point);
            check(tex && tex->GetID(),"authored action-bar icon loads through renderer");
        }
        state.headPose.rotation=glm::quat(glm::vec3(glm::radians(-27.f),0,0));
        frame();
        glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());std::vector<unsigned char> pixels(1280*800*4);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        std::ofstream image("../vr-editor-check/workspace.ppm",std::ios::binary);image<<"P6\n1280 800\n255\n";
        for(int y=799;y>=0;--y)for(int x=0;x<1280;++x)image.write((const char*)&pixels[(y*1280+x)*4],3);
        auto error=glGetError();check(error==GL_NO_ERROR,"spatial capture/compositing and controller shaders have no GL errors");
        // Reverse triangle submission order without changing any positions, UVs,
        // normals or face winding. An opaque shell must render the same either way.
        workspace.wristToolsVisible=false;left.active=false;
        state.headPose.position={0,1.6f,0};state.headPose.rotation=glm::quat(1,0,0,0);
        const std::string rightPath="Resources/VR-Engine-Root/Valve-Index-Knuckles/valve-index-knuckle-right/source/valve_controller_knu_1_0_right/valve_controller_knu_1_0_right.obj";
        const auto loadedRight=getModelLoader().loadModel(rightPath);auto* controllerMesh=getModelLoader().getMesh(loadedRight.meshIndex);
        auto* controllerInfo=getModelLoader().getMeshInfo(loadedRight.meshIndex);
        hand.gripPose=state.headPose;hand.gripPose.position=state.headPose.position+glm::vec3(0,0,-.35f)-(controllerInfo->boundsMin+controllerInfo->boundsMax)*.5f;
        frame();workspace.renderPreview(renderer);
        std::vector<unsigned char> before(1280*800*4),after(before.size());
        glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,before.data());
        hand.active=false;frame();glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,after.data());
        size_t visiblePixels=0;for(size_t i=0;i<before.size();i+=4)if(std::abs(int(before[i])-int(after[i]))+std::abs(int(before[i+1])-int(after[i+1]))+std::abs(int(before[i+2])-int(after[i+2]))>12)++visiblePixels;
        check(visiblePixels>500,"depth regression renders a visible right controller");hand.active=true;frame();
        glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,before.data());
        std::vector<float> vertices(controllerMesh->getVertexCount()*8);glBindBuffer(GL_ARRAY_BUFFER,controllerMesh->getVertexBuffer());
        glGetBufferSubData(GL_ARRAY_BUFFER,0,vertices.size()*sizeof(float),vertices.data());auto reversed=vertices;
        for(size_t i=0;i<vertices.size()/24;++i)std::copy_n(vertices.data()+i*24,24,reversed.data()+vertices.size()-(i+1)*24);
        glBufferSubData(GL_ARRAY_BUFFER,0,reversed.size()*sizeof(float),reversed.data());workspace.renderPreview(renderer);
        glBindTexture(GL_TEXTURE_2D,workspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,after.data());
        size_t different=0;for(size_t i=0;i<before.size();i+=4)if(std::abs(int(before[i])-int(after[i]))+std::abs(int(before[i+1])-int(after[i+1]))+std::abs(int(before[i+2])-int(after[i+2]))>12)++different;
        check(different<1280*800*.002,"right controller outer surface is independent of triangle order (depth/self-occlusion regression)");
        std::printf("[ControllerDepth] changed pixels: %zu\n",different);
        glBindBuffer(GL_ARRAY_BUFFER,controllerMesh->getVertexBuffer());glBufferSubData(GL_ARRAY_BUFFER,0,vertices.size()*sizeof(float),vertices.data());
        narrowVisible=true;for(int i=0;i<4;++i)frame();workspace.save();
        ImGui::SetCurrentContext(workspace.context());const auto narrowID=ImGui::FindWindowByName("Optional narrow panel")->RootWindowDockTree->ID;ImGui::SetCurrentContext(desktop);
        auto legacy=readSurfaces(folder);check(legacy.contains(narrowID),"legacy panel regression starts with a captured dock root");workspace.setStorage(folder.parent_path()/"LegacyStaging");
        legacy[narrowID].size={.25f,.7473684f};
        { std::ofstream file(folder/"vr-surfaces.txt");for(const auto& [id,s]:legacy)
            file<<id<<' '<<s.position.x<<' '<<s.position.y<<' '<<s.position.z<<' '<<s.rotation.w<<' '<<s.rotation.x<<' '<<s.rotation.y<<' '<<s.rotation.z<<' '<<s.size.x<<' '<<s.size.y<<'\n'; }
        workspace.setStorage(folder);for(int i=0;i<4;++i)frame();workspace.save();
        ImGui::SetCurrentContext(workspace.context());auto* narrowRoot=ImGui::FindWindowByName("Optional narrow panel")->RootWindowDockTree;
        const auto narrowSize=narrowRoot->Size;ImGui::SetCurrentContext(desktop);
        const auto repaired=readSurfaces(folder).at(narrowID);
        std::printf("[PanelAspect] pixels %.1f %.1f meters %.6f %.6f\n",narrowSize.x,narrowSize.y,repaired.size.x,repaired.size.y);
        check(narrowSize.x>=360 && narrowSize.y>=220,"saved narrow optional dock restores to a usable pixel size");
        check(std::abs(repaired.size.x/repaired.size.y-narrowSize.x/narrowSize.y)<.0001f,"legacy stretched panel is corrected to its actual pixel aspect ratio");
        showBackdrop=true;frame();workspace.save();ImGui::SetCurrentContext(workspace.context());const auto backdropID=ImGui::FindWindowByName("ProjectLoadOverlay")->ID;ImGui::SetCurrentContext(desktop);
        check(!readSurfaces(folder).contains(backdropID),"full-screen loading dimmer does not become an unknown floating window");
        // Preview must contain the actual scene, rather than just UI on a clear color.
        {
            VREditorWorkspace sceneWorkspace;XRInputState previewState;
            previewState.hasFocus=previewState.sessionActive=true;previewState.headPose.position={0,1.6f,0};
            previewState.headPose.positionValid=previewState.headPose.orientationValid=true;
            previewState.headPose.rotation=glm::angleAxis(glm::radians(-20.f),glm::vec3(1,0,0));
            sceneWorkspace.beginUI(previewState,1.f/90,false);sceneWorkspace.endUI();
            sceneWorkspace.sceneFromTracking=glm::translate(glm::mat4(1),glm::vec3(3,0,0));
            Camera sceneCamera;sceneCamera.solidBackground=true;sceneCamera.backgroundColor={.02f,.03f,.04f};
            SceneObject cube("Preview regression cube",ObjectType::Cube,701);cube.hasRenderer=true;cube.renderType=RenderType::Cube;
            cube.position=glm::vec3(3,1.6f,0)+previewState.headPose.rotation*glm::vec3(0,0,-3);
            cube.scale=glm::vec3(1);cube.material.color={1,.02f,.02f};
            cube.material.emissionColor={1,0,0};cube.material.emissionStrength=1;cube.material.ambientStrength=1;
            std::vector<SceneObject> scene{cube};
            sceneWorkspace.renderPreview(renderer,&sceneCamera,&scene);
            std::vector<unsigned char> scenePixels(1280*800*4);
            glBindTexture(GL_TEXTURE_2D,sceneWorkspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,scenePixels.data());
            size_t objectPixels=0;for(size_t i=0;i<scenePixels.size();i+=4)if(int(scenePixels[i])+scenePixels[i+1]+scenePixels[i+2]>60)++objectPixels;
            check(objectPixels>2000,"loaded scene geometry renders into the desktop VR preview using the tracking-to-scene transform");
            std::printf("[ScenePreview] visible geometry pixels: %zu\n",objectPixels);
            std::ofstream snapshot("../vr-editor-check/scene-preview.ppm",std::ios::binary);snapshot<<"P6\n1280 800\n255\n";
            for(int y=799;y>=0;--y)for(int x=0;x<1280;++x)snapshot.write((const char*)&scenePixels[(y*1280+x)*4],3);
            scene[0].position.x+=10;sceneWorkspace.renderPreview(renderer,&sceneCamera,&scene);
            glBindTexture(GL_TEXTURE_2D,sceneWorkspace.mirrorTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,scenePixels.data());
            size_t offscreenPixels=0;for(size_t i=0;i<scenePixels.size();i+=4)if(int(scenePixels[i])+scenePixels[i+1]+scenePixels[i+2]>60)++offscreenPixels;
            check(offscreenPixels<objectPixels/10,"preview refreshes when scene geometry moves out of view");
            sceneWorkspace.shutdown();
        }
        error=glGetError();check(error==GL_NO_ERROR,"scene preview and repaired panel capture finish without GL errors");
        const auto otherFolder=folder.parent_path()/"WorkspaceOther";workspace.setStorage(otherFolder);
        check(workspace.panelCount()==0,"project change discards the previous room's live panel surfaces");
        checkPreviewInteraction(renderer,desktop,folder.parent_path()/"PreviewInteraction");
        checkSpatialTools(renderer,desktop);
        checkPopupPlacement(renderer,desktop,folder.parent_path()/"PopupPlacement");
        checkSavedLayoutsAndHands(desktop,folder.parent_path()/"SavedLayoutInput");
        std::printf("[VREditorProbe] failures=%d GL=%u\n",failures,error);std::fflush(stdout);
        workspace.shutdown();
    }
    ImGui::SetCurrentContext(desktop);ImGui_ImplOpenGL3_Shutdown();ImGui::DestroyContext(desktop);
    glfwDestroyWindow(window);glfwTerminate();return failures?1:0;
}
