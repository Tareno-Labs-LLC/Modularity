"""GPU regressions for normal-map precision and grazing specular lighting.

Run with Python, numpy and moderngl. Uses an offscreen OpenGL 3.3 context.
"""
from pathlib import Path
import numpy as np
import moderngl as gl

ROOT = Path(__file__).resolve().parents[2]
ctx = gl.create_standalone_context(require=330)
W, H = 640, 480
source = (ROOT / 'Resources/Shaders/frag.glsl').read_text()
# Isolate the direct specular contribution without replacing its implementation.
specular_source = source.replace('(attenuation * diffuse + specular)', 'specular')
vertex = '''#version 330 core
in vec3 pos;
in vec2 uv;
out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
uniform mat4 vp;
void main() {
    FragPos = pos;
    Normal = vec3(0, 1, 0);
    TexCoord = uv;
    gl_Position = vp * vec4(pos, 1);
}
'''
vertices = np.array([
    [-12.5, 0, 12.5, 0, 0], [12.5, 0, 12.5, 1, 0], [12.5, 0, -12.5, 1, 1],
    [-12.5, 0, 12.5, 0, 0], [12.5, 0, -12.5, 1, 1], [-12.5, 0, -12.5, 0, 1],
], dtype='f4')
vbo = ctx.buffer(vertices.tobytes())
output = ctx.texture((W, H), 4, dtype='f4')
fbo = ctx.framebuffer([output])


def render(fragment, height=.6, normal=(128, 128, 255), normal_format=0x8051,
           use_pbr=True, half_target=False):
    program = ctx.program(vertex_shader=vertex, fragment_shader=fragment)
    eye = np.array([2., height, 8.])
    forward = np.array([0., -.08, -1.])
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    view = np.eye(4)
    view[:3, :3] = [right, up, -forward]
    view[:3, 3] = -view[:3, :3] @ eye
    q = 1 / np.tan(np.radians(45) / 2)
    near, far = .1, 100.
    projection = np.array([
        [q / (W / H), 0, 0, 0], [0, q, 0, 0],
        [0, 0, -(far + near) / (far - near), -2 * far * near / (far - near)],
        [0, 0, -1, 0],
    ])
    program['vp'].write((projection @ view).T.astype('f4').tobytes())
    unit = 0
    for name in program:
        uniform = program[name]
        # GL validates sampler types even when their paths are disabled.
        if isinstance(uniform, gl.Uniform) and uniform.gl_type in [35678, 35679, 35680]:
            uniform.value = unit
            unit += 1
    values = dict(viewPos=tuple(eye), uvTransform=(15, 15, 0, 0), usePbr=use_pbr,
                  pbrMetallic=1., pbrRoughness=.582, ambientStrength=0.,
                  normalMapIntensity=2., hasNormalMap=True, lightCount=1)
    for name, value in values.items():
        if name in program:
            program[name].value = value
    for name, first in [('lightTypeArr', 0), ('lightDirArr', (-.41, -.66, .63)),
                        ('lightColorArr', (1, 1, 1)), ('lightIntensityArr', 1.)]:
        if name in program:
            uniform = program[name]
            rest = (0, 0, 0) if isinstance(first, tuple) else 0
            uniform.value = [first] + [rest] * (uniform.array_length - 1)
    textures = []
    for name, pixel, fmt in [('texture1', (255, 255, 255), 0x8051),
                              ('normalMap', normal, normal_format)]:
        if name not in program:
            continue
        texture = ctx.texture((1, 1), 3, bytes(pixel), internal_format=fmt)
        texture.build_mipmaps()
        texture.filter = (gl.NEAREST_MIPMAP_NEAREST, gl.NEAREST)
        texture.use(program[name].value)
        textures.append(texture)
    vao = ctx.vertex_array(program, [(vbo, '3f 2f', 'pos', 'uv')])
    half_texture = ctx.texture((W, H), 4, dtype='f2') if half_target else None
    target = ctx.framebuffer([half_texture]) if half_target else fbo
    target.use()
    ctx.viewport = (0, 0, W, H)
    target.clear(0, 0, 0, 0)
    vao.render()
    data = np.frombuffer(target.read(components=4, dtype='f4'), dtype='f4').reshape(H, W, 4).copy()
    assert np.isfinite(data).all()
    assert ctx.error == 'GL_NO_ERROR'
    for texture in textures:
        texture.release()
    vao.release()
    program.release()
    if half_target:
        target.release()
        half_texture.release()
    return data


# The earlier precision-only test missed full-precision normals at lower camera
# heights. Exercise both encoded-neutral and deliberately tilted normal maps.
for height in [.6, .04, .004]:
    for normal in [(128, 128, 255), (128, 140, 254)]:
        for fmt in [0x8051, 0x8D62]:  # RGB8 and RGB565
            for pbr in [True, False]:
                result = render(specular_source, height, normal, fmt, pbr)
                floor = result[..., 3] > .5
                assert floor.any()
                black = np.count_nonzero(floor & (result[..., :3].max(axis=2) < 1e-5))
                assert black == 0, (height, normal, fmt, pbr, black)
print('PASS: grazing view/normal/precision sweep for PBR and legacy specular')

# Compare the actual forward shading normal with the encoded SSR surface normal.
normal_source = source.replace('FragColor = vec4(finalColor, alpha);',
                               'FragColor = vec4(norm * 0.5 + 0.5, alpha);')
# The correction must not flip genuinely back-facing geometry toward the eye.
raw_normal_source = normal_source.replace(
    'viewFacingMappedNormal(normalize(TBN * mapN), normalize(Normal), viewDir)',
    'normalize(TBN * mapN)')
backface = render(normal_source, height=-.04)
raw_backface = render(raw_normal_source, height=-.04)
assert np.any(backface[..., 3] > .5)
assert np.max(np.abs(backface - raw_backface)) < 1e-6
print('PASS: true backface normals are unchanged')

forward = render(normal_source, height=.04, normal=(128, 140, 254))
gbuffer = render((ROOT / 'Resources/Shaders/gbuffer_frag.glsl').read_text(),
                 height=.04, normal=(128, 140, 254))
mask = forward[..., 3] > .5
encoded = gbuffer[..., :2]
decoded = np.dstack([encoded, 1 - np.abs(encoded).sum(axis=2)])
t = np.maximum(-decoded[..., 2], 0)
decoded[..., :2] += np.where(decoded[..., :2] >= 0, -t[..., None], t[..., None])
decoded /= np.maximum(np.linalg.norm(decoded, axis=2, keepdims=True), 1e-8)
assert np.max(np.abs(decoded[mask] - (forward[mask, :3] * 2 - 1))) < 1e-5
print('PASS: forward and SSR surface normals agree')
# The engine stores this buffer as RGBA16F. Its quantization must not push the
# corrected normal back across the view horizon and cause SSR's early rejection.
half_surface = render((ROOT / 'Resources/Shaders/gbuffer_frag.glsl').read_text(),
                      height=.04, normal=(128, 140, 254), half_target=True)
e = half_surface[..., :2]
normals = np.dstack([e, 1 - np.abs(e).sum(axis=2)])
t = np.maximum(-normals[..., 2], 0)
normals[..., :2] += np.where(normals[..., :2] >= 0, -t[..., None], t[..., None])
normals /= np.maximum(np.linalg.norm(normals, axis=2, keepdims=True), 1e-8)
x, y = np.meshgrid((np.arange(W) + .5) / W * 2 - 1,
                   (np.arange(H) + .5) / H * 2 - 1)
q = 1 / np.tan(np.radians(45) / 2)
f = np.array([0., -.08, -1.]); f /= np.linalg.norm(f)
r = np.cross(f, [0, 1, 0]); r /= np.linalg.norm(r)
u = np.cross(r, f)
view_vectors = -np.stack([x * (W / H) / q, y / q, -np.ones_like(x)], axis=2) @ np.array([r, u, -f])
view_vectors /= np.linalg.norm(view_vectors, axis=2, keepdims=True)
cosines = np.sum(normals * view_vectors, axis=2)[mask]
assert np.min(cosines) > 0, (np.min(cosines), np.count_nonzero(cosines <= 0))
print('PASS: corrected normals remain view-facing after RGBA16F storage')
fbo.use()


# Check the finite grazing limit directly: no arbitrary denominator clamp may
# drive specular to zero as positive NdotV approaches zero.
probe = source[:source.index('void main()')] + '''
uniform float probeNdotV;
void main() {
    vec3 V = normalize(vec3(0, probeNdotV, 1));
    vec3 value = evaluateDirectSpecular(vec3(0,1,0), V,
        normalize(vec3(0,1,-1)), vec3(0.04), 0.6, 0.4,
        0.0, 0.0, vec3(1), 1.0, true);
    FragColor = vec4(value, 1);
}
'''
program = ctx.program(vertex_shader='''#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0, 1);
}
''', fragment_shader=probe)
vao = ctx.vertex_array(program, [])
samples = []
for cosine in [1e-4, 1e-5, 1e-6, 1e-7]:
    program['probeNdotV'].value = cosine
    vao.render(vertices=3)
    samples.append(np.frombuffer(fbo.read(viewport=(0, 0, 1, 1), components=4,
                                         dtype='f4'), dtype='f4')[0])
assert min(samples) > 0 and max(samples) / min(samples) < 1.01, samples
print('PASS: stable finite specular limit down to NdotV=1e-7')
print(ctx.info['GL_RENDERER'])
