#pragma once

// [3DS] What the 3DS backend's files share.

#include <3ds.h>
#include <citro3d.h>

#include "prism/texture.h"

namespace prism {

	// The palettes (prism's 4), as the GPU's RGBA8 texels, and a count of
	// their changes: a paletted texture is converted again when its
	// palette changed since (drawing_3ds.cpp).
	extern u32 gN3DSPalettes[4][256];
	extern int gN3DSPaletteVersions[4];

	// The color the top screen is cleared to, RGBA8 (screeneffect_3ds.cpp).
	extern u32 gN3DSClearColor;

	// Makes the texture's texels current (a paletted one, with its palette;
	// made first if they were let go or had no room when loaded).
	void prepareN3DSTexture(const TextureData& tTexture);

	// A paletted texture with its own palette (RGBA8 colors, a sprite's own
	// in its SFF): kept and made again as prism's paletted ones, at half the
	// memory of the ARGB32 texture it would be otherwise.
	TextureData loadN3DSTextureWithOwnPalette(const Buffer& b, const u32* tRGBAPalette, int tWidth, int tHeight);

	// Lets streamed textures' pixels go, those drawn longest ago first, until
	// tWanted bytes are free (0: all); the bytes freed.
	size_t letN3DSStreamedPixelsGo(size_t tWanted);
	// The heap not in use, from newlib's count and the heap's size.
	size_t getN3DSHeapFree();
	// What the sprite reader keeps of the last streamed sprite, let go with them.
	extern size_t (*gN3DSLetStreamCacheGo)();

	// A sound read by tRead straight into linear memory (soundeffect_3ds.cpp);
	// its ID, -1 if it could not be, -2 if it has to be read whole first.
	int loadN3DSSoundEffectFromReader(void (*tRead)(void* tCaller, void* tDst, uint32_t tSize), void* tCaller, uint32_t tLength);
	// The sounds kept smaller, and left out, for want of linear memory.
	int getN3DSSoundsShrunk();
	// A .snd's part of the linear memory left for sounds (the next one
	// loaded; back to all of it after), and its start and end.
	void setN3DSSoundFileShare(float tShare);
	void startN3DSSoundFile();
	void endN3DSSoundFile();
	int getN3DSSoundsLeftOut();

	// A streamed paletted texture: no pixels kept, read through tStream
	// when drawn (its palette prism's tPaletteID, or tRGBAPalette when not
	// NULL); the texture lets tStream go.
	TextureData loadN3DSStreamedTexture(N3DSTextureStream* tStream, const u32* tRGBAPalette, int tPaletteID, int tWidth, int tHeight);

	// The palette a paletted texture's texels are made with (RGBA5551).
	void getN3DSTexturePalette(const TextureData& tTexture, u16* oColors);

	// A new frame (drawing_3ds.cpp): textures drawn in it, or the one
	// before, keep their texels.
	void advanceN3DSTextureFrame();

	// The texture's size on the GPU (powers of two).
	int getN3DSTextureWidth(const TextureData& tTexture);
	int getN3DSTextureHeight(const TextureData& tTexture);

	// The drawing's end, at exit (system_3ds.cpp).
	void shutdownN3DSDrawing();

}
