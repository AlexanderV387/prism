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
	// 3 the portraits alone, where the portrait cache says they are.
	enum { N3DS_SPRITE_READ_PLAIN, N3DS_SPRITE_READ_BIG_BUFFER, N3DS_SPRITE_READ_WHOLE, N3DS_SPRITE_READ_PORTRAITS, N3DS_SPRITE_READ_AMOUNT };

	// A frame shown (drawing_3ds.cpp): its time counts for the screen's
	// frame statistics.
	void addN3DSPerfFrame();

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
