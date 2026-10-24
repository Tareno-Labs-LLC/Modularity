"""Run network regressions with the changed session and existing engine libraries."""
from pathlib import Path
import os
import sys
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
flags = "/nologo /std:c++23preview /EHsc /MD /Od /Gy "
flags += " ".join('/D"' + v.replace('"', '\\"') + '"' for v in values("PreprocessorDefinitions"))
flags += " " + " ".join(f'/I"{v}"' for v in values("AdditionalIncludeDirectories"))
sources = [repo / p for p in ["tools/engine-audit-selftest/engine_test.cpp", "src/Engine.cpp", "src/ScriptRuntime.cpp", "src/ManagedScriptRuntime.cpp", "src/WinView/Window.cpp", "src/EditorWindows/Panels/SceneHierarchyWindow.cpp", "src/Network/NetworkSession.cpp", "src/Network/LoopbackBackend.cpp"]]
objects = [out / (p.stem + ".obj") for p in sources]
compile_rsp = out / "engine-compile.rsp"
if "--full-build" in sys.argv:
    sources = sources[:1]
    objects = objects[:1]
compile_sources = sources[:1] if "--reuse-objects" in sys.argv else sources
compile_rsp.write_text(flags + " /c " + " ".join(f'"{p}"' for p in compile_sources) + f' /Fo:"{out.as_posix()}/"')
line = (repo / "build/Modularity.dir/Release/Modularity.tlog/link.command.1.tlog").read_text(encoding="utf-16").splitlines()[1]
exe = out / "engine-audit.exe"
line = re.sub(r'/OUT:"[^"]+"', lambda _: f'/OUT:"{exe}"', line, flags=re.I)
for arg in ("PDB", "IMPLIB", "DEF"):
    line = re.sub('/' + arg + r':"[^"]+"', '', line, flags=re.I)
line = re.sub(r'/WHOLEARCHIVE:(?:"[^"]+"|\S+)', '', line, flags=re.I)
line = re.sub(r'/DEBUG(?::\S+)?', '', line, flags=re.I)
line = line.replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE')
line = re.sub(r'MODULARITY\.DIR\\RELEASE\\MAIN\.OBJ', lambda _: " ".join(f'"{p}"' for p in objects), line, flags=re.I)
link_rsp = out / "engine-link.rsp"
line += f' /IMPLIB:"{out / "engine-audit.lib"}"'
if "--full-build" in sys.argv:
    line = re.sub(r'RELEASE\\CORE\.LIB', lambda _: f'"{out / "full-build/core.lib"}"', line, flags=re.I)
link_rsp.write_text(line)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
subprocess.run(f'call "{vcvars}" >nul && cl @"{compile_rsp}" && link @"{link_rsp}" /LTCG', shell=True, cwd=repo / "build", check=True)
# Use the built editor's DLL search paths without copying any runtime files.
env = os.environ.copy()
env["PATH"] = str(repo / "build/release") + os.pathsep + env.get("PATH", "")

fixture = out / "script-fixture.dll"
fixture_rsp = out / "script-fixture.rsp"
fixture_rsp.write_text(flags.replace('/D"MODULARITY_SCRIPT_HOST"', '') +
    f' /DMODULARITY_SCRIPT_IMPORTS /LD "{repo / "tools/engine-audit-selftest/script_fixture.cpp"}" /Fo:"{out / "script_fixture.obj"}" /Fe:"{fixture}" /link "{out / "engine-audit.lib"}"')
subprocess.run(f'call "{vcvars}" >nul && cl @"{fixture_rsp}"', shell=True, cwd=out, check=True)
import http.server
import threading
class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.send_response(500 if self.path == "/error" else 204 if self.path == "/empty" else 200)
        body = b"failed cancelled HTTP 500" if self.path != "/empty" else b""
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
import sys
(out / "child-test.py").write_text('from pathlib import Path\nimport time\nPath("child-ready.txt").write_text("ready")\ntime.sleep(2)\nPath("child-completed.txt").write_text("unexpected")\n')
(out / "child-test.cmd").write_text(f'@echo off\n"{sys.executable}" "{out / "child-test.py"}"\n')
(out / "child-ready.txt").unlink(missing_ok=True)
try:
    subprocess.run([str(exe), f"http://127.0.0.1:{server.server_port}", str(fixture)], cwd=out, env=env, check=True, timeout=90)
finally:
    server.shutdown()
