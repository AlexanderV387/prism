#include "prism/texture.h"

#include <stdlib.h>
#include <string.h>
#include <png.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "prism/file.h"
#include "prism/log.h"
#include "prism/memoryhandler.h"
#include "prism/system.h"

#include "prism3ds.h"
#include "prism3dsperf.h"

// [3DS] Textures on the PICA200: power-of-two sizes from 8 to 1024, in 8x8
// tiles of pixels in Z order, the image's first row at the bottom (so that
// v = 0 is the image's top: drawing_3ds.cpp). 32-bit images as RGBA8,
// paletted ones as RGBA5551, 16-bit ones as RGBA4. The GPU has no palettes: paletted images
// keep their 8-bit pixels and are converted with their palette when drawn
// after it changed.

namespace prism {

	static const u8 gTileOrder[64] = {
		 0,  1,  4,  5, 16, 17, 20, 21,
		 2,  3,  6,  7, 18, 19, 22, 23,
		 8,  9, 12, 13, 24, 25, 28, 29,
		10, 11, 14, 15, 26, 27, 30, 31,
		32, 33, 36, 37, 48, 49, 52, 53,
		34, 35, 38, 39, 50, 51, 54, 55,
		40, 41, 44, 45, 56, 57, 60, 61,
		42, 43, 46, 47, 58, 59, 62, 63,
	};

	static const int MAXIMUM_TEXTURE_SIZE = 1024;

	// The texels of the image's row y: the texel of pixel x is at
	// mBase + ((x >> 3) << 6) + mOrder[x & 7].
	typedef struct {
		u32 mBase;
		const u8* mOrder;
	} TexelRow;

	static inline TexelRow texelRow(int y, const C3D_Tex* tTex) {
		const int flipped = tTex->height - 1 - y;
		TexelRow ret;
		ret.mBase = (flipped >> 3) * (tTex->width >> 3) * 64;
		ret.mOrder = &gTileOrder[(flipped & 7) * 8];
		return ret;
	}

	static inline u32 texelOffset(const TexelRow& tRow, int x) {
		return tRow.mBase + ((x >> 3) << 6) + tRow.mOrder[x & 7];
	}

	// The time spent making textures, for the load-time log.
	static void countTexture(uint64_t tStart, int tWidth, int tHeight) {
		gN3DSPerf.mTextureTicks += getN3DSPerfTicks() - tStart;
		gN3DSPerf.mTextures++;
		gN3DSPerf.mTexels += (uint64_t)tWidth * tHeight;
	}

	static int powerOfTwoAtLeast(int tSize) {
		int ret = 8;
		while (ret < tSize && ret < MAXIMUM_TEXTURE_SIZE) ret *= 2;
		return ret;
	}

	// A new texture of the image's size, its texels cleared. Without the
	// memory for it, one that is never drawn (and a warning, counted in the
	// compatibility log): the game goes on, short of that sprite.
	static TextureData makeTexture(int tWidth, int tHeight, GPU_TEXCOLOR tFormat) {
		TextureData ret;
		ret.mTextureSize.x = tWidth;
		ret.mTextureSize.y = tHeight;
		ret.mHasPalette = 0;
		ret.mPaletteID = 0;
		ret.mTexture = allocTextureMemory(sizeof(N3DSTextureData));

		if (tWidth > MAXIMUM_TEXTURE_SIZE || tHeight > MAXIMUM_TEXTURE_SIZE) {
			logWarningFormat("Texture of %dx%d cut to %d.", tWidth, tHeight, MAXIMUM_TEXTURE_SIZE);
		}

		Texture texture = (Texture)ret.mTexture->mData;
		if (!C3D_TexInit(&texture->mTex, powerOfTwoAtLeast(tWidth), powerOfTwoAtLeast(tHeight), tFormat)) {
			logWarning("Out of texture (linear) memory: a sprite left out.");
			texture->mIsInitialized = 0;
			return ret;
		}
		texture->mIsInitialized = 1;
		C3D_TexSetFilter(&texture->mTex, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&texture->mTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		memset(texture->mTex.data, 0, texture->mTex.size);
		return ret;
	}

	static int clampedWidth(int tWidth) { return tWidth > MAXIMUM_TEXTURE_SIZE ? MAXIMUM_TEXTURE_SIZE : tWidth; }

	int getN3DSTextureWidth(const TextureData& tTexture) {
		return ((Texture)tTexture.mTexture->mData)->mTex.width;
	}

	int getN3DSTextureHeight(const TextureData& tTexture) {
		return ((Texture)tTexture.mTexture->mData)->mTex.height;
	}

	TextureData loadTextureFromARGB32Buffer(const Buffer& b, int tWidth, int tHeight) {
		const uint64_t start = getN3DSPerfTicks();
		TextureData ret = makeTexture(tWidth, tHeight, GPU_RGBA8);
		if (!((Texture)ret.mTexture->mData)->mIsInitialized) return ret;
		C3D_Tex* tex = &((Texture)ret.mTexture->mData)->mTex;
		u32* dst = (u32*)tex->data;
		const u32* src = (const u32*)b.mData;
		const int w = clampedWidth(tWidth), h = clampedWidth(tHeight);

		for (int y = 0; y < h; y++) {
			const TexelRow row = texelRow(y, tex);
			const u32* line = src + y * tWidth;
			for (int x = 0; x < w; x++) {
				const u32 argb = line[x];
				dst[texelOffset(row, x)] = (argb << 8) | (argb >> 24);
			}
		}

		C3D_TexFlush(tex);
		countTexture(start, w, h);
		return ret;
	}

	TextureData loadTextureFromARGB16Buffer(const Buffer& b, int tWidth, int tHeight) {
		const uint64_t start = getN3DSPerfTicks();
		TextureData ret = makeTexture(tWidth, tHeight, GPU_RGBA4);
		if (!((Texture)ret.mTexture->mData)->mIsInitialized) return ret;
		C3D_Tex* tex = &((Texture)ret.mTexture->mData)->mTex;
		u16* dst = (u16*)tex->data;
		const u16* src = (const u16*)b.mData;
		const int w = clampedWidth(tWidth), h = clampedWidth(tHeight);

		for (int y = 0; y < h; y++) {
			const TexelRow row = texelRow(y, tex);
			const u16* line = src + y * tWidth;
			for (int x = 0; x < w; x++) {
				const u16 argb = line[x];
				dst[texelOffset(row, x)] = (u16)((argb << 4) | (argb >> 12));
			}
		}

		C3D_TexFlush(tex);
		countTexture(start, w, h);
		return ret;
	}

	// The Dreamcast's twiddled order (as texture.cpp writes it).
#define TWIDTAB(x) ( (x&1)|((x&2)<<1)|((x&4)<<2)|((x&8)<<3)|((x&16)<<4)| \
                     ((x&32)<<5)|((x&64)<<6)|((x&128)<<7)|((x&256)<<8)|((x&512)<<9) )
#define TWIDOUT(x, y) ( TWIDTAB((y)) | (TWIDTAB((x)) << 1) )

	TextureData loadTextureFromTwiddledARGB16Buffer(const Buffer& b, int tWidth, int tHeight) {
		u16* untwiddled = (u16*)malloc(tWidth * tHeight * 2);
		const u16* src = (const u16*)b.mData;
		const int min = tWidth < tHeight ? tWidth : tHeight;
		const int mask = min - 1;

		for (int y = 0; y < tHeight; y++) {
			for (int x = 0; x < tWidth; x++) {
				untwiddled[y * tWidth + x] = src[TWIDOUT(x & mask, y & mask) + (x / min + y / min) * min * min];
			}
		}

		Buffer plain = makeBuffer(untwiddled, tWidth * tHeight * 2);
		TextureData ret = loadTextureFromARGB16Buffer(plain, tWidth, tHeight);
		free(untwiddled);
		return ret;
	}

	// ==========================================================================
	// Paletted textures: their 8-bit pixels kept compressed (in runs: most of
	// a sprite is runs of its transparent color), their texels made when
	// loaded if there is room, else when first drawn; when there is no room
	// to make one, the textures drawn longest ago let theirs go (a CvS
	// character's sprites alone take 56 MB of texels, more than the 3DS has
	// for two).

	static unsigned int gN3DSTextureFrame = 1;
	static std::unordered_set<Texture> gResidentTextures;   // paletted, with texels
	static std::vector<unsigned char> gIndexScratch;

	void advanceN3DSTextureFrame() { gN3DSTextureFrame++; }

	// Paletted textures' own palettes (a sprite's in its SFF, not one of
	// prism's 4), as RGBA5551, each kept once for all its textures.
	typedef struct {
		u16 mColors[256];
		int mReferences;
	} OwnPalette;
	static std::vector<OwnPalette> gOwnPalettes;
	static std::unordered_multimap<uint32_t, int> gOwnPaletteIndices;   // by a hash of the colors

	static uint32_t hashColors(const u16* tColors) {
		uint32_t hash = 2166136261u;
		for (int i = 0; i < 256; i++) hash = (hash ^ tColors[i]) * 16777619u;
		return hash;
	}

	static int addOwnPalette(const u16* tColors) {
		const uint32_t hash = hashColors(tColors);
		const auto range = gOwnPaletteIndices.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it) {
			auto& palette = gOwnPalettes[it->second];
			if (palette.mReferences && !memcmp(palette.mColors, tColors, sizeof(palette.mColors))) {
				palette.mReferences++;
				return it->second;
			}
		}
		int index = -1;
		for (size_t i = 0; i < gOwnPalettes.size(); i++) {
			if (!gOwnPalettes[i].mReferences) { index = (int)i; break; }
		}
		if (index < 0) {
			gOwnPalettes.push_back(OwnPalette());
			index = (int)gOwnPalettes.size() - 1;
		}
		else {
			const uint32_t oldHash = hashColors(gOwnPalettes[index].mColors);
			const auto old = gOwnPaletteIndices.equal_range(oldHash);
			for (auto it = old.first; it != old.second; ++it) {
				if (it->second == index) { gOwnPaletteIndices.erase(it); break; }
			}
		}
		memcpy(gOwnPalettes[index].mColors, tColors, sizeof(gOwnPalettes[index].mColors));
		gOwnPalettes[index].mReferences = 1;
		gOwnPaletteIndices.insert(std::make_pair(hash, index));
		return index;
	}

	void forgetN3DSTexture(N3DSTextureData* tTexture) {
		gResidentTextures.erase(tTexture);
		if (tTexture->mOwnPalette >= 0 && tTexture->mOwnPalette < (int)gOwnPalettes.size()) {
			gOwnPalettes[tTexture->mOwnPalette].mReferences--;
			tTexture->mOwnPalette = -1;
		}
	}

	static u16 toRGBA5551(u32 tRGBA) {
		return (u16)((((tRGBA >> 27) & 0x1F) << 11) | (((tRGBA >> 19) & 0x1F) << 6) | (((tRGBA >> 11) & 0x1F) << 1) | ((tRGBA & 0xFF) >= 0x80));
	}

	void getN3DSTexturePalette(const TextureData& tTexture, u16* oColors) {
		const Texture texture = (Texture)tTexture.mTexture->mData;
		if (texture->mOwnPalette >= 0) {
			memcpy(oColors, gOwnPalettes[texture->mOwnPalette].mColors, 256 * sizeof(u16));
			return;
		}
		const int id = tTexture.mPaletteID & 3;
		for (int i = 0; i < 256; i++) oColors[i] = toRGBA5551(gN3DSPalettes[id][i]);
	}

	// The palette version a texture's texels are up to: its own palette's
	// never changes.
	static const int OWN_PALETTE_VERSION = 0x7FFFFFFF;

	static int getPaletteVersion(const TextureData& tTexture) {
		const Texture texture = (Texture)tTexture.mTexture->mData;
		return texture->mOwnPalette >= 0 ? OWN_PALETTE_VERSION : gN3DSPaletteVersions[tTexture.mPaletteID & 3];
	}

	// Runs: a byte n < 128, then n + 1 bytes as they are; a byte n >= 128,
	// then one byte n - 127 times.
	static unsigned char* compressIndices(const unsigned char* tSource, int tSize, int* oCompressedSize) {
		// At most one byte more for each 128 (literals all the way).
		std::vector<unsigned char>& out = gIndexScratch;
		out.resize((size_t)tSize + tSize / 128 + 2);
		unsigned char* o = out.data();
		int i = 0;
		while (i < tSize) {
			const unsigned char value = tSource[i];
			const int runEnd = std::min(tSize, i + 128);
			int run = 1;
			// Four at a time first (most of a sprite is runs of color 0).
			const uint32_t four = value * 0x01010101u;
			while (i + run + 4 <= runEnd) {
				uint32_t next;
				memcpy(&next, tSource + i + run, 4);
				if (next != four) break;
				run += 4;
			}
			while (i + run < runEnd && tSource[i + run] == value) run++;
			if (run >= 3) {
				*o++ = (unsigned char)(0x80 | (run - 1));
				*o++ = value;
				i += run;
				continue;
			}
			const int literalStart = i;
			const int literalEnd = runEnd;
			while (i < literalEnd) {
				if (i + 2 < tSize && tSource[i] == tSource[i + 1] && tSource[i] == tSource[i + 2]) break;
				i++;
			}
			const int length = i - literalStart;
			*o++ = (unsigned char)(length - 1);
			memcpy(o, tSource + literalStart, length);
			o += length;
		}
		const size_t size = o - out.data();
		unsigned char* ret = (unsigned char*)malloc(size);
		if (ret) memcpy(ret, out.data(), size);
		*oCompressedSize = (int)size;
		return ret;
	}

	static const unsigned char* decompressIndices(const Texture tTexture, int tSize) {
		std::vector<unsigned char>& out = gIndexScratch;
		out.resize(tSize);
		const unsigned char* p = tTexture->mIndexed;
		const unsigned char* end = p + tTexture->mIndexedSize;
		int i = 0;
		while (p < end && i < tSize) {
			const int c = *p++;
			if (c & 0x80) {
				const int run = std::min((c & 0x7F) + 1, tSize - i);
				memset(&out[i], *p++, run);
				i += run;
			}
			else {
				const int length = std::min(c + 1, tSize - i);
				memcpy(&out[i], p, length);
				p += c + 1;
				i += length;
			}
		}
		return out.data();
	}

	// Lets the texels go of the paletted texture drawn longest ago (not in
	// this frame or the one before: the GPU may still draw them). 0 if there
	// is none.
	static int evictOldestTexture() {
		Texture oldest = NULL;
		for (const auto texture : gResidentTextures) {
			if (texture->mLastUsedFrame + 1 >= gN3DSTextureFrame) continue;
			if (!oldest || texture->mLastUsedFrame < oldest->mLastUsedFrame) oldest = texture;
		}
		if (!oldest) return 0;
		C3D_TexDelete(&oldest->mTex);
		oldest->mIsInitialized = 0;
		gResidentTextures.erase(oldest);
		gN3DSPerf.mEvictedTextures++;
		return 1;
	}

	// The texels of a paletted texture, cleared; when drawn (tIsEvicting),
	// making room for them if needed.
	// Linear memory kept free for the rest (sounds, music, other textures):
	// more when loading, the textures drawn coming first.
	static const size_t LOAD_RESERVE = 8 * 1024 * 1024;
	static const size_t DRAW_RESERVE = 3 * 1024 * 1024;

	static int makePalettedTexels(Texture tTexture, int tWidth, int tHeight, int tIsEvicting) {
		const int width = powerOfTwoAtLeast(tWidth), height = powerOfTwoAtLeast(tHeight);
		const size_t needed = (size_t)width * height * 2 + (tIsEvicting ? DRAW_RESERVE : LOAD_RESERVE);
		while (linearSpaceFree() < needed || !C3D_TexInit(&tTexture->mTex, width, height, GPU_RGBA5551)) {
			if (!tIsEvicting || !evictOldestTexture()) return 0;
		}
		tTexture->mIsInitialized = 1;
		tTexture->mPaletteVersion = -1;
		C3D_TexSetFilter(&tTexture->mTex, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&tTexture->mTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		memset(tTexture->mTex.data, 0, tTexture->mTex.size);
		gResidentTextures.insert(tTexture);
		return 1;
	}

	static TextureData loadPalettedTextureFrom8BitBufferWithOwnPalette(const Buffer& b, int tPaletteID, int tWidth, int tHeight, int tOwnPalette);

	// A paletted texture's texels from its indices, with its palette as it
	// is now; eight texels (a tile's row) at a time.
	static void fillPalettedTexels(const TextureData& tTexture, const unsigned char* tIndices) {
		Texture texture = (Texture)tTexture.mTexture->mData;
		C3D_Tex* tex = &texture->mTex;
		u16* dst = (u16*)tex->data;
		u16 palette[256];
		getN3DSTexturePalette(tTexture, palette);
		const int w = clampedWidth(tTexture.mTextureSize.x), h = clampedWidth(tTexture.mTextureSize.y);
		const int fullBlocks = w >> 3;

		for (int y = 0; y < h; y++) {
			const TexelRow row = texelRow(y, tex);
			const unsigned char* line = tIndices + y * tTexture.mTextureSize.x;
			u16* block = dst + row.mBase;
			const u8* order = row.mOrder;
			for (int b = 0; b < fullBlocks; b++, block += 64, line += 8) {
				block[order[0]] = palette[line[0]];
				block[order[1]] = palette[line[1]];
				block[order[2]] = palette[line[2]];
				block[order[3]] = palette[line[3]];
				block[order[4]] = palette[line[4]];
				block[order[5]] = palette[line[5]];
				block[order[6]] = palette[line[6]];
				block[order[7]] = palette[line[7]];
			}
			for (int x = 0; x < (w & 7); x++) {
				block[order[x]] = palette[line[x]];
			}
		}

		C3D_TexFlush(tex);
		texture->mPaletteVersion = getPaletteVersion(tTexture);
	}

	TextureData loadN3DSTextureWithOwnPalette(const Buffer& b, const u32* tRGBAPalette, int tWidth, int tHeight) {
		u16 colors[256];
		for (int i = 0; i < 256; i++) colors[i] = toRGBA5551(tRGBAPalette[i]);
		const int ownPalette = addOwnPalette(colors);
		TextureData ret = loadPalettedTextureFrom8BitBufferWithOwnPalette(b, 0, tWidth, tHeight, ownPalette);
		return ret;
	}

	TextureData loadPalettedTextureFrom8BitBuffer(const Buffer& b, int tPaletteID, int tWidth, int tHeight) {
		return loadPalettedTextureFrom8BitBufferWithOwnPalette(b, tPaletteID, tWidth, tHeight, -1);
	}

	static TextureData loadPalettedTextureFrom8BitBufferWithOwnPalette(const Buffer& b, int tPaletteID, int tWidth, int tHeight, int tOwnPalette) {
		// RGBA5551: half the memory of RGBA8, and the palettes' alpha is
		// all or nothing (color 0 transparent).
		TextureData ret;
		ret.mTextureSize.x = tWidth;
		ret.mTextureSize.y = tHeight;
		ret.mHasPalette = 1;
		ret.mPaletteID = tPaletteID;
		ret.mTexture = allocTextureMemory(sizeof(N3DSTextureData));
		if (tWidth > MAXIMUM_TEXTURE_SIZE || tHeight > MAXIMUM_TEXTURE_SIZE) {
			logWarningFormat("Texture of %dx%d cut to %d.", tWidth, tHeight, MAXIMUM_TEXTURE_SIZE);
		}

		Texture texture = (Texture)ret.mTexture->mData;
		texture->mOwnPalette = tOwnPalette;
		texture->mIndexed = compressIndices((const unsigned char*)b.mData, tWidth * tHeight, &texture->mIndexedSize);
		if (!texture->mIndexed) {
			logWarning("Out of heap memory: a sprite left out.");
			return ret;
		}

		// Converted now with the palette as it is (usually already set), not
		// in the first frame it is drawn; again later if the palette changes.
		// From the indices at hand, not the ones just compressed.
		if (makePalettedTexels(texture, tWidth, tHeight, 0)) {
			const uint64_t start = getN3DSPerfTicks();
			texture->mLastUsedFrame = gN3DSTextureFrame;
			fillPalettedTexels(ret, (const unsigned char*)b.mData);
			countTexture(start, clampedWidth(tWidth), clampedWidth(tHeight));
		}
		else {
			gN3DSPerf.mDeferredTextures++;
		}
		return ret;
	}

	void prepareN3DSTexture(const TextureData& tTexture) {
		if (!tTexture.mHasPalette) return;

		Texture texture = (Texture)tTexture.mTexture->mData;
		texture->mLastUsedFrame = gN3DSTextureFrame;
		if (!texture->mIndexed) return;
		const uint64_t start = getN3DSPerfTicks();
		int isMadeLate = 0;
		if (!texture->mIsInitialized) {
			if (!makePalettedTexels(texture, tTexture.mTextureSize.x, tTexture.mTextureSize.y, 1)) {
				logWarning("Out of texture (linear) memory: a sprite not drawn.");
				return;
			}
			gN3DSPerf.mLateTextures++;
			isMadeLate = 1;
		}
		if (texture->mPaletteVersion == getPaletteVersion(tTexture)) return;
		const int isRepaletted = !isMadeLate && texture->mPaletteVersion != -1;

		fillPalettedTexels(tTexture, decompressIndices(texture, tTexture.mTextureSize.x * tTexture.mTextureSize.y));
		countTexture(start, clampedWidth(tTexture.mTextureSize.x), clampedWidth(tTexture.mTextureSize.y));
		if (isMadeLate) gN3DSPerf.mLateTextureTicks += getN3DSPerfTicks() - start;
		else if (isRepaletted) {
			gN3DSPerf.mRepaletted++;
			gN3DSPerf.mRepalettedTicks += getN3DSPerfTicks() - start;
		}
	}

	// ==========================================================================
	// PNG, to 32-bit ARGB (as the other backends' loadPNGARGB32Buffer).

	typedef struct {
		const unsigned char* mData;
		size_t mLength;
		size_t mPosition;
	} PNGMemoryReader;

	static void readPNGFromMemory(png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
		PNGMemoryReader* reader = (PNGMemoryReader*)png_get_io_ptr(png_ptr);
		if (reader->mPosition + byteCountToRead > reader->mLength) {
			png_error(png_ptr, "read past the end");
		}
		memcpy(outBytes, reader->mData + reader->mPosition, byteCountToRead);
		reader->mPosition += byteCountToRead;
	}

	static Buffer decodePNGToARGB32(const Buffer& tRawPNG, int* oWidth, int* oHeight) {
		*oWidth = *oHeight = 0;
		if (tRawPNG.mLength < 8 || png_sig_cmp((png_const_bytep)tRawPNG.mData, 0, 8)) {
			logError("Not a PNG file.");
			return makeBuffer(NULL, 0);
		}

		png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
		png_infop info_ptr = png_ptr ? png_create_info_struct(png_ptr) : NULL;
		if (!info_ptr) {
			png_destroy_read_struct(&png_ptr, NULL, NULL);
			logError("Cannot create the PNG reader.");
			return makeBuffer(NULL, 0);
		}

		u32* pixels = NULL;
		png_bytep* rows = NULL;
		if (setjmp(png_jmpbuf(png_ptr))) {
			png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
			free(pixels);
			free(rows);
			logError("Unable to read the PNG file.");
			return makeBuffer(NULL, 0);
		}

		PNGMemoryReader reader = { (const unsigned char*)tRawPNG.mData, tRawPNG.mLength, 0 };
		png_set_read_fn(png_ptr, &reader, readPNGFromMemory);
		png_read_info(png_ptr, info_ptr);

		const int width = (int)png_get_image_width(png_ptr, info_ptr);
		const int height = (int)png_get_image_height(png_ptr, info_ptr);
		const int colorType = png_get_color_type(png_ptr, info_ptr);

		// Everything to 8-bit BGRA in memory: a little-endian 0xAARRGGBB.
		if (png_get_bit_depth(png_ptr, info_ptr) == 16) png_set_strip_16(png_ptr);
		if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
		if (colorType == PNG_COLOR_TYPE_GRAY && png_get_bit_depth(png_ptr, info_ptr) < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
		if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
		if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png_ptr);
		png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
		png_set_bgr(png_ptr);
		png_read_update_info(png_ptr, info_ptr);

		pixels = (u32*)allocMemory(width * height * 4);
		rows = (png_bytep*)malloc(height * sizeof(png_bytep));
		for (int y = 0; y < height; y++) rows[y] = (png_bytep)(pixels + y * width);
		png_read_image(png_ptr, rows);
		png_read_end(png_ptr, NULL);

		free(rows);
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

		*oWidth = width;
		*oHeight = height;
		return makeBufferOwned(pixels, width * height * 4);
	}

	Buffer loadPNGARGB32Buffer(const char* tFileDir, int* oWidth, int* oHeight) {
		Buffer raw = fileToBuffer(tFileDir);
		Buffer ret = decodePNGToARGB32(raw, oWidth, oHeight);
		freeBuffer(raw);
		return ret;
	}

	static TextureData makeTextureFromARGB32OrEmpty(Buffer& b, int tWidth, int tHeight, const char* tName) {
		if (!b.mData) {
			logErrorFormat("Unable to load %s.", tName);
			Buffer empty = makeBuffer(calloc(1, 8 * 8 * 4), 8 * 8 * 4);
			TextureData ret = loadTextureFromARGB32Buffer(empty, 8, 8);
			free(empty.mData);
			return ret;
		}
		TextureData ret = loadTextureFromARGB32Buffer(b, tWidth, tHeight);
		freeBuffer(b);
		return ret;
	}

	TextureData loadTexturePNG(const char* tFileDir) {
		int width, height;
		Buffer b = loadPNGARGB32Buffer(tFileDir, &width, &height);
		return makeTextureFromARGB32OrEmpty(b, width, height, tFileDir);
	}

	TextureData loadTextureFromRawPNGBuffer(const Buffer& b, int /*tWidth*/, int /*tHeight*/) {
		int width, height;
		Buffer decoded = decodePNGToARGB32(b, &width, &height);
		return makeTextureFromARGB32OrEmpty(decoded, width, height, "a PNG sprite");
	}

	// .pkg (the Dreamcast's compressed textures): the .png next to it.
	static void getPNGPathForPKG(char* tDst, const char* tFileDir) {
		strcpy(tDst, tFileDir);
		char* fileExt = getFileExtension(tDst);
		if (fileExt) strcpy(fileExt, "png");
	}

	TextureData loadTexturePKG(const char* tFileDir) {
		char pngPath[1024];
		getPNGPathForPKG(pngPath, tFileDir);
		return loadTexturePNG(pngPath);
	}

	TextureData loadTexture(const char* tFileDir) {
		const char* fileExt = getFileExtension(tFileDir);
		if (fileExt && !strcmp("pkg", fileExt)) return loadTexturePKG(tFileDir);
		return loadTexturePNG(tFileDir);
	}

	void unloadTexture(TextureData& tTexture) {
		freeTextureMemory(tTexture.mTexture);
	}

	TruetypeFont loadTruetypeFont(const char* /*tName*/, float /*tSize*/) {
		return (TruetypeFont)malloc(1);
	}

	void unloadTruetypeFont(TruetypeFont tFont) {
		free(tFont);
	}

	int getTextureHash(const TextureData& tTexture) {
		return (int)(uintptr_t)tTexture.mTexture->mData;
	}

	int canLoadTexture(const char* tPath) {
		const char* fileExt = getFileExtension(tPath);
		if (fileExt && !strcmp("pkg", fileExt)) {
			char pngPath[1024];
			getPNGPathForPKG(pngPath, tPath);
			return isFile(pngPath);
		}
		return isFile(tPath);
	}

	void saveScreenShot(const char* /*tFileDir*/) {}

	void copyScreenShotToClipboard() {} // UNSUPPORTED

}
