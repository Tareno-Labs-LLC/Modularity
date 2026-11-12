"""Link and run asymmetric multiview checks against the Release engine."""
from pathlib import Path
import os
import re
import sys
import subprocess
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[2]
out = repo / "build/stereo-selftest"
out.mkdir(parents=True, exist_ok=True)
player = "--player" in sys.argv
core = "core_player" if player else "core"
app = "ModularityPlayer" if player else "Modularity"
ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
tree = ET.parse(repo / f"build/Dev-Resources/{core}.vcxproj").getroot()
group = next(g for g in tree.findall("m:ItemDefinitionGroup", ns) if "release|x64" in g.attrib.get("Condition", "").lower())
compiler = group.find("m:ClCompile", ns)
def values(name):
    return [v for v in compiler.find("m:" + name, ns).text.split(";") if v and not v.startswith("%(")]
flags = "/nologo /std:c++23preview /EHsc /MD /O2 /Gy "
flags += " ".join('/D"' + v.replace('"', '\\"') + '"' for v in values("PreprocessorDefinitions"))
flags += " " + " ".join(f'/I"{v}"' for v in values("AdditionalIncludeDirectories"))
sources = [Path(__file__).with_name("render_test.cpp")]
objects = [out / "render_test.obj"]
compile_rsp = out / "graphics-compile.rsp"
compile_rsp.write_text(flags + " /c " + " ".join(f'"{p}"' for p in sources) + f' /Fo:"{out.as_posix()}/"')
line = (repo / f"build/Dev-Resources/{app}.dir/Release/{app}.tlog/link.command.1.tlog").read_text(encoding="utf-16").splitlines()[1]
exe = out / ("stereo-player-test.exe" if player else "stereo-test.exe")
line = re.sub(r'/OUT:"[^"]+"', lambda _: f'/OUT:"{exe}"', line, flags=re.I)
for arg in ("PDB", "IMPLIB", "DEF"):
    line = re.sub('/' + arg + r':"[^"]+"', '', line, flags=re.I)
line = re.sub(r'/WHOLEARCHIVE:(?:"[^"]+"|\S+)', '', line, flags=re.I)
line = re.sub(r'/DEBUG(?::\S+)?', '', line, flags=re.I)
line = line.replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE')
line = re.sub(re.escape(app) + r'\.DIR\\RELEASE\\MAIN(?:_PLAYER)?\.OBJ', lambda _: " ".join(f'"{p}"' for p in objects), line, flags=re.I)
assert re.search(r'render_test\.obj', line, re.I), "Test entry point was not substituted"
assert not re.search(r'\bMAIN(?:_PLAYER)?\.OBJ', line, re.I), "Application entry point remains in test"
link_rsp = out / "graphics-link.rsp"
link_rsp.write_text(line)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
os.environ["PATH"] = str(vswhere.parent) + os.pathsep + os.environ.get("PATH", "")
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
subprocess.run(f'call "{vcvars}" >nul && cl @"{compile_rsp}" && link @"{link_rsp}" /LTCG', shell=True, cwd=repo / "build/Dev-Resources", check=True)
# Use the built editor's DLL search paths without copying any runtime files.
env = os.environ.copy()
env["PATH"] = str(repo / "build/Dev-ExportedBuild") + os.pathsep + env.get("PATH", "")
subprocess.run([str(exe), str(out)] + [v for v in sys.argv[1:] if v != "--player"], cwd=repo, env=env, check=True, timeout=180)

