#include "prism/mugensoundfilereader.h"

#include <assert.h>
#include <algorithm>
#include <stdio.h>

#include <prism/file.h>
#include <prism/filereader.h>
#include <prism/memoryhandler.h>
#include <prism/soundeffect.h>
#include <prism/log.h>
#include <prism/system.h>
#include <prism/profiling.h>
#ifdef __3DS__
#include "3ds/prism3ds.h"
#include "3ds/prism3dsperf.h"
#endif

namespace prism {

	typedef struct {
		uint32_t mNextFileOffset;
		uint32_t mSubfileLength;

		uint32_t mGroupNumber;
		uint32_t mSampleNumber;
	} MugenSoundFileSubFileHeader;

	typedef struct {
		char mSignature[12];
		uint16_t mVersionLow;
		uint16_t mVersionHigh;

		uint32_t mNumberOfSounds;
		uint32_t mFirstSubFileOffset;

		char mComments[488];
	} MugenSoundFileHeader;

	static struct {
		FileReader mFileReader;
#ifdef __3DS__
		char mFullPath[1024];   // the file's, for the sounds read while they play
#endif
	} gMugenSoundFileReaderData;

#ifdef __3DS__
	// [3DS] Longer than this, a sound is read from the file while it plays
	// instead of kept in the DSP's (linear) memory: the music inside New
	// Kyouki's and Onslaught's .snd took some 20 MB of it, their sprites
	// made again and again meanwhile.
	static const uint32_t N3DS_STREAMED_SOUND_SIZE = 1024 * 1024;
#endif

	static void addSoundGroup(MugenSounds* tSounds, int tGroupNumber) {
		MugenSoundGroup* e = (MugenSoundGroup*)allocMemory(sizeof(MugenSoundGroup));
		e->mSamples = new_int_map();
		int_map_push_owned(&tSounds->mGroups, tGroupNumber, e);
	}

	static int loadSingleMugenSoundSubfileAndReturnIfOver(MugenSounds* tSounds) {
		MugenSoundFileSubFileHeader subHeader;
		gMugenSoundFileReaderData.mFileReader.mRead(&gMugenSoundFileReaderData.mFileReader, &subHeader, sizeof(MugenSoundFileSubFileHeader));

		MugenSoundGroup* group;
		if (!int_map_contains(&tSounds->mGroups, subHeader.mGroupNumber)) {
			addSoundGroup(tSounds, subHeader.mGroupNumber);
		}
		group = (MugenSoundGroup*)int_map_get(&tSounds->mGroups, subHeader.mGroupNumber);
		MugenSoundSample* e = (MugenSoundSample*)allocMemory(sizeof(MugenSoundSample));
#ifdef __3DS__
		int isStreamed = 0;
		if (subHeader.mSubfileLength > N3DS_STREAMED_SOUND_SIZE && gMugenSoundFileReaderData.mFullPath[0]) {
			const uint32_t start = gMugenSoundFileReaderData.mFileReader.mGetCurrentOffset(&gMugenSoundFileReaderData.mFileReader);
			e->mSoundEffectID = loadN3DSStreamedSoundEffect(gMugenSoundFileReaderData.mFullPath, start, subHeader.mSubfileLength);
			isStreamed = e->mSoundEffectID != -2;
		}
		if (isStreamed) {}
		else
#endif
		if (unsigned(getAvailableSoundMemory()) >= subHeader.mSubfileLength * 2) {
#ifdef __3DS__
			// [3DS] A big one straight into the DSP's memory, not through the heap.
			e->mSoundEffectID = -2;
			if (subHeader.mSubfileLength > 64 * 1024) {
				const uint32_t start = gMugenSoundFileReaderData.mFileReader.mGetCurrentOffset(&gMugenSoundFileReaderData.mFileReader);
				e->mSoundEffectID = loadN3DSSoundEffectFromReader([](void* tCaller, void* tDst, uint32_t tSize) {
					((FileReader*)tCaller)->mRead((FileReader*)tCaller, tDst, tSize);
				}, &gMugenSoundFileReaderData.mFileReader, subHeader.mSubfileLength);
				if (e->mSoundEffectID == -2) gMugenSoundFileReaderData.mFileReader.mSeek(&gMugenSoundFileReaderData.mFileReader, start);
			}
			if (e->mSoundEffectID == -2)
#endif
			{
				Buffer waveFileBuffer = gMugenSoundFileReaderData.mFileReader.mReadBufferReadOnly(&gMugenSoundFileReaderData.mFileReader, subHeader.mSubfileLength);
				e->mSoundEffectID = loadSoundEffectFromBuffer(waveFileBuffer);
				freeBuffer(waveFileBuffer);
			}
		}
		else {
			logWarningFormat("Early out for sound effect %d %d, does not fit in sound memory.", subHeader.mGroupNumber, subHeader.mSampleNumber);
			e->mSoundEffectID = -1;
		}

		int_map_push_owned(&group->mSamples, subHeader.mSampleNumber, e);

#ifndef __3DS__
		if (e->mSoundEffectID == -1) {
			return 1;
		}
#endif
		// [3DS] One left out (no room, or not a WAV) does not end the others.

		if (subHeader.mNextFileOffset != 0) {
			gMugenSoundFileReaderData.mFileReader.mSeek(&gMugenSoundFileReaderData.mFileReader, subHeader.mNextFileOffset);
		}
		return 0;
	}

	static MugenSounds loadMugenSoundFileWithReader() {
		MugenSounds ret;
		ret.mGroups = new_int_map();

		MugenSoundFileHeader header;
		gMugenSoundFileReaderData.mFileReader.mRead(&gMugenSoundFileReaderData.mFileReader, &header, sizeof(MugenSoundFileHeader));

		gMugenSoundFileReaderData.mFileReader.mSeek(&gMugenSoundFileReaderData.mFileReader, header.mFirstSubFileOffset);

		int i;
		for (i = 0; i < (int)header.mNumberOfSounds; i++) {
			if (loadSingleMugenSoundSubfileAndReturnIfOver(&ret)) {
				break;
			}
		}

		return ret;
	}

	static void initMugenSoundFileReader() {
		gMugenSoundFileReaderData.mFileReader = getFileFileReader();
	}

#ifdef __3DS__
	// [3DS] The file's size without its sounds read while they play: what
	// its other sounds share (counted with them, all of them were kept
	// smaller than needed).
	static size_t getN3DSSoundFileSizeKeptInMemory(const char* tPath) {
		size_t size = (size_t)getFileSize(tPath);
		char path[1024];
		getFullPath(path, tPath);
		FILE* file = fopen(path, "rb");
		if (!file) return size;
		MugenSoundFileHeader header;
		if (fread(&header, sizeof(header), 1, file) == 1) {
			uint32_t offset = header.mFirstSubFileOffset;
			for (uint32_t i = 0; i < header.mNumberOfSounds && offset; i++) {
				MugenSoundFileSubFileHeader subHeader;
				if (fseek(file, offset, SEEK_SET) || fread(&subHeader, sizeof(subHeader), 1, file) != 1) break;
				if (subHeader.mSubfileLength > N3DS_STREAMED_SOUND_SIZE) size -= std::min<size_t>(size, subHeader.mSubfileLength);
				if (subHeader.mNextFileOffset <= offset) break;
				offset = subHeader.mNextFileOffset;
			}
		}
		fclose(file);
		return size;
	}
#endif

	MugenSounds loadMugenSoundFile(const char* tPath)
	{
		initMugenSoundFileReader();
		gMugenSoundFileReaderData.mFileReader.mInit(&gMugenSoundFileReaderData.mFileReader, tPath);
#ifdef __3DS__
		getFullPath(gMugenSoundFileReaderData.mFullPath, tPath);
		const int streamedBefore = getN3DSSoundsStreamed();
		const int shrunkBefore = getN3DSSoundsShrunk(), leftOutBefore = getN3DSSoundsLeftOut();
		const uint32_t linearBefore = linearSpaceFree();
		startN3DSSoundFile(getN3DSSoundFileSizeKeptInMemory(tPath));
		const int halvings = getN3DSSoundFileHalvings();
#endif
		MugenSounds ret = loadMugenSoundFileWithReader();
#ifdef __3DS__
		endN3DSSoundFile();
		addN3DSPerfLine("  sounds %s: %u KB of linear memory, %d kept smaller (8-bit, mono or half the rate; %d halvings for all), %d left out, %d read from the SD card when played", tPath,
			(unsigned)((linearBefore - linearSpaceFree()) / 1024), getN3DSSoundsShrunk() - shrunkBefore, halvings, getN3DSSoundsLeftOut() - leftOutBefore, getN3DSSoundsStreamed() - streamedBefore);
		gMugenSoundFileReaderData.mFullPath[0] = '\0';
#endif
		gMugenSoundFileReaderData.mFileReader.mDelete(&gMugenSoundFileReaderData.mFileReader);

		return ret;
	}

	static int unloadMugenSoundFileSample(void* tCaller, void* tData) {
		(void)tCaller;
		MugenSoundSample* e = (MugenSoundSample*)tData;
		unloadSoundEffect(e->mSoundEffectID);
		return 1;
	}

	static int unloadMugenSoundFileGroup(void* tCaller, void* tData) {
		(void)tCaller;
		MugenSoundGroup* e = (MugenSoundGroup*)tData;

		int_map_remove_predicate(&e->mSamples, unloadMugenSoundFileSample, NULL);
		delete_int_map(&e->mSamples);

		return 1;
	}

	void unloadMugenSoundFile(MugenSounds* tSounds)
	{
		int_map_remove_predicate(&tSounds->mGroups, unloadMugenSoundFileGroup, NULL);
		delete_int_map(&tSounds->mGroups);
	}

	MugenSounds createEmptyMugenSoundFile() {
		MugenSounds ret;
		ret.mGroups = new_int_map();
		return ret;
	}

	static MugenSoundSample* getMugenSoundSample(MugenSounds* tSounds, int tGroup, int tSample) {
		assert(int_map_contains(&tSounds->mGroups, tGroup));
		MugenSoundGroup* group = (MugenSoundGroup*)int_map_get(&tSounds->mGroups, tGroup);

		assert(int_map_contains(&group->mSamples, tSample));
		return (MugenSoundSample*)int_map_get(&group->mSamples, tSample);
	}

	int playMugenSound(MugenSounds* tSounds, int tGroup, int tSample)
	{
		const auto sample = getMugenSoundSample(tSounds, tGroup, tSample);
		return playSoundEffect(sample->mSoundEffectID);
	}

	int playMugenSoundAdvanced(MugenSounds* tSounds, int tGroup, int tSample, float tVolume, int tChannel, float tFrequencyMultiplier, int tIsLooping, float tPanning)
	{
		setProfilingSectionMarkerCurrentFunction();
		const auto sample = getMugenSoundSample(tSounds, tGroup, tSample);
		int channel = playSoundEffectChannel(sample->mSoundEffectID, tChannel, tVolume, tFrequencyMultiplier, tIsLooping);
		panSoundEffect(channel, tPanning);
		return channel;
	}

	int tryPlayMugenSound(MugenSounds* tSounds, int tGroup, int tSample)
	{
		if (!hasMugenSound(tSounds, tGroup, tSample)) {
			return 0;
		}

		playMugenSound(tSounds, tGroup, tSample);
		return 1;
	}

	int tryPlayMugenSoundAdvanced(MugenSounds* tSounds, int tGroup, int tSample, float tVolume, int tChannel, float tFrequencyMultiplier, int tIsLooping, float tPanning)
	{
		setProfilingSectionMarkerCurrentFunction();
		if (!hasMugenSound(tSounds, tGroup, tSample)) {
			return 0;
		}

		playMugenSoundAdvanced(tSounds, tGroup, tSample, tVolume, tChannel, tFrequencyMultiplier, tIsLooping, tPanning);
		return 1;
	}

	int hasMugenSound(MugenSounds* tSounds, int tGroup, int tSample)
	{
		if (!int_map_contains(&tSounds->mGroups, tGroup)) return 0;
		MugenSoundGroup* group = (MugenSoundGroup*)int_map_get(&tSounds->mGroups, tGroup);

		return int_map_contains(&group->mSamples, tSample);
	}

}