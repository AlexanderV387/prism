#pragma once

#include <string>
#include <map>

#include "datastructures.h"
#include "texture.h"

namespace prism {

typedef struct {
	Vector2DI mOffset;
	TextureData mTexture;
	// Sprite pixels per texel: above 1 for a sprite made smaller as it was
	// read (setMugenSpriteFileReaderDownscale); its offset is the sprite's.
	int mScale = 1;
} MugenSpriteFileSubSprite;

typedef struct {
	std::vector<MugenSpriteFileSubSprite> mTextures;
	TextureSize mOriginalTextureSize;
	int mIsLinked;
	int mIsLinkedTo;

	Vector2D mAxisOffset;
} MugenSpriteFileSprite;

typedef struct {
	std::map<int, MugenSpriteFileSprite> mSprites;
} MugenSpriteFileGroup;

typedef struct {
	int mGroup;
	int mItem;
	Buffer mBuffer;
} MugenSpriteFilePalette;

typedef struct {
	std::map<int, MugenSpriteFileGroup> mGroups;
	std::vector<MugenSpriteFileSprite*> mAllSprites;
	std::vector<MugenSpriteFilePalette> mPalettes;
	
	// group and item palette 1,1 is mapped to
	int mPaletteMappedGroup;
	int mPaletteMappedItem;
} MugenSpriteFile;

MugenSpriteFile loadMugenSpriteFile(const char * tPath, int tHasPaletteFile, const char* tOptionalPaletteFile);
MugenSpriteFile loadMugenSpriteFilePortraits(const char * tPath, int tHasPaletteFile, const char* tOptionalPaletteFile);
// Only sprite tGroup,tItem of a file (a character's standing one for a
// menu); a high resolution true color one read smaller to about
// tMaximumHeight (0: as it is).
MugenSpriteFile loadMugenSpriteFileSingleSprite(const char* tPath, int tHasPaletteFile, const char* tOptionalPaletteFile, int tGroup, int tItem, int tMaximumHeight);

void unloadMugenSpriteFile(MugenSpriteFile* tFile);
void unloadMugenSpriteFileSprite(MugenSpriteFileSprite* tSprite);
MugenSpriteFile loadMugenSpriteFileWithoutPalette(const std::string& tPath);
MugenSpriteFile loadMugenSpriteFileWithoutPalette(const char* tPath);
MugenSpriteFileSprite* getMugenSpriteFileTextureReference(MugenSpriteFile* tFile, int tGroup, int tSprite);

MugenSpriteFileSprite loadSingleTextureFromPCXBuffer(const Buffer& tBuffer);

void setMugenSpriteFileReaderToBuffer();
void setMugenSpriteFileReaderToFileOperations();
void setMugenSpriteFileReaderToUsePalette(int tPaletteID);
void setMugenSpriteFileReaderToNotUsePalette();
// [3DS] The sprites read from now on made this many times smaller (1: as
// they are): a 1280x720 stage's are drawn at a third of their size.
void setMugenSpriteFileReaderDownscale(int tFactor);
int getMugenSpriteFileReaderDownscale();
// [3DS] The texture memory a sprite file's true color sprites (SFF v2's
// PNG24 and PNG32: kept as they are, never let go) would take read
// tFactor times smaller, from its sprite list alone; -1 when it cannot
// tell (an SFF v1, a file that cannot be read).
long long estimateMugenSpriteFileTrueColorTextureBytes(const char* tPath, int tFactor);
void setMugenSpriteFileReaderCustomFunctionsAndForceARGB16(TextureData(*mCustomLoadTextureFromARGB16Buffer)(const Buffer&, int, int), TextureData(*mCustomLoadTextureFromARGB32Buffer)(const Buffer&, int, int), TextureData(*mCustomLoadPalettedTextureFrom8BitBuffer)(const Buffer&, int, int, int));
void setMugenSpriteFileReaderSubTextureSplit(int tSubTextureSplitMin, int tSubTextureSplitMax);
#if defined(__3DS__) || defined(N3DS_TEXTURE_SIM)
// [3DS] The heap kept free while loading sprites (sprites that would take it
// are left out).
void setMugenSpriteFileReaderHeapReserve(size_t tReserve);
#endif

int hasMugenSprite(MugenSpriteFile* tSprites, int tGroup, int tSprite);
void remapMugenSpriteFilePalette(MugenSpriteFile* tSprites, const Vector2DI& tSource, const Vector2DI& tDestination, int tPaletteID);

void imguiMugenSpriteFile(MugenSpriteFile& tSprites, const std::string_view& tName);

}