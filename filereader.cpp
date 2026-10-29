#include "prism/filereader.h"

#include "prism/memoryhandler.h"

#include <string.h>

namespace prism {

	typedef struct {
		Buffer b;
		BufferPointer p;

		int mIsOver;
	} MugenSpriteFileBufferReaderData;

	static void initBufferReader(FileReader* tReader, const char* tPath) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)allocMemory(sizeof(MugenSpriteFileBufferReaderData));
		data->b = fileToBuffer(tPath);
		data->p = getBufferPointer(data->b);
		data->mIsOver = 0;
		tReader->mData = data;
	}

	static void deleteBufferReader(FileReader* tReader) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)tReader->mData;
		freeBuffer(data->b);
		freeMemory(data);
	}

	static void readBufferReader(FileReader* tReader, void* tDst, uint32_t tSize) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)tReader->mData;
		readFromBufferPointer(tDst, &data->p, tSize);
	}

	static void seekBufferReader(FileReader* tReader, uint32_t tPosition) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)tReader->mData;
		data->p = ((char*)data->b.mData) + tPosition;

		if (data->p >= ((char*)data->b.mData) + data->b.mLength) {
			tReader->mSetOver(tReader);
		}
	}

	static Buffer readBufferReaderBuffer(FileReader* tReader, uint32_t tSize) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)tReader->mData;
		return makeBuffer(data->p, tSize);
	}

	static uint32_t getCurrentBufferReaderOffset(FileReader* tReader) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)tReader->mData;
		return (uint32_t)(data->p - ((char*)data->b.mData));
	}

	static void setBufferReaderOver(FileReader* tReader) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)tReader->mData;
		data->p = NULL;
		data->mIsOver = 1;
	}

	static int isBufferReaderOver(FileReader* tReader) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)tReader->mData;
		return data->mIsOver;
	}

	FileReader getBufferFileReader() {
		FileReader ret;
		ret.mType = FILE_READER_TYPE_BUFFER;
		ret.mInit = initBufferReader;
		ret.mRead = readBufferReader;
		ret.mReadBufferReadOnly = readBufferReaderBuffer;
		ret.mSeek = seekBufferReader;
		ret.mGetCurrentOffset = getCurrentBufferReaderOffset;
		ret.mDelete = deleteBufferReader;
		ret.mSetOver = setBufferReaderOver;
		ret.mIsOver = isBufferReaderOver;
		return ret;
	}

	typedef struct {
		FileHandler mFile;
		uint32_t mFileSize;
		int mIsOver;
	} MugenSpriteFileFileReaderData;

	static void initFileReader(FileReader* tReader, const char* tPath) {
		MugenSpriteFileFileReaderData* data = (MugenSpriteFileFileReaderData*)allocMemory(sizeof(MugenSpriteFileFileReaderData));
		data->mFile = fileOpen(tPath, O_RDONLY);
		data->mFileSize = uint32_t(fileTotal(data->mFile));
		data->mIsOver = 0;
		tReader->mData = data;
	}

	static void deleteFileReader(FileReader* tReader) {
		MugenSpriteFileFileReaderData* data = (MugenSpriteFileFileReaderData*)tReader->mData;
		fileClose(data->mFile);
		freeMemory(data);
	}

	static void readFileReader(FileReader* tReader, void* tDst, uint32_t tSize) {
		MugenSpriteFileFileReaderData* data = (MugenSpriteFileFileReaderData*)tReader->mData;

		fileRead(data->mFile, tDst, tSize);
	}

	static void seekFileReader(FileReader* tReader, uint32_t tPosition) {
		MugenSpriteFileFileReaderData* data = (MugenSpriteFileFileReaderData*)tReader->mData;

		fileSeek(data->mFile, tPosition, SEEK_SET);
		uint32_t position = tReader->mGetCurrentOffset(tReader);

		if (position >= data->mFileSize) {
			tReader->mSetOver(tReader);
		}
	}

	static Buffer readFileReaderBuffer(FileReader* tReader, uint32_t tSize) {
		(void)tReader;
		uint32_t originalPosition = tReader->mGetCurrentOffset(tReader);
		char* dst = (char*)allocMemory(tSize + 10);
		tReader->mRead(tReader, dst, tSize);

		tReader->mSeek(tReader, originalPosition);

		return makeBufferOwned(dst, tSize);
	}

	static uint32_t getCurrentFileReaderOffset(FileReader* tReader) {
		MugenSpriteFileFileReaderData* data = (MugenSpriteFileFileReaderData*)tReader->mData;

		return (uint32_t)fileTell(data->mFile);
	}

	static void setFileReaderOver(FileReader* tReader) {
		MugenSpriteFileFileReaderData* data = (MugenSpriteFileFileReaderData*)tReader->mData;
		data->mIsOver = 1;
	}

	static int isFileReaderOver(FileReader* tReader) {
		MugenSpriteFileFileReaderData* data = (MugenSpriteFileFileReaderData*)tReader->mData;
		return data->mIsOver;
	}

	FileReader getFileFileReader() {
		FileReader ret;
		ret.mType = FILE_READER_TYPE_FILE;
		ret.mInit = initFileReader;
		ret.mRead = readFileReader;
		ret.mReadBufferReadOnly = readFileReaderBuffer;
		ret.mSeek = seekFileReader;
		ret.mGetCurrentOffset = getCurrentFileReaderOffset;
		ret.mDelete = deleteFileReader;
		ret.mSetOver = setFileReaderOver;
		ret.mIsOver = isFileReaderOver;
		return ret;
	}

	// A file read a window at a time: reads and seeks inside a window cost
	// nothing, large reads keep the SD card fast, and only the windows are
	// in memory (not the whole file). Three of them, the one used longest ago
	// filled anew: an SFF v2 goes back and forth between its table of sprites
	// and their pixels (two places), each jump was a new 1 MB read with one
	// window (2.5 GB read for a 19 MB file).
	static const uint32_t FILE_READER_WINDOW_SIZE = 512 * 1024;
	static const int FILE_READER_WINDOW_AMOUNT = 3;

	typedef struct {
		char* mData;
		uint32_t mStart;
		uint32_t mLength;
		uint32_t mLastUsed;
	} FileReaderWindow;

	typedef struct {
		FileHandler mFile;
		uint32_t mFileSize;
		uint32_t mPosition;
		FileReaderWindow mWindows[FILE_READER_WINDOW_AMOUNT];
		uint32_t mUseCounter;
		int mIsOver;
	} WindowedFileReaderData;

	static void initWindowedFileReader(FileReader* tReader, const char* tPath) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)allocMemory(sizeof(WindowedFileReaderData));
		data->mFile = fileOpen(tPath, O_RDONLY);
		data->mFileSize = uint32_t(fileTotal(data->mFile));
		data->mPosition = 0;
		for (auto& window : data->mWindows) {
			window.mData = (char*)allocMemory(FILE_READER_WINDOW_SIZE);
			window.mStart = window.mLength = window.mLastUsed = 0;
		}
		data->mUseCounter = 0;
		data->mIsOver = 0;
		tReader->mData = data;
	}

	static void deleteWindowedFileReader(FileReader* tReader) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)tReader->mData;
		fileClose(data->mFile);
		for (auto& window : data->mWindows) freeMemory(window.mData);
		freeMemory(data);
	}

	// The window holding these bytes, NULL if none.
	static FileReaderWindow* findWindow(WindowedFileReaderData* tData, uint32_t tPosition, uint32_t tSize) {
		for (auto& window : tData->mWindows) {
			if (tPosition >= window.mStart && tPosition + tSize <= window.mStart + window.mLength) {
				window.mLastUsed = ++tData->mUseCounter;
				return &window;
			}
		}
		return NULL;
	}

	static FileReaderWindow* fillWindow(WindowedFileReaderData* tData, uint32_t tPosition) {
		FileReaderWindow* window = &tData->mWindows[0];
		for (auto& other : tData->mWindows) {
			if (other.mLastUsed < window->mLastUsed) window = &other;
		}
		window->mStart = tPosition;
		window->mLength = 0;
		window->mLastUsed = ++tData->mUseCounter;
		if (tPosition >= tData->mFileSize) return window;
		const uint32_t length = tData->mFileSize - tPosition < FILE_READER_WINDOW_SIZE ? tData->mFileSize - tPosition : FILE_READER_WINDOW_SIZE;
		fileSeek(tData->mFile, tPosition, SEEK_SET);
		window->mLength = uint32_t(fileRead(tData->mFile, window->mData, length));
		return window;
	}

	static void readWindowedFileReader(FileReader* tReader, void* tDst, uint32_t tSize) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)tReader->mData;
		char* dst = (char*)tDst;
		while (tSize) {
			FileReaderWindow* window = findWindow(data, data->mPosition, 1);
			if (!window) {
				if (tSize >= FILE_READER_WINDOW_SIZE) {
					fileSeek(data->mFile, data->mPosition, SEEK_SET);
					const uint32_t read = uint32_t(fileRead(data->mFile, dst, tSize));
					data->mPosition += read;
					if (read < tSize) memset(dst + read, 0, tSize - read);
					return;
				}
				window = fillWindow(data, data->mPosition);
				if (!window->mLength) {
					memset(dst, 0, tSize);
					return;
				}
			}
			const uint32_t offset = data->mPosition - window->mStart;
			uint32_t amount = window->mLength - offset;
			if (amount > tSize) amount = tSize;
			memcpy(dst, window->mData + offset, amount);
			dst += amount;
			data->mPosition += amount;
			tSize -= amount;
		}
	}

	static void seekWindowedFileReader(FileReader* tReader, uint32_t tPosition) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)tReader->mData;
		data->mPosition = tPosition;
		if (tPosition >= data->mFileSize) {
			tReader->mSetOver(tReader);
		}
	}

	// Inside a window, the window itself (valid until that window is filled
	// anew: at least until the next read).
	static Buffer readWindowedFileReaderBuffer(FileReader* tReader, uint32_t tSize) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)tReader->mData;
		FileReaderWindow* window = findWindow(data, data->mPosition, tSize);
		if (!window && tSize < FILE_READER_WINDOW_SIZE) {
			window = fillWindow(data, data->mPosition);
			if (!(data->mPosition + tSize <= window->mStart + window->mLength)) window = NULL;
		}
		if (window) {
			return makeBuffer(window->mData + (data->mPosition - window->mStart), tSize);
		}
		const uint32_t originalPosition = data->mPosition;
		char* dst = (char*)allocMemory(tSize + 10);
		readWindowedFileReader(tReader, dst, tSize);
		data->mPosition = originalPosition;
		return makeBufferOwned(dst, tSize);
	}

	static uint32_t getCurrentWindowedFileReaderOffset(FileReader* tReader) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)tReader->mData;
		return data->mPosition;
	}

	static void setWindowedFileReaderOver(FileReader* tReader) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)tReader->mData;
		data->mIsOver = 1;
	}

	static int isWindowedFileReaderOver(FileReader* tReader) {
		WindowedFileReaderData* data = (WindowedFileReaderData*)tReader->mData;
		return data->mIsOver;
	}

	FileReader getWindowedFileFileReader() {
		FileReader ret;
		ret.mType = FILE_READER_TYPE_FILE;
		ret.mInit = initWindowedFileReader;
		ret.mRead = readWindowedFileReader;
		ret.mReadBufferReadOnly = readWindowedFileReaderBuffer;
		ret.mSeek = seekWindowedFileReader;
		ret.mGetCurrentOffset = getCurrentWindowedFileReaderOffset;
		ret.mDelete = deleteWindowedFileReader;
		ret.mSetOver = setWindowedFileReaderOver;
		ret.mIsOver = isWindowedFileReaderOver;
		return ret;
	}

	void initBufferFileReaderReadOnlyBuffer(FileReader* tReader, const Buffer& tBuffer) {
		MugenSpriteFileBufferReaderData* data = (MugenSpriteFileBufferReaderData*)allocMemory(sizeof(MugenSpriteFileBufferReaderData));
		data->b = tBuffer;
		data->p = getBufferPointer(data->b);
		data->mIsOver = 0;
		tReader->mData = data;
	}

}