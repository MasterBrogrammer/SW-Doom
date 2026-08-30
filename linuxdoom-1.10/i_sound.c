#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <SDL.h>

#include "z_zone.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"
#include "doomdef.h"

enum { NUM_VOICES = 8 };
enum { MIX_PAD_PADBYTES = 256 }; /* silence 128 after payload */

typedef struct {
    unsigned char *data; /* unsigned 8-bit PCM past 8-byte DMX header; NULL if missing */
    int            length; /* payload sample count (NOT including pad) */
    int            rate;   /* Hz from header */
} pad_t;

typedef struct {
    int                  handle; /* 0 = free */
    int                  sfx_id;
    unsigned             started;
    const unsigned char *pcm;    /* NULL = idle */
    const unsigned char *pcm_end;
    unsigned int         pos;    /* 16.16 */
    unsigned int         step;   /* 16.16 */
    int                 *leftvol_lookup;
    int                 *rightvol_lookup;
} voice_t;

static pad_t   pads[NUMSFX];
static voice_t voices[NUM_VOICES];
static int     steptable[256];
static int     vol_lookup[128 * 256];

static SDL_AudioDeviceID audio_dev;
static int               device_rate;
static unsigned          next_handle;
static unsigned          start_seq;
static int               starts_count;
static int               mixed_nonzero_count;
static volatile int      mix_heard;
static long long         mix_energy[NUMSFX];
static unsigned          mix_frames[NUMSFX];
static FILE             *wav_out;
static unsigned          wav_bytes;

static unsigned short
rd_u16(const unsigned char *p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

static unsigned int
rd_u32(const unsigned char *p)
{
    return (unsigned int)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* Menu volume is 0..15; mixer LUTs expect 0..127. */
static int
menu_vol_to_mix(int vol)
{
    if (vol < 0)
	return 0;
    if (vol <= 15)
	return vol * 8;
    if (vol > 127)
	return 127;
    return vol;
}

static void
vols_from_sep(int vol, int sep, int *leftvol, int *rightvol)
{
    int left;
    int right;

    vol = menu_vol_to_mix(vol);
    if (sep < 0)
	sep = 0;
    if (sep > 255)
	sep = 255;

    sep += 1;
    left = vol - ((vol * sep * sep) >> 16);
    sep = sep - 257;
    right = vol - ((vol * sep * sep) >> 16);

    if (left < 0)
	left = 0;
    if (left > 127)
	left = 127;
    if (right < 0)
	right = 0;
    if (right > 127)
	right = 127;

    *leftvol = left;
    *rightvol = right;
}

static void
bind_voice_vols(voice_t *v, int vol, int sep)
{
    int left;
    int right;

    vols_from_sep(vol, sep, &left, &right);
    v->leftvol_lookup = &vol_lookup[left * 256];
    v->rightvol_lookup = &vol_lookup[right * 256];
}

static int
find_voice(int handle)
{
    int i;

    if (handle == 0)
	return -1;
    for (i = 0; i < NUM_VOICES; i++)
    {
	if (voices[i].handle == handle)
	    return i;
    }
    return -1;
}

static int
alloc_voice(void)
{
    int i;
    int best;
    unsigned best_started;

    for (i = 0; i < NUM_VOICES; i++)
    {
	if (voices[i].handle == 0)
	    return i;
    }

    best = 0;
    best_started = voices[0].started;
    for (i = 1; i < NUM_VOICES; i++)
    {
	if (voices[i].started < best_started)
	{
	    best_started = voices[i].started;
	    best = i;
	}
    }
    return best;
}

static void
clear_voice(voice_t *v)
{
    v->pcm = NULL;
    v->handle = 0;
    v->sfx_id = 0;
    v->started = 0;
    v->pcm_end = NULL;
    v->pos = 0;
    v->step = 0;
    v->leftvol_lookup = NULL;
    v->rightvol_lookup = NULL;
}

static void
precache_pad(int sfx_id)
{
    char namebuf[9];
    int lumpnum;
    int lumplen;
    const unsigned char *raw;
    unsigned short format;
    unsigned short rate;
    unsigned int nsamples;
    unsigned char *buf;
    int i;

    pads[sfx_id].data = NULL;
    pads[sfx_id].length = 0;
    pads[sfx_id].rate = 0;

    if (S_sfx[sfx_id].name == NULL)
	return;

    sprintf(namebuf, "ds%s", S_sfx[sfx_id].name);
    lumpnum = W_CheckNumForName(namebuf);
    if (lumpnum < 0)
	return;

    lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8)
	return;

    raw = (const unsigned char *)W_CacheLumpNum(lumpnum, PU_STATIC);
    format = rd_u16(raw);
    rate = rd_u16(raw + 2);
    nsamples = rd_u32(raw + 4);

    if (format != 3 || rate == 0 || nsamples == 0
	|| (int)nsamples > lumplen - 8)
    {
	Z_Free((void *)raw);
	return;
    }

    buf = (unsigned char *)Z_Malloc((int)nsamples + MIX_PAD_PADBYTES, PU_STATIC, 0);
    memcpy(buf, raw + 8, nsamples);
    for (i = 0; i < MIX_PAD_PADBYTES; i++)
	buf[nsamples + i] = 128;

    Z_Free((void *)raw);

    pads[sfx_id].data = buf;
    pads[sfx_id].length = (int)nsamples;
    pads[sfx_id].rate = (int)rate;
    S_sfx[sfx_id].data = pads[sfx_id].data;
}

static void SDLCALL
mix_callback(void *userdata, Uint8 *stream, int len)
{
    Sint16 *out;
    int frames;
    int f;
    int vi;
    int dl;
    int dr;
    unsigned int sample;
    voice_t *v;

    (void)userdata;

    out = (Sint16 *)stream;
    frames = len / (int)sizeof(Sint16) / 2;
    memset(stream, 0, (size_t)len);

    for (f = 0; f < frames; f++)
    {
	dl = 0;
	dr = 0;

	for (vi = 0; vi < NUM_VOICES; vi++)
	{
	    v = &voices[vi];
	    if (v->pcm == NULL)
		continue;

	    sample = v->pcm[v->pos >> 16];
	    dl += v->leftvol_lookup[sample];
	    dr += v->rightvol_lookup[sample];
	    if (v->sfx_id > 0 && v->sfx_id < NUMSFX)
	    {
		int e = v->leftvol_lookup[sample];
		if (e < 0)
		    e = -e;
		mix_energy[v->sfx_id] += e;
		mix_frames[v->sfx_id]++;
	    }
	    v->pos += v->step;
	    if (v->pcm + (v->pos >> 16) >= v->pcm_end)
		clear_voice(v);
	}

	if (dl > 0x7fff)
	    dl = 0x7fff;
	else if (dl < -0x8000)
	    dl = -0x8000;

	if (dr > 0x7fff)
	    dr = 0x7fff;
	else if (dr < -0x8000)
	    dr = -0x8000;

	out[f * 2] = (Sint16)dl;
	out[f * 2 + 1] = (Sint16)dr;

	if (wav_out)
	{
	    fwrite(&out[f * 2], sizeof(Sint16), 2, wav_out);
	    wav_bytes += 4;
	}

	if (dl != 0 || dr != 0)
	{
	    mixed_nonzero_count++;
	    mix_heard = 1;
	}
    }
}

void I_SetChannels(void)
{
    int i;
    int j;
    int *steptablemid = steptable + 128;

    for (i = -128; i < 128; i++)
	steptablemid[i] = (int)(pow(2.0, (i / 64.0)) * 65536.0);

    for (i = 0; i < 128; i++)
	for (j = 0; j < 256; j++)
	    vol_lookup[i * 256 + j] = (i * (j - 128) * 256) / 127;
}

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[9];
    int lump;

    sprintf(namebuf, "ds%s", sfxinfo->name);
    lump = W_CheckNumForName(namebuf);
    return lump;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    pad_t *pad;
    voice_t *v;
    unsigned int step;
    int slot;
    int handle;

    (void)priority;

    if (!audio_dev || id < 1 || id >= NUMSFX)
	return 0;

    pad = &pads[id];
    if (pad->data == NULL || pad->length <= 0)
	return 0;

    if (sep < 0)
	sep = 0;
    if (sep > 255)
	sep = 255;
    if (pitch < 0)
	pitch = 0;
    if (pitch > 255)
	pitch = 255;

    step = (unsigned int)((int64_t)steptable[pitch] * pad->rate / device_rate);
    if (step == 0)
	step = 1;

    SDL_LockAudioDevice(audio_dev);
    slot = alloc_voice();
    v = &voices[slot];

    handle = (int)next_handle++;
    if (next_handle == 0)
	next_handle = 1;

    v->handle = handle;
    v->sfx_id = id;
    v->started = ++start_seq;
    v->pcm_end = pad->data + pad->length;
    v->pos = 0;
    v->step = step;
    bind_voice_vols(v, vol, sep);
    /* Publish pcm last so the callback never sees a half-built voice. */
    v->pcm = pad->data;

    starts_count++;
    SDL_UnlockAudioDevice(audio_dev);
    {
	int left;
	int right;
	vols_from_sep(vol, sep, &left, &right);
	fprintf(stderr,
		"I_StartSound: id=%d handle=%d vol=%d sep=%d pitch=%d step=%u left=%d right=%d len=%d rate=%d\n",
		id, handle, vol, sep, pitch, step, left, right,
		pad->length, pad->rate);
    }
    fflush(stderr);
    return handle;
}

void I_StopSound(int handle)
{
    int i;

    if (!audio_dev || handle == 0)
	return;

    SDL_LockAudioDevice(audio_dev);
    i = find_voice(handle);
    if (i >= 0)
	clear_voice(&voices[i]);
    SDL_UnlockAudioDevice(audio_dev);
}

int I_SoundIsPlaying(int handle)
{
    int i;
    int playing;

    if (handle == 0 || !audio_dev)
	return 0;

    SDL_LockAudioDevice(audio_dev);
    i = find_voice(handle);
    playing = (i >= 0 && voices[i].pcm != NULL);
    SDL_UnlockAudioDevice(audio_dev);
    return playing;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    int i;

    /* S_ resets pitch to NORM_PITCH every tic for non-local sounds; leave step. */
    (void)pitch;

    if (!audio_dev || handle == 0)
	return;

    SDL_LockAudioDevice(audio_dev);
    i = find_voice(handle);
    if (i >= 0 && voices[i].pcm != NULL)
	bind_voice_vols(&voices[i], vol, sep);
    SDL_UnlockAudioDevice(audio_dev);
}

void I_UpdateSound(void)
{
    static int logged_heard;

    if (mix_heard && !logged_heard)
    {
	logged_heard = 1;
	fprintf(stderr, "I_UpdateSound: mix heard\n");
	fflush(stderr);
    }
}

void I_SubmitSound(void)
{
}

void I_ShutdownSound(void)
{
    int i;

    if (!audio_dev)
	return;

    SDL_LockAudioDevice(audio_dev);
    for (i = 0; i < NUM_VOICES; i++)
	clear_voice(&voices[i]);
    SDL_UnlockAudioDevice(audio_dev);

    SDL_CloseAudioDevice(audio_dev);
    audio_dev = 0;

    fprintf(stderr, "I_ShutdownSound: starts=%d mixed_nonzero=%d\n",
	    starts_count, mixed_nonzero_count);
    {
	int i;
	for (i = 1; i < NUMSFX; i++)
	{
	    if (mix_frames[i])
		fprintf(stderr, "I_mix: id=%d frames=%u energy=%lld name=%s\n",
			i, mix_frames[i], mix_energy[i],
			S_sfx[i].name ? S_sfx[i].name : "?");
	}
    }
    if (wav_out)
    {
	unsigned riff = wav_bytes + 36;
	unsigned rate = (unsigned)device_rate;
	unsigned brate = rate * 4;
	unsigned char hdr[44];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, "RIFF", 4);
	hdr[4] = riff; hdr[5] = riff >> 8; hdr[6] = riff >> 16; hdr[7] = riff >> 24;
	memcpy(hdr + 8, "WAVEfmt ", 8);
	hdr[16] = 16;
	hdr[20] = 1;
	hdr[22] = 2;
	hdr[24] = rate; hdr[25] = rate >> 8; hdr[26] = rate >> 16; hdr[27] = rate >> 24;
	hdr[28] = brate; hdr[29] = brate >> 8; hdr[30] = brate >> 16; hdr[31] = brate >> 24;
	hdr[32] = 4;
	hdr[34] = 16;
	memcpy(hdr + 36, "data", 4);
	hdr[40] = wav_bytes; hdr[41] = wav_bytes >> 8;
	hdr[42] = wav_bytes >> 16; hdr[43] = wav_bytes >> 24;
	rewind(wav_out);
	fwrite(hdr, 1, 44, wav_out);
	fclose(wav_out);
	wav_out = NULL;
	fprintf(stderr, "I_ShutdownSound: wrote mix wav %u bytes\n", wav_bytes);
    }
}

void I_InitSound(void)
{
    SDL_AudioSpec want;
    SDL_AudioSpec have;
    int i;

    audio_dev = 0;
    device_rate = 0;
    next_handle = 1;
    start_seq = 0;
    starts_count = 0;
    mixed_nonzero_count = 0;
    mix_heard = 0;
    memset(mix_energy, 0, sizeof(mix_energy));
    memset(mix_frames, 0, sizeof(mix_frames));
    wav_out = NULL;
    wav_bytes = 0;
    {
	const char *wavpath = getenv("SWDOOM_WAV");
	if (wavpath && wavpath[0])
	{
	    wav_out = fopen(wavpath, "wb");
	    if (wav_out)
		fwrite("\0", 1, 44, wav_out);
	}
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
	fprintf(stderr, "I_InitSound: SDL_InitSubSystem: %s\n", SDL_GetError());
	return;
    }

    memset(&want, 0, sizeof(want));
    want.freq = 22050;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix_callback;
    want.userdata = NULL;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
				    SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (audio_dev == 0)
    {
	fprintf(stderr, "I_InitSound: SDL_OpenAudioDevice: %s\n", SDL_GetError());
	return;
    }

    if (have.format != AUDIO_S16SYS || have.channels != 2)
    {
	fprintf(stderr, "I_InitSound: need S16 stereo, got format=%d channels=%d\n",
		(int)have.format, (int)have.channels);
	SDL_CloseAudioDevice(audio_dev);
	audio_dev = 0;
	return;
    }

    device_rate = have.freq;

    for (i = 1; i < NUMSFX; i++)
	precache_pad(i);

    for (i = 0; i < NUM_VOICES; i++)
	clear_voice(&voices[i]);

    next_handle = 1;
    I_SetChannels();
    SDL_PauseAudioDevice(audio_dev, 0);
    fprintf(stderr, "I_InitSound: opened %d Hz\n", device_rate);
    fflush(stderr);

    if (M_CheckParm("-sfxprobe"))
	I_StartSound(sfx_pistol, 8, 128, 128, 64);
}

void I_RestartSound(void)
{
    SDL_AudioSpec want;
    SDL_AudioSpec have;

    if (!audio_dev)
	return;

    SDL_LockAudioDevice(audio_dev);
    {
	int i;
	for (i = 0; i < NUM_VOICES; i++)
	    clear_voice(&voices[i]);
    }
    SDL_UnlockAudioDevice(audio_dev);
    SDL_CloseAudioDevice(audio_dev);
    audio_dev = 0;

    memset(&want, 0, sizeof(want));
    want.freq = device_rate ? device_rate : 22050;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix_callback;
    want.userdata = NULL;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
				    SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (audio_dev == 0)
    {
	fprintf(stderr, "I_RestartSound: SDL_OpenAudioDevice: %s\n", SDL_GetError());
	return;
    }
    if (have.format != AUDIO_S16SYS || have.channels != 2)
    {
	fprintf(stderr, "I_RestartSound: need S16 stereo\n");
	SDL_CloseAudioDevice(audio_dev);
	audio_dev = 0;
	return;
    }
    device_rate = have.freq;
    next_handle = 1;
    SDL_PauseAudioDevice(audio_dev, 0);
    fprintf(stderr, "I_RestartSound: opened %d Hz driver=%s\n",
	    device_rate,
	    SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    fflush(stderr);
}
