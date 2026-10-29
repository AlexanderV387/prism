#include "prism/memorystack.h"

#include <stdio.h>

#include <prism/memoryhandler.h>
#include <prism/log.h>

namespace prism {

	MemoryStack createMemoryStack(uint32_t tSize)
	{
		MemoryStack ret;
		ret.mAddress = allocMemory(tSize);
		ret.mSize = tSize;
		ret.mOffset = 0;
		ret.mAmount = 0;
		return ret;
	}

	void destroyMemoryStack(MemoryStack* tStack)
	{
		freeMemory(tStack->mAddress);
	}

	void resizeMemoryStackToCurrentSize(MemoryStack* tStack) {
		// Not shrunk: realloc may move the block, and what was allocated on
		// it keeps pointing at the old one (found with AddressSanitizer).
		(void)tStack;
	}

	void* allocMemoryOnMemoryStack(MemoryStack* tStack, uint32_t tSize)
	{
		const auto padding = (4 - (tSize % 4)) % 4;
		if (tStack->mOffset + tSize + padding > tStack->mSize) {
			// Full: on its own (freed with the rest of the screen's memory),
			// not past the stack's end.
			static int hasWarned = 0;
			if (!hasWarned) {
				logWarningFormat("Memory stack of %u bytes full: allocating outside it.", (unsigned)tStack->mSize);
				hasWarned = 1;
			}
			tStack->mAmount++;
			return allocMemory(tSize);
		}
		void* ret = (void*)((uintptr_t)tStack->mAddress + tStack->mOffset);
		tStack->mOffset += tSize + padding;

		tStack->mAmount++;
		return ret;
	}

	int canFitOnMemoryStack(MemoryStack* tStack, uint32_t tSize)
	{
		const auto padding = (4 - (tSize % 4)) % 4;
		return (tStack->mOffset + tSize + padding) <= tStack->mSize;
	}

}