#include <algorithm>
#include "prism/n3dsperf.h"
#include "prism3dsperf.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <malloc.h>

#include <3ds.h>
#include <citro3d.h>

#include "prism/log.h"
#include "prism/mugenanimationhandler.h"

#ifndef N3DS_PERF_FILE
#define N3DS_PERF_FILE "sdmc:/3ds/dolmexica/load-log.txt"
#endif
#ifndef N3DS_COMPAT_FILE
#define N3DS_COMPAT_FILE "sdmc:/3ds/dolmexica/compat-log.txt"
#endif

#define N3DS_COMPAT_MAXIMUM_WARNINGS 300

namespace prism {

	N3DSPerfCounters gN3DSPerf;
	int gN3DSFileBufferSize;

	static void resetLogicParts();

	// Where frames' time went: game logic (the rest), making the drawing,
	// waiting for the GPU and the screen; the GPU's own drawing time.
	typedef struct {
		uint32_t mFrames;
		uint64_t mFrameTicks, mWaitTicks, mDrawTicks;
		double mGPUMilliseconds;
		uint64_t mSprites, mBatches;
	} FrameParts;

	static void addFrameParts(FrameParts* tParts, uint64_t tFrameTicks, uint64_t tWaitTicks, uint64_t tDrawTicks, int tSprites, int tBatches) {
		tParts->mFrames++;
		tParts->mFrameTicks += tFrameTicks;
		tParts->mWaitTicks += tWaitTicks;
		tParts->mDrawTicks += tDrawTicks;
		tParts->mGPUMilliseconds += C3D_GetDrawingTime();
		tParts->mSprites += tSprites;
		tParts->mBatches += tBatches;
	}

	static void describeFrameParts(const FrameParts& tParts, char* oText, size_t tSize) {
		const double n = tParts.mFrames ? tParts.mFrames : 1;
		const double frame = getN3DSPerfMilliseconds(tParts.mFrameTicks) / n;
		const double wait = getN3DSPerfMilliseconds(tParts.mWaitTicks) / n;
		const double draw = getN3DSPerfMilliseconds(tParts.mDrawTicks) / n;
		snprintf(oText, tSize, "%.1f ms a frame: game logic %.1f ms, making the drawing %.1f ms, waiting for GPU and screen %.1f ms | GPU drawing %.1f ms | %.0f sprites in %.0f batches",
			frame, frame - wait - draw, draw, wait, tParts.mGPUMilliseconds / n, tParts.mSprites / n, tParts.mBatches / n);
	}

	static struct {
		FILE* mFile;
		std::string mPending;
		uint64_t mStart;

		int mIsLoading;
		uint64_t mLoadStart;
		N3DSPerfCounters mLoadCounters;
		N3DSPerfCounters mShownCounters;   // when the screen was shown
		int mScreen;

		uint64_t mLastFrame;
		FrameParts mParts;      // the screen's
		FrameParts mInterval;   // the last 10 seconds'
		int mIntervalNumber;
		uint32_t mFrames;
		uint64_t mFrameTicks, mWorstFrame;
		uint32_t mSlowFrames;   // over 17 ms
		uint32_t mVerySlowFrames;   // over 34 ms
	} gPerf;

	typedef struct {
		std::string mName;
		std::vector<std::string> mLines;
		std::vector<std::pair<std::string, int>> mWarnings;
		std::map<std::string, int> mWarningIndices;
		int mDroppedWarnings;
	} CompatSection;

	static struct {
		std::vector<CompatSection> mSections;
		int mCurrent = -1;
		int mIsDirty;
	} gCompat;

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
		r.mStatTicks = a.mStatTicks - b.mStatTicks; r.mStats = a.mStats - b.mStats;
		r.mTextureTicks = a.mTextureTicks - b.mTextureTicks; r.mTextures = a.mTextures - b.mTextures; r.mTexels = a.mTexels - b.mTexels;
		r.mDefTicks = a.mDefTicks - b.mDefTicks; r.mDefs = a.mDefs - b.mDefs;
		r.mSoundTicks = a.mSoundTicks - b.mSoundTicks; r.mSounds = a.mSounds - b.mSounds;
		r.mDeferredTextures = a.mDeferredTextures - b.mDeferredTextures;
		r.mLateTextures = a.mLateTextures - b.mLateTextures; r.mLateTextureTicks = a.mLateTextureTicks - b.mLateTextureTicks;
		r.mEvictedTextures = a.mEvictedTextures - b.mEvictedTextures;
		r.mRepaletted = a.mRepaletted - b.mRepaletted; r.mRepalettedTicks = a.mRepalettedTicks - b.mRepalettedTicks;
		return r;
	}

	static void writeFrameStatistics() {
		if (!gPerf.mFrames) return;
		addN3DSPerfLine("  screen %d shown: %u frames, %.1f ms average, worst %.1f ms, %u over 17 ms, %u over 34 ms",
			gPerf.mScreen, gPerf.mFrames, getN3DSPerfMilliseconds(gPerf.mFrameTicks) / gPerf.mFrames,
			getN3DSPerfMilliseconds(gPerf.mWorstFrame), gPerf.mSlowFrames, gPerf.mVerySlowFrames);
		char parts[300];
		describeFrameParts(gPerf.mParts, parts, sizeof(parts));
		addN3DSPerfLine("  %s", parts);
		const N3DSPerfCounters c = subtract(gN3DSPerf, gPerf.mShownCounters);
		if (c.mRepaletted) {
			addN3DSPerfLine("  textures made again for a changed palette: %u (%.0f ms)", c.mRepaletted, getN3DSPerfMilliseconds(c.mRepalettedTicks));
		}
		if (c.mLateTextures || c.mEvictedTextures) {
			addN3DSPerfLine("  textures made while shown: %u (%.0f ms, %.2f ms each), let go for room: %u",
				c.mLateTextures, getN3DSPerfMilliseconds(c.mLateTextureTicks), getN3DSPerfMilliseconds(c.mLateTextureTicks) / (c.mLateTextures ? c.mLateTextures : 1), c.mEvictedTextures);
		}
		gPerf.mFrames = 0;
		memset(&gPerf.mParts, 0, sizeof(gPerf.mParts));
		memset(&gPerf.mInterval, 0, sizeof(gPerf.mInterval));
		resetLogicParts();
		gPerf.mIntervalNumber = 0;
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
		const double io = getN3DSPerfMilliseconds(c.mOpenTicks + c.mReadTicks + c.mSeekTicks + c.mStatTicks);
		const double tex = getN3DSPerfMilliseconds(c.mTextureTicks);
		const double def = getN3DSPerfMilliseconds(c.mDefTicks);
		const double snd = getN3DSPerfMilliseconds(c.mSoundTicks);
		addN3DSPerfLine("screen %d: loaded in %.0f ms | SD %.0f ms (%u file checks %.0f ms, %u opens %.0f ms, %u reads %.0f ms, %u seeks %.0f ms, %.0f KB) | textures %.0f ms (%u, %.1f Mtexels) | .def parsing %.0f ms (%u) | sounds %.0f ms (%u) | rest %.0f ms",
			gPerf.mScreen, total, io, c.mStats, getN3DSPerfMilliseconds(c.mStatTicks), c.mOpens, getN3DSPerfMilliseconds(c.mOpenTicks), c.mReads, getN3DSPerfMilliseconds(c.mReadTicks),
			c.mSeeks, getN3DSPerfMilliseconds(c.mSeekTicks), c.mReadBytes / 1024.0, tex, c.mTextures, c.mTexels / 1e6,
			def, c.mDefs, snd, c.mSounds, total - io - tex - def - snd);
		const auto info = mallinfo();
		addN3DSPerfLine("  heap %d KB used, linear memory %d KB free", (int)(info.uordblks / 1024), (int)(linearSpaceFree() / 1024));
		if (c.mDeferredTextures) addN3DSPerfLine("  %u textures without room yet: made when drawn", c.mDeferredTextures);
		flushPerf();
		gPerf.mShownCounters = gN3DSPerf;
		gPerf.mLastFrame = 0;
	}

	// ==========================================================================
	// The compatibility log.

	static void appendFormat(std::string& oText, const char* tFormat, ...) {
		char buffer[1024];
		va_list args;
		va_start(args, tFormat);
		vsnprintf(buffer, sizeof(buffer), tFormat, args);
		va_end(args);
		oText += buffer;
	}

	// Written over itself, never emptied first: emptying and writing a file
	// again took the SD card 8.5 s each time on a player's console (looking
	// for room anew); a shorter text is padded to the file's length.
	static void writeCompatLog() {
		gCompat.mIsDirty = 0;
		const uint64_t start = getN3DSPerfTicks();
		std::string text = "compatibility log: per character and fight, what the game said and the warnings meanwhile (each once, with how often)\n";
		for (const auto& section : gCompat.mSections) {
			appendFormat(text, "\n== %s\n", section.mName.c_str());
			for (const auto& line : section.mLines) appendFormat(text, "  %s\n", line.c_str());
			if (!section.mWarnings.empty()) appendFormat(text, "  warnings (%d different):\n", (int)section.mWarnings.size());
			for (const auto& warning : section.mWarnings) appendFormat(text, "    %5dx %s\n", warning.second, warning.first.c_str());
			if (section.mDroppedWarnings) appendFormat(text, "    (and %d more different ones)\n", section.mDroppedWarnings);
		}

		static long sFileSize = -1;   // as found when first written, then as written
		FILE* file = fopen(N3DS_COMPAT_FILE, "r+b");
		if (!file) {
			file = fopen(N3DS_COMPAT_FILE, "wb");
			sFileSize = 0;
		}
		if (!file) return;
		if (sFileSize < 0) {
			fseek(file, 0, SEEK_END);
			sFileSize = ftell(file);
			fseek(file, 0, SEEK_SET);
		}
		if ((long)text.size() < sFileSize) text.append(sFileSize - text.size() - 1, ' ').append("\n");
		fwrite(text.data(), 1, text.size(), file);
		fclose(file);
		if ((long)text.size() > sFileSize) sFileSize = (long)text.size();

		const double ms = getN3DSPerfMilliseconds(getN3DSPerfTicks() - start);
		if (ms > 100) addN3DSPerfLine("  compat-log.txt written in %.0f ms (the SD card was slow to write)", ms);
	}

	static int findCompatSection(const char* tName, int* oIsNew = NULL) {
		if (oIsNew) *oIsNew = 0;
		for (size_t i = 0; i < gCompat.mSections.size(); i++) {
			if (gCompat.mSections[i].mName == tName) return (int)i;
		}
		if (oIsNew) *oIsNew = 1;
		CompatSection section;
		section.mName = tName;
		section.mDroppedWarnings = 0;
		gCompat.mSections.push_back(section);
		return (int)gCompat.mSections.size() - 1;
	}

	void beginN3DSCompatSection(const char* tName) {
		int isNew;
		gCompat.mCurrent = findCompatSection(tName, &isNew);
		if (!isNew) return;
		gCompat.mSections[gCompat.mCurrent].mLines.push_back("(loading...)");
		writeCompatLog();
		gCompat.mSections[gCompat.mCurrent].mLines.pop_back();
		gCompat.mIsDirty = 1;
	}

	void addN3DSCompatLine(const char* tFormat, ...) {
		if (gCompat.mCurrent < 0) gCompat.mCurrent = findCompatSection("outside a character");
		char line[512];
		va_list args;
		va_start(args, tFormat);
		vsnprintf(line, sizeof(line), tFormat, args);
		va_end(args);
		auto& lines = gCompat.mSections[gCompat.mCurrent].mLines;
		lines.push_back(line);
		if (lines.size() > 200) lines.erase(lines.begin());   // a character in many fights
		gCompat.mIsDirty = 1;
	}

	int getN3DSHeapUsedKB() { return (int)(mallinfo().uordblks / 1024); }
	int getN3DSLinearFreeKB() { return (int)(linearSpaceFree() / 1024); }

	void endN3DSCompatSection() {
		gCompat.mCurrent = -1;
	}

	static void addCompatWarning(const LogEntry& tEntry) {
		if (gCompat.mCurrent < 0) gCompat.mCurrent = findCompatSection("outside a character");
		auto& section = gCompat.mSections[gCompat.mCurrent];
		std::string text = tEntry.mText;
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
		if (tEntry.mType == LOG_TYPE_ERROR) text = "ERROR " + text;
		gCompat.mIsDirty = 1;
		const auto it = section.mWarningIndices.find(text);
		if (it != section.mWarningIndices.end()) {
			section.mWarnings[it->second].second++;
		}
		else if ((int)section.mWarnings.size() < N3DS_COMPAT_MAXIMUM_WARNINGS) {
			section.mWarningIndices[text] = (int)section.mWarnings.size();
			section.mWarnings.push_back(std::make_pair(text, 1));
			// A new one in a fight: written at once, in case it crashes next.
			if (!gPerf.mIsLoading) writeCompatLog();
		}
		else {
			section.mDroppedWarnings++;
		}
	}

	// Every log entry (whatever the log's level): while loading, as a
	// timeline of the load's steps ("Loading sprite file", the fight's
	// "init players"...); warnings and errors also to the compatibility log.
	static void logCallback(void* /*tCaller*/, const LogEntry& tEntry) {
		const char* text = tEntry.mText;
		if (tEntry.mType >= LOG_TYPE_WARNING) addCompatWarning(tEntry);
		if (strstr(text, "StateMachineStateChange")) return;   // every move in a fight
		if (strstr(text, "Loading handled screen")) startLoad();
		if (!gPerf.mIsLoading && !strstr(text, "Unloading handled screen")) return;

		const char* message = strchr(text, ']');
		message = message ? message + 2 : text;
		char line[300];
		snprintf(line, sizeof(line), "  %s", message);
		size_t length = strlen(line);
		while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
		addN3DSPerfLine("%s", line);

		if (strstr(text, "Show screen")) {
			endLoad();
			if (gCompat.mIsDirty) writeCompatLog();
		}
	}

	// The CPU's speed: a loop of 50 million steps (one subtract and branch
	// each) timed in the 268 MHz mode and in the New 3DS's 804 MHz one (with
	// its L2 cache), as ptm:sysm answers asking for each; left in the fast one.
	static double timeCPULoop() {
		const uint64_t start = getN3DSPerfTicks();
		uint32_t steps = 50000000;
		asm volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(steps) : : "cc");
		return getN3DSPerfMilliseconds(getN3DSPerfTicks() - start);
	}

	static void addCPUSpeedLine() {
		bool isNew3DS = false;
		APT_CheckNew3DS(&isNew3DS);
		const double asStarted = timeCPULoop();
		const Result initResult = ptmSysmInit();
		if (R_FAILED(initResult)) {
			addN3DSPerfLine("CPU: %s, loop of 50 million steps %.0f ms; ptm:sysm not available (0x%08lX): the fast mode cannot be asked for", isNew3DS ? "New 3DS" : "Old 3DS", asStarted, (unsigned long)initResult);
			return;
		}
		const Result slowResult = PTMSYSM_ConfigureNew3DSCPU(0);
		const double slow = timeCPULoop();
		const Result fastResult = PTMSYSM_ConfigureNew3DSCPU(3);
		const double fast = timeCPULoop();
		ptmSysmExit();
		addN3DSPerfLine("CPU: %s, loop of 50 million steps: %.0f ms as started, %.0f ms in the slow mode (0x%08lX), %.0f ms in the fast mode (0x%08lX)",
			isNew3DS ? "New 3DS" : "Old 3DS", asStarted, slow, (unsigned long)slowResult, fast, (unsigned long)fastResult);
	}

	void initN3DSPerf() {
		gPerf.mStart = getN3DSPerfTicks();
		gPerf.mFile = fopen(N3DS_PERF_FILE, "w");
		setLogCallback(logCallback, NULL);
		addN3DSPerfLine("load-time log: time in ms since the start, then what happened");
		addCPUSpeedLine();
	}

	// Where the game logic's time went (prism/n3dsperf.h): named parts, or
	// an address (an actor's update, found in the .elf) when unnamed.
	typedef struct {
		const char* mName;
		const void* mAddress;
		uint64_t mTicks;
	} LogicPart;
	static const int LOGIC_PART_AMOUNT = 48;
	static LogicPart gLogicParts[LOGIC_PART_AMOUNT];
	static int gLogicPartAmount;

	uint64_t getN3DSLogicPartTicks() { return svcGetSystemTick(); }

	static void addLogicPart(const char* tName, const void* tAddress, uint64_t tTicks) {
		for (int i = 0; i < gLogicPartAmount; i++) {
			if (gLogicParts[i].mName == tName && gLogicParts[i].mAddress == tAddress) {
				gLogicParts[i].mTicks += tTicks;
				return;
			}
		}
		if (gLogicPartAmount >= LOGIC_PART_AMOUNT) return;
		gLogicParts[gLogicPartAmount].mName = tName;
		gLogicParts[gLogicPartAmount].mAddress = tAddress;
		gLogicParts[gLogicPartAmount].mTicks = tTicks;
		gLogicPartAmount++;
	}

	static void resetLogicParts() { gLogicPartAmount = 0; }

	void addN3DSLogicPartTicks(const char* tName, uint64_t tTicks) { addLogicPart(tName, NULL, tTicks); }
	void addN3DSLogicPartTicksForAddress(const void* tAddress, uint64_t tTicks) { addLogicPart(NULL, tAddress, tTicks); }

	// The parts taking the most, a frame's milliseconds each; emptied.
	static void describeLogicParts(uint32_t tFrames, char* oText, size_t tSize) {
		*oText = '\0';
		if (!gLogicPartAmount || !tFrames) return;
		std::sort(gLogicParts, gLogicParts + gLogicPartAmount, [](const LogicPart& a, const LogicPart& b) { return a.mTicks > b.mTicks; });
		size_t used = 0;
		used += snprintf(oText + used, tSize - used, " | logic parts:");
		for (int i = 0; i < gLogicPartAmount && i < 8 && used < tSize; i++) {
			const double ms = getN3DSPerfMilliseconds(gLogicParts[i].mTicks) / tFrames;
			if (ms < 0.05) break;
			if (gLogicParts[i].mName) used += snprintf(oText + used, tSize - used, " %s %.1f ms,", gLogicParts[i].mName, ms);
			else used += snprintf(oText + used, tSize - used, " %p %.1f ms,", gLogicParts[i].mAddress, ms);
		}
		gLogicPartAmount = 0;
	}

	// Every 10 seconds of a screen (600 frames), how its frames went: what
	// grows (animations, the heap) shows there.
	static const uint32_t INTERVAL_FRAMES = 600;

	static void writeIntervalLine() {
		gPerf.mIntervalNumber++;
		char parts[300];
		describeFrameParts(gPerf.mInterval, parts, sizeof(parts));
		const auto info = mallinfo();
		char logicParts[600];
		describeLogicParts(gPerf.mInterval.mFrames, logicParts, sizeof(logicParts));
		addN3DSPerfLine("  at %d s: %s | %d animations | heap %d KB used, linear memory %d KB free, %u textures made while shown, %u let go, %u made again for a palette (%.0f ms)%s",
			gPerf.mIntervalNumber * 10, parts, getMugenAnimationHandlerLiveAmount(), (int)(info.uordblks / 1024), (int)(linearSpaceFree() / 1024),
			gN3DSPerf.mLateTextures - gPerf.mShownCounters.mLateTextures, gN3DSPerf.mEvictedTextures - gPerf.mShownCounters.mEvictedTextures,
			gN3DSPerf.mRepaletted - gPerf.mShownCounters.mRepaletted, getN3DSPerfMilliseconds(gN3DSPerf.mRepalettedTicks - gPerf.mShownCounters.mRepalettedTicks), logicParts);
		memset(&gPerf.mInterval, 0, sizeof(gPerf.mInterval));
	}

	// The frame counter on the bottom screen (its first line), every half
	// second: frames a second, and where a frame's time went.
	static FrameParts gCounterParts;

	static void updateFrameCounter() {
		const double n = gCounterParts.mFrames;
		const double frame = getN3DSPerfMilliseconds(gCounterParts.mFrameTicks) / n;
		const double wait = getN3DSPerfMilliseconds(gCounterParts.mWaitTicks) / n;
		const double draw = getN3DSPerfMilliseconds(gCounterParts.mDrawTicks) / n;
		printf("\x1b[1;1H\x1b[0mFPS %4.1f logica %4.1fms dibujo %3.1fms   ", 1000.0 / frame, frame - wait - draw, draw);
		memset(&gCounterParts, 0, sizeof(gCounterParts));
	}

	void addN3DSPerfFrame(uint64_t tWaitTicks, uint64_t tDrawTicks, int tSprites, int tBatches) {
		const uint64_t now = getN3DSPerfTicks();
		if (gPerf.mLastFrame && !gPerf.mIsLoading) {
			const uint64_t t = now - gPerf.mLastFrame;
			addFrameParts(&gPerf.mParts, t, tWaitTicks, tDrawTicks, tSprites, tBatches);
			addFrameParts(&gPerf.mInterval, t, tWaitTicks, tDrawTicks, tSprites, tBatches);
			addFrameParts(&gCounterParts, t, tWaitTicks, tDrawTicks, tSprites, tBatches);
			if (gCounterParts.mFrames >= 30) updateFrameCounter();
			if (gPerf.mInterval.mFrames >= INTERVAL_FRAMES) writeIntervalLine();
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
