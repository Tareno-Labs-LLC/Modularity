from pathlib import Path
import math
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--original-right', type=Path, help='Optional original OBJ: verify UVs/positions/faces remain unchanged')
parser.add_argument('--package', type=Path, help='moduengine.vr-controllers-<version> folder (default: newest in the sibling Modu-Package-Manager checkout)')
args = parser.parse_args()

# The controller assets moved out of the engine into the moduengine.vr-controllers
# ModuPAK. Its payload mirrors the engine layout under Editor/, so the .controller
# files' "Resources/..." values resolve against <package>/Editor.
package = args.package
if package is None:
    registry = Path(__file__).resolve().parents[3] / 'Modu-Package-Manager' / 'Packages'
    found = sorted(registry.glob('*/*/moduengine.vr-controllers-*'))
    assert found, f'no moduengine.vr-controllers package under {registry}; pass --package'
    package = found[-1]
root = package / 'Editor'
assets = root / 'Resources/VR-Engine-Root'
for config in (assets / 'ControllerMaterials').glob('*.controller'):
    for line in config.read_text().splitlines():
        if '=' not in line:
            continue
        key, value = line.split('=', 1)
        if key in ('mesh', 'albedo', 'specular', 'normal', 'orm'):
            assert (root / value).is_file(), (config, key, value)

for path in (assets / 'Valve-Index-Knuckles').rglob('*.obj'):
    vertices, normals, scores = [], [], []
    volume = 0.0
    for line in path.read_text().splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 'v':
            vertices.append(tuple(map(float, parts[1:4])))
        elif parts[0] == 'vn':
            normals.append(tuple(map(float, parts[1:4])))
        elif parts[0] == 'f':
            corners = [part.split('/') for part in parts[1:4]]
            a, b, c = [vertices[int(part[0]) - 1] for part in corners]
            ab = [b[i] - a[i] for i in range(3)]
            ac = [c[i] - a[i] for i in range(3)]
            cross = (ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0])
            volume += sum(a[i]*cross[i] for i in range(3))/6
            length = math.sqrt(sum(x*x for x in cross))
            if length < 1e-10:
                continue
            face_normals = [normals[int(part[2]) - 1] for part in corners]
            scores.append(sum(cross[i] * sum(n[i] for n in face_normals) / 3 for i in range(3)) / length)
    bad = sum(score < 0 for score in scores) / len(scores)
    average = sum(scores) / len(scores)
    assert bad < .01 and average > .85, (path, bad, average)
    assert volume > 0, (path, 'inward face winding', volume)
    assert all(abs(math.sqrt(sum(x*x for x in n)) - 1) < .001 for n in normals)
    print(path.name, 'face/normal agreement:', round(average, 4), 'inward fraction:', round(bad, 5))
print('Controller configs and Index geometry PASS')
if args.original_right:
    current = next((assets / 'Valve-Index-Knuckles').glob('**/*right.obj'))
    def geometry_channels(path):
        return [line.split() for line in path.read_text().splitlines()
                if line.split() and line.split()[0] in ('v', 'vt', 'f')]
    assert geometry_channels(current) == geometry_channels(args.original_right), 'Geometry/UV channels changed'
    print('Original positions, texture UVs and face winding preserved PASS')
