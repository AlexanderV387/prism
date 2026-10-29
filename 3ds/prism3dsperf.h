#pragma once

// [3DS] Load-time measurements (the load phase of the port): each screen's
// load and each sprite file, split into SD card reads, texture making, .def
// parsing and sounds, with the frame times of the screen shown after; and
// which way sprite files are read (rotating, to compare). Written to
// sdmc:/3ds/dolmexica/load-log.txt, between loads.

#include <stdint.h>

namespace prism {

	typedef struct {
		uint64_t mOpenTicks, mReadTicks, mSeekTicks;
		uint32_t mOpens, mReads, mSeeks;
		uint64_t mReadBytes;

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
	enum { N3DS_SPRITE_READ_PLAIN, N3DS_SPRITE_READ_BIG_BUFFER, N3DS_SPRITE_READ_WHOLE, N3DS_SPRITE_READ_AMOUNT };
	int getNextN3DSSpriteReadVariant();

	// A frame shown (drawing_3ds.cpp): its time counts for the screen's
	// frame statistics.
	void addN3DSPerfFrame();

}
