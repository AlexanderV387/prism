#include "prism/soundeffect.h"

#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>

#include <3ds.h>

#include "prism/file.h"
#include "prism/log.h"

#include "prism3dsperf.h"

// [3DS] Sound effects on the DSP (ndsp, which needs /3ds/dspfirm.cdc: without
// it the game is silent). The WAVs are kept as PCM (8 or 16-bit) in linear memory,
// where the DSP reads them; prism's channels 1 to 23 are the DSP's 0 to 22
// (24, the DSP's 23, is the music's). Sounds on any channel take the free
// ones from the top, leaving the low ones to sounds on a given channel (a
// character's voice).

namespace prism {

	typedef struct {
		void* mData;
		u32 mSampleAmount;
		int mChannelAmount;
		int mBytesPerSample;
		u32 mRate;
	} N3DSSoundEffect;

	static const int CHANNEL_AMOUNT = 24;

	static struct {
		int mIsAvailable;
		float mVolume = 1.0;
		int mNextID = 0;
		std::map<int, N3DSSoundEffect> mSoundEffects;
		ndspWaveBuf mWaveBufs[CHANNEL_AMOUNT];
		void* mPlaying[CHANNEL_AMOUNT];
		float mChannelVolume[CHANNEL_AMOUNT];
		float mChannelPanning[CHANNEL_AMOUNT];
	} gN3DSSoundEffectData;

	void initSoundEffects() {
		gN3DSSoundEffectData.mIsAvailable = R_SUCCEEDED(ndspInit());
		if (!gN3DSSoundEffectData.mIsAvailable) {
			logWarning("No sound: ndspInit failed (is /3ds/dspfirm.cdc on the SD card?).");
			return;
		}
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		atexit(ndspExit);
	}

	void setupSoundEffectHandler() {}

	void shutdownSoundEffectHandler() {
		stopAllSoundEffects();
		for (auto& e : gN3DSSoundEffectData.mSoundEffects) linearFree(e.second.mData);
		gN3DSSoundEffectData.mSoundEffects.clear();
	}

	void setSoundEffectCompression(int /*tIsEnabled*/) {}

	// ==========================================================================
	// WAV (PCM, 8 or 16 bits, mono or stereo), kept as it is when there is
	// room: 8-bit sounds stay 8-bit (a boss's theme song as a sound: 10 MB,
	// not 21).

	static u32 readU32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
	static u16 readU16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

	typedef struct {
		int mChannels, mBits, mFormat;
		u32 mRate;
		size_t mDataStart;   // in the WAV
		u32 mDataLength;
	} WAVInfo;

	// The WAV's format and where its samples start, from its first tLength
	// bytes; 0 if those do not reach the samples.
	static int parseWAVHeader(const u8* d, size_t tLength, size_t tTotalLength, WAVInfo* oInfo) {
		if (tLength < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
		memset(oInfo, 0, sizeof(*oInfo));
		for (size_t pos = 12; pos + 8 <= tLength;) {
			const u32 chunkLength = readU32(d + pos + 4);
			const u8* chunk = d + pos + 8;
			if (!memcmp(d + pos, "fmt ", 4) && chunkLength >= 16 && pos + 8 + 16 <= tLength) {
				oInfo->mFormat = readU16(chunk);
				oInfo->mChannels = readU16(chunk + 2);
				oInfo->mRate = readU32(chunk + 4);
				oInfo->mBits = readU16(chunk + 14);
			}
			else if (!memcmp(d + pos, "data", 4)) {
				oInfo->mDataStart = pos + 8;
				oInfo->mDataLength = (u32)std::min<size_t>(chunkLength, tTotalLength - (pos + 8));
				return 1;
			}
			pos += 8 + chunkLength + (chunkLength & 1);
		}
		return 0;
	}

	static int isSupportedWAV(const WAVInfo& tInfo) {
		if (tInfo.mFormat != 1 || (tInfo.mBits != 8 && tInfo.mBits != 16) || tInfo.mChannels < 1 || tInfo.mChannels > 2 || !tInfo.mRate) {
			logWarningFormat("Unsupported WAV sound (format %d, %d bits, %d channels).", tInfo.mFormat, tInfo.mBits, tInfo.mChannels);
			return 0;
		}
		return 1;
	}

	// Linear memory kept for the sprites' textures: a character's sounds
	// (Modern Kitana's .snd is 54 MB) took all of it, the fighters drawn
	// with none; with 24 MB her textures (32 MB) were made again and again.
	// A sound that does not fit beside it is kept smaller: 8-bit, then
	// mono, then at half the rate (each halves it); left out if not.
	static const size_t SOUND_TEXTURE_RESERVE = 44 * 1024 * 1024;

	typedef struct {
		int mOutBytes;      // 1 or 2
		int mOutChannels;   // 1 or 2
		int mStep;          // 1 or 2 (half the rate)
	} SoundShape;

	static size_t getShapedSize(u32 tFrames, const SoundShape& tShape) {
		return size_t((tFrames + tShape.mStep - 1) / tShape.mStep) * tShape.mOutChannels * tShape.mOutBytes;
	}

	// A sound file's part of the linear memory left beside the reserve (the
	// first fighter's half, the second's all of what is left).
	static float gN3DSSoundFileShare = 1.0f;
	static size_t gN3DSSoundFileBudget = (size_t)-1;   // left for this file

	void setN3DSSoundFileShare(float tShare) { gN3DSSoundFileShare = tShare; }

	// How many halvings every sound of the file gets from the start (all a
	// bit worse rather than the last ones left out: Modern Kitana lost 103).
	static int gN3DSSoundFileHalvings;

	// The texture memory kept from a file's sounds: less for a small one
	// (the fight's own, fight.snd and common.snd, 157 KB together, are
	// loaded after both fighters' sounds: after New Kyouki's 6.4 MB they had
	// none left, the announcer silent, while 44 MB were kept for sprites).
	static const size_t SMALL_SOUND_FILE = 1024 * 1024;
	static const size_t SMALL_SOUND_FILE_TEXTURE_RESERVE = 8 * 1024 * 1024;
	static size_t gN3DSSoundTextureReserve = SOUND_TEXTURE_RESERVE;

	void startN3DSSoundFile(size_t tFileSize) {
		gN3DSSoundTextureReserve = tFileSize <= SMALL_SOUND_FILE ? SMALL_SOUND_FILE_TEXTURE_RESERVE : SOUND_TEXTURE_RESERVE;
		const size_t freeLinear = linearSpaceFree();
		const size_t forSounds = freeLinear > gN3DSSoundTextureReserve ? freeLinear - gN3DSSoundTextureReserve : 0;
		gN3DSSoundFileBudget = size_t(forSounds * gN3DSSoundFileShare);
		gN3DSSoundFileHalvings = 0;
		for (size_t size = tFileSize; size > gN3DSSoundFileBudget && gN3DSSoundFileHalvings < 3; size /= 2) gN3DSSoundFileHalvings++;
	}

	int getN3DSSoundFileHalvings() { return gN3DSSoundFileHalvings; }

	// One halving more of a sound's shape (8-bit, then mono, then half the
	// rate); 0 if there is none left.
	static int halveSoundShape(const WAVInfo& tInfo, SoundShape* oShape) {
		if (oShape->mOutBytes == 2) oShape->mOutBytes = 1;
		else if (oShape->mOutChannels == 2) oShape->mOutChannels = 1;
		else if (oShape->mStep == 1 && tInfo.mRate >= 16000) oShape->mStep = 2;
		else return 0;
		return 1;
	}

	void endN3DSSoundFile() {
		gN3DSSoundTextureReserve = SOUND_TEXTURE_RESERVE;
		gN3DSSoundFileBudget = (size_t)-1;
		gN3DSSoundFileShare = 1.0f;
		gN3DSSoundFileHalvings = 0;
	}

	// The shape a sound of tFrames is kept in, 0 if none fits.
	static int chooseSoundShape(const WAVInfo& tInfo, u32 tFrames, SoundShape* oShape) {
		*oShape = { tInfo.mBits / 8, tInfo.mChannels, 1 };
		for (int i = 0; i < gN3DSSoundFileHalvings; i++) halveSoundShape(tInfo, oShape);
		const size_t freeLinear = linearSpaceFree();
		for (;;) {
			const size_t size = getShapedSize(tFrames, *oShape);
			if (size + gN3DSSoundTextureReserve <= freeLinear && size <= gN3DSSoundFileBudget) return 1;
			if (!halveSoundShape(tInfo, oShape)) return 0;
		}
	}

	// Samples from the WAV's bytes to the shape kept: the DSP's 8-bit
	// samples are signed, WAV's are not.
	typedef struct {
		const WAVInfo* mInfo;
		SoundShape mShape;
		u8* mDst;
		u32 mFrame;          // input frames seen
		u8 mPartial[4];      // an input frame cut between two chunks
		int mPartialLength;
	} SoundConverter;

	static int readInputSample(const u8* p, int tBits) {
		return tBits == 8 ? (int(p[0]) - 128) << 8 : int(s16(p[0] | (p[1] << 8)));
	}

	static void convertFrame(SoundConverter* c, const u8* tFrame) {
		const int inBytes = c->mInfo->mBits / 8;
		if (c->mFrame++ & (c->mShape.mStep - 1)) return;   // a step of 1 or 2
		int values[2];
		values[0] = readInputSample(tFrame, c->mInfo->mBits);
		values[1] = c->mInfo->mChannels == 2 ? readInputSample(tFrame + inBytes, c->mInfo->mBits) : values[0];
		if (c->mShape.mOutChannels == 1 && c->mInfo->mChannels == 2) values[0] = (values[0] + values[1]) / 2;
		for (int ch = 0; ch < c->mShape.mOutChannels; ch++) {
			if (c->mShape.mOutBytes == 1) *c->mDst++ = u8(s8(values[ch] >> 8));
			else {
				const s16 v = s16(values[ch]);
				memcpy(c->mDst, &v, 2);
				c->mDst += 2;
			}
		}
	}

	static void convertBytes(SoundConverter* c, const u8* tSrc, size_t tLength) {
		const int frameBytes = c->mInfo->mBits / 8 * c->mInfo->mChannels;
		// Kept as it is (most sounds): the bytes copied, the 8-bit ones made
		// signed, the same as frame by frame (Starfire's 6.9 MB took 2.6 s so).
		if (c->mShape.mStep == 1 && c->mShape.mOutChannels == c->mInfo->mChannels && c->mShape.mOutBytes * 8 == c->mInfo->mBits) {
			while (tLength && c->mPartialLength) {
				c->mPartial[c->mPartialLength++] = *tSrc++;
				tLength--;
				if (c->mPartialLength == frameBytes) {
					convertFrame(c, c->mPartial);
					c->mPartialLength = 0;
				}
			}
			const size_t whole = tLength - tLength % frameBytes;
			if (c->mShape.mOutBytes == 2) {
				memcpy(c->mDst, tSrc, whole);
			}
			else {
				for (size_t i = 0; i < whole; i++) c->mDst[i] = tSrc[i] ^ 0x80;
			}
			c->mDst += whole;
			c->mFrame += u32(whole / frameBytes);
			tSrc += whole;
			tLength -= whole;
			while (tLength--) c->mPartial[c->mPartialLength++] = *tSrc++;
			return;
		}
		while (tLength && c->mPartialLength) {
			c->mPartial[c->mPartialLength++] = *tSrc++;
			tLength--;
			if (c->mPartialLength == frameBytes) {
				convertFrame(c, c->mPartial);
				c->mPartialLength = 0;
			}
		}
		for (; tLength >= (size_t)frameBytes; tLength -= frameBytes, tSrc += frameBytes) convertFrame(c, tSrc);
		while (tLength--) c->mPartial[c->mPartialLength++] = *tSrc++;
	}

	static int addSoundEffect(void* tData, u32 tFrames, const WAVInfo& tInfo, const SoundShape& tShape) {
		DSP_FlushDataCache(tData, getShapedSize(tFrames, tShape));
		N3DSSoundEffect e;
		e.mData = tData;
		e.mSampleAmount = (tFrames + tShape.mStep - 1) / tShape.mStep;
		e.mChannelAmount = tShape.mOutChannels;
		e.mBytesPerSample = tShape.mOutBytes;
		e.mRate = tInfo.mRate / tShape.mStep;
		const int id = gN3DSSoundEffectData.mNextID++;
		gN3DSSoundEffectData.mSoundEffects[id] = e;
		return id;
	}

	static int gN3DSSoundsShrunk, gN3DSSoundsLeftOut;

	// Room for a sound of tFrames: its shape and memory, NULL if none.
	static u8* allocateSound(const WAVInfo& tInfo, u32 tFrames, SoundShape* oShape) {
		if (!chooseSoundShape(tInfo, tFrames, oShape)) {
			if (!gN3DSSoundsLeftOut++) {
				logWarning("Out of linear memory for a sound: left out (sounds keep 44 MB for the sprites).");
			}
			return NULL;
		}
		if (oShape->mOutBytes * 8 != tInfo.mBits || oShape->mOutChannels != tInfo.mChannels || oShape->mStep != 1) gN3DSSoundsShrunk++;
		const size_t size = getShapedSize(tFrames, *oShape);
		u8* samples = (u8*)linearAlloc(size);
		if (!samples) {
			logWarning("Out of linear memory for a sound.");
		}
		else if (gN3DSSoundFileBudget != (size_t)-1) {
			gN3DSSoundFileBudget -= std::min(size, gN3DSSoundFileBudget);
		}
		return samples;
	}

	static int loadSoundEffectFromBufferUncounted(const Buffer& tBuffer) {
		WAVInfo info;
		if (!parseWAVHeader((const u8*)tBuffer.mData, tBuffer.mLength, tBuffer.mLength, &info)) {
			logWarning("Not a WAV sound.");
			return -1;
		}
		if (!isSupportedWAV(info)) return -1;
		const u32 frames = info.mDataLength / (info.mBits / 8 * info.mChannels);
		if (!frames) return -1;
		SoundShape shape;
		u8* samples = allocateSound(info, frames, &shape);
		if (!samples) return -1;
		SoundConverter c = { &info, shape, samples, 0, {0}, 0 };
		convertBytes(&c, (const u8*)tBuffer.mData + info.mDataStart, size_t(frames) * (info.mBits / 8 * info.mChannels));
		return addSoundEffect(samples, frames, info, shape);
	}

	int loadSoundEffectFromBuffer(const Buffer& tBuffer) {
		const uint64_t start = getN3DSPerfTicks();
		const int ret = loadSoundEffectFromBufferUncounted(tBuffer);
		gN3DSPerf.mSoundTicks += getN3DSPerfTicks() - start;
		gN3DSPerf.mSounds++;
		return ret;
	}

	// A WAV of tLength bytes read by tRead in pieces straight into linear
	// memory (a big one in the heap first did not fit there next to a
	// boss's sprites); -2 if its header is longer than the start read (then
	// read it whole).
	int loadN3DSSoundEffectFromReader(void (*tRead)(void* tCaller, void* tDst, uint32_t tSize), void* tCaller, uint32_t tLength) {
		const uint64_t startTicks = getN3DSPerfTicks();
		u8 header[1024];
		const uint32_t headerLength = std::min<uint32_t>(tLength, sizeof(header));
		tRead(tCaller, header, headerLength);
		WAVInfo info;
		if (!parseWAVHeader(header, headerLength, tLength, &info)) {
			if (headerLength == tLength) {
				logWarning("Not a WAV sound.");
				return -1;
			}
			return -2;
		}
		if (!isSupportedWAV(info)) return -1;
		const u32 frames = info.mDataLength / (info.mBits / 8 * info.mChannels);
		if (!frames) return -1;
		SoundShape shape;
		u8* samples = allocateSound(info, frames, &shape);
		if (!samples) return -1;
		SoundConverter c = { &info, shape, samples, 0, {0}, 0 };
		size_t left = size_t(frames) * (info.mBits / 8 * info.mChannels);
		const size_t inHeader = std::min<size_t>(left, headerLength - info.mDataStart);
		convertBytes(&c, header + info.mDataStart, inHeader);
		left -= inHeader;
		static u8 chunk[16 * 1024];
		while (left) {
			const uint32_t amount = (uint32_t)std::min<size_t>(left, sizeof(chunk));
			tRead(tCaller, chunk, amount);
			convertBytes(&c, chunk, amount);
			left -= amount;
		}
		const int ret = addSoundEffect(samples, frames, info, shape);
		gN3DSPerf.mSoundTicks += getN3DSPerfTicks() - startTicks;
		gN3DSPerf.mSounds++;
		return ret;
	}

	int getN3DSSoundsShrunk() { return gN3DSSoundsShrunk; }
	int getN3DSSoundsLeftOut() { return gN3DSSoundsLeftOut; }

	int loadSoundEffect(const char* tPath) {
		Buffer b = fileToBuffer(tPath);
		const int ret = loadSoundEffectFromBuffer(b);
		freeBuffer(b);
		return ret;
	}

	void unloadSoundEffect(int tID) {
		auto it = gN3DSSoundEffectData.mSoundEffects.find(tID);
		if (it == gN3DSSoundEffectData.mSoundEffects.end()) return;

		for (int i = 1; i < CHANNEL_AMOUNT; i++) {
			if (gN3DSSoundEffectData.mPlaying[i] == it->second.mData) stopSoundEffect(i);
		}
		linearFree(it->second.mData);
		gN3DSSoundEffectData.mSoundEffects.erase(it);
	}

	// ==========================================================================
	// Playing.

	static int toDSPChannel(int tChannel) { return tChannel - 1; }

	static void applyChannelMix(int tChannel) {
		const float volume = gN3DSSoundEffectData.mChannelVolume[tChannel];
		// -1 left, 0 center, 1 right (as prism's other backends: SoLoud's
		// pan); the far side kept at half at most, the 3DS's speakers being
		// that close together (the far side silent sounded lopsided).
		const float panning = gN3DSSoundEffectData.mChannelPanning[tChannel];
		static const float PANNING_WIDTH = 0.5f;
		// At most full volume: louder (a character's volume = 255 gave 3.5
		// times) clipped into a buzz.
		const float level = std::min(volume, 1.0f);
		float mix[12] = { 0 };
		mix[0] = level * std::min(1.0f, 1 - panning * PANNING_WIDTH);
		mix[1] = level * std::min(1.0f, 1 + panning * PANNING_WIDTH);
		ndspChnSetMix(toDSPChannel(tChannel), mix);
	}

	// Playing, or queued and not started yet: the DSP reports a channel as
	// playing only from its next frame, so two sounds on any free channel
	// in the same tick (a KO voice and the announcer's "KO", a hit's sound
	// and a voice) took the same channel, and the second cut the first.
	int isSoundEffectPlayingOnChannel(int tChannel) {
		if (!gN3DSSoundEffectData.mIsAvailable || tChannel < 1 || tChannel >= CHANNEL_AMOUNT) return 0;
		const u8 status = gN3DSSoundEffectData.mWaveBufs[tChannel].status;
		return ndspChnIsPlaying(toDSPChannel(tChannel)) || status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING;
	}

	static int findEmptyChannel() {
		for (int i = CHANNEL_AMOUNT - 1; i >= 1; i--) {
			if (!isSoundEffectPlayingOnChannel(i)) return i;
		}
		return -1;
	}

	int playSoundEffectChannel(int tID, int tChannel, float tVolume, float tFreqMul, int tIsLooping) {
		if (!gN3DSSoundEffectData.mIsAvailable) return -1;
		auto it = gN3DSSoundEffectData.mSoundEffects.find(tID);
		if (it == gN3DSSoundEffectData.mSoundEffects.end()) return -1;
		const N3DSSoundEffect& e = it->second;

		if (tChannel == -1) {
			tChannel = findEmptyChannel();
			if (tChannel == -1) return -1;
		}
		if (tChannel < 1 || tChannel >= CHANNEL_AMOUNT) return -1;

		const int dsp = toDSPChannel(tChannel);
		ndspChnWaveBufClear(dsp);
		ndspChnReset(dsp);
		ndspChnSetInterp(dsp, NDSP_INTERP_LINEAR);
		ndspChnSetRate(dsp, e.mRate * (tFreqMul > 0 ? tFreqMul : 1.0f));
		if (e.mBytesPerSample == 1) ndspChnSetFormat(dsp, e.mChannelAmount == 2 ? NDSP_FORMAT_STEREO_PCM8 : NDSP_FORMAT_MONO_PCM8);
		else ndspChnSetFormat(dsp, e.mChannelAmount == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);

		gN3DSSoundEffectData.mChannelVolume[tChannel] = tVolume;
		gN3DSSoundEffectData.mChannelPanning[tChannel] = 0;
		applyChannelMix(tChannel);

		ndspWaveBuf* buf = &gN3DSSoundEffectData.mWaveBufs[tChannel];
		memset(buf, 0, sizeof(*buf));
		buf->data_vaddr = e.mData;
		buf->nsamples = e.mSampleAmount;
		buf->looping = tIsLooping ? true : false;
		gN3DSSoundEffectData.mPlaying[tChannel] = e.mData;
		ndspChnWaveBufAdd(dsp, buf);

		// For the load log: the sounds of a screen's first 5 seconds.
		static uint32_t loggedOnScreen = 0, lastFrames = 0xFFFFFFFF;
		const uint32_t frames = getN3DSPerfFramesShown();
		if (frames < lastFrames) loggedOnScreen = 0;
		lastFrames = frames;
		if (frames < 300 && loggedOnScreen < 40) {
			loggedOnScreen++;
			addN3DSPerfLine("  sound %d at frame %u: channel %d, %u samples, %d channels, %u Hz (x%.2f), volume %.2f%s",
				tID, (unsigned)frames, tChannel, (unsigned)e.mSampleAmount, e.mChannelAmount, (unsigned)e.mRate, tFreqMul, tVolume, tIsLooping ? ", looping" : "");
		}
		return tChannel;
	}

	int playSoundEffect(int tID) {
		return playSoundEffectChannel(tID, -1, getSoundEffectVolume());
	}

	int playSoundEffectFile(const std::string& tPath) {
		const int id = loadSoundEffect(tPath.c_str());
		if (id == -1) return -1;
		return playSoundEffect(id);
	}

	void stopSoundEffect(int tChannel) {
		if (!gN3DSSoundEffectData.mIsAvailable || tChannel < 1 || tChannel >= CHANNEL_AMOUNT) return;
		ndspChnWaveBufClear(toDSPChannel(tChannel));
		gN3DSSoundEffectData.mWaveBufs[tChannel].status = NDSP_WBUF_DONE;
		gN3DSSoundEffectData.mPlaying[tChannel] = NULL;
	}

	void stopAllSoundEffects() {
		for (int i = 1; i < CHANNEL_AMOUNT; i++) stopSoundEffect(i);
	}

	void panSoundEffect(int tChannel, float tPanning) {
		if (!gN3DSSoundEffectData.mIsAvailable || tChannel < 1 || tChannel >= CHANNEL_AMOUNT) return;
		gN3DSSoundEffectData.mChannelPanning[tChannel] = std::min(std::max(tPanning, -1.0f), 1.0f);
		applyChannelMix(tChannel);
	}

	float getSoundEffectVolume() { return gN3DSSoundEffectData.mVolume; }
	void setSoundEffectVolume(float tVolume) { gN3DSSoundEffectData.mVolume = tVolume; }

}
