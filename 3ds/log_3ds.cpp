#include "prism/log.h"

#include <malloc.h>
#include <string.h>

#include <3ds.h>

#include <prism/debug.h>

// [3DS] The log to the SD card, next to the game's files.

#ifndef N3DS_LOG_FILE
#define N3DS_LOG_FILE "sdmc:/3ds/dolmexica/log.txt"
#endif

namespace prism {

	void logTextureMemoryState() {
		logFormat("Linear memory free: %d", (int)linearSpaceFree());
	}

	void logMemoryState() {
		const auto info = mallinfo();
		logFormat("Heap: %d used, %d free", (int)info.uordblks, (int)info.fordblks);
		logTextureMemoryState();
	}

	void printLogColorStart(LogType /*tType*/) {}
	void printLogColorEnd(LogType /*tType*/) {}

	void hardwareLogToFile(FileHandler& tFileHandler, const char* tText) {
		if (getMinimumLogType() == LOG_TYPE_NONE) return;

		auto prevLogType = getMinimumLogType();
		setMinimumLogType(LOG_TYPE_NONE);

		if (tFileHandler == FILEHND_INVALID) {
			tFileHandler = fopen(N3DS_LOG_FILE, "wb");
		}
		if (tFileHandler != FILEHND_INVALID) {
			fwrite(tText, 1, strlen(tText), tFileHandler);
			fflush(tFileHandler);
		}

		setMinimumLogType(prevLogType);
	}

}
