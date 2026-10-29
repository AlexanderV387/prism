#include "prism3dsperf.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <malloc.h>

#include <3ds.h>

#include "prism/log.h"

#ifndef N3DS_PERF_FILE
#define N3DS_PERF_FILE "sdmc:/3ds/dolmexica/load-log.txt"
#endif

namespace prism {

	N3DSPerfCounters gN3DSPerf;
	int gN3DSFileBufferSize;

	static struct {
		FILE* mFile;
		std::string mPending;
		uint64_t mStart;

		int mIsLoading;
		uint64_t mLoadStart;
		N3DSPerfCounters mLoadCounters;
		int mScreen;

		uint64_t mLastFrame;
		uint32_t mFrames;
		uint64_t mFrameTicks, mWorstFrame;
		uint32_t mSlowFrames;   // over 17 ms
		uint32_t mVerySlowFrames;   // over 34 ms
	} gPerf;

	uint64_t getN3DSPerfTicks() { return svcGetSystemTick(); }
	double getN3DSPerfMilliseconds(uint64_t tTicks) { return tTicks / (double)SYSCLOCK_ARM11 * 1000.0; }

	static double sinceStart() { return getN3DSPerfMilliseconds(getN3DSPerfTicks() - gPerf.mStart); }

	void addN3DSPerfLine(const char* tFormat, ...) {
		char line[512];
		va_list args;
		va_start(args, tFormat);
		vsnprintf(line, sizeof(line), tFormat, args);
		va_end(args);
		char stamped[600];
		snprintf(stamped, sizeof(stamped), "%9.1f %s\n", sinceStart(), line);
		gPerf.mPending += stamped;
	}

	// Written between loads only (writing to the SD card takes time).
	static void flushPerf() {
		if (!gPerf.mFile || gPerf.mPending.empty()) return;
		fwrite(gPerf.mPending.data(), 1, gPerf.mPending.size(), gPerf.mFile);
		fflush(gPerf.mFile);
		gPerf.mPending.clear();
	}

	static N3DSPerfCounters subtract(const N3DSPerfCounters& a, const N3DSPerfCounters& b) {
		N3DSPerfCounters r;
		r.mOpenTicks = a.mOpenTicks - b.mOpenTicks; r.mReadTicks = a.mReadTicks - b.mReadTicks; r.mSeekTicks = a.mSeekTicks - b.mSeekTicks;
		r.mOpens = a.mOpens - b.mOpens; r.mReads = a.mReads - b.mReads; r.mSeeks = a.mSeeks - b.mSeeks;
		r.mReadBytes = a.mReadBytes - b.mReadBytes;
		r.mTextureTicks = a.mTextureTicks - b.mTextureTicks; r.mTextures = a.mTextures - b.mTextures; r.mTexels = a.mTexels - b.mTexels;
		r.mDefTicks = a.mDefTicks - b.mDefTicks; r.mDefs = a.mDefs - b.mDefs;
		r.mSoundTicks = a.mSoundTicks - b.mSoundTicks; r.mSounds = a.mSounds - b.mSounds;
		return r;
	}

	static void writeFrameStatistics() {
		if (!gPerf.mFrames) return;
		addN3DSPerfLine("  screen %d shown: %u frames, %.1f ms average, worst %.1f ms, %u over 17 ms, %u over 34 ms",
			gPerf.mScreen, gPerf.mFrames, getN3DSPerfMilliseconds(gPerf.mFrameTicks) / gPerf.mFrames,
			getN3DSPerfMilliseconds(gPerf.mWorstFrame), gPerf.mSlowFrames, gPerf.mVerySlowFrames);
		gPerf.mFrames = 0;
		gPerf.mFrameTicks = gPerf.mWorstFrame = 0;
		gPerf.mSlowFrames = gPerf.mVerySlowFrames = 0;
	}

	static void startLoad() {
		writeFrameStatistics();
		gPerf.mScreen++;
		gPerf.mIsLoading = 1;
		gPerf.mLoadStart = getN3DSPerfTicks();
		gPerf.mLoadCounters = gN3DSPerf;
		addN3DSPerfLine("screen %d: loading", gPerf.mScreen);
	}

	static void endLoad() {
		if (!gPerf.mIsLoading) return;
		gPerf.mIsLoading = 0;
		const double total = getN3DSPerfMilliseconds(getN3DSPerfTicks() - gPerf.mLoadStart);
		const N3DSPerfCounters c = subtract(gN3DSPerf, gPerf.mLoadCounters);
		const double io = getN3DSPerfMilliseconds(c.mOpenTicks + c.mReadTicks + c.mSeekTicks);
		const double tex = getN3DSPerfMilliseconds(c.mTextureTicks);
		const double def = getN3DSPerfMilliseconds(c.mDefTicks);
		const double snd = getN3DSPerfMilliseconds(c.mSoundTicks);
		addN3DSPerfLine("screen %d: loaded in %.0f ms | SD %.0f ms (%u opens %.0f ms, %u reads %.0f ms, %u seeks %.0f ms, %.0f KB) | textures %.0f ms (%u, %.1f Mtexels) | .def parsing %.0f ms (%u) | sounds %.0f ms (%u) | rest %.0f ms",
			gPerf.mScreen, total, io, c.mOpens, getN3DSPerfMilliseconds(c.mOpenTicks), c.mReads, getN3DSPerfMilliseconds(c.mReadTicks),
			c.mSeeks, getN3DSPerfMilliseconds(c.mSeekTicks), c.mReadBytes / 1024.0, tex, c.mTextures, c.mTexels / 1e6,
			def, c.mDefs, snd, c.mSounds, total - io - tex - def - snd);
		const auto info = mallinfo();
		addN3DSPerfLine("  heap %d KB used, linear memory %d KB free", (int)(info.uordblks / 1024), (int)(linearSpaceFree() / 1024));
		flushPerf();
		gPerf.mLastFrame = 0;
	}

	// Every log entry (whatever the log's level), as a timeline: the load's
	// steps ("Loading sprite file", the fight's "init players"...).
	static void logCallback(void* /*tCaller*/, const LogEntry& tEntry) {
		const char* text = tEntry.mText;
		if (strstr(text, "StateMachineStateChange")) return;   // every move in a fight
		if (strstr(text, "Loading handled screen")) startLoad();

		const char* message = strchr(text, ']');
		message = message ? message + 2 : text;
		char line[300];
		snprintf(line, sizeof(line), "  %s", message);
		size_t length = strlen(line);
		while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
		addN3DSPerfLine("%s", line);

		if (strstr(text, "Show screen")) endLoad();
	}

	void initN3DSPerf() {
		gPerf.mStart = getN3DSPerfTicks();
		gPerf.mFile = fopen(N3DS_PERF_FILE, "w");
		setLogCallback(logCallback, NULL);
		addN3DSPerfLine("load-time log: time in ms since the start, then what happened");
	}

	void addN3DSPerfFrame() {
		const uint64_t now = getN3DSPerfTicks();
		if (gPerf.mLastFrame && !gPerf.mIsLoading) {
			const uint64_t t = now - gPerf.mLastFrame;
			gPerf.mFrames++;
			gPerf.mFrameTicks += t;
			if (t > gPerf.mWorstFrame) gPerf.mWorstFrame = t;
			const double ms = getN3DSPerfMilliseconds(t);
			if (ms > 17) gPerf.mSlowFrames++;
			if (ms > 34) gPerf.mVerySlowFrames++;
		}
		gPerf.mLastFrame = now;
	}

}
