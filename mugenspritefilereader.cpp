#include "prism/mugenspritefilereader.h"

#include <assert.h>
#include <string.h>
#include <algorithm>
#include <set>

#ifdef DREAMCAST
#include <png/png.h>
#else
#include <png.h>
#endif

#include "prism/file.h"
#include "prism/filereader.h"
#include "prism/log.h"
#include "prism/system.h"
#include "prism/texture.h"
#include "prism/math.h"
#include "prism/drawing.h"
#include "prism/compression.h"
#include "prism/stlutil.h"

#include "prism/lz5.h"

#ifdef __3DS__
#include "3ds/prism3dsperf.h"
#include <malloc.h>
#endif
#if defined(__3DS__) || defined(N3DS_TEXTURE_SIM)
#include "3ds/prism3ds.h"
#include <malloc.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
// The heap guard, on the 3DS and in its simulation.
#define N3DS_HEAP_GUARD
#endif

#ifdef _WIN32
#include <imgui/imgui.h>
#include "prism/windows/debugimgui_win.h"
#endif

using namespace std;

namespace prism {

typedef struct {
	char mSignature[12];
	uint8_t mVersion[4];
} SFFSharedHeader;

typedef struct {
	char mSignature[12];
	char mVersion[4];
	
	uint32_t mReserved1;
	uint32_t mReserved2;

	uint8_t mCompatibleVersion[4];

	uint32_t mReserved3;
	uint32_t mReserved4;

	uint32_t mSpriteOffset;
	uint32_t mSpriteTotal;

	uint32_t mPaletteOffset;
	uint32_t mPaletteTotal;

	uint32_t mLDataOffset;
	uint32_t mLDataLength;

	uint32_t mTDataOffset;
	uint32_t mTDataLength;

	uint32_t mReserved5;
	uint32_t mReserved6;

	char mComments[436];
} SFFHeader2;

typedef struct {
	uint16_t mGroupNo;
	uint16_t mItemNo;
	uint16_t mNumCols;
	uint16_t mIndex;

	uint32_t mDataOffset;
	uint32_t mDataLength;
} SFFPalette2;

typedef struct {
	uint16_t mGroupNo;
	uint16_t mItemNo;
	uint16_t mWidth;
	uint16_t mHeight;
	
	int16_t mAxisX;
	int16_t mAxisY;

	uint16_t mIndex;

	uint8_t mFormat;
	uint8_t mColorDepth;

	uint32_t mDataOffset;
	uint32_t mDataLength;

	uint16_t mPaletteIndex;
	uint16_t mFlags;
} SFFSprite2;

typedef struct {
	char mSignature[12];
	char mVersion[4];
	int32_t mGroupAmount;
	int32_t mImageAmount;
	int32_t mFirstFileOffset;
	int32_t mSubheaderSize;
	int8_t mPaletteType;
	char mBlank[3];
	char mComments[476];
} SFFHeader;


typedef struct {
	int32_t mNextFilePosition;
	int32_t mSubfileLength;
	int16_t mImageAxisXCoordinate;
	int16_t mImageAxisYCoordinate;
	int16_t mGroup;
	int16_t mImage;
	int16_t mIndexOfPreciousSpriteCopy;
	int8_t mHasSamePaletteAsPreviousImage;
	char mComments[12];
} SFFSubFileHeader;

typedef struct {
	uint8_t mZSoftID;
	uint8_t mVersion;
	uint8_t mEncoding;
	uint8_t mBitsPerPixel;

	int16_t mMinX;
	int16_t mMinY;
	int16_t mMaxX;
	int16_t mMaxY;

	int16_t mHorizontalResolution;
	int16_t mVerticalResolution;

	uint8_t mHeaderPalette[48];
	uint8_t mReserved;
	uint8_t mPlaneAmount;
	uint16_t mBytesPerLine;
	uint16_t mPaletteInfo;
	char mFiller[58];
} PCXHeader;

static struct {
	int mIsOnlyLoadingPortraits;
	int mHasPaletteFile;
	int mIsUsingRealPalette;
	int mPaletteID;
	FileReader mReader;

	TextureData(*mCustomLoadTextureFromARGB16Buffer)(const Buffer&, int, int);
	TextureData(*mCustomLoadTextureFromARGB32Buffer)(const Buffer&, int, int);
	TextureData(*mCustomLoadPalettedTextureFrom8BitBuffer)(const Buffer&, int, int, int);
	int mIsWritingDreamcastOptimized;

	// default values chosen with dolmexica baldhead character special attack stress test, equal values ensure no texture memory fragmentation when only using mugen sprites
	int mSubTextureSplitMin = 64;
	int mSubTextureSplitMax = 64;
} gPrismMugenSpriteFileReaderData;

static uint32_t get2DBufferIndex(uint32_t i, uint32_t j, uint32_t w) {
	return j * w + i;
}

static int isRGBPalette(const Buffer& b)
{
	return b.mLength == 256 * 3;
}

static int isARGBPalette(const Buffer& b)
{
	return b.mLength == 256 * 4;
}

static TextureData loadTextureFromPalettedImageData1bppTo16ARGB(const Buffer& tPCXImageBuffer, const Buffer& tPaletteBuffer, int w, int h) {
	uint8_t* output = (uint8_t*)allocMemory(w*h * 2);
	uint8_t* img = (uint8_t*)tPCXImageBuffer.mData;
	uint8_t* pal = (uint8_t*)tPaletteBuffer.mData;

	if (isRGBPalette(tPaletteBuffer))
	{
		int i, j;
		for (j = 0; j < h; j++) {
			for (i = 0; i < w; i++) {
				uint8_t pid = img[get2DBufferIndex(i, j, w)];
				uint8_t a = ((uint8_t)(pid == 0 ? 0 : 0xFF)) >> 4;
				uint8_t r = ((uint8_t)pal[pid * 3 + 0]) >> 4;
				uint8_t g = ((uint8_t)pal[pid * 3 + 1]) >> 4;
				uint8_t b = ((uint8_t)pal[pid * 3 + 2]) >> 4;

				output[get2DBufferIndex(i, j, w) * 2 + 0] = (g << 4) | b;
				output[get2DBufferIndex(i, j, w) * 2 + 1] = (a << 4) | r;
			}
		}
	}
	else
	{
		int i, j;
		for (j = 0; j < h; j++) {
			for (i = 0; i < w; i++) {
				uint8_t pid = img[get2DBufferIndex(i, j, w)];
				uint8_t a = ((uint8_t)pal[pid * 4 + 0]) >> 4;
				uint8_t r = ((uint8_t)pal[pid * 4 + 1]) >> 4;
				uint8_t g = ((uint8_t)pal[pid * 4 + 2]) >> 4;
				uint8_t b = ((uint8_t)pal[pid * 4 + 3]) >> 4;

				output[get2DBufferIndex(i, j, w) * 2 + 0] = (g << 4) | b;
				output[get2DBufferIndex(i, j, w) * 2 + 1] = (a << 4) | r;
			}
		}
	}

	Buffer b = makeBufferOwned(output, w*h * 2);

	TextureData ret;
	if (gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB16Buffer) {
		ret = gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB16Buffer(b, w, h);

	}
	else {
		ret = loadTextureFromARGB16Buffer(b, w, h);
	}

	freeBuffer(b);

	return ret;
}

static TextureData loadTextureFromPalettedImageData1bppTo32ARGB(const Buffer& tPCXImageBuffer, const Buffer& tPaletteBuffer, int w, int h) {
	uint8_t* output = (uint8_t*)allocMemory(w*h * 4);
	uint8_t* img = (uint8_t*)tPCXImageBuffer.mData;
	uint8_t* pal = (uint8_t*)tPaletteBuffer.mData;

	if (isRGBPalette(tPaletteBuffer))
	{
		int i, j;
		for (j = 0; j < h; j++) {
			for (i = 0; i < w; i++) {
				uint8_t pid = img[get2DBufferIndex(i, j, w)];
				output[get2DBufferIndex(i, j, w) * 4 + 0] = pal[pid * 3 + 2];
				output[get2DBufferIndex(i, j, w) * 4 + 1] = pal[pid * 3 + 1];
				output[get2DBufferIndex(i, j, w) * 4 + 2] = pal[pid * 3 + 0];
				output[get2DBufferIndex(i, j, w) * 4 + 3] = pid == 0 ? 0 : 0xFF;
			}
		}
	}
	else
	{
		int i, j;
		for (j = 0; j < h; j++) {
			for (i = 0; i < w; i++) {
				uint8_t pid = img[get2DBufferIndex(i, j, w)];
				output[get2DBufferIndex(i, j, w) * 4 + 0] = pal[pid * 4 + 3];
				output[get2DBufferIndex(i, j, w) * 4 + 1] = pal[pid * 4 + 2];
				output[get2DBufferIndex(i, j, w) * 4 + 2] = pal[pid * 4 + 1];
				output[get2DBufferIndex(i, j, w) * 4 + 3] = pal[pid * 4 + 0];
			}
		}
	}

	Buffer b = makeBufferOwned(output, w*h * 4);

	TextureData ret;
	if (gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB32Buffer) {
		ret = gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB32Buffer(b, w, h);
	}
	else {
		ret = loadTextureFromARGB32Buffer(b, w, h);
	}

	freeBuffer(b);

	return ret;
}

static TextureData loadTextureFromPalettedImageData1bpp(const Buffer& tPCXImageBuffer, const Buffer& tPaletteBuffer, int w, int h) {
#if defined(__3DS__) || defined(N3DS_TEXTURE_SIM)
	// [3DS] A paletted texture with its own palette (half the memory of
	// ARGB32, and let go and made again when memory runs short), when its
	// colors are all or nothing transparent (always so in RGB palettes).
	{
		const uint8_t* pal = (const uint8_t*)tPaletteBuffer.mData;
		u32 colors[256];
		int isAllOrNothing = 1;
		if (isRGBPalette(tPaletteBuffer)) {
			for (int i = 0; i < 256; i++) colors[i] = ((u32)pal[i * 3 + 0] << 24) | ((u32)pal[i * 3 + 1] << 16) | ((u32)pal[i * 3 + 2] << 8) | (i ? 0xFF : 0);
		}
		else {
			for (int i = 0; i < 256; i++) {
				colors[i] = ((u32)pal[i * 4 + 1] << 24) | ((u32)pal[i * 4 + 2] << 16) | ((u32)pal[i * 4 + 3] << 8) | pal[i * 4 + 0];
				if (pal[i * 4 + 0] != 0 && pal[i * 4 + 0] != 0xFF) isAllOrNothing = 0;
			}
		}
		if (isAllOrNothing) return loadN3DSTextureWithOwnPalette(tPCXImageBuffer, colors, w, h);
	}
#endif
	if (isOnDreamcast() || gPrismMugenSpriteFileReaderData.mIsWritingDreamcastOptimized) {
		return loadTextureFromPalettedImageData1bppTo16ARGB(tPCXImageBuffer, tPaletteBuffer, w, h);
	}
	else {
		return loadTextureFromPalettedImageData1bppTo32ARGB(tPCXImageBuffer, tPaletteBuffer, w, h);
	}
}

typedef struct {
	Buffer mBuffer;
	Vector2DI mOffset;
	Vector2DI mSize;
} SubImageBuffer;

static std::vector<MugenSpriteFileSubSprite> loadTextureFromPalettedImageList1bpp(const std::vector<SubImageBuffer>& tBufferList, const Buffer& tPaletteBuffer) {
	vector<MugenSpriteFileSubSprite> ret;

	for (const auto& buffer : tBufferList)
	{
		MugenSpriteFileSubSprite newSprite;
		newSprite.mOffset = buffer.mOffset;
		newSprite.mTexture = loadTextureFromPalettedImageData1bpp(buffer.mBuffer, tPaletteBuffer, buffer.mSize.x, buffer.mSize.y);
		ret.push_back(newSprite);
	}

	return ret;
}

static std::vector<MugenSpriteFileSubSprite> loadTextureFromRawImageList1bpp(const std::vector<SubImageBuffer>& tBufferList) {
	std::vector<MugenSpriteFileSubSprite> ret;

	for (const auto& buffer : tBufferList)
	{
		MugenSpriteFileSubSprite newSprite;
		newSprite.mOffset = buffer.mOffset;
		if (gPrismMugenSpriteFileReaderData.mCustomLoadPalettedTextureFrom8BitBuffer) {
			newSprite.mTexture = gPrismMugenSpriteFileReaderData.mCustomLoadPalettedTextureFrom8BitBuffer(buffer.mBuffer, gPrismMugenSpriteFileReaderData.mPaletteID, buffer.mSize.x, buffer.mSize.y);
		}
		else {
			newSprite.mTexture = loadPalettedTextureFrom8BitBuffer(buffer.mBuffer, gPrismMugenSpriteFileReaderData.mPaletteID, buffer.mSize.x, buffer.mSize.y);
		}
		ret.push_back(newSprite);
	}

	return ret;
}

static std::vector<MugenSpriteFileSubSprite> loadTextureFromImageARGB32List(const std::vector<SubImageBuffer>& tBufferList) {
	std::vector<MugenSpriteFileSubSprite> ret;

	for (const auto& buffer : tBufferList)
	{
		MugenSpriteFileSubSprite newSprite;
		newSprite.mOffset = buffer.mOffset;

		if (gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB32Buffer) {
			newSprite.mTexture = gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB32Buffer(buffer.mBuffer, buffer.mSize.x, buffer.mSize.y);
		}
		else {
			newSprite.mTexture = loadTextureFromARGB32Buffer(buffer.mBuffer, buffer.mSize.x, buffer.mSize.y);
		}
		ret.push_back(newSprite);
	}

	return ret;
}

static Buffer decodeRLE5BufferAndReturnOwnedBuffer(const Buffer& b, int tFinalSize) {
	uint8_t* output = (uint8_t*)allocMemory(tFinalSize+10);
	uint8_t* input = (uint8_t*)b.mData;

	int ip;
	int op = 0;
	for (ip = 0; ip < (int)b.mLength && op < tFinalSize; ip++) {
		uint8_t cur = input[ip];

		if ((cur & 0xC0) == 0xC0) { // decode
			int steps = cur & 0x3F;
			ip++;
			if (ip >= (int)b.mLength) break;
			const int amount = min(steps, tFinalSize - op);
			memset(output + op, input[ip], amount);
			op += amount;
		}
		else {
			output[op++] = cur;
		}
	}

	return makeBufferOwned(output, tFinalSize);
}

static Buffer decodeRLE8BufferAndReturnOwnedBuffer(const Buffer& b, int tFinalSize) {
	uint8_t* output = (uint8_t*)allocMemory(tFinalSize + 10);
	uint8_t* input = (uint8_t*)b.mData;

	uint32_t dstpos = 0;
	uint32_t srcpos = 0;

	while (srcpos < (uint32_t)b.mLength && dstpos < (uint32_t)tFinalSize)
	{
		if (((input[srcpos] & 0xC0) == 0x40))
		{
			if (srcpos + 1 >= (uint32_t)b.mLength) break;
			const int runLength = input[srcpos] & 0x3F;
			int run;
			for (run = 0; run < runLength && dstpos < (uint32_t)tFinalSize; run++)
			{
				output[dstpos] = input[srcpos + 1];
				dstpos++;
			}
			srcpos += 2;
		}
		else
		{
			output[dstpos] = input[srcpos];
			dstpos++;
			srcpos++;
		}
	}

	return makeBufferOwned(output, tFinalSize);
}

static MugenSpriteFileSprite makeMugenSpriteFileSprite(const std::vector<MugenSpriteFileSubSprite>& tTextures, const TextureSize& tOriginalTextureSize, const Vector2D& tAxisOffset) {
	MugenSpriteFileSprite e;
	e.mTextures = tTextures;
	e.mOriginalTextureSize = tOriginalTextureSize;
	e.mIsLinked = 0;
	e.mIsLinkedTo = 0;
	e.mAxisOffset = tAxisOffset;
	return e;
}

static MugenSpriteFileSprite makeLinkedMugenSpriteFileSprite(int tIsLinkedTo, const Vector2D& tAxisOffset) {
	MugenSpriteFileSprite e;
	e.mIsLinked = 1;
	e.mIsLinkedTo = tIsLinkedTo;
	e.mAxisOffset = tAxisOffset;
	return e;
}

static Buffer loadPCXPaletteToBuffer(BufferPointer p, int tEncodedSize) {
	int size = 256 * 3;
	char* data = (char*)allocMemory(size);
	memcpy(data, p + tEncodedSize, size);
	return makeBufferOwned(data, size);
}

static SubImageBuffer getSingleAllocatedBufferFromSource(const Buffer& b, int x, int y, int dx, int dy, int tBytesPerLine, int tWidth, int tHeight, int tBytesPerPixel) {
	int dstSize = dx*dy;
	char* dst = (char*)allocMemory(dstSize*tBytesPerPixel);
	char* src = (char*)b.mData;

	int j;
	for (j = 0; j < dy; j++) {
		int startIndexDst = get2DBufferIndex(0, j, dx)*tBytesPerPixel;
		int startIndexSrc = get2DBufferIndex(x, y + j, tBytesPerLine)*tBytesPerPixel;

		int rowSrcSize, rowBufferSize;
		if (y + j >= tHeight) rowSrcSize = 0;
		else rowSrcSize = min(dx, tWidth - x);
		rowBufferSize = (dx - rowSrcSize);
		rowSrcSize *= tBytesPerPixel;
		rowBufferSize *= tBytesPerPixel;

		memcpy(dst + startIndexDst, src + startIndexSrc, rowSrcSize);
		memset(dst + startIndexDst + rowSrcSize, 0, rowBufferSize);
	}

	SubImageBuffer ret;
	ret.mBuffer = makeBufferOwned(dst, dstSize*tBytesPerPixel);
	ret.mOffset = Vector2DI(x, y);
	ret.mSize = Vector2DI(dx, dy);
	return ret;
}

#if defined(VITA)
// GXM cannot address a texture dimension above this, so those still have to be tiled
static const int VITA_MAXIMUM_TEXTURE_DIMENSION = 4096;
#endif

static int getMaximumSizeFit(int tVal) {
#if defined(VITA)
	return min(tVal, VITA_MAXIMUM_TEXTURE_DIMENSION);
#elif defined(__3DS__) || defined(N3DS_TEXTURE_SIM)
	// [3DS] Pieces of the image's own size, the texture padding them to
	// powers of two, when that wastes at most half: a CvS sprite in 2.2
	// pieces instead of 9 (each piece a draw), for 14% more texels.
	if (tVal > 1024) return 1024;
	int powerOfTwo = 8;
	while (powerOfTwo < tVal) powerOfTwo *= 2;
	if (powerOfTwo * 2 <= tVal * 3 || tVal <= 8) return tVal;
	return powerOfTwo / 2;
#else
	const int mini = gPrismMugenSpriteFileReaderData.mSubTextureSplitMin;
	const int maxi = gPrismMugenSpriteFileReaderData.mSubTextureSplitMax;

	if (tVal <= mini) return mini;

	int i = mini;
	while(i < 5000) {
		if (i <= tVal && (i * 2 > tVal || i == maxi)) {
			return i;
		}

		i *= 2;
	}

	logError("Unable to find fit");
	logErrorInteger(tVal);
	recoverFromError();
	return 0;
#endif
}

std::vector<SubImageBuffer> breakImageBufferUpIntoMultipleBuffers(const Buffer& b, int tBytesPerLine, int tWidth, int tHeight, int tBytesPerPixel) {
	std::vector<SubImageBuffer> ret;

	int y = 0;
	while (y < tHeight) { // INFO: Need power of 2 split for paletted images on Dreamcast, they need to be twiddled and twiddling only works with power of 2
		int heightLeft = tHeight - y;
		int dy = getMaximumSizeFit(heightLeft);
		int x = 0;
		while (x < tWidth) {
			int widthLeft = tWidth - x;
			int dx = getMaximumSizeFit(widthLeft);
			auto newBuffer = getSingleAllocatedBufferFromSource(b, x, y, dx, dy, tBytesPerLine, tWidth, tHeight, tBytesPerPixel);
			ret.push_back(newBuffer);
			x += dx;
		}
		y += dy;
	}

	return ret;
}

static void freeSubImageBufferList(std::vector<SubImageBuffer>& tBufferList) {
	for (auto& buffer : tBufferList)
	{
		freeBuffer(buffer.mBuffer);
	}
	tBufferList.clear();
}

typedef struct {
	BufferPointer p;
	Buffer b;
} PNGReadCaller;

static void readPNGDataFromInputStream(png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
	png_voidp io_ptr = png_get_io_ptr(png_ptr);
	if (io_ptr == NULL) {
		logError("Did not get caller");
		recoverFromError();
	}

	PNGReadCaller* caller = (PNGReadCaller*)io_ptr;
	if (((uintptr_t)caller->p) + byteCountToRead > ((uintptr_t)caller->b.mData) + caller->b.mLength) {
		logError("Trying to read outside buffer");
		recoverFromError();
	}
	readFromBufferPointer(outBytes, &caller->p, uint32_t(byteCountToRead));
}

static Buffer parseRGBAPNG(png_structp* png_ptr, png_infop* info_ptr, int tHasAlpha, int width, int height)
{
	uint8_t* dst = (uint8_t*)allocMemory(width*height * 4);

	const auto bytesPerRow = png_get_rowbytes(*png_ptr, *info_ptr);
	char* rowData = (char*)allocMemory(int(bytesPerRow));

	// read single row at a time
	uint32_t rowIdx;
	for (rowIdx = 0; rowIdx < (uint32_t)height; ++rowIdx)
	{
		png_read_row(*png_ptr, (png_bytep)rowData, NULL);

		uint32_t rowOffset = rowIdx * width;

		uint32_t byteIndex = 0;
		uint32_t colIdx;
		for (colIdx = 0; colIdx < (uint32_t)width; ++colIdx)
		{
			uint32_t targetPixelIndex = rowOffset + colIdx;
			dst[targetPixelIndex * 4 + 2] = rowData[byteIndex++];
			dst[targetPixelIndex * 4 + 1] = rowData[byteIndex++];
			dst[targetPixelIndex * 4 + 0] = rowData[byteIndex++];
			dst[targetPixelIndex * 4 + 3] = tHasAlpha ? rowData[byteIndex++] : 255;
		}
		assert(byteIndex == bytesPerRow);
	}

	freeMemory(rowData);

	return makeBufferOwned(dst, width*height*4);
}

static Buffer parsePalettedPNGWithPNGPalette(png_structp* png_ptr, png_infop* info_ptr, int width, int height)
{
	uint8_t* dst = (uint8_t*)allocMemory(width*height * 4);

	auto bytesPerRow = png_get_rowbytes(*png_ptr, *info_ptr);
	uint8_t* rowData = (uint8_t*)allocMemory(int(bytesPerRow));

	png_colorp pngPalette;
	int palAmount;
	png_get_PLTE(*png_ptr, *info_ptr, &pngPalette, &palAmount);
	assert(palAmount <= 256 * 3);

	png_bytep transAlpha = NULL;
	int transAmount = 0;
	png_color_16p transColor = NULL;

	png_get_tRNS(*png_ptr, *info_ptr, &transAlpha, &transAmount, &transColor);
	uint32_t rowIdx;
	for (rowIdx = 0; rowIdx < (uint32_t)height; ++rowIdx)
	{
		png_read_row(*png_ptr, (png_bytep)rowData, NULL);

		uint32_t rowOffset = rowIdx * width;
		uint32_t byteIndex = 0;
		uint32_t colIdx;
		for (colIdx = 0; colIdx < (uint32_t)width; ++colIdx)
		{
			uint32_t targetPixelIndex = rowOffset + colIdx;
			int index = rowData[byteIndex++];
			assert(index < palAmount);
			dst[targetPixelIndex * 4 + 2] = pngPalette[index].red;
			dst[targetPixelIndex * 4 + 1] = pngPalette[index].green;
			dst[targetPixelIndex * 4 + 0] = pngPalette[index].blue;
			dst[targetPixelIndex * 4 + 3] = ((transAmount == 0) ? 255 : (index >= transAmount) ? 255 : transAlpha[index]);
		}
		assert(byteIndex == bytesPerRow);
	}

	freeMemory(rowData);

	return makeBufferOwned(dst, width*height * 4);
}

static Buffer parsePalettedPNGWithoutPalette(png_structp* png_ptr, png_infop* info_ptr, int width, int height)
{
	uint8_t* dst = (uint8_t*)allocMemory(width * height);

	auto bytesPerRow = png_get_rowbytes(*png_ptr, *info_ptr);
	uint8_t* rowData = (uint8_t*)allocMemory(int(bytesPerRow));

	uint32_t rowIdx;
	for (rowIdx = 0; rowIdx < (uint32_t)height; ++rowIdx)
	{
		png_read_row(*png_ptr, (png_bytep)rowData, NULL);

		uint32_t rowOffset = rowIdx * width;
		uint32_t byteIndex = 0;
		uint32_t colIdx;
		for (colIdx = 0; colIdx < (uint32_t)width; ++colIdx)
		{
			uint32_t targetPixelIndex = rowOffset + colIdx;
			int index = rowData[byteIndex++];
			assert(index <= 0xFF);
			dst[targetPixelIndex] = index;
		}
		assert(byteIndex == bytesPerRow);
	}

	freeMemory(rowData);

	return makeBufferOwned(dst, width * height);
}

static Buffer loadBufferFromRawPNGBuffer(const Buffer& tRawPNGBuffer, int tWidth, int tHeight, int tIsUsingPngPalette) {
	BufferPointer p = getBufferPointer(tRawPNGBuffer);
	
	uint8_t* pngSignature = (uint8_t*)p;
	p += 8;
	if (!png_check_sig(pngSignature, 8)) {
		logError("Invalid png signature");
		recoverFromError();
	}

	png_structp png_ptr = NULL;
	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

	if (png_ptr == NULL) {
		logError("Cannot create read struct.");
		recoverFromError();
	}

	png_infop info_ptr = NULL;
	info_ptr = png_create_info_struct(png_ptr);

	if (info_ptr == NULL)
	{
		png_destroy_read_struct(&png_ptr, NULL, NULL);
		logError("Cannot create info struct.");
		recoverFromError();
	}

	PNGReadCaller caller;
	caller.p = p;
	caller.b = tRawPNGBuffer;

	png_set_read_fn(png_ptr, &caller, readPNGDataFromInputStream);
	png_set_sig_bytes(png_ptr, 8);

	png_read_info(png_ptr, info_ptr);

	png_uint_32 width = 0;
	png_uint_32 height = 0;
	int bitDepth = 0;
	int colorType = -1;
	png_uint_32 retval = png_get_IHDR(png_ptr, info_ptr,
		&width,
		&height,
		&bitDepth,
		&colorType,
		NULL, NULL, NULL);
	(void)tWidth;
	assert((int)width == tWidth);
	(void)tHeight;
	assert((int)height == tHeight);

	if (retval != 1) {
		logError("Unable to read image data");
		recoverFromError();
	}

	Buffer ret;
	if (colorType == PNG_COLOR_TYPE_RGB) {
		ret = parseRGBAPNG(&png_ptr, &info_ptr, 0, width, height);
	}
	else if (colorType == PNG_COLOR_TYPE_RGB_ALPHA) {
		ret = parseRGBAPNG(&png_ptr, &info_ptr, 1, width, height);
	}
	else if (colorType == PNG_COLOR_TYPE_PALETTE) {
		if (tIsUsingPngPalette)
		{
			ret = parsePalettedPNGWithPNGPalette(&png_ptr, &info_ptr, width, height);
		}
		else
		{
			ret = parsePalettedPNGWithoutPalette(&png_ptr, &info_ptr, width, height);
		}
	}
	else {
		logError("Unrecognized color type");
		logErrorInteger(colorType);
		recoverFromError();
		ret = makeBuffer(NULL, 0);
	}
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

	return ret;
}

static MugenSpriteFileSprite makeMugenSpriteFileSpriteFromRawAndPaletteBufferGeneral(Buffer& tRawImageBuffer, const Buffer& tPaletteBuffer, int tIsUsingPaletteBuffer, int tWidth, int tHeight, int tBytesPerLine, const Vector2D& tAxisOffset) {
	auto subImageList = breakImageBufferUpIntoMultipleBuffers(tRawImageBuffer, tBytesPerLine, tWidth, tHeight, 1);
	freeBuffer(tRawImageBuffer);

	std::vector<MugenSpriteFileSubSprite> textures;
	if (tIsUsingPaletteBuffer) {
		textures = loadTextureFromPalettedImageList1bpp(subImageList, tPaletteBuffer);
	}
	else {
		textures = loadTextureFromRawImageList1bpp(subImageList);
	}
	freeSubImageBufferList(subImageList);

	return makeMugenSpriteFileSprite(textures, makeTextureSize(tWidth, tHeight), tAxisOffset);
}

static MugenSpriteFileSprite makeMugenSpriteFileSpriteFromRawAndPaletteBuffer(Buffer& tRawImageBuffer, const Buffer& tPaletteBuffer, int tWidth, int tHeight, int tBytesPerLine, const Vector2D& tAxisOffset) {
	return makeMugenSpriteFileSpriteFromRawAndPaletteBufferGeneral(tRawImageBuffer, tPaletteBuffer, 1, tWidth, tHeight, tBytesPerLine, tAxisOffset);
}

static MugenSpriteFileSprite makeMugenSpriteFileSpriteFromRawBuffer(Buffer& tRawImageBuffer, int tWidth, int tHeight, int tBytesPerLine, const Vector2D& tAxisOffset) {
	return makeMugenSpriteFileSpriteFromRawAndPaletteBufferGeneral(tRawImageBuffer, makeBuffer(NULL, 0), 0, tWidth, tHeight, tBytesPerLine, tAxisOffset);
}

static MugenSpriteFileSprite makeMugenSpriteFileSpriteFromRawPNGBuffer(Buffer& tRawPNGBuffer, int tWidth, int tHeight, int tBytesPerLine, const Vector2D& tAxisOffset, int tColorDepth, const std::vector<MugenSpriteFilePalette>& tPalettes, int tPalette) {
	if (tColorDepth == 8)
	{
		Buffer palettedBuffer = loadBufferFromRawPNGBuffer(tRawPNGBuffer, tBytesPerLine, tHeight, 0);
		if (gPrismMugenSpriteFileReaderData.mIsUsingRealPalette && tPalette == 0) {
			return makeMugenSpriteFileSpriteFromRawBuffer(palettedBuffer, tWidth, tHeight, tWidth, tAxisOffset);
		}
		else {
			const auto& paletteElement = tPalettes[tPalette];
			return makeMugenSpriteFileSpriteFromRawAndPaletteBuffer(palettedBuffer, paletteElement.mBuffer, tWidth, tHeight, tWidth, tAxisOffset);
		}
	}
	else
	{
		Buffer argb32Buffer = loadBufferFromRawPNGBuffer(tRawPNGBuffer, tBytesPerLine, tHeight, 1);
		freeBuffer(tRawPNGBuffer);

		auto subImageList = breakImageBufferUpIntoMultipleBuffers(argb32Buffer, tBytesPerLine, tWidth, tHeight, 4);
		freeBuffer(argb32Buffer);

		auto textures = loadTextureFromImageARGB32List(subImageList);
		freeSubImageBufferList(subImageList);

		return makeMugenSpriteFileSprite(textures, makeTextureSize(tWidth, tHeight), tAxisOffset);
	}
}

static void insertPaletteIntoMugenSpriteFile(MugenSpriteFile* tSprites, const Buffer& b, int tGroup = -1, int tItem = -1) {
	assert(isRGBPalette(b) || isARGBPalette(b));

	if (gPrismMugenSpriteFileReaderData.mIsUsingRealPalette && tSprites->mPalettes.empty()) {
		if (isRGBPalette(b)) {
			setPaletteFromBGR256WithFirstValueTransparentBuffer(gPrismMugenSpriteFileReaderData.mPaletteID, b);
		}
		else {
			setPaletteFromARGB256Buffer(gPrismMugenSpriteFileReaderData.mPaletteID, b);
		}
	}

	MugenSpriteFilePalette paletteElement;
	paletteElement.mBuffer = b;
	paletteElement.mGroup = tGroup;
	paletteElement.mItem = tItem;
	tSprites->mPalettes.push_back(paletteElement);
}

 
#ifdef N3DS_HEAP_GUARD
extern "C" {
	extern u32 __ctru_heap_size;
}

// [3DS] Heap kept free while loading sprites: a character whose sprites do
// not fit gets some left out (not drawn), not a crash for want of memory.
// The heap in use is asked for every 16 sprites (it goes through the free
// blocks), estimated in between.
static size_t gN3DSSpriteHeapReserve = 4 * 1024 * 1024;
static int gN3DSSpritesLeftOut;
static int gN3DSHeapUsedCheckedSprites;
static size_t gN3DSHeapUsed;

void setMugenSpriteFileReaderHeapReserve(size_t tReserve) {
	gN3DSSpriteHeapReserve = tReserve;
}

// A screen-sized sprite (a super move's full-screen frames, a cut-in) also
// leaves 4 MB more: when not all fit, the many small ones after it (the
// moves' sprites, effects) come first.
static const size_t N3DS_BIG_SPRITE_NEED = 512 * 1024;
static const size_t N3DS_BIG_SPRITE_EXTRA_RESERVE = 4 * 1024 * 1024;

static int hasN3DSHeapRoomForSprite(size_t tNeeded) {
	if (gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits) return 1;
	const size_t reserve = gN3DSSpriteHeapReserve + (tNeeded >= N3DS_BIG_SPRITE_NEED ? N3DS_BIG_SPRITE_EXTRA_RESERVE : 0);
	if (!(gN3DSHeapUsedCheckedSprites++ & 15)) gN3DSHeapUsed = mallinfo().uordblks;
	gN3DSHeapUsed += tNeeded / 3;
	if (gN3DSHeapUsed + tNeeded + reserve > __ctru_heap_size) {
		gN3DSHeapUsed = mallinfo().uordblks;
		gN3DSHeapUsedCheckedSprites = 1;
		if (gN3DSHeapUsed + tNeeded + reserve > __ctru_heap_size) {
			gN3DSSpritesLeftOut++;
			logWarning("Out of heap memory: a sprite left out.");
			return 0;
		}
	}
	if (tNeeded >= N3DS_BIG_SPRITE_NEED) {
		// In one piece, as it is asked for.
		void* probe = malloc(tNeeded + 1024 * 1024);
		if (!probe) {
			gN3DSSpritesLeftOut++;
			logWarning("Out of heap memory: a sprite left out.");
			return 0;
		}
		free(probe);
	}
	return 1;
}
#endif

#ifdef N3DS_HEAP_GUARD
// [3DS] Streamed sprites: one with no room in the heap is not left out but
// read from its file again when drawn (texture_3ds.cpp keeps a few of them,
// those drawn last): where it is in the file, how it is stored, and the
// piece of it a texture shows.
enum {
	N3DS_STREAM_PCX,     // SFF v1
	N3DS_STREAM_RAW,     // SFF v2, format 0
	N3DS_STREAM_RLE8,    // format 2
	N3DS_STREAM_LZ5,     // format 4
	N3DS_STREAM_PNG8,    // format 10
};

typedef struct {
	N3DSTextureStream mBase;
	const std::string* mPath;
	uint32_t mOffset;
	uint32_t mLength;
	uint8_t mKind;
	uint16_t mWidth, mHeight, mBytesPerLine;   // the whole image
	uint16_t mX, mY;                           // the piece's corner in it
} N3DSSpriteStream;

static std::set<std::string> gN3DSStreamPaths;     // each sprite file's path, kept for its streams
static const std::string* gN3DSStreamPath;          // the one being loaded
static uint32_t gN3DSSubfileOffset;                 // the SFF v1 sprite being loaded: where its data is
static uint32_t gN3DSStreamedSprites;               // in the file being loaded

// Two files kept open (one a character's): opening one on the SD card takes some 4 ms.
static struct {
	const std::string* mPath;
	FileHandler mFile;
	uint32_t mLastUsed;
} gN3DSStreamFiles[2];
static uint32_t gN3DSStreamFileUses;
static std::vector<uint8_t> gN3DSStreamData;

static FileHandler getN3DSStreamFile(const std::string* tPath) {
	gN3DSStreamFileUses++;
	for (auto& open : gN3DSStreamFiles) {
		if (open.mPath == tPath && open.mFile != FILEHND_INVALID) {
			open.mLastUsed = gN3DSStreamFileUses;
			return open.mFile;
		}
	}
	auto* slot = &gN3DSStreamFiles[0];
	if (gN3DSStreamFiles[1].mLastUsed < slot->mLastUsed) slot = &gN3DSStreamFiles[1];
	if (slot->mPath && slot->mFile != FILEHND_INVALID) fileClose(slot->mFile);
	slot->mPath = tPath;
	slot->mFile = fileOpen(tPath->c_str(), O_RDONLY);
	slot->mLastUsed = gN3DSStreamFileUses;
	return slot->mFile;
}

static Buffer decodeLZ5BufferAndReturnOwnedBuffer(const Buffer& b, uint32_t tFinalSize);
static Buffer loadBufferFromRawPNGBuffer(const Buffer& tRawPNGBuffer, int tWidth, int tHeight, int tIsUsingPngPalette);

// The last sprite read whole, kept for its other pieces (a big one is cut
// in several: each read it whole from the card, 17 ms a piece).
// Its own copy, not the memory handler's (which frees a screen's blocks at
// its end, under it).
static struct {
	const std::string* mPath;
	uint32_t mOffset;
	std::vector<uint8_t> mImage;
} gN3DSStreamCache;

static int gIsLoadingN3DSStream;   // its data in use: not let go meanwhile
static uint32_t gN3DSLiveStreams;  // the streamed textures there are

static void dropN3DSStreamWorkerResult();

static size_t letN3DSStreamCacheGo() {
	if (gIsLoadingN3DSStream) return 0;
	dropN3DSStreamWorkerResult();
	const size_t freed = gN3DSStreamData.capacity() + gN3DSStreamCache.mImage.capacity();
	std::vector<uint8_t>().swap(gN3DSStreamCache.mImage);
	gN3DSStreamCache.mPath = NULL;
	std::vector<uint8_t>().swap(gN3DSStreamData);
	return freed;
}

static int loadN3DSSpriteStreamUnguarded(N3DSSpriteStream* e);
static void copyN3DSStreamPiece(const N3DSSpriteStream* e, const Buffer& tImage, unsigned char* oIndices, int tWidth, int tHeight);

// [3DS] Streamed sprites read and decoded on the other core: a sprite cost
// the game's core some 35 ms (Modern Kitana's PNGs: her X-ray images, her
// blood), two frames, the drawing skipped meanwhile (the X-ray shown in
// flashes). The game asks, goes on (not drawing it), and takes it when it
// is there. Only plain C++ there: no memory handler, no prism file reader.
typedef struct {
	const std::string* mPath;
	uint32_t mOffset;
	uint32_t mLength;
	uint8_t mKind;
	uint16_t mBytesPerLine, mHeight;
} N3DSStreamRequest;

enum { N3DS_WORKER_IDLE, N3DS_WORKER_ASKED, N3DS_WORKER_WORKING, N3DS_WORKER_DONE };

typedef struct {
	std::mutex mMutex;
	std::condition_variable mCondition;
	int mState = N3DS_WORKER_IDLE;
	int mIsStarted = 0;
	int mIsAvailable = 0;
	N3DSStreamRequest mRequest;
	std::vector<uint8_t> mResult;
	int mIsResultValid = 0;
	uint32_t mReads = 0;         // for the load log: read there,
	uint64_t mReadTicks = 0;     // how long it took,
	uint32_t mGameCoreReads = 0; // and read on the game's core instead
} N3DSStreamWorker;

// Never destroyed: the thread waits on it until the very end (destroyed at
// the game's exit under it, the exit hung).
static N3DSStreamWorker& getN3DSStreamWorker() {
	static N3DSStreamWorker* worker = new N3DSStreamWorker();
	return *worker;
}
#define gN3DSWorker (getN3DSStreamWorker())

static int isN3DSStreamKindOnWorker(int tKind) {
	return tKind == N3DS_STREAM_PCX || tKind == N3DS_STREAM_RAW || tKind == N3DS_STREAM_RLE8 || tKind == N3DS_STREAM_PNG8;
}

typedef struct {
	const uint8_t* mData;
	size_t mLeft;
} N3DSPNGMemoryReader;

static void readN3DSWorkerPNG(png_structp tPNG, png_bytep oData, png_size_t tLength) {
	N3DSPNGMemoryReader* reader = (N3DSPNGMemoryReader*)png_get_io_ptr(tPNG);
	if (tLength > reader->mLeft) png_error(tPNG, "PNG data cut short");
	memcpy(oData, reader->mData, tLength);
	reader->mData += tLength;
	reader->mLeft -= tLength;
}

// An 8-bit (or less) paletted PNG's indices, one byte each, tBytesPerLine a row.
static int decodeN3DSWorkerPNG8(const uint8_t* tData, size_t tLength, int tBytesPerLine, int tHeight, std::vector<uint8_t>& oImage) {
	std::vector<png_bytep> rows;
	std::vector<uint8_t> decoded;
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) return 0;
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		return 0;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		return 0;
	}
	N3DSPNGMemoryReader reader = { tData, tLength };
	png_set_read_fn(png, &reader, readN3DSWorkerPNG);
	png_read_info(png, info);
	png_uint_32 width = 0, height = 0;
	int bitDepth = 0, colorType = -1;
	png_get_IHDR(png, info, &width, &height, &bitDepth, &colorType, NULL, NULL, NULL);
	if ((colorType != PNG_COLOR_TYPE_PALETTE && colorType != PNG_COLOR_TYPE_GRAY) || bitDepth > 8 || !width || !height) {
		png_destroy_read_struct(&png, &info, NULL);
		return 0;
	}
	if (bitDepth < 8) png_set_packing(png);
	png_set_interlace_handling(png);
	png_read_update_info(png, info);
	decoded.assign(size_t(width) * height, 0);
	rows.resize(height);
	for (png_uint_32 y = 0; y < height; y++) rows[y] = decoded.data() + size_t(y) * width;
	png_read_image(png, rows.data());
	png_destroy_read_struct(&png, &info, NULL);

	oImage.assign(size_t(tBytesPerLine) * tHeight, 0);
	const int rowsCopied = std::min<int>(tHeight, int(height));
	const int rowLength = std::min<int>(tBytesPerLine, int(width));
	for (int y = 0; y < rowsCopied; y++) memcpy(oImage.data() + size_t(y) * tBytesPerLine, decoded.data() + size_t(y) * width, rowLength);
	return 1;
}

// The sprite whole (tBytesPerLine * tHeight indices), from its file.
static int loadN3DSStreamOnWorker(const N3DSStreamRequest& tRequest, std::vector<uint8_t>& oImage) {
	// The file kept open for the next one (opening one on the card takes
	// some milliseconds): this thread's own, only touched here.
	static FILE* file = NULL;
	static const std::string* filePath = NULL;
	if (filePath != tRequest.mPath) {
		if (file) fclose(file);
		char path[1024];
		getFullPath(path, tRequest.mPath->c_str());
		file = fopen(path, "rb");
		filePath = file ? tRequest.mPath : NULL;
		if (file) setvbuf(file, NULL, _IONBF, 0);   // read straight into ours
	}
	if (!file) return 0;
	std::vector<uint8_t> data(size_t(tRequest.mLength) + 16, 0);
	const int isRead = !fseek(file, long(tRequest.mOffset), SEEK_SET) && fread(data.data(), 1, tRequest.mLength, file) == tRequest.mLength;
	if (!isRead) {
		fclose(file);
		file = NULL;
		filePath = NULL;
		return 0;
	}

	const size_t size = size_t(tRequest.mBytesPerLine) * tRequest.mHeight;
	const uint8_t* input = data.data();
	switch (tRequest.mKind) {
	case N3DS_STREAM_RAW:
		oImage.assign(input, input + std::min<size_t>(size, tRequest.mLength));
		oImage.resize(size, 0);
		return 1;
	case N3DS_STREAM_PCX: {
		if (tRequest.mLength <= sizeof(PCXHeader)) return 0;
		oImage.assign(size, 0);
		size_t op = 0;
		for (size_t ip = sizeof(PCXHeader); ip < tRequest.mLength && op < size; ip++) {
			const uint8_t cur = input[ip];
			if ((cur & 0xC0) == 0xC0) {
				if (++ip >= tRequest.mLength) break;
				const size_t amount = std::min<size_t>(cur & 0x3F, size - op);
				memset(oImage.data() + op, input[ip], amount);
				op += amount;
			}
			else oImage[op++] = cur;
		}
		return 1;
	}
	case N3DS_STREAM_RLE8: {
		if (tRequest.mLength < 4) return 0;
		oImage.assign(size, 0);
		size_t op = 0;
		for (size_t ip = 4; ip < tRequest.mLength && op < size;) {
			if ((input[ip] & 0xC0) == 0x40) {
				if (ip + 1 >= tRequest.mLength) break;
				const size_t amount = std::min<size_t>(input[ip] & 0x3F, size - op);
				memset(oImage.data() + op, input[ip + 1], amount);
				op += amount;
				ip += 2;
			}
			else oImage[op++] = input[ip++];
		}
		return 1;
	}
	case N3DS_STREAM_PNG8:
		return decodeN3DSWorkerPNG8(input, tRequest.mLength, tRequest.mBytesPerLine, tRequest.mHeight, oImage);
	}
	return 0;
}

static void runN3DSStreamWorker(void*) {
	for (;;) {
		N3DSStreamRequest request;
		{
			std::unique_lock<std::mutex> lock(gN3DSWorker.mMutex);
			gN3DSWorker.mCondition.wait(lock, [] { return gN3DSWorker.mState == N3DS_WORKER_ASKED; });
			gN3DSWorker.mState = N3DS_WORKER_WORKING;
			request = gN3DSWorker.mRequest;
		}
		std::vector<uint8_t> image;
		int isValid = 0;
#ifdef __3DS__
		const uint64_t start = svcGetSystemTick();
#endif
		try {
			isValid = loadN3DSStreamOnWorker(request, image);
		}
		catch (const std::bad_alloc&) {
			std::vector<uint8_t>().swap(image);
		}
		{
			std::lock_guard<std::mutex> lock(gN3DSWorker.mMutex);
			gN3DSWorker.mResult.swap(image);
			gN3DSWorker.mIsResultValid = isValid;
			gN3DSWorker.mState = N3DS_WORKER_DONE;
			gN3DSWorker.mReads++;
#ifdef __3DS__
			gN3DSWorker.mReadTicks += svcGetSystemTick() - start;
#endif
		}
	}
}

#ifdef __3DS__
static void runN3DSStreamWorkerThread(void* tCaller) { runN3DSStreamWorker(tCaller); }
#endif

static void startN3DSStreamWorker() {
	if (gN3DSWorker.mIsStarted) return;
	gN3DSWorker.mIsStarted = 1;
#if defined(N3DS_TEXTURE_SIM) && !defined(__3DS__)
	if (getenv("N3DS_SIM_SYNC_STREAMS")) return;   // as before: read on the game's thread
#endif
#ifdef __3DS__
	// The other core (the New 3DS's third), at a priority under the game's;
	// else on the game's own, when it waits for the screen.
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	int core = 2;
	Thread thread = threadCreate(runN3DSStreamWorkerThread, NULL, 64 * 1024, priority + 1, core, true);
	if (!thread) thread = threadCreate(runN3DSStreamWorkerThread, NULL, 64 * 1024, priority + 1, core = -2, true);
	gN3DSWorker.mIsAvailable = thread != NULL;
	addN3DSPerfLine("  streamed sprites' reader: %s", !thread ? "not started (read on the game's core)" : core == 2 ? "on the other core" : "on the game's core, under it");
#else
	std::thread(runN3DSStreamWorker, nullptr).detach();
	gN3DSWorker.mIsAvailable = 1;
#endif
}

// A sprite read on the other core and not taken yet, let go (the heap is short).
static void dropN3DSStreamWorkerResult() {
	if (!gN3DSWorker.mIsAvailable) return;
	std::lock_guard<std::mutex> lock(gN3DSWorker.mMutex);
	if (gN3DSWorker.mState != N3DS_WORKER_DONE) return;
	std::vector<uint8_t>().swap(gN3DSWorker.mResult);
	gN3DSWorker.mState = N3DS_WORKER_IDLE;
}

// Waits for the sprite being read on the other core, then forgets it.
static void forgetN3DSStreamWorkerResult() {
	if (!gN3DSWorker.mIsAvailable) return;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(gN3DSWorker.mMutex);
			if (gN3DSWorker.mState != N3DS_WORKER_ASKED && gN3DSWorker.mState != N3DS_WORKER_WORKING) {
				std::vector<uint8_t>().swap(gN3DSWorker.mResult);
				gN3DSWorker.mState = N3DS_WORKER_IDLE;
				return;
			}
		}
#ifdef __3DS__
		svcSleepThread(1000000);
#else
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
	}
}

// Small sprites are read on the game's core at once (as before the other
// core: their effects drawn when they come) for up to this long a frame;
// the big ones, and the rest, on the other core.
static const uint32_t N3DS_STREAM_SMALL_LENGTH = 32 * 1024;
static const double N3DS_STREAM_SYNC_MS_A_FRAME = 6.0;

static int isN3DSStreamCached(const N3DSSpriteStream* e) {
	return !gN3DSStreamCache.mImage.empty() && gN3DSStreamCache.mPath == e->mPath && gN3DSStreamCache.mOffset == e->mOffset;
}

// tIndices NULL: only whether the sprite is there now (read at once when it
// can be, asked of the other core if not), nothing copied.
static int loadN3DSSpriteStream(N3DSTextureStream* tStream, unsigned char* oIndices, int tWidth, int tHeight) {
	N3DSSpriteStream* e = (N3DSSpriteStream*)tStream;
	if (isN3DSStreamCached(e)) {
		if (oIndices) copyN3DSStreamPiece(e, makeBuffer(gN3DSStreamCache.mImage.data(), uint32_t(gN3DSStreamCache.mImage.size())), oIndices, tWidth, tHeight);
		return N3DS_STREAM_LOADED;
	}

	startN3DSStreamWorker();
	const int hasWorker = gN3DSWorker.mIsAvailable && isN3DSStreamKindOnWorker(e->mKind) && !gN3DSStreamsForceSync;
	if (hasWorker) {
		std::unique_lock<std::mutex> lock(gN3DSWorker.mMutex);
		if (gN3DSWorker.mState == N3DS_WORKER_DONE) {
			const int isThisOne = gN3DSWorker.mRequest.mPath == e->mPath && gN3DSWorker.mRequest.mOffset == e->mOffset;
			if (gN3DSWorker.mIsResultValid && !gIsLoadingN3DSStream) {
				// Kept as the last one read, whichever it is: its pieces come next.
				gN3DSStreamCache.mImage.swap(gN3DSWorker.mResult);
				gN3DSStreamCache.mPath = gN3DSWorker.mRequest.mPath;
				gN3DSStreamCache.mOffset = gN3DSWorker.mRequest.mOffset;
			}
			std::vector<uint8_t>().swap(gN3DSWorker.mResult);
			gN3DSWorker.mState = N3DS_WORKER_IDLE;
			lock.unlock();
			if (isThisOne) {
				if (!isN3DSStreamCached(e)) return N3DS_STREAM_FAILED;
				if (oIndices) copyN3DSStreamPiece(e, makeBuffer(gN3DSStreamCache.mImage.data(), uint32_t(gN3DSStreamCache.mImage.size())), oIndices, tWidth, tHeight);
				return N3DS_STREAM_LOADED;
			}
			lock.lock();
		}
		const int isAskedAlready = gN3DSWorker.mState != N3DS_WORKER_IDLE && gN3DSWorker.mRequest.mPath == e->mPath && gN3DSWorker.mRequest.mOffset == e->mOffset;
		if (isAskedAlready) return N3DS_STREAM_PENDING;
	}

	// A small one, while this frame's time for them lasts: read here now.
#ifdef __3DS__
	const int isTimeLeft = getN3DSPerfMilliseconds(gN3DSStreamSyncTicksThisFrame) < N3DS_STREAM_SYNC_MS_A_FRAME;
#else
	const int isTimeLeft = 1;
#endif
	const int isHere = !hasWorker || (e->mLength <= N3DS_STREAM_SMALL_LENGTH && isTimeLeft);
	// The whole sprite, read and decoded, with room left: else not drawn
	// this time (out of heap in a fight is the game's end).
	const size_t need = size_t(e->mBytesPerLine) * e->mHeight * (isHere ? 1 : 2) + e->mLength + 768 * 1024;
	if (getN3DSHeapFree() < need) {
		letN3DSStreamedPixelsGo(need - getN3DSHeapFree());
		if (getN3DSHeapFreeExact() < need) return N3DS_STREAM_FAILED;
	}

	if (isHere) {
#ifdef __3DS__
		const uint64_t start = getN3DSPerfTicks();
#endif
		if (!gN3DSStreamsForceSync) gN3DSWorker.mGameCoreReads++;
		gIsLoadingN3DSStream = 1;
		const int isLoaded = loadN3DSSpriteStreamUnguarded(e);
		gIsLoadingN3DSStream = 0;
#ifdef __3DS__
		gN3DSStreamSyncTicksThisFrame += getN3DSPerfTicks() - start;
#endif
		if (!isLoaded) return N3DS_STREAM_FAILED;
		if (oIndices) copyN3DSStreamPiece(e, makeBuffer(gN3DSStreamCache.mImage.data(), uint32_t(gN3DSStreamCache.mImage.size())), oIndices, tWidth, tHeight);
		return N3DS_STREAM_LOADED;
	}

	std::unique_lock<std::mutex> lock(gN3DSWorker.mMutex);
	if (gN3DSWorker.mState != N3DS_WORKER_IDLE) return N3DS_STREAM_PENDING;   // busy with another: asked again next frame
	gN3DSWorker.mRequest = { e->mPath, e->mOffset, e->mLength, e->mKind, e->mBytesPerLine, e->mHeight };
	gN3DSWorker.mState = N3DS_WORKER_ASKED;
	lock.unlock();
	gN3DSWorker.mCondition.notify_one();
	return N3DS_STREAM_PENDING;
}

static void copyN3DSStreamPiece(const N3DSSpriteStream* e, const Buffer& tImage, unsigned char* oIndices, int tWidth, int tHeight) {
	// The piece, as getSingleAllocatedBufferFromSource cuts it.
	const uint8_t* source = (const uint8_t*)tImage.mData;
	for (int y = 0; y < tHeight; y++) {
		const int sourceY = e->mY + y;
		if (sourceY >= e->mHeight) break;
		const int amount = std::min(tWidth, int(e->mWidth) - int(e->mX));
		const size_t start = size_t(sourceY) * e->mBytesPerLine + e->mX;
		if (amount <= 0 || start + amount > tImage.mLength) continue;
		memcpy(oIndices + size_t(y) * tWidth, source + start, amount);
	}
}

// Read and decoded here, on the game's core: kept whole as the last one
// read (its pieces copied from there); 0 if it could not be.
static int loadN3DSSpriteStreamUnguarded(N3DSSpriteStream* e) {
	FileHandler file = getN3DSStreamFile(e->mPath);
	if (file == FILEHND_INVALID) return 0;
	try {
		gN3DSStreamData.resize(e->mLength + 16);
	}
	catch (const std::bad_alloc&) {
		std::vector<uint8_t>().swap(gN3DSStreamData);
		return 0;
	}
	fileSeek(file, e->mOffset, SEEK_SET);
	const uint32_t read = uint32_t(fileRead(file, gN3DSStreamData.data(), e->mLength));
	if (read < e->mLength) memset(gN3DSStreamData.data() + read, 0, e->mLength - read);
	uint8_t* data = gN3DSStreamData.data();

	const uint32_t size = uint32_t(e->mBytesPerLine) * e->mHeight;
	Buffer image = makeBuffer(NULL, 0);
	switch (e->mKind) {
	case N3DS_STREAM_PCX:
		if (e->mLength > sizeof(PCXHeader)) image = decodeRLE5BufferAndReturnOwnedBuffer(makeBuffer(data + sizeof(PCXHeader), e->mLength - sizeof(PCXHeader)), size);
		break;
	case N3DS_STREAM_RAW:
		image = makeBuffer(data, e->mLength);
		break;
	case N3DS_STREAM_RLE8:
	case N3DS_STREAM_LZ5: {
		if (e->mLength < 4) break;
		uint32_t decompressedSize;
		memcpy(&decompressedSize, data, 4);
		const Buffer encoded = makeBuffer(data + 4, e->mLength - 4);
		image = e->mKind == N3DS_STREAM_RLE8 ? decodeRLE8BufferAndReturnOwnedBuffer(encoded, decompressedSize) : decodeLZ5BufferAndReturnOwnedBuffer(encoded, decompressedSize);
		break;
	}
	case N3DS_STREAM_PNG8:
		image = loadBufferFromRawPNGBuffer(makeBuffer(data, e->mLength), e->mBytesPerLine, e->mHeight, 0);
		break;
	}
	int isKept = 0;
	if (image.mData) {
		try {
			gN3DSStreamCache.mImage.assign((const uint8_t*)image.mData, (const uint8_t*)image.mData + image.mLength);
			gN3DSStreamCache.mPath = e->mPath;
			gN3DSStreamCache.mOffset = e->mOffset;
			isKept = 1;
		}
		catch (const std::bad_alloc&) {
			std::vector<uint8_t>().swap(gN3DSStreamCache.mImage);
			gN3DSStreamCache.mPath = NULL;
		}
		if (image.mIsOwned) freeBuffer(image);
	}
	// A big sprite's data is not kept: the heap is short in these fights.
	if (gN3DSStreamData.capacity() > 256 * 1024) std::vector<uint8_t>().swap(gN3DSStreamData);
	return isKept;
}

// The fight's last one gone: its files closed and what was kept let go,
// so that nothing is left in the middle of the heap for the next one.
static void freeN3DSSpriteStream(N3DSTextureStream* tStream) {
	free(tStream);
	if (gN3DSLiveStreams && --gN3DSLiveStreams) return;
	forgetN3DSStreamWorkerResult();
#ifdef __3DS__
	if (gN3DSWorker.mReads || gN3DSWorker.mGameCoreReads) {
		const double ms = gN3DSWorker.mReadTicks / (double)SYSCLOCK_ARM11 * 1000.0;
		addN3DSPerfLine("  streamed sprites read: %u on the other core (%.0f ms there, %.1f ms each), %u on the game's core (small ones, LZ5)",
			(unsigned)gN3DSWorker.mReads, ms, gN3DSWorker.mReads ? ms / gN3DSWorker.mReads : 0.0, (unsigned)gN3DSWorker.mGameCoreReads);
	}
#endif
	gN3DSWorker.mReads = gN3DSWorker.mGameCoreReads = 0;
	gN3DSWorker.mReadTicks = 0;
	letN3DSStreamedPixelsGo(0);
	letN3DSStreamCacheGo();
	for (auto& open : gN3DSStreamFiles) {
		if (open.mPath && open.mFile != FILEHND_INVALID) fileClose(open.mFile);
		open.mPath = NULL;
		open.mFile = FILEHND_INVALID;
		open.mLastUsed = 0;
	}
}

// A sprite streamed in the pieces the loaded ones are cut in, with prism's
// palette (tPaletteBuffer empty) or its own; empty if its palette has
// colors partly transparent (no paletted texture for those).
static MugenSpriteFileSprite makeN3DSStreamedSprite(int tKind, uint32_t tOffset, uint32_t tLength, int tWidth, int tHeight, int tBytesPerLine, const Buffer* tPaletteBuffer, const Vector2D& tAxisOffset) {
	u32 colors[256];
	const u32* palette = NULL;
	if (tPaletteBuffer) {
		const uint8_t* pal = (const uint8_t*)tPaletteBuffer->mData;
		if (isRGBPalette(*tPaletteBuffer)) {
			for (int i = 0; i < 256; i++) colors[i] = ((u32)pal[i * 3 + 0] << 24) | ((u32)pal[i * 3 + 1] << 16) | ((u32)pal[i * 3 + 2] << 8) | (i ? 0xFF : 0);
		}
		else {
			for (int i = 0; i < 256; i++) {
				if (pal[i * 4 + 0] != 0 && pal[i * 4 + 0] != 0xFF) return makeMugenSpriteFileSprite(std::vector<MugenSpriteFileSubSprite>(), makeTextureSize(tWidth, tHeight), tAxisOffset);
				colors[i] = ((u32)pal[i * 4 + 1] << 24) | ((u32)pal[i * 4 + 2] << 16) | ((u32)pal[i * 4 + 3] << 8) | pal[i * 4 + 0];
			}
		}
		palette = colors;
	}

	gN3DSLetStreamCacheGo = letN3DSStreamCacheGo;
	std::vector<MugenSpriteFileSubSprite> textures;
	for (int y = 0; y < tHeight;) {
		const int dy = getMaximumSizeFit(tHeight - y);
		for (int x = 0; x < tWidth;) {
			const int dx = getMaximumSizeFit(tWidth - x);
			N3DSSpriteStream* stream = (N3DSSpriteStream*)malloc(sizeof(N3DSSpriteStream));
			gN3DSLiveStreams++;
			stream->mBase.mLoad = loadN3DSSpriteStream;
			stream->mBase.mFree = freeN3DSSpriteStream;
			stream->mBase.mImageSize = uint32_t(tBytesPerLine) * tHeight;
			stream->mBase.mImageKey = tOffset;
			stream->mPath = gN3DSStreamPath;
			stream->mOffset = tOffset;
			stream->mLength = tLength;
			stream->mKind = (uint8_t)tKind;
			stream->mWidth = (uint16_t)tWidth;
			stream->mHeight = (uint16_t)tHeight;
			stream->mBytesPerLine = (uint16_t)tBytesPerLine;
			stream->mX = (uint16_t)x;
			stream->mY = (uint16_t)y;
			MugenSpriteFileSubSprite piece;
			piece.mOffset = Vector2DI(x, y);
			piece.mTexture = loadN3DSStreamedTexture(&stream->mBase, palette, gPrismMugenSpriteFileReaderData.mPaletteID, dx, dy);
			textures.push_back(piece);
			x += dx;
		}
		y += dy;
	}
	gN3DSStreamedSprites++;
	return makeMugenSpriteFileSprite(textures, makeTextureSize(tWidth, tHeight), tAxisOffset);
}
#endif

// SFF v1: the file's first palette, and whether the sprites now use it (the
// ones sharing the palette before them).
static std::vector<uint8_t> gPCXBasePalette;
static int gIsOnPCXBasePalette;
static int gPCXSpriteGroup;   // the group of the sprite being loaded
static int gIsPCXPaletteFromPortrait;   // the last palette of its own, a portrait's

static void resetPCXBasePalette() {
	gPCXBasePalette.clear();
	gIsOnPCXBasePalette = 1;
	gIsPCXPaletteFromPortrait = 0;
	gPCXSpriteGroup = -1;
}

static MugenSpriteFileSprite loadTextureFromPCXBuffer(MugenSpriteFile* tDst, int mIsUsingOwnPalette, const Buffer& b, const Vector2D& tAxisOffset) {
	PCXHeader header;

	BufferPointer p = getBufferPointer(b);

	readFromBufferPointer(&header, &p, sizeof(PCXHeader));

	int bytesPerPixel = header.mBitsPerPixel / 8;
	int w = header.mMaxX - header.mMinX + 1;
	int h = header.mMaxY - header.mMinY + 1;
	int32_t pcxImageSize = bytesPerPixel*header.mBytesPerLine*h;
	
	assert(header.mBitsPerPixel == 8);
	assert(header.mEncoding == 1);
	if (header.mPlaneAmount != 1) {
		logWarningFormat("Unsupported pcx plane amount: %d", header.mPlaneAmount);
	}

	int encodedSize;
	if (mIsUsingOwnPalette) {
		encodedSize = b.mLength - sizeof(PCXHeader) - 256 * 3;
	}
	else {
		encodedSize = b.mLength - sizeof(PCXHeader);
	}

	if (mIsUsingOwnPalette) {
		auto insertPalette = loadPCXPaletteToBuffer(p, encodedSize);
		if (gPCXBasePalette.empty()) {
			gPCXBasePalette.assign((const uint8_t*)insertPalette.mData, (const uint8_t*)insertPalette.mData + 256 * 3);
		}
		// A palette of its own that is the file's first one (the character's
		// colors, repeated) is the character's palette: the .act chosen, as
		// in MUGEN (CvS characters repeat it in 0,1, then share it). Not the
		// portraits' (group 9000): drawn for their own colors (CvS Morrigan's
		// big portrait is the file's first sprite).
		if (gPCXSpriteGroup != 9000 && !tDst->mPalettes.empty() && !memcmp(insertPalette.mData, gPCXBasePalette.data(), 256 * 3)) {
			freeBuffer(insertPalette);
			gIsOnPCXBasePalette = 1;
		}
		else {
			insertPaletteIntoMugenSpriteFile(tDst, insertPalette);
			gIsOnPCXBasePalette = tDst->mPalettes.size() == 1;
		}
	}
	assert(!tDst->mPalettes.empty());

#ifdef N3DS_HEAP_GUARD
	if (!hasN3DSHeapRoomForSprite((size_t)pcxImageSize * 3)) {
#if defined(N3DS_TEXTURE_SIM) && !defined(__3DS__)
		if (getenv("N3DS_SIM_TRACE_LEFT_OUT")) fprintf(stderr, "n3dssim: streamed %d (%dx%d)\n", gPCXSpriteGroup, w, h);
#endif
		// Read again from the file when drawn.
		gN3DSSpritesLeftOut--;
		const int isOnRealPalette = gIsOnPCXBasePalette && gPrismMugenSpriteFileReaderData.mIsUsingRealPalette;
		const Buffer* palette = isOnRealPalette ? NULL : &(gIsOnPCXBasePalette ? tDst->mPalettes.front() : tDst->mPalettes.back()).mBuffer;
		return makeN3DSStreamedSprite(N3DS_STREAM_PCX, gN3DSSubfileOffset, uint32_t(b.mLength), w, h, header.mBytesPerLine, palette, tAxisOffset);
	}
#endif
	Buffer encodedImageBuffer = makeBuffer(p, encodedSize);
	Buffer rawImageBuffer = decodeRLE5BufferAndReturnOwnedBuffer(encodedImageBuffer, pcxImageSize);

	if (gIsOnPCXBasePalette && gPrismMugenSpriteFileReaderData.mIsUsingRealPalette) {
		return makeMugenSpriteFileSpriteFromRawBuffer(rawImageBuffer, w, h, header.mBytesPerLine, tAxisOffset);
	}
	else {
		auto paletteElement = gIsOnPCXBasePalette ? tDst->mPalettes.front() : tDst->mPalettes.back();
		return makeMugenSpriteFileSpriteFromRawAndPaletteBuffer(rawImageBuffer, paletteElement.mBuffer, w, h, header.mBytesPerLine, tAxisOffset);
	}
	
}

static MugenSpriteFile makeEmptySpriteFile();
static void unloadSinglePalette(MugenSpriteFilePalette& palette);

MugenSpriteFileSprite loadSingleTextureFromPCXBuffer(const Buffer& tBuffer) {
	MugenSpriteFile sprites = makeEmptySpriteFile();

	resetPCXBasePalette();
	auto sprite = loadTextureFromPCXBuffer(&sprites, 1, tBuffer, Vector2D(0, 0));
	for (auto& palette : sprites.mPalettes)
	{
		unloadSinglePalette(palette);
	}

	return sprite;
}

static int findEmptyNegativeSpriteSlot(const MugenSpriteFileGroup& tGroup)
{
	int testIndex = -10000;
	while (tGroup.mSprites.find(testIndex) != tGroup.mSprites.end())
	{
		testIndex--;
	}
	return testIndex;
}

static void insertTextureIntoSpriteFile(MugenSpriteFile* tDst, MugenSpriteFileSprite& tTexture, int tGroup, int tSprite) {

	if (!stl_map_contains(tDst->mGroups, tGroup)) {
		tDst->mGroups[tGroup] = MugenSpriteFileGroup();
	}

	auto& g = tDst->mGroups[tGroup];

	int realIndex = tSprite;
	if (g.mSprites.find(realIndex) != g.mSprites.end())
	{
		realIndex = findEmptyNegativeSpriteSlot(g);
		logWarningFormat("[MugenSpriteFileReader] Duplicate sprite for %d %d. Ignoring second and saving as negative index %d.", tGroup, tSprite, realIndex);
	}
	g.mSprites[realIndex] = tTexture;
	tDst->mAllSprites.push_back(&g.mSprites[realIndex]);
}

static int gPreviousGroup;

static int gLoadedSubFileAmount;

#ifdef __3DS__
// [3DS] Where each sprite file's portraits are (the select screen's two
// sprites per character), found once by going through the file and kept in
// the game's folder: then read straight from there, without the rest of the
// file (a 13 MB file took 1.2 s). SFF v1: the subfiles' offsets; SFF v2: the
// sprites' indices. A file of another size, or offsets that no longer lead to
// portraits, goes through the file again.
#ifndef N3DS_PORTRAIT_CACHE_FILE
#define N3DS_PORTRAIT_CACHE_FILE "sdmc:/3ds/dolmexica/portrait-cache.txt"
#endif

static struct {
	int mIsLoaded;
	std::map<std::string, std::vector<uint32_t>> mEntries;
	std::string mKey;   // the file being loaded: "path|size"
	std::vector<uint32_t> mFound;
} gN3DSPortraitCache;

static void loadN3DSPortraitCache() {
	if (gN3DSPortraitCache.mIsLoaded) return;
	gN3DSPortraitCache.mIsLoaded = 1;
	FILE* file = fopen(N3DS_PORTRAIT_CACHE_FILE, "r");
	if (!file) return;
	char line[1200];
	while (fgets(line, sizeof(line), file)) {
		char* tab = strchr(line, '\t');
		if (!tab) continue;
		*tab = '\0';
		std::vector<uint32_t> positions;
		char* p = tab + 1;
		while (*p && *p != '\n') {
			positions.push_back((uint32_t)strtoul(p, &p, 10));
			while (*p == ' ') p++;
		}
		if (!positions.empty()) gN3DSPortraitCache.mEntries[line] = positions;
	}
	fclose(file);
}

static void saveN3DSPortraitCache() {
	FILE* file = fopen(N3DS_PORTRAIT_CACHE_FILE, "w");
	if (!file) return;
	for (const auto& entry : gN3DSPortraitCache.mEntries) {
		fprintf(file, "%s\t", entry.first.c_str());
		for (const auto position : entry.second) fprintf(file, "%u ", (unsigned)position);
		fputc('\n', file);
	}
	fclose(file);
}

static const std::vector<uint32_t>* findN3DSPortraitCacheEntry() {
	if (!gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits) return NULL;
	loadN3DSPortraitCache();
	const auto it = gN3DSPortraitCache.mEntries.find(gN3DSPortraitCache.mKey);
	return it == gN3DSPortraitCache.mEntries.end() ? NULL : &it->second;
}

static void addN3DSPortraitCacheEntry(const MugenSpriteFile& tFile) {
	if (!gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits) return;
	if (gN3DSPortraitCache.mFound.empty() || gN3DSPortraitCache.mFound.size() != tFile.mAllSprites.size()) return;
	gN3DSPortraitCache.mEntries[gN3DSPortraitCache.mKey] = gN3DSPortraitCache.mFound;
	saveN3DSPortraitCache();
}
#endif

static void logSingleSFFSubFile(const SFFSubFileHeader& tSubHeader) {
	debugFormat("[MugenSpriteFileReader] sff sprite %d: group %d image %d length %d linkedTo %d", gLoadedSubFileAmount++, (int)tSubHeader.mGroup, (int)tSubHeader.mImage, (int)tSubHeader.mSubfileLength, (int)tSubHeader.mIndexOfPreciousSpriteCopy);
}

static void loadSingleSFFFileAndInsertIntoSpriteFile(const SFFSubFileHeader& subHeader, MugenSpriteFile* tDst) {
	logSingleSFFSubFile(subHeader);

	MugenSpriteFileSprite texture;

	if (subHeader.mSubfileLength) {
#ifdef N3DS_HEAP_GUARD
		gN3DSSubfileOffset = gPrismMugenSpriteFileReaderData.mReader.mGetCurrentOffset(&gPrismMugenSpriteFileReaderData.mReader);
#endif
		Buffer pcxBuffer = gPrismMugenSpriteFileReaderData.mReader.mReadBufferReadOnly(&gPrismMugenSpriteFileReaderData.mReader, subHeader.mSubfileLength);
		int isFirstImageWithActiveActFile = gPrismMugenSpriteFileReaderData.mHasPaletteFile && subHeader.mGroup == 0 && subHeader.mImage == 0; 
		int isUsingOwnPalette = !subHeader.mHasSamePaletteAsPreviousImage && !isFirstImageWithActiveActFile;
		if (isFirstImageWithActiveActFile) gIsOnPCXBasePalette = 1;
		// A portrait's palette (group 9000) is its own: the sprites after it
		// sharing "the previous palette" are the character's, drawn with the
		// .act chosen as in WinMUGEN (2002 Rugal's 1279 sprites came after
		// his big portrait's palette, his chosen colors not shown).
		if (isUsingOwnPalette) gIsPCXPaletteFromPortrait = subHeader.mGroup == 9000;
		else if (subHeader.mGroup != 9000 && gIsPCXPaletteFromPortrait) gIsOnPCXBasePalette = 1;
		gPreviousGroup = subHeader.mGroup;
		gPCXSpriteGroup = subHeader.mGroup;
		texture = loadTextureFromPCXBuffer(tDst, isUsingOwnPalette, pcxBuffer, Vector2D(subHeader.mImageAxisXCoordinate, subHeader.mImageAxisYCoordinate));
		freeBuffer(pcxBuffer);
	}
	else {
		texture = makeLinkedMugenSpriteFileSprite(subHeader.mIndexOfPreciousSpriteCopy, Vector2D(subHeader.mImageAxisXCoordinate, subHeader.mImageAxisYCoordinate));
	}

	insertTextureIntoSpriteFile(tDst, texture, subHeader.mGroup, subHeader.mImage);
}

static void removeAllPalettesExceptFirst(MugenSpriteFile* tDst) {
	assert(tDst->mPalettes.size() >= 1);

	while (tDst->mPalettes.size() > 1) {
		auto& paletteElement = tDst->mPalettes.back();
		freeBuffer(paletteElement.mBuffer);
		tDst->mPalettes.pop_back();
	}
}

static void loadSingleSFFFile(MugenSpriteFile* tDst) {
	SFFSubFileHeader subHeader;

	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &subHeader, sizeof(SFFSubFileHeader));

	verboseInteger(sizeof(SFFSubFileHeader));
	verboseInteger(subHeader.mNextFilePosition);
	verboseInteger(subHeader.mSubfileLength);
	verboseInteger(subHeader.mGroup);
	verboseInteger(subHeader.mImage);
	verboseString(subHeader.mComments);

	if (gPrismMugenSpriteFileReaderData.mHasPaletteFile && subHeader.mGroup == 0 && subHeader.mImage == 0) {
		removeAllPalettesExceptFirst(tDst);
	}

	if (!gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits || (subHeader.mGroup == 9000 && subHeader.mImage <= 1)) {
#ifdef __3DS__
		if (gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits) {
			gN3DSPortraitCache.mFound.push_back(gPrismMugenSpriteFileReaderData.mReader.mGetCurrentOffset(&gPrismMugenSpriteFileReaderData.mReader) - sizeof(SFFSubFileHeader));
		}
#endif
		loadSingleSFFFileAndInsertIntoSpriteFile(subHeader, tDst);
	}

	if (!subHeader.mNextFilePosition) {
		gPrismMugenSpriteFileReaderData.mReader.mSetOver(&gPrismMugenSpriteFileReaderData.mReader);
	}
	else {
		gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, subHeader.mNextFilePosition);		
	}
}

static void loadSFFHeader(SFFHeader* tHeader) {
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, tHeader, sizeof(SFFHeader));
}

static MugenSpriteFile makeEmptySpriteFile() {
	MugenSpriteFile ret;
	ret.mPaletteMappedGroup = ret.mPaletteMappedItem = 1;
	return ret;
}

static void loadMugenSpriteFilePaletteFile(MugenSpriteFile* ret, const char* tPath) {
	assert(isFile(tPath));

	auto b = fileToBuffer(tPath);
	auto src = fileToBuffer(tPath);

	int i;
	for (i = 0; i < 256; i++) {
		int j = 255 - i;
		((uint8_t*)b.mData)[j * 3 + 0] = ((uint8_t*)src.mData)[i * 3 + 0];
		((uint8_t*)b.mData)[j * 3 + 1] = ((uint8_t*)src.mData)[i * 3 + 1];
		((uint8_t*)b.mData)[j * 3 + 2] = ((uint8_t*)src.mData)[i * 3 + 2];
	}

	freeBuffer(src);
	insertPaletteIntoMugenSpriteFile(ret, b);
}

static MugenSpriteFile loadMugenSpriteFile1(int tHasPaletteFile, const char* tOptionalPaletteFile) {
	MugenSpriteFile ret = makeEmptySpriteFile();
	
	if (tHasPaletteFile) {
		loadMugenSpriteFilePaletteFile(&ret, tOptionalPaletteFile);
	}
	
	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, 0);

	SFFHeader header;
	loadSFFHeader(&header);

	verboseInteger(header.mGroupAmount);
	verboseInteger(header.mImageAmount);
	verboseInteger(header.mFirstFileOffset);

	gPreviousGroup = -1;
	gLoadedSubFileAmount = 0;
	resetPCXBasePalette();

#ifdef __3DS__
	if (const auto positions = findN3DSPortraitCacheEntry()) {
		int isValid = 1;
		for (const auto position : *positions) {
			SFFSubFileHeader subHeader;
			gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, position);
			gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &subHeader, sizeof(SFFSubFileHeader));
			if (subHeader.mGroup != 9000 || subHeader.mImage > 1) isValid = 0;
		}
		if (isValid) {
			for (const auto position : *positions) {
				SFFSubFileHeader subHeader;
				gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, position);
				gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &subHeader, sizeof(SFFSubFileHeader));
				loadSingleSFFFileAndInsertIntoSpriteFile(subHeader, &ret);
			}
			return ret;
		}
	}
	gN3DSPortraitCache.mFound.clear();
#endif

	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, header.mFirstFileOffset);
	while (!gPrismMugenSpriteFileReaderData.mReader.mIsOver(&gPrismMugenSpriteFileReaderData.mReader)) {
		loadSingleSFFFile(&ret);
		if(gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits && ret.mAllSprites.size() == 2) break;
	}

#ifdef __3DS__
	addN3DSPortraitCacheEntry(ret);
#endif
	return ret;
}

static void loadSFFHeader2(SFFHeader2* tHeader) {
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, tHeader, sizeof(SFFHeader2));
}

static Buffer processRawPalette2FixedAlpha(Buffer tRaw) {
	int n = tRaw.mLength / 4;

	assert(n <= 256);
	char* raw = (char*)tRaw.mData;
	char* out = (char*)allocMemory(256 * 3);
	memset(out, 0, 256 * 3);

	int i = 0;
	for (i = 0; i < n; i++)
	{
		out[3 * i + 0] = raw[4 * i + 0];
		out[3 * i + 1] = raw[4 * i + 1];
		out[3 * i + 2] = raw[4 * i + 2];
	}

	return makeBufferOwned(out, 256 * 3);
}

static Buffer processRawPalette2AlphaFromBuffer(Buffer tRaw) {
	int n = tRaw.mLength / 4;

	assert(n <= 256);
	uint8_t* raw = (uint8_t*)tRaw.mData;
	uint8_t* out = (uint8_t*)allocMemory(256 * 4);
	memset(out, 0, 256 * 4);

	int i = 0;
	for (i = 0; i < n; i++) {
		out[4 * i + 1] = raw[4 * i + 0];
		out[4 * i + 2] = raw[4 * i + 1];
		out[4 * i + 3] = raw[4 * i + 2];
		out[4 * i + 0] = (i == 0) ? 0 : raw[4 * i + 3];
	}

	return makeBufferOwned(out, 256 * 4);
}

static bool isUsingSpecialPalette2Loading(Buffer tRaw) // SF2_Ryu (the HD remake one) expects its alpha values to be respected, which seemingly breaks all other SFFv2 1.0 palettes
{
	int n = tRaw.mLength / 4;
	assert(n <= 256);
	
	uint8_t* raw = (uint8_t*)tRaw.mData;
	return n == 256 && raw[3] != 0x0; // no clue if this even holds up
}

static Buffer processRawPalette2(Buffer tRaw) 
{
	if (isUsingSpecialPalette2Loading(tRaw))
	{
		return processRawPalette2AlphaFromBuffer(tRaw);
	}
	else
	{
		return processRawPalette2FixedAlpha(tRaw);
	}
}

static void loadSinglePalette2(SFFHeader2* tHeader, MugenSpriteFile* tDst) {
	SFFPalette2 palette;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &palette, sizeof(SFFPalette2));

	verboseInteger(palette.mDataOffset);
	verboseInteger(palette.mDataLength);
	verboseInteger(palette.mIndex);

	uint32_t originalPosition = gPrismMugenSpriteFileReaderData.mReader.mGetCurrentOffset(&gPrismMugenSpriteFileReaderData.mReader);
	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, tHeader->mLDataOffset + palette.mDataOffset);
	Buffer rawPalette = gPrismMugenSpriteFileReaderData.mReader.mReadBufferReadOnly(&gPrismMugenSpriteFileReaderData.mReader, palette.mDataLength);
	auto processedPalette = processRawPalette2(rawPalette);
	freeBuffer(rawPalette);
	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, originalPosition);

	insertPaletteIntoMugenSpriteFile(tDst, processedPalette, palette.mGroupNo, palette.mItemNo);
}

static void loadPalettes2(SFFHeader2* tHeader, MugenSpriteFile* tDst) {
	int i = 0;

	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, tHeader->mPaletteOffset);
	for (i = 0; i < (int)tHeader->mPaletteTotal; i++) {
		loadSinglePalette2(tHeader, tDst);
	}
}


static Buffer readRawRLE8Sprite2(uint32_t tSize) {
	uint32_t decompressedSize = 0;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &decompressedSize, sizeof(uint32_t));
	Buffer b = gPrismMugenSpriteFileReaderData.mReader.mReadBufferReadOnly(&gPrismMugenSpriteFileReaderData.mReader, tSize - 4);
	Buffer ret = decodeRLE8BufferAndReturnOwnedBuffer(b, decompressedSize);
	freeBuffer(b);

	return ret;
}

static Buffer decodeLZ5BufferAndReturnOwnedBuffer(const Buffer& b, uint32_t tFinalSize) {
	uint8_t* out = (uint8_t*)allocMemory(tFinalSize + 10);
	uint8_t* src = (uint8_t*)b.mData;

	decompressLZ5(out, src, b.mLength);

	return makeBufferOwned(out, tFinalSize);
}

static Buffer readRawLZ5Sprite2(uint32_t tSize) {
	uint32_t decompressedSize = 0;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &decompressedSize, sizeof(uint32_t));
	Buffer b = gPrismMugenSpriteFileReaderData.mReader.mReadBufferReadOnly(&gPrismMugenSpriteFileReaderData.mReader, tSize - 4);
	Buffer ret = decodeLZ5BufferAndReturnOwnedBuffer(b, decompressedSize);
	freeBuffer(b);

	return ret;
}

static Buffer readRawUncompressedSprite2(uint32_t tSize) {
	uint8_t* out = (uint8_t*)allocMemory(tSize + 10);

	Buffer b = gPrismMugenSpriteFileReaderData.mReader.mReadBufferReadOnly(&gPrismMugenSpriteFileReaderData.mReader, tSize);
	memcpy(out, b.mData, tSize);
	freeBuffer(b);

	return makeBufferOwned(out, tSize);
}


static uint32_t getSpriteDataOffset(SFFSprite2* tSprite, SFFHeader2* tHeader) {
	uint32_t realOffset;
	if (tSprite->mFlags) realOffset = tHeader->mTDataOffset;
	else realOffset = tHeader->mLDataOffset;

	return realOffset + tSprite->mDataOffset;
}

static Buffer readRawSprite2(SFFSprite2* tSprite, SFFHeader2* tHeader) {
	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, getSpriteDataOffset(tSprite, tHeader));

	if (tSprite->mFormat == 0) {
		return readRawUncompressedSprite2(tSprite->mDataLength);
	}
	else if (tSprite->mFormat == 2) {		
		return readRawRLE8Sprite2(tSprite->mDataLength);
	} else if (tSprite->mFormat == 4) {
		return readRawLZ5Sprite2(tSprite->mDataLength);
	}
	else {
		logError("Unable to parse sprite format.");
		logErrorInteger(tSprite->mFormat);
		recoverFromError();
		return makeBuffer(NULL, 0);
	}
	


}

static void insertLinkedTextureIntoSpriteFile2(MugenSpriteFile* tDst, SFFSprite2* tSprite) {
	MugenSpriteFileSprite e = makeLinkedMugenSpriteFileSprite(tSprite->mIndex, Vector2D(tSprite->mAxisX, tSprite->mAxisY));
	insertTextureIntoSpriteFile(tDst, e, tSprite->mGroupNo, tSprite->mItemNo);
}

static void loadSingleSprite2(SFFHeader2* tHeader, MugenSpriteFile* tDst, int tHasPalette) {
	SFFSprite2 sprite;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &sprite, sizeof(SFFSprite2));

	verboseLog("Load sprite2");
	verboseInteger(sprite.mGroupNo);
	verboseInteger(sprite.mItemNo);
	verboseInteger(sprite.mFormat);

	if (gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits && (sprite.mGroupNo != 9000 || sprite.mItemNo > 1)) {
		return;
	}

	if (!sprite.mDataLength) {
		insertLinkedTextureIntoSpriteFile2(tDst, &sprite);
		return;
	}

	int isPaletted = sprite.mFormat == 0 || sprite.mFormat == 2 || sprite.mFormat == 4;
	int isRawPNG = sprite.mFormat == 10 || sprite.mFormat == 11 || sprite.mFormat == 12;
	const int palette = tHasPalette ? sprite.mPaletteIndex + 1 : sprite.mPaletteIndex;

	MugenSpriteFileSprite e;
#ifdef N3DS_HEAP_GUARD
	// A PNG is decoded to 32 bits and copied once more in pieces (twice its
	// pixels in 4 bytes), whatever its depth; the others, its 8-bit pixels.
	if ((isPaletted || isRawPNG) && !hasN3DSHeapRoomForSprite((size_t)sprite.mWidth * sprite.mHeight * (isRawPNG ? 8 : 3) + sprite.mDataLength)) {
		// Paletted ones read again from the file when drawn; the others left out.
		const int kind = sprite.mFormat == 0 ? N3DS_STREAM_RAW : sprite.mFormat == 2 ? N3DS_STREAM_RLE8 : sprite.mFormat == 4 ? N3DS_STREAM_LZ5 : sprite.mFormat == 10 ? N3DS_STREAM_PNG8 : -1;
		if (kind >= 0 && palette < int(tDst->mPalettes.size())) {
			gN3DSSpritesLeftOut--;
			const Buffer* paletteBuffer = (gPrismMugenSpriteFileReaderData.mIsUsingRealPalette && palette == 0) ? NULL : &tDst->mPalettes[palette].mBuffer;
			const uint32_t offset = getSpriteDataOffset(&sprite, tHeader) + (kind == N3DS_STREAM_PNG8 ? 4 : 0);
			e = makeN3DSStreamedSprite(kind, offset, sprite.mDataLength, sprite.mWidth, sprite.mHeight, sprite.mWidth, paletteBuffer, Vector2D(sprite.mAxisX, sprite.mAxisY));
		}
		else {
			e = makeMugenSpriteFileSprite(std::vector<MugenSpriteFileSubSprite>(), makeTextureSize(sprite.mWidth, sprite.mHeight), Vector2D(sprite.mAxisX, sprite.mAxisY));
		}
	}
	else
#endif
	if (isPaletted) {
		uint32_t originalPosition = gPrismMugenSpriteFileReaderData.mReader.mGetCurrentOffset(&gPrismMugenSpriteFileReaderData.mReader);

		Buffer rawBuffer = readRawSprite2(&sprite, tHeader);

		if (gPrismMugenSpriteFileReaderData.mIsUsingRealPalette && palette == 0) {
			e =  makeMugenSpriteFileSpriteFromRawBuffer(rawBuffer, sprite.mWidth, sprite.mHeight, sprite.mWidth, Vector2D(sprite.mAxisX, sprite.mAxisY));
		}
		else {
			auto& paletteElement = tDst->mPalettes[palette];
			e = makeMugenSpriteFileSpriteFromRawAndPaletteBuffer(rawBuffer, paletteElement.mBuffer, sprite.mWidth, sprite.mHeight, sprite.mWidth, Vector2D(sprite.mAxisX, sprite.mAxisY));
		}
		gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, originalPosition);
	}
	else if (isRawPNG) {
		uint32_t originalPosition = gPrismMugenSpriteFileReaderData.mReader.mGetCurrentOffset(&gPrismMugenSpriteFileReaderData.mReader);
		gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, getSpriteDataOffset(&sprite, tHeader));
		uint16_t textureWidth, textureHeight;
		gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &textureWidth, 2);
		gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &textureHeight, 2);
		Buffer pngBuffer = gPrismMugenSpriteFileReaderData.mReader.mReadBufferReadOnly(&gPrismMugenSpriteFileReaderData.mReader, sprite.mDataLength);
		e = makeMugenSpriteFileSpriteFromRawPNGBuffer(pngBuffer, sprite.mWidth, sprite.mHeight, sprite.mWidth, Vector2D(sprite.mAxisX, sprite.mAxisY), sprite.mColorDepth, tDst->mPalettes, palette);
		gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, originalPosition);
	}
	else {
		logError("Unrecognized image format.");
		logErrorInteger(sprite.mFormat);
		recoverFromError();
	}

	insertTextureIntoSpriteFile(tDst, e, sprite.mGroupNo, sprite.mItemNo);
}

static void loadSprites2(SFFHeader2* tHeader, MugenSpriteFile* tDst, int tHasPalette) {
	int i = 0;
	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, tHeader->mSpriteOffset);
	for (i = 0; i < (int)tHeader->mSpriteTotal; i++) {
#ifdef __3DS__
		const size_t spritesBefore = tDst->mAllSprites.size();
#endif
		loadSingleSprite2(tHeader, tDst, tHasPalette);
#ifdef __3DS__
		if (gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits && tDst->mAllSprites.size() > spritesBefore) gN3DSPortraitCache.mFound.push_back(i);
#endif
		if(gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits && tDst->mAllSprites.size() == 2) break;
	}
}

static MugenSpriteFile loadMugenSpriteFile2(int tHasPaletteFile, const char* tOptionalPaletteFile) {
	MugenSpriteFile ret = makeEmptySpriteFile();

	if (tHasPaletteFile) {
		loadMugenSpriteFilePaletteFile(&ret, tOptionalPaletteFile);
	}

	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, 0);

	SFFHeader2 header;
	loadSFFHeader2(&header);

	verboseInteger(header.mSpriteOffset);
	verboseInteger(header.mSpriteTotal);
	verboseInteger(header.mPaletteOffset);
	verboseInteger(header.mPaletteTotal);

	loadPalettes2(&header, &ret);

#ifdef __3DS__
	if (const auto indices = findN3DSPortraitCacheEntry()) {
		int isValid = 1;
		for (const auto index : *indices) {
			SFFSprite2 sprite;
			if (index >= header.mSpriteTotal) { isValid = 0; break; }
			gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, header.mSpriteOffset + index * sizeof(SFFSprite2));
			gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &sprite, sizeof(SFFSprite2));
			if (sprite.mGroupNo != 9000 || sprite.mItemNo > 1) isValid = 0;
		}
		if (isValid) {
			for (const auto index : *indices) {
				gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, header.mSpriteOffset + index * sizeof(SFFSprite2));
				loadSingleSprite2(&header, &ret, tHasPaletteFile);
			}
			return ret;
		}
	}
	gN3DSPortraitCache.mFound.clear();
#endif
	loadSprites2(&header, &ret, tHasPaletteFile);

#ifdef __3DS__
	addN3DSPortraitCacheEntry(ret);
#endif
	return ret;
}

static Buffer loadPaddedBufferPreloaded() {
	uint32_t size;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &size, 4);

	char* dst = (char*)allocMemory(size);
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, dst, size);

	char padBuffer[10];
	int pad = (4 - (size % 4)) % 4;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, padBuffer, pad);

	return makeBufferOwned(dst, size);
}

static void loadSinglePalettePreloaded(MugenSpriteFile* tDst) {

	auto b = loadPaddedBufferPreloaded();
	insertPaletteIntoMugenSpriteFile(tDst, b);
}

static void loadPalettesPreloaded(MugenSpriteFile* tDst) {
	uint32_t amount;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &amount, 4);

	uint32_t i;
	for (i = 0; i < amount; i++) {
		loadSinglePalettePreloaded(tDst);
	}

}

typedef struct {
	Vector3DI mOffset;
	int32_t mHasPalette;
	TextureSize mTextureSize;
} SubspriteHeader;

static MugenSpriteFileSubSprite loadSingleSpriteSubSpritePreloaded() {
	SubspriteHeader header;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &header, sizeof(SubspriteHeader));

	Buffer b = loadPaddedBufferPreloaded();
	decompressBufferZSTD(&b);

	TextureData data;
	if (header.mHasPalette) {
		data = loadPalettedTextureFrom8BitBuffer(b, gPrismMugenSpriteFileReaderData.mPaletteID, header.mTextureSize.x, header.mTextureSize.y);
	}
	else {
		data = loadTextureFromTwiddledARGB16Buffer(b, header.mTextureSize.x, header.mTextureSize.y);
	}

	freeBuffer(b);

	MugenSpriteFileSubSprite newSprite;
	newSprite.mOffset = header.mOffset.xy();
	newSprite.mTexture = data;

	return newSprite;
}

typedef struct {
	uint32_t mBlockSpriteAmount;
	uint32_t mBlockSize;
} PreloadedBlock;

typedef struct {
	int32_t mIsLinked;
	int32_t mIsLinkedTo;

	float mAxisOffsetX;
	float mAxisOffsetY;
	float mAxisOffsetZ;

	int32_t mGroupNumber;
	int32_t mSpriteNumber;

	int32_t mOriginalTextureSizeX;
	int32_t mOriginalTextureSizeY;

	uint32_t mSubspriteAmount;
} PreloadedSprite;

static void loadSingleSpritePreloaded(MugenSpriteFile* tDst) {
	PreloadedSprite spriteHeader;

	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &spriteHeader, sizeof(PreloadedSprite));

	Vector2D axisOffset = Vector2D(spriteHeader.mAxisOffsetX, spriteHeader.mAxisOffsetY);

	if (spriteHeader.mIsLinked) {
		MugenSpriteFileSprite sprite = makeLinkedMugenSpriteFileSprite(spriteHeader.mIsLinkedTo, axisOffset);
		insertTextureIntoSpriteFile(tDst, sprite, spriteHeader.mGroupNumber, spriteHeader.mSpriteNumber);
		return;
	}

	std::vector<MugenSpriteFileSubSprite> textures;
	uint32_t i;
	for (i = 0; i < spriteHeader.mSubspriteAmount; i++) {
		auto subSprite = loadSingleSpriteSubSpritePreloaded();
		textures.push_back(subSprite);
	}

	MugenSpriteFileSprite sprite = makeMugenSpriteFileSprite(textures, makeTextureSize(spriteHeader.mOriginalTextureSizeX, spriteHeader.mOriginalTextureSizeY), axisOffset);
	insertTextureIntoSpriteFile(tDst, sprite, spriteHeader.mGroupNumber, spriteHeader.mSpriteNumber);
}

static void loadSingleBlockPreloaded(MugenSpriteFile* tDst) {
	PreloadedBlock header;

	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &header, sizeof(PreloadedBlock));

	char* buf = (char*)allocMemory(header.mBlockSize + 2);
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, buf, header.mBlockSize);
	Buffer b = makeBufferOwned(buf, header.mBlockSize);

	FileReader originalReader = gPrismMugenSpriteFileReaderData.mReader;
	setMugenSpriteFileReaderToBuffer();
	initBufferFileReaderReadOnlyBuffer(&gPrismMugenSpriteFileReaderData.mReader, b);

	for (uint32_t i = 0; i < header.mBlockSpriteAmount; i++) {
		loadSingleSpritePreloaded(tDst);
	}

	gPrismMugenSpriteFileReaderData.mReader.mDelete(&gPrismMugenSpriteFileReaderData.mReader);
	gPrismMugenSpriteFileReaderData.mReader = originalReader;
}

static void loadSpritesPreloaded(MugenSpriteFile* tDst) {
	uint32_t amount;

	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &amount, 4);
	uint32_t i;
	for (i = 0; i < amount; i++) {
		loadSingleBlockPreloaded(tDst);
	}

}

static MugenSpriteFile loadMugenSpriteFilePreloaded(int tHasPaletteFile, const char* tOptionalPaletteFile) {
	MugenSpriteFile ret = makeEmptySpriteFile();

	if (tHasPaletteFile) {
		loadMugenSpriteFilePaletteFile(&ret, tOptionalPaletteFile);
	}
	gPrismMugenSpriteFileReaderData.mReader.mSeek(&gPrismMugenSpriteFileReaderData.mReader, 0);
	SFFSharedHeader header;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &header, sizeof(SFFSharedHeader));

	loadPalettesPreloaded(&ret);
	loadSpritesPreloaded(&ret);

	return ret;
}

static void checkMugenSpriteFileReader() {
	if (gPrismMugenSpriteFileReaderData.mReader.mType == FILE_READER_TYPE_NONE) {
		setMugenSpriteFileReaderToFileOperations();
	}
}

static MugenSpriteFile loadMugenSpriteFileGeneral(const char * tPath, int tHasPaletteFile, const char* tOptionalPaletteFile)
{
	logFormat("Loading sprite file: %s", tPath);

	if (!isFile(tPath)) {
		logErrorFormat("Unable to open sprite file %s. Aborting.", tPath);
		recoverFromError();
	}
	MugenSpriteFile ret;
	char preloadPath[1024];
	sprintf(preloadPath, "%s.preloaded", tPath);
	if (isFile(preloadPath) && !gPrismMugenSpriteFileReaderData.mIsWritingDreamcastOptimized) {

		ret = loadMugenSpriteFileGeneral(preloadPath, tHasPaletteFile, tOptionalPaletteFile);

		debugFormat("Memory State after pre-loading: %s as %s", tPath, preloadPath);
		debugLogTextureMemoryState();
		debugLogMemoryState();

		return ret;
	}

	gPrismMugenSpriteFileReaderData.mHasPaletteFile = tHasPaletteFile;
	checkMugenSpriteFileReader();

#ifdef __3DS__
	// [3DS] Read in large reads: the SD card is slow at small ones (Kung Fu
	// Man's sprites: 3.4 to 4.5 s in small reads, 0.4 s through a 64 KB
	// buffer, 0.3 s whole; Morrigan's 20 MB, 5.4 s of SD through 64 KB with a
	// seek per sprite). Small files whole, the rest a megabyte at a time (the
	// whole of a big one took heap the sprites needed).
	const FileReader originalN3DSReader = gPrismMugenSpriteFileReaderData.mReader;
	const auto n3dsFileSize = getFileSize(tPath);
	gN3DSPortraitCache.mKey = std::string(tPath) + "|" + std::to_string((unsigned long)n3dsFileSize);
	int n3dsVariant = n3dsFileSize <= 2 * 1024 * 1024 ? N3DS_SPRITE_READ_WHOLE : N3DS_SPRITE_READ_WINDOWED;
	gN3DSSpritesLeftOut = 0;
	gN3DSStreamedSprites = 0;
	gN3DSStreamPath = &*gN3DSStreamPaths.insert(std::string(tPath)).first;
	gN3DSHeapUsedCheckedSprites = 0;
	// Portraits found before: only a few reads, through a small buffer.
	if (findN3DSPortraitCacheEntry()) n3dsVariant = N3DS_SPRITE_READ_PORTRAITS;
	const N3DSPerfCounters n3dsBefore = gN3DSPerf;
	const uint64_t n3dsStart = getN3DSPerfTicks();
	if (n3dsVariant == N3DS_SPRITE_READ_WHOLE) gPrismMugenSpriteFileReaderData.mReader = getBufferFileReader();
	if (n3dsVariant == N3DS_SPRITE_READ_WINDOWED) gPrismMugenSpriteFileReaderData.mReader = getWindowedFileFileReader();
	if (n3dsVariant == N3DS_SPRITE_READ_BIG_BUFFER) gN3DSFileBufferSize = 64 * 1024;
	if (n3dsVariant == N3DS_SPRITE_READ_PORTRAITS) gN3DSFileBufferSize = 4 * 1024;
#elif defined(N3DS_TEXTURE_SIM)
	// As on the 3DS: big files a window at a time.
	const FileReader originalN3DSReader = gPrismMugenSpriteFileReaderData.mReader;
	gN3DSSpritesLeftOut = 0;
	gN3DSStreamedSprites = 0;
	gN3DSStreamPath = &*gN3DSStreamPaths.insert(std::string(tPath)).first;
	gN3DSHeapUsedCheckedSprites = 0;
	if (getFileSize(tPath) > 2 * 1024 * 1024) gPrismMugenSpriteFileReaderData.mReader = getWindowedFileFileReader();
#endif
	gPrismMugenSpriteFileReaderData.mReader.mInit(&gPrismMugenSpriteFileReaderData.mReader, tPath);
#ifdef __3DS__
	gN3DSFileBufferSize = 0;
#endif

	SFFSharedHeader header;
	gPrismMugenSpriteFileReaderData.mReader.mRead(&gPrismMugenSpriteFileReaderData.mReader, &header, sizeof(SFFSharedHeader));

	if (header.mVersion[1] == 1 && header.mVersion[3] == 1) {
		ret = loadMugenSpriteFile1(tHasPaletteFile, tOptionalPaletteFile);
	} else if (header.mVersion[1] == 1 && header.mVersion[3] == 2) {
		ret = loadMugenSpriteFile2(tHasPaletteFile, tOptionalPaletteFile);
	}
	else if (header.mVersion[1] == 0 && header.mVersion[3] == 2) {
		ret = loadMugenSpriteFile2(tHasPaletteFile, tOptionalPaletteFile);
	}
	else if (header.mVersion[1] == 0 && header.mVersion[3] == 100) {
		ret = loadMugenSpriteFilePreloaded(tHasPaletteFile, tOptionalPaletteFile);
	}
	else {
		logError("Unrecognized SFF version.");
		logErrorInteger(header.mVersion[0]);
		logErrorInteger(header.mVersion[1])  ;
		logErrorInteger(header.mVersion[2]);
		logErrorInteger(header.mVersion[3]);
		recoverFromError();
		return MugenSpriteFile();
	}

	gPrismMugenSpriteFileReaderData.mReader.mDelete(&gPrismMugenSpriteFileReaderData.mReader);
#if defined(N3DS_TEXTURE_SIM) && !defined(__3DS__)
	gPrismMugenSpriteFileReaderData.mReader = originalN3DSReader;
	if (gN3DSSpritesLeftOut) fprintf(stderr, "n3dssim: %s: %d sprites left out, no room in the heap\n", tPath, gN3DSSpritesLeftOut);
	if (gN3DSStreamedSprites) fprintf(stderr, "n3dssim: %s: %u sprites streamed (read when drawn)\n", tPath, gN3DSStreamedSprites);
#endif
#ifdef __3DS__
	gPrismMugenSpriteFileReaderData.mReader = originalN3DSReader;
	{
		static const char* variantNames[] = { "small reads", "64 KB buffer", "whole file", "portraits from the cache, 4 KB buffer", "512 KB windows" };
		const double total = getN3DSPerfMilliseconds(getN3DSPerfTicks() - n3dsStart);
		const double io = getN3DSPerfMilliseconds((gN3DSPerf.mOpenTicks - n3dsBefore.mOpenTicks) + (gN3DSPerf.mReadTicks - n3dsBefore.mReadTicks) + (gN3DSPerf.mSeekTicks - n3dsBefore.mSeekTicks));
		const double tex = getN3DSPerfMilliseconds(gN3DSPerf.mTextureTicks - n3dsBefore.mTextureTicks);
		addN3DSPerfLine("  sprites %s (%s): %.0f ms | SD %.0f ms (%u reads, %u seeks, %.0f KB) | textures %.0f ms (%u) | decoding and the rest %.0f ms",
			tPath, variantNames[n3dsVariant], total, io, gN3DSPerf.mReads - n3dsBefore.mReads, gN3DSPerf.mSeeks - n3dsBefore.mSeeks,
			(gN3DSPerf.mReadBytes - n3dsBefore.mReadBytes) / 1024.0, tex, gN3DSPerf.mTextures - n3dsBefore.mTextures, total - io - tex);
		if (gN3DSSpritesLeftOut) addN3DSPerfLine("  %d sprites left out: no room in the heap", gN3DSSpritesLeftOut);
		if (gN3DSStreamedSprites) addN3DSPerfLine("  %u sprites streamed: read from the file when drawn", gN3DSStreamedSprites);
	}
#endif

	debugFormat("Memory State after loading: %s", tPath);
	debugLogTextureMemoryState();
	debugLogMemoryState();

	return ret;
}

MugenSpriteFile loadMugenSpriteFile(const char * tPath, int tHasPaletteFile, const char* tOptionalPaletteFile) {
	gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits = 0;
	return loadMugenSpriteFileGeneral(tPath, tHasPaletteFile, tOptionalPaletteFile);
}

MugenSpriteFile loadMugenSpriteFilePortraits(const char * tPath, int tHasPaletteFile, const char* tOptionalPaletteFile) {
	gPrismMugenSpriteFileReaderData.mIsOnlyLoadingPortraits = 1;
	return loadMugenSpriteFileGeneral(tPath, tHasPaletteFile, tOptionalPaletteFile);
}

MugenSpriteFile loadMugenSpriteFileWithoutPalette(const std::string& tPath)
{
	char path[1024];
	strcpy(path, tPath.data());
	return loadMugenSpriteFileWithoutPalette(path);
}

MugenSpriteFile loadMugenSpriteFileWithoutPalette(const char * tPath)
{
	return loadMugenSpriteFile(tPath, 0, NULL);
}

static void unloadSinglePalette(MugenSpriteFilePalette& palette) {
	freeBuffer(palette.mBuffer);
}

void unloadMugenSpriteFile(MugenSpriteFile* tFile) {
	for (auto& group : tFile->mGroups)
	{
		for (auto& sprite : group.second.mSprites)
		{
			unloadMugenSpriteFileSprite(&sprite.second);
		}
	}
	tFile->mGroups.clear();

	for (auto& palette : tFile->mPalettes)
	{
		unloadSinglePalette(palette);
	}
	tFile->mPalettes.clear();
	tFile->mAllSprites.clear();
}

void unloadMugenSpriteFileSprite(MugenSpriteFileSprite* tSprite) {

	if (!tSprite->mIsLinked) {
		for (auto& texture : tSprite->mTextures)
		{
			unloadTexture(texture.mTexture);
		}
		tSprite->mTextures.clear();
	}
}

static MugenSpriteFileSprite* getLinkedSpriteTargetOrNullOnBrokenLink(MugenSpriteFile* tFile, MugenSpriteFileSprite* tSprite);

MugenSpriteFileSprite* getMugenSpriteFileTextureReference(MugenSpriteFile* tFile, int tGroup, int tSprite)
{
	if (!tFile) return NULL;
	if (!stl_map_contains(tFile->mGroups, tGroup)) return NULL;

	auto& g = tFile->mGroups[tGroup];
	if (!stl_map_contains(g.mSprites, tSprite)) return NULL;

	auto& original = g.mSprites[tSprite];
	if (original.mIsLinked) {
		MugenSpriteFileSprite* e = getLinkedSpriteTargetOrNullOnBrokenLink(tFile, &original);
		if (e) {
			original.mOriginalTextureSize = e->mOriginalTextureSize;
			original.mTextures = e->mTextures;
		}
		else {
			original.mIsLinked = 0;
		}
	}
	return &g.mSprites[tSprite];
}

static MugenSpriteFileSprite* getLinkedSpriteTargetOrNullOnBrokenLink(MugenSpriteFile* tFile, MugenSpriteFileSprite* tSprite) {
	const auto spriteAmount = tFile->mAllSprites.size();
	MugenSpriteFileSprite* e = tSprite;
	size_t followedLinkAmount = 0;
	while (e->mIsLinked) {
		if (e->mIsLinkedTo < 0 || size_t(e->mIsLinkedTo) >= spriteAmount) {
			logWarningFormat("[MugenSpriteFileReader] Sprite links to out-of-range index %d of %d. Ignoring link.", e->mIsLinkedTo, int(spriteAmount));
			return NULL;
		}
		if (followedLinkAmount++ >= spriteAmount) {
			logWarningFormat("[MugenSpriteFileReader] Cyclic sprite link reaching index %d. Ignoring link.", e->mIsLinkedTo);
			return NULL;
		}
		e = tFile->mAllSprites[e->mIsLinkedTo];
	}
	return e;
}

void setMugenSpriteFileReaderToBuffer()
{
	gPrismMugenSpriteFileReaderData.mReader = getBufferFileReader();
}

void setMugenSpriteFileReaderToFileOperations()
{
	gPrismMugenSpriteFileReaderData.mReader = getFileFileReader();
}

void setMugenSpriteFileReaderToUsePalette(int tPaletteID)
{
	gPrismMugenSpriteFileReaderData.mIsUsingRealPalette = 1;
	gPrismMugenSpriteFileReaderData.mPaletteID = tPaletteID;
}

void setMugenSpriteFileReaderToNotUsePalette()
{
	gPrismMugenSpriteFileReaderData.mIsUsingRealPalette = 0;
}

void setMugenSpriteFileReaderCustomFunctionsAndForceARGB16(TextureData(*tCustomLoadTextureFromARGB16Buffer)(const Buffer&, int, int), TextureData(*tCustomLoadTextureFromARGB32Buffer)(const Buffer&, int, int), TextureData(*tCustomLoadPalettedTextureFrom8BitBuffer)(const Buffer&, int, int, int))
{
	gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB16Buffer = tCustomLoadTextureFromARGB16Buffer;
	gPrismMugenSpriteFileReaderData.mCustomLoadTextureFromARGB32Buffer = tCustomLoadTextureFromARGB32Buffer;
	gPrismMugenSpriteFileReaderData.mCustomLoadPalettedTextureFrom8BitBuffer = tCustomLoadPalettedTextureFrom8BitBuffer;
	gPrismMugenSpriteFileReaderData.mIsWritingDreamcastOptimized = 1;
}

void setMugenSpriteFileReaderSubTextureSplit(int tSubTextureSplitMin, int tSubTextureSplitMax)
{
	gPrismMugenSpriteFileReaderData.mSubTextureSplitMin = tSubTextureSplitMin;
	gPrismMugenSpriteFileReaderData.mSubTextureSplitMax = tSubTextureSplitMax;
}

int hasMugenSprite(MugenSpriteFile* tSprites, int tGroup, int tSprite)
{
	if (!tSprites) return 0;
	if (!stl_map_contains(tSprites->mGroups, tGroup)) return 0;
	const auto& g = tSprites->mGroups[tGroup];
	return stl_map_contains(g.mSprites, tSprite);
}

void remapMugenSpriteFilePalette(MugenSpriteFile* tSprites, const Vector2DI& tSource, const Vector2DI& tDestination, int tPaletteID)
{
	if (tSource.x != 1 || tSource.y != 1) return;
	if (tDestination.x == tSprites->mPaletteMappedGroup && tDestination.y == tSprites->mPaletteMappedItem) return;

	for (int i = 0; i < int(tSprites->mPalettes.size()); i++) {
		auto& paletteElement = tSprites->mPalettes[i];
		if (paletteElement.mGroup != tDestination.x || paletteElement.mItem != tDestination.y) continue;
		assert(isRGBPalette(paletteElement.mBuffer) || isARGBPalette(paletteElement.mBuffer));
		if (isRGBPalette(paletteElement.mBuffer))
		{
			setPaletteFromBGR256WithFirstValueTransparentBuffer(tPaletteID, paletteElement.mBuffer);
		}
		else
		{
			setPaletteFromARGB256Buffer(tPaletteID, paletteElement.mBuffer);
		}
		tSprites->mPaletteMappedGroup = paletteElement.mGroup;
		tSprites->mPaletteMappedItem = paletteElement.mItem;
	}
}

#ifdef _WIN32
static void imguiMugenSpriteFileSubSprite(MugenSpriteFileSubSprite& tSubSprite) {
	ImGui::Text("Offset = %d / %d", tSubSprite.mOffset.x, tSubSprite.mOffset.y);
	imguiTextureData("TextureData", tSubSprite.mTexture);
}

static void imguiMugenSpriteFileSpriteSingle(MugenSpriteFileSprite& tSprite) {
	imguiVector("Textures", tSprite.mTextures, imguiMugenSpriteFileSubSprite);
	ImGui::Text("OriginalTextureSize = %d / %d", tSprite.mOriginalTextureSize.x, tSprite.mOriginalTextureSize.y);
	ImGui::Text("IsLinked = %d", tSprite.mIsLinked);
	ImGui::Text("IsLinkedTo = %d", tSprite.mIsLinkedTo);
	ImGui::Text("AxisOffset = %d / %d", tSprite.mAxisOffset.x, tSprite.mAxisOffset.y);
}

static void imguiMugenSpriteFileSprite(MugenSpriteFileSprite& tSprite, int tSpriteID) {
	if (ImGui::TreeNode(std::to_string(tSpriteID).c_str()))
	{
		imguiMugenSpriteFileSpriteSingle(tSprite);
		ImGui::TreePop();
	}
}

static void imguiMugenSpriteFileGroup(MugenSpriteFileGroup& tGroup, int tGroupID) {
	if (ImGui::TreeNode(std::to_string(tGroupID).c_str()))
	{
		if (ImGui::TreeNode("MugenSpriteFileSprites"))
		{
			for (auto& sprite : tGroup.mSprites)
			{
				imguiMugenSpriteFileSprite(sprite.second, sprite.first);
			}
			ImGui::TreePop();
		}

		ImGui::TreePop();
	}
}

static void imguiMugenSpriteFilePalette(MugenSpriteFilePalette& tPalette)
{
	ImGui::Text("Group = %d", tPalette.mGroup);
	ImGui::Text("Item = %d", tPalette.mItem);
	imguiBuffer("Buffer", tPalette.mBuffer);
}

void imguiMugenSpriteFile(MugenSpriteFile& tSprites, const std::string_view& tName)
{
	if (ImGui::TreeNode(tName.data()))
	{
		if (ImGui::TreeNode("Groups"))
		{
			for (auto& group : tSprites.mGroups)
			{
				imguiMugenSpriteFileGroup(group.second, group.first);
			}
			ImGui::TreePop();
		}

		imguiVector("AllSprites", tSprites.mAllSprites, imguiMugenSpriteFileSpriteSingle);
		imguiVector("Palettes", tSprites.mPalettes, imguiMugenSpriteFilePalette);

		ImGui::Text("PaletteMappedGroup = %d", tSprites.mPaletteMappedGroup);
		ImGui::Text("mPaletteMappedItem = %d", tSprites.mPaletteMappedItem);

		ImGui::TreePop();
	}
}
#endif
}