#include "prism/system.h"

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <exception>
#include <new>
#include <malloc.h>
#include <signal.h>

#include <3ds.h>

#include "prism/log.h"

#include "prism3ds.h"
#include "prism3dsperf.h"

extern "C" {
	extern u32 __ctru_heap_size;
}

// [3DS] C++ allocations through malloc, as newlib's own, but the size of the
// last one with no room kept: the log of an "out of heap memory" stop says
// what was asked for.
static volatile size_t gN3DSLastFailedNewSize;

void* operator new(size_t tSize) {
	for (;;) {
		void* ret = malloc(tSize ? tSize : 1);
		if (ret) return ret;
		gN3DSLastFailedNewSize = tSize;
		const std::new_handler handler = std::get_new_handler();
		if (!handler) throw std::bad_alloc();
		handler();
	}
}
void* operator new[](size_t tSize) { return operator new(tSize); }
void* operator new(size_t tSize, const std::nothrow_t&) noexcept {
	try { return operator new(tSize); }
	catch (...) { return NULL; }
}
void* operator new[](size_t tSize, const std::nothrow_t&) noexcept {
	try { return operator new(tSize); }
	catch (...) { return NULL; }
}
void operator delete(void* tData) noexcept { free(tData); }
void operator delete[](void* tData) noexcept { free(tData); }
void operator delete(void* tData, size_t) noexcept { free(tData); }
void operator delete[](void* tData, size_t) noexcept { free(tData); }
void operator delete(void* tData, const std::nothrow_t&) noexcept { free(tData); }
void operator delete[](void* tData, const std::nothrow_t&) noexcept { free(tData); }

// [3DS] The console: the screens, the romfs (prism's own files) and the
// New 3DS's speed. The HOME Menu's "close" ends the game.

namespace prism {

	static struct {
		ScreenSize mScreenSize = { 640, 480 };
		ScreenSize mDisplayedScreenSize = { 400, 240 };
		int mIsLoaded = 0;
	} gSystemN3DSData;

	static void shutdownN3DS() {
		shutdownN3DSDrawing();
		romfsExit();
		gfxExit();
	}

	// An uncaught C++ error: logged, then the game stops as on any other error
	// (std::terminate's abort would end it without a word, the sound still
	// playing as the memory goes: a crash in the sound thread).
	// The biggest piece the heap can still give at once.
	static size_t findLargestFreeHeapBlock() {
		size_t low = 0, high = 32 * 1024 * 1024;
		while (high - low > 1024) {
			const size_t middle = low + (high - low) / 2;
			void* probe = malloc(middle);
			if (probe) {
				free(probe);
				low = middle;
			}
			else {
				high = middle;
			}
		}
		return low;
	}

	static void terminateN3DS() {
		letN3DSStreamedPixelsGo(0);
		const unsigned usedKB = (unsigned)(mallinfo().uordblks / 1024);
		const unsigned heapKB = (unsigned)(__ctru_heap_size / 1024);
		const unsigned largestKB = (unsigned)(findLargestFreeHeapBlock() / 1024);
		const unsigned askedBytes = (unsigned)gN3DSLastFailedNewSize;
		try {
			const auto exception = std::current_exception();
			if (exception) std::rethrow_exception(exception);
			logError("Uncaught C++ error.");
		}
		catch (const std::bad_alloc&) {
			logErrorFormat("Out of heap memory (a C++ allocation of %u bytes): %u KB used of %u KB, the biggest free piece %u KB.", askedBytes, usedKB, heapKB, largestKB);
		}
		catch (const std::exception& e) {
			logErrorFormat("Uncaught C++ error: %s (heap %u KB used of %u KB).", e.what(), usedKB, heapKB);
		}
		catch (...) {
			logErrorFormat("Uncaught C++ error (heap %u KB used of %u KB).", usedKB, heapKB);
		}
		abortSystem();
	}

	// A C++ allocation with no room: the streamed sprites' pixels go (read
	// again when drawn), then it is tried again; with none, std::bad_alloc.
	static void newHandlerN3DS() {
		// Not from another thread (the streamed sprites' reader): the game's
		// data is the game's thread's.
		if (threadGetCurrent() != NULL) throw std::bad_alloc();
		if (!letN3DSStreamedPixelsGo(0)) throw std::bad_alloc();
	}

	// abort() from a library (libpng's error with no handler, ...): logged,
	// then the game stops as on any other error, instead of newlib's silent
	// return to the Homebrew Launcher.
	static void abortSignalN3DS(int) {
		signal(SIGABRT, SIG_DFL);
		const unsigned usedKB = (unsigned)(mallinfo().uordblks / 1024);
		const unsigned heapKB = (unsigned)(__ctru_heap_size / 1024);
		const unsigned largestKB = (unsigned)(findLargestFreeHeapBlock() / 1024);
		logErrorFormat("abort() called (a library stopped the game): heap %u KB used of %u KB, the biggest free piece %u KB, linear memory %u KB free.", usedKB, heapKB, largestKB, (unsigned)(linearSpaceFree() / 1024));
		abortSystem();
	}

	// A CPU exception (a bad memory access, a jump to a bad address, an
	// undefined instruction): logged with where it happened (pc and lr, for
	// the .elf), then the game stops as on any other error, instead of
	// leaving without a word (two of the train's loads ended so: no Luma
	// screen, nothing in the logs). Each thread has its own handler stack:
	// a thread that ran out of its own still gets one.
	enum { N3DS_EXCEPTION_STACK_SIZE = 16 * 1024, N3DS_EXCEPTION_STACK_AMOUNT = 4 };
	static u8 gN3DSExceptionStacks[N3DS_EXCEPTION_STACK_AMOUNT][N3DS_EXCEPTION_STACK_SIZE] __attribute__((aligned(8)));
	static const char* gN3DSExceptionThreadNames[N3DS_EXCEPTION_STACK_AMOUNT];
	static volatile int gN3DSIsInException;

	static void __attribute__((target("arm"))) exceptionHandlerN3DS(ERRF_ExceptionInfo* tInfo, CpuRegisters* tRegs) {
		if (gN3DSIsInException++) {
			// Another thread's at the same time: it waits.
			for (;;) svcSleepThread(1000000000LL);
		}
		const char* kind = tInfo->type == ERRF_EXCEPTION_PREFETCH_ABORT ? "a jump to a bad address (prefetch abort)"
			: tInfo->type == ERRF_EXCEPTION_DATA_ABORT ? "a bad memory access (data abort)"
			: tInfo->type == ERRF_EXCEPTION_UNDEFINED ? "an undefined instruction"
			: "a floating point exception";
		const char* thread = "unknown";
		const u8* sp = (const u8*)tRegs;
		for (int i = 0; i < N3DS_EXCEPTION_STACK_AMOUNT; i++) {
			if (sp >= gN3DSExceptionStacks[i] && sp < gN3DSExceptionStacks[i] + N3DS_EXCEPTION_STACK_SIZE && gN3DSExceptionThreadNames[i]) thread = gN3DSExceptionThreadNames[i];
		}
		static char text[512];
		snprintf(text, sizeof(text), "CPU exception in the %s thread: %s at pc %08lX, lr %08lX, sp %08lX, address %08lX (fsr %08lX).",
			thread, kind, (unsigned long)tRegs->pc, (unsigned long)tRegs->lr, (unsigned long)tRegs->sp, (unsigned long)tInfo->far, (unsigned long)tInfo->fsr);
		logError(text);
		snprintf(text, sizeof(text), "Registers: r0 %08lX r1 %08lX r2 %08lX r3 %08lX r4 %08lX r5 %08lX r6 %08lX r7 %08lX r8 %08lX r9 %08lX r10 %08lX r11 %08lX r12 %08lX cpsr %08lX.",
			(unsigned long)tRegs->r[0], (unsigned long)tRegs->r[1], (unsigned long)tRegs->r[2], (unsigned long)tRegs->r[3], (unsigned long)tRegs->r[4], (unsigned long)tRegs->r[5], (unsigned long)tRegs->r[6],
			(unsigned long)tRegs->r[7], (unsigned long)tRegs->r[8], (unsigned long)tRegs->r[9], (unsigned long)tRegs->r[10], (unsigned long)tRegs->r[11], (unsigned long)tRegs->r[12], (unsigned long)tRegs->cpsr);
		logError(text);
		// The words on the stack: return addresses among them (the calls
		// that led there).
		const u32* stack = (const u32*)tRegs->sp;
		if (tRegs->sp >= 0x08000000 && tRegs->sp < 0x10000000 - 64 * 4) {
			int length = snprintf(text, sizeof(text), "Stack:");
			for (int i = 0; i < 48 && length < (int)sizeof(text) - 10; i++) length += snprintf(text + length, sizeof(text) - length, " %08lX", (unsigned long)stack[i]);
			logError(text);
		}
		abortSystem();
		for (;;) {}
	}

	void installN3DSExceptionHandler(int tSlot, const char* tThreadName) {
		if (tSlot < 0 || tSlot >= N3DS_EXCEPTION_STACK_AMOUNT) return;
		gN3DSExceptionThreadNames[tSlot] = tThreadName;
		threadOnException(exceptionHandlerN3DS, gN3DSExceptionStacks[tSlot] + N3DS_EXCEPTION_STACK_SIZE, WRITE_DATA_TO_HANDLER_STACK);
	}

	void initSystem() {
		if (gSystemN3DSData.mIsLoaded) return;
		installN3DSExceptionHandler(0, "main");
		signal(SIGABRT, abortSignalN3DS);
		std::set_terminate(terminateN3DS);
		std::set_new_handler(newHandlerN3DS);
		osSetSpeedupEnable(true);
		gfxInitDefault();
		// The log on the bottom screen too (printf), while the port is young.
		consoleInit(GFX_BOTTOM, NULL);
		romfsInit();
		atexit(shutdownN3DS);
		initN3DSPerf();
		gSystemN3DSData.mIsLoaded = 1;
	}

	void shutdownSystem() {}

	void updateSystem() {
		if (!aptMainLoop()) {
			exit(0);
		}
	}

	// An error the game cannot go on from: its message stays on the bottom
	// screen until START (or the HOME Menu's close).
	void abortSystem() {
		logError("Aborting system.");
		flushN3DSPerfOnError();
		printf("\nThe game stopped on an error (see above).\nPress START to exit.\n");
		while (aptMainLoop()) {
			hidScanInput();
			if (hidKeysDown() & KEY_START) break;
			gspWaitForVBlank();
		}
		exit(1);
	}

	void returnToMenu() {
		exit(0);
	}

	void setGameName(const char* /*tName*/) {}
	void updateGameName(const char* /*tName*/) {}
	void setIcon(const char* /*tPath*/) {}

	void setScreen(int tX, int tY, int /*tFramerate*/, int /*tIsVGA*/) {
		gSystemN3DSData.mScreenSize.x = tX;
		gSystemN3DSData.mScreenSize.y = tY;
	}

	void setScreenSize(int tX, int tY) {
		gSystemN3DSData.mScreenSize.x = tX;
		gSystemN3DSData.mScreenSize.y = tY;
	}

	ScreenSize getScreenSize() { return gSystemN3DSData.mScreenSize; }

	void setScreenPosition(int /*tX*/, int /*tY*/) {}
	void setScreenFullscreen(bool /*tIsFullscreen*/) {}
	ScreenSize getDisplayedScreenSize() { return gSystemN3DSData.mDisplayedScreenSize; }
	void setDisplayedScreenSize(int /*tX*/, int /*tY*/) {}
	void setScreenFramerate(int /*tFramerate*/) {}
	void setVGA() {}

	int isOnDreamcast() { return 0; }
	int isOnWindows() { return 0; }
	int isOnWeb() { return 0; }
	int isOnVita() { return 0; }

	uint64_t getSystemTicks() {
		return osGetTime();
	}

	uint64_t getUnixTimestampSeconds() { return (uint64_t)time(NULL); }

	uint64_t getUnixTimestampMilliseconds() {
		// osGetTime counts from 1900; the Unix epoch is 70 years later.
		return osGetTime() - 2208988800000ULL;
	}

}

// A failed assertion, the same way: logged (file, line, what failed), then the
// game stops as on any other error, instead of newlib's silent abort.
extern "C" __attribute__((noreturn)) void __assert_func(const char* tFile, int tLine, const char* tFunction, const char* tExpression) {
	using namespace prism;
	logErrorFormat("Assertion failed: %s (%s, %s line %d)", tExpression, tFunction ? tFunction : "", tFile, tLine);
	abortSystem();
	while (1) {}
}
