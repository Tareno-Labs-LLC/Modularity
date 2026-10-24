"""Run network regressions with the changed session and existing engine libraries."""
from pathlib import Path
import os
import re
import subprocess
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[2]
out = repo / "build/engine-audit-check"
out.mkdir(parents=True, exist_ok=True)
ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
tree = ET.parse(repo / "build/core.vcxproj").getroot()
group = next(g for g in tree.findall("m:ItemDefinitionGroup", ns) if "release|x64" in g.attrib.get("Condition", "").lower())
compiler = group.find("m:ClCompile", ns)
def values(name):
    return [v for v in compiler.find("m:" + name, ns).text.split(";") if v and not v.startswith("%(")]
flags = "/nologo /std:c++23preview /EHsc /MD /O2 /Gy "
flags += " ".join('/D"' + v.replace('"', '\\"') + '"' for v in values("PreprocessorDefinitions"))
flags += " " + " ".join(f'/I"{v}"' for v in values("AdditionalIncludeDirectories"))
sources = [repo / "tools/engine-audit-selftest/graphics_test.cpp", repo / "src/WinView/Window.cpp"]
objects = [out / "graphics_test.obj", out / "Window.obj"]
compile_rsp = out / "graphics-compile.rsp"
compile_rsp.write_text(flags + " /c " + " ".join(f'"{p}"' for p in sources) + f' /Fo:"{out.as_posix()}/"')
line = (repo / "build/Modularity.dir/Release/Modularity.tlog/link.command.1.tlog").read_text(encoding="utf-16").splitlines()[1]
exe = out / "graphics-audit.exe"
line = re.sub(r'/OUT:"[^"]+"', lambda _: f'/OUT:"{exe}"', line, flags=re.I)
for arg in ("PDB", "IMPLIB", "DEF"):
    line = re.sub('/' + arg + r':"[^"]+"', '', line, flags=re.I)
line = re.sub(r'/WHOLEARCHIVE:(?:"[^"]+"|\S+)', '', line, flags=re.I)
line = re.sub(r'/DEBUG(?::\S+)?', '', line, flags=re.I)
line = line.replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE')
line = re.sub(r'MODULARITY\.DIR\\RELEASE\\MAIN\.OBJ', lambda _: " ".join(f'"{p}"' for p in objects), line, flags=re.I)
link_rsp = out / "graphics-link.rsp"
link_rsp.write_text(line)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
subprocess.run(f'call "{vcvars}" >nul && cl @"{compile_rsp}" && link @"{link_rsp}" /LTCG', shell=True, cwd=repo / "build", check=True)
# Use the built editor's DLL search paths without copying any runtime files.
env = os.environ.copy()
env["PATH"] = str(repo / "build/release") + os.pathsep + env.get("PATH", "")
subprocess.run([str(exe)], cwd=repo / "build/release", env=env, check=True, timeout=30)
