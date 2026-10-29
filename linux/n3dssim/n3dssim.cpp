// [3DS simulation] Runs Prism's 3DS texture code (3ds/texture_3ds.cpp) on a
// PC, headless: textures in plain memory against a budget of linear memory
// (N3DS_SIM_LINEAR, bytes; 70 MB by default), drawn sprites made current as
// on the 3DS and their texels checked against their palette indices. With
// AddressSanitizer, any write or free out of place stops it at its line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <sanitizer/common_interface_defs.h>

#include "3ds.h"
#include "citro3d.h"

#include "prism/drawing.h"
#include "prism/log.h"
#include "prism/memoryhandler.h"
#include "../../3ds/prism3ds.h"
#include "../../3ds/prism3dsperf.h"

extern "C" {
	u32 __ctru_heap_size = 48 * 1024 * 1024 - 512 * 1024;
}

// The 3DS's heap (48 MB, the 512 KB stack inside it): with N3DS_SIM_HEAP
// (bytes, without AddressSanitizer), allocations past it fail as there,
// mallinfo() telling what is in use as newlib's does; textures (linear
// memory on the 3DS) are not in it.
#if !defined(__SANITIZE_ADDRESS__)
#include <malloc.h>
extern "C" {
	void* __libc_malloc(size_t);
	void __libc_free(void*);
	void* __libc_calloc(size_t, size_t);
	void* __libc_realloc(void*, size_t);
	void* __libc_memalign(size_t, size_t);
}
static size_t gHeapUsed = 0;
static int gHeapLimitState = -1;   // -1 not looked at yet, 0 none, 1 limited
static size_t gHeapLimit = 0;

static int isHeapLimited() {
	if (gHeapLimitState < 0) {
		const char* env = getenv("N3DS_SIM_HEAP");
		gHeapLimitState = env ? 1 : 0;
		if (env) gHeapLimit = strtoull(env, NULL, 10);
		if (env) __ctru_heap_size = (u32)gHeapLimit;
	}
	return gHeapLimitState;
}

static void* countAllocation(void* tPointer) {
	if (tPointer) gHeapUsed += malloc_usable_size(tPointer);
	return tPointer;
}

static int hasHeapRoom(size_t tSize) {
	return !isHeapLimited() || gHeapUsed + tSize <= gHeapLimit;
}

extern "C" {
	void* malloc(size_t tSize) { return hasHeapRoom(tSize) ? countAllocation(__libc_malloc(tSize)) : NULL; }
	void free(void* tPointer) {
		if (tPointer) gHeapUsed -= malloc_usable_size(tPointer);
		__libc_free(tPointer);
	}
	void* calloc(size_t tAmount, size_t tSize) { return hasHeapRoom(tAmount * tSize) ? countAllocation(__libc_calloc(tAmount, tSize)) : NULL; }
	void* realloc(void* tPointer, size_t tSize) {
		const size_t before = tPointer ? malloc_usable_size(tPointer) : 0;
		if (tSize > before && !hasHeapRoom(tSize - before)) return NULL;
		void* ret = __libc_realloc(tPointer, tSize);
		if (ret || !tSize) gHeapUsed -= before;
		return countAllocation(ret);
	}
	void* memalign(size_t tAlignment, size_t tSize) { return hasHeapRoom(tSize) ? countAllocation(__libc_memalign(tAlignment, tSize)) : NULL; }
	void* aligned_alloc(size_t tAlignment, size_t tSize) { return memalign(tAlignment, tSize); }
	int posix_memalign(void** oPointer, size_t tAlignment, size_t tSize) {
		*oPointer = memalign(tAlignment, tSize);
		return *oPointer ? 0 : 12;
	}
	struct mallinfo mallinfo(void) noexcept {
		struct mallinfo ret;
		memset(&ret, 0, sizeof(ret));
		ret.uordblks = (int)gHeapUsed;
		return ret;
	}
}
#define TEXTURE_MALLOC __libc_malloc
#define TEXTURE_FREE __libc_free
#else
#define TEXTURE_MALLOC malloc
#define TEXTURE_FREE free
#endif

static size_t gLinearBudget = 0, gLinearUsed = 0;
static const unsigned int TEX_MAGIC = 0x7E7A11ED;

static size_t getBudget() {
	if (!gLinearBudget) {
		const char* env = getenv("N3DS_SIM_LINEAR");
		gLinearBudget = env ? strtoull(env, NULL, 10) : 70u * 1024 * 1024;
	}
	return gLinearBudget;
}

size_t linearSpaceFree() { return getBudget() - gLinearUsed; }

bool C3D_TexInit(C3D_Tex* tex, u16 width, u16 height, GPU_TEXCOLOR format) {
	const int bytes = format == GPU_RGBA8 ? 4 : format == GPU_RGB8 ? 3 : 2;
	const size_t size = (size_t)width * height * bytes;
	tex->width = width; tex->height = height; tex->fmt = format;
	if ((width & (width - 1)) || (height & (height - 1)) || width < 8 || height < 8 || width > 1024 || height > 1024) return false;
	if (gLinearUsed + size > getBudget()) return false;
	tex->data = TEXTURE_MALLOC(size);
	tex->size = size;
	tex->magic = TEX_MAGIC;
	gLinearUsed += size;
	return true;
}

void C3D_TexDelete(C3D_Tex* tex) {
	if (tex->magic != TEX_MAGIC) {
		fprintf(stderr, "n3dssim: C3D_TexDelete of a texture not allocated (double delete?)\n");
		abort();
	}
	static int traced = 0;
	if (getenv("N3DS_SIM_TRACE_DELETES") && traced < atoi(getenv("N3DS_SIM_TRACE_DELETES"))) {
		traced++;
		fprintf(stderr, "n3dssim: C3D_TexDelete %zu bytes from:\n", tex->size);
#if defined(__SANITIZE_ADDRESS__)
		__sanitizer_print_stack_trace();
#endif
	}
	gLinearUsed -= tex->size;
	TEXTURE_FREE(tex->data);
	tex->data = NULL;
	tex->magic = 0;
}

namespace prism {

	N3DSPerfCounters gN3DSPerf;
	int gN3DSFileBufferSize;
	uint64_t getN3DSPerfTicks() { return 0; }
	double getN3DSPerfMilliseconds(uint64_t tTicks) { return tTicks / 1000.0; }
	void addN3DSPerfLine(const char*, ...) {}

	u32 gN3DSPalettes[4][256];
	int gN3DSPaletteVersions[4];

	static struct {
		float mFramerate = 60.0;
		unsigned long mDraws, mChecked, mMismatches, mFrames;
	} gSim;

	// The texels a paletted texture should have now.
	static void checkPalettedTexture(const TextureData& tTexture) {
		Texture texture = (Texture)tTexture.mTexture->mData;
		if (!tTexture.mHasPalette || !texture->mIsInitialized || !texture->mIndexed) return;
		if (texture->mTex.magic != TEX_MAGIC) {
			fprintf(stderr, "n3dssim: drawing a texture whose texels are gone\n");
			abort();
		}
		const int w = tTexture.mTextureSize.x, h = tTexture.mTextureSize.y;
		unsigned char* indices = (unsigned char*)malloc(w * h);
		const unsigned char* p = texture->mIndexed;
		const unsigned char* end = p + texture->mIndexedSize;
		int i = 0;
		while (p < end && i < w * h) {
			const int c = *p++;
			if (c & 0x80) { int run = (c & 0x7F) + 1; if (run > w * h - i) run = w * h - i; memset(indices + i, *p++, run); i += run; }
			else { int length = c + 1; if (length > w * h - i) length = w * h - i; memcpy(indices + i, p, length); p += c + 1; i += length; }
		}
		static const u8 order[64] = { 0,1,4,5,16,17,20,21, 2,3,6,7,18,19,22,23, 8,9,12,13,24,25,28,29, 10,11,14,15,26,27,30,31, 32,33,36,37,48,49,52,53, 34,35,38,39,50,51,54,55, 40,41,44,45,56,57,60,61, 42,43,46,47,58,59,62,63 };
		const int id = tTexture.mPaletteID & 3;
		u16 colors[256];
		getN3DSTexturePalette(tTexture, colors);
		const u16* texels = (const u16*)texture->mTex.data;
		int bad = 0;
		for (int y = 0; y < h && y < 1024 && !bad; y++) {
			const int flipped = texture->mTex.height - 1 - y;
			const u32 base = (flipped >> 3) * (texture->mTex.width >> 3) * 64;
			for (int x = 0; x < w && x < 1024; x++) {
				const u16 expected = colors[indices[y * w + x]];
				if (texels[base + ((x >> 3) << 6) + order[(flipped & 7) * 8 + (x & 7)]] != expected) { bad = 1; break; }
			}
		}
		free(indices);
		gSim.mChecked++;
		if (bad && gSim.mMismatches++ < 10) fprintf(stderr, "n3dssim: texture %p (%dx%d, palette %d) texels differ from its indices\n", (void*)texture, w, h, id);
	}

	static void drawSimulated(const TextureData& tTexture) {
		gSim.mDraws++;
		prepareN3DSTexture(tTexture);
		checkPalettedTexture(tTexture);
	}

	void printN3DSSimulationSummary() {
		fprintf(stderr, "n3dssim: %lu frames, %lu draws, %lu paletted checked, %lu wrong; linear %zu KB used of %zu; deferred %u, made late %u, let go %u, made again for a palette %u\n",
			gSim.mFrames, gSim.mDraws, gSim.mChecked, gSim.mMismatches, gLinearUsed / 1024, getBudget() / 1024,
			gN3DSPerf.mDeferredTextures, gN3DSPerf.mLateTextures, gN3DSPerf.mEvictedTextures, gN3DSPerf.mRepaletted);
	}

	void initDrawing() { for (int i = 0; i < 4; i++) gN3DSPaletteVersions[i] = 0; }

	void drawSprite(const TextureData& tTexture, const Position&, const PrismRectangle&) { drawSimulated(tTexture); }
	void drawSpriteNoRectangle(const TextureData& tTexture, const Position&, const Position&, const Position&, const Position&, const PrismRectangle&) { drawSimulated(tTexture); }
	void drawMultilineText(const char*, const char*, const Position&, const Vector3D&, Color, const Vector3D&, const Vector3D&) {}
	void drawTruetypeText(const char*, TruetypeFont, const Position&, const Vector3DI&, const Vector3D&, float, const GeoRectangle2D&) {}

	void waitForScreen() {}
	void startDrawing() { gSim.mFrames++; advanceN3DSTextureFrame(); }
	void stopDrawing() {}
	void waitForRendering() {}

	bool isSkippingDrawing() { return false; }
	void setDrawingFrameSkippingEnabled(bool) {}
	void resetDrawingFrameStartTime() {}
	void disableDrawing() {}
	void enableDrawing() {}

	void scaleDrawing(float, const Position&) {}
	void scaleDrawing2D(const Vector2D&, const Position2D&) {}
	void scaleDrawing3D(const Vector3D&, const Position&) {}

	void setDrawingBaseColorOffsetAdvanced(float, float, float) {}
	void setDrawingBaseColor(Color) {}
	void setDrawingBaseColorAdvanced(float, float, float) {}
	void setDrawingColorSolidity(int) {}
	void setDrawingColorInversed(int) {}
	void setDrawingColorFactor(float) {}
	void setDrawingTransparency(float) {}
	void setDrawingDestinationTransparency(float) {}
	void setDrawingRotationZ(float, const Position2D&) {}
	void setDrawingRotationZ(float, const Position&) {}
	void setDrawingParametersToIdentity() {}
	void setDrawingBlendType(BlendType) {}

	void setPaletteFromARGB256Buffer(int tPaletteID, const Buffer& tBuffer) {
		const u8* src = (const u8*)tBuffer.mData;
		u32* dst = gN3DSPalettes[tPaletteID & 3];
		for (int i = 0; i < 256; i++) {
			const u8* c = src + 4 * i;
			dst[i] = ((u32)c[1] << 24) | ((u32)c[2] << 16) | ((u32)c[3] << 8) | c[0];
		}
		gN3DSPaletteVersions[tPaletteID & 3]++;
	}

	void setPaletteFromBGR256WithFirstValueTransparentBuffer(int tPaletteID, const Buffer& tBuffer) {
		const u8* src = (const u8*)tBuffer.mData;
		u32* dst = gN3DSPalettes[tPaletteID & 3];
		for (int i = 0; i < 256; i++) {
			const u8* c = src + 3 * i;
			dst[i] = ((u32)c[0] << 24) | ((u32)c[1] << 16) | ((u32)c[2] << 8) | (i == 0 ? 0 : 0xFF);
		}
		gN3DSPaletteVersions[tPaletteID & 3]++;
	}

	float getRealFramerate() { return gSim.mFramerate; }

}
