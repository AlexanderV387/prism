#include "prism/sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>
#include <mpg123.h>
#include <tremor/ivorbisfile.h>

#include "prism/actorhandler.h"
#include "prism/file.h"
#include "prism/log.h"

#include "prism3dsperf.h"

// [3DS] Music: MP3 (libmpg123), OGG (Tremor) and WAV files streamed to the
// DSP's last channel. A thread decodes ahead (on the New 3DS's third core
// when it can), woken by ndsp after each of its frames; the main thread
// starts, stops and pauses under a lock. CD tracks are not played.

namespace prism {

	static const int MUSIC_CHANNEL = 23;
	static const int BUFFER_AMOUNT = 3;
	static const int BUFFER_FRAMES = 4096;  // per buffer, stereo or mono

	typedef enum { DECODER_NONE, DECODER_MP3, DECODER_OGG, DECODER_WAV } DecoderType;

	static struct {
		int mIsAvailable;
		LightLock mLock;
		LightEvent mWake;
		Thread mThread;
		volatile int mIsRunning;

		DecoderType mType;
		mpg123_handle* mMP3;
		OggVorbis_File mOGG;
		FILE* mFile;            // OGG and WAV
		long mWAVDataStart, mWAVDataEnd;

		int mChannels;
		long mRate;
		int mIsLooping;
		int mIsDecoding;        // more to read
		int mIsPaused;
		uint64_t mFramesPlayed;

		s16* mBufferData[BUFFER_AMOUNT];
		ndspWaveBuf mBuffers[BUFFER_AMOUNT];

		float mVolume = 1.0;
		float mPanning = 0.0;
	} gN3DSMusic;

	// ==========================================================================
	// Decoders (called with the lock held).

	static void closeDecoder() {
		switch (gN3DSMusic.mType) {
		case DECODER_MP3:
			mpg123_close(gN3DSMusic.mMP3);
			break;
		case DECODER_OGG:
			ov_clear(&gN3DSMusic.mOGG);   // also closes the file
			gN3DSMusic.mFile = NULL;
			break;
		case DECODER_WAV:
			fclose(gN3DSMusic.mFile);
			gN3DSMusic.mFile = NULL;
			break;
		default:
			break;
		}
		gN3DSMusic.mType = DECODER_NONE;
	}

	static u32 readU32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
	static u16 readU16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

	static int openWAV(const char* tPath) {
		FILE* f = fopen(tPath, "rb");
		if (!f) return 0;
		u8 header[12];
		if (fread(header, 1, 12, f) != 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
			fclose(f);
			return 0;
		}
		int bits = 0, format = 0;
		for (;;) {
			u8 chunk[8];
			if (fread(chunk, 1, 8, f) != 8) break;
			const u32 length = readU32(chunk + 4);
			if (!memcmp(chunk, "fmt ", 4) && length >= 16) {
				u8 fmt[16];
				if (fread(fmt, 1, 16, f) != 16) break;
				format = readU16(fmt);
				gN3DSMusic.mChannels = readU16(fmt + 2);
				gN3DSMusic.mRate = readU32(fmt + 4);
				bits = readU16(fmt + 14);
				fseek(f, length - 16 + (length & 1), SEEK_CUR);
			}
			else if (!memcmp(chunk, "data", 4)) {
				if (format != 1 || bits != 16 || gN3DSMusic.mChannels < 1 || gN3DSMusic.mChannels > 2) break;
				gN3DSMusic.mWAVDataStart = ftell(f);
				gN3DSMusic.mWAVDataEnd = gN3DSMusic.mWAVDataStart + length;
				gN3DSMusic.mFile = f;
				gN3DSMusic.mType = DECODER_WAV;
				return 1;
			}
			else {
				fseek(f, length + (length & 1), SEEK_CUR);
			}
		}
		fclose(f);
		return 0;
	}

	static int openOGG(const char* tPath) {
		FILE* f = fopen(tPath, "rb");
		if (!f) return 0;
		if (ov_open(f, &gN3DSMusic.mOGG, NULL, 0)) {
			fclose(f);
			return 0;
		}
		vorbis_info* info = ov_info(&gN3DSMusic.mOGG, -1);
		if (!info || info->channels < 1 || info->channels > 2) {
			ov_clear(&gN3DSMusic.mOGG);
			return 0;
		}
		gN3DSMusic.mChannels = info->channels;
		gN3DSMusic.mRate = info->rate;
		gN3DSMusic.mFile = f;
		gN3DSMusic.mType = DECODER_OGG;
		return 1;
	}

	static int openMP3(const char* tPath) {
		if (!gN3DSMusic.mMP3) return 0;
		if (mpg123_open(gN3DSMusic.mMP3, tPath) != MPG123_OK) return 0;
		int channels = 0, encoding = 0;
		long rate = 0;
		if (mpg123_getformat(gN3DSMusic.mMP3, &rate, &channels, &encoding) != MPG123_OK || channels < 1 || channels > 2) {
			mpg123_close(gN3DSMusic.mMP3);
			return 0;
		}
		mpg123_format_none(gN3DSMusic.mMP3);
		mpg123_format(gN3DSMusic.mMP3, rate, channels, MPG123_ENC_SIGNED_16);
		gN3DSMusic.mChannels = channels;
		gN3DSMusic.mRate = rate;
		gN3DSMusic.mType = DECODER_MP3;
		return 1;
	}

	// Up to tFrames frames into tDst; 0 at the end.
	static int decode(s16* tDst, int tFrames) {
		const int bytes = tFrames * gN3DSMusic.mChannels * 2;
		int done = 0;
		while (done < bytes) {
			long got = 0;
			if (gN3DSMusic.mType == DECODER_MP3) {
				size_t amount = 0;
				const int ret = mpg123_read(gN3DSMusic.mMP3, (unsigned char*)tDst + done, bytes - done, &amount);
				got = (long)amount;
				if (ret != MPG123_OK && ret != MPG123_NEW_FORMAT && !got) break;
			}
			else if (gN3DSMusic.mType == DECODER_OGG) {
				int bitstream;
				got = ov_read(&gN3DSMusic.mOGG, (char*)tDst + done, bytes - done, &bitstream);
				if (got <= 0) break;
			}
			else if (gN3DSMusic.mType == DECODER_WAV) {
				long left = gN3DSMusic.mWAVDataEnd - ftell(gN3DSMusic.mFile);
				long want = bytes - done;
				if (want > left) want = left;
				if (want <= 0) break;
				got = (long)fread((char*)tDst + done, 1, want, gN3DSMusic.mFile);
				if (got <= 0) break;
			}
			else {
				break;
			}
			done += got;
		}
		return done / (gN3DSMusic.mChannels * 2);
	}

	static void rewindDecoder() {
		if (gN3DSMusic.mType == DECODER_MP3) mpg123_seek(gN3DSMusic.mMP3, 0, SEEK_SET);
		else if (gN3DSMusic.mType == DECODER_OGG) ov_raw_seek(&gN3DSMusic.mOGG, 0);
		else if (gN3DSMusic.mType == DECODER_WAV) fseek(gN3DSMusic.mFile, gN3DSMusic.mWAVDataStart, SEEK_SET);
	}

	// ==========================================================================
	// The DSP side.

	static void applyMix() {
		float mix[12] = { 0 };
		const float pan = gN3DSMusic.mPanning;   // -1 left, 0 center, 1 right
		mix[0] = gN3DSMusic.mVolume * (pan > 0 ? 1 - pan : 1);
		mix[1] = gN3DSMusic.mVolume * (pan < 0 ? 1 + pan : 1);
		ndspChnSetMix(MUSIC_CHANNEL, mix);
	}

	static void fillBuffers() {
		for (int i = 0; i < BUFFER_AMOUNT; i++) {
			ndspWaveBuf* buf = &gN3DSMusic.mBuffers[i];
			if (buf->status != NDSP_WBUF_FREE && buf->status != NDSP_WBUF_DONE) continue;
			if (buf->status == NDSP_WBUF_DONE) {
				gN3DSMusic.mFramesPlayed += buf->nsamples;
				buf->status = NDSP_WBUF_FREE;
			}
			if (!gN3DSMusic.mIsDecoding) continue;

			int frames = decode(gN3DSMusic.mBufferData[i], BUFFER_FRAMES);
			if (frames < BUFFER_FRAMES) {
				if (gN3DSMusic.mIsLooping) {
					rewindDecoder();
					frames += decode(gN3DSMusic.mBufferData[i] + frames * gN3DSMusic.mChannels, BUFFER_FRAMES - frames);
				}
				else {
					gN3DSMusic.mIsDecoding = 0;
				}
			}
			if (!frames) continue;

			buf->data_vaddr = gN3DSMusic.mBufferData[i];
			buf->nsamples = frames;
			DSP_FlushDataCache(buf->data_vaddr, frames * gN3DSMusic.mChannels * 2);
			ndspChnWaveBufAdd(MUSIC_CHANNEL, buf);
		}
	}

	static void musicThread(void*) {
		while (gN3DSMusic.mIsRunning) {
			LightEvent_Wait(&gN3DSMusic.mWake);
			LightLock_Lock(&gN3DSMusic.mLock);
			if (gN3DSMusic.mType != DECODER_NONE && !gN3DSMusic.mIsPaused) fillBuffers();
			LightLock_Unlock(&gN3DSMusic.mLock);
		}
	}

	static void ndspFrameCallback(void*) {
		LightEvent_Signal(&gN3DSMusic.mWake);
	}

	static void shutdownMusic() {
		if (!gN3DSMusic.mIsAvailable) return;
		stopMusic();
		gN3DSMusic.mIsRunning = 0;
		LightEvent_Signal(&gN3DSMusic.mWake);
		threadJoin(gN3DSMusic.mThread, U64_MAX);
		threadFree(gN3DSMusic.mThread);
		ndspSetCallback(NULL, NULL);
		if (gN3DSMusic.mMP3) mpg123_delete(gN3DSMusic.mMP3);
		mpg123_exit();
		for (int i = 0; i < BUFFER_AMOUNT; i++) linearFree(gN3DSMusic.mBufferData[i]);
		ndspExit();
		gN3DSMusic.mIsAvailable = 0;
	}

	void initSound() {
		if (gN3DSMusic.mIsAvailable) return;
		if (R_FAILED(ndspInit())) {
			logWarning("No music: ndspInit failed (is /3ds/dspfirm.cdc on the SD card?).");
			return;
		}

		LightLock_Init(&gN3DSMusic.mLock);
		LightEvent_Init(&gN3DSMusic.mWake, RESET_ONESHOT);
		mpg123_init();
		gN3DSMusic.mMP3 = mpg123_new(NULL, NULL);
		for (int i = 0; i < BUFFER_AMOUNT; i++) {
			gN3DSMusic.mBufferData[i] = (s16*)linearAlloc(BUFFER_FRAMES * 2 * sizeof(s16));
			memset(&gN3DSMusic.mBuffers[i], 0, sizeof(ndspWaveBuf));
		}

		gN3DSMusic.mIsRunning = 1;
		gN3DSMusic.mIsAvailable = 1;
		ndspSetCallback(ndspFrameCallback, NULL);

		// The third core when there is one (New 3DS), else this one.
		s32 priority = 0x30;
		svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
		gN3DSMusic.mThread = threadCreate(musicThread, NULL, 32 * 1024, priority - 1, 2, false);
		if (!gN3DSMusic.mThread) gN3DSMusic.mThread = threadCreate(musicThread, NULL, 32 * 1024, priority - 1, -2, false);

		atexit(shutdownMusic);
	}

	void shutdownSound() {}
	void updateSound() {}

	float getVolume() { return gN3DSMusic.mVolume; }

	void setVolume(float tVolume) {
		gN3DSMusic.mVolume = tVolume;
		if (gN3DSMusic.mIsAvailable) applyMix();
	}

	float getPanningValue() { return gN3DSMusic.mPanning; }

	void setPanningValue(float tPanning) {
		gN3DSMusic.mPanning = tPanning;
		if (gN3DSMusic.mIsAvailable) applyMix();
	}

	// CD tracks: none on the 3DS.
	void playTrack(int /*tTrack*/) {}
	void stopTrack() {}
	void pauseTrack() {}
	void resumeTrack() {}
	void playTrackOnce(int /*tTrack*/) {}

	static void startStreaming(const char* tPath, int tIsLooping) {
		if (!gN3DSMusic.mIsAvailable) return;
		stopStreamingMusicFile();

		char path[1024];
		getFullPath(path, tPath);
		const char* fileExt = getFileExtension(path);

		LightLock_Lock(&gN3DSMusic.mLock);
		int isOpen = 0;
		if (fileExt && !strcasecmp(fileExt, "ogg")) isOpen = openOGG(path);
		else if (fileExt && !strcasecmp(fileExt, "wav")) isOpen = openWAV(path);
		else isOpen = openMP3(path);

		if (isOpen) {
			ndspChnReset(MUSIC_CHANNEL);
			ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_LINEAR);
			ndspChnSetRate(MUSIC_CHANNEL, (float)gN3DSMusic.mRate);
			ndspChnSetFormat(MUSIC_CHANNEL, gN3DSMusic.mChannels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
			applyMix();
			for (int i = 0; i < BUFFER_AMOUNT; i++) gN3DSMusic.mBuffers[i].status = NDSP_WBUF_FREE;
			gN3DSMusic.mIsLooping = tIsLooping;
			gN3DSMusic.mIsDecoding = 1;
			gN3DSMusic.mIsPaused = 0;
			gN3DSMusic.mFramesPlayed = 0;
			fillBuffers();
			addN3DSPerfLine("  music %s: %ld Hz, %d channels", path, gN3DSMusic.mRate, gN3DSMusic.mChannels);
		}
		else {
			logWarningFormat("Unable to play the music %s.", path);
		}
		LightLock_Unlock(&gN3DSMusic.mLock);
	}

	void streamMusicFile(const char* tPath) { startStreaming(tPath, 1); }
	void streamMusicFileOnce(const char* tPath) { startStreaming(tPath, 0); }

	void stopStreamingMusicFile() {
		if (!gN3DSMusic.mIsAvailable) return;
		LightLock_Lock(&gN3DSMusic.mLock);
		ndspChnWaveBufClear(MUSIC_CHANNEL);
		closeDecoder();
		gN3DSMusic.mIsDecoding = 0;
		gN3DSMusic.mIsPaused = 0;
		LightLock_Unlock(&gN3DSMusic.mLock);
	}

	uint64_t getStreamingSoundTimeElapsedInMilliseconds() {
		if (!gN3DSMusic.mRate) return 0;
		return gN3DSMusic.mFramesPlayed * 1000 / gN3DSMusic.mRate;
	}

	int isPlayingStreamingMusic() {
		return gN3DSMusic.mType != DECODER_NONE && (gN3DSMusic.mIsDecoding || ndspChnIsPlaying(MUSIC_CHANNEL));
	}

	void stopMusic() { stopStreamingMusicFile(); }

	void pauseMusic() {
		if (!gN3DSMusic.mIsAvailable) return;
		gN3DSMusic.mIsPaused = 1;
		ndspChnSetPaused(MUSIC_CHANNEL, true);
	}

	void resumeMusic() {
		if (!gN3DSMusic.mIsAvailable) return;
		gN3DSMusic.mIsPaused = 0;
		ndspChnSetPaused(MUSIC_CHANNEL, false);
	}

	void crossFadeMusicLayer(const char* tNewPath, bool tIsLooping) {
		startStreaming(tNewPath, tIsLooping);
	}

	static void loadMicrophoneHandler(void* /*tData*/) {}
	static void unloadMicrophoneHandler(void* /*tData*/) {}
	static void updateMicrophoneHandler(void* /*tData*/) {}

	ActorBlueprint getMicrophoneHandlerActorBlueprint() {
		return makeActorBlueprint(loadMicrophoneHandler, unloadMicrophoneHandler, updateMicrophoneHandler);
	}

	float getMicrophoneVolume() { return 0.0; }

}
