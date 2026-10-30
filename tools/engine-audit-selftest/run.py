"""Compile the production mesh loader/validator and run audit regressions on Windows."""
from pathlib import Path
import os
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
out = repo / "build/Dev-Resources/engine-audit-check"
out.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
sources = [repo / "tools/engine-audit-selftest/mesh_asset_test.cpp", repo / "src/Render25D/MMesh.cpp", repo / "src/Render25D/MMeshLoader.cpp", repo / "tools/engine-audit-selftest/mesh_gpu_stubs.cpp"]
before = "--before" in sys.argv
if before:
    original = out / "MMesh-before.cpp"
    original.write_bytes(subprocess.check_output(["git", "show", "HEAD:src/Render25D/MMesh.cpp"], cwd=repo))
    sources[1] = original
exe = out / ("mesh-before.exe" if before else "mesh-audit.exe")
flags = ["/nologo", "/std:c++20", "/EHsc", "/MD", "/O2", "/Gy", "/Gw", "/DNOMINMAX", "/DMODULARITY_HAS_VULKAN=0"]
flags += [f'/I"{repo / p}"' for p in ["src", "include", "src/ThirdParty/glad", "src/ThirdParty/ModuGUI", "src/ThirdParty/assimp/include", "build/Dev-Resources/src/ThirdParty/assimp/include"]]
flags += [f'"{source}"' for source in sources]
flags += [f'/Fo:"{out.as_posix()}/"', f'/Fe:"{exe}"', "/link", "/OPT:REF", "/INCREMENTAL:NO"]
rsp = out / "mesh.rsp"
rsp.write_text(" ".join(flags))
subprocess.run(f'call "{vcvars}" >nul && cl @"{rsp}"', shell=True, cwd=out, check=True)
subprocess.run([str(exe), "--range-only" if before else str(out / "fixtures")], cwd=repo, check=True)
