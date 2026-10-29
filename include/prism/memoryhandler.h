#pragma once

#include <stdlib.h>
#include <string_view>

#ifdef DREAMCAST
#include <kos.h>

namespace prism {
typedef pvr_ptr_t Texture;

#elif defined(__3DS__) || defined(N3DS_TEXTURE_SIM)
#include <citro3d.h>

namespace prism {

// [3DS] A texture on the GPU (power-of-two sizes, at most 1024). Paletted
// images keep their 8-bit pixels, compressed: the GPU has no palettes, so
// they are converted with their palette when drawn after it changed, and
// their texels can be let go when memory runs short and made again when
// drawn (texture_3ds.cpp).
typedef struct {
	C3D_Tex mTex;
	int mIsInitialized;
	unsigned char* mIndexed;
	int mIndexedSize;
	int mOwnPalette;   // its own palette (texture_3ds.cpp's table), or -1: prism's
	int mPaletteVersion;
	unsigned int mLastUsedFrame;
} N3DSTextureData;

typedef N3DSTextureData* Texture;

#elif defined __linux__

namespace prism {

typedef struct {
	unsigned int mTexture;
} GLTextureData;

typedef GLTextureData* Texture;

#elif defined _WIN32 || defined __EMSCRIPTEN__
#include <SDL.h>
#include <GL/glew.h>

namespace prism {

typedef struct {
	GLuint mTexture;
} GLTextureData;

typedef GLTextureData* Texture;

#elif defined(VITA)

#include <vita2d.h>
#include <psp2/gxm.h>
#include <psp2/types.h>
#include <psp2/kernel/sysmem.h>

namespace prism {

	typedef struct {
		vita2d_texture* mTexture;
} VitaTextureData;

typedef VitaTextureData* Texture;

void* vitaGpuAlloc(SceKernelMemBlockType type, unsigned int size, unsigned int alignment, unsigned int attribs, SceUID* uid);
void vitaGpuFree(SceUID uid);
void releaseVitaTextureDataFromSlab(vita2d_texture* tTexture);

#endif

struct TextureMemory_internal {
	void* mData;
	size_t mSize;
	size_t mCompressedSize;

	int mIsVirtual;
	int mIsCompressed;

	struct TextureMemory_internal* mPrevInUsageList;
	struct TextureMemory_internal* mNextInUsageList;
};

typedef struct TextureMemory_internal* TextureMemory;

void* allocMemory(int tSize);
void* allocClearedMemory(int tBlockAmount, int tBlockSize);
void freeMemory(void* tData);
void* reallocMemory(void* tData, int tSize);
TextureMemory allocTextureMemory(int tSize);
void freeTextureMemory(TextureMemory tMem);
void referenceTextureMemory(TextureMemory tMem);

void pushMemoryStack();
void popMemoryStack();
void pushTextureMemoryStack();
void popTextureMemoryStack();

void initMemoryHandler();
void shutdownMemoryHandler();
void setMemoryHandlingInactive();

void setMemoryHandlerCompressionActive();
void setMemoryHandlerCompressionInactive();

void debugPrintMemoryStack();
int getAvailableTextureMemory();
int getAvailableSoundMemory();

int getAllocatedMemoryBlockAmount();

#ifdef _WIN32
void imguiMemoryHandler();
void imguiTextureMemory(const std::string_view& tName, const TextureMemory& tTextureMemory);
#endif

}