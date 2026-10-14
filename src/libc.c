// the few libc functions clang may emit calls to on its own (zeroing or
// copying a big local struct). the mod links without a libc.

#include <ultra64.h>

void *memset(void *dst, int c, unsigned int n) {
    volatile u8 *d = dst; // volatile: keeps clang from turning the loop back into a call to itself
    while (n--) *d++ = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n) {
    volatile u8 *d = dst; // volatile: keeps clang from turning the loop back into a call to itself
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
