/* v0.7.15: sdl2-compat (SDL 2's API on SDL 3 - what Arch, Fedora and others
 * ship as "sdl2"): SDL_RenderGetScale() includes the logical-size scale, but
 * SDL_RenderSetScale() sets only the renderer's own one. The viewer saves and
 * restores the scale around its overlays each frame, so in a window bigger
 * than the picture the scale compounded to inf and the picture went black
 * (and the stats overlay drew wrong). This hands back what SDL_RenderSetScale()
 * needs; with SDL 2 itself nothing changes. (From the Arch package's PKGBUILD,
 * which can drop its copy now - the #ifndef keeps the two from colliding.) */
#pragma once
#include <SDL2/SDL.h>
#ifndef SDL_RenderGetScale
static inline void sft_render_get_scale(SDL_Renderer *ren, float *scaleX, float *scaleY)
{
    float sx = 1.0f, sy = 1.0f, ux = 1.0f, uy = 1.0f;
    int lw = 0, lh = 0;
    SDL_RenderGetScale(ren, &sx, &sy);
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    if (lw > 0 && lh > 0) {
        SDL_RenderSetScale(ren, 1.0f, 1.0f);
        SDL_RenderGetScale(ren, &ux, &uy);
        if (ux > 0.0f) sx /= ux;
        if (uy > 0.0f) sy /= uy;
        SDL_RenderSetScale(ren, sx, sy);
    }
    if (scaleX) *scaleX = sx;
    if (scaleY) *scaleY = sy;
}
#define SDL_RenderGetScale sft_render_get_scale
#endif
