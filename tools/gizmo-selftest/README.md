# ModuGuizmo rendering and interaction probe

On Windows with Visual Studio and Python, from the repository root:

```powershell
cmake --build build --config Release --target imguizmo glad
python tools/gizmo-selftest/build_probe.py
```

The probe checks projected handle geometry,
normal-based shading, identical colors against dark/bright backgrounds,
orthographic and universal tools, clip-stack balance behind the camera,
draw-only stereo state, and controller-ray dragging/release. It also compiles
through the legacy SDK header to check include compatibility.

The rendered comparison is written to `build/gizmo-preview.ppm` using a software
rasterizer over the production ImGui draw data. Add `--opengl` to the Python
command to validate with a hidden OpenGL window instead.
