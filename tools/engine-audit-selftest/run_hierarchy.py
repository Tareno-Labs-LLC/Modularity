"""Compile and run the production hierarchy snapshot regressions, without GL."""
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
exe = out / "hierarchy-audit.exe"
rsp = out / "hierarchy.rsp"
ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
project = ET.parse(repo / "build/core.vcxproj").getroot()
include_dirs = project.find(".//m:AdditionalIncludeDirectories", ns).text.split(";")
includes = " ".join(f'/I"{p}"' for p in include_dirs if not p.startswith("%"))
rsp.write_text(f'/nologo /std:c++20 /EHsc /MD /O2 {includes} "{repo / "tools/engine-audit-selftest/hierarchy_test.cpp"}" /Fo:"{out / "hierarchy_test.obj"}" /Fe:"{exe}"')
subprocess.run(f'call "{vcvars}" >nul && cl @"{rsp}"', shell=True, cwd=out, check=True)
subprocess.run([str(exe)], cwd=out, check=True)
