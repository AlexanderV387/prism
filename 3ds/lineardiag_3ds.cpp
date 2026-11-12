// [3DS] Diagnosis of the linear memory (built with N3DS_LINEAR_DIAG, linked
// with --wrap=linearAlloc --wrap=linearMemAlign --wrap=linearFree): every
// block taken from it (textures, sounds, the screens, the GPU's buffers) gets
// 128 bytes of a known pattern before and after it, checked every few frames
// and when the block is let go. The first one found written over stops the
// game with the block, who asked for it and its neighbours in the error log:
// with the linear memory run out (Chun Li HD against herself), both screens,
// the sprites and the sounds went wrong, something writing where it should
// not. Each failed allocation is written to the load log too.

#ifdef N3DS_LINEAR_DIAG

#include <stdio.h>
#include <string.h>
#include <map>

#include <3ds.h>

#include "prism/log.h"
#include "prism/system.h"

#include "prism3dsperf.h"

extern "C" {
	void* __real_linearMemAlign(size_t tSize, size_t tAlignment);
	void __real_linearFree(void* tMemory);
}

namespace prism {

	static const size_t GUARD = 128;
	static const uint8_t PATTERN = 0xA5;

	typedef struct {
		uint8_t* mBase;      // what the allocator gave
		size_t mSize;        // what was asked for
		uint32_t mCaller;    // who asked (an address in the .elf)
		uint32_t mNumber;    // the how manyth
	} Block;

	static std::map<uintptr_t, Block> gBlocks;   // by the address given out
	static LightLock gLock;
	static int gIsLockReady;
	static uint32_t gNumber;
	static uint32_t gFailures;
	static int gIsStopping;

	static void lock() {
		if (!gIsLockReady) {
			LightLock_Init(&gLock);
			gIsLockReady = 1;
		}
		LightLock_Lock(&gLock);
	}
	static void unlock() { LightLock_Unlock(&gLock); }

	static int isGuardIntact(const uint8_t* tGuard, size_t* oFirstBad) {
		for (size_t i = 0; i < GUARD; i++) {
			if (tGuard[i] != PATTERN) {
				*oFirstBad = i;
				return 0;
			}
		}
		return 1;
	}

	static void describeBlock(char* oText, size_t tLength, const char* tName, const Block& tBlock) {
		snprintf(oText, tLength, "%s: %p, %u bytes, asked for by 0x%08lX (allocation #%lu)", tName, tBlock.mBase + GUARD, (unsigned)tBlock.mSize, (unsigned long)tBlock.mCaller, (unsigned long)tBlock.mNumber);
	}

	// A guard found written over: the block, its neighbours, then the error screen.
	static void stopOnDamage(std::map<uintptr_t, Block>::iterator tIt, int tIsAfter, size_t tFirstBad, const char* tWhen) {
		gIsStopping = 1;
		const Block& block = tIt->second;
		const uint8_t* guard = tIsAfter ? block.mBase + GUARD + block.mSize : block.mBase;
		char text[300];
		logErrorFormat("Linear memory written over (%s): the %s guard of a block, from its byte %u.", tWhen, tIsAfter ? "end" : "start", (unsigned)tFirstBad);
		describeBlock(text, sizeof(text), "Block", block);
		logError(text);
		snprintf(text, sizeof(text), "Guard bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			guard[0], guard[1], guard[2], guard[3], guard[4], guard[5], guard[6], guard[7], guard[8], guard[9], guard[10], guard[11], guard[12], guard[13], guard[14], guard[15]);
		logError(text);
		if (tIt != gBlocks.begin()) {
			auto previous = tIt;
			--previous;
			describeBlock(text, sizeof(text), "Before it", previous->second);
			logError(text);
		}
		auto next = tIt;
		++next;
		if (next != gBlocks.end()) {
			describeBlock(text, sizeof(text), "After it", next->second);
			logError(text);
		}
		logErrorFormat("Blocks: %u, failed allocations so far: %u, linear memory free: %u KB.", (unsigned)gBlocks.size(), (unsigned)gFailures, (unsigned)(linearSpaceFree() / 1024));
		unlock();
		abortSystem();
	}

	static void* allocate(size_t tSize, size_t tAlignment, uint32_t tCaller) {
		if (tAlignment < 0x80) tAlignment = 0x80;
		uint8_t* base = (uint8_t*)__real_linearMemAlign(tSize + 2 * GUARD, tAlignment);
		lock();
		gNumber++;
		if (!base) {
			gFailures++;
			const uint32_t failures = gFailures;
			unlock();
			if (failures <= 40) addN3DSPerfLine("  linear memory: %u bytes asked for by 0x%08lX not there (%u KB free)", (unsigned)tSize, (unsigned long)tCaller, (unsigned)(linearSpaceFree() / 1024));
			return NULL;
		}
		memset(base, PATTERN, GUARD);
		memset(base + GUARD + tSize, PATTERN, GUARD);
		// The guards out of the cache: the GPU and the DSP read and write the
		// memory itself.
		GSPGPU_FlushDataCache(base, GUARD);
		GSPGPU_FlushDataCache(base + GUARD + tSize, GUARD);
		Block block = { base, tSize, tCaller, gNumber };
		gBlocks[uintptr_t(base + GUARD)] = block;
		unlock();
		return base + GUARD;
	}

	// Every block's guards, the cache flushed first (written by the GPU or
	// the DSP, the memory can differ from what the CPU has cached).
	void checkN3DSLinearGuards(const char* tWhen) {
		if (gIsStopping) return;
		lock();
		if (!gBlocks.empty()) {
			const Block& first = gBlocks.begin()->second;
			const Block& last = gBlocks.rbegin()->second;
			const uintptr_t start = uintptr_t(first.mBase), end = uintptr_t(last.mBase) + last.mSize + 2 * GUARD;
			svcFlushProcessDataCache(CUR_PROCESS_HANDLE, start, end - start);
		}
		for (auto it = gBlocks.begin(); it != gBlocks.end(); ++it) {
			const Block& block = it->second;
			size_t firstBad = 0;
			if (!isGuardIntact(block.mBase, &firstBad)) stopOnDamage(it, 0, firstBad, tWhen);
			if (!isGuardIntact(block.mBase + GUARD + block.mSize, &firstBad)) stopOnDamage(it, 1, firstBad, tWhen);
		}
		unlock();
	}

	uint32_t getN3DSLinearFailures() { return gFailures; }
}

using namespace prism;

extern "C" {

	void* __wrap_linearAlloc(size_t tSize) {
		return allocate(tSize, 0x80, uint32_t(uintptr_t(__builtin_return_address(0))));
	}

	void* __wrap_linearMemAlign(size_t tSize, size_t tAlignment) {
		return allocate(tSize, tAlignment, uint32_t(uintptr_t(__builtin_return_address(0))));
	}

	void __wrap_linearFree(void* tMemory) {
		if (!tMemory) return;
		lock();
		auto it = gBlocks.find(uintptr_t(tMemory));
		if (it == gBlocks.end()) {
			unlock();
			__real_linearFree(tMemory);
			return;
		}
		size_t firstBad = 0;
		if (!gIsStopping) {
			GSPGPU_FlushDataCache(it->second.mBase, GUARD);
			GSPGPU_FlushDataCache(it->second.mBase + GUARD + it->second.mSize, GUARD);
			if (!isGuardIntact(it->second.mBase, &firstBad)) stopOnDamage(it, 0, firstBad, "as it was let go");
			if (!isGuardIntact(it->second.mBase + GUARD + it->second.mSize, &firstBad)) stopOnDamage(it, 1, firstBad, "as it was let go");
		}
		uint8_t* base = it->second.mBase;
		gBlocks.erase(it);
		unlock();
		__real_linearFree(base);
	}

}

#endif
