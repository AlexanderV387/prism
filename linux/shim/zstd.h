#pragma once
/* Minimal zstd.h shim for environments that have the libzstd runtime
 * (libzstd.so.1) but not the dev package. Declares only the functions
 * prism uses (compression.cpp, memoryhandler.cpp). If the real zstd.h
 * is available, prefer it by putting its include dir earlier on the
 * include path (or just don't add this shim dir). ABI-stable API. */

#include <stddef.h>

#if defined (__cplusplus)
extern "C" {
#endif

size_t ZSTD_compress(void* dst, size_t dstCapacity,
                     const void* src, size_t srcSize,
                     int compressionLevel);
size_t ZSTD_decompress(void* dst, size_t dstCapacity,
                       const void* src, size_t compressedSize);

#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR   (0ULL - 2)
unsigned long long ZSTD_getFrameContentSize(const void* src, size_t srcSize);

size_t ZSTD_compressBound(size_t srcSize);
unsigned ZSTD_isError(size_t code);

/* Contexts and advanced parameters (stable API, zstd >= 1.4.0): texture_3ds.cpp's kept sprite indices. */
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
ZSTD_CCtx* ZSTD_createCCtx(void);
size_t ZSTD_freeCCtx(ZSTD_CCtx* cctx);
ZSTD_DCtx* ZSTD_createDCtx(void);
size_t ZSTD_freeDCtx(ZSTD_DCtx* dctx);
typedef enum {
    ZSTD_c_compressionLevel = 100,
    ZSTD_c_windowLog = 101,
    ZSTD_c_contentSizeFlag = 200,
    ZSTD_c_checksumFlag = 201
} ZSTD_cParameter;
size_t ZSTD_CCtx_setParameter(ZSTD_CCtx* cctx, ZSTD_cParameter param, int value);
size_t ZSTD_compress2(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
size_t ZSTD_decompressDCtx(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);

#if defined (__cplusplus)
}
#endif
