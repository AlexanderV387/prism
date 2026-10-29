#pragma once

// [3DS] Load-time measurements (the load phase of the port): each screen's
// load and each sprite file, split into SD card reads, texture making, .def
// parsing and sounds, with the frame times of the screen shown after. Written to
// sdmc:/3ds/dolmexica/load-log.txt, between loads.

#include <stdint.h>

namespace prism {

	typedef struct {
		uint64_t mOpenTicks, mReadTicks, mSeekTicks;
		uint32_t mOpens, mReads, mSeeks;
		uint64_t mReadBytes;

		uint64_t mStatTicks;   // isFile's look on the SD card
		uint32_t mStats;

		uint64_t mTextureTicks;
		uint32_t mTextures;
		uint64_t mTexels;

		uint64_t mDefTicks;
		uint32_t mDefs;

		uint64_t mSoundTicks;
		uint32_t mSounds;

		// Paletted textures without room when loaded (made when drawn), made
		// when drawn, and let go to make room (texture_3ds.cpp).
		uint32_t mDeferredTextures;
		uint32_t mLateTextures;
		uint64_t mLateTextureTicks;
		uint32_t mEvictedTextures;
		// Paletted textures made again for a palette that changed.
		uint32_t mRepaletted;
		uint64_t mRepalettedTicks;
	} N3DSPerfCounters;

	extern N3DSPerfCounters gN3DSPerf;

	// The SD card's FILE buffer for files opened now (0: newlib's own).
	extern int gN3DSFileBufferSize;

	uint64_t getN3DSPerfTicks();
	double getN3DSPerfMilliseconds(uint64_t tTicks);

	void initN3DSPerf();
	void addN3DSPerfLine(const char* tFormat, ...);

	// Ways of reading a sprite file: 0 small reads through newlib's buffer,
	// 1 the same with a 64 KB buffer, 2 the whole file at once into memory.
	// 3 the portraits alone, where the portrait cache says they are. 4 a
	// megabyte at a time.
	enum { N3DS_SPRITE_READ_PLAIN, N3DS_SPRITE_READ_BIG_BUFFER, N3DS_SPRITE_READ_WHOLE, N3DS_SPRITE_READ_PORTRAITS, N3DS_SPRITE_READ_WINDOWED, N3DS_SPRITE_READ_AMOUNT };

	// A frame shown (drawing_3ds.cpp): its time counts for the screen's
	// frame statistics, with its parts: waiting in C3D_FrameBegin (for the
	// GPU and the screen), making the drawing on the CPU, the sprites and
	// the batches they went in.
	void addN3DSPerfFrame(uint64_t tWaitTicks, uint64_t tDrawTicks, int tSprites, int tBatches);
	// A game frame whose picture was left out (drawing_3ds.cpp, to keep up).
	void addN3DSPerfSkippedFrame();
	// Frames shown since the screen was (0 while loading).
	uint32_t getN3DSPerfFramesShown();

	// The compatibility log (sdmc:/3ds/dolmexica/compat-log.txt): per
	// section (a character, a fight), the game's lines and the warnings
	// logged meanwhile, each once with how often. Written when a section
	// begins for the first time (a crash leaves what was loading), when a new
	// warning comes outside a load and after each load (each costs some
	// 25 ms on the SD card).
	void beginN3DSCompatSection(const char* tName);
	void addN3DSCompatLine(const char* tFormat, ...);
	void endN3DSCompatSection();

	// Memory, for the logs: the heap in use, the textures' (linear) memory free.
	int getN3DSHeapUsedKB();
	int getN3DSLinearFreeKB();

}
