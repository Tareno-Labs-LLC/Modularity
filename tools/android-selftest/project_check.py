"""Load Tenna's actual exported CRLF project and scene through the production readers."""
from pathlib import Path
import argparse
import io
import os
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('apk', type=Path)
parser.add_argument('--before', action='store_true', help='Reproduce the old Unix CRLF parsing failure')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
build = repo / 'build/Dev-Resources'
out = build / 'android-selftest'
out.mkdir(parents=True, exist_ok=True)
fixture = Path(tempfile.mkdtemp(prefix='project-', dir=out))
with zipfile.ZipFile(args.apk) as apk:
    stream = io.BytesIO(apk.read('assets/content.modbundle'))
    assert stream.read(11) == b'MODRUNTIME1'
    version, count = struct.unpack('<II', stream.read(8))
    assert version == 1
    for _ in range(count):
        length, size = struct.unpack('<IQ', stream.read(12))
        name = stream.read(length).decode()
        data = stream.read(size)
        if name in ('project.modu', 'scripts.modu', 'Assets/Scenes/Main.scene'):
            path = fixture / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
assert b'pipeline=2D\r\n' in (fixture / 'project.modu').read_bytes()

ns = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
def release_group(path):
    tree = ET.parse(path)
    return next(g for g in tree.findall('m:ItemDefinitionGroup', ns) if "'release|x64'" in g.attrib.get('Condition', ''))
core = release_group(build / 'core.vcxproj')
exe_group = release_group(build / 'Modularity.vcxproj')
includes = core.find('m:ClCompile/m:AdditionalIncludeDirectories', ns).text.split(';')
defines = core.find('m:ClCompile/m:PreprocessorDefinitions', ns).text.split(';')
libs = exe_group.find('m:Link/m:AdditionalDependencies', ns).text.split(';')
project_source = repo / 'src/ProjectManager.cpp'
if args.before:
    # Binary mode models Android's ifstream behavior on Windows, where the CRT
    # would otherwise hide the bug by removing CR before std::getline sees it.
    original = project_source.read_text().replace("            if (!line.empty() && line.back() == '\\r') line.pop_back();\n", '')
    project_source = fixture / 'ProjectManager-before.cpp'
    project_source.write_text(original)
vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
vcvars = Path(vs) / 'VC/Auxiliary/Build/vcvars64.bat'
exe = fixture / 'project-test.exe'
rsp = fixture / 'project-test.rsp'
rsp.write_text('/nologo /std:c++23preview /EHsc /MD /O1 /Gy /Gw ' +
    ' '.join(f'/I"{p}"' for p in includes if p and '%' not in p) + ' ' +
    ' '.join(f'/D"{p}"' for p in defines if p and '%' not in p and '$' not in p) + ' ' +
    f'"{repo / "tools/android-selftest/project_test.cpp"}" "{project_source}" '
    f'/Fo:"{fixture.as_posix()}/" /Fe:"{exe}" /link /OPT:REF /INCREMENTAL:NO ' +
    ' '.join(f'"{p}"' for p in libs if p and '%' not in p))
subprocess.run(f'call "{vcvars}" >nul && cl @"{rsp}"', shell=True, cwd=build, check=True)
env = dict(os.environ)
env['PATH'] = str(repo / 'build/Dev-ExportedBuild') + os.pathsep + env.get('PATH', '')
result = subprocess.run([str(exe), str(fixture)], cwd=repo, env=env)
if args.before:
    assert result.returncode == 1, f'Expected old-reader regression, got {result.returncode}'
    print('PASS: reproduced the old Android CRLF parsing failure against the exported Tenna project')
else:
    result.check_returncode()
