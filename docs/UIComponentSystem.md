# UI component expansion

The expansion extends `UIElementComponent`, its scene keys and the existing ImGui viewport renderer. `Canvas`, `UIImage`, `UISlider`, `UIButton`, `UIText` and `Sprite2D` keep their scene IDs (0–6 including None). New types are appended. Scene keys are optional, so existing scenes retain their defaults. Native scripts must rebuild for ABI 59 because scene-component memory layout changed.

## Controls and creation

The UI creation menu separates Rendering, Controls, Layout, Collections, Menus and Navigation, Editor Controls, and Templates. New UI objects use the existing UI component and hierarchy; the main scene-object type remains `UIImage` for extended controls. The inspector identifies the actual control type and shows only that control's fields, in Content, Items, Appearance, Behaviour and Layout sections. Links to other objects (scrollbar to scroll view, search field to list, dropdown to its popup list, property grid target) use the editor's drag-drop reference slot. Items can be reordered by dragging their handle; moves remap tree parents, expansion and selection.

Implemented controls:

- Panel, editable input/search fields, checkbox/toggle, radio group, progress bar, two-handle range editing, separator, hover tooltip.
- Scroll viewport and linked scrollbars; list, grid, multi-column list, hierarchy tree and virtualized log views.
- Native dropdown/enum popup, scene-owned dropdown template, nested menus/context menus, tabs, foldout, toolbar, breadcrumb and modal dialog.
- Color picker with RGB/HSV/HEX and alpha editing; scalar and vector drag fields; reorderable list; scene-object reference field; basic property grid; file/folder browser; adjustable split container; dockable panel container backed by ordinary Canvas children.

Text editing uses ImGui caret, selection, clipboard, Unicode entry and password handling. Multiline submission uses Ctrl+Enter; single-line submission uses Enter. Disabled fields reject edits. Panels can use the existing texture asset field.

### Appearance

Every control resolves one palette from its component (`UIControls::ResolvePalette`). **Tint** (`color`) is the accent of interactive controls and the fill of panels; the Accent, Background, Border and Text overrides derive from it while their alpha is 0, so recoloring the tint re-themes the whole control. Hover, pressed, selected, tab, header, table and scrollbar states are all derived from those four colors. Accents that would vanish against their track (for example the dark slate tint saved by early builds) are lifted until they read. Custom chrome goes through ModuGUI's `ShadeRect`, so the controls pick up the editor shade theme.

New controls (creation menu, Add Component, templates) start from `ApplyCreationDefaults`: the editor's violet accent on dark frames, sensible sizes, and starter items for collections, tabs, toolbars, menus and radio groups so a new control is visible and usable immediately. Single-line frames (inputs, dropdowns, numeric/vector fields) fill their rect height instead of sitting at its top.

Custom-drawn controls: a single-track range slider with two handles (arrows move the lower handle, Shift+arrows the upper, Ctrl for x10); a progress bar with centered label or percentage and an animated indeterminate sweep; a proportional scrollbar (drag the thumb, click the track to page, wheel, arrows); a foldout header with chevron; a segmented toolbar; a breadcrumb with chevrons that collapses leading segments into "..."; a menu bar whose open menu follows the hovered title; a split handle with a resize cursor; search fields with a magnifier and clear button; X/Y/Z/W tags on vector editors; a searchable object picker; a two-column property grid; a file browser with folder/file icons, folders listed first. Log lines containing `warn`/`error` near their start are tinted. A multi-column list takes its column titles from the label (`Name|Type|Value`).

### Editing versus playing

In the Scene view and a stopped Game view the controls are an inert preview, exactly like the legacy Button and Slider: they render at full opacity but take no clicks, so selecting and moving them works. A modal shows where its dialog will appear, and a tooltip area is outlined. They become live while playing, and in the standalone player and 3D canvases.

The property grid edits a linked object's name, enabled state and transform/UI fields. It does not replace the full editor inspector or automatically expose arbitrary script properties. The file browser reads directories on explicit navigation/refresh, reports filesystem errors and supports a folders-only option. It does not read or execute file contents.

## Layout

Layout is independent of the visual type and can be attached to an existing UI element in its inspector. Horizontal, vertical and grid arrangement can nest. Spacing, padding/margins (left/top/right/bottom), minimum/preferred/maximum size, flexible weights, content fitting, aspect ratio and normalized responsive anchors are opt-in.

Measurement proceeds from descendants to ancestors. Arrangement resolves against the actual parent rectangle, including resolution changes. Authored `ui.position`/`ui.size` values are preserved. Negative preferred sizes inherit authored size. A positive aspect ratio derives height from width. Flexible weights distribute remaining main-axis space. Disabled direct children do not consume layout space.

Responsive anchors use normalized minimum/maximum points. An axis with separated anchors stretches: authored size becomes its additive size delta, and position offsets the pivot within that region. An axis with coincident anchors retains authored size. Grid columns are explicit; row height follows the largest measured cell.

Legacy branches retain their original transform resolver. New branches also resolve in the existing world-UI path. Arbitrarily rotated layouts and rotated interactive hit regions still need dedicated validation; no transformed-stencil implementation is supplied.

## Clipping and scrolling

`clipChildren` attaches rectangular clipping to existing images, panels or other UI containers. Canvas `maskChildren` keeps its original meaning. Nested masks intersect, including empty intersections: an empty intersection hides descendants instead of disabling clipping. The same ImGui clipping scope restricts widget hit testing.

The scroll template creates a root, viewport, content container, and linked vertical/horizontal scrollbars. The viewport measures direct content and clamps its offsets. Content moves through layout resolution, without changing authored positions. Lists/grids/logs use ImGui coarse item virtualization; filtering retains source indices and scratch buffers retain their allocations. Tree adjacency is built in linear time, with persistent expansion indices. Tree virtualization and tree drag reparenting are not implemented. Reorderable lists support drag/drop within the same list. Multi-column rows use `|` between column values.

## Composite templates

- Scroll View with Content and Scrollbars.
- Dropdown with Popup, Viewport and Items. The trigger and list share the root's item data and selection, through scene ID references; changing items through scripting updates the dropdown. Popup descendants hide when closed.
- Tab View with Pages. Direct children are pages in scene hierarchy order; the selected tab controls visibility without overwriting enabled flags.
- Searchable List. The search field references its result list.
- Resizable Split with Two Panels. The panes fill the split view on either side of a 6px drag handle.
- Hierarchy Tree with hierarchical example data.
- Dock Container with Canvas Panels. Each panel uses the existing offscreen canvas target and input context. Panels can dock, tab, float, close and reopen. Native dock arrangements currently persist only for the running session; panel hierarchy/settings are scene-serialized.

Duplicating a hierarchy remaps `linkedObjectId` when both objects were copied. External references remain external. Runtime focus/event pulses, search filter and scratch buffers are not saved; persistent input, selection, expansion, scroll position, values and visual/layout settings are saved. Item labels are length-prefixed inside the existing scene text encoding, preserving empty labels, Unicode and line breaks.

## ModuCPP

Script-facing access uses `.` as usual:

```cpp
obj.UI.Toggle.Value = true;
obj.UI.Toggle.OnChanged([](bool value) { /* ... */ });
obj.UI.Input.Text = "Search";
obj.UI.Input.Placeholder = "Type here...";
obj.UI.Input.OnChanged([](const std::string& text) { /* ... */ });
obj.UI.Input.OnSubmit([](const std::string& text) { /* ... */ });
obj.UI.AddItem("Root");
obj.UI.AddItem("Child", 0);
obj.UI.SetSelectedIndex(1);
obj.UI.OnSelectionChanged([](int index) { /* ... */ });
obj.UI.OnChanged([]() { /* control value changed */ });
obj.UI.Focus();
```

Other helpers include `ClearItems`, `GetSelectedItem`, `SetFilter`, `SetOpen`, `SetLinkedObject`, `SetProgress`, `SetRange`/`GetRange`, and `SetNumericValue`/`GetNumericValue`. Event revisions prevent repeated submit/change callbacks simply because a script polls the same control more than once. Existing runtime object creation/destruction APIs still apply to these scene components.

## Validation and remaining scope

Run `powershell -ExecutionPolicy Bypass -File tools/Test-UI.ps1` after configuring/building the repository. `tools/Render-UIGallery.ps1` renders every control with the editor theme to `build/ui-gallery.png` (`-Edit` for the edit-time preview), for reviewing visual changes without launching the editor. The headless checks use the repository's real ImGui library. They cover stable legacy IDs; creation defaults and palette contrast; split-view panes; item moves; collection payload round trips and malformed payloads; layout/fitting/responsiveness; collapsed content; every control's draw scopes; mouse activation; disabled/clipped hit testing; focus/text entry/submission; range bounds and range-slider keyboard control; inert edit-time previews; a 100,000-row list with bounded emitted geometry; public script API compilation; and offscreen keyboard delivery/release. Build both `Modularity` and `ModularityPlayer` for integration validation.

This is an incremental expansion, not complete Unity parity. Remaining work includes scene persistence for native dock arrangements, image/stencil masks, tree virtualization/reparenting, arbitrary property reflection, explicit navigation-order editing, full drag/drop coverage, per-state sprites, and undo-aware property-grid integration. Gamepad and offscreen keyboard routing require interactive platform validation. Large-scale collection performance and visual regression testing across resolutions also remain to be measured. The existing ImGui docking backend also serves runtime dock containers. Offscreen texture composition and drag/drop docking still need visual/platform validation.
