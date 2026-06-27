#ifndef EMU_OVERLAY_SDL_H
#define EMU_OVERLAY_SDL_H

#include "emu_overlay_render.h"

typedef void (*OverlaySdlSwapFn)(void);

// Get the SDL render backend.
// Before calling, set these environment variables:
//   EMU_OVERLAY_FONT — path to TTF font file
EmuOvlRenderBackend* overlay_sdl_get_backend(void);
void overlay_sdl_present_init(int logical_w, int logical_h);
int overlay_sdl_present_active(void);
void overlay_sdl_present_bind_target(void);
void overlay_sdl_present_draw(void);
void overlay_sdl_present_swap(OverlaySdlSwapFn swap_fn);

#endif
