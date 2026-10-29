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
// it the game is silent). The WAVs are kept as 16-bit PCM in linear memory,
// where the DSP reads them; prism's channels 1 to 23 are the DSP's 0 to 22
// (24, the DSP's 23, is the music's). Sounds on any channel take the free
// ones from the top, leaving the low ones to sounds on a given channel (a
// character's voice).

namespace prism {

	typedef struct {
		s16* mData;
		u32 mSampleAmount;
		int mChannelAmount;
		u32 mRate;
	} N3DSSoundEffect;

	static const int CHANNEL_AMOUNT = 24;

	static struct {
		int mIsAvailable;
		float mVolume = 1.0;
		int mNextID = 0;
		std::map<int, N3DSSoundEffect> mSoundEffects;
		ndspWaveBuf mWaveBufs[CHANNEL_AMOUNT];
		s16* mPlaying[CHANNEL_AMOUNT];
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
	// WAV (PCM, 8 or 16 bits, mono or stereo) to 16-bit samples.

	static u32 readU32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
	static u16 readU16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

	static int loadSoundEffectFromBufferUncounted(const Buffer& tBuffer);

	int loadSoundEffectFromBuffer(const Buffer& tBuffer) {
		const uint64_t start = getN3DSPerfTicks();
		const int ret = loadSoundEffectFromBufferUncounted(tBuffer);
		gN3DSPerf.mSoundTicks += getN3DSPerfTicks() - start;
		gN3DSPerf.mSounds++;
		return ret;
	}

	static int loadSoundEffectFromBufferUncounted(const Buffer& tBuffer) {
		const u8* d = (const u8*)tBuffer.mData;
		const size_t length = tBuffer.mLength;
		if (length < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) {
			logWarning("Not a WAV sound.");
			return -1;
		}

		int channels = 0, bits = 0, format = 0;
		u32 rate = 0;
		const u8* data = NULL;
		u32 dataLength = 0;
		for (size_t pos = 12; pos + 8 <= length;) {
			const u32 chunkLength = readU32(d + pos + 4);
			const u8* chunk = d + pos + 8;
			if (!memcmp(d + pos, "fmt ", 4) && chunkLength >= 16) {
				format = readU16(chunk);
				channels = readU16(chunk + 2);
				rate = readU32(chunk + 4);
				bits = readU16(chunk + 14);
			}
			else if (!memcmp(d + pos, "data", 4)) {
				data = chunk;
				dataLength = (u32)std::min<size_t>(chunkLength, length - (pos + 8));
				break;
			}
			pos += 8 + chunkLength + (chunkLength & 1);
		}

		if (!data || format != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2 || !rate) {
			logWarningFormat("Unsupported WAV sound (format %d, %d bits, %d channels).", format, bits, channels);
			return -1;
		}

		const u32 sampleAmount = dataLength / (bits / 8) / channels;
		if (!sampleAmount) return -1;
		s16* samples = (s16*)linearAlloc(sampleAmount * channels * sizeof(s16));
		if (!samples) {
			logWarning("Out of linear memory for a sound.");
			return -1;
		}

		if (bits == 16) {
			memcpy(samples, data, sampleAmount * channels * sizeof(s16));
		}
		else {
			for (u32 i = 0; i < sampleAmount * channels; i++) samples[i] = (s16)((data[i] - 128) << 8);
		}
		DSP_FlushDataCache(samples, sampleAmount * channels * sizeof(s16));

		N3DSSoundEffect e;
		e.mData = samples;
		e.mSampleAmount = sampleAmount;
		e.mChannelAmount = channels;
		e.mRate = rate;
		const int id = gN3DSSoundEffectData.mNextID++;
		gN3DSSoundEffectData.mSoundEffects[id] = e;
		return id;
	}

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
		float mix[12] = { 0 };
		mix[0] = volume * std::min(1.0f, 1 - panning * PANNING_WIDTH);
		mix[1] = volume * std::min(1.0f, 1 + panning * PANNING_WIDTH);
		ndspChnSetMix(toDSPChannel(tChannel), mix);
	}

	int isSoundEffectPlayingOnChannel(int tChannel) {
		if (!gN3DSSoundEffectData.mIsAvailable || tChannel < 1 || tChannel >= CHANNEL_AMOUNT) return 0;
		return ndspChnIsPlaying(toDSPChannel(tChannel));
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
		ndspChnSetFormat(dsp, e.mChannelAmount == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);

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
