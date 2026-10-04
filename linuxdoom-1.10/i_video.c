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
//	DOOM graphics stuff for SDL3 (replaces the X11 driver).
//
//	The 8-bit screen is expanded through the palette into a
//	32-bit texture, scaled up by an integer factor with nearest
//	sampling, then stretched to the window at 4:3 so pixels keep
//	the original aspect ratio.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
// SDL3 pulls in <stdbool.h>; DOOM defines its own false/true enum.
#undef true
#undef false

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"

#include "doomdef.h"

#ifdef __GNUG__
#pragma implementation "i_video.h"
#endif
#include "i_video.h"


static SDL_Window*	window;
static SDL_Renderer*	renderer;
static SDL_Texture*	screentexture;	// SCREENWIDTH x SCREENHEIGHT, palette expanded
static SDL_Texture*	upscaled;	// integer multiple, nearest sampled

static Uint32		colors[256];
static Uint32		rgbbuffer[SCREENWIDTH*SCREENHEIGHT];

static int		multiply = 3;
static int		upscale = 4;
static boolean		mousegrabbed;

// Accumulated mouse motion, posted once per tic.
static float		mousedx;
static float		mousedy;
static int		mousebuttons;

extern int		usemouse;


//
// Translate an SDL key to a DOOM key code.
//
static int xlatekey (SDL_Keycode sym)
{
    switch (sym)
    {
      case SDLK_LEFT:		return KEY_LEFTARROW;
      case SDLK_RIGHT:		return KEY_RIGHTARROW;
      case SDLK_DOWN:		return KEY_DOWNARROW;
      case SDLK_UP:		return KEY_UPARROW;
      case SDLK_ESCAPE:		return KEY_ESCAPE;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:	return KEY_ENTER;
      case SDLK_TAB:		return KEY_TAB;
      case SDLK_F1:		return KEY_F1;
      case SDLK_F2:		return KEY_F2;
      case SDLK_F3:		return KEY_F3;
      case SDLK_F4:		return KEY_F4;
      case SDLK_F5:		return KEY_F5;
      case SDLK_F6:		return KEY_F6;
      case SDLK_F7:		return KEY_F7;
      case SDLK_F8:		return KEY_F8;
      case SDLK_F9:		return KEY_F9;
      case SDLK_F10:		return KEY_F10;
      case SDLK_F11:		return KEY_F11;
      case SDLK_F12:		return KEY_F12;

      case SDLK_BACKSPACE:
      case SDLK_DELETE:		return KEY_BACKSPACE;

      case SDLK_PAUSE:		return KEY_PAUSE;

      case SDLK_KP_PLUS:
      case SDLK_EQUALS:		return KEY_EQUALS;
      case SDLK_KP_MINUS:
      case SDLK_MINUS:		return KEY_MINUS;

      case SDLK_LSHIFT:
      case SDLK_RSHIFT:		return KEY_RSHIFT;

      case SDLK_LCTRL:
      case SDLK_RCTRL:		return KEY_RCTRL;

      case SDLK_LALT:
      case SDLK_RALT:
      case SDLK_LGUI:
      case SDLK_RGUI:		return KEY_RALT;

      default:
	// Printable keys arrive as lowercase ASCII, which is what
	// the game, menus and cheat code matcher expect.
	if (sym >= ' ' && sym <= '~')
	    return sym;
	return 0;
    }
}


//
// The mouse is captured only while actually playing, so the
// cursor is free in menus, while paused and when the window
// is not focused.
//
static void UpdateGrab (void)
{
    boolean	want;

    want = usemouse
	&& !menuactive
	&& !paused
	&& !demoplayback
	&& gamestate == GS_LEVEL
	&& (SDL_GetWindowFlags (window) & SDL_WINDOW_INPUT_FOCUS);

    if (want != mousegrabbed)
    {
	SDL_SetWindowRelativeMouseMode (window, want);
	mousegrabbed = want;
	mousedx = mousedy = 0;
    }
}


static void ToggleFullscreen (void)
{
    SDL_SetWindowFullscreen (window,
			     !(SDL_GetWindowFlags (window) & SDL_WINDOW_FULLSCREEN));
}


static void I_GetEvent (void)
{
    SDL_Event	sdlev;
    event_t	event;

    while (SDL_PollEvent (&sdlev))
    {
	switch (sdlev.type)
	{
	  case SDL_EVENT_QUIT:
	    I_Quit ();
	    break;

	  case SDL_EVENT_KEY_DOWN:
	    // DOOM tracks held keys itself; the X driver turned
	    // autorepeat off, so drop repeats here as well.
	    if (sdlev.key.repeat)
		break;
	    if (sdlev.key.key == SDLK_RETURN
		&& (sdlev.key.mod & SDL_KMOD_ALT))
	    {
		ToggleFullscreen ();
		break;
	    }
	    event.type = ev_keydown;
	    event.data1 = xlatekey (sdlev.key.key);
	    if (event.data1)
		D_PostEvent (&event);
	    break;

	  case SDL_EVENT_KEY_UP:
	    event.type = ev_keyup;
	    event.data1 = xlatekey (sdlev.key.key);
	    if (event.data1)
		D_PostEvent (&event);
	    break;

	  case SDL_EVENT_MOUSE_BUTTON_DOWN:
	  case SDL_EVENT_MOUSE_BUTTON_UP:
	    {
		int	bit;

		switch (sdlev.button.button)
		{
		  case SDL_BUTTON_LEFT:	  bit = 1; break;
		  case SDL_BUTTON_RIGHT:  bit = 2; break;
		  case SDL_BUTTON_MIDDLE: bit = 4; break;
		  default:		  bit = 0; break;
		}
		if (sdlev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		    mousebuttons |= bit;
		else
		    mousebuttons &= ~bit;

		if (!mousegrabbed)
		    break;
		event.type = ev_mouse;
		event.data1 = mousebuttons;
		event.data2 = event.data3 = 0;
		D_PostEvent (&event);
	    }
	    break;

	  case SDL_EVENT_MOUSE_MOTION:
	    if (mousegrabbed)
	    {
		mousedx += sdlev.motion.xrel;
		mousedy += sdlev.motion.yrel;
	    }
	    break;

	  default:
	    break;
	}
    }
}


//
// I_StartFrame
//
void I_StartFrame (void)
{
    // er?
}


//
// I_StartTic
//
void I_StartTic (void)
{
    event_t	event;

    if (!window)
	return;

    I_GetEvent ();
    UpdateGrab ();

    if (mousegrabbed && ((int)mousedx || (int)mousedy))
    {
	event.type = ev_mouse;
	event.data1 = mousebuttons;
	event.data2 = (int)mousedx * 4;
	event.data3 = -(int)mousedy * 4;
	D_PostEvent (&event);
	// keep sub-pixel remainders for the next tic
	mousedx -= (int)mousedx;
	mousedy -= (int)mousedy;
    }
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
    // what is this?
}


//
// I_FinishUpdate
//
void I_FinishUpdate (void)
{
    static int	lasttic;
    int		tics;
    int		i;
    byte*	src;

    // draws little dots on the bottom of the screen
    if (devparm)
    {
	i = I_GetTime();
	tics = i - lasttic;
	lasttic = i;
	if (tics > 20) tics = 20;

	for (i=0 ; i<tics*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0xff;
	for ( ; i<20*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0x0;
    }

    src = screens[0];
    for (i=0 ; i<SCREENWIDTH*SCREENHEIGHT ; i++)
	rgbbuffer[i] = colors[src[i]];

    SDL_UpdateTexture (screentexture, NULL, rgbbuffer,
		       SCREENWIDTH * sizeof(Uint32));

    SDL_SetRenderTarget (renderer, upscaled);
    SDL_RenderTexture (renderer, screentexture, NULL, NULL);
    SDL_SetRenderTarget (renderer, NULL);

    SDL_RenderClear (renderer);
    SDL_RenderTexture (renderer, upscaled, NULL, NULL);
    SDL_RenderPresent (renderer);
}


//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, screens[0], SCREENWIDTH*SCREENHEIGHT);
}


//
// I_SetPalette
//
void I_SetPalette (byte* palette)
{
    int		i;
    int		r, g, b;

    for (i=0 ; i<256 ; i++)
    {
	r = gammatable[usegamma][*palette++];
	g = gammatable[usegamma][*palette++];
	b = gammatable[usegamma][*palette++];
	colors[i] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
}


void I_ShutdownGraphics (void)
{
    if (!window)
	return;

    SDL_SetWindowRelativeMouseMode (window, 0);
    SDL_DestroyTexture (upscaled);
    SDL_DestroyTexture (screentexture);
    SDL_DestroyRenderer (renderer);
    SDL_DestroyWindow (window);
    window = NULL;
    SDL_QuitSubSystem (SDL_INIT_VIDEO);
}


void I_InitGraphics (void)
{
    static int	firsttime = 1;
    SDL_WindowFlags	flags;

    if (!firsttime)
	return;
    firsttime = 0;

    if (M_CheckParm("-2"))
	multiply = 2;
    if (M_CheckParm("-3"))
	multiply = 3;
    if (M_CheckParm("-4"))
	multiply = 4;

    if (!SDL_InitSubSystem (SDL_INIT_VIDEO))
	I_Error ("Could not initialize SDL video: %s", SDL_GetError ());

    flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (M_CheckParm("-fullscreen"))
	flags |= SDL_WINDOW_FULLSCREEN;

    // 4:3 window: DOOM's 320x200 was shown on 4:3 monitors.
    window = SDL_CreateWindow ("DOOM",
			       SCREENWIDTH*multiply,
			       SCREENWIDTH*multiply*3/4,
			       flags);
    if (!window)
	I_Error ("Could not create window: %s", SDL_GetError ());

    renderer = SDL_CreateRenderer (window, NULL);
    if (!renderer)
	I_Error ("Could not create renderer: %s", SDL_GetError ());

    SDL_SetRenderVSync (renderer, 1);
    SDL_SetRenderLogicalPresentation (renderer, SCREENWIDTH*upscale,
				      SCREENWIDTH*upscale*3/4,
				      SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor (renderer, 0, 0, 0, 255);

    screentexture = SDL_CreateTexture (renderer,
				       SDL_PIXELFORMAT_ARGB8888,
				       SDL_TEXTUREACCESS_STREAMING,
				       SCREENWIDTH, SCREENHEIGHT);

    upscaled = SDL_CreateTexture (renderer,
				  SDL_PIXELFORMAT_ARGB8888,
				  SDL_TEXTUREACCESS_TARGET,
				  SCREENWIDTH*upscale,
				  SCREENHEIGHT*upscale);
    if (!screentexture || !upscaled)
	I_Error ("Could not create textures: %s", SDL_GetError ());

    SDL_SetTextureScaleMode (screentexture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode (upscaled, SDL_SCALEMODE_LINEAR);

    // Rendering goes straight into screens[0]; no separate
    // framebuffer is needed since we convert on every update.
}
