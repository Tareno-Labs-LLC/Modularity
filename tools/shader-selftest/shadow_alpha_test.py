"""GPU regression for directional/cubemap shadow alpha. Run with numpy and moderngl."""
from pathlib import Path
import moderngl as gl
import numpy as np

ROOT = Path(__file__).resolve().parents[2] / "Resources/Shaders"
ctx = gl.create_standalone_context(require=330)
identity = np.eye(4, dtype="f4").tobytes()
vertices = np.array([
    [-1, -1, 0, 0, 0], [1, -1, 0, 1, 0], [1, 1, 0, 1, 1],
    [-1, -1, 0, 0, 0], [1, 1, 0, 1, 1], [-1, 1, 0, 0, 1],
], dtype="f4")
vbo = ctx.buffer(vertices.tobytes())
depth = ctx.depth_texture((32, 32))
fbo = ctx.framebuffer(depth_attachment=depth)
ctx.enable(gl.DEPTH_TEST)
albedo = ctx.texture((2, 1), 4, bytes([255, 255, 255, 0, 255, 255, 255, 255]))
albedo.filter = (gl.NEAREST, gl.NEAREST)
opacity = ctx.texture((1, 1), 1, bytes([255]))

for prefix in ("shadow_directional_depth", "shadow_depth"):
    fragment = (ROOT / (prefix + "_frag.glsl")).read_text()
    # Verify both optimized and individual interfaces link against the same fragment stage.
    for suffix in ("_vert.glsl", "_mdi_vert.glsl"):
        program = ctx.program(vertex_shader=(ROOT / (prefix + suffix)).read_text(),
                              fragment_shader=fragment)
        if suffix.startswith("_mdi"):
            program.release()
            continue
        program["model"].write(identity)
        program["lightSpaceMatrix"].write(identity)
        program["albedoTex"].value = 0
        program["opacityMap"].value = 1
        if "farPlane" in program:
            program["farPlane"].value = 10
            program["lightPos"].value = (0, 0, -1)
        vao = ctx.vertex_array(program, [(vbo, "3f 2f", "aPos", "aTexCoord")])

        def render(alpha=1, cutoff=0, use_albedo=True, use_opacity=False, uv=(1, 1, 0, 0)):
            program["materialAlpha"].value = alpha
            program["alphaCutoff"].value = cutoff
            program["hasAlbedoTex"].value = use_albedo
            program["hasOpacityMap"].value = use_opacity
            program["uvTransform"].value = uv
            albedo.use(0)
            opacity.use(1)
            fbo.use()
            fbo.clear(depth=1)
            vao.render()
            return np.frombuffer(depth.read(), dtype="f4").reshape(32, 32) < .99

        mask = render()
        assert not mask[:, :16].any() and mask[:, 16:].all(), prefix + " transparent texels"
        assert render(use_opacity=True).all(), prefix + " opacity overrides albedo"
        assert not render(alpha=0, use_albedo=False).any(), prefix + " invisible material"
        assert render(use_albedo=False).all(), prefix + " opaque material"
        assert render(alpha=.5, use_albedo=False).sum() == 512, prefix + " translucent coverage"
        assert not render(alpha=.5, cutoff=.6, use_albedo=False).any(), prefix + " cutoff discard"
        assert render(alpha=.7, cutoff=.6, use_albedo=False).all(), prefix + " cutoff solid"
        mask = render(uv=(1, 1, .5, 0))
        assert mask[:, :16].all() and not mask[:, 16:].any(), prefix + " transformed UVs"
        print(prefix + ": PASS (alpha, opacity map, cutoff, coverage, UVs, batch link)")
