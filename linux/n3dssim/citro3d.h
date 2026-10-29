#pragma once
// [3DS simulation] citro3d's textures, in plain memory against a budget of
// "linear memory" (Makefile.n3dssim).
#include "3ds.h"
typedef enum { GPU_RGBA8 = 0, GPU_RGB8, GPU_RGBA5551, GPU_RGB565, GPU_RGBA4 } GPU_TEXCOLOR;
typedef enum { GPU_NEAREST = 0, GPU_LINEAR } GPU_TEXTURE_FILTER_PARAM;
typedef enum { GPU_CLAMP_TO_EDGE = 0, GPU_CLAMP_TO_BORDER, GPU_REPEAT, GPU_MIRRORED_REPEAT } GPU_TEXTURE_WRAP_PARAM;
typedef struct {
	void* data;
	GPU_TEXCOLOR fmt;
	size_t size;
	u16 width, height;
	unsigned int magic;   // set while the texels are allocated
} C3D_Tex;
bool C3D_TexInit(C3D_Tex* tex, u16 width, u16 height, GPU_TEXCOLOR format);
void C3D_TexDelete(C3D_Tex* tex);
static inline void C3D_TexFlush(C3D_Tex*) {}
static inline void C3D_TexSetFilter(C3D_Tex*, GPU_TEXTURE_FILTER_PARAM, GPU_TEXTURE_FILTER_PARAM) {}
static inline void C3D_TexSetWrap(C3D_Tex*, GPU_TEXTURE_WRAP_PARAM, GPU_TEXTURE_WRAP_PARAM) {}
