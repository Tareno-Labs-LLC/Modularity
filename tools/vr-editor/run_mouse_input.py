"""Test engine-side VR input against freshly compiled, pinned ModuGUI sources."""
from pathlib import Path
import os
import subprocess

repo = Path(__file__).resolve().parents[2]
out = repo / "build/vr-mouse-input-check"
pristine = out / "src/ThirdParty/ModuGUI"
pristine.mkdir(parents=True, exist_ok=True)
# Read HEAD rather than the working submodule: an uncommitted declaration or
# a prebuilt library must not hide a dependency that a fresh clone cannot link.
files = ["imgui.cpp", "imgui_draw.cpp", "imgui_shade.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
         "imgui.h", "imgui_internal.h", "imconfig.h", "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h"]
for name in files:
    (pristine / name).write_bytes(subprocess.check_output(
        ["git", "-C", str(repo / "src/ThirdParty/ModuGUI"), "show", "HEAD:" + name]))
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
sources = [repo / "tools/vr-editor/mouse_input_test.cpp"] + [pristine / n for n in files if n.endswith(".cpp")]
exe = out / "vr-mouse-input.exe"
rsp = out / "compile.rsp"
rsp.write_text('/nologo /std:c++20 /EHsc /MD /O2 /MP4 ' +
    f'/I"{out / "src"}" /I"{repo / "src"}" ' +
    " ".join(f'"{p}"' for p in sources) + f' /Fo:"{out.as_posix()}/" /Fe:"{exe}" /link user32.lib imm32.lib')
subprocess.run(f'call "{vcvars}" >nul && cl @"{rsp}"', shell=True, cwd=out, check=True)
subprocess.run([str(exe)], cwd=out, check=True)
