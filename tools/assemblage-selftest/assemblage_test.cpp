// Self-test for the Assemblage core: cell addressing, chunking, the RLE codec,
// animation resolution, and both asset formats.
//
// The core is Engine-free by design (it depends only on glm and the standard
// library), so unlike the ModuOBJ self-test this one compiles Assemblage.cpp
// directly and needs no engine build, no window, and no GL context.
//
// Build/run: tools/assemblage-selftest/run.sh   (or run.ps1 on Windows)
#include "Assemblage.h"

#include <cstdio>
#include <sstream>

namespace fsys = std::filesystem;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(cond)) {                                                          \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);       \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static fsys::path gTemp;

using namespace Assemblage;

// ---------------------------------------------------------------------------

static void TestCellPacking() {
    const Cell cell = MakeCell(1234, CellFlag_FlipX | CellFlag_Rot90);
    CHECK(CellTile(cell) == 1234, "packed tile id round-trips");
    CHECK((CellFlagsOf(cell) & CellFlag_FlipX) != 0, "flipX flag survives packing");
    CHECK((CellFlagsOf(cell) & CellFlag_Rot90) != 0, "rot90 flag survives packing");
    CHECK((CellFlagsOf(cell) & CellFlag_FlipY) == 0, "unset flag stays unset");
    CHECK(!CellIsEmpty(cell), "a packed tile is not empty");
    CHECK(CellIsEmpty(MakeCell(kEmptyTile, CellFlag_FlipX)),
          "tile id 0 is empty regardless of flags");

    // The id field must survive right up to its 24-bit limit.
    const Cell big = MakeCell(kTileIdMask, CellFlag_None);
    CHECK(CellTile(big) == kTileIdMask, "maximum tile id round-trips");
}

static void TestChunkAddressing() {
    const int size = 32;

    CHECK(CellToChunk(0, 0, size).x == 0 && CellToChunk(0, 0, size).y == 0,
          "origin cell maps to chunk (0,0)");
    CHECK(CellToChunk(31, 31, size).x == 0, "last cell of a chunk stays in it");
    CHECK(CellToChunk(32, 0, size).x == 1, "first cell past the edge advances the chunk");

    // Negative coordinates are the classic off-by-one: plain integer division
    // maps both -1 and 0 to chunk 0, which silently aliases two cells.
    CHECK(CellToChunk(-1, -1, size).x == -1 && CellToChunk(-1, -1, size).y == -1,
          "cell -1 lands in chunk -1, not chunk 0");
    CHECK(CellToChunk(-32, 0, size).x == -1, "cell -32 is the first cell of chunk -1");
    CHECK(CellToChunk(-33, 0, size).x == -2, "cell -33 crosses into chunk -2");

    CHECK(CellIndexInChunk(0, 0, size) == 0, "origin index is 0");
    CHECK(CellIndexInChunk(31, 31, size) == size * size - 1, "last index is in range");
    CHECK(CellIndexInChunk(-1, -1, size) == size * size - 1,
          "cell -1 is the last cell of its chunk");
    CHECK(CellIndexInChunk(-32, -32, size) == 0, "cell -32 is the first cell of its chunk");
}

static Asset MakeAsset() {
    Asset asset;
    asset.assemblageId = GenerateAssemblageId();
    asset.name = "Cabin";
    asset.chunkSize = 32;
    asset.cellSize = glm::vec2(1.0f, 1.0f);
    asset.origin = glm::vec2(-4.0f, 2.0f);
    asset.tilesetPath = "Assets/Tiles/Cabin.modutile";
    asset.addLayer("Ground");
    asset.addLayer("Walls");
    return asset;
}

static void TestCellReadWrite() {
    Asset asset = MakeAsset();
    const int ground = asset.layers[0].id;
    const int walls = asset.layers[1].id;
    CHECK(ground != walls, "layers get distinct ids");

    CHECK(GetCell(asset, ground, 5, 5) == 0, "unwritten cell reads empty");
    CHECK(asset.findLayer(ground)->chunks.empty(),
          "reading a missing cell allocates nothing");

    ChunkCoord dirty;
    CHECK(SetCell(asset, ground, 5, 5, MakeCell(7), &dirty), "first write reports a change");
    CHECK(dirty.x == 0 && dirty.y == 0, "write reports the chunk that needs a rebuild");
    CHECK(CellTile(GetCell(asset, ground, 5, 5)) == 7, "written cell reads back");
    CHECK(GetCell(asset, walls, 5, 5) == 0, "layers do not share cells");

    CHECK(!SetCell(asset, ground, 5, 5, MakeCell(7)), "rewriting the same value reports no change");

    // Clearing a cell that was never set must not allocate a chunk.
    const size_t before = asset.findLayer(ground)->chunks.size();
    CHECK(!SetCell(asset, ground, 900, 900, MakeCell(kEmptyTile)),
          "clearing an unset cell reports no change");
    CHECK(asset.findLayer(ground)->chunks.size() == before,
          "clearing an unset cell allocates no chunk");

    // Erasing the last cell of a chunk must reclaim the chunk, or a map that is
    // painted then erased keeps paying to serialize and rebuild empty chunks.
    CHECK(SetCell(asset, ground, 5, 5, MakeCell(kEmptyTile)), "clearing reports a change");
    CHECK(asset.findLayer(ground)->chunks.empty(), "emptied chunk is reclaimed");

    CHECK(!SetCell(asset, 9999, 0, 0, MakeCell(1)), "writing to an unknown layer fails");

    // Negative coordinates must work end to end, not just in the addressing math.
    CHECK(SetCell(asset, ground, -1, -1, MakeCell(3)), "negative cell writes");
    CHECK(CellTile(GetCell(asset, ground, -1, -1)) == 3, "negative cell reads back");
    CHECK(CellTile(GetCell(asset, ground, 0, 0)) == 0, "negative cell does not alias cell 0");
}

static void TestWorldMapping() {
    Asset asset = MakeAsset();
    asset.cellSize = glm::vec2(2.0f, 2.0f);
    asset.origin = glm::vec2(10.0f, -6.0f);

    const glm::vec2 world = CellToWorld(asset, 3, 4);
    CHECK(world.x == 16.0f && world.y == 2.0f, "cell maps to the expected world point");

    int cx = 0, cy = 0;
    WorldToCell(asset, world, cx, cy);
    CHECK(cx == 3 && cy == 4, "world point maps back to its cell");

    // A point anywhere inside a cell must resolve to that cell, not the next one.
    WorldToCell(asset, world + glm::vec2(1.9f, 1.9f), cx, cy);
    CHECK(cx == 3 && cy == 4, "a point inside the cell resolves to that cell");

    WorldToCell(asset, asset.origin - glm::vec2(0.1f, 0.1f), cx, cy);
    CHECK(cx == -1 && cy == -1, "a point just below origin resolves to cell -1");
}

static void TestAnimationIsShared() {
    TileDef tile;
    tile.id = 1;
    CHECK(ResolveAnimationFrame(tile, 12.5) == 0, "a still tile always resolves frame 0");

    tile.animation.fps = 10.0f;
    tile.animation.frames.resize(4);
    tile.animation.mode = TileAnimationMode::Loop;

    CHECK(ResolveAnimationFrame(tile, 0.0) == 0, "loop starts at frame 0");
    CHECK(ResolveAnimationFrame(tile, 0.25) == 2, "loop advances with the clock");
    CHECK(ResolveAnimationFrame(tile, 0.45) == 0, "loop wraps");

    // The point of putting animation on the definition: the frame is a pure
    // function of (tile, time), so every cell referencing this tile agrees
    // without any per-cell state to store or update.
    CHECK(ResolveAnimationFrame(tile, 3.14) == ResolveAnimationFrame(tile, 3.14),
          "frame resolution is deterministic for a given time");

    tile.animation.mode = TileAnimationMode::PingPong;
    CHECK(ResolveAnimationFrame(tile, 0.0) == 0, "pingpong starts at frame 0");
    CHECK(ResolveAnimationFrame(tile, 0.35) == 3, "pingpong reaches the last frame");
    CHECK(ResolveAnimationFrame(tile, 0.45) == 2, "pingpong reverses");
    CHECK(ResolveAnimationFrame(tile, 0.65) == 0, "pingpong returns to frame 0");

    tile.animation.mode = TileAnimationMode::Once;
    CHECK(ResolveAnimationFrame(tile, 100.0) == 3, "once clamps to the last frame");
}

static void TestAssetRoundTrip() {
    Asset asset = MakeAsset();
    const int ground = asset.layers[0].id;
    asset.layers[1].sortingOrder = 40;
    asset.layers[1].locked = true;
    asset.layers[1].opacity = 0.5f;
    asset.layers[1].visible = false;

    for (int x = 0; x < 40; ++x) {
        CHECK(SetCell(asset, ground, x, 0, MakeCell(static_cast<TileId>(x + 1))) || x < 0,
              "bulk write succeeds");
    }
    CHECK(SetCell(asset, ground, -5, -5, MakeCell(99, CellFlag_FlipY)), "negative bulk write");
    CHECK(asset.findLayer(ground)->chunks.size() == 3,
          "cells spanning a chunk edge occupy three chunks");

    std::string error;
    std::ostringstream out;
    CHECK(WriteAssetStream(out, asset, error), "asset serializes");

    Asset loaded;
    std::istringstream in(out.str());
    CHECK(ReadAssetStream(in, loaded, error), error.empty() ? "asset parses" : error.c_str());

    CHECK(loaded.assemblageId == asset.assemblageId, "assemblage id round-trips");
    CHECK(loaded.kind == asset.kind, "kind round-trips");
    CHECK(loaded.chunkSize == asset.chunkSize, "chunk size round-trips");
    CHECK(loaded.origin == asset.origin, "origin round-trips");
    CHECK(loaded.tilesetPath == asset.tilesetPath, "tileset reference round-trips");
    CHECK(loaded.layers.size() == 2, "layer count round-trips");
    CHECK(loaded.layers[1].sortingOrder == 40, "layer sorting order round-trips");
    CHECK(loaded.layers[1].locked, "layer locked flag round-trips");
    CHECK(!loaded.layers[1].visible, "layer visibility round-trips");

    for (int x = 0; x < 40; ++x) {
        CHECK(CellTile(GetCell(loaded, ground, x, 0)) == static_cast<TileId>(x + 1),
              "every cell round-trips");
    }
    const Cell negative = GetCell(loaded, ground, -5, -5);
    CHECK(CellTile(negative) == 99, "negative cell round-trips");
    CHECK((CellFlagsOf(negative) & CellFlag_FlipY) != 0, "cell flags round-trip");

    // Empty layers must not resurrect chunks.
    CHECK(loaded.findLayer(asset.layers[1].id)->chunks.empty(),
          "an empty layer loads with no chunks");
}

static void TestRleCompactsEmptyAndUniform() {
    Asset asset = MakeAsset();
    const int ground = asset.layers[0].id;

    // A fully uniform chunk must collapse to a single run, or a large map's file
    // grows with its area instead of its content.
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            SetCell(asset, ground, x, y, MakeCell(5));
        }
    }
    std::string error;
    std::ostringstream out;
    CHECK(WriteAssetStream(out, asset, error), "uniform asset serializes");
    const std::string text = out.str();
    CHECK(text.find("cells=1024*5;") != std::string::npos,
          "a uniform chunk collapses to one run");

    Asset loaded;
    std::istringstream in(text);
    CHECK(ReadAssetStream(in, loaded, error), "uniform asset parses");
    CHECK(CellTile(GetCell(loaded, ground, 17, 21)) == 5, "uniform chunk round-trips");
}

static void TestAssetRejections() {
    std::string error;

    // A newer format version must be refused outright rather than half-parsed.
    {
        Asset asset = MakeAsset();
        std::ostringstream out;
        CHECK(WriteAssetStream(out, asset, error), "asset serializes for version test");
        std::string text = out.str();
        const size_t at = text.find("formatVersion=1;");
        CHECK(at != std::string::npos, "found the version line");
        text.replace(at, std::string("formatVersion=1;").size(), "formatVersion=99;");

        Asset loaded;
        std::istringstream in(text);
        CHECK(!ReadAssetStream(in, loaded, error), "a newer formatVersion is rejected");
        CHECK(error.find("newer than this engine supports") != std::string::npos,
              "the version error explains itself");
    }

    // An unknown kind is what a future structured-authoring workflow would use;
    // this engine must say so instead of misreading it as a 2D grid.
    {
        Asset asset = MakeAsset();
        std::ostringstream out;
        CHECK(WriteAssetStream(out, asset, error), "asset serializes for kind test");
        std::string text = out.str();
        const size_t at = text.find("kind=\"grid2d\";");
        CHECK(at != std::string::npos, "found the kind line");
        text.replace(at, std::string("kind=\"grid2d\";").size(), "kind=\"voxel3d\";");

        Asset loaded;
        std::istringstream in(text);
        CHECK(!ReadAssetStream(in, loaded, error), "an unknown kind is rejected");
        CHECK(error.find("voxel3d") != std::string::npos, "the kind error names the kind");
    }

    // Not an assemblage at all.
    {
        Asset loaded;
        std::istringstream in("MODU_SCENESETTINGS{\n    version=29;\n}\n");
        CHECK(!ReadAssetStream(in, loaded, error), "a non-assemblage file is rejected");
        CHECK(error.find("MODU_ASSEMBLAGE") != std::string::npos,
              "the error names the missing header");
    }

    // A chunk pointing at a layer that does not exist would silently vanish.
    {
        std::ostringstream text;
        text << "MODU_ASSEMBLAGE {\n"
             << "    formatVersion=1;\n"
             << "    assemblageId=\"" << GenerateAssemblageId() << "\";\n"
             << "    kind=\"grid2d\";\n"
             << "    chunkSize=32;\n"
             << "}\n\n"
             << "MODU_ASMCHUNK = (layer=7, coord=0,0) {\n"
             << "    encoding=\"rle32\";\n"
             << "    cells=1024*1;\n"
             << "}\n";
        Asset loaded;
        std::istringstream in(text.str());
        CHECK(!ReadAssetStream(in, loaded, error), "a chunk on an unknown layer is rejected");
    }
}

static void TestUnknownBlocksAreSkipped() {
    // A block this engine does not know must be stepped over by brace depth
    // rather than derailing the parse of everything after it.
    std::ostringstream text;
    const std::string id = GenerateAssemblageId();
    text << "MODU_ASSEMBLAGE {\n"
         << "    formatVersion=1;\n"
         << "    assemblageId=\"" << id << "\";\n"
         << "    kind=\"grid2d\";\n"
         << "    chunkSize=32;\n"
         << "}\n\n"
         << "MODU_ASMFUTURE = (id=3) {\n"
         << "    something=1;\n"
         << "}\n\n"
         << "MODU_ASMLAYER = (id=1, name=\"Ground\") {\n"
         << "    kind=\"tile\";\n"
         << "}\n\n"
         << "MODU_ASMCHUNK = (layer=1, coord=0,0) {\n"
         << "    encoding=\"rle32\";\n"
         << "    cells=1024*4;\n"
         << "}\n";

    Asset loaded;
    std::string error;
    std::istringstream in(text.str());
    CHECK(ReadAssetStream(in, loaded, error), error.empty() ? "parses past an unknown block" : error.c_str());
    CHECK(loaded.layers.size() == 1, "the layer after an unknown block still loads");
    CHECK(CellTile(GetCell(loaded, 1, 0, 0)) == 4, "the chunk after an unknown block still loads");
}

static void TestTilesetRoundTrip() {
    Tileset set;
    set.tilesetId = GenerateTilesetId();
    set.name = "Cabin";

    TileDef grass;
    grass.id = 1;
    grass.name = "Grass";
    grass.sheetPath = "Assets/Tiles/cabin.png";
    grass.frame.rect = glm::ivec4(0, 0, 16, 16);
    grass.tags = {"ground", "walkable"};
    grass.collision = TileCollision::Box;
    grass.collisionSize = glm::vec2(1.0f, 1.0f);
    grass.meta["durability"] = "3";
    set.tiles.push_back(grass);

    TileDef water;
    water.id = 2;
    water.name = "Water";
    water.sheetPath = "Assets/Tiles/cabin.png";
    water.frame.rect = glm::ivec4(16, 0, 16, 16);
    water.animation.fps = 8.0f;
    water.animation.mode = TileAnimationMode::PingPong;
    water.animation.frames = {TileFrame{glm::ivec4(16, 0, 16, 16)},
                              TileFrame{glm::ivec4(32, 0, 16, 16)},
                              TileFrame{glm::ivec4(48, 0, 16, 16)}};
    water.autoRuleSet = "blob";
    water.variants = {3, 4};
    set.tiles.push_back(water);

    std::string error;
    std::ostringstream out;
    CHECK(WriteTilesetStream(out, set, error), "tileset serializes");

    Tileset loaded;
    std::istringstream in(out.str());
    CHECK(ReadTilesetStream(in, loaded, error), error.empty() ? "tileset parses" : error.c_str());

    CHECK(loaded.tilesetId == set.tilesetId, "tileset id round-trips");
    CHECK(loaded.tiles.size() == 2, "tile count round-trips");
    CHECK(loaded.nextTileId == 3, "nextTileId is derived from the highest tile id");

    const TileDef* g = loaded.find(1);
    CHECK(g != nullptr, "tile 1 is found by id");
    if (g) {
        CHECK(g->name == "Grass", "tile name round-trips");
        CHECK(g->frame.rect == glm::ivec4(0, 0, 16, 16), "tile rect round-trips");
        CHECK(g->tags.size() == 2 && g->tags[1] == "walkable", "tile tags round-trip");
        CHECK(g->collision == TileCollision::Box, "tile collision round-trips");
        CHECK(g->meta.count("durability") == 1 && g->meta.at("durability") == "3",
              "custom metadata round-trips");
    }

    const TileDef* w = loaded.find(2);
    CHECK(w != nullptr, "tile 2 is found by id");
    if (w) {
        CHECK(w->animation.animated(), "animated tile reports itself animated");
        CHECK(w->animation.frames.size() == 3, "animation frames round-trip");
        CHECK(w->animation.mode == TileAnimationMode::PingPong, "animation mode round-trips");
        CHECK(w->animation.frames[2].rect == glm::ivec4(48, 0, 16, 16),
              "the last animation frame round-trips");
        CHECK(w->autoRuleSet == "blob", "auto rule set round-trips");
        CHECK(w->variants.size() == 2 && w->variants[1] == 4, "variants round-trip");
    }
}

static void TestTilesetRejections() {
    std::string error;

    // Tile id 0 means "empty cell", so a tile may never claim it.
    {
        std::ostringstream text;
        text << "MODU_TILESET {\n    formatVersion=1;\n    tilesetId=\""
             << GenerateTilesetId() << "\";\n}\n\n"
             << "MODU_TILE = (id=0, name=\"Bad\") {\n    sheet=\"x.png\";\n}\n";
        Tileset loaded;
        std::istringstream in(text.str());
        CHECK(!ReadTilesetStream(in, loaded, error), "tile id 0 is rejected");
    }

    // Duplicate ids would make every cell referencing them ambiguous.
    {
        std::ostringstream text;
        text << "MODU_TILESET {\n    formatVersion=1;\n    tilesetId=\""
             << GenerateTilesetId() << "\";\n}\n\n"
             << "MODU_TILE = (id=1, name=\"A\") {\n    sheet=\"x.png\";\n}\n\n"
             << "MODU_TILE = (id=1, name=\"B\") {\n    sheet=\"y.png\";\n}\n";
        Tileset loaded;
        std::istringstream in(text.str());
        CHECK(!ReadTilesetStream(in, loaded, error), "duplicate tile ids are rejected");
        CHECK(error.find("duplicate") != std::string::npos, "the error says duplicate");
    }
}

static void TestFileIO() {
    Asset asset = MakeAsset();
    SetCell(asset, asset.layers[0].id, 2, 3, MakeCell(11));

    const fsys::path assetPath = gTemp / "Cabin.moduasm";
    std::string error;
    CHECK(SaveAsset(assetPath, asset, error), error.empty() ? "asset saves" : error.c_str());
    CHECK(fsys::exists(assetPath), "asset file exists");
    CHECK(!fsys::exists(fsys::path(assetPath.string() + ".tmp")),
          "the temporary file is cleaned up");

    Asset loaded;
    CHECK(LoadAsset(assetPath, loaded, error), error.empty() ? "asset loads" : error.c_str());
    CHECK(CellTile(GetCell(loaded, asset.layers[0].id, 2, 3)) == 11, "cell survives a file round-trip");

    // A refused save must leave the previous good file untouched.
    Asset broken = asset;
    broken.assemblageId.clear();
    CHECK(!SaveAsset(assetPath, broken, error), "an invalid asset refuses to save");
    Asset stillThere;
    CHECK(LoadAsset(assetPath, stillThere, error), "the previous asset survives a refused save");
    CHECK(CellTile(GetCell(stillThere, asset.layers[0].id, 2, 3)) == 11,
          "the previous asset still has its cells");

    // The cache must reload after the file changes on disk.
    AssetCache cache = MakeAssetCache();
    const Asset* first = cache.get(assetPath, error);
    CHECK(first != nullptr, "cache loads the asset");
    const Asset* second = cache.get(assetPath, error);
    CHECK(second != nullptr && cache.size() == 1, "a second get does not add an entry");

    CHECK(!LoadAsset(gTemp / "missing.moduasm", loaded, error), "a missing file fails to load");
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: assemblage_test <temp-dir>\n");
        return 2;
    }
    gTemp = fsys::path(argv[1]);
    std::error_code ec;
    fsys::create_directories(gTemp, ec);

    TestCellPacking();
    TestChunkAddressing();
    TestCellReadWrite();
    TestWorldMapping();
    TestAnimationIsShared();
    TestAssetRoundTrip();
    TestRleCompactsEmptyAndUniform();
    TestAssetRejections();
    TestUnknownBlocksAreSkipped();
    TestTilesetRoundTrip();
    TestTilesetRejections();
    TestFileIO();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
