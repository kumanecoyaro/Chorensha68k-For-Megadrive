#ifndef TYPES_H
#define TYPES_H

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed long s32;
typedef unsigned long u32;

#define NULL ((void *)0)

/* 68000 16x16->32 signed multiply (avoids libgcc __mulsi3) */
static inline s32 muls(s16 a, s16 b)
{
    s32 r = a;
    __asm__("muls.w %1,%0" : "+d"(r) : "dmi"(b));
    return r;
}

static inline u32 mulu(u16 a, u16 b)
{
    u32 r = a;
    __asm__("mulu.w %1,%0" : "+d"(r) : "dmi"(b));
    return r;
}

/* 68000 32/16 signed divide, quotient only (avoids libgcc __divsi3) */
static inline s16 divs(s32 a, s16 b)
{
    __asm__("divs.w %1,%0" : "+d"(a) : "dmi"(b));
    return (s16)a;
}

#endif
