#pragma once
// [3DS simulation] The few libctru types and calls the 3DS texture code
// uses, for running it on a PC (Makefile.n3dssim).
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
#define SYSCLOCK_ARM11 268111856LL
size_t linearSpaceFree();
