"""Syntax-check changed engine units with the configured editor/player defines."""
from pathlib import Path
import os
import subprocess
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[2]
out = repo / "build/engine-audit-check"
out.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
for target, sources in [
    ("core", ["src/Engine.cpp", "src/EditorWindows/Panels/SceneHierarchyWindow.cpp", "src/Network/NetworkSession.cpp", "src/Network/LoopbackBackend.cpp", "src/WinView/Window.cpp", "src/ScriptRuntime.cpp", "src/ManagedScriptRuntime.cpp", "tools/network-selftest/network_test.cpp"]),
    ("core_player", ["src/Engine.cpp", "src/ScriptRuntime.cpp", "src/ManagedScriptRuntime.cpp", "src/WinView/Window.cpp"]),
]:
    root = ET.parse(repo / f"build/{target}.vcxproj").getroot()
    group = next(g for g in root.findall("m:ItemDefinitionGroup", ns) if "release|x64" in g.attrib.get("Condition", "").lower())
    compiler = group.find("m:ClCompile", ns)
    def values(name):
        return [v for v in compiler.find("m:" + name, ns).text.split(";") if v and not v.startswith("%(")]
    flags = ["/nologo", "/std:c++23preview", "/EHsc", "/MD", "/Zs", "/FC"]
    flags += ['/D"' + v.replace('"', '\\"') + '"' for v in values("PreprocessorDefinitions")]
    flags += [f'/I"{v}"' for v in values("AdditionalIncludeDirectories")]
    flags += [f'"{repo / p}"' for p in sources]
    response = out / f"{target}-syntax.rsp"
    response.write_text(" ".join(flags))
    print(f"Checking {target}: {', '.join(sources)}", flush=True)
    subprocess.run(f'call "{vcvars}" >nul && cl @"{response}"', shell=True, cwd=repo, check=True)
