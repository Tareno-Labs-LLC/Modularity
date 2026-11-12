# Matugen overlay regression check

After configuring/building the engine's ImGui library, run from the repo root:

```sh
g++ -std=c++17 -O0 -g -pthread -DIMGUI_USE_WCHAR32 -Isrc \
    tools/theme-selftest/matugen_test.cpp build/libimgui.a \
    $(pkg-config --libs freetype2) -ldl -o /tmp/modularity-matugen-test
/tmp/modularity-matugen-test
```

This uses a temporary generated CSS file. It checks fade interpolation,
preservation of project colors/alpha/layout, palette replacement, invalid and
missing files, disabling the overlay, and deliberate editor color edits. It
does not modify the desktop's actual Matugen palette.
