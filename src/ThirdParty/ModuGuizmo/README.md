# ModuGuizmo

Vendored from https://github.com/darkresident55/ModuGuizmo.git
Commit: 105e4fde45e4e74aae8973d98750820ef7aca7e2

The source files retain the upstream ImGuizmo API and filenames. The engine
keeps the `imguizmo` CMake target and package ID for existing integrations.

Local extensions in ImGuizmo.cpp / ImGuizmo.h:
- Spatial controller input (`SetInputRay`) and draw-only stereo projection (`DrawGizmo`).
- Balanced draw-list clipping when a gizmo is behind the camera.
- Slim cylindrical translate shafts and faceted cone tips, shaded with a fixed view-space
  side key/fill light, low ambient term, and a broad specular highlight. Each cone
  face has a constant normal so its light/dark sides remain visible at normal
  viewport sizes; shaft normals stay smooth. This uses ImGui
  vertex colors and is independent of scene lights, exposure, and materials.

When refreshing the snapshot, retain these extensions. The old
`ThirdParty/ImGuizmo/ImGuizmo.h` forwards here for script/SDK compatibility.
