// hello - the first Modularity GBA ROM: a SOUL you steer over a scrolling battle grid.
//
// It is written the way the GBA ModuCPP target will emit a script: per-frame logic on `fx`
// pixels, speeds authored in pixels per second, sprites written to the OAM shadow. Every
// second it logs how many CPU cycles the game logic used, to mGBA's log window.

#include "../runtime/modu_gba.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Art, packed into 4bpp tiles by the compiler (nothing of this runs on the GBA)
// ---------------------------------------------------------------------------------------------
struct Tiles16x16 {
    u32 words[32];  // four 8x8 tiles in 1D order: top-left, top-right, bottom-left, bottom-right
};

constexpr u32 PaletteIndex(char c) {
    return c == 'X' ? 1u : (c == 'o' ? 2u : 0u);
}

consteval Tiles16x16 PackSprite16(const char* const (&rows)[16]) {
    Tiles16x16 out{};
    for (int tile = 0; tile < 4; ++tile) {
        const int tileX = (tile & 1) * 8;
        const int tileY = (tile >> 1) * 8;
        for (int row = 0; row < 8; ++row) {
            u32 word = 0;
            for (int col = 0; col < 8; ++col) {
                word |= PaletteIndex(rows[tileY + row][tileX + col]) << (col * 4);
            }
            out.words[tile * 8 + row] = word;
        }
    }
    return out;
}

constexpr const char* kSoulArt[16] = {
    "................",
    "..XXXX....XXXX..",
    ".XooXXX..XXXXXX.",
    "XXoXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXX",
    ".XXXXXXXXXXXXXX.",
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    "....XXXXXXXX....",
    ".....XXXXXX.....",
    "......XXXX......",
    ".......XX.......",
    "................",
    "................",
};

constexpr Tiles16x16 kSoulTiles = PackSprite16(kSoulArt);

// Background: a 16 px grid built from four 8x8 tiles (empty, corner, top edge, left edge).
alignas(4) constexpr u32 kGridTiles[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0x11111111, 1, 1, 1, 1, 1, 1, 1},
    {0x11111111, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 1},
};

constexpr int kGridScreenBlock = 31;

// ---------------------------------------------------------------------------------------------
// Tuning - authored the way an inspector field is: pixels per second
// ---------------------------------------------------------------------------------------------
constexpr fx kWalkSpeed = PxPerSecond(60.0);
constexpr fx kRunSpeed = PxPerSecond(120.0);      // hold B
constexpr fx kGridDrift = PxPerSecond(15.0);      // 0.25 px per frame: one pixel every 4 frames
constexpr fx kDiagonal = Fx(0.70710678);          // 1 / sqrt(2), so diagonals aren't faster
constexpr fx kSoulMin = Px(0.0);
constexpr fx kSoulMaxX = Px(kScreenWidth - 16);
constexpr fx kSoulMaxY = Px(kScreenHeight - 16);
constexpr s32 kGridWrapMask = (16 << fx::kShift) - 1;  // the grid repeats every 16 px
constexpr s32 kLogInterval = Frames(1.0);

static_assert(kGridDrift.raw == 64, "15 px/s should be exactly 0.25 px/frame");

void LoadGraphics() {
    MEM_PAL_BG[0] = RGB15(0, 0, 0);
    MEM_PAL_BG[1] = RGB15(9, 2, 13);
    MEM_PAL_OBJ[1] = RGB15(31, 0, 0);
    MEM_PAL_OBJ[2] = RGB15(31, 22, 22);

    Dma3Copy32(MEM_VRAM, kGridTiles, sizeof(kGridTiles) / 4);
    Dma3Copy32(MEM_VRAM_OBJ, kSoulTiles.words, sizeof(kSoulTiles.words) / 4);

    volatile u16* map = MEM_VRAM + kGridScreenBlock * 0x400;
    static constexpr u16 kGridPattern[4] = {1, 2, 3, 0};
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            map[y * 32 + x] = kGridPattern[(y & 1) * 2 + (x & 1)];
        }
    }
    REG_BG0CNT = BG_CBB(0) | BG_SBB(kGridScreenBlock) | BG_4BPP | BG_REG_32x32;
}

}  // namespace

int main() {
    InitVBlankInterrupt();
    LoadGraphics();

    HideAllSprites();
    const Sprite soulSprite{0};
    oamShadow[0] = {0, ATTR1_SIZE_16, 0, 0};  // square 16x16, 4bpp, tile 0, palette 0
    PushSpritesToOam();
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    StartCycleCounter();
    const bool logging = debug::Open();
    debug::Line line;
    if (logging) {
        (line << "Modularity GBA runtime up. Walk " << static_cast<u32>(kWalkSpeed.raw)
              << "/256 px per frame, grid drift " << static_cast<u32>(kGridDrift.raw)
              << "/256").Send();
    }

    Vector2 soul{Px((kScreenWidth - 16) / 2), Px((kScreenHeight - 16) / 2)};
    Vector2 grid{};
    u32 worstLogicCycles = 0;
    s32 framesUntilLog = kLogInterval;

    while (true) {
        VBlankIntrWait();

        // VBlank: hand last frame's results to the hardware.
        PushSpritesToOam();
        REG_BG0HOFS = static_cast<u16>(grid.x.Floor());
        REG_BG0VOFS = static_cast<u16>(grid.y.Floor());

        // Game logic - what a transpiled TickUpdate() becomes.
        const u32 logicStart = CycleCount();

        input.Poll();
        const int dx = input.Axis(KEY_LEFT, KEY_RIGHT);
        const int dy = input.Axis(KEY_UP, KEY_DOWN);
        fx speed = input.Held(KEY_B) ? kRunSpeed : kWalkSpeed;
        if (dx != 0 && dy != 0) speed = speed * kDiagonal;
        soul.x = Clamp(soul.x + speed * dx, kSoulMin, kSoulMaxX);
        soul.y = Clamp(soul.y + speed * dy, kSoulMin, kSoulMaxY);
        soulSprite.SetPosition(soul);

        grid.x.raw = (grid.x + kGridDrift).raw & kGridWrapMask;
        grid.y.raw = (grid.y + kGridDrift).raw & kGridWrapMask;

        const u32 logicCycles = CycleCount() - logicStart;
        if (logicCycles > worstLogicCycles) worstLogicCycles = logicCycles;

        if (logging && --framesUntilLog <= 0) {
            framesUntilLog = kLogInterval;
            (line << "logic " << logicCycles << " cycles (worst " << worstLogicCycles
                  << ") of " << kCyclesPerFrame << " per frame; soul at "
                  << static_cast<u32>(soul.x.Floor()) << "," << static_cast<u32>(soul.y.Floor()))
                .Send();
        }
    }
}
