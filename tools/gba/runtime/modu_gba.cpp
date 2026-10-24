// modu_gba.cpp - the few out-of-line functions the compiler itself may call.
//
// GCC can turn a struct copy or a zeroing loop into a call to memcpy/memset even in freestanding
// code, and there is no libc to provide them. These are only for RAM; VRAM and OAM drop 8-bit
// writes, so copies there go through Dma3Copy32.

#include "modu_gba.h"

using size_t = decltype(sizeof(0));

// The attribute stops GCC from "optimising" these loops back into calls to themselves.
#define MODU_NO_LOOP_TO_CALL __attribute__((optimize("no-tree-loop-distribute-patterns")))

extern "C" {

MODU_NO_LOOP_TO_CALL void* memcpy(void* dst, const void* src, size_t n) {
    u8* d = static_cast<u8*>(dst);
    const u8* s = static_cast<const u8*>(src);
    while (n--) *d++ = *s++;
    return dst;
}

MODU_NO_LOOP_TO_CALL void* memmove(void* dst, const void* src, size_t n) {
    u8* d = static_cast<u8*>(dst);
    const u8* s = static_cast<const u8*>(src);
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}

MODU_NO_LOOP_TO_CALL void* memset(void* dst, int value, size_t n) {
    u8* d = static_cast<u8*>(dst);
    while (n--) *d++ = static_cast<u8>(value);
    return dst;
}

}  // extern "C"
