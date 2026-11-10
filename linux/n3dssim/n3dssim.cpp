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
#include "prism/math.h"
#include "prism/system.h"
#include <math.h>
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
#include <atomic>
static std::atomic<size_t> gHeapUsed(0);   // the streamed sprites' reader allocates too
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

// N3DS_SIM_TRACK: who made each block still there (the blocks made in a
// fight and kept after it: in the heap's middle on the 3DS, in pieces).
#include <execinfo.h>
#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <map>
#include <mutex>
struct TrackedAllocation { size_t mSize; unsigned mGeneration; void* mCallers[16]; int mDepth; };
static std::unordered_map<void*, TrackedAllocation>* gTracked;
static std::mutex gTrackedMutex;
static thread_local int gIsTracking;
static unsigned gTrackGeneration;
static int isTrackingAllocations() {
	static int state = -1;
	if (state < 0) state = getenv("N3DS_SIM_TRACK") ? 1 : 0;
	return state;
}
static void trackAllocation(void* tPointer, size_t tSize) {
	if (!tPointer || !isTrackingAllocations() || gIsTracking) return;
	gIsTracking = 1;
	TrackedAllocation e;
	e.mSize = tSize;
	e.mGeneration = gTrackGeneration;
	e.mDepth = backtrace(e.mCallers, 16);
	{
		std::lock_guard<std::mutex> lock(gTrackedMutex);
		if (!gTracked) gTracked = new std::unordered_map<void*, TrackedAllocation>();
		(*gTracked)[tPointer] = e;
	}
	gIsTracking = 0;
}
static void untrackAllocation(void* tPointer) {
	if (!tPointer || !isTrackingAllocations() || gIsTracking) return;
	gIsTracking = 1;
	{
		std::lock_guard<std::mutex> lock(gTrackedMutex);
		if (gTracked) gTracked->erase(tPointer);
	}
	gIsTracking = 0;
}
extern "C" void n3dssimMarkAllocations() { gTrackGeneration++; }
// The blocks made since the mark and still there, by who made them.
extern "C" void n3dssimDumpAllocations(size_t tMinimumTotal) {
	if (!gTracked) return;
	gIsTracking = 1;
	std::map<std::string, std::pair<size_t, size_t>> bySite;
	{
		std::lock_guard<std::mutex> lock(gTrackedMutex);
		for (const auto& entry : *gTracked) {
			const TrackedAllocation& e = entry.second;
			if (e.mGeneration < gTrackGeneration) continue;
			char** names = backtrace_symbols(e.mCallers, e.mDepth);
			std::string site;
			for (int i = 2; i < e.mDepth && i < 14; i++) {
				std::string name = names ? names[i] : "?";
				const size_t open = name.find('('), plus = name.find('+');
				if (open != std::string::npos && plus != std::string::npos && plus > open) name = name.substr(open + 1, plus - open - 1);
				site += name + " < ";
			}
			free(names);
			auto& totals = bySite[site];
			totals.first += e.mSize;
			totals.second++;
		}
	}
	std::vector<std::pair<size_t, std::string>> sorted;
	size_t total = 0, blocks = 0;
	for (const auto& entry : bySite) { sorted.push_back(std::make_pair(entry.second.first, entry.first + " (" + std::to_string(entry.second.second) + " blocks)")); total += entry.second.first; blocks += entry.second.second; }
	std::sort(sorted.rbegin(), sorted.rend());
	fprintf(stderr, "n3dssim: %zu bytes in %zu blocks made since the mark still there\n", total, blocks);
	for (const auto& entry : sorted) {
		if (entry.first < tMinimumTotal) break;
		fprintf(stderr, "  %8zu %s\n", entry.first, entry.second.c_str());
	}
	gIsTracking = 0;
}

static void* countAllocation(void* tPointer) {
	if (tPointer) {
		gHeapUsed += malloc_usable_size(tPointer);
		trackAllocation(tPointer, malloc_usable_size(tPointer));
	}
	return tPointer;
}

static int hasHeapRoom(size_t tSize) {
	return !isHeapLimited() || gHeapUsed + tSize <= gHeapLimit;
}

extern "C" {
	void* malloc(size_t tSize) { return hasHeapRoom(tSize) ? countAllocation(__libc_malloc(tSize)) : NULL; }
	void free(void* tPointer) {
		if (tPointer) gHeapUsed -= malloc_usable_size(tPointer);
		untrackAllocation(tPointer);
		__libc_free(tPointer);
	}
	void* calloc(size_t tAmount, size_t tSize) { return hasHeapRoom(tAmount * tSize) ? countAllocation(__libc_calloc(tAmount, tSize)) : NULL; }
	void* realloc(void* tPointer, size_t tSize) {
		const size_t before = tPointer ? malloc_usable_size(tPointer) : 0;
		if (tSize > before && !hasHeapRoom(tSize - before)) return NULL;
		void* ret = __libc_realloc(tPointer, tSize);
		if (ret || !tSize) {
			gHeapUsed -= before;
			untrackAllocation(tPointer);
		}
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
		ret.uordblks = (int)gHeapUsed.load();
		return ret;
	}
}
#define TEXTURE_MALLOC __libc_malloc
#define TEXTURE_FREE __libc_free
#else
#define TEXTURE_MALLOC malloc
#define TEXTURE_FREE free
// No tracking with AddressSanitizer (it has malloc): the tests still link.
extern "C" void n3dssimMarkAllocations() {}
extern "C" void n3dssimDumpAllocations(size_t) {}
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
		if (!indices) return;   // the simulated heap full: not checked this time
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

	// ==================================================================
	// Pictures of chosen frames (N3DS_SIM_SHOT="frame,frame,...", into
	// N3DS_SIM_SHOT_DIR, PPM): the sprites drawn as drawing_3ds.cpp draws
	// them (its transform, colors and blending), on a 400x240 top screen.
	struct SimDrawData {
		float r, g, b, a, rOffset, gOffset, bOffset;
		Matrix4D mMatrix;
		BlendType mBlendType;
		int mIsColorSolid, mIsColorInversed;
		float mDestAlpha;
	};
	static SimDrawData gDraw;
	static int gIsDrawingDisabled;
	struct SimSprite {
		TextureData mTexture;
		Position2D mTopLeft, mTopRight, mBottomLeft, mBottomRight;
		PrismRectangle mTexturePosition;
		SimDrawData mData;
		float mZ;
		bool operator<(const SimSprite& o) const { return mZ < o.mZ; }
	};
	static std::multiset<SimSprite> gSimList;
	static const int SHOT_W = 400, SHOT_H = 240;

	static int isShotFrame(unsigned long tFrame) {
		static std::set<unsigned long>* frames = NULL;
		if (!frames) {
			frames = new std::set<unsigned long>();
			const char* q = getenv("N3DS_SIM_SHOT");
			while (q && *q) { frames->insert(strtoul(q, (char**)&q, 10)); while (*q == ',' || *q == ' ') q++; }
		}
		return frames->count(tFrame) > 0;
	}

	static void simDrawingIdentity() {
		gDraw.r = gDraw.g = gDraw.b = gDraw.a = 1;
		gDraw.rOffset = gDraw.gOffset = gDraw.bOffset = 0;
		gDraw.mBlendType = BLEND_TYPE_NORMAL;
		gDraw.mIsColorSolid = gDraw.mIsColorInversed = 0;
		gDraw.mDestAlpha = 1;
		const ScreenSize sz = getScreenSize();
		const float scale = fminf(400 / (float)sz.x, SHOT_H / (float)sz.y);
		gDraw.mMatrix = makeIdentityMatrix4D();
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createTranslationMatrix4D(Vector3D(floorf((SHOT_W - sz.x * scale) / 2), floorf((SHOT_H - sz.y * scale) / 2), 0)));
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createScaleMatrix4D(Vector3D(scale, scale, 1)));
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

	void initDrawing() { for (int i = 0; i < 4; i++) gN3DSPaletteVersions[i] = 0; simDrawingIdentity(); }

	void drawSpriteNoRectangle(const TextureData& tTexture, const Position& tTopLeft, const Position& tTopRight, const Position& tBottomLeft, const Position& tBottomRight, const PrismRectangle& tTexturePosition) {
		if (gIsDrawingDisabled) return;
		drawSimulated(tTexture);
		if (!isShotFrame(gSim.mFrames)) return;
		SimSprite e;
		e.mTexture = tTexture;
		e.mTopLeft = tTopLeft.xy(); e.mTopRight = tTopRight.xy(); e.mBottomLeft = tBottomLeft.xy(); e.mBottomRight = tBottomRight.xy();
		e.mTexturePosition = tTexturePosition;
		e.mData = gDraw;
		e.mZ = tTopLeft.z;
		gSimList.insert(e);
	}

	void drawSprite(const TextureData& tTexture, const Position& tPos, const PrismRectangle& tTexturePosition) {
		const auto sizeX = abs(tTexturePosition.bottomRight.x - tTexturePosition.topLeft.x) + 1;
		const auto sizeY = abs(tTexturePosition.bottomRight.y - tTexturePosition.topLeft.y) + 1;
		drawSpriteNoRectangle(tTexture, tPos, Vector3D(tPos.x + sizeX, tPos.y, tPos.z), Vector3D(tPos.x, tPos.y + sizeY, tPos.z), Vector3D(tPos.x + sizeX, tPos.y + sizeY, tPos.z), tTexturePosition);
	}
	void drawMultilineText(const char*, const char*, const Position&, const Vector3D&, Color, const Vector3D&, const Vector3D&) {}
	void drawTruetypeText(const char*, TruetypeFont, const Position&, const Vector3DI&, const Vector3D&, float, const GeoRectangle2D&) {}

	static const u8 gMorton[64] = { 0,1,4,5,16,17,20,21, 2,3,6,7,18,19,22,23, 8,9,12,13,24,25,28,29, 10,11,14,15,26,27,30,31, 32,33,36,37,48,49,52,53, 34,35,38,39,50,51,54,55, 40,41,44,45,56,57,60,61, 42,43,46,47,58,59,62,63 };

	static void sampleTexel(const C3D_Tex& t, int x, int y, float* rgba) {
		if (x < 0) x = 0; if (y < 0) y = 0; if (x >= t.width) x = t.width - 1; if (y >= t.height) y = t.height - 1;
		const int flipped = t.height - 1 - y;
		const u32 index = (flipped >> 3) * (t.width >> 3) * 64 + ((x >> 3) << 6) + gMorton[(flipped & 7) * 8 + (x & 7)];
		if (t.fmt == GPU_RGBA8) {
			const u32 c = ((const u32*)t.data)[index];
			rgba[0] = (c >> 24) / 255.0f; rgba[1] = ((c >> 16) & 255) / 255.0f; rgba[2] = ((c >> 8) & 255) / 255.0f; rgba[3] = (c & 255) / 255.0f;
		}
		else if (t.fmt == GPU_RGBA4) {
			const u16 c = ((const u16*)t.data)[index];
			rgba[0] = (c >> 12) / 15.0f; rgba[1] = ((c >> 8) & 15) / 15.0f; rgba[2] = ((c >> 4) & 15) / 15.0f; rgba[3] = (c & 15) / 15.0f;
		}
		else {
			const u16 c = ((const u16*)t.data)[index];
			rgba[0] = (c >> 11) / 31.0f; rgba[1] = ((c >> 6) & 31) / 31.0f; rgba[2] = ((c >> 1) & 31) / 31.0f; rgba[3] = (float)(c & 1);
		}
	}

	static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

	static void rasterTriangle(float* fb, const SimSprite& e, const C3D_Tex& t, const float px[3], const float py[3], const float pu[3], const float pv[3]) {
		const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
		if (fabsf(area) < 1e-6f) return;
		int x0 = (int)floorf(fminf(px[0], fminf(px[1], px[2]))), x1 = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
		int y0 = (int)floorf(fminf(py[0], fminf(py[1], py[2]))), y1 = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
		if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > SHOT_W) x1 = SHOT_W; if (y1 > SHOT_H) y1 = SHOT_H;
		const SimDrawData& d = e.mData;
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
			const float cx = x + 0.5f, cy = y + 0.5f;
			const float w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
			const float w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
			const float w2 = 1 - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0) continue;
			const float u = w0 * pu[0] + w1 * pu[1] + w2 * pu[2], v = w0 * pv[0] + w1 * pv[1] + w2 * pv[2];
			float tex[4];
			sampleTexel(t, (int)floorf(u * t.width), (int)floorf(v * t.height), tex);
			float c[3], alpha = tex[3] * clamp01(d.a);
			if (d.mBlendType != BLEND_TYPE_ONE && tex[3] <= 0) continue;
			if (d.mIsColorSolid) { c[0] = clamp01(d.r); c[1] = clamp01(d.g); c[2] = clamp01(d.b); }
			else {
				const float mul[3] = { clamp01(d.r), clamp01(d.g), clamp01(d.b) }, off[3] = { d.rOffset, d.gOffset, d.bOffset };
				for (int i = 0; i < 3; i++) c[i] = clamp01((d.mIsColorInversed ? 1 - tex[i] : tex[i]) * mul[i] + off[i]);
			}
			float* o = fb + (y * SHOT_W + x) * 3;
			for (int i = 0; i < 3; i++) {
				if (d.mBlendType == BLEND_TYPE_ADDITION) o[i] = clamp01(c[i] * alpha + o[i] * clamp01(d.mDestAlpha));
				else if (d.mBlendType == BLEND_TYPE_SUBTRACTION) o[i] = clamp01(o[i] * clamp01(d.mDestAlpha) - c[i] * alpha);
				else if (d.mBlendType == BLEND_TYPE_ONE) o[i] = c[i];
				else o[i] = c[i] * alpha + o[i] * (1 - alpha);
			}
		}
	}

	static void writeShot() {
		std::vector<float> fb(SHOT_W * SHOT_H * 3, 0.0f);
		for (const auto& e : gSimList) {
			Texture texture = (Texture)e.mTexture.mTexture->mData;
			prepareN3DSTexture(e.mTexture);
			if (!texture->mIsInitialized || !texture->mTex.data) continue;
			const C3D_Tex& t = texture->mTex;
			const float tw = t.width, th = t.height;
			const PrismRectangle& r = e.mTexturePosition;
			float u1, u2, v1, v2;
			if (r.topLeft.x < r.bottomRight.x) { u1 = r.topLeft.x / tw; u2 = (r.bottomRight.x + 1) / tw; } else { u1 = (r.topLeft.x + 1) / tw; u2 = r.bottomRight.x / tw; }
			if (r.topLeft.y < r.bottomRight.y) { v1 = r.topLeft.y / th; v2 = (r.bottomRight.y + 1) / th; } else { v1 = (r.topLeft.y + 1) / th; v2 = r.bottomRight.y / th; }
			Position p[4];
			const Position2D corners[4] = { e.mTopLeft, e.mBottomLeft, e.mTopRight, e.mBottomRight };
			for (int i = 0; i < 4; i++) { p[i] = rotateScaleTranslatePositionByMatrix4D(e.mData.mMatrix, corners[i].xyz(0)); p[i].x = floorf(p[i].x); p[i].y = floorf(p[i].y); }
			const float ax[3] = { (float)p[0].x, (float)p[1].x, (float)p[2].x }, ay[3] = { (float)p[0].y, (float)p[1].y, (float)p[2].y };
			const float au[3] = { u1, u1, u2 }, av[3] = { v1, v2, v1 };
			rasterTriangle(fb.data(), e, t, ax, ay, au, av);
			const float bx[3] = { (float)p[2].x, (float)p[1].x, (float)p[3].x }, by[3] = { (float)p[2].y, (float)p[1].y, (float)p[3].y };
			const float bu[3] = { u2, u1, u2 }, bv[3] = { v1, v2, v2 };
			rasterTriangle(fb.data(), e, t, bx, by, bu, bv);
			if (getenv("N3DS_SIM_SHOT_LIST")) fprintf(stderr, "n3dssim: sprite z %.1f tex %dx%d at (%.0f,%.0f)-(%.0f,%.0f) rect (%.0f,%.0f)-(%.0f,%.0f) blend %d a %.2f dst %.2f solid %d rgb %.2f,%.2f,%.2f off %.2f,%.2f,%.2f\n", e.mZ, e.mTexture.mTextureSize.x, e.mTexture.mTextureSize.y, (double)p[0].x, (double)p[0].y, (double)p[3].x, (double)p[3].y, (double)r.topLeft.x, (double)r.topLeft.y, (double)r.bottomRight.x, (double)r.bottomRight.y, (int)e.mData.mBlendType, e.mData.a, e.mData.mDestAlpha, e.mData.mIsColorSolid, e.mData.r, e.mData.g, e.mData.b, e.mData.rOffset, e.mData.gOffset, e.mData.bOffset);
		}
		const char* dir = getenv("N3DS_SIM_SHOT_DIR");
		char path[1024];
		snprintf(path, sizeof(path), "%s/shot%05lu.ppm", dir ? dir : ".", gSim.mFrames);
		FILE* f = fopen(path, "wb");
		if (!f) return;
		fprintf(f, "P6\n%d %d\n255\n", SHOT_W, SHOT_H);
		for (size_t i = 0; i < fb.size(); i++) fputc((int)(fb[i] * 255 + 0.5f), f);
		fclose(f);
		fprintf(stderr, "n3dssim: picture %s (%zu sprites)\n", path, gSimList.size());
	}

	void waitForScreen() {}
	void startDrawing() { gSim.mFrames++; advanceN3DSTextureFrame(); gSimList.clear(); }
	void stopDrawing() {
		if (isShotFrame(gSim.mFrames) && !gSimList.empty()) writeShot();
		gSimList.clear();
		issueN3DSTextureReadAhead();
	}
	void waitForRendering() {}

	bool isSkippingDrawing() { return false; }
	void setDrawingFrameSkippingEnabled(bool) {}
	void resetDrawingFrameStartTime() {}
	void disableDrawing() { gIsDrawingDisabled = 1; }
	void enableDrawing() { gIsDrawingDisabled = 0; }

	void scaleDrawing(float tFactor, const Position& tScalePosition) { scaleDrawing3D(Vector3D(tFactor, tFactor, 1), tScalePosition); }
	void scaleDrawing2D(const Vector2D& tFactor, const Position2D& tScalePosition) { scaleDrawing3D(tFactor.xyz(1.0), tScalePosition.xyz(0.0)); }
	void scaleDrawing3D(const Vector3D& tFactor, const Position& tScalePosition) {
		const auto p = Vector3D(tScalePosition.x, tScalePosition.y, 0);
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createTranslationMatrix4D(p));
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createScaleMatrix4D(Vector3D(tFactor.x, tFactor.y, tFactor.z)));
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createTranslationMatrix4D(vecScale(p, -1)));
	}

	void setDrawingBaseColorOffsetAdvanced(float r, float g, float b) { gDraw.rOffset = r; gDraw.gOffset = g; gDraw.bOffset = b; }
	void setDrawingBaseColor(Color tColor) { getRGBFromColor(tColor, &gDraw.r, &gDraw.g, &gDraw.b); }
	void setDrawingBaseColorAdvanced(float r, float g, float b) { gDraw.r = r; gDraw.g = g; gDraw.b = b; }
	void setDrawingColorSolidity(int tIsSolid) { gDraw.mIsColorSolid = tIsSolid; }
	void setDrawingColorInversed(int tIsInversed) { gDraw.mIsColorInversed = tIsInversed; }
	void setDrawingColorFactor(float) {}
	void setDrawingTransparency(float tAlpha) { gDraw.a = tAlpha; }
	void setDrawingDestinationTransparency(float tAlpha) { gDraw.mDestAlpha = tAlpha; }
	void setDrawingRotationZ(float tAngle, const Position2D& tPosition) { setDrawingRotationZ(tAngle, tPosition.xyz(0.0)); }
	void setDrawingRotationZ(float tAngle, const Position& tPosition) {
		tAngle = (2 * M_PI - tAngle);
		const auto p = Vector3D(tPosition.x, tPosition.y, 0);
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createTranslationMatrix4D(p));
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createRotationZMatrix4D(tAngle));
		gDraw.mMatrix = matMult4D(gDraw.mMatrix, createTranslationMatrix4D(vecScale(p, -1)));
	}
	void setDrawingParametersToIdentity() { simDrawingIdentity(); }
	void setDrawingBlendType(BlendType tBlendType) { gDraw.mBlendType = tBlendType; }

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
