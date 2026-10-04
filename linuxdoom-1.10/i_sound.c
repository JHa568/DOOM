// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	System interface for sound, SDL3 audio stream mixer.
//
//	Sound effects are mixed in-process (no sndserver). Music is
//	not played: MUS lumps need an OPL or MIDI synth, which the
//	original release never shipped either.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>
// SDL3 pulls in <stdbool.h>; DOOM defines its own false/true enum.
#undef true
#undef false

#include "z_zone.h"

#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"

#include "doomdef.h"

#ifdef __GNUG__
#pragma implementation "i_sound.h"
#endif


#define OUTPUT_RATE		44100
#define NUM_CHANNELS		16
#define MIX_FRAMES		512

// DMX sound lump header: format (3), sample rate, sample count.
#define SFX_HEADER_SIZE		8


typedef struct
{
    const byte*	data;		// unsigned 8-bit samples
    unsigned	length;		// in samples
    unsigned	position;	// 16.16 fixed point
    unsigned	step;		// 16.16 fixed point
    int		leftvol;	// 0..127
    int		rightvol;	// 0..127
    int		sfxid;
    int		handle;		// 0 when the channel is free
} mixchannel_t;


static SDL_AudioStream*		audiostream;
static mixchannel_t		mixchannels[NUM_CHANNELS];
static int			nexthandle = 1;

// Pitch 128 is normal speed; each 64 steps is one octave.
static unsigned			steptable[256];


//
// Mix active channels into an interleaved stereo buffer.
// Called with the stream lock held.
//
static void MixFrames (Sint16* out, int frames)
{
    int			i;
    int			c;
    int			left, right;
    int			sample;
    mixchannel_t*	ch;

    for (i=0 ; i<frames ; i++)
    {
	left = right = 0;

	for (c=0 ; c<NUM_CHANNELS ; c++)
	{
	    ch = &mixchannels[c];
	    if (!ch->handle)
		continue;

	    sample = (int)ch->data[ch->position >> 16] - 128;
	    left += sample * ch->leftvol;
	    right += sample * ch->rightvol;

	    ch->position += ch->step;
	    if ((ch->position >> 16) >= ch->length)
		ch->handle = 0;
	}

	// sample is +-128, volume up to 127: one channel is ~ full scale/2.
	left >>= 1;
	right >>= 1;

	if (left > 32767) left = 32767;
	else if (left < -32768) left = -32768;
	if (right > 32767) right = 32767;
	else if (right < -32768) right = -32768;

	*out++ = left;
	*out++ = right;
    }
}


//
// SDL asks for more data on its audio thread; the stream
// lock is held for the duration of this callback.
//
static void MixCallback (void* userdata, SDL_AudioStream* stream,
			 int additional, int total)
{
    static Sint16	buffer[MIX_FRAMES*2];
    int			frames;

    frames = additional / (int)(2 * sizeof(Sint16));
    while (frames > 0)
    {
	int	n = frames < MIX_FRAMES ? frames : MIX_FRAMES;

	MixFrames (buffer, n);
	SDL_PutAudioStreamData (stream, buffer, n * 2 * sizeof(Sint16));
	frames -= n;
    }
}


//
// Separation 0..255, 128 centered; volume 0..15 (snd_SfxVolume range).
//
static void SetChannelParams (mixchannel_t* ch, int vol, int sep)
{
    int		volume;

    volume = vol * 127 / 15;
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;

    // Same panning law as the original linuxdoom mixer.
    sep += 1;
    ch->leftvol = volume - ((volume*sep*sep) >> 16);
    sep = sep - 257;
    ch->rightvol = volume - ((volume*sep*sep) >> 16);

    if (ch->leftvol < 0) ch->leftvol = 0;
    if (ch->rightvol < 0) ch->rightvol = 0;
}


void I_SetChannels (void)
{
    int		i;

    for (i=0 ; i<256 ; i++)
	steptable[i] = (unsigned)(pow (2.0, (i - 128) / 64.0) * 65536.0);
}


//
// Missing lumps (e.g. DOOM II sounds asked for by a DOOM 1
// shareware IWAD) fall back to the pistol, as the original did.
//
int I_GetSfxLumpNum (sfxinfo_t* sfx)
{
    char	namebuf[9];
    int		lump;

    sprintf (namebuf, "ds%s", sfx->name);
    lump = W_CheckNumForName (namebuf);
    if (lump == -1)
	lump = W_GetNumForName ("dspistol");
    return lump;
}


int
I_StartSound
( int		id,
  int		vol,
  int		sep,
  int		pitch,
  int		priority )
{
    sfxinfo_t*		sfx;
    const byte*		lump;
    int			lumplen;
    int			rate;
    unsigned		length;
    int			i;
    int			slot;
    int			handle;
    mixchannel_t*	ch;

    if (!audiostream)
	return 0;

    sfx = &S_sfx[id];
    if (sfx->link)
	sfx = sfx->link;
    if (!sfx->data)
	return 0;

    lump = (const byte *)sfx->data;
    lumplen = W_LumpLength (sfx->lumpnum);
    if (lumplen <= SFX_HEADER_SIZE)
	return 0;

    rate = lump[2] | (lump[3] << 8);
    length = lump[4] | (lump[5] << 8) | (lump[6] << 16) | (lump[7] << 24);
    if (length > (unsigned)(lumplen - SFX_HEADER_SIZE))
	length = lumplen - SFX_HEADER_SIZE;
    if (!rate)
	rate = 11025;
    if (pitch < 0) pitch = 0;
    if (pitch > 255) pitch = 255;

    SDL_LockAudioStream (audiostream);

    // Only one chainsaw / pistol etc. at a time, as in the original.
    slot = -1;
    if (id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful
	|| id == sfx_sawhit || id == sfx_stnmov || id == sfx_pistol)
    {
	for (i=0 ; i<NUM_CHANNELS ; i++)
	    if (mixchannels[i].handle && mixchannels[i].sfxid == id)
		slot = i;
    }

    // Otherwise a free channel, else steal the one closest to done.
    if (slot == -1)
    {
	unsigned	best = 0;

	for (i=0 ; i<NUM_CHANNELS ; i++)
	{
	    if (!mixchannels[i].handle)
	    {
		slot = i;
		break;
	    }
	    if (mixchannels[i].position >= best)
	    {
		best = mixchannels[i].position;
		slot = i;
	    }
	}
    }

    ch = &mixchannels[slot];
    ch->data = lump + SFX_HEADER_SIZE;
    ch->length = length;
    ch->position = 0;
    ch->step = (unsigned)(((unsigned long long)rate * steptable[pitch])
			  / OUTPUT_RATE);
    ch->sfxid = id;
    SetChannelParams (ch, vol, sep);

    handle = nexthandle++;
    if (nexthandle <= 0)
	nexthandle = 1;
    ch->handle = handle;

    SDL_UnlockAudioStream (audiostream);

    return handle;
}


static mixchannel_t* FindChannel (int handle)
{
    int		i;

    if (!handle)
	return NULL;
    for (i=0 ; i<NUM_CHANNELS ; i++)
	if (mixchannels[i].handle == handle)
	    return &mixchannels[i];
    return NULL;
}


void I_StopSound (int handle)
{
    mixchannel_t*	ch;

    if (!audiostream)
	return;
    SDL_LockAudioStream (audiostream);
    ch = FindChannel (handle);
    if (ch)
	ch->handle = 0;
    SDL_UnlockAudioStream (audiostream);
}


int I_SoundIsPlaying (int handle)
{
    int		playing;

    if (!audiostream)
	return 0;
    SDL_LockAudioStream (audiostream);
    playing = FindChannel (handle) != NULL;
    SDL_UnlockAudioStream (audiostream);
    return playing;
}


void
I_UpdateSoundParams
( int	handle,
  int	vol,
  int	sep,
  int	pitch )
{
    mixchannel_t*	ch;

    if (!audiostream)
	return;
    SDL_LockAudioStream (audiostream);
    ch = FindChannel (handle);
    if (ch)
	SetChannelParams (ch, vol, sep);
    SDL_UnlockAudioStream (audiostream);
}


//
// Mixing happens in the audio callback, so these are no-ops.
//
void I_UpdateSound (void)
{
}

void I_SubmitSound (void)
{
}


void I_ShutdownSound (void)
{
    if (!audiostream)
	return;
    SDL_DestroyAudioStream (audiostream);
    audiostream = NULL;
    SDL_QuitSubSystem (SDL_INIT_AUDIO);
}


void I_InitSound (void)
{
    SDL_AudioSpec	spec;
    int			i;

    I_SetChannels ();

    if (M_CheckParm ("-nosound") || M_CheckParm ("-nosfx"))
	return;

    // Cache every effect now; S_StartSoundAtVolume expects
    // sfx->data to be set (the original precached them too).
    for (i=1 ; i<NUMSFX ; i++)
    {
	if (S_sfx[i].link)
	    continue;
	S_sfx[i].lumpnum = I_GetSfxLumpNum (&S_sfx[i]);
	S_sfx[i].data = W_CacheLumpNum (S_sfx[i].lumpnum, PU_STATIC);
    }
    for (i=1 ; i<NUMSFX ; i++)
    {
	if (!S_sfx[i].link)
	    continue;
	S_sfx[i].lumpnum = S_sfx[i].link->lumpnum;
	S_sfx[i].data = S_sfx[i].link->data;
    }

    if (!SDL_InitSubSystem (SDL_INIT_AUDIO))
    {
	fprintf (stderr, "I_InitSound: SDL audio unavailable: %s\n",
		 SDL_GetError ());
	return;
    }

    spec.freq = OUTPUT_RATE;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;

    audiostream = SDL_OpenAudioDeviceStream (SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
					     &spec, MixCallback, NULL);
    if (!audiostream)
    {
	fprintf (stderr, "I_InitSound: could not open audio: %s\n",
		 SDL_GetError ());
	SDL_QuitSubSystem (SDL_INIT_AUDIO);
	return;
    }

    SDL_ResumeAudioStreamDevice (audiostream);
    fprintf (stderr, "I_InitSound: SDL audio, %d Hz, %d channels mixed\n",
	     OUTPUT_RATE, NUM_CHANNELS);
}


//
// MUSIC API. No synth available, so songs are accepted and ignored.
//
void I_InitMusic (void)		{ }
void I_ShutdownMusic (void)	{ }
void I_SetMusicVolume (int volume)	{ snd_MusicVolume = volume; }
void I_PauseSong (int handle)	{ }
void I_ResumeSong (int handle)	{ }
int  I_RegisterSong (void* data)	{ return 1; }
void I_PlaySong (int handle, int looping)	{ }
void I_StopSong (int handle)	{ }
void I_UnRegisterSong (int handle)	{ }
