# TinyEXR

Single-header OpenEXR reader from [tinyexr](https://github.com/syoyo/tinyexr), vendored so
the engine can load `.exr` textures. Poly Haven and similar libraries ship normal,
roughness and metalness maps as EXR, and stb_image has no EXR support.

- **Licence:** BSD-3-Clause (see the header of `tinyexr.h`).
- **Source:** `master` branch, unmodified. Three files, not one: recent versions split
  the bounds-checked reader out of the amalgamation, so `tinyexr.h` includes
  `exr_reader.hh`, which in turn includes `streamreader.hh`. All three must be kept
  together when updating.

## Zlib backend

TinyEXR needs a zlib-compatible API for its compressed scanlines. It is compiled here with
`TINYEXR_USE_STB_ZLIB=1`, which routes that through `stbi_zlib_decode_buffer` /
`stbi_zlib_compress` — both already compiled into the engine (`Textures/Texture.cpp` defines
`STB_IMAGE_IMPLEMENTATION`, `stb_image_write_impl.cpp` defines `STB_IMAGE_WRITE_IMPLEMENTATION`).
That avoids vendoring miniz or wiring up a second zlib.

The implementation is compiled exactly once, in `src/Textures/ExrLoader.cpp`. Include that
header rather than `tinyexr.h` — the amalgamation is ~11k lines and pulls in a lot.

## Note on the decoded data

EXR is floating point and usually linear. `LoadExrToRgba8` clamps to [0,1] and scales to
8-bit, which is right for the data maps this exists to load (normal/roughness/metal are all
authored in that range). A genuinely high-dynamic-range colour EXR would lose its highlights
— if that ever matters, the fix is a float texture path, not a different clamp here.
