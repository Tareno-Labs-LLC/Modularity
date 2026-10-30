#include "Engine.h"
#if !MODULARITY_RUNTIME_ONLY
#include "EditorLocalization.h"
#include "VREditorWidgets.h"
#include "Panels/GizmoToolbar.h"
#include "ScenePicking.h"
#include "ThirdParty/ModuGUI/imgui_internal.h"
#include <cstdlib>
namespace Loc = Modularity::Loc;

bool Engine::isVREditorActive() const {
    return vrEditor.requested && (xrSystem.isCreated() || vrEditor.preview);
}

void Engine::renderVREditorEntry() {
    if(isVREditorActive() || !ImGui::BeginMenu("VR Editor"))return;
#if MODULARITY_HAS_OPENXR
    ImGui::BeginDisabled(usingVulkan());
    if(ImGui::MenuItem(vrEditor.requested?"Retry VR Editor":"Enter VR Editor")) {
        vrEditor.requested=true;xrStartupAttempted=false;vrEditor.recenterPending=true;
        gameViewCursorLocked=false;cursorLocked=false;
    }
    if(ImGui::MenuItem("Preview layout")){vrEditor.requested=vrEditor.preview=true;}
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+ImGui::GetFontSize()*28.f);
    if(usingVulkan())ImGui::TextUnformatted("VR Editor requires the OpenGL renderer.");
    else if(!xrStartupError.empty())ImGui::TextWrapped("%s",xrStartupError.c_str());
    ImGui::PopTextWrapPos();
#else
    ImGui::TextUnformatted("This build has no OpenXR support.");
#endif
    ImGui::EndMenu();
}

void Engine::renderVREditorMonitor() {
    auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(viewport->Pos);ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("VR Editor Monitor",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted(vrEditor.preview&&!xrSystem.isRunning()?"Modularity / VR workspace preview":"Modularity / VR editor");
    ImGui::SameLine();if(ImGui::Button("Return to desktop"))vrEditor.pendingAction=3;
    ImGui::SameLine();if(ImGui::Button("Recenter windows"))vrEditor.recenterPending=true;
    ImGui::TextUnformatted("Left stick: move     Right stick: smooth turn     Left A/X + stick: select tool");
    ImGui::TextUnformatted("Exit Play: hold both thumbstick clicks (or left Menu) for 0.7 seconds.");
    if(!vrEditor.preview)ImGui::TextUnformatted("Index panels: squeeze to grab; tune the force in Layout / Input.");
    if(vrEditor.preview)ImGui::TextUnformatted("Left click: select / UI     Drag a title or tab: move / dock     Right drag: move a whole panel");
    if(vrEditor.mirrorTexture()) {
        auto size=ImGui::GetContentRegionAvail();const float height=std::min(size.y,size.x/1.6f);
        if(height>0)vrEditor.renderPreviewImage({height*1.6f,height});
    } else if(vrEditor.preview)ImGui::TextUnformatted("Preparing workspace preview...");
    else if(!xrSystem.isRunning())ImGui::TextUnformatted("Starting VR session...");
    else if(!xrSystem.session().shouldRender())ImGui::TextUnformatted("Put on the headset to display the VR editor.");
    else if(!Modularity::XR::InputState().headPose.isValid())ImGui::TextUnformatted("Waiting for headset tracking...");
    else ImGui::TextUnformatted("Preparing headset mirror...");
    ImGui::End();
}

void Engine::renderVREditorUI(const Modularity::XR::XRInputState& trackedInput) {
    Modularity::XR::XRInputState previewInput;
    const auto* frameInput=&trackedInput;
    if(vrEditor.preview && !xrSystem.isRunning()) {
        // Keep the desktop editor's look angle in the virtual headset. A camera
        // above the scene often looks down; resetting its pitch shows empty sky.
        previewInput=trackedInput;
        const auto look=glm::angleAxis(glm::radians(std::clamp(camera.pitch,-85.f,85.f)),glm::vec3(1,0,0));
        previewInput.headPose.rotation=look;
        for(auto& hand:previewInput.controllers) {
            hand.aimPose.rotation=look*hand.aimPose.rotation;
            hand.gripPose.rotation=look*hand.gripPose.rotation;
            hand.gripPose.position=previewInput.headPose.position+look*(hand.gripPose.position-previewInput.headPose.position);
        }
        frameInput=&previewInput;
    }
    const auto& input=*frameInput;
    const auto layoutRoot=projectManager.currentProject.isLoaded
        ? projectManager.currentProject.projectPath/"ProjectUserSettings"/"VRWorkspace"
        : fs::path("Resources")/"VRWorkspace";
    vrEditor.setStorage(layoutRoot);
    if(!vrEditor.findControllerPackage) {
        vrEditor.findControllerPackage=[this]() {
            const fs::path root=packageManager.installedPackageRoot("moduengine.vr-controllers");
            static bool reported=false;
            if(root.empty() && !reported) {
                reported=true;
                addConsoleMessage("VR controller models are not installed. Add the \"VR Controller Models\" ModuPAK "
                                  "(moduengine.vr-controllers) from the Package Manager to see your controllers in VR.",
                                  ConsoleMessageType::Info);
            }
            return root;
        };
    }
    auto* mainContext=ImGui::GetCurrentContext();
    vrEditor.wristToolsVisible=!showLauncher && !isPlaying;
    if(vrEditor.wristToolsVisible && !sceneLoadInProgress && input.headPose.isValid()) {
        if(!vrEditor.navigationInitialized) {
            const auto headForward=input.headPose.rotation*glm::vec3(0,0,-1);
            const float viewYaw=std::atan2(-camera.front.x,-camera.front.z);
            const float headYaw=std::atan2(-headForward.x,-headForward.z);
            vrEditor.sceneFromTracking=glm::translate(glm::mat4(1),camera.position)*
                glm::rotate(glm::mat4(1),viewYaw-headYaw,glm::vec3(0,1,0))*glm::translate(glm::mat4(1),-input.headPose.position);
            vrEditor.navigationInitialized=true;
        }
        vrEditor.updateNavigation(input,deltaTime);
        xrSystem.setOriginToWorld(vrEditor.sceneFromTracking);
        // Only the editor camera moves; tracked input never writes into scene objects.
        camera.position=glm::vec3(vrEditor.sceneFromTracking*glm::vec4(input.headPose.position,1));
        camera.front=glm::normalize(glm::mat3(vrEditor.sceneFromTracking)*(input.headPose.rotation*glm::vec3(0,0,-1)));
        camera.up=glm::normalize(glm::mat3(vrEditor.sceneFromTracking)*(input.headPose.rotation*glm::vec3(0,1,0)));
        camera.yaw=glm::degrees(std::atan2(camera.front.z,camera.front.x));
        camera.pitch=glm::degrees(std::asin(std::clamp(camera.front.y,-1.f,1.f)));
        camera.orthographic=false;
    }
    vrEditor.beginUI(input,deltaTime,showLauncher);
    const ImGuizmo::OPERATION tools[]={ImGuizmo::TRANSLATE,ImGuizmo::ROTATE,ImGuizmo::SCALE,ImGuizmo::BOUNDS,ImGuizmo::UNIVERSAL};
    if(const int step=vrEditor.takeToolStep()) {
        int index=0;for(int i=0;i<5;++i)if(mCurrentGizmoOperation==tools[i])index=i;
        mCurrentGizmoOperation=tools[(index+step+5)%5];
    }
    // Share the already-loaded font catalog, never mutate a locked shared atlas.
    if(auto it=uiFontContexts.find(mainContext);it!=uiFontContexts.end())uiFontContexts[vrEditor.context()]=it->second;
    const ImGuiID desktopDockspace=mainDockspaceId;
    mainDockspaceId=0;
    const bool wasFullscreen=viewportFullscreen;viewportFullscreen=false;
    if(vrEditor.resetLayout) {
        for(ImGuiID id=0x56520001;id<=0x56520004;++id)ImGui::DockBuilderRemoveNode(id);
        vrEditor.initializedLayout=false;vrEditor.resetLayout=false;vrEditor.recenterPending=true;
    }
    if(!showLauncher && !isPlaying && !vrEditor.initializedLayout) {
        struct Dock {ImGuiID id;ImVec2 position,size;const char* first;const char* second;};
        const Dock docks[]={
            {0x56520001,{40,120},{620,820},Loc::Window("WINDOW_HIERARCHY","Hierarchy"),Loc::Window("WINDOW_CAMERA","Camera")},
            {0x56520002,{2450,120},{620,820},Loc::Window("WINDOW_INSPECTOR","Inspector"),Loc::Window("WINDOW_ENVIRONMENT","Environment")},
            {0x56520003,{900,1500},{1140,410},Loc::Window("WINDOW_PROJECT","Project"),Loc::Window("WINDOW_PROJECT_SETTINGS","Project Settings")},
            {0x56520004,{900,80},{1030,460},Loc::Window("WINDOW_VIEWPORT","Viewport"),Loc::Window("WINDOW_CONSOLE","Console")},
        };
        for(const auto& dock:docks)if(!ImGui::DockBuilderGetNode(dock.id)) {
            ImGui::DockBuilderAddNode(dock.id,ImGuiDockNodeFlags_None);
            ImGui::DockBuilderSetNodePos(dock.id,dock.position);ImGui::DockBuilderSetNodeSize(dock.id,dock.size);
            ImGui::DockBuilderDockWindow(dock.first,dock.id);if(dock.second)ImGui::DockBuilderDockWindow(dock.second,dock.id);
            ImGui::DockBuilderFinish(dock.id);
        }
        vrEditor.initializedLayout=true;
    }
    if(showLauncher) {
        // Reuse the real project selector, including recent projects and dialogs.
        renderLauncher();
    } else if(!isPlaying) {
        if(showHierarchy)renderHierarchyPanel();
        if(showInspector)renderInspectorPanel();
        if(showFileBrowser)renderFileBrowserPanel();
        if(showViewport) {
            if(vrEditor.viewportPanelVisible)renderViewport();
            else {
                if(ImGui::Begin(Loc::Window("WINDOW_VIEWPORT","Viewport"),&showViewport)) {
                    ImGui::TextUnformatted("The headset is your scene viewport.");
                    ImGui::TextUnformatted("Look around naturally. Left stick moves; right stick turns smoothly.");
                    ImGui::TextUnformatted("Hold left A/X + flick the stick to change the hand tool.");
                    if(const auto* selected=getSelectedObject())ImGui::Text("Selected: %s",selected->name.c_str());
                    else ImGui::TextUnformatted("Point at a scene object and press trigger to select it.");
                    ImGui::Checkbox("Show additional 2D viewport tools",&vrEditor.viewportPanelVisible);
                }ImGui::End();
            }
        }
        if(showGameViewport && vrEditor.viewportPanelVisible)renderGameViewportWindow();
        if(showConsole)renderConsolePanel();
        if(showEnvironmentWindow)renderEnvironmentWindow();
        if(showCameraWindow)renderCameraWindow();
        if(showAnimationWindow)renderAnimationWindow();
        if(showProjectBrowser)renderProjectBrowserPanel();
        if(showRegistryPackagesWindow)renderRegistryPackagesWindow();
        if(showBuildSettings)renderBuildSettingsWindow();
        if(showGameProfilerWindow)renderGameProfilerWindow();
        if(showVersionControlWindow)renderVersionControlWindow();
        if(showScriptingWindow && hasScriptingWindowPackage())renderScriptingWindow();
        if(showMeshBuilder && hasMeshBuilderPackage())renderMeshBuilderPanel();
        if(showLightmappingWindow)renderLightmappingWindow();
        if(showXRPreviewWindow)renderXRPreviewWindow();
        if(showAIPathfindingWindow)renderAIPathfindingWindow();
        if(showPixelSpriteEditorWindow && hasSpriteEditorPackage())renderPixelSpriteEditorWindow();
        if(showVisualScriptingWindow)renderVisualScriptingWindow();
        if(showSectorMapWindow)renderSectorMapWindow();
        if(showAssemblageWindow)renderAssemblageWindow();
        if(showModularityDoctorWindow)renderModularityDoctorWindow();
        renderScriptEditorWindows();
    }
    ImGui::SetNextWindowPos({1020,1050},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({1000,230},{1600,500});
    ImGui::SetNextWindowSize({1000,230},ImGuiCond_FirstUseEver);
    if(ImGui::Begin("VR Management",nullptr,ImGuiWindowFlags_MenuBar|ImGuiWindowFlags_NoDocking)) {
        if(!showLauncher && !isPlaying)renderMainMenuBar(true);
        else if(ImGui::BeginMenuBar()){ImGui::TextUnformatted("Modularity VR");ImGui::EndMenuBar();}
        auto actionButton=[&](const char* id,const char* name,const char* label,bool on) {
            const std::string path=std::string("Resources/VR-Engine-Root/ActionBar/")+name+(on?" On.png":" Off.png");
            return VREditorWidgets::IconButton(renderer,id,path.c_str(),label,on);
        };
        const float iconRowWidth=72*5+ImGui::GetStyle().ItemSpacing.x*4;
        ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x,(ImGui::GetWindowWidth()-iconRowWidth)*.5f));
        // Five authored icons, in the same order as the desktop controls/sketch.
        ImGui::BeginDisabled(sceneLoadInProgress);
        if(actionButton("##VREngine","Engine Button",isPlaying?"Stop Play and return to editor":"Editor / project selection",!isPlaying)) {
            if(isPlaying)vrEditor.pendingAction=1;
            else ImGui::OpenPopup("VR Engine");
        }
        if(ImGui::BeginPopup("VR Engine")) {
            if(ImGui::MenuItem("Project selection",nullptr,false,!showLauncher))vrEditor.pendingAction=4;
            if(ImGui::MenuItem("Return to desktop"))vrEditor.pendingAction=3;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(showLauncher || !projectManager.currentProject.isLoaded);
        if(actionButton("##VRPlay","Play Button",isPlaying?"Stop Play Mode":"Play Mode",isPlaying))vrEditor.pendingAction=1;
        ImGui::SameLine();
        if(actionButton("##VRSpec","Test Mode",specMode?"Disable Spec Mode":"Spec Mode",specMode))vrEditor.pendingAction=5;
        ImGui::SameLine();ImGui::BeginDisabled(!isPlaying);
        if(actionButton("##VRPause","Pause Button",isPaused?"Resume":"Pause",isPaused))vrEditor.pendingAction=2;
        ImGui::EndDisabled();ImGui::SameLine();
        if(VREditorWidgets::IconButton(renderer,"##VRTime","Resources/VR-Engine-Root/ActionBar/Time Slider.png","Time scale",gameTimeScale!=1.f))ImGui::OpenPopup("VR Time Scale");
        if(ImGui::BeginPopup("VR Time Scale")) {
            ImGui::SetNextItemWidth(280);
            ImGui::SliderFloat("Speed",&gameTimeScale,kMinGameTimeScale,kMaxGameTimeScale,"%.2fx");
            if(ImGui::IsItemDeactivatedAfterEdit())saveEditorUserSettings();
            if(ImGui::Button("Normal speed")){gameTimeScale=1.f;saveEditorUserSettings();}
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if(ImGui::Button("Recenter",{110,34}))vrEditor.recenterPending=true;
        ImGui::SameLine();if(ImGui::Button("Reset layout",{130,34}))vrEditor.resetLayout=true;
        ImGui::SameLine();if(ImGui::Button("Keyboard",{115,34}))vrEditor.keyboardVisible=!vrEditor.keyboardVisible;
        ImGui::SameLine();if(ImGui::Button("Desktop",{110,34}))vrEditor.pendingAction=3;
        ImGui::SameLine();
        if(ImGui::BeginMenu("Layout / Input")) {
            static char layoutName[49]="My VR layout";
            static std::string layoutResult;
            ImGui::SetNextItemWidth(230);ImGui::InputText("Layout name",layoutName,sizeof(layoutName));
            if(ImGui::Button("Save named layout"))layoutResult=vrEditor.saveLayout(layoutName)?"Layout saved.":"Use a name with letters, numbers, spaces, - or _.";
            for(const auto& name:vrEditor.layoutNames())if(ImGui::MenuItem(name.c_str()))vrEditor.pendingLayoutLoad=name;
            if(ImGui::Button("Save current layout")){vrEditor.save();layoutResult="Current layout saved.";}
            if(!layoutResult.empty())ImGui::TextWrapped("%s",layoutResult.c_str());
            ImGui::Separator();ImGui::SetNextItemWidth(230);
            ImGui::SliderFloat("Grab force",&vrEditor.grabForce,.1f,1.f,"%.2f");
            ImGui::SetItemTooltip("Measured squeeze force. Default 0.36 uses 60%% of the old 0.60 threshold.");
            if(ImGui::IsItemDeactivatedAfterEdit())vrEditor.save();
            if(ImGui::Checkbox("Stabilize hands and clicks",&vrEditor.smoothHands))vrEditor.save();
            ImGui::TextUnformatted("Touchpad swipe: scroll | Press: context menu");
            ImGui::EndMenu();
        }
        ImGui::SameLine();
        if(!isPlaying && ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Hierarchy",nullptr,&showHierarchy);ImGui::MenuItem("Inspector",nullptr,&showInspector);
            ImGui::MenuItem("Project files",nullptr,&showFileBrowser);ImGui::MenuItem("Viewport",nullptr,&showViewport);
            ImGui::MenuItem("Console",nullptr,&showConsole);ImGui::MenuItem("Project settings",nullptr,&showProjectBrowser);
            ImGui::MenuItem("Additional 2D viewport",nullptr,&vrEditor.viewportPanelVisible);
            ImGui::EndMenu();
        }
        ImGui::TextUnformatted("Left stick: move   |   Right stick: turn   |   Left A/X + stick: tool   |   Grip + stick: panel size / depth");
        ImGui::TextUnformatted("Return from Play: hold both stick clicks or left Menu for 0.7s");
    }ImGui::End();
    const auto& left=input.controllers[0];
    if(vrEditor.wristToolsVisible && input.hasFocus && left.active && left.gripPose.isValid()) {
        ImGui::SetNextWindowPos({300,2500},ImGuiCond_Always);ImGui::SetNextWindowSize({414,116},ImGuiCond_Always);
        if(ImGui::Begin("VR Hand Tools",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoDocking|
            ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoNavFocus)) {
            const GizmoToolbar::Icon icons[]={GizmoToolbar::Icon::Translate,GizmoToolbar::Icon::Rotate,GizmoToolbar::Icon::Scale,GizmoToolbar::Icon::Bounds,GizmoToolbar::Icon::Universal};
            const char* labels[]={"Move","Rotate","Scale","Bounds","Universal"};
            for(int i=0;i<5;++i) {
                if(i)ImGui::SameLine();ImGui::PushID(i);
                if(VREditorWidgets::IconButton(renderer,"##Tool",GizmoToolbar::IconTexturePath(icons[i]),labels[i],mCurrentGizmoOperation==tools[i]))mCurrentGizmoOperation=tools[i];
                ImGui::PopID();
            }
            ImGui::TextUnformatted("Hold left A/X + stick: change tool");
        }ImGui::End();
    }
    // Existing engine dialogs remain usable in the spatial context as well.
    renderTermsOfServiceModal();renderWindowsDisclaimerPopup();renderLowSpecBenchmarkPopup();
    renderModuCppNvimWarningPopup();renderEditorLanguagePromptPopup();renderDialogs();
    renderModuPakExportDialog();renderModuPakImportDialog();renderModuObjExportDialog();renderModuObjImportDialog();
    vrEditor.renderKeyboard();
    // Run controller interaction once using a head-center view. Each eye later
    // draws the same gizmo with its own projection, without changing objects.
    if(!showLauncher && !isPlaying && !sceneLoadInProgress) {
        if(auto* object=getSelectedObject();object && !object->IsInvariable) {
            auto model=ComposeTransform(object->position,object->rotation,object->scale);
            const auto before=model;
            const bool wasUsing=vrEditor.gizmoUsing();
            float rotationSnap[3]={rotationSnapValue,rotationSnapValue,rotationSnapValue};
            const float* snap=useSnap?(mCurrentGizmoOperation==ImGuizmo::ROTATE?rotationSnap:snapValue):nullptr;
            glm::vec3 center(0),extents(.5f);
            Renderer::getObjectLocalBounds(*object,center,extents);
            const float bounds[]={center.x-extents.x,center.y-extents.y,center.z-extents.z,
                                  center.x+extents.x,center.y+extents.y,center.z+extents.z};
            const bool changed=vrEditor.manipulateGizmo(model,camera.getViewMatrix(),mCurrentGizmoOperation,
                mCurrentGizmoMode,snap,mCurrentGizmoOperation==ImGuizmo::BOUNDS?bounds:nullptr);
            if(!wasUsing && vrEditor.gizmoUsing())recordState("gizmo");
            if(changed) {
                const auto delta=model*glm::inverse(before);
                for(int id:selectedObjectIds) {
                    auto* selected=findObjectById(id);if(!selected || selected->IsInvariable)continue;
                    bool selectedAncestor=false;
                    for(auto* parent=findObjectById(selected->parentId);parent;parent=findObjectById(parent->parentId)) {
                        if(std::find(selectedObjectIds.begin(),selectedObjectIds.end(),parent->id)!=selectedObjectIds.end()) {
                            selectedAncestor=true;break;
                        }
                    }
                    if(selectedAncestor)continue;
                    glm::vec3 position,rotation,scale;
                    DecomposeMatrix(delta*ComposeTransform(selected->position,selected->rotation,selected->scale),
                        position,rotation,scale,&selected->scale,&selected->rotation);
                    selected->position=position;selected->rotation=glm::degrees(rotation);selected->scale=scale;
                    syncLocalTransform(*selected);
                }
                updateHierarchyWorldTransforms(true);
                projectManager.currentProject.hasUnsavedChanges=true;
            }
        } else vrEditor.clearGizmo();
        vrEditor.updateSceneHover(sceneObjects,camera.getViewMatrix(),deltaTime);
    } else vrEditor.clearGizmo();
    glm::vec3 pickOrigin,pickDirection;
    if(vrEditor.takeScenePick(pickOrigin,pickDirection) && !showLauncher && !isPlaying && !sceneLoadInProgress) {
        const int id=EditorScenePicking::PickRay(sceneObjects,pickOrigin,pickDirection,camera.getViewMatrix());
        if(id>=0)setPrimarySelection(id,ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
        else clearSelection();
    }
    vrEditor.selection=selectedObjectIds;
    vrEditor.endUI();
    if(!vrEditor.pendingLayoutLoad.empty()) {
        const auto name=std::move(vrEditor.pendingLayoutLoad);vrEditor.pendingLayoutLoad.clear();
        if(!vrEditor.loadLayout(name))addConsoleMessage("Could not load VR layout: "+name,ConsoleMessageType::Warning);
    }
    mainDockspaceId=desktopDockspace;viewportFullscreen=wasFullscreen;
}

void Engine::applyVREditorActions() {
    const int action=vrEditor.pendingAction;vrEditor.pendingAction=0;
    if(action==1 && !showLauncher && projectManager.currentProject.isLoaded && !sceneLoadInProgress) {
        togglePlayMode();gameViewCursorLocked=false;cursorLocked=false;
    } else if(action==2 && isPlaying)togglePause();
    else if(action==5 && !showLauncher && projectManager.currentProject.isLoaded)toggleSpecMode();
    else if(action==3) {
        vrEditor.save();vrEditor.requested=vrEditor.preview=false;
        vrEditor.navigationInitialized=false;
        if(!isPlaying)shutdownXRSystem();
    } else if(action==4 && !isPlaying) {
        if(projectManager.currentProject.hasUnsavedChanges)
            requestSceneSave(projectManager.currentProject.currentSceneName,PendingScenePostAction::CloseProject,"",true);
        else performCloseProject();
    }
    if(!vrEditor.pendingProject.empty()) {
        auto path=std::move(vrEditor.pendingProject);vrEditor.pendingProject.clear();startProjectLoad(path);
    }
}

void Engine::updateVREditorShortcut() {
    if(vrEditor.returnGesture(Modularity::XR::InputState(),deltaTime,isPlaying)) {
        vrEditor.requested=true;
        togglePlayMode();gameViewCursorLocked=false;cursorLocked=false;
        viewportController.setFocused(false);glfwSetInputMode(editorWindow,GLFW_CURSOR,GLFW_CURSOR_NORMAL);
    }
    // Desktop return can be requested from the mirror even when a headset loses focus.
    if(vrEditor.pendingAction==3)applyVREditorActions();
}
#endif
