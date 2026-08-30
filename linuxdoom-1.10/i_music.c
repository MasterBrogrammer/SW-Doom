#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CoreFoundation.h>

#include "i_sound.h"

static AUGraph       graph;
static AUNode        synth_node;
static AUNode        out_node;
static AudioUnit     synth;
static AudioUnit     output_au;
static MusicPlayer   player;
static MusicSequence sequence;
static int           music_ready;
static int           music_playing;
static int           music_loop = 1;
static int           music_vol = 8;
static unsigned char *song_midi;
static int           song_midi_len;

enum { MUSIC_TRIM = 55 };

static unsigned
be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
	 | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static unsigned short
le16(const unsigned char *p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

static void
apply_vol(void)
{
    int v;
    float f;

    v = music_vol;
    if (v < 0)
	v = 0;
    if (v > 15)
	v = 15;
    f = (v / 15.0f) * (MUSIC_TRIM / 100.0f);

    if (output_au)
	AudioUnitSetParameter(output_au, kHALOutputParam_Volume,
			      kAudioUnitScope_Global, 0, f, 0);
}

typedef struct {
    unsigned char *p;
    int n;
    int cap;
} buf_t;

static void
bputc(buf_t *b, int c)
{
    if (b->n >= b->cap)
    {
	b->cap = b->cap ? b->cap * 2 : 8192;
	b->p = (unsigned char *)realloc(b->p, (size_t)b->cap);
    }
    b->p[b->n++] = (unsigned char)c;
}

static void
bvlq(buf_t *b, unsigned v)
{
    unsigned tmp = v & 0x7f;
    while ((v >>= 7) > 0)
    {
	tmp <<= 8;
	tmp |= 0x80 | (v & 0x7f);
    }
    for (;;)
    {
	bputc(b, (int)(tmp & 0xff));
	if (tmp & 0x80)
	    tmp >>= 8;
	else
	    break;
    }
}

static const unsigned char mus_ctrl[15] = {
    0, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121
};

static int
mus_to_midi(const unsigned char *mus, unsigned char **out, int *outlen)
{
    unsigned short scorelen;
    unsigned short scorestart;
    const unsigned char *p;
    const unsigned char *end;
    buf_t body;
    buf_t file;
    int map[16];
    int nextch;
    int vol[16];
    int i;
    unsigned delta;

    if (memcmp(mus, "MUS\x1a", 4) != 0)
	return -1;
    scorelen = le16(mus + 4);
    scorestart = le16(mus + 6);
    p = mus + scorestart;
    end = p + scorelen;

    memset(&body, 0, sizeof(body));
    for (i = 0; i < 16; i++)
    {
	map[i] = -1;
	vol[i] = 64;
    }
    map[15] = 9;
    nextch = 0;
    delta = 0;

    while (p < end)
    {
	int desc = *p++;
	int mchan = desc & 15;
	int type = (desc >> 4) & 7;
	int midich;

	if (map[mchan] < 0)
	{
	    if (nextch == 9)
		nextch++;
	    if (nextch >= 16)
		map[mchan] = 0;
	    else
		map[mchan] = nextch++;
	}
	midich = map[mchan];

	if (type == 6)
	    break;

	bvlq(&body, delta);
	delta = 0;

	if (type == 0)
	{
	    int note = (p < end) ? (*p++ & 0x7f) : 0;
	    bputc(&body, 0x80 | midich);
	    bputc(&body, note);
	    bputc(&body, 0);
	}
	else if (type == 1)
	{
	    int note = (p < end) ? *p++ : 0;
	    if (note & 0x80)
	    {
		note &= 0x7f;
		if (p < end)
		    vol[mchan] = *p++ & 0x7f;
	    }
	    bputc(&body, 0x90 | midich);
	    bputc(&body, note);
	    bputc(&body, vol[mchan]);
	}
	else if (type == 2)
	{
	    int bend = (p < end) ? *p++ : 0;
	    int pb = bend * 64;
	    bputc(&body, 0xe0 | midich);
	    bputc(&body, pb & 0x7f);
	    bputc(&body, (pb >> 7) & 0x7f);
	}
	else if (type == 3)
	{
	    int ctrl = (p < end) ? *p++ : 0;
	    bputc(&body, 0xb0 | midich);
	    bputc(&body, (ctrl < 15) ? mus_ctrl[ctrl] : 0);
	    bputc(&body, 0);
	}
	else if (type == 4)
	{
	    int ctrl = (p < end) ? *p++ : 0;
	    int val = (p < end) ? *p++ : 0;
	    if (ctrl == 0)
	    {
		bputc(&body, 0xc0 | midich);
		bputc(&body, val & 0x7f);
	    }
	    else
	    {
		bputc(&body, 0xb0 | midich);
		bputc(&body, (ctrl < 15) ? mus_ctrl[ctrl] : ctrl);
		bputc(&body, val & 0x7f);
	    }
	}
	else if (type == 5 || type == 7)
	{
	    if (p < end)
		p++;
	}

	if (desc & 0x80)
	{
	    unsigned v = 0;
	    int b;
	    do
	    {
		if (p >= end)
		    break;
		b = *p++;
		v = (v << 7) + (b & 0x7f);
	    } while (b & 0x80);
	    delta = v;
	}
    }

    bvlq(&body, 0);
    bputc(&body, 0xff);
    bputc(&body, 0x2f);
    bputc(&body, 0x00);

    memset(&file, 0, sizeof(file));
    {
	static const unsigned char hdr[] = {
	    'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,70
	};
	unsigned tlen = (unsigned)body.n;
	int j;
	for (j = 0; j < 14; j++)
	    bputc(&file, hdr[j]);
	bputc(&file, 'M');
	bputc(&file, 'T');
	bputc(&file, 'r');
	bputc(&file, 'k');
	bputc(&file, (int)((tlen >> 24) & 0xff));
	bputc(&file, (int)((tlen >> 16) & 0xff));
	bputc(&file, (int)((tlen >> 8) & 0xff));
	bputc(&file, (int)(tlen & 0xff));
	for (j = 0; j < body.n; j++)
	    bputc(&file, body.p[j]);
    }
    free(body.p);
    *out = file.p;
    *outlen = file.n;
    return 0;
}

static int
midi_bytes(const unsigned char *p, int max)
{
    int ntrks;
    int i;
    const unsigned char *q;
    unsigned hlen;

    if (max < 14 || memcmp(p, "MThd", 4) != 0)
	return -1;
    hlen = be32(p + 4);
    if (hlen < 6 || 8 + (int)hlen > max)
	return -1;
    ntrks = (p[10] << 8) | p[11];
    q = p + 8 + hlen;
    for (i = 0; i < ntrks; i++)
    {
	unsigned tlen;
	if ((q + 8) - p > max || memcmp(q, "MTrk", 4) != 0)
	    return -1;
	tlen = be32(q + 4);
	q += 8 + tlen;
	if (q - p > max)
	    return -1;
    }
    return (int)(q - p);
}

static void
loop_tracks(MusicSequence seq)
{
    UInt32 n = 0;
    UInt32 i;
    MusicTimeStamp longest = 0;

    if (MusicSequenceGetTrackCount(seq, &n) != noErr)
	return;
    for (i = 0; i < n; i++)
    {
	MusicTrack t;
	MusicTimeStamp len = 0;
	UInt32 sz = sizeof(len);
	if (MusicSequenceGetIndTrack(seq, i, &t) != noErr)
	    continue;
	if (MusicTrackGetProperty(t, kSequenceTrackProperty_TrackLength,
				  &len, &sz) == noErr && len > longest)
	    longest = len;
    }
    if (longest <= 0)
	return;
    for (i = 0; i < n; i++)
    {
	MusicTrack t;
	MusicTrackLoopInfo info;
	if (MusicSequenceGetIndTrack(seq, i, &t) != noErr)
	    continue;
	info.loopDuration = longest;
	info.numberOfLoops = 0;
	MusicTrackSetProperty(t, kSequenceTrackProperty_LoopInfo,
			      &info, sizeof(info));
    }
}

void I_InitMusic(void)
{
    AudioComponentDescription dls;
    AudioComponentDescription outd;
    OSStatus err;

    music_ready = 0;
    synth = 0;
    output_au = 0;
    graph = 0;
    player = 0;
    sequence = 0;

    memset(&dls, 0, sizeof(dls));
    dls.componentType = kAudioUnitType_MusicDevice;
    dls.componentSubType = kAudioUnitSubType_MIDISynth;
    dls.componentManufacturer = kAudioUnitManufacturer_Apple;

    memset(&outd, 0, sizeof(outd));
    outd.componentType = kAudioUnitType_Output;
    outd.componentSubType = kAudioUnitSubType_DefaultOutput;
    outd.componentManufacturer = kAudioUnitManufacturer_Apple;

    err = NewAUGraph(&graph);
    if (err != noErr)
    {
	fprintf(stderr, "I_InitMusic: NewAUGraph %d\n", (int)err);
	return;
    }
    err = AUGraphAddNode(graph, &dls, &synth_node);
    if (err != noErr)
    {
	dls.componentSubType = kAudioUnitSubType_DLSSynth;
	err = AUGraphAddNode(graph, &dls, &synth_node);
    }
    if (err != noErr)
    {
	fprintf(stderr, "I_InitMusic: no MIDISynth/DLSSynth %d\n", (int)err);
	DisposeAUGraph(graph);
	graph = 0;
	return;
    }
    err = AUGraphAddNode(graph, &outd, &out_node);
    if (err != noErr)
    {
	fprintf(stderr, "I_InitMusic: output node %d\n", (int)err);
	DisposeAUGraph(graph);
	graph = 0;
	return;
    }
    err = AUGraphOpen(graph);
    if (err != noErr)
    {
	fprintf(stderr, "I_InitMusic: AUGraphOpen %d\n", (int)err);
	DisposeAUGraph(graph);
	graph = 0;
	return;
    }
    AUGraphNodeInfo(graph, synth_node, NULL, &synth);
    AUGraphNodeInfo(graph, out_node, NULL, &output_au);
    err = AUGraphConnectNodeInput(graph, synth_node, 0, out_node, 0);
    if (err != noErr)
    {
	fprintf(stderr, "I_InitMusic: connect %d\n", (int)err);
	DisposeAUGraph(graph);
	graph = 0;
	synth = 0;
	output_au = 0;
	return;
    }
    err = AUGraphInitialize(graph);
    if (err != noErr)
    {
	fprintf(stderr, "I_InitMusic: AUGraphInitialize %d\n", (int)err);
	DisposeAUGraph(graph);
	graph = 0;
	synth = 0;
	output_au = 0;
	return;
    }
    AUGraphStart(graph);
    NewMusicPlayer(&player);
    music_ready = 1;
    apply_vol();
    fprintf(stderr, "I_InitMusic: CoreAudio MIDI ready\n");
    fflush(stderr);
    if (song_midi && song_midi_len >= 14)
	I_PlaySong(1, music_loop);
}

void I_ShutdownMusic(void)
{
    I_StopSong(1);
    if (player)
	DisposeMusicPlayer(player);
    player = 0;
    if (sequence)
	DisposeMusicSequence(sequence);
    sequence = 0;
    if (graph)
    {
	AUGraphStop(graph);
	AUGraphUninitialize(graph);
	DisposeAUGraph(graph);
    }
    graph = 0;
    synth = 0;
    output_au = 0;
    free(song_midi);
    song_midi = NULL;
    song_midi_len = 0;
    music_ready = 0;
}

void I_SetMusicVolume(int volume)
{
    /* linuxdoom S_SetMusicVolume hits 127 then the 0..15 menu value. */
    if (volume == 127)
	return;
    music_vol = volume;
    apply_vol();
}

void I_PauseSong(int handle)
{
    (void)handle;
    if (player && music_playing)
	MusicPlayerStop(player);
}

void I_ResumeSong(int handle)
{
    (void)handle;
    if (player && music_playing)
	MusicPlayerStart(player);
}

int I_RegisterSong(void *data)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned char *midi = NULL;
    int len = 0;

    free(song_midi);
    song_midi = NULL;
    song_midi_len = 0;
    if (!p)
	return 1;

    if (memcmp(p, "MThd", 4) == 0)
    {
	len = midi_bytes(p, 8 * 1024 * 1024);
	if (len < 14)
	    return 1;
	midi = (unsigned char *)malloc((size_t)len);
	if (!midi)
	    return 1;
	memcpy(midi, p, (size_t)len);
    }
    else if (memcmp(p, "MUS\x1a", 4) == 0)
    {
	if (mus_to_midi(p, &midi, &len) != 0 || !midi)
	    return 1;
    }
    else
    {
	fprintf(stderr, "I_RegisterSong: not MIDI or MUS\n");
	return 1;
    }
    song_midi = midi;
    song_midi_len = len;
    fprintf(stderr, "I_RegisterSong: %d midi bytes\n", len);
    fflush(stderr);
    return 1;
}

void I_PlaySong(int handle, int looping)
{
    CFDataRef cf;
    OSStatus err;

    (void)handle;
    music_loop = looping;
    if (!music_ready || !player || !song_midi || song_midi_len < 14)
    {
	fprintf(stderr, "I_PlaySong: defer loop=%d ready=%d midi=%d\n",
		looping, music_ready, song_midi_len);
	fflush(stderr);
	return;
    }

    I_StopSong(1);

    err = NewMusicSequence(&sequence);
    if (err != noErr)
	return;
    cf = CFDataCreate(kCFAllocatorDefault, song_midi, (CFIndex)song_midi_len);
    if (!cf)
    {
	DisposeMusicSequence(sequence);
	sequence = 0;
	return;
    }
    err = MusicSequenceFileLoadData(sequence, cf, kMusicSequenceFile_MIDIType, 0);
    CFRelease(cf);
    if (err != noErr)
    {
	fprintf(stderr, "I_PlaySong: MusicSequenceFileLoadData %d\n", (int)err);
	DisposeMusicSequence(sequence);
	sequence = 0;
	return;
    }
    MusicSequenceSetAUGraph(sequence, graph);
    if (looping)
	loop_tracks(sequence);
    MusicPlayerSetSequence(player, sequence);
    MusicPlayerSetTime(player, 0);
    apply_vol();
    err = MusicPlayerStart(player);
    if (err != noErr)
    {
	fprintf(stderr, "I_PlaySong: MusicPlayerStart %d\n", (int)err);
	return;
    }
    music_playing = 1;
    apply_vol();
    fprintf(stderr, "I_PlaySong: started loop=%d vol=%d\n", looping, music_vol);
    fflush(stderr);
}

void I_StopSong(int handle)
{
    (void)handle;
    if (player)
	MusicPlayerStop(player);
    if (sequence)
    {
	if (player)
	    MusicPlayerSetSequence(player, NULL);
	DisposeMusicSequence(sequence);
	sequence = 0;
    }
    music_playing = 0;
}

void I_UnRegisterSong(int handle)
{
    (void)handle;
    I_StopSong(1);
    free(song_midi);
    song_midi = NULL;
    song_midi_len = 0;
}
