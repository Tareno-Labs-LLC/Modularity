"""Rebuild configured engine units directly when MSBuild file tracking is unavailable.

Keeps the configured Release definitions/includes and existing dependency libraries.
Builds separate archives/mains and audit executables by default. Explicit --install
backs up and relinks the normal executables and stages resources/SDK. Generated
project files are unchanged. Default compilation makes no dependency guesses.
"""
from pathlib import Path
import os
import sys
import shutil
import time
import re
import subprocess
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[2]
out = repo / "build/engine-audit-check/full-build"
out.mkdir(parents=True, exist_ok=True)
ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"

def project(name):
    root = ET.parse(repo / f"build/{name}.vcxproj").getroot()
    group = next(g for g in root.findall("m:ItemDefinitionGroup", ns) if "release|x64" in g.get("Condition", "").lower())
    compiler = group.find("m:ClCompile", ns)
    def values(name):
        return [v for v in compiler.find("m:" + name, ns).text.split(";") if v and not v.startswith("%(")]
    flags = "/nologo /std:c++23preview /EHsc /MD /O2 /Ob2 /Z7 /Gy /permissive- /MP4 /external:W0 "
    flags += " ".join('/D"' + v.replace('"', '\\"') + '"' for v in values("PreprocessorDefinitions"))
    flags += " " + " ".join(f'/I"{v}"' for v in values("AdditionalIncludeDirectories"))
    additional = compiler.find("m:AdditionalOptions", ns)
    if additional is not None: flags += " " + additional.text.replace("%(AdditionalOptions)", "")
    sources = [Path(i.get("Include")) for i in root.findall(".//m:ClCompile", ns) if i.get("Include")]
    assert len({p.stem.lower() for p in sources}) == len(sources), "Object basenames must be unique"
    return flags, sources

def run(command):
    subprocess.run(f'call "{vcvars}" >nul && {command}', shell=True, cwd=repo / "build", check=True)

install = "--install" in sys.argv
backup = out / ("previous-build-" + time.strftime("%Y%m%d-%H%M%S"))
if install:
    backup.mkdir(parents=True)
    print(f"Previous binary backup: {backup}", flush=True)
for core, app in [("core", "Modularity"), ("core_player", "ModularityPlayer")]:
    objdir = out / core
    objdir.mkdir(exist_ok=True)
    flags, sources = project(core)
    rsp = out / f"{core}-compile.rsp"
    # Explicit recovery for a failed invocation whose headers/configuration are
    # unchanged. The default always recompiles every configured engine source.
    pending = sources
    if "--resume" in sys.argv:
        pending = [p for p in sources if not (objdir / (p.stem + ".obj")).exists() or
                   p.stat().st_mtime > (objdir / (p.stem + ".obj")).stat().st_mtime]
    rsp.write_text(flags + " /c " + " ".join(f'"{p}"' for p in pending) + f' /Fo:"{objdir.as_posix()}/"')
    print(f"Compiling all {len(sources)} {core} units", flush=True)
    if pending: run(f'cl @"{rsp}"')
    archive = out / f"{core}.lib"
    rsp = out / f"{core}-archive.rsp"
    rsp.write_text(f'/nologo /OUT:"{archive}" ' + " ".join(f'"{objdir / (p.stem + ".obj")}"' for p in sources))
    run(f'lib @"{rsp}"')
    mainflags, mains = project(app)
    rsp = out / f"{app}-main.rsp"
    rsp.write_text(mainflags + " /c " + " ".join(f'"{p}"' for p in mains) + f' /Fo:"{objdir.as_posix()}/"')
    run(f'cl @"{rsp}"')
    command = (repo / f"build/{app}.dir/Release/{app}.tlog/link.command.1.tlog").read_text(encoding="utf-16").splitlines()[1]
    command = re.sub(r'RELEASE\\' + core + r'\.LIB', lambda _: f'"{archive}"', command, flags=re.I)
    command = re.sub(r'/WHOLEARCHIVE:(?:"[^"\r\n]*[/\\]' + core + r'\.lib"|[^ \r\n]*[/\\]' + core + r'\.lib)',
                     lambda _: f'/WHOLEARCHIVE:"{archive}"', command, flags=re.I)
    # Preserve resources/SDK export lists and dependency libraries from CMake.
    exe = repo / f"build/release/{app + ('' if install else '-audit')}.exe"
    if install:
        for extension in (".exe", ".pdb", ".lib", ".exp"):
            previous = exe.with_suffix(extension)
            if previous.exists(): shutil.copy2(previous, backup / previous.name)
        previousArchive = repo / f"build/release/{core}.lib"
        if previousArchive.exists(): shutil.copy2(previousArchive, backup / previousArchive.name)
    command = re.sub(r'/OUT:"[^"]+"', lambda _: f'/OUT:"{exe}"', command, flags=re.I)
    for option in ("PDB", "IMPLIB"):
        command = re.sub('/' + option + r':"[^"]+"', '', command, flags=re.I)
    for main in mains:
        command = re.sub(app + r'\.DIR\\RELEASE\\' + re.escape(main.stem) + r'\.OBJ',
                         lambda _: f'"{objdir / (main.stem + ".obj")}"', command, flags=re.I)
    command += f' /PDB:"{exe.with_suffix(".pdb")}" /IMPLIB:"{exe.with_suffix(".lib")}"'
    rsp = out / f"{app}-link.rsp"
    rsp.write_text(command)
    run(f'link @"{rsp}"')
    print(f"Built {exe}", flush=True)
    if install: shutil.copy2(archive, previousArchive)
if install:
    subprocess.run(["cmake", "-E", "copy_directory", str(repo / "Resources"), str(repo / "build/release/Resources")], check=True)
    subprocess.run(["cmake", "-E", "copy_directory", str(repo / "docs"), str(repo / "build/release/docs")], check=True)
    subprocess.run(["cmake", f"-DSRC_ROOT={repo}", f"-DBUILD_ROOT={repo / 'build'}",
                    f"-DDST_ROOT={repo / 'build/release/Resources/ScriptSDK'}", "-P", str(repo / "cmake/CopyScriptSDK.cmake")], check=True)
    print("Staged matching resources, documentation, and native script SDK.", flush=True)
