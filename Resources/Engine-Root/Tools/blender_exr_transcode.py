"""Transcodes an EXR that Modularity's bundled reader cannot decompress into a
PNG it can.

Run headlessly by the editor:
    blender --background --factory-startup --python blender_exr_transcode.py \
            -- <input.exr> <output.png>

Blender links real OpenEXR, so it reads DWAA/DWAB - the lossy DreamWorks
compression Poly Haven ships by default and TinyEXR does not implement.

Colour management is the whole risk here, and it is why this does not simply call
save_render(). Normal, roughness and metalness maps are DATA, not pictures: run
them through a view transform (Blender's default is AgX) and the values come out
tone-mapped, which for a normal map means lighting that is subtly and
unfixably wrong. So:

  * the image is marked Non-Color, so Blender does not treat its values as sRGB
  * the view transform is forced to Standard and the look to None
  * save_render() is given a scene whose display settings pass values through

The result matches what a straight EXR decode would have produced, quantised to
8 bits - the same precision Modularity's own EXR path produces via
LoadExrToRgba8, so nothing is lost relative to a natively readable EXR.
"""
import os
import sys

import bpy


def main() -> int:
    argv = sys.argv
    if '--' not in argv:
        print('TRANSCODE_FAIL no arguments after --')
        return 1
    argv = argv[argv.index('--') + 1:]
    if len(argv) < 2:
        print('TRANSCODE_FAIL expected <input> <output>')
        return 1

    src, dst = argv[0], argv[1]
    if not os.path.isfile(src):
        print('TRANSCODE_FAIL input does not exist: %s' % src)
        return 1

    try:
        img = bpy.data.images.load(src)
    except Exception as exc:                                   # noqa: BLE001
        print('TRANSCODE_FAIL could not load: %s' % exc)
        return 1

    width, height = img.size
    if width <= 0 or height <= 0:
        print('TRANSCODE_FAIL decoded to an empty image')
        return 1

    # Data, not a picture. Set before saving so no transfer function is applied.
    try:
        img.colorspace_settings.name = 'Non-Color'
    except Exception:                                          # noqa: BLE001
        # Older/newer Blender may name it differently; Raw is the same intent.
        try:
            img.colorspace_settings.name = 'Raw'
        except Exception:                                      # noqa: BLE001
            pass

    scene = bpy.context.scene
    scene.view_settings.view_transform = 'Standard'
    try:
        scene.view_settings.look = 'None'
    except Exception:                                          # noqa: BLE001
        pass
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.display_settings.display_device = 'sRGB'

    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    # 8-bit matches what Modularity's own EXR path produces (LoadExrToRgba8), so
    # this loses nothing relative to an EXR the engine could read natively.
    scene.render.image_settings.color_depth = '8'
    scene.render.image_settings.compression = 15

    try:
        img.save_render(filepath=dst, scene=scene)
    except Exception as exc:                                   # noqa: BLE001
        print('TRANSCODE_FAIL could not save: %s' % exc)
        return 1

    if not os.path.isfile(dst):
        print('TRANSCODE_FAIL no output written')
        return 1

    print('TRANSCODE_OK %d %d' % (width, height))
    return 0


sys.exit(main())
