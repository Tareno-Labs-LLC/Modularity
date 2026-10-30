"""Build/run the gizmo probe with the existing Windows Release libraries."""
from pathlib import Path
import os
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
build = repo / "build/Dev-Resources"
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
installation = subprocess.check_output(
    [str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
    text=True,
).strip()
vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
arguments = ["/nologo", "/std:c++20", "/EHsc", "/MD", "/O2"]
arguments += [f'/I"{repo / p}"' for p in ["src", "src/ThirdParty/ModuGUI", "src/ThirdParty/glad", "src/ThirdParty/glfw/include"]]
arguments += [f'"{repo / "tools/gizmo-selftest/probe.cpp"}"']
arguments += [f'"{build / p}"' for p in ["release/imguizmo.lib", "release/imgui.lib", "release/glad.lib", "src/ThirdParty/glfw/src/release/glfw3.lib"]]
arguments += [f'/Fe:"{build / "gizmo-probe.exe"}"', f'/Fo:"{build / "gizmo-probe.obj"}"', "/link", "opengl32.lib", "user32.lib", "imm32.lib", "gdi32.lib", "shell32.lib"]
use_gl = "--opengl" in sys.argv
executable = build / ("gizmo-probe-opengl.exe" if use_gl else "gizmo-probe-headless.exe")
arguments = [arg if not arg.startswith('/Fe:') else f'/Fe:"{executable}"' for arg in arguments]
response = build / "gizmo-probe.rsp"
response.write_text(" ".join(arguments))
subprocess.run(f'call "{vcvars}" >nul && cl @"{response}"', shell=True, cwd=repo, check=True)
subprocess.run([str(executable), str(build / "gizmo-preview.ppm")] + ([] if use_gl else ["--headless"]), cwd=repo, check=True)
