#include "prism/system.h"

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <3ds.h>

#include "prism/log.h"

#include "prism3ds.h"
#include "prism3dsperf.h"

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

	void initSystem() {
		if (gSystemN3DSData.mIsLoaded) return;
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
