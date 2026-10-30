"""Offscreen SSR regression. Requires numpy and moderngl; no editor needed."""
from pathlib import Path
import subprocess
import sys
import numpy as np
import moderngl

ROOT = Path(__file__).resolve().parents[2]
ctx = moderngl.create_standalone_context(require=330)
W, H = 160, 120
near, far = 0.1, 100.0
f = 1.0 / np.tan(np.radians(60.0) / 2.0)
projection = np.array([[f / (W / H), 0, 0, 0], [0, f, 0, 0],
                       [0, 0, -(far + near) / (far - near), -2 * far * near / (far - near)],
                       [0, 0, -1, 0]], dtype='f4')
x, y = np.meshgrid((np.arange(W) + 0.5) / W * 2 - 1,
                   (np.arange(H) + 0.5) / H * 2 - 1)
rays = np.stack([x * (W / H) / f, y / f, -np.ones_like(x)], axis=-1)
floor_t = np.divide(-1., rays[..., 1], out=np.full((H, W), np.inf), where=rays[..., 1] < 0)
wall_t = np.full((H, W), 6.)
floor = floor_t < wall_t
t = np.minimum(floor_t, wall_t)
points = rays * t[..., None]
clip = np.concatenate([points, np.ones((H, W, 1))], axis=-1) @ projection.T
depth = (clip[..., 2] / clip[..., 3] * .5 + .5).astype('f4')
surface = np.zeros((H, W, 4), dtype='f4')
surface[..., 1] = floor  # octEncode(+Y)=(0,1); octEncode(+Z)=(0,0)
surface[..., 2] = .03
surface[..., 3] = 1
color = np.zeros((H, W, 3), dtype='f4')
color[:] = [1, .25, .1]
textures = [ctx.texture((W, H), n, data.tobytes(), dtype='f4')
            for n, data in [(3, color), (1, depth), (4, surface)]]
for texture in textures:
    texture.filter = (moderngl.NEAREST, moderngl.NEAREST)
    texture.repeat_x = texture.repeat_y = False
out = ctx.texture((W, H), 4, dtype='f4')
fbo = ctx.framebuffer([out])
vertices = np.array([-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1], dtype='f4')
vbo = ctx.buffer(vertices.tobytes())

def render(source, distance=40., thickness=.01, roughness=.6):
    program = ctx.program(vertex_shader=(ROOT / 'Resources/Shaders/postfx_vert.glsl').read_text(),
                          fragment_shader=source)
    for name, matrix in [('viewProj', projection), ('invViewProj', np.linalg.inv(projection))]:
        program[name].write(matrix.T.astype('f4').tobytes())
    values = dict(viewPos=(0,0,0), screenSize=(W,H), nearPlane=near, farPlane=far,
                  intensity=1., steps=48, refineSteps=12, maxDistance=distance,
                  thickness=thickness, maxRoughness=roughness, hasFallbackCube=False,
                  sceneColor=0, sceneDepth=1, sceneSurface=2, fallbackCube=3)
    for name, value in values.items():
        program[name].value = value
    for unit, texture in enumerate(textures):
        texture.use(unit)
    vao = ctx.vertex_array(program, [(vbo, '2f 2f', 'aPos', 'aTexCoord')])
    fbo.use()
    ctx.viewport = (0,0,W,H)
    vao.render(moderngl.TRIANGLE_STRIP)
    result = np.frombuffer(fbo.read(components=4, dtype='f4'), dtype='f4').reshape(H,W,4)
    assert np.isfinite(result).all(), 'Non-finite reflection output'
    assert (result[...,3] >= 0).all() and (result[...,3] <= 1).all()
    assert ctx.error == 'GL_NO_ERROR'
    vao.release()
    program.release()
    return result

current = (ROOT / 'Resources/Shaders/ssr_frag.glsl').read_text()
new = render(current)
new_hits = np.count_nonzero(new[...,3][floor] > .01)
print(ctx.info['GL_RENDERER'])
print(f'Thin wall reflection coverage: {new_hits}/{np.count_nonzero(floor)} floor pixels')
assert new_hits > np.count_nonzero(floor) * .8, 'Skipped thin-wall intersections'
assert np.max(new[...,3][floor]) > .5, 'Expected a visible floor reflection'
if '--compare-head' in sys.argv:
    original = subprocess.check_output(['git', 'show', 'HEAD:Resources/Shaders/ssr_frag.glsl'], cwd=ROOT, text=True)
    old_hits = np.count_nonzero(render(original)[...,3][floor] > .01)
    print(f'HEAD comparison: {old_hits} reflection pixels')
# Exercise endpoint clipping, finite ranges, degenerate roughness cutoff, and
# camera-facing rays (the vertical wall reflects back toward the camera).
for distance in [.1, 2., 10., 40., 200.]:
    render(current, distance=distance)
assert not np.any(render(current, roughness=0.))
# Empty depth must never produce intersections, even at the far-plane exit.
textures[1].write(np.ones((H,W), dtype='f4').tobytes())
assert not np.any(render(current))
print('PASS: GLSL link, recovered crossings, clipping/range sweep, zero cutoff, sky rejection')
