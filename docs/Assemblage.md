# Assemblage

Assemblage is Modularity's **structured world authoring** mode. It sits beside the
existing **Freeform** workflow (place sprites and SceneOBJs anywhere, rotate and
scale them freely) and does not replace it. A scene can use either, or both at
once.

If you have used a tilemap editor before, the 2D grid that ships today will feel
familiar. The name is deliberately broader than "tilemap": the asset format
carries a `kind` discriminator so later structured-authoring workflows can reuse
it instead of forking a second format.

> **Status:** the data model, on-disk formats, editing tools and rendering are
> implemented. Tile collision and auto-tiling arrive in later stages; see
> section 9 for exactly what is still deferred.

## 1) How it fits into a scene

The important design rule: **a scene never stores cells.**

An Assemblage appears in the hierarchy as an ordinary SceneOBJ carrying an
**Assemblage** component, with one child SceneOBJ per layer carrying an
**Assemblage Layer** component. The cells themselves live in a separate
`.moduasm` asset that the root object references by path.

```
Nightfall's Cabin
├── Cabin Tiles              [Assemblage]  -> Assets/Maps/Cabin.moduasm
│   ├── Background           [Assemblage Layer]  layerId=1
│   ├── Floor                [Assemblage Layer]  layerId=2
│   └── Walls                [Assemblage Layer]  layerId=3
├── Nightfall                (Freeform)
├── Bed                      (Freeform)
└── Foreground Shadows       [Assemblage Layer]  layerId=4
```

This buys three things:

- **Undo stays cheap.** Modularity's undo snapshots the whole SceneObject vector.
  A 10,000 cell map that lived inside SceneObjects would be deep-copied on every
  edit, 64 times over. Two or three objects are free to copy.
- **Layer ordering is not a new system.** A layer sorts by the object's own
  `layer` plus the component's `sortingOrder`, in the same pass as freeform
  sprites. A freeform object placed between two tile layers simply sorts there;
  nothing special-cases it.
- **Everything else comes free.** Renaming, hiding, reordering, selecting,
  parenting and serializing a layer are the operations that already work on any
  SceneOBJ.

A scene may contain **any number of Assemblages**, each with its own grid,
tileset and cell size.

### Scene compatibility

Both components are written to a scene **only when present**. A scene with no
Assemblage is byte-identical before and after this feature, and needs no
conversion. Because the scene stores only a *reference*, an older build that does
not understand these keys cannot destroy a map by re-saving the scene - the
`.moduasm` file is untouched.

## 2) The `.moduasm` format

Text, versioned, and built from the same `MODU_*` block grammar as scenes and
ModuOBJ assets, so it diffs and merges in git like everything else.

```
MODU_ASSEMBLAGE {
    formatVersion=1;
    assemblageId="asm-0123456789abcdef";
    name="Cabin Floor";
    kind="grid2d";
    chunkSize=32;
    cellSize=1,1;
    origin=0,0;
    nextLayerId=4;
    tileset="Assets/Tiles/Cabin.modutile";
}

MODU_ASMLAYER = (id=1, name="Ground") {
    kind="tile";
    order=0;
    sortingOrder=0;
    opacity=1;
    tint=1,1,1,1;
    visible=1;
    locked=0;
    collision=1;
}

MODU_ASMCHUNK = (layer=1, coord=0,0) {
    encoding="rle32";
    cells=3*17,18,1020*0;
}
```

### Extension points

| Field | Purpose |
|---|---|
| `formatVersion` | A file newer than the engine is **refused with a clear message**, never half-parsed. |
| `kind` | Which structured model this is. v1 defines `grid2d`. A new model claims a new kind rather than overloading this one. |
| layer `kind` | Which content a layer holds. v1 defines `tile`. |
| `coord` | Variable-arity, so a future 3D kind writes `coord=0,0,0` without changing the block grammar. |
| `encoding` | Names the cell codec, so a binary or base64 codec can be added without a new block type. |

An unrecognized `MODU_` block is skipped by brace depth, so a file written by a
newer minor tool still loads whatever this engine does understand.

### Chunks

Cells are grouped into fixed `chunkSize` x `chunkSize` blocks (32 by default,
1024 cells). Chunks exist so a single edit rebuilds one chunk instead of the
whole map, and so a sparse map costs nothing for the empty space - a chunk is
allocated on first non-empty write and **reclaimed when its last cell is
cleared**.

Chunk coordinates use floor division, so negative cell coordinates work and a map
can grow in any direction from its origin. Chunks are written in sorted order so
a saved asset is byte-stable across runs.

### Cells

Each cell is one packed `uint32`:

| Bits | Meaning |
|---|---|
| 0-23 | Tile id. `0` means empty, so a zeroed chunk is an empty chunk. |
| 24-26 | Transform flags: flip X, flip Y, rotate 90. |
| 27-31 | Reserved, written as zero. |

Cells serialize run-length encoded as `count*value` pairs, with the count omitted
for a run of one. An empty or uniform chunk collapses to a single token
(`cells=1024*0`), so file size tracks content rather than area.

## 3) The `.modutile` format

Tile definitions live in their own asset so many maps can share one tileset, and
so a cell stores a small id rather than a copy of the sprite information.

```
MODU_TILESET {
    formatVersion=1;
    tilesetId="tls-0123456789abcdef";
    name="Cabin";
    nextTileId=3;
}

MODU_TILE = (id=1, name="Grass") {
    sheet="Assets/Tiles/cabin.png";
    rect=0,0,16,16;
    tags="ground,walkable";
    collision="box";
    collisionSize=1,1;
    collisionOffset=0,0;
    tint=1,1,1,1;
    meta.durability="3";
}

MODU_TILE = (id=2, name="Water") {
    sheet="Assets/Tiles/cabin.png";
    rect=16,0,16,16;
    collision="none";
    animFps=8;
    animMode="pingpong";
    animFrames="16,0,16,16|32,0,16,16|48,0,16,16";
    autoRuleSet="blob";
    variants=3,4;
}
```

Tile id `0` is reserved for "empty" and may not be claimed. Duplicate ids are
rejected rather than repaired, because they would silently merge two tiles.
`meta.*` keys are free-form user metadata, preserved verbatim.

`rect` is a pixel region on the sheet, matching how `SpritesheetDocument` already
stores sprite frames, so a tile can be cut from an existing spritesheet sidecar.

### Animated tiles share one timeline

Animation is defined on the **tile definition**, never on the cell. The current
frame is a pure function of the definition and the clock:

```cpp
int frame = Assemblage::ResolveAnimationFrame(tile, timeSeconds);
```

There is no per-cell timer to update and no per-cell state to store, so a map
with 50,000 animated cells costs exactly as much to animate as a map with one,
and every cell using a given tile is always in step.

Modes are `loop`, `pingpong` and `once`. A tile with `animFps` unset or fewer
than two frames is a still tile and always resolves frame 0.

## 4) 2D collider additions

`Collider2DType` gained two shapes alongside the existing Box, Polygon and Edge.
Existing values are unchanged, so saved scenes are unaffected.

- **Circle** - a real radius test, using `radius`.
- **Line** - a single segment with thickness, solved as a true capsule with round
  caps rather than the axis-aligned quad an Edge segment expands to. Uses
  `points[0..1]` and `edgeThickness` as the diameter.

`Collider2DComponent` also gained the **sprite outline** generator settings
(`outlineAlphaThreshold`, `outlineTolerance`, `outlineMaxVertices`,
`outlineClosed`, `outlineSourcePath`). These drive an *edit-time* generator that
writes ordinary polygon points into `points`: the runtime never does per-pixel
work, and the generated outline stays hand-editable afterward. They are stored on
the component so regenerating reproduces the same geometry after a reload, and
are written to a scene only when they differ from their defaults.

## 5) Script ABI

Adding these components changed `sizeof(SceneObject)`, which is hashed into the
script layout signature, so `MODULARITY_NATIVE_SCRIPT_ABI_VERSION` moved from 44
to **45**. Compiled scripts must be rebuilt once.

This bump is banked for the whole Assemblage feature. Fields needed by the later
stages (editing, rendering, collision, the ModuCPP spawn API) go in under this
same version rather than forcing another rebuild per release.

## 6) Self-test

The Assemblage core depends only on glm and the standard library - no Engine, no
renderer, no GL context - so it is verified headlessly:

```bash
tools/assemblage-selftest/run.sh          # POSIX toolchain
```
```powershell
powershell -ExecutionPolicy Bypass -File tools\assemblage-selftest\run.ps1   # Windows / MSVC
```

Unlike the other self-tests in `tools/`, this one needs no prior engine build: it
compiles `src/Assemblage.cpp` directly. The Windows runner locates the MSVC
toolset with `vswhere` and enters a developer shell itself, so it runs from a
plain prompt.

Coverage includes cell packing, chunk addressing across the negative-coordinate
boundary, chunk reclamation, world/cell mapping, animation frame resolution,
round-tripping both formats, RLE compaction, and every rejection path (newer
format version, unknown kind, unknown layer, tile id 0, duplicate tile ids).

## 7) Editing (stage 2)

The viewport toolbar carries a **Freeform | Assemblage** switch beside the
existing snap and grid controls. It is a tool-mode switch, not a scene switch:
both kinds of content stay loaded and drawn in either mode: only the active tools
change.

The **Assemblage** panel (Workflow menu) holds the tools, the layer list and the
tile palette.

| Tool | Behaviour |
|---|---|
| Pencil | Paint cells; drag paints a stroke |
| Eraser | Clear cells; drag erases a stroke |
| Rectangle | Drag to fill a rectangle |
| Fill | Flood fill the connected matching region |
| Line | Drag to draw a straight run of cells |
| Eyedropper | Adopt the tile under the cursor |
| Select | Drag a region; Ctrl+C / Ctrl+V copy and paste |

Freehand strokes interpolate between frames, so a fast drag does not leave holes
where the cursor jumped more than a cell.

**Undo** holds two kinds of entry in one stack. An object edit still snapshots the
whole scene as it always has; a tile edit carries only the cells it touched. One
stroke is one entry regardless of how many cells it painted, and tile edits undo
in the correct order relative to object edits because they share the stack.

Dropping an image on the palette adds tiles: one per region when the image has a
`.spritesheet` sidecar, otherwise a single whole-image tile. The tileset asset is
created on the first drop, so an Assemblage that is never painted leaves no stray
files behind.

Painted cells are flushed to their `.moduasm` when the **scene** is saved, so
normal Ctrl+S covers tile work. The Inspector also has an explicit Save button
per Assemblage.

## 8) Rendering (stage 3)

Assemblage layers ride the **same sorted draw list as every 2D sprite**. The one
shared sort predicate reads the layer's `sortingOrder` from its own component and
a sprite's from `ui.sortingOrder`, so a Freeform sprite ordered between two tile
layers genuinely draws between them with no special casing anywhere.

Tiles are emitted into the **Light2D compositor** alongside sprites when it is
running. This is not optional: the compositor draws its whole result as a single
image *before* any legacy-path sprite, so a tile left on the legacy path would
float above every lit sprite regardless of sorting order. The legacy batched
emitter is the fallback for frames where the compositor did not run.

Geometry is cached per chunk in **world space**, not screen space. The camera
moves every frame but cells do not, so a chunk is rebuilt only when its cells
change; projection to screen costs two multiplies per corner. Culling is one AABB
test per chunk, standing in for up to 1024 cells.

Editing one cell invalidates its chunk, plus the neighbouring chunks across an
edge when the cell sits on one. That neighbour invalidation exists now so
auto-tiling needs no new plumbing later.

All four 2D paths draw Assemblage content: the editor scene view, the editor game
view, and the standalone player. The fourth, Render-In-3D UI canvases, explicitly
skips it: those draw in their own screen space rather than the 2D world, so a tile
grid has no meaning there.

## 9) Deferred and stubbed

Recorded here so the gaps are visible rather than discovered.

**Deferred to the collision stage**
- Tile collision is stored (`collision`, `collisionSize`, `collisionOffset`,
  `collisionRadius`, `collisionPoints` on the tile; `collisionEnabled` on the
  layer) and shown in the Inspector, but **no colliders are generated from it
  yet**. Adjacent-collider merging is part of that stage.
- `Collider2DType::Circle` and `Line` exist as enum values with their fields
  serialized, but the 2D SAT solver in `Engine.cpp` does not handle them yet, so
  a collider set to either currently behaves as its default shape.
- The sprite-outline generator settings are stored but nothing generates an
  outline yet.

**Deferred to the auto-tiling stage**
- `autoRuleSet` is stored on tiles and never read. Neighbour rules, corners,
  edges, inner corners and isolated-tile handling are all still to come.
- `variants` *is* used, but only by the random-variation brush, not by rules.
- The dirty-neighbour invalidation auto-tiling needs is already implemented and
  running (`AssemblageRuntime::markCellDirty`), so that stage adds rule
  evaluation rather than new plumbing.

**Intentionally minimal for now**
- **Layer rename** is done on the layer SceneOBJ in the Hierarchy; the panel
  shows the asset's layer name but does not edit it. The two can drift.
- **Layer reorder** is by editing sorting order, not drag-and-drop.
- **Move selection** is not implemented. Select plus copy/paste works; dragging a
  selection to a new position does not.
- **Animated tiles** have no authoring UI. The format and the runtime resolve are
  complete, so a hand-written `.modutile` animates correctly today.
- **Grid origin and cell size** are edited in the Inspector, not with viewport
  handles.
- **Tile rotation flags** (`CellFlag_Rot90`) are stored, applied in UVs for
  flips, but no brush sets them.
- Adding or removing a layer takes a **full scene undo snapshot** rather than a
  delta. It is a rare structural operation and the layer objects genuinely change,
  so the cheap path does not apply.
- There is **no ModuCPP API** for reading or writing cells at runtime.
- `.moduasm` and `.modutile` have **no File Browser icons or double-click open**.

## 10) Editor / runtime split

The player is a compile-time strip of the editor, and Assemblage respects that
line:

| Compiled into | Files |
|---|---|
| Editor **and** player | `Assemblage.{h,cpp}`, `AssemblageRuntime.{h,cpp}`, the emitter in `ViewportRenderHelpers` |
| Editor **only** | `EditorWindows/AssemblageWindow.cpp`, `EditorWindows/AssemblageViewport.cpp` |

So a shipped game loads, animates and draws Assemblage content, and carries none
of the authoring code.

Three call sites in runtime translation units reach into the authoring API and
are guarded with `#if !MODULARITY_RUNTIME_ONLY`:

- the tile-delta branches of `Engine::undo` / `Engine::redo` (the player never
  records a tile edit, so it can never hold an entry of that kind),
- the flush of dirty Assemblage assets on scene save (the player never saves),
- the viewport overlay call in `ViewportWindows.cpp` (that file ships in the
  player, but the editor scene viewport it draws does not run there).

The *declarations* of the authoring methods in `Engine.h` sit behind the same
guard. That is deliberate: it turns a stray call from runtime code into a compile
error at the call site rather than an unresolved symbol when the player links,
which is exactly how this was caught the first time.

`syncAssemblageProjectRoot` is **outside** the guard - the player resolves the
same project-relative asset paths when it loads a scene.
