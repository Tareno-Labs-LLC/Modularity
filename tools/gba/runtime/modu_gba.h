#pragma once
// modu_gba.h - the entire runtime a Modularity GBA project links against.
//
// Freestanding by design: no libc, no STL, no heap, no exceptions, and no floating point at
// runtime. Everything here is either an inline accessor that compiles to a load/store on a
// hardware register, or a value the compiler folds before the ROM is built. The rule this file
// exists to keep: a script line costs what the same hand-written ARM would.
//
// Numbers: positions and speeds are `fx`, a plain 32-bit int counted in 1/256ths of a pixel.
// Decimal values only ever appear in `consteval` helpers (Px, PxPerSecond, Frames), which the
// compiler evaluates while building - a decimal can never reach the GBA, so there is no FPU to
// miss. Speeds are authored in pixels per second and become a per-frame step at compile time.

using u8 = unsigned char;
using s8 = signed char;
using u16 = unsigned short;
using s16 = signed short;
using u32 = unsigned int;
using s32 = signed int;

// ---------------------------------------------------------------------------------------------
// Memory map and registers
// ---------------------------------------------------------------------------------------------
#define MODU_REG16(addr) (*reinterpret_cast<volatile u16*>(addr))
#define MODU_REG32(addr) (*reinterpret_cast<volatile u32*>(addr))

#define REG_DISPCNT   MODU_REG16(0x04000000)
#define REG_DISPSTAT  MODU_REG16(0x04000004)
#define REG_VCOUNT    MODU_REG16(0x04000006)
#define REG_BG0CNT    MODU_REG16(0x04000008)
#define REG_BG0HOFS   MODU_REG16(0x04000010)  // write-only
#define REG_BG0VOFS   MODU_REG16(0x04000012)  // write-only
#define REG_KEYINPUT  MODU_REG16(0x04000130)  // active low
#define REG_IE        MODU_REG16(0x04000200)
#define REG_IF        MODU_REG16(0x04000202)
#define REG_IME       MODU_REG16(0x04000208)

#define REG_DMA3SAD   MODU_REG32(0x040000D4)
#define REG_DMA3DAD   MODU_REG32(0x040000D8)
#define REG_DMA3CNT   MODU_REG32(0x040000DC)

#define REG_TM2CNT_L  MODU_REG16(0x04000108)
#define REG_TM2CNT_H  MODU_REG16(0x0400010A)
#define REG_TM3CNT_L  MODU_REG16(0x0400010C)
#define REG_TM3CNT_H  MODU_REG16(0x0400010E)

// The BIOS IRQ dispatcher jumps through this word; IntrWait polls the flags word below it.
#define MODU_BIOS_IRQ_VECTOR MODU_REG32(0x03007FFC)

#define MEM_PAL_BG   (reinterpret_cast<volatile u16*>(0x05000000))
#define MEM_PAL_OBJ  (reinterpret_cast<volatile u16*>(0x05000200))
#define MEM_VRAM     (reinterpret_cast<volatile u16*>(0x06000000))
#define MEM_VRAM_OBJ (reinterpret_cast<volatile u32*>(0x06010000))
#define MEM_OAM      (reinterpret_cast<volatile u32*>(0x07000000))

// DISPCNT
constexpr u16 DCNT_MODE0  = 0x0000;
constexpr u16 DCNT_OBJ_1D = 0x0040;
constexpr u16 DCNT_BG0    = 0x0100;
constexpr u16 DCNT_OBJ    = 0x1000;
// DISPSTAT / IE
constexpr u16 DSTAT_VBL_IRQ = 0x0008;
constexpr u16 IRQ_VBLANK    = 0x0001;
// BGxCNT
constexpr u16 BG_CBB(u16 n) { return static_cast<u16>(n << 2); }
constexpr u16 BG_SBB(u16 n) { return static_cast<u16>(n << 8); }
constexpr u16 BG_4BPP       = 0x0000;
constexpr u16 BG_REG_32x32  = 0x0000;
// Timers
constexpr u16 TM_ENABLE  = 0x0080;
constexpr u16 TM_CASCADE = 0x0004;

constexpr u16 RGB15(u16 r, u16 g, u16 b) { return static_cast<u16>(r | (g << 5) | (b << 10)); }

// One frame is 228 scanlines x 1232 cycles of the 16.78 MHz CPU.
constexpr u32 kCyclesPerFrame = 280896;
constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 160;

// ---------------------------------------------------------------------------------------------
// fx - pixels with 8 fraction bits. Same instructions as a plain int: add is one `adds`, turning
// it into a whole pixel for the hardware is one `asrs #8`.
// ---------------------------------------------------------------------------------------------
struct fx {
    s32 raw = 0;

    static constexpr int kShift = 8;
    static constexpr s32 kOne = 1 << kShift;

    static constexpr fx FromRaw(s32 r) { fx v; v.raw = r; return v; }
    static constexpr fx FromInt(s32 i) { return FromRaw(i << kShift); }

    constexpr s32 Floor() const { return raw >> kShift; }
    constexpr s32 Round() const { return (raw + (kOne >> 1)) >> kShift; }

    constexpr fx operator+(fx o) const { return FromRaw(raw + o.raw); }
    constexpr fx operator-(fx o) const { return FromRaw(raw - o.raw); }
    constexpr fx operator-() const { return FromRaw(-raw); }
    constexpr fx operator*(s32 i) const { return FromRaw(raw * i); }
    // 32-bit multiply (Thumb has no 64-bit one): keep |a * b| under 32768 px^2, which covers
    // speed x factor math. Position x position is not what this is for.
    constexpr fx operator*(fx o) const { return FromRaw((raw * o.raw) >> kShift); }
    constexpr fx& operator+=(fx o) { raw += o.raw; return *this; }
    constexpr fx& operator-=(fx o) { raw -= o.raw; return *this; }

    constexpr bool operator==(fx o) const { return raw == o.raw; }
    constexpr bool operator<(fx o) const { return raw < o.raw; }
    constexpr bool operator>(fx o) const { return raw > o.raw; }
    constexpr bool operator<=(fx o) const { return raw <= o.raw; }
    constexpr bool operator>=(fx o) const { return raw >= o.raw; }
};

constexpr fx Clamp(fx v, fx lo, fx hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The GBA refreshes at 16777216 / 280896 = 59.7275 Hz, not 60.
consteval double FramesPerSecond() { return 16777216.0 / 280896.0; }
consteval s32 RoundToInt(double v) { return static_cast<s32>(v >= 0.0 ? v + 0.5 : v - 0.5); }

// Compile-time only: a decimal written in a script becomes its 1/256-pixel int here.
consteval fx Px(double pixels) { return fx::FromRaw(RoundToInt(pixels * fx::kOne)); }
consteval fx Fx(double scalar) { return Px(scalar); }
// Speeds are authored in pixels per second; the per-frame step is worked out here, once.
consteval fx PxPerSecond(double pixelsPerSecond) { return Px(pixelsPerSecond / FramesPerSecond()); }
consteval s32 Frames(double seconds) { return RoundToInt(seconds * FramesPerSecond()); }

struct Vector2 {
    fx x, y;
};

// ---------------------------------------------------------------------------------------------
// BIOS calls
// ---------------------------------------------------------------------------------------------
#if defined(__thumb__)
#define MODU_SWI(n) "swi " #n
#else
#define MODU_SWI(n) "swi " #n " << 16"
#endif

// Sleeps the CPU until the next VBlank interrupt; the cheapest way to wait for a frame.
inline void VBlankIntrWait() {
    asm volatile(MODU_SWI(0x05) ::: "r0", "r1", "r2", "r3", "memory");
}

// ---------------------------------------------------------------------------------------------
// DMA - VRAM and OAM ignore 8-bit writes, so bulk copies go through here, never memcpy.
// ---------------------------------------------------------------------------------------------
inline void Dma3Copy32(volatile void* dst, const void* src, u32 words) {
    REG_DMA3SAD = reinterpret_cast<u32>(src);
    REG_DMA3DAD = reinterpret_cast<u32>(dst);
    REG_DMA3CNT = words | 0x84000000u;  // enable, 32-bit units
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------
enum Key : u16 {
    KEY_A = 0x0001, KEY_B = 0x0002, KEY_SELECT = 0x0004, KEY_START = 0x0008,
    KEY_RIGHT = 0x0010, KEY_LEFT = 0x0020, KEY_UP = 0x0040, KEY_DOWN = 0x0080,
    KEY_R = 0x0100, KEY_L = 0x0200,
};

struct InputState {
    u16 now = 0;
    u16 prev = 0;

    void Poll() { prev = now; now = static_cast<u16>(~REG_KEYINPUT & 0x03FF); }
    bool Held(Key k) const { return (now & k) != 0; }
    bool Pressed(Key k) const { return (now & ~prev & k) != 0; }
    // -1, 0 or +1 - a d-pad axis as a step direction.
    int Axis(Key negative, Key positive) const { return Held(positive) - Held(negative); }
};

inline InputState input;

// ---------------------------------------------------------------------------------------------
// Sprites - scripts write a shadow copy during the frame; it reaches OAM by DMA in VBlank.
// ---------------------------------------------------------------------------------------------
struct ObjAttr {
    u16 attr0, attr1, attr2, affine;
};

constexpr u16 ATTR0_HIDE = 0x0200;
constexpr u16 ATTR1_SIZE_16 = 0x4000;  // with the square shape (attr0 bits 14-15 = 0)

alignas(4) inline ObjAttr oamShadow[128];

inline void HideAllSprites() {
    for (ObjAttr& o : oamShadow) o.attr0 = ATTR0_HIDE;
}

inline void PushSpritesToOam() {
    Dma3Copy32(MEM_OAM, oamShadow, sizeof(oamShadow) / 4);
}

struct Sprite {
    u8 id;

    void SetPosition(s32 x, s32 y) const {
        ObjAttr& o = oamShadow[id];
        o.attr0 = static_cast<u16>((o.attr0 & ~0x00FF) | (y & 0x00FF));
        o.attr1 = static_cast<u16>((o.attr1 & ~0x01FF) | (x & 0x01FF));
    }
    void SetPosition(const Vector2& p) const { SetPosition(p.x.Floor(), p.y.Floor()); }
};

// ---------------------------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------------------------
extern "C" void modu_irq_handler();  // ARM, in IWRAM (gba_crt0.s)

inline void InitVBlankInterrupt() {
    REG_IME = 0;
    MODU_BIOS_IRQ_VECTOR = reinterpret_cast<u32>(&modu_irq_handler);
    REG_DISPSTAT = DSTAT_VBL_IRQ;
    REG_IE = IRQ_VBLANK;
    REG_IF = 0xFFFF;  // drop anything stale
    REG_IME = 1;
}

// ---------------------------------------------------------------------------------------------
// Profiling - timers 2+3 cascaded into a free-running 32-bit CPU cycle counter.
// ---------------------------------------------------------------------------------------------
inline void StartCycleCounter() {
    REG_TM2CNT_H = 0;
    REG_TM3CNT_H = 0;
    REG_TM2CNT_L = 0;
    REG_TM3CNT_L = 0;
    REG_TM3CNT_H = TM_ENABLE | TM_CASCADE;
    REG_TM2CNT_H = TM_ENABLE;  // prescaler 1: counts every CPU cycle
}

inline u32 CycleCount() {
    u16 hi, lo;
    do {
        hi = REG_TM3CNT_L;
        lo = REG_TM2CNT_L;
    } while (hi != REG_TM3CNT_L);
    return (static_cast<u32>(hi) << 16) | lo;
}

// ---------------------------------------------------------------------------------------------
// mGBA debug log (Tools > View Logs). Real hardware ignores these addresses.
// ---------------------------------------------------------------------------------------------
namespace debug {

#define REG_DEBUG_ENABLE MODU_REG16(0x04FFF780)
#define REG_DEBUG_FLAGS  MODU_REG16(0x04FFF700)
#define MEM_DEBUG_STRING (reinterpret_cast<volatile char*>(0x04FFF600))

inline bool Open() {
    REG_DEBUG_ENABLE = 0xC0DE;
    return REG_DEBUG_ENABLE == 0x1DEA;
}

// A tiny line builder. Numbers are printed by subtracting powers of ten - the GBA has no divide
// instruction, and this keeps the log from pulling one in.
struct Line {
    char text[128];
    int length = 0;

    Line& operator<<(const char* s) {
        while (*s && length < 127) text[length++] = *s++;
        return *this;
    }
    Line& operator<<(u32 v) {
        static constexpr u32 kPowers[] = {1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
                                          10000u, 1000u, 100u, 10u, 1u};
        bool started = false;
        for (u32 p : kPowers) {
            char digit = '0';
            while (v >= p) { v -= p; ++digit; }
            if (digit != '0' || started || p == 1u) {
                started = true;
                if (length < 127) text[length++] = digit;
            }
        }
        return *this;
    }
    void Send() {
        int i = 0;
        for (; i < length; ++i) MEM_DEBUG_STRING[i] = text[i];
        MEM_DEBUG_STRING[i] = 0;
        REG_DEBUG_FLAGS = 0x100 | 3;  // INFO
        length = 0;
    }
};

}  // namespace debug
