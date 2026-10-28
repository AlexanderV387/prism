#include "prism/texture.h"

#include <stdlib.h>
#include <string.h>
#include <png.h>

#include "prism/file.h"
#include "prism/log.h"
#include "prism/memoryhandler.h"
#include "prism/system.h"

#include "prism3ds.h"

// [3DS] Textures on the PICA200: power-of-two sizes from 8 to 1024, in 8x8
// tiles of pixels in Z order, the image's first row at the bottom (so that
// v = 0 is the image's top: drawing_3ds.cpp). 32-bit and paletted images
// as RGBA8, 16-bit ones as RGBA4. The GPU has no palettes: paletted images
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

	// The texel of the image's pixel x, y.
	static inline u32 texelOffset(int x, int y, int tWidth, int tHeight) {
		y = tHeight - 1 - y;
		return ((y >> 3) * (tWidth >> 3) + (x >> 3)) * 64 + gTileOrder[(y & 7) * 8 + (x & 7)];
	}

	static int powerOfTwoAtLeast(int tSize) {
		int ret = 8;
		while (ret < tSize && ret < MAXIMUM_TEXTURE_SIZE) ret *= 2;
		return ret;
	}

	// A new texture of the image's size, its texels cleared.
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
			logErrorFormat("Out of memory for a texture of %dx%d (linear memory free: %d).", tWidth, tHeight, (int)linearSpaceFree());
			abortSystem();
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
		TextureData ret = makeTexture(tWidth, tHeight, GPU_RGBA8);
		C3D_Tex* tex = &((Texture)ret.mTexture->mData)->mTex;
		u32* dst = (u32*)tex->data;
		const u32* src = (const u32*)b.mData;
		const int w = clampedWidth(tWidth), h = clampedWidth(tHeight);

		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const u32 argb = src[y * tWidth + x];
				dst[texelOffset(x, y, tex->width, tex->height)] = (argb << 8) | (argb >> 24);
			}
		}

		C3D_TexFlush(tex);
		return ret;
	}

	TextureData loadTextureFromARGB16Buffer(const Buffer& b, int tWidth, int tHeight) {
		TextureData ret = makeTexture(tWidth, tHeight, GPU_RGBA4);
		C3D_Tex* tex = &((Texture)ret.mTexture->mData)->mTex;
		u16* dst = (u16*)tex->data;
		const u16* src = (const u16*)b.mData;
		const int w = clampedWidth(tWidth), h = clampedWidth(tHeight);

		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const u16 argb = src[y * tWidth + x];
				dst[texelOffset(x, y, tex->width, tex->height)] = (u16)((argb << 4) | (argb >> 12));
			}
		}

		C3D_TexFlush(tex);
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

	TextureData loadPalettedTextureFrom8BitBuffer(const Buffer& b, int tPaletteID, int tWidth, int tHeight) {
		TextureData ret = makeTexture(tWidth, tHeight, GPU_RGBA8);
		ret.mHasPalette = 1;
		ret.mPaletteID = tPaletteID;

		Texture texture = (Texture)ret.mTexture->mData;
		texture->mIndexed = (unsigned char*)malloc(tWidth * tHeight);
		memcpy(texture->mIndexed, b.mData, tWidth * tHeight);
		texture->mPaletteVersion = -1;
		return ret;
	}

	void prepareN3DSTexture(const TextureData& tTexture) {
		if (!tTexture.mHasPalette) return;

		Texture texture = (Texture)tTexture.mTexture->mData;
		const int id = tTexture.mPaletteID & 3;
		if (texture->mPaletteVersion == gN3DSPaletteVersions[id]) return;

		C3D_Tex* tex = &texture->mTex;
		u32* dst = (u32*)tex->data;
		const u32* palette = gN3DSPalettes[id];
		const int w = clampedWidth(tTexture.mTextureSize.x), h = clampedWidth(tTexture.mTextureSize.y);

		for (int y = 0; y < h; y++) {
			const unsigned char* row = texture->mIndexed + y * tTexture.mTextureSize.x;
			for (int x = 0; x < w; x++) {
				dst[texelOffset(x, y, tex->width, tex->height)] = palette[row[x]];
			}
		}

		C3D_TexFlush(tex);
		texture->mPaletteVersion = gN3DSPaletteVersions[id];
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
