#pragma once

#include <stdint.h>

// [3DS] Where the game logic's time goes, for the load log's "at N s" lines
// (3ds/perf_3ds.cpp): named parts (a string literal each, kept as given), a
// frame's time in them summed over the 10 seconds. Nothing elsewhere.

namespace prism {

#ifdef __3DS__
	uint64_t getN3DSLogicPartTicks();
	void addN3DSLogicPartTicks(const char* tName, uint64_t tTicks);
	void addN3DSLogicPartTicksForAddress(const void* tAddress, uint64_t tTicks);
#endif

}
