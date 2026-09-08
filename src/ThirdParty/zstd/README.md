# zstd (single-file decompressor)

Decompression-only amalgamation of [zstd](https://github.com/facebook/zstd), vendored so
Modularity can read Zstandard-compressed `.blend` files (Blender 3.0+ writes them by
default) without depending on a `zstd` binary being installed. The previous approach shelled
out to the CLI, which is generally present on Linux and essentially never on Windows.

- **Version:** 1.6.0 (upstream commit `10da6ba6de05e29169261fa4b68eb99239f770dd`)
- **Licence:** BSD-3-Clause or GPL-2.0, at your option — see `LICENSE`. Modularity uses it
  under the BSD terms.

## Regenerating

From a zstd checkout:

```sh
cd build/single_file_libs
python combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
```

Then copy `zstddeclib.c` here, along with `lib/zstd.h`, `lib/zstd_errors.h` and `LICENSE`.

## Local deviation from the generated output

One line is removed:

```c
#define ZSTD_STRIP_ERROR_STRINGS
```

Upstream bakes it in to shrink the binary, but it makes `ZSTD_getErrorName()` return the
literal string `"Error strings stripped"`. Import failures are reported to the editor
console, so the real message is worth the few KB. Re-apply this edit after regenerating.

## Notes for anyone touching the decompression path

Blender does **not** write a compressed `.blend` as one zstd frame. It writes a sequence of
frames followed by a skippable seek-table frame, so it can seek within the file. That means
`ZSTD_decompress()` is the wrong call: it decodes only the first frame and returns a fraction
of the data. `ZSTD_getFrameContentSize()` likewise describes only the first frame. Use the
streaming API (`ZSTD_decompressStream` in a loop over the whole input) — it walks
concatenated frames and skips skippable ones on its own. See `DecompressZstdBlend` in
`src/ModelLoader.cpp`.
