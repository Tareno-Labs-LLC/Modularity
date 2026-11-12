"""Verify shader polling cannot wait on slow filesystem I/O (no GPU required)."""
from pathlib import Path
import os
import subprocess

repo = Path(__file__).resolve().parents[2]
out = repo / "build/render-stall-selftest"
out.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
os.environ["PATH"] = str(vswhere.parent) + os.pathsep + os.environ.get("PATH", "")
source = Path(__file__).with_name("watcher_test.cpp")
exe = out / "watcher-test.exe"
subprocess.run(f'call "{vcvars}" >nul && cl /nologo /std:c++17 /EHsc /MD /O2 "{source}" /Fo:"{out}/watcher-test.obj" /Fe:"{exe}"', shell=True, cwd=out, check=True)
subprocess.run([str(exe)], check=True, timeout=10)
