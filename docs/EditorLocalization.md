# Editor Localization

## Overview
Modularity has two independent language settings. They are deliberately not tied together.

| Setting | Scope | Controls | Stored in |
| --- | --- | --- | --- |
| Editor Language | global, per user | the language the editor UI is displayed in | `launcher_settings.modu` |
| ModuCPP Code Language | per project, or the global default | the localized ModuCPP syntax scripts are authored in | `project.modu` (blank = follow global) |

"Editor Language: German, ModuCPP Code Language: English" and the reverse are both valid. Nothing in the system forces them to match.

Both live on the Language Manager page in Project Settings.

## Architecture
Every visible editor string is addressed by a stable internal key. The key is never the English text, so rewording an English label does not orphan every translation.

```text
WINDOW_INSPECTOR
MENU_ENGINE_SAVE_SCENE_AS
SETTINGS_TAB_LANGUAGE_MANAGER
COMPONENT_RIGIDBODY3D
COMPONENT_RIGIDBODY3D_MASS
```

Keys are `SCREAMING_SNAKE_CASE`, namespaced by prefix (`WINDOW_`, `MENU_`, `SETTINGS_`,
`COMPONENT_`, `COMMON_`, `DIALOG_`, `MODUPAK_`). Section and row descriptions use the
row's key plus `_DESC`.

Each language provides a dictionary mapping those keys to display strings:

```text
Resources/Languages/
    English/
        Editor.json     editor UI strings
        ModuCPP.json    ModuCPP syntax aliases
    German/
        Editor.json
        ModuCPP.json
```

Editor UI localization and ModuCPP syntax localization are separate files even inside the same language folder, because they are separate systems with separate lifetimes: one changes what you read, the other changes what compiles.

Adding a language is a new folder. No recompile, and no editor window has to be touched.

## Fallback Behavior
Lookup order for every key:

1. the selected editor language
2. English, which is the shipped `English/Editor.json` plus the built-in default compiled into the call site
3. the key itself, with a one-shot warning logged

A missing translation can never crash the editor and can never blank a label. Because every call site carries its English text as the last argument, a missing, empty or malformed `Editor.json` degrades to a fully working English editor rather than to a UI full of raw keys.

Keys that fell all the way through are listed in Language Manager > Editor Language > Missing Keys (Developer mode).

## Call Sites
```cpp
namespace Loc = Modularity::Loc;

// Plain display text.
ImGui::Button(Loc::T("COMMON_SAVE", "Save"));

// Window and popup titles. Returns "Inspektor###Inspector": display text in
// front, stable ImGui id behind, so docking layouts, focus calls and popup
// ids all keep working across a language switch.
ImGui::Begin(Loc::Window("WINDOW_INSPECTOR", "Inspector"), &showInspector);

// The id-only form, for DockBuilderDockWindow / SetWindowFocus / OpenPopup.
ImGui::DockBuilderDockWindow(Loc::WindowRef("Inspector"), dockRight);

// A widget whose label doubles as its ImGui id. Returns "<localized>###<key>",
// pinning the id to the key so it does not move when the language does.
ImGui::Checkbox(Loc::Widget("COMPONENT_UI_RENDER_IN_3_D", "Render In 3D"), &ui.renderIn3D);

// Inspector field label. The second argument is the *serialized* field name and
// is only ever an address for the lookup.
fieldRow(Loc::Field("COMPONENT_RIGIDBODY3D", "mass", "Mass"));
```

### Window ids and saved layouts
ImGui writes `Inspektor###Inspector` to the layout ini as `[Window][###Inspector]`. Layouts saved before localization used `[Window][Inspector]` and would otherwise undock every panel once. `loadWorkspaceIniWithLocalizedWindowIds` in `MainMenuBar.cpp` rewrites the old keys while loading. Saving is untouched, so the migration runs once and then becomes a no-op.

## Component Variable Display Names
Serialized field names are never renamed. `Loc::Field` resolves a display label only; the inspector keeps reading and writing the original field. The serialized name is folded into the key (`COMPONENT_RIGIDBODY3D` + `lockRotationX` -> `COMPONENT_RIGIDBODY3D_LOCK_ROTATION_X`).

```text
Internal field:      groundCheckDistance
English display:     Ground Check Distance
German display:      Bodenprüfdistanz
Serialized as:       groundCheckDistance      (unchanged, always)
```

Where no explicit translation exists, the label is generated from the serialized name using the editor's existing inspector formatting (`Loc::PrettyFieldName`): `groundCheckDistance` becomes `Ground Check Distance`, `max_hp_2` becomes `Max Hp 2`.

This is what keeps localized editor settings from touching scene data. Changing the editor language cannot modify a project.

## What Is Not Translated
Display text only. None of the following are ever touched:

- internal component ids
- serialization keys
- class names used internally
- file paths
- asset identifiers
- project data keys
- C++ symbols
- ModuCPP backend identifiers
- existing scene or project data

## Runtime Switching
Changing the editor language applies on the next frame. No restart, no project reload.

`Loc::T` and friends hand out pointers into tables that are rewritten in place rather than rebuilt, so text a window is already holding for the current frame updates instead of dangling. Menus, toolbars, window titles, inspector labels and Project Settings all pick the new language up immediately because they resolve their strings every frame.

Language files can be re-read without restarting via Language Manager > Editor Language > Reload Language Files. That reload is deferred to the top of the next frame, because it swaps whole registries and the settings page holds a `LanguagePack` reference while drawing.

## Language Detection and Defaults
- The editor defaults to English.
- On first launch, if the OS locale maps to an installed language, the editor asks once whether to switch. It never switches on its own.
- The answer is remembered globally (`osLanguagePromptAnswered`), so the prompt appears at most once.
- The global editor language never writes to project files.

The ISO 639-1 to folder mapping lives in `kIsoToLanguage` in `EditorLocalization.cpp`. A new language needs one row there only if OS detection should recognize it; the language itself works without it.

## Adding A Language
1. Copy `Resources/Languages/English/` to `Resources/Languages/<Name>/`.
2. Set `language.id`, `language.displayName` and `language.endonym` in both files.
3. Translate the values in `Editor.json`. Keys stay exactly as they are. Partial translations are fine: anything omitted falls back to English.
4. Fill in `aliases` in `ModuCPP.json` if the language should also have a localized code syntax. Leave it empty otherwise; the two are independent.
5. Restart, or use Reload Language Files.

See `docs/moducpp/manual/syntax-languages.md` for the `ModuCPP.json` alias format and the rules the lexer applies.

## Current Coverage
960 keys, fully translated into German. String conversion is applied to:

- all dockable editor window titles, and the main dialogs
- the main menu bar: Engine, Actions, Workflow (including every window toggle, workspace
  preset, UI scale, animation mode and feedback sound item) and Packages
- **every** Project Settings section header, row label and tooltip across all 15 pages,
  plus the page names, search box, visibility mode selector and project name row
- the whole Language Manager page
- ModuPAK Manager: the five view tabs and its action buttons
- the shared `COMMON_*` button vocabulary (Save, Cancel, Apply, Close, Create, Delete, ...)
- the ModuCPP translation dialogs (project translate, import translate, OS language prompt)
- **every** built-in component: all 31 inspector headers, their field rows and their
  labelled widgets (checkboxes, drags, sliders, combos, color pickers)
- the Add Component menu: all 13 categories and 50 entries, plus its search box and chrome

Window toggles in the Workflow menu deliberately reuse the same `WINDOW_*` keys as the
window titles, so a window is named once and renamed in one place.

### Component menu identity
`addEntry("Physics/Rigidbody 3D", ...)` keeps its path as the identifier: icons, ordering
and the action lookup all key off it. Only the drawn text is localized, through
`localizedCategory` / `localizedComponentName`, and the localization key is derived from
the *path*, not from the English display text. The Add Component search box matches both
the English path and the localized label, so searching works in either language.

Remaining editor strings still render their built-in English text and are converted by wrapping them in `Loc::T` with a new key, then adding that key to the language files. No structural work is needed for any of them.

ModuCPP *script* field labels are still baked into generated C++ at transpile time by `inspectorLabelFromFieldName`, so a script's own public fields are not localized yet. Routing them through `Loc::Field` needs a small transpiler change plus an engine-exported resolver.
