"""Check Android toolchain discovery on Windows without installing or changing SDKs."""
from pathlib import Path
import os
import subprocess
import tempfile
import sys

repo = Path(__file__).resolve().parents[2]
out = repo / 'build/Dev-Resources/android-selftest'
out.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
vcvars = Path(vs) / 'VC/Auxiliary/Build/vcvars64.bat'
exe = out / 'android-toolchain-test.exe'
subprocess.run(f'call "{vcvars}" >nul && cl /nologo /std:c++20 /EHsc /MD /O2 '
    f'/I"{repo / "src"}" "{repo / "tools/android-selftest/toolchain_test.cpp"}" '
    f'/Fo:"{out.as_posix()}/" /Fe:"{exe}"', shell=True, cwd=out, check=True)
fixture = tempfile.mkdtemp(prefix='fixture-', dir=out)
subprocess.run([str(exe), fixture], cwd=out, check=True)

if '--scripts' in sys.argv:
    sdk = Path(os.environ.get('ANDROID_SDK_ROOT', repo / 'Build-Android/android-sdk'))
    ndk_value = os.environ.get('ANDROID_NDK_ROOT')
    if ndk_value:
        ndk = Path(ndk_value)
    else:
        candidates = [p for p in (sdk / 'ndk').iterdir() if p.name.replace('.', '').isdigit()]
        ndk = max(candidates, key=lambda p: tuple(int(n) for n in p.name.split('.')))
    driver = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android26-clang++.cmd'
    sources = [repo / 'tools/android-selftest/script_test.cpp'] + [repo / 'src' / name for name in
        ['ScriptCompiler.cpp', 'EmbeddedScript.cpp', 'ModuCPPTranspiler.cpp', 'ModuCPPLanguagePack.cpp']]
    includes = ['src', 'include', 'src/ThirdParty/ModuGUI', 'src/ThirdParty/glm',
                'src/ThirdParty/glfw/include', 'src/ThirdParty/glad']
    exe = out / 'android-script-test.exe'
    rsp = out / 'script-test.rsp'
    rsp.write_text('/nologo /std:c++23preview /EHsc /MD /Od /Gy /MP1 /DNOMINMAX '
        '/DWIN32_LEAN_AND_MEAN /DMODULARITY_USE_MONO=0 ' +
        ' '.join(f'/I"{repo / p}"' for p in includes) + ' ' +
        ' '.join(f'"{p}"' for p in sources) + f' /Fo:"{out.as_posix()}/" /Fe:"{exe}" '
        '/link /OPT:REF user32.lib shell32.lib advapi32.lib')
    subprocess.run(f'call "{vcvars}" >nul && cl @"{rsp}"', shell=True, cwd=out, check=True)
    subprocess.run([str(exe), str(repo), str(driver), str(out / 'script fixture with spaces')], cwd=repo, check=True)
