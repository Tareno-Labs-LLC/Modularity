# VR editor workspace

Use **Tools → VR Editor → Enter VR Editor** in the editor. In the project selector,
select a VR-enabled project and choose **Open Project in VR** below **Open Project**.
The regular **Open Project** button opens the desktop editor. You can also launch
`Modularity.exe --vr-editor --project "path/to/project.modu"`. OpenGL and an
OpenXR-enabled build are required. The desktop displays a mirror with a return
button. **Tools → VR Editor → Preview layout** renders the loaded scene behind the spatial UI and
allows mouse testing without starting OpenXR. It keeps the editor camera's look
angle so a scene viewed from above remains visible when preview starts.

The workspace uses the actual editor panels in a separate ModuGUI context.
Hierarchy starts on the left, Inspector on the right, Project files below,
Viewport controls/Console above, and management controls at the bottom. The
scene itself fills the headset with tracked stereo views, starting at the editor
camera position. An additional flat viewport is optional under Panels. Drag tabs onto
other panels to dock them; drag a tab into space to separate it. Layout and
physical panel poses are saved per project in `ProjectUserSettings/VRWorkspace`.
The project selector has its own layout in `Resources/VRWorkspace`.
Panels preserve their pixel aspect ratio when rendered, resized or restored.
Older stretched poses are corrected when loaded, and narrow optional dock nodes
are expanded to a usable size. Camera, Environment and Project Settings start as
tabs in the main panel groups. Full-screen loading dimmers do not become separate
floating windows; the loading card remains visible.

## Controls

| Input | Action |
| --- | --- |
| Controller aim + trigger | Point, click, drag widgets and dock tabs |
| Trigger pointed at the scene | Select an object; an empty-space click clears selection |
| Drag a window title | Move the floating panel in world space |
| Grip while pointing at a panel | Move the entire panel |
| Grip + joystick up/down | Adjust panel distance |
| Grip + joystick left/right | Resize the panel |
| Right joystick up/down without grip | Scroll the pointed panel |
| Left joystick | Move through the scene relative to the headset, including pitch (2 m/s) |
| Right joystick left/right | Smooth turn around your head (90 degrees/s) |
| Hold left A/X + joystick left/right | Previous/next transform tool; return to center between flicks |
| Hold both joystick clicks for 0.7 seconds in Play | Stop Play and return to the VR editor |
| Hold left Menu for 0.7 seconds in Play | Alternative return shortcut on controllers that expose Menu |

The five-button transform strip follows a position 16 cm above the left grip
and faces the wearer. It selects the same Move, Rotate, Scale, Bounds and
Universal operations used by the viewport. The right controller can also click
the strip. It hides in Play, the project selector, and when left tracking is
unavailable. Grip gestures take priority over tool cycling and navigation.
Navigation only moves the editor camera, and pauses during UI dragging, tracking
loss and tool selection; scene transforms and game locomotion are unaffected. On Valve Index,
use the joystick-click chord to return: the system owns its Menu button.
The Index VR editor session binds grip to squeeze **force**, so closing your
fingers around the handle does not grab a panel. Squeeze firmly to grab, then
relax pressure to release; press/release hysteresis prevents chatter. This policy
is session-only and does not change saved project bindings or other controllers.
The editor's default grab threshold is **0.36** measured force (60% of the previous
0.6 threshold), with release at 65% of that threshold. **Layout / Input → Grab
force** adjusts and saves it per VR workspace.

On Index, swipe the controller touchpad vertically or horizontally to scroll the
pointed UI. Press the touchpad to open native right-click menus; joystick
navigation remains separate. Initial touch never scrolls, and pad presses use
force hysteresis to avoid repeated right clicks.

Hand stabilization uses a short, frame-rate-independent filter (22 ms for aim,
18 ms for grip). Trigger and touchpad clicks hold their aim through movements
below 6 mm / 0.45 degrees, then unlock for intentional dragging. Grip drags use
the same movement threshold. Tracking/focus loss resets the filter; desktop
preview input and the game's tracked poses are unaffected. Disable this in
**Layout / Input → Stabilize hands and clicks**.

The desktop switches to its VR monitor as soon as the XR session is created,
including while the headset is waking up. Startup, headset visibility, tracking
and mirror preparation have separate status messages. XR restores the previous
framebuffer after submission, and desktop UI draws to the window framebuffer.
VR entry initializes the OpenGL renderer before creating the XR session, even
from the project selector. Renderer initialization failures remain on desktop
with a visible error instead of leaving SteamVR waiting for frames.

VR layouts automatically save native docking/tab/window state and physical
window positions, rotations and sizes. Startup restores the arrangement relative
to the new headset anchor instead of discarding it during recentering. The
**Layout / Input** menu can save the current layout immediately, save a named
layout, or load a named layout. Each project stores these in
`ProjectUserSettings/VRWorkspace/Layouts`; the project selector uses
`Resources/VRWorkspace/Layouts`. Named layouts include input preferences.

Scene selection outlines use each eye's asymmetric OpenXR projection and the
same mask shader and selection animation as the desktop editor. Pointing at an
object fades the ray to purple and briefly pulses its hover outline. The beam
ends at the picked surface; UI takes precedence and uses white, or gray for a
disabled control. Nested popups follow their actual parent at a shared pixel
density, including while auto-fitting after opening.

Transform handles render in stereo using ImGuizmo. Interaction runs once per
frame from the controller ray; each eye reprojects the handles without applying
the transform again. Trigger-drag a handle to edit the selection with normal
undo, snapping, parent transforms and multi-selection root propagation.

The management bar uses the nine On/Off images in
`Resources/VR-Engine-Root/ActionBar` across five slots:

1. **Engine:** return from Play; while editing, open project selection/desktop commands.
2. **Play:** enter or stop Play.
3. **Yellow triangle:** Spec Mode, matching the desktop play controls.
4. **Red square:** pause/resume, matching the desktop pause control.
5. **Clock:** open the game time-scale slider.

The icon art is drawn over ModuGUI's existing shaded button surfaces; both the
normal style and bevel/gradient theme are shared with the desktop. Management
also offers recenter, reset layout, keyboard, desktop and panel visibility.
The floating keyboard automatically opens for text input. In layout preview,
left mouse substitutes for trigger and right mouse substitutes for grip. The
preview image captures mouse gestures, so dragging cannot move its desktop
monitor window. Title/tab drags move panels spatially while retaining native
docking; right-drag translates a whole panel without rotating it. Clicks owned by
UI panels or open popups do not pick scene objects behind them. Object picking
shares the desktop editor's mesh/primitive hit testing and updates the Hierarchy
and Inspector selection.

## Controller visuals

`Resources/VR-Engine-Root/ControllerMaterials/*.controller` configure Valve Index
and Touch Plus meshes, texture maps, PBR values and grip-relative alignment.
Touch Plus model units are already meters. A small studio-light shader keeps
controllers visible even in an empty project selector. The right Index OBJ's
inverted/inconsistent normals were repaired in the resource and VR Thingy asset;
positions, UVs and faces are preserved. Controller meshes also render with proper
depth testing and back-face culling, so rear faces cannot paint over the front
shell. The original project OBJ is backed up in
`Library/LocomotionRegression/valve_controller_knu_1_0_right.before-normal-fix.obj`.

## Verification

Build editor and runtime separately:

```powershell
cmake --build build --config Release --target Modularity --parallel 1 -- /p:UseMultiToolTask=true /p:CL_MPCount=2 /p:EnforceProcessCountAcrossBuilds=true
cmake --build build --config Release --target core_player --parallel 1 -- /p:UseMultiToolTask=true /p:CL_MPCount=2 /p:EnforceProcessCountAcrossBuilds=true
python tools/vr-editor/check_assets.py
python tools/vr-editor/build_probe.py
```

Run `build/release/VREditorProbe.exe` from `build/release`. It uses a hidden real
OpenGL context to verify controller pointer clicks, physical panel dragging,
native tab docking/tearing, focus release, root hit testing, return gestures,
joystick tool selection, hand-strip movement, navigation direction/speed,
head-centered turning, controller triangle-order independence, legacy stretched
panel recovery, visible scene geometry in the preview, asset imports and GL errors.
The preview interaction fixture also drives actual desktop mouse events through
the monitor image, checking UI clicks, title dragging, right-drag movement,
scene-object picking, empty-space deselection and click ownership.
It writes `build/vr-editor-check/workspace.ppm` and
`build/vr-editor-check/scene-preview.ppm` for visual inspection.

`MODULARITY_VR_EDITOR_PREVIEW=1` enables preview before XR initialization,
including when `--vr-editor` is also supplied. Hardware testing is still needed
for controller alignment, comfort and interaction feel on the headset.

The Index material configs recenter the source mesh on the grip handle using
`offset=0,0,-0.11` (meters), independently of controller tracking and aim rays.
Hardware alignment still needs checking on the actual Index.

The crash log tee forwards text blocks instead of writing stderr one character
at a time. This prevents missing-asset warnings from stalling VR editor frames.
The spatial probe also checks pitched movement, scene hover/selection outlines,
controller gizmo dragging, focus cancellation, draw-only stereo reprojection and
a resized nested popup's placement and aspect ratio.
Sustained gizmo drags are checked after interacting with a floating panel,
including translation-axis constraints, trigger release and Index grip pressure
while the trigger remains held.
Binding checks verify both Index editor force paths, unchanged gameplay and
non-Index bindings, and the default session policy. Mirror checks use separate
eye targets to verify copy contents, framebuffer restoration and desktop isolation.
