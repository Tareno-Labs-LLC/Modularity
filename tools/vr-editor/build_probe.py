from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[2]
build = repo / 'build/Dev-Resources/vr-editor-check'
build.mkdir(exist_ok=True)
ns = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
tree = ET.parse(repo / 'build/Dev-Resources/core.vcxproj').getroot()
group = next(g for g in tree.findall('m:ItemDefinitionGroup', ns) if 'release|x64' in g.attrib.get('Condition', '').lower())
compiler = group.find('m:ClCompile', ns)
def values(name):
    return [v for v in compiler.find('m:' + name, ns).text.split(';') if not v.startswith('%(')]
flags = '/nologo /std:c++latest /EHsc /MD /O2 '
flags += ' '.join('/D"' + v.replace('"', '\\"') + '"' for v in values('PreprocessorDefinitions'))
flags += ' ' + ' '.join('/I"' + v + '"' for v in values('AdditionalIncludeDirectories'))
source = repo / 'tools/vr-editor/probe.cpp'
obj = build / 'probe.obj'
(build / 'probe.compile.rsp').write_text(flags + f' /c "{source}" /Fo:"{obj}"')
line = (repo / 'build/Dev-Resources/Modularity.dir/Release/Modularity.tlog/link.command.1.tlog').read_text(encoding='utf-16').splitlines()[1]
line = re.sub(r'/OUT:"[^"]+"', '/OUT:"' + (repo / 'build/Dev-ExportedBuild/VREditorProbe.exe').as_posix() + '"', line, flags=re.I)
for arg in ('PDB', 'IMPLIB', 'DEF'):
    line = re.sub('/' + arg + r':"[^"]+"', '', line, flags=re.I)
line = line.replace('/SUBSYSTEM:WINDOWS', '/SUBSYSTEM:CONSOLE')
# The probe calls linked engine code directly; it does not load script DLLs.
# Keep normal archive reachability instead of the executable's SDK export roots.
line = re.sub(r'/WHOLEARCHIVE:(?:"[^"]+"|\S+)', '', line, flags=re.I)
line = re.sub(r'/DEBUG(?::\S+)?', '', line, flags=re.I)

line = re.sub(r'MODULARITY\.DIR\\RELEASE\\MAIN\.OBJ', lambda m: '"' + obj.as_posix() + '"', line, flags=re.I)
(build / 'probe.link.rsp').write_text(line)
vcvars = r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
command = f'call "{vcvars}" >nul && cl @"{build / "probe.compile.rsp"}"'
if '--compile-only' not in sys.argv:
    command += f' && link @"{build / "probe.link.rsp"}" /LTCG'
subprocess.run(command, shell=True, check=True, cwd=repo / 'build/Dev-Resources')
