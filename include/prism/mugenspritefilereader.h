#pragma once

#include <string>
#include <map>
#include <set>

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

	// Where its pixels are not transparent (x0, y0 to x1, y1, past the
	// last), found only when asked (setMugenSpriteFileReaderFindingVisible);
	// x1 < 0 not found (the whole sprite).
	int mVisibleX0 = 0, mVisibleY0 = 0, mVisibleX1 = -1, mVisibleY1 = -1;
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
// Each sprite's size (group, item: width, height), from the file's sprite
// list (SFF v1: each one's PCX header), no pixels read.
std::map<std::pair<int, int>, Vector2DI> getMugenSpriteFileSpriteSizes(const char* tPath);
// Only these sprites of a file (a stage's for a menu's picture of it),
// read at the reader's factor (setMugenSpriteFileReaderDownscale), kept
// in the SD card's cache as the whole files' are.
MugenSpriteFile loadMugenSpriteFileSomeSprites(const char* tPath, const std::set<std::pair<int, int>>& tSprites);
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
// An SFF v2's sprites using palette 1,1 made in palette 1,tItem instead
// (MUGEN 1.1's palno with no palette file), when not on a palette slot; 0 or
// 1 as they are.
void setMugenSpriteFileReaderPaletteItem(int tItem);
// Each sprite's visible part found as it is read (its mVisibleX0...): a
// character's picture on the select screen, its canvas mostly empty.
void setMugenSpriteFileReaderFindingVisible(int tIsFinding);
void setMugenSpriteFileReaderDownscale(int tFactor);
// [3DS] With a factor above: the sprites no longer than tSide on either side
// read whole (0: none): a high resolution character's hit sparks, drawn at
// their own size, not at its.
void setMugenSpriteFileReaderKeepingSmallSpritesWhole(int tSide);
int getMugenSpriteFileReaderDownscale();
// [3DS] The texture memory a sprite file's true color sprites (SFF v2's
// PNG24 and PNG32: kept as they are, never let go) would take read
// tFactor times smaller (those no longer than tWholeSide whole), from its
// sprite list alone; -1 when it cannot tell (an SFF v1, a file that cannot
// be read).
long long estimateMugenSpriteFileTrueColorTextureBytes(const char* tPath, int tFactor, int tWholeSide = 0);
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