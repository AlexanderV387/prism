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

	// A new frame (drawing_3ds.cpp): textures drawn in it, or the one
	// before, keep their texels.
	void advanceN3DSTextureFrame();

	// The texture's size on the GPU (powers of two).
	int getN3DSTextureWidth(const TextureData& tTexture);
	int getN3DSTextureHeight(const TextureData& tTexture);

	// The drawing's end, at exit (system_3ds.cpp).
	void shutdownN3DSDrawing();

}
