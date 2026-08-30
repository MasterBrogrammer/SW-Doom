#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <SDL.h>

#include "doomstat.h"
#include "i_system.h"
#include "i_sound.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "doomdef.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_PixelFormat *pxfmt;
static uint32_t palette_rgb[256];
static int multiply = 3;
static int walk_posted;
static int walk_until;
static int fire_posted;
static int mouse_buttons;

static int xlatekey(SDL_Keycode k)
{
    switch (k)
    {
      case SDLK_LEFT: return KEY_LEFTARROW;
      case SDLK_RIGHT: return KEY_RIGHTARROW;
      case SDLK_DOWN: return KEY_DOWNARROW;
      case SDLK_UP: return KEY_UPARROW;
      case SDLK_ESCAPE: return KEY_ESCAPE;
      case SDLK_RETURN: return KEY_ENTER;
      case SDLK_TAB: return KEY_TAB;
      case SDLK_F1: return KEY_F1;
      case SDLK_F2: return KEY_F2;
      case SDLK_F3: return KEY_F3;
      case SDLK_F4: return KEY_F4;
      case SDLK_F5: return KEY_F5;
      case SDLK_F6: return KEY_F6;
      case SDLK_F7: return KEY_F7;
      case SDLK_F8: return KEY_F8;
      case SDLK_F9: return KEY_F9;
      case SDLK_F10: return KEY_F10;
      case SDLK_F11: return KEY_F11;
      case SDLK_F12: return KEY_F12;
      case SDLK_BACKSPACE:
      case SDLK_DELETE: return KEY_BACKSPACE;
      case SDLK_PAUSE: return KEY_PAUSE;
      case SDLK_EQUALS: return KEY_EQUALS;
      case SDLK_MINUS: return KEY_MINUS;
      case SDLK_LSHIFT:
      case SDLK_RSHIFT: return KEY_RSHIFT;
      case SDLK_LCTRL:
      case SDLK_RCTRL: return KEY_RCTRL;
      case SDLK_LALT:
      case SDLK_RALT: return KEY_RALT;
      default:
	if (k >= SDLK_SPACE && k <= SDLK_BACKQUOTE)
	    return (int)k;
	if (k >= SDLK_a && k <= SDLK_z)
	    return (int)k;
	return (int)k;
    }
}

void I_ShutdownGraphics(void)
{
    if (pxfmt)
	SDL_FreeFormat(pxfmt);
    pxfmt = NULL;
    if (texture)
	SDL_DestroyTexture(texture);
    texture = NULL;
    if (renderer)
	SDL_DestroyRenderer(renderer);
    renderer = NULL;
    if (window)
	SDL_DestroyWindow(window);
    window = NULL;
    SDL_Quit();
}

void I_StartFrame(void)
{
}

static void poste(int type, int data1)
{
    event_t event;
    event.type = type;
    event.data1 = data1;
    event.data2 = 0;
    event.data3 = 0;
    D_PostEvent(&event);
}

static void poste3(int type, int data1, int data2, int data3)
{
    event_t event;
    event.type = type;
    event.data1 = data1;
    event.data2 = data2;
    event.data3 = data3;
    D_PostEvent(&event);
}

static void mouse_set(int button, int down)
{
    int bit = 0;
    if (button == SDL_BUTTON_LEFT)
	bit = 1;
    else if (button == SDL_BUTTON_RIGHT)
	bit = 2;
    else if (button == SDL_BUTTON_MIDDLE)
	bit = 4;
    else
	return;
    if (down)
	mouse_buttons |= bit;
    else
	mouse_buttons &= ~bit;
    poste3(ev_mouse, mouse_buttons, 0, 0);
}

void I_GetEvent(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
	switch (ev.type)
	{
	  case SDL_QUIT:
	    I_Quit();
	    break;
	  case SDL_KEYDOWN:
	    poste(ev_keydown, xlatekey(ev.key.keysym.sym));
	    break;
	  case SDL_KEYUP:
	    poste(ev_keyup, xlatekey(ev.key.keysym.sym));
	    break;
	  case SDL_MOUSEBUTTONDOWN:
	    mouse_set(ev.button.button, 1);
	    break;
	  case SDL_MOUSEBUTTONUP:
	    mouse_set(ev.button.button, 0);
	    break;
	  case SDL_MOUSEMOTION:
	    poste3(ev_mouse, mouse_buttons,
		   ev.motion.xrel * 4, -ev.motion.yrel * 4);
	    break;
	  default:
	    break;
	}
    }
}

void I_StartTic(void)
{
    I_GetEvent();
    if (M_CheckParm("-walk") && gamestate == GS_LEVEL)
    {
	if (!walk_posted)
	{
	    walk_posted = 1;
	    walk_until = gametic + 70 * 3;
	    poste(ev_keydown, KEY_UPARROW);
	}
	else if (gametic >= walk_until)
	{
	    poste(ev_keyup, KEY_UPARROW);
	    walk_until = 0x7fffffff;
	}
    }
    if (M_CheckParm("-fire") && gamestate == GS_LEVEL && !fire_posted)
    {
	fire_posted = 1;
	poste(ev_keydown, KEY_RCTRL);
    }
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
    int pitch;
    void *pixels;
    unsigned char *src;
    uint32_t *dst;
    int i;
    int n;

    if (!texture || !screens[0])
	return;

    if (SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0)
	return;

    src = screens[0];
    dst = (uint32_t *)pixels;
    n = SCREENWIDTH * SCREENHEIGHT;
    if (pitch == SCREENWIDTH * 4)
    {
	for (i = 0; i < n; i++)
	    dst[i] = palette_rgb[src[i]];
    }
    else
    {
	int y;
	for (y = 0; y < SCREENHEIGHT; y++)
	{
	    uint32_t *row = (uint32_t *)((unsigned char *)pixels + y * pitch);
	    unsigned char *srow = src + y * SCREENWIDTH;
	    for (i = 0; i < SCREENWIDTH; i++)
		row[i] = palette_rgb[srow[i]];
	}
    }
    SDL_UnlockTexture(texture);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    {
	const char *ppm = getenv("SWDOOM_PPM");
	static int ppm_done;
	if (ppm && !ppm_done && gametic > 35 && screens[0])
	{
	    FILE *f = fopen(ppm, "wb");
	    if (f)
	    {
		int x, y;
		fprintf(f, "P6\n%d %d\n255\n", SCREENWIDTH, SCREENHEIGHT);
		for (y = 0; y < SCREENHEIGHT; y++)
		{
		    for (x = 0; x < SCREENWIDTH; x++)
		    {
			uint32_t p = palette_rgb[screens[0][y * SCREENWIDTH + x]];
			unsigned char rgb[3];
			rgb[0] = (unsigned char)((p >> 16) & 0xff);
			rgb[1] = (unsigned char)((p >> 8) & 0xff);
			rgb[2] = (unsigned char)(p & 0xff);
			fwrite(rgb, 1, 3, f);
		    }
		}
		fclose(f);
	    }
	    ppm_done = 1;
	}
    }
}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_SetPalette(byte *palette)
{
    int i;
    if (!pxfmt || !palette)
	return;
    for (i = 0; i < 256; i++)
    {
	int r = gammatable[usegamma][*palette++];
	int g = gammatable[usegamma][*palette++];
	int b = gammatable[usegamma][*palette++];
	palette_rgb[i] = SDL_MapRGB(pxfmt, (Uint8)r, (Uint8)g, (Uint8)b);
    }
}

void I_InitGraphics(void)
{
    int p;
    int w;
    int h;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
	I_Error("SDL_Init: %s", SDL_GetError());

    p = M_CheckParm("-2");
    if (p)
	multiply = 2;
    p = M_CheckParm("-3");
    if (p)
	multiply = 3;
    p = M_CheckParm("-4");
    if (p)
	multiply = 4;

    w = SCREENWIDTH * multiply;
    h = SCREENHEIGHT * multiply;

    window = SDL_CreateWindow(
	"SW-Doom",
	SDL_WINDOWPOS_CENTERED,
	SDL_WINDOWPOS_CENTERED,
	w,
	h,
	SDL_WINDOW_SHOWN);
    if (!window)
	I_Error("SDL_CreateWindow: %s", SDL_GetError());

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
	renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer)
	I_Error("SDL_CreateRenderer: %s", SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(renderer, SCREENWIDTH, SCREENHEIGHT);

    texture = SDL_CreateTexture(
	renderer,
	SDL_PIXELFORMAT_ARGB8888,
	SDL_TEXTUREACCESS_STREAMING,
	SCREENWIDTH,
	SCREENHEIGHT);
    if (!texture)
	I_Error("SDL_CreateTexture: %s", SDL_GetError());

    pxfmt = SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888);
    if (!pxfmt)
	I_Error("SDL_AllocFormat failed");

    if (!screens[0])
	screens[0] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);

    SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_ShowCursor(SDL_DISABLE);
    I_RestartSound();
    I_InitMusic();
}
