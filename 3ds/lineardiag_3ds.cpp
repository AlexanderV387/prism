// [3DS] Diagnosis of the linear memory (built with N3DS_LINEAR_DIAG, linked
// with --wrap=linearAlloc --wrap=linearMemAlign --wrap=linearFree
// --wrap=malloc --wrap=free).
//
// With the linear memory run out (Chun Li HD against herself) both screens,
// the sprites and the sounds went wrong: prueba 97 found the top screen's
// framebuffer given out again to two textures. libctru keeps the linear
// memory's free pieces and its blocks in use in small heap blocks (16 and 20
// bytes: MemBlock, addrMapNode), so something wrote over them. Here:
//
// - those small blocks come from pages of their own, read-only but while
//   libctru's allocator runs: whatever writes there stops the game with a
//   CPU exception, its pc in the error log;
// - libctru's free count is checked after each allocation and each block
//   let go (it changes by the block's size, exactly) and against the blocks
//   in use, and each block's size as libctru keeps it against the one it
//   gave: the operation where the count went wrong, and the last ones before
//   it, in the error log;
// - the linear memory used from any thread but the game's own is logged;
// - every block gets 128 bytes of a known pattern before and after it,
//   checked every few frames and when it is let go.
//
// (Wrapping the whole heap instead, pruebas 98 to 100, kept the game from
// starting.)

#ifdef N3DS_LINEAR_DIAG

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <map>

#include <3ds.h>

#include "prism/log.h"
#include "prism/system.h"

#include "prism3dsperf.h"

extern "C" {
	void* __real_linearMemAlign(size_t tSize, size_t tAlignment);
	void __real_linearFree(void* tMemory);
	void* __real_malloc(size_t tSize);
	void __real_free(void* tMemory);
	extern u32 __ctru_linear_heap_size;
}

namespace prism {

	// ------------------------------------------------------------------------
	// The pages for libctru's own small blocks.

	static const size_t NODE_SIZE = 32;
	static const uint32_t NODE_AMOUNT = 16384;
	static const size_t POOL_BYTES = NODE_SIZE * NODE_AMOUNT;   // 512 KB
	static const size_t PAGE = 0x1000;

	static uint8_t* gPool;                  // page-aligned, POOL_BYTES
	static uint16_t gPoolFree[NODE_AMOUNT];   // the free ones (outside the pages)
	static uint32_t gPoolFreeAmount;
	static uint32_t gPoolMisses;            // asked for with all taken: from the heap
	static int gIsPoolProtected;            // the pages can be made read-only
	static volatile u32 gOpThread;          // the thread inside libctru's allocator (its TLS)

	static u32 currentThread() { return u32(uintptr_t(getThreadLocalStorage())); }

	static int isInPool(const void* tMemory) {
		return gPool && (const uint8_t*)tMemory >= gPool && (const uint8_t*)tMemory < gPool + POOL_BYTES;
	}

	static void setPoolWritable(int tIsWritable) {
		if (!gIsPoolProtected) return;
		u32 out = 0;
		svcControlMemory(&out, u32(uintptr_t(gPool)), 0, POOL_BYTES, MEMOP_PROT, tIsWritable ? MEMPERM_READWRITE : MEMPERM_READ);
	}

	static Result gPoolResult;
	static int gIsPoolReported;

	static void initPool() {
		static int isTried = 0;
		if (isTried) return;
		isTried = 1;
		// A page spare on both sides: newlib's own headers next to it stay writable.
		uint8_t* raw = (uint8_t*)memalign(PAGE, POOL_BYTES + 2 * PAGE);
		if (!raw) return;
		gPool = raw + PAGE;
		for (uint32_t i = 0; i < NODE_AMOUNT; i++) gPoolFree[i] = uint16_t(NODE_AMOUNT - 1 - i);
		gPoolFreeAmount = NODE_AMOUNT;
		u32 out = 0;
		gPoolResult = svcControlMemory(&out, u32(uintptr_t(gPool)), 0, POOL_BYTES, MEMOP_PROT, MEMPERM_READ);
		gIsPoolProtected = R_SUCCEEDED(gPoolResult);
		if (gIsPoolProtected) setPoolWritable(1);   // made read-only once the first operation is over
	}

	// Written to the load log from the game's thread, once it is there (the
	// first allocation comes before it, with the screens).
	static void reportPool() {
		if (gIsPoolReported) return;
		gIsPoolReported = 1;
		if (!gPool) addN3DSPerfLine("  linear diagnosis: no pages for libctru's blocks (heap short)");
		else if (gIsPoolProtected) addN3DSPerfLine("  linear diagnosis: libctru's blocks on pages of their own at %p, read-only but in its allocator", gPool);
		else addN3DSPerfLine("  linear diagnosis: libctru's blocks on pages of their own at %p, not made read-only (the console gave %08lX)", gPool, (unsigned long)gPoolResult);
	}

	static void beginAllocatorOperation() {
		initPool();
		setPoolWritable(1);
		gOpThread = currentThread();
	}

	static void endAllocatorOperation() {
		gOpThread = 0;
		setPoolWritable(0);
	}

	// ------------------------------------------------------------------------
	// The blocks given out, the guards around them, the operations.

	static const size_t GUARD = 128;
	static const uint8_t PATTERN = 0xA5;

	typedef struct {
		uint8_t* mBase;      // what the allocator gave
		size_t mSize;        // what was asked for
		size_t mRealSize;    // what libctru keeps for it (with the guards, rounded)
		uint32_t mCaller;    // who asked (an address in the .elf)
		uint32_t mNumber;    // the how manyth
	} Block;

	typedef struct {
		uint32_t mNumber;
		uint8_t mIsFree;
		uint32_t mAddress, mSize, mCaller;
		uint32_t mFreeBefore, mFreeAfter;
	} Operation;

	static const int OPERATION_AMOUNT = 24;
	static Operation gOperations[OPERATION_AMOUNT];
	static uint32_t gOperationAmount;

	static std::map<uintptr_t, Block> gBlocks;   // by the address given out
	static LightLock gLock;
	static int gIsLockReady;
	static uint32_t gNumber;
	static uint32_t gFailures;
	static size_t gRealInUse;       // the blocks' sizes as libctru keeps them
	static size_t gLost;            // let go and not given back (libctru short of a block for it)
	static int gIsStopping;
	static u32 gMainThread;
	static int gUnknownFreeReports;

	static void lock() {
		if (!gIsLockReady) {
			LightLock_Init(&gLock);
			gIsLockReady = 1;
		}
		LightLock_Lock(&gLock);
	}
	static void unlock() { LightLock_Unlock(&gLock); }

	static void recordOperation(int tIsFree, const void* tAddress, size_t tSize, uint32_t tCaller, size_t tFreeBefore, size_t tFreeAfter) {
		Operation& o = gOperations[gOperationAmount % OPERATION_AMOUNT];
		o.mNumber = gNumber;
		o.mIsFree = uint8_t(tIsFree);
		o.mAddress = uint32_t(uintptr_t(tAddress));
		o.mSize = uint32_t(tSize);
		o.mCaller = tCaller;
		o.mFreeBefore = uint32_t(tFreeBefore);
		o.mFreeAfter = uint32_t(tFreeAfter);
		gOperationAmount++;
	}

	static void logLastOperations() {
		const uint32_t amount = gOperationAmount < OPERATION_AMOUNT ? gOperationAmount : OPERATION_AMOUNT;
		logErrorFormat("The last %u operations (oldest first):", (unsigned)amount);
		for (uint32_t i = gOperationAmount - amount; i < gOperationAmount; i++) {
			const Operation& o = gOperations[i % OPERATION_AMOUNT];
			logErrorFormat("#%lu %s %08lX, %lu bytes, by %08lX; free %lu -> %lu", (unsigned long)o.mNumber, o.mIsFree ? "free" : "alloc", (unsigned long)o.mAddress, (unsigned long)o.mSize, (unsigned long)o.mCaller, (unsigned long)o.mFreeBefore, (unsigned long)o.mFreeAfter);
		}
	}

	static void logSummary() {
		logErrorFormat("Blocks: %u (%u KB as libctru keeps them), failed allocations: %u, free: %u KB, lost: %u KB, total: %u KB.",
			(unsigned)gBlocks.size(), (unsigned)(gRealInUse / 1024), (unsigned)gFailures, (unsigned)(linearSpaceFree() / 1024), (unsigned)(gLost / 1024), (unsigned)(__ctru_linear_heap_size / 1024));
		logErrorFormat("libctru's blocks on their own pages: %u in use, %u from the heap.", (unsigned)(NODE_AMOUNT - gPoolFreeAmount), (unsigned)gPoolMisses);
	}

	static void stop() {
		logLastOperations();
		logSummary();
		unlock();
		abortSystem();
	}

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
		logErrorFormat("Guard bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			guard[0], guard[1], guard[2], guard[3], guard[4], guard[5], guard[6], guard[7], guard[8], guard[9], guard[10], guard[11], guard[12], guard[13], guard[14], guard[15]);
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
		stop();
	}

	// Noted here, logged from the game's thread (the log is not for others).
	static const int OTHER_THREAD_AMOUNT = 8;
	static uint32_t gOtherThreadCallers[OTHER_THREAD_AMOUNT];
	static u32 gOtherThreads[OTHER_THREAD_AMOUNT];
	static uint32_t gOtherThreadUses;

	static void noteThread(uint32_t tCaller) {
		const u32 thread = currentThread();
		if (!gMainThread) gMainThread = thread;
		if (thread == gMainThread) return;
		if (gOtherThreadUses < OTHER_THREAD_AMOUNT) {
			gOtherThreadCallers[gOtherThreadUses] = tCaller;
			gOtherThreads[gOtherThreadUses] = thread;
		}
		gOtherThreadUses++;
	}

	static void reportOtherThreads() {
		static uint32_t reported = 0;
		for (; reported < gOtherThreadUses && reported < OTHER_THREAD_AMOUNT; reported++) {
			addN3DSPerfLine("  linear diagnosis: linear memory used from another thread (TLS %08lX), by 0x%08lX", (unsigned long)gOtherThreads[reported], (unsigned long)gOtherThreadCallers[reported]);
			logWarningFormat("Linear memory used from another thread, by 0x%08lX.", (unsigned long)gOtherThreadCallers[reported]);
		}
	}

	static void* allocate(size_t tSize, size_t tAlignment, uint32_t tCaller) {
		if (tAlignment < 0x80) tAlignment = 0x80;
		lock();
		noteThread(tCaller);
		gNumber++;
		const size_t freeBefore = linearSpaceFree();
		beginAllocatorOperation();
		uint8_t* base = (uint8_t*)__real_linearMemAlign(tSize + 2 * GUARD, tAlignment);
		endAllocatorOperation();
		const size_t realSize = base ? linearGetSize(base) : 0;
		const size_t freeAfter = linearSpaceFree();
		recordOperation(0, base ? base + GUARD : NULL, tSize, tCaller, freeBefore, freeAfter);
		if (!base) {
			gFailures++;
			const uint32_t failures = gFailures;
			if (freeAfter != freeBefore && !gIsStopping) {
				gIsStopping = 1;
				logErrorFormat("Linear memory: a failed allocation (#%lu, %u bytes, by 0x%08lX) changed the free count.", (unsigned long)gNumber, (unsigned)tSize, (unsigned long)tCaller);
				stop();
			}
			unlock();
			if (failures <= 40 && currentThread() == gMainThread) addN3DSPerfLine("  linear memory: %u bytes asked for by 0x%08lX not there (%u KB free)", (unsigned)tSize, (unsigned long)tCaller, (unsigned)(freeAfter / 1024));
			return NULL;
		}
		if (gIsStopping) {
			unlock();
			return base + GUARD;
		}
		// libctru's count: down by the block's size, exactly.
		if (realSize < tSize + 2 * GUARD || freeBefore - freeAfter != realSize) {
			gIsStopping = 1;
			logErrorFormat("Linear memory's count went wrong in allocation #%lu: %p, %u bytes (%u kept), by 0x%08lX; free %u -> %u.",
				(unsigned long)gNumber, base + GUARD, (unsigned)tSize, (unsigned)realSize, (unsigned long)tCaller, (unsigned)freeBefore, (unsigned)freeAfter);
			stop();
		}
		// A block given out over one still in use.
		{
			const uintptr_t start = uintptr_t(base), end = uintptr_t(base) + realSize;
			auto it = gBlocks.upper_bound(start);
			if (it != gBlocks.begin()) {
				auto before = it;
				--before;
				if (uintptr_t(before->second.mBase) + before->second.mRealSize > start) it = before;
			}
			if (it != gBlocks.end() && uintptr_t(it->second.mBase) < end) {
				gIsStopping = 1;
				char text[300];
				logErrorFormat("Linear memory given out twice: %p, %u bytes, asked for by 0x%08lX (allocation #%lu), over a block still in use.", base + GUARD, (unsigned)tSize, (unsigned long)tCaller, (unsigned long)gNumber);
				describeBlock(text, sizeof(text), "In use", it->second);
				logError(text);
				stop();
			}
		}
		memset(base, PATTERN, GUARD);
		memset(base + GUARD + tSize, PATTERN, GUARD);
		// The guards out of the cache: the GPU and the DSP read and write the
		// memory itself.
		GSPGPU_FlushDataCache(base, GUARD);
		GSPGPU_FlushDataCache(base + GUARD + tSize, GUARD);
		Block block = { base, tSize, realSize, tCaller, gNumber };
		gBlocks[uintptr_t(base + GUARD)] = block;
		gRealInUse += realSize;
		unlock();
		return base + GUARD;
	}

	static void letGo(void* tMemory, uint32_t tCaller) {
		lock();
		noteThread(tCaller);
		auto it = gBlocks.find(uintptr_t(tMemory));
		if (it == gBlocks.end()) {
			// Not one of the blocks given out: let go twice, or never given.
			const size_t kept = linearGetSize(tMemory);
			if (gUnknownFreeReports < 20 && currentThread() == gMainThread) {
				gUnknownFreeReports++;
				addN3DSPerfLine("  linear diagnosis: %p let go by 0x%08lX, not a block in use (libctru keeps %u bytes there)", tMemory, (unsigned long)tCaller, (unsigned)kept);
			}
			if (kept && !gIsStopping) {
				gIsStopping = 1;
				logErrorFormat("Linear memory: %p let go by 0x%08lX is not a block given out, but libctru keeps %u bytes there.", tMemory, (unsigned long)tCaller, (unsigned)kept);
				stop();
			}
			unlock();
			return;
		}
		size_t firstBad = 0;
		if (!gIsStopping) {
			GSPGPU_FlushDataCache(it->second.mBase, GUARD);
			GSPGPU_FlushDataCache(it->second.mBase + GUARD + it->second.mSize, GUARD);
			if (!isGuardIntact(it->second.mBase, &firstBad)) stopOnDamage(it, 0, firstBad, "as it was let go");
			if (!isGuardIntact(it->second.mBase + GUARD + it->second.mSize, &firstBad)) stopOnDamage(it, 1, firstBad, "as it was let go");
		}
		const Block block = it->second;
		gBlocks.erase(it);
		gRealInUse -= block.mRealSize;
		gNumber++;
		const size_t kept = linearGetSize(block.mBase);
		const size_t freeBefore = linearSpaceFree();
		beginAllocatorOperation();
		__real_linearFree(block.mBase);
		endAllocatorOperation();
		const size_t freeAfter = linearSpaceFree();
		recordOperation(1, block.mBase + GUARD, block.mSize, tCaller, freeBefore, freeAfter);
		if (!gIsStopping) {
			if (kept != block.mRealSize) {
				gIsStopping = 1;
				char text[300];
				logErrorFormat("Linear memory: libctru kept %u bytes for a block it gave out as %u (written over since).", (unsigned)kept, (unsigned)block.mRealSize);
				describeBlock(text, sizeof(text), "Block", block);
				logError(text);
				stop();
			}
			if (freeAfter == freeBefore) {
				// libctru had no small block to note it in (the heap run out): lost.
				gLost += block.mRealSize;
			}
			else if (freeAfter - freeBefore != block.mRealSize) {
				gIsStopping = 1;
				char text[300];
				logErrorFormat("Linear memory's count went wrong letting a block go (by 0x%08lX): free %u -> %u.", (unsigned long)tCaller, (unsigned)freeBefore, (unsigned)freeAfter);
				describeBlock(text, sizeof(text), "Block", block);
				logError(text);
				stop();
			}
		}
		unlock();
	}

	// Every block's guards and size, and libctru's count, the cache flushed
	// first (written by the GPU or the DSP, the memory can differ from what
	// the CPU has cached).
	void checkN3DSLinearGuards(const char* tWhen) {
		if (gIsStopping) return;
		// At most twice a second (a load's steps come by the hundred).
		static u64 lastCheck = 0;
		const u64 now = osGetTime();
		if (now - lastCheck < 500) return;
		lastCheck = now;
		lock();
		reportPool();
		reportOtherThreads();
		const size_t freeNow = linearSpaceFree();
		if (gNumber && freeNow + gRealInUse + gLost != __ctru_linear_heap_size) {
			gIsStopping = 1;
			logErrorFormat("Linear memory's count went wrong between operations (%s): free %u + in use %u + lost %u is not %u.",
				tWhen, (unsigned)freeNow, (unsigned)gRealInUse, (unsigned)gLost, (unsigned)__ctru_linear_heap_size);
			stop();
		}
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
			const size_t kept = linearGetSize(block.mBase);
			if (kept != block.mRealSize) {
				gIsStopping = 1;
				char text[300];
				logErrorFormat("Linear memory (%s): libctru keeps %u bytes for a block it gave out as %u (written over since).", tWhen, (unsigned)kept, (unsigned)block.mRealSize);
				describeBlock(text, sizeof(text), "Block", block);
				logError(text);
				stop();
			}
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
		letGo(tMemory, uint32_t(uintptr_t(__builtin_return_address(0))));
	}

	// libctru's small blocks, asked for inside its allocator, from the pages;
	// everything else as always.
	void* __wrap_malloc(size_t tSize) {
		if (gOpThread && gOpThread == currentThread() && tSize <= NODE_SIZE && gPool) {
			if (gPoolFreeAmount) return gPool + size_t(gPoolFree[--gPoolFreeAmount]) * NODE_SIZE;
			gPoolMisses++;
		}
		return __real_malloc(tSize);
	}

	void __wrap_free(void* tMemory) {
		if (!isInPool(tMemory)) {
			__real_free(tMemory);
			return;
		}
		if (!gOpThread || gOpThread != currentThread()) {
			// One of libctru's blocks let go from outside its allocator.
			gIsStopping = 1;
			logErrorFormat("Linear memory: one of libctru's own blocks (%p) let go from outside its allocator, by 0x%08lX.", tMemory, (unsigned long)uintptr_t(__builtin_return_address(0)));
			abortSystem();
		}
		const uint32_t index = uint32_t(((uint8_t*)tMemory - gPool) / NODE_SIZE);
		gPoolFree[gPoolFreeAmount++] = uint16_t(index);
	}

}
#endif
