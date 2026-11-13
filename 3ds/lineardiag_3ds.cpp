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

	void checkN3DSHeapGuards(const char* tWhen);

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
		// The allocator's own count: a block given out over one still in use
		// (its free list written over) stops the game at once.
		{
			const uintptr_t start = uintptr_t(base), end = uintptr_t(base) + tSize + 2 * GUARD;
			auto it = gBlocks.upper_bound(start);
			if (it != gBlocks.begin()) {
				auto before = it;
				--before;
				if (uintptr_t(before->second.mBase) + before->second.mSize + 2 * GUARD > start) it = before;
			}
			if (it != gBlocks.end() && uintptr_t(it->second.mBase) < end) {
				gIsStopping = 1;
				char text[300];
				logErrorFormat("Linear memory given out twice: %p, %u bytes, asked for by 0x%08lX (allocation #%lu), over a block still in use.", base + GUARD, (unsigned)tSize, (unsigned long)tCaller, (unsigned long)gNumber);
				describeBlock(text, sizeof(text), "In use", it->second);
				logError(text);
				logErrorFormat("Blocks: %u, failed allocations so far: %u, linear memory free: %u KB.", (unsigned)gBlocks.size(), (unsigned)gFailures, (unsigned)(linearSpaceFree() / 1024));
				checkN3DSHeapGuards("the linear memory given out twice");
				unlock();
				abortSystem();
			}
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
		// At most twice a second (a load's steps come by the hundred).
		static u64 lastCheck = 0;
		const u64 now = osGetTime();
		if (now - lastCheck < 500) return;
		lastCheck = now;
		checkN3DSHeapGuards(tWhen);
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

// ============================================================================
// The heap, the same way (linked with --wrap=malloc --wrap=free --wrap=calloc
// --wrap=realloc --wrap=memalign): libctru keeps its list of the linear
// memory's free pieces in 16-byte blocks of the heap, and a block of linear
// memory in use (the top screen's) was given out again: something wrote
// over the heap. Each block gets 16 bytes of a pattern before and after it,
// checked with the linear memory's and when it is let go; the block written
// over and who asked for it (C++'s through operator new: its caller) in the
// error log. Its table is outside the heap.

#include <reent.h>

// newlib's own allocator, under both its names (malloc and _malloc_r, the
// one its own functions call: a stdio buffer made by setvbuf through malloc
// was let go by fclose through _free_r, which read the guard as its header
// and the game stopped at its start).
extern "C" {
	void* __real__malloc_r(struct _reent* tReent, size_t tSize);
	void __real__free_r(struct _reent* tReent, void* tMemory);
	void* __real__memalign_r(struct _reent* tReent, size_t tAlignment, size_t tSize);
	void* __real__realloc_r(struct _reent* tReent, void* tMemory, size_t tSize);
}
static void* realMalloc(size_t tSize) { return __real__malloc_r(_REENT, tSize); }
static void realFree(void* tMemory) { __real__free_r(_REENT, tMemory); }
static void* realMemalign(size_t tAlignment, size_t tSize) { return __real__memalign_r(_REENT, tAlignment, tSize); }
static void* realRealloc(void* tMemory, size_t tSize) { return __real__realloc_r(_REENT, tMemory, tSize); }

namespace prism {

	static const size_t HEAP_GUARD = 16;
	static const uint8_t HEAP_PATTERN = 0x5A;
	static const uint32_t HEAP_SLOTS = 1 << 17;   // 131072 blocks at most followed

	typedef struct {
		uintptr_t mAddress;   // given out (0: empty)
		uint32_t mSize;
		uint32_t mCaller;
		uint16_t mFront;      // its front guard's size (more for an alignment)
	} HeapSlot;

	static HeapSlot gHeapSlots[HEAP_SLOTS];
	static uint32_t gHeapBlocks, gHeapUntracked;
	static RecursiveLock gHeapLock;
	static int gIsHeapLockReady;
	static int gIsHeapStopping;
	static __thread uint32_t gNewCaller;   // set by operator new (system_3ds.cpp)

	void setN3DSHeapCaller(uint32_t tCaller) { gNewCaller = tCaller; }

	static void heapLock() {
		if (!gIsHeapLockReady) {
			RecursiveLock_Init(&gHeapLock);
			gIsHeapLockReady = 1;
		}
		RecursiveLock_Lock(&gHeapLock);
	}
	static void heapUnlock() { RecursiveLock_Unlock(&gHeapLock); }

	static uint32_t slotOf(uintptr_t tAddress) { return uint32_t((tAddress >> 3) * 2654435761u) & (HEAP_SLOTS - 1); }

	static HeapSlot* findSlot(uintptr_t tAddress) {
		for (uint32_t i = slotOf(tAddress), n = 0; n < HEAP_SLOTS; i = (i + 1) & (HEAP_SLOTS - 1), n++) {
			if (gHeapSlots[i].mAddress == tAddress) return &gHeapSlots[i];
			if (!gHeapSlots[i].mAddress) return NULL;
		}
		return NULL;
	}

	// A slot emptied, the ones after it moved back where their search would
	// stop short (marked as removed instead, the slots filled with the marks
	// and each search went through the whole table: the game seemed frozen at
	// its start).
	static void removeSlot(HeapSlot* tSlot) {
		const uint32_t mask = HEAP_SLOTS - 1;
		uint32_t hole = uint32_t(tSlot - gHeapSlots);
		gHeapSlots[hole].mAddress = 0;
		for (uint32_t j = (hole + 1) & mask; gHeapSlots[j].mAddress; j = (j + 1) & mask) {
			const uint32_t home = slotOf(gHeapSlots[j].mAddress);
			// Moved back when its home is not in (hole, j], going around.
			const int isBetween = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
			if (isBetween) continue;
			gHeapSlots[hole] = gHeapSlots[j];
			gHeapSlots[j].mAddress = 0;
			hole = j;
		}
	}

	static int addSlot(uintptr_t tAddress, uint32_t tSize, uint32_t tCaller, uint16_t tFront) {
		if (gHeapBlocks >= HEAP_SLOTS * 3 / 4) return 0;
		for (uint32_t i = slotOf(tAddress), n = 0; n < HEAP_SLOTS; i = (i + 1) & (HEAP_SLOTS - 1), n++) {
			if (!gHeapSlots[i].mAddress) {
				gHeapSlots[i] = { tAddress, tSize, tCaller, tFront };
				gHeapBlocks++;
				return 1;
			}
		}
		return 0;
	}

	static int isHeapGuardIntact(const uint8_t* tGuard, size_t tLength, size_t* oFirstBad) {
		for (size_t i = 0; i < tLength; i++) {
			if (tGuard[i] != HEAP_PATTERN) {
				*oFirstBad = i;
				return 0;
			}
		}
		return 1;
	}

	static void stopOnHeapDamage(const HeapSlot& tSlot, int tIsAfter, size_t tFirstBad, const char* tWhen) {
		gIsHeapStopping = 1;
		gIsStopping = 1;
		const uint8_t* guard = tIsAfter ? (const uint8_t*)tSlot.mAddress + tSlot.mSize : (const uint8_t*)tSlot.mAddress - tSlot.mFront;
		logErrorFormat("Heap written over (%s): the %s guard of a block, from its byte %u.", tWhen, tIsAfter ? "end" : "start", (unsigned)tFirstBad);
		logErrorFormat("Heap block: %p, %u bytes, asked for by 0x%08lX.", (void*)tSlot.mAddress, (unsigned)tSlot.mSize, (unsigned long)tSlot.mCaller);
		logErrorFormat("Guard bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			guard[0], guard[1], guard[2], guard[3], guard[4], guard[5], guard[6], guard[7], guard[8], guard[9], guard[10], guard[11], guard[12], guard[13], guard[14], guard[15]);
		// The block just before it (a start guard written over: that one went past its end).
		const HeapSlot* before = NULL;
		for (uint32_t i = 0; i < HEAP_SLOTS; i++) {
			const HeapSlot& other = gHeapSlots[i];
			if (!other.mAddress || other.mAddress >= tSlot.mAddress) continue;
			if (!before || other.mAddress > before->mAddress) before = &other;
		}
		if (before) logErrorFormat("Heap block before it: %p, %u bytes, asked for by 0x%08lX.", (void*)before->mAddress, (unsigned)before->mSize, (unsigned long)before->mCaller);
		logErrorFormat("Heap blocks followed: %u (%u not followed).", (unsigned)gHeapBlocks, (unsigned)gHeapUntracked);
	}

	static int checkHeapSlot(const HeapSlot& tSlot, const char* tWhen) {
		size_t firstBad = 0;
		if (!isHeapGuardIntact((const uint8_t*)tSlot.mAddress - HEAP_GUARD, HEAP_GUARD, &firstBad)) {
			stopOnHeapDamage(tSlot, 0, firstBad, tWhen);
			return 0;
		}
		if (!isHeapGuardIntact((const uint8_t*)tSlot.mAddress + tSlot.mSize, HEAP_GUARD, &firstBad)) {
			stopOnHeapDamage(tSlot, 1, firstBad, tWhen);
			return 0;
		}
		return 1;
	}

	void checkN3DSHeapGuards(const char* tWhen) {
		if (gIsHeapStopping) return;
		heapLock();
		for (uint32_t i = 0; i < HEAP_SLOTS; i++) {
			if (!gHeapSlots[i].mAddress) continue;
			if (!checkHeapSlot(gHeapSlots[i], tWhen)) {
				heapUnlock();
				abortSystem();
			}
		}
		heapUnlock();
	}

	static void* heapAllocate(size_t tSize, size_t tAlignment, uint32_t tCaller) {
		const size_t front = tAlignment > HEAP_GUARD ? tAlignment : HEAP_GUARD;
		uint8_t* base = (uint8_t*)(tAlignment > 8 ? realMemalign(tAlignment, tSize + front + HEAP_GUARD) : realMalloc(tSize + front + HEAP_GUARD));
		if (!base) return NULL;
		uint8_t* p = base + front;
		memset(p - HEAP_GUARD, HEAP_PATTERN, HEAP_GUARD);
		memset(p + tSize, HEAP_PATTERN, HEAP_GUARD);
		heapLock();
		if (!addSlot(uintptr_t(p), uint32_t(tSize), tCaller, uint16_t(front))) {
			gHeapUntracked++;
			heapUnlock();
			// Not followed: given out without guards (its own malloc).
			realFree(base);
			return tAlignment > 8 ? realMemalign(tAlignment, tSize) : realMalloc(tSize);
		}
		heapUnlock();
		return p;
	}

	static int heapFree(void* tMemory) {
		heapLock();
		HeapSlot* slot = findSlot(uintptr_t(tMemory));
		if (!slot) {
			heapUnlock();
			return 0;
		}
		if (!gIsHeapStopping && !checkHeapSlot(*slot, "as it was let go")) {
			heapUnlock();
			abortSystem();
		}
		const uint16_t front = slot->mFront;
		removeSlot(slot);
		gHeapBlocks--;
		heapUnlock();
		realFree((uint8_t*)tMemory - front);
		return 1;
	}

	static size_t heapSize(void* tMemory) {
		heapLock();
		HeapSlot* slot = findSlot(uintptr_t(tMemory));
		const size_t size = slot ? slot->mSize : (size_t)-1;
		heapUnlock();
		return size;
	}
}

extern "C" {

	static uint32_t takeCaller(void* tReturnAddress) {
		const uint32_t caller = prism::gNewCaller ? prism::gNewCaller : uint32_t(uintptr_t(tReturnAddress));
		prism::gNewCaller = 0;
		return caller;
	}

	static void freeAny(void* tMemory) {
		if (!tMemory) return;
		if (!prism::heapFree(tMemory)) realFree(tMemory);
	}

	static void* reallocAny(void* tMemory, size_t tSize, uint32_t tCaller) {
		if (!tMemory) return prism::heapAllocate(tSize, 8, tCaller);
		const size_t oldSize = prism::heapSize(tMemory);
		if (oldSize == (size_t)-1) return realRealloc(tMemory, tSize);   // not followed
		if (!tSize) {
			freeAny(tMemory);
			return NULL;
		}
		void* p = prism::heapAllocate(tSize, 8, tCaller);
		if (!p) return NULL;
		memcpy(p, tMemory, oldSize < tSize ? oldSize : tSize);
		freeAny(tMemory);
		return p;
	}

	static void* callocAny(size_t tAmount, size_t tSize, uint32_t tCaller) {
		const size_t size = tAmount * tSize;
		void* p = prism::heapAllocate(size, 8, tCaller);
		if (p) memset(p, 0, size);
		return p;
	}

	void* __wrap_malloc(size_t tSize) { return prism::heapAllocate(tSize, 8, takeCaller(__builtin_return_address(0))); }
	void* __wrap__malloc_r(struct _reent*, size_t tSize) { return prism::heapAllocate(tSize, 8, takeCaller(__builtin_return_address(0))); }
	void* __wrap_memalign(size_t tAlignment, size_t tSize) { return prism::heapAllocate(tSize, tAlignment, takeCaller(__builtin_return_address(0))); }
	void* __wrap__memalign_r(struct _reent*, size_t tAlignment, size_t tSize) { return prism::heapAllocate(tSize, tAlignment, takeCaller(__builtin_return_address(0))); }
	void* __wrap_calloc(size_t tAmount, size_t tSize) { return callocAny(tAmount, tSize, takeCaller(__builtin_return_address(0))); }
	void* __wrap__calloc_r(struct _reent*, size_t tAmount, size_t tSize) { return callocAny(tAmount, tSize, takeCaller(__builtin_return_address(0))); }
	void __wrap_free(void* tMemory) { freeAny(tMemory); }
	void __wrap__free_r(struct _reent*, void* tMemory) { freeAny(tMemory); }
	void* __wrap_realloc(void* tMemory, size_t tSize) { return reallocAny(tMemory, tSize, takeCaller(__builtin_return_address(0))); }
	void* __wrap__realloc_r(struct _reent*, void* tMemory, size_t tSize) { return reallocAny(tMemory, tSize, takeCaller(__builtin_return_address(0))); }

}
#endif
