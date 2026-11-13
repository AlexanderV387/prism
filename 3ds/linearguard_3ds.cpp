// [3DS] libctru's linear memory allocator (libctru 2.x, allocator/linear.cpp)
// starts its pool on the first allocation, when the pool is not "ready":
// ready means its list of free pieces is not empty. An allocation that takes
// the last free byte leaves the list empty, and the next one starts the
// pool again over the whole linear memory: the screens' framebuffers and
// every texture in use given out a second time. With a high resolution
// character (Chun Li HD against herself) the textures took it all: the
// sprites and both screens drawn over each other, the sounds broken (found
// in the Azahar emulator with the diagnosis build, lineardiag_3ds.cpp:
// "free 512 -> 0", then a texture at the first framebuffer's address).
//
// Linked with --wrap=linearAlloc --wrap=linearMemAlign: an allocation that
// would leave no linear memory free is given back and fails instead, so the
// list is never empty. (The diagnosis build does the same in its own
// wrappers.)

#if defined(__3DS__) && !defined(N3DS_LINEAR_DIAG)

#include <3ds.h>

extern "C" {
	void* __real_linearMemAlign(size_t tSize, size_t tAlignment);

	void* __wrap_linearMemAlign(size_t tSize, size_t tAlignment) {
		void* memory = __real_linearMemAlign(tSize, tAlignment);
		if (memory && !linearSpaceFree()) {
			linearFree(memory);
			return NULL;
		}
		return memory;
	}

	void* __wrap_linearAlloc(size_t tSize) {
		return __wrap_linearMemAlign(tSize, 0x80);
	}
}

#endif
