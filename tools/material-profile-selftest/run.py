"""Compile the project shader-profile and native material persistence checks."""
from pathlib import Path
import os
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
out = repo / "build/material-profile-selftest"
out.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
os.environ["PATH"] = str(vswhere.parent) + os.pathsep + os.environ.get("PATH", "")
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
sources = ["tools/material-profile-selftest/profile_test.cpp", "src/UnityAssetImport.cpp", "src/EngineMaterialIO.cpp", "src/Platform/AssetSourceDesktop.cpp", "src/DurableFile.cpp"]
flags = ["/nologo", "/std:c++20", "/EHsc", "/MD", "/O1", "/DNOMINMAX", "/DMODULARITY_HAS_VULKAN=0"]
flags += [f'/I"{repo / p}"' for p in ["src", "include", "src/ThirdParty/glad", "src/ThirdParty/ModuGUI"]]
flags += [f'"{repo / p}"' for p in sources]
exe = out / "profile-test.exe"
flags += [f'/Fo:"{out.as_posix()}/"', f'/Fe:"{exe}"']
rsp = out / "import.rsp"
rsp.write_text(" ".join(flags))
subprocess.run(f'call "{vcvars}" >nul && cl @"{rsp}"', shell=True, cwd=out, check=True)
subprocess.run([str(exe), str(out / "fixtures"), *sys.argv[1:]], cwd=repo, check=True)
