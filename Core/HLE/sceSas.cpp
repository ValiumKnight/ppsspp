// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// SAS is a software mixing engine that runs on the Media Engine CPU. We just HLE it.
// This is a very rough implementation that needs lots of work.
//
// This file just contains the API, the real stuff is in HW/SasAudio.cpp/h.
//
// JPCSP is, as it often is, a pretty good reference although I didn't actually use it much yet:
// http://code.google.com/p/jpcsp/source/browse/trunk/src/jpcsp/HLE/modules150/sceSasCore.java

#include "base/basictypes.h"
#include "Log.h"
#include "HLE.h"
#include "../MIPS/MIPS.h"
#include "../HW/SasAudio.h"

#include "sceSas.h"
#include "sceKernel.h"

enum {
	ERROR_SAS_INVALID_VOICE = 0x80420010,
	ERROR_SAS_INVALID_ADSR_CURVE_MODE = 0x80420013,
	ERROR_SAS_INVALID_PARAMETER = 0x80420014,
	ERROR_SAS_VOICE_PAUSED = 0x80420016,
	ERROR_SAS_INVALID_SIZE = 0x8042001A,
	ERROR_SAS_BUSY = 0x80420030,
	ERROR_SAS_NOT_INIT = 0x80420100,
};

// TODO - allow more than one, associating each with one Core pointer (passed in to all the functions)
// No known games use more than one instance of Sas though.
SasInstance *sas;

void __SasInit() {
	sas = new SasInstance();
}

void __SasDoState(PointerWrap &p) {
	if (sas != NULL) {
		sas->DoState(p);
	}
	p.DoMarker("sceSas");
}

void __SasShutdown() {
	delete sas;
	sas = 0;
}


u32 sceSasInit(u32 core, u32 grainSize, u32 maxVoices, u32 outputMode, u32 sampleRate)
{
	INFO_LOG(HLE,"0=sceSasInit(%08x, grainSize=%i, maxVoices=%i, %i, %i)", core, grainSize, maxVoices, outputMode, sampleRate);
	sas->SetGrainSize(grainSize);
	sas->maxVoices = maxVoices;
	sas->sampleRate = sampleRate;
	sas->outputMode = outputMode;
	for (int i = 0; i < 32; i++) {
		sas->voices[i].playing = false;
	}
	return 0;
}

u32 sceSasGetEndFlag(u32 core)
{
	u32 endFlag = 0;
	for (int i = 0; i < sas->maxVoices; i++) {
		if (!sas->voices[i].playing)
			endFlag |= 1 << i;
	}
	DEBUG_LOG(HLE,"%08x=sceSasGetEndFlag()", endFlag);
	return endFlag;
}

// Runs the mixer
u32 _sceSasCore(u32 core, u32 outAddr)
{
	DEBUG_LOG(HLE,"0=sceSasCore(, %08x)	(grain: %i samples)", outAddr, sas->GetGrainSize());
	if (!Memory::IsValidAddress(outAddr)) {
		return ERROR_SAS_INVALID_PARAMETER;
	}
	Memory::Memset(outAddr, 0, sas->GetGrainSize() * 2 * 2);
	sas->Mix(outAddr);
	return 0;
}

// Another way of running the mixer, what was the difference again?
u32 _sceSasCoreWithMix(u32 core, u32 outAddr)
{
	DEBUG_LOG(HLE,"0=sceSasCoreWithMix(, %08x) (grain: %i samples)", outAddr, sas->GetGrainSize());
	if (!Memory::IsValidAddress(outAddr)) {
		return ERROR_SAS_INVALID_PARAMETER;
	}
	sas->Mix(outAddr);
	return 0;
}

u32 sceSasSetVoice(u32 core, int voiceNum, u32 vagAddr, int size, int loop)
{
	DEBUG_LOG(HLE,"0=sceSasSetVoice(core=%08x, voicenum=%i, vag=%08x, size=%i, loop=%i)", 
		core, voiceNum, vagAddr, size, loop);

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	if (!Memory::IsValidAddress(vagAddr)) {
		ERROR_LOG(HLE, "Ignoring invalid VAG audio address %08x", vagAddr);
		return 0;
	}

	//Real VAG header is 0x30 bytes behind the vagAddr
	SasVoice &v = sas->voices[voiceNum];
	v.type = VOICETYPE_VAG;
	v.vagAddr = vagAddr;
	v.vagSize = size;
	v.loop = loop ? true : false;
	v.ChangedParams(true);
	return 0;
}

u32 sceSasSetVoicePCM(u32 core, int voiceNum, u32 pcmAddr, int size, int loop)
{
	DEBUG_LOG(HLE,"0=sceSasSetVoicePCM(core=%08x, voicenum=%i, pcmAddr=%08x, size=%i, loop=%i)", core, voiceNum, pcmAddr, size, loop);
	SasVoice &v = sas->voices[voiceNum];
	v.type = VOICETYPE_PCM;
	v.pcmAddr = pcmAddr;
	v.pcmSize = size;
	v.loop = loop ? true : false;
	v.playing = true;
	return 0;
}

u32 sceSasGetPauseFlag(u32 core)
{
	u32 pauseFlag = 0;
	for (int i = 0; i < sas->maxVoices; i++) {
		if (sas->voices[i].paused)
			pauseFlag |= 1 << i;
	}
	DEBUG_LOG(HLE,"%08x=sceSasGetPauseFlag()", pauseFlag);
	return pauseFlag;
}

u32 sceSasSetPause(u32 core, int voicebit, int pause)
{
	DEBUG_LOG(HLE,"0=sceSasSetPause(core=%08x, voicebit=%08x, pause=%i)", core, voicebit, pause);
	for (int i = 0; voicebit != 0; i++, voicebit >>= 1)
	{
		if (i < PSP_SAS_VOICES_MAX && i >= 0)
		{
			if ((voicebit & 1) != 0)
				sas->voices[i].paused = pause ? true : false;
		}
		// TODO: Correct error code?  Mimana crashes otherwise.
		else
			return ERROR_SAS_INVALID_VOICE;
	}

	return 0;
}

u32 sceSasSetVolume(u32 core, int voiceNum, int l, int r, int el, int er)
{
	DEBUG_LOG(HLE,"0=sceSasSetVolume(core=%08x, voiceNum=%i, l=%i, r=%i, el=%i, er=%i", core, voiceNum, l, r, el, er);

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	v.volumeLeft = l;
	v.volumeRight = r;
	return 0;
}

u32 sceSasSetPitch(u32 core, int voiceNum, int pitch)
{
	DEBUG_LOG(HLE,"0=sceSasSetPitch(core=%08x, voiceNum=%i, pitch=%i)", core, voiceNum, pitch);

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	v.pitch = pitch;
	return 0;
}

u32 sceSasSetKeyOn(u32 core, int voiceNum)
{
	DEBUG_LOG(HLE,"0=sceSasSetKeyOn(core=%08x, voiceNum=%i)", core, voiceNum);

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	v.KeyOn();
	return 0;
}

// sceSasSetKeyOff can be used to start sounds, that just sound during the Release phase!
u32 sceSasSetKeyOff(u32 core, int voiceNum)
{
	if (voiceNum == -1) {
		// TODO: Some games (like Every Extend Extra) deliberately pass voiceNum = -1. Does that mean all voices? for now let's ignore.
		DEBUG_LOG(HLE,"sceSasSetKeyOff(core=%08x, voiceNum=%i) - voiceNum = -1???", core, voiceNum);
		return 0;
	} else if (voiceNum < 0 || voiceNum >= PSP_SAS_VOICES_MAX) {
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	} else {
		DEBUG_LOG(HLE,"0=sceSasSetKeyOff(core=%08x, voiceNum=%i)", core, voiceNum);
		SasVoice &v = sas->voices[voiceNum];
		v.KeyOff();
		return 0;
	}
}

u32 sceSasSetNoise(u32 core, int voiceNum, int freq)
{
	DEBUG_LOG(HLE,"0=sceSasSetNoise(core=%08x, voiceNum=%i, freq=%i)", core, voiceNum, freq);

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	v.type = VOICETYPE_NOISE;
	v.noiseFreq = freq;
	return 0;
}

u32 sceSasSetSL(u32 core, int voiceNum, int level)
{
	DEBUG_LOG(HLE,"0=sceSasSetSL(core=%08x, voiceNum=%i, level=%i)", core, voiceNum, level);

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	v.envelope.sustainLevel = level;
	return 0;
}

u32 sceSasSetADSR(u32 core, int voiceNum,int flag ,int a, int d, int s, int r)
{
	DEBUG_LOG(HLE,"0=sceSasSetADSR(core=%08x, voicenum=%i, flag=%i, a=%08x, d=%08x, s=%08x, r=%08x)",core, voiceNum, flag, a,d,s,r)

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	if ((flag & 0x1) != 0) v.envelope.attackRate  = a;
	if ((flag & 0x2) != 0) v.envelope.decayRate   = d;
	if ((flag & 0x4) != 0) v.envelope.sustainRate = s;
	if ((flag & 0x8) != 0) v.envelope.releaseRate = r;
	return 0;
}

u32 sceSasSetADSRMode(u32 core, int voiceNum,int flag ,int a, int d, int s, int r)
{
	DEBUG_LOG(HLE,"0=sceSasSetADSRMode(core=%08x, voicenum=%i, flag=%i, a=%08x, d=%08x, s=%08x, r=%08x)",core, voiceNum, flag, a,d,s,r)

	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	if ((flag & 0x1) != 0) v.envelope.attackType  = a;
	if ((flag & 0x2) != 0) v.envelope.decayType   = d;
	if ((flag & 0x4) != 0) v.envelope.sustainType = s;
	if ((flag & 0x8) != 0) v.envelope.releaseType = r;
	return 0 ;
}


u32 sceSasSetSimpleADSR(u32 core, u32 voiceNum, u32 ADSREnv1, u32 ADSREnv2)
{
	DEBUG_LOG(HLE,"0=sasSetSimpleADSR(%08x, %i, %08x, %08x)", core, voiceNum, ADSREnv1, ADSREnv2);
	SasVoice &v = sas->voices[voiceNum];
	v.envelope.SetSimpleEnvelope(ADSREnv1 & 0xFFFF, ADSREnv2 & 0xFFFF);
	return 0;
}

u32 sceSasGetEnvelopeHeight(u32 core, u32 voiceNum)
{
	// Spam reduction
	if (voiceNum == 17)
	{
		DEBUG_LOG(HLE,"UNIMPL 0=sceSasGetEnvelopeHeight(core=%08x, voicenum=%i)", core, voiceNum);
	}
	if (voiceNum >= PSP_SAS_VOICES_MAX)
	{
		WARN_LOG(HLE, "%s: invalid voicenum %d", __FUNCTION__, voiceNum);
		return ERROR_SAS_INVALID_VOICE;
	}

	SasVoice &v = sas->voices[voiceNum];
	return v.envelope.GetHeight();
}

u32 sceSasRevType(u32 core, int type)
{
	DEBUG_LOG(HLE,"0=sceSasRevType(core=%08x, type=%i)", core, type);
	sas->waveformEffect.type = type;
	return 0;
}

u32 sceSasRevParam(u32 core, int delay, int feedback)
{
	DEBUG_LOG(HLE,"0=sceSasRevParam(core=%08x, delay=%i, feedback=%i)", core, delay, feedback);
	sas->waveformEffect.delay = delay;
	sas->waveformEffect.feedback = feedback;
	return 0;
}

u32 sceSasRevEVOL(u32 core, int lv, int rv)
{
	DEBUG_LOG(HLE,"0=sceSasRevEVOL(core=%08x, leftVolume=%i, rightVolume=%i)", core, lv, rv);
	sas->waveformEffect.leftVol = lv;
	sas->waveformEffect.rightVol = rv;
	return 0;
}

u32 sceSasRevVON(u32 core, int dry, int wet)
{
	DEBUG_LOG(HLE,"0=sceSasRevVON(core=%08x, dry=%i, wet=%i)", core, dry, wet);
	sas->waveformEffect.isDryOn = (dry > 0);
	sas->waveformEffect.isWetOn = (wet > 0);
	return 0;
}

u32 sceSasGetGrain(u32 core)
{
	DEBUG_LOG(HLE,"0=sceSasGetGrain(core=%08x)", core);
	return sas->GetGrainSize();
}

u32 sceSasSetGrain(u32 core, int grain)
{
	INFO_LOG(HLE,"0=sceSasSetGrain(core=%08x, grain=%i)", core, grain);
	sas->SetGrainSize(grain);
	return 0;
}

u32 sceSasGetOutputMode(u32 core)
{
	DEBUG_LOG(HLE,"0=sceSasGetOutputMode(core=%08x)", core);
	return sas->outputMode;
}

u32 sceSasSetOutputMode(u32 core, u32 outputMode)
{
	DEBUG_LOG(HLE,"0=sceSasSetOutputMode(core=%08x, outputMode=%i)", core, outputMode);
	sas->outputMode = outputMode;
	return 0;
}


u32 sceSasGetAllEnvelopeHeights(u32 core, u32 heightsAddr)
{
	DEBUG_LOG(HLE,"0=sceSasGetAllEnvelopeHeights(core=%08x, heightsAddr=%i)", core, heightsAddr);
	if (Memory::IsValidAddress(heightsAddr)) {
		for (int i = 0; i < PSP_SAS_VOICES_MAX; i++) {
			int voiceHeight = sas->voices[i].envelope.GetHeight();
			Memory::Write_U32(voiceHeight, heightsAddr + i * 4);
		}
	}
	return 0;
}

u32 sceSasSetTriangularWave(u32 sasCore, int voice, int unknown)
{
	ERROR_LOG(HLE,"UNIMPL 0=sceSasSetTriangularWave(core=%08x, voice=%i, unknown=%i)", sasCore, voice, unknown);
	return 0;
}

u32 sceSasSetSteepWave(u32 sasCore, int voice, int unknown)
{
	ERROR_LOG(HLE,"UNIMPL 0=sceSasSetSteepWave(core=%08x, voice=%i, unknown=%i)", sasCore, voice, unknown);
	return 0;
}

const HLEFunction sceSasCore[] =
{
	{0x42778a9f, WrapU_UUUUU<sceSasInit>, "__sceSasInit"}, // (SceUID * sasCore, int grain, int maxVoices, int outputMode, int sampleRate)
	{0xa3589d81, WrapU_UU<_sceSasCore>, "__sceSasCore"},
	{0x50a14dfc, WrapU_UU<_sceSasCoreWithMix>, "__sceSasCoreWithMix"},	// Process and mix into buffer (int sasCore, int sasInOut, int leftVolume, int rightVolume)
	{0x68a46b95, WrapU_U<sceSasGetEndFlag>, "__sceSasGetEndFlag"},
	{0x440ca7d8, WrapU_UIIIII<sceSasSetVolume>, "__sceSasSetVolume"},
	{0xad84d37f, WrapU_UII<sceSasSetPitch>, "__sceSasSetPitch"},
	{0x99944089, WrapU_UIUII<sceSasSetVoice>, "__sceSasSetVoice"},
	{0xb7660a23, WrapU_UII<sceSasSetNoise>, "__sceSasSetNoise"},
	{0x019b25eb, WrapU_UIIIIII<sceSasSetADSR>, "__sceSasSetADSR"},
	{0x9ec3676a, WrapU_UIIIIII<sceSasSetADSRMode>, "__sceSasSetADSRmode"},
	{0x5f9529f6, WrapU_UII<sceSasSetSL>, "__sceSasSetSL"},
	{0x74ae582a, WrapU_UU<sceSasGetEnvelopeHeight>, "__sceSasGetEnvelopeHeight"},	
	{0xcbcd4f79, WrapU_UUUU<sceSasSetSimpleADSR>, "__sceSasSetSimpleADSR"},
	{0xa0cf2fa4, WrapU_UI<sceSasSetKeyOff>, "__sceSasSetKeyOff"},
	{0x76f01aca, WrapU_UI<sceSasSetKeyOn>, "__sceSasSetKeyOn"},
	{0xf983b186, WrapU_UII<sceSasRevVON>, "__sceSasRevVON"},
	{0xd5a229c9, WrapU_UII<sceSasRevEVOL>, "__sceSasRevEVOL"},  // effect volume
	{0x33d4ab37, WrapU_UI<sceSasRevType>, "__sceSasRevType"},
	{0x267a6dd2, WrapU_UII<sceSasRevParam>, "__sceSasRevParam"},
	{0x2c8e6ab3, WrapU_U<sceSasGetPauseFlag>, "__sceSasGetPauseFlag"},
	{0x787d04d5, WrapU_UII<sceSasSetPause>, "__sceSasSetPause"},
	{0xa232cbe6, WrapU_UII<sceSasSetTriangularWave>, "__sceSasSetTriangularWave"},
	{0xd5ebbbcd, WrapU_UII<sceSasSetSteepWave>, "__sceSasSetSteepWave"},
	{0xBD11B7C2, WrapU_U<sceSasGetGrain>, "__sceSasGetGrain"},
	{0xd1e0a01e, WrapU_UI<sceSasSetGrain>, "__sceSasSetGrain"},
	{0xe175ef66, WrapU_U<sceSasGetOutputMode>, "__sceSasGetOutputmode"},
	{0xe855bf76, WrapU_UU<sceSasSetOutputMode>, "__sceSasSetOutputmode"},
	{0x07f58c24, WrapU_UU<sceSasGetAllEnvelopeHeights>, "__sceSasGetAllEnvelopeHeights"},
	{0xE1CD9561, WrapU_UIUII<sceSasSetVoicePCM>, "__sceSasSetVoicePCM"},
};

void Register_sceSasCore()
{
	RegisterModule("sceSasCore", ARRAY_SIZE(sceSasCore), sceSasCore);
}

