// Software sound mixer for the ESP32-P4 port (STDSOUND_ESP32).
//
// Buffers are plain PCM in RAM (8/16 bit, mono/stereo, any rate). The engine
// starts/stops them; stdSound_ESP32_Pump() (called once per frame from the
// platform loop) mixes the playing buffers into 16-bit stereo at
// STDSOUND_SAMPLE_RATE and hands the result to the HAL audio path.
#include "Win95/stdSound.h"
#include "Gui/jkGUISound.h"
#include "Main/Main.h"
#include "stdPlatform.h"
#include "Platform/wuRegistry.h"
#include "General/stdMath.h"
#include "jk.h"

#include "jk_esp.h"

#include <stdio.h>
#include <string.h>

#ifndef STDSOUND_SAMPLE_RATE
#define STDSOUND_SAMPLE_RATE (22050)
#endif
// how much audio each pump produces at most (frames of stereo output)
#define STDSOUND_PUMP_FRAMES (STDSOUND_SAMPLE_RATE / 20)

enum stdEsp32SoundFormat {
    STDSOUND_FMT_8BIT_MONO = 0,
    STDSOUND_FMT_8BIT_STEREO,
    STDSOUND_FMT_16BIT_MONO,
    STDSOUND_FMT_16BIT_STEREO,
};

uint32_t stdSound_ParseWav(stdFile_t sound_file, uint32_t *nSamplesPerSec, int32_t *bitsPerSample, int32_t *bStereo, int32_t *seekOffset)
{
    unsigned int result;
    char v9[4];
    stdWaveFormat v10;
    uint32_t seekPos;

    std_g_pHS->fseek(sound_file, 8, 0);
    std_g_pHS->fileRead(sound_file, v9, 4);
    result = 0;
    if ( !_memcmp(v9, "WAVE", 4) )
    {
        std_g_pHS->fseek(sound_file, 4, SEEK_CUR);
        std_g_pHS->fileRead(sound_file, &seekPos, 4);
        std_g_pHS->fileRead(sound_file, &v10, sizeof(stdWaveFormat));
        *nSamplesPerSec = v10.nSamplesPerSec;
        *bitsPerSample = 8 * (v10.nBlockAlign / (int)v10.nChannels);
        *bStereo = v10.nChannels == 2;
        if (seekPos > 0x10 )
            std_g_pHS->fseek(sound_file, seekPos - 16, 1);
        while (!std_g_pHS->fileEof(sound_file))
        {
            std_g_pHS->fileRead(sound_file, v9, 4);
            std_g_pHS->fileRead(sound_file, &seekPos, 4);
            if (!_memcmp(v9, "data", 4)) break;
            std_g_pHS->fseek(sound_file, seekPos, SEEK_CUR);
        }
        *seekOffset = std_g_pHS->ftell(sound_file);
        result = seekPos;
    }
    return result;
}

static stdSound_buffer_t* stdSound_aPlayingSounds[SITH_MIXER_NUMPLAYINGSOUNDS];
static int16_t* stdSound_pMixBuf = NULL;
static uint64_t stdSound_lastPumpUs = 0;
static int stdSound_bInitted = 0;

static int stdSound_FormatFor(int bStereo, int bitsPerSample)
{
    if (bStereo) return bitsPerSample == 8 ? STDSOUND_FMT_8BIT_STEREO : STDSOUND_FMT_16BIT_STEREO;
    return bitsPerSample == 8 ? STDSOUND_FMT_8BIT_MONO : STDSOUND_FMT_16BIT_MONO;
}

static size_t stdSound_BytesPerFrame(int format)
{
    switch (format) {
    case STDSOUND_FMT_8BIT_MONO: return 1;
    case STDSOUND_FMT_8BIT_STEREO: return 2;
    case STDSOUND_FMT_16BIT_MONO: return 2;
    case STDSOUND_FMT_16BIT_STEREO: return 4;
    }
    return 1;
}

// fetch source frame `idx` as L/R 16-bit
static inline void stdSound_Fetch(const stdSound_buffer_t* buf, uint32_t idx, int32_t* l, int32_t* r)
{
    switch (buf->format) {
    case STDSOUND_FMT_8BIT_MONO: {
        int32_t v = ((int32_t)((const uint8_t*)buf->data)[idx] - 128) << 8;
        *l = v; *r = v;
        break;
    }
    case STDSOUND_FMT_8BIT_STEREO: {
        const uint8_t* p = (const uint8_t*)buf->data + idx * 2;
        *l = ((int32_t)p[0] - 128) << 8;
        *r = ((int32_t)p[1] - 128) << 8;
        break;
    }
    case STDSOUND_FMT_16BIT_MONO: {
        int32_t v = ((const int16_t*)buf->data)[idx];
        *l = v; *r = v;
        break;
    }
    default: {
        const int16_t* p = (const int16_t*)buf->data + idx * 2;
        *l = p[0];
        *r = p[1];
        break;
    }
    }
}

// Mix up to `frames` frames of output; returns the number of frames produced.
static size_t stdSound_Mix(int16_t* out, size_t frames)
{
    memset(out, 0, frames * 2 * sizeof(int16_t));
    for (int i = 0; i < SITH_MIXER_NUMPLAYINGSOUNDS; i++) {
        stdSound_buffer_t* buf = stdSound_aPlayingSounds[i];
        if (!buf || !buf->data || !buf->isPlaying || buf->vol <= 0.0) continue;
        const uint32_t numFrames = buf->bufferBytes / stdSound_BytesPerFrame(buf->format);
        if (numFrames == 0) continue;
        const uint32_t rate = buf->freq > 0 ? (uint32_t)buf->freq : buf->nSamplesPerSec;
        const uint32_t step = (uint32_t)(((uint64_t)rate << 16) / STDSOUND_SAMPLE_RATE);
        const int32_t volL = (int32_t)(buf->vol * (buf->pan <= 0 ? 1.0f : 1.0f - buf->pan) * 256.0f);
        const int32_t volR = (int32_t)(buf->vol * (buf->pan >= 0 ? 1.0f : 1.0f + buf->pan) * 256.0f);
        uint32_t pos = buf->currentSample;
        int16_t* dst = out;
        for (size_t j = 0; j < frames; j++) {
            uint32_t idx = pos >> 16;
            if (idx >= numFrames) {
                if (buf->isLooping) {
                    pos = 0;
                    idx = 0;
                } else {
                    stdSound_BufferStop(buf);
                    break;
                }
            }
            int32_t l, r;
            stdSound_Fetch(buf, idx, &l, &r);
            int32_t ml = dst[0] + ((l * volL) >> 8);
            int32_t mr = dst[1] + ((r * volR) >> 8);
            dst[0] = (int16_t)stdMath_ClampInt(ml, -0x8000, 0x7FFF);
            dst[1] = (int16_t)stdMath_ClampInt(mr, -0x8000, 0x7FFF);
            dst += 2;
            pos += step;
        }
        buf->currentSample = pos;
    }
    return frames;
}

// Called from the platform frame loop: produce the audio for the elapsed
// time (bounded) and hand it to the HAL.
void stdSound_ESP32_Pump(void)
{
    if (!stdSound_bInitted || !stdSound_pMixBuf) return;
    uint64_t now = jk_esp_time_us();
    if (stdSound_lastPumpUs == 0) stdSound_lastPumpUs = now;
    uint64_t elapsed = now - stdSound_lastPumpUs;
    size_t frames = (size_t)((elapsed * STDSOUND_SAMPLE_RATE) / 1000000ULL);
    if (frames < 64) return;
    if (frames > STDSOUND_PUMP_FRAMES) frames = STDSOUND_PUMP_FRAMES;
    stdSound_lastPumpUs = now;
    stdSound_Mix(stdSound_pMixBuf, frames);
    jk_esp_audio_write(stdSound_pMixBuf, frames);
}

int stdSound_Startup()
{
    jkGuiSound_b3DSound = 0;
    memset(stdSound_aPlayingSounds, 0, sizeof(stdSound_aPlayingSounds));
    if (stdSound_bInitted) {
        return 1;
    }
    stdSound_pMixBuf = (int16_t*)malloc(STDSOUND_PUMP_FRAMES * 2 * sizeof(int16_t));
    if (!stdSound_pMixBuf) return 0;
    jk_esp_audio_set_rate(STDSOUND_SAMPLE_RATE);
    stdSound_lastPumpUs = 0;
    stdSound_bInitted = 1;
    return 1;
}

void stdSound_Shutdown()
{
    memset(stdSound_aPlayingSounds, 0, sizeof(stdSound_aPlayingSounds));
}

void stdSound_SetMenuVolume(flex_t a1)
{
    stdSound_fMenuVolume = a1;
}

stdSound_buffer_t* stdSound_BufferCreate(int bStereo, uint32_t nSamplesPerSec, uint16_t bitsPerSample, int bufferLen)
{
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;
    _memset(out, 0, sizeof(*out));
    out->data = NULL;
    out->bStereo = bStereo;
    out->bufferLen = bufferLen;
    out->nSamplesPerSec = nSamplesPerSec;
    out->bitsPerSample = bitsPerSample;
    out->refcnt = 1;
    out->vol = 1.0 * stdSound_fMenuVolume;
    out->pan = 0.0;
    out->format = stdSound_FormatFor(bStereo, bitsPerSample);
    return out;
}

void* stdSound_BufferSetData(stdSound_buffer_t* sound, int bufferBytes, int32_t* bufferMaxSize)
{
    sound->bufferBytes = bufferBytes;
    if (bufferMaxSize)
        *bufferMaxSize = bufferBytes;
    if (sound->data && !sound->bIsCopy)
        STD_FREE(sound->data);
    sound->bufferBytes = 0;
    sound->data = STD_ALLOC(bufferBytes);
    if (!sound->data) {
        return NULL;
    }
    sound->bufferBytes = bufferBytes;
    memset(sound->data, 0, sound->bufferBytes);
    return sound->data;
}

int stdSound_BufferUnlock(stdSound_buffer_t* sound, void* buffer, int bufferRead)
{
    return 1;
}

int stdSound_BufferPlay(stdSound_buffer_t* buf, int loop)
{
    if (!buf || !buf->data) return 0;
    buf->isLooping = loop;
    if (stdSound_IsPlaying(buf, NULL)) {
        return 1;
    }
    for (int i = 0; i < SITH_MIXER_NUMPLAYINGSOUNDS; i++) {
        if (!stdSound_aPlayingSounds[i]) {
            buf->currentSample = 0;
            buf->isPlaying = 1;
            stdSound_aPlayingSounds[i] = buf;
            return 1;
        }
    }
    return 0;
}

int stdSound_BufferQueueAfterAnother(stdSound_buffer_t* bufPrev, stdSound_buffer_t* bufNext)
{
    return stdSound_BufferPlay(bufNext, 0);
}

void stdSound_BufferUnqueueProcessed(stdSound_buffer_t* buf)
{
}

void stdSound_BufferRelease(stdSound_buffer_t* sound)
{
    if (!sound) return;
    stdSound_BufferStop(sound);
    if (sound->data && !sound->bIsCopy) {
        STD_FREE(sound->data);
        sound->data = NULL;
    }
    memset(sound, 0, sizeof(*sound));
    STD_FREE(sound);
}

int stdSound_BufferReset(stdSound_buffer_t* sound)
{
    if (!sound) return 0;
    sound->isPlaying = 0;
    sound->currentSample = 0;
    sound->isLooping = 0;
    return 1;
}

void stdSound_BufferSetPan(stdSound_buffer_t* a1, flex_t a2)
{
    if (!a1) return;
    a1->pan = a2;
}

void stdSound_BufferSetFrequency(stdSound_buffer_t* sound, int freq)
{
    if (!sound) return;
    sound->freq = freq;
}

stdSound_buffer_t* stdSound_BufferDuplicate(stdSound_buffer_t* sound)
{
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;
    _memset(out, 0, sizeof(*out));
    out->data = sound->data;
    out->bStereo = sound->bStereo;
    out->bufferLen = sound->bufferLen;
    out->nSamplesPerSec = sound->nSamplesPerSec;
    out->bitsPerSample = sound->bitsPerSample;
    out->refcnt = 1;
    out->vol = sound->vol;
    out->pan = sound->pan;
    out->format = sound->format;
    out->bufferBytes = sound->bufferBytes;
    out->bIsCopy = 1;
    out->isPlaying = 0;
    out->currentSample = 0;
    out->isLooping = 0;
    return out;
}

void stdSound_IA3D_idk(flex_t a)
{
}

int stdSound_BufferStop(stdSound_buffer_t* buf)
{
    if (!buf) return 1;
    buf->isPlaying = 0;
    buf->currentSample = 0;
    buf->isLooping = 0;
    for (int i = 0; i < SITH_MIXER_NUMPLAYINGSOUNDS; i++) {
        if (stdSound_aPlayingSounds[i] == buf) {
            stdSound_aPlayingSounds[i] = NULL;
            return 1;
        }
    }
    return 1;
}

void stdSound_BufferSetVolume(stdSound_buffer_t* sound, flex_t vol)
{
    if (!sound) return;
    sound->vol = vol * stdSound_fMenuVolume;
}

int stdSound_3DSetMode(stdSound_buffer_t* a1, int a2)
{
    return 1;
}

stdSound_3dBuffer_t* stdSound_BufferQueryInterface(stdSound_buffer_t* pSoundBuffer)
{
    return pSoundBuffer;
}

void stdSound_CommitDeferredSettings()
{
}

void stdSound_SetPositionOrientation(rdVector3 *pos, rdVector3 *lvec, rdVector3 *uvec)
{
}

void stdSound_SetPosition(stdSound_buffer_t* sound, rdVector3 *pos)
{
}

void stdSound_SetVelocity(stdSound_buffer_t* sound, rdVector3 *vel)
{
}

int stdSound_IsPlaying(stdSound_buffer_t* sound, rdVector3 *pos)
{
    if (sound) return sound->isPlaying;
    return 0;
}

void stdSound_3DBufferRelease(stdSound_3dBuffer_t* p3DBuffer)
{
}
