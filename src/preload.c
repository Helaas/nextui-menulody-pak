/*
 * menulody_overlay.so — LD_PRELOAD hook for flicker-free overlay rendering.
 *
 * Intercepts SDL_RenderPresent so we can composite the Menulody now-playing
 * pill into the SDL renderer BEFORE the real present call.  The pill is thus
 * part of the GPU-composed frame and presented atomically — zero flicker.
 *
 * The Menulody daemon renders the pill into a shared-memory file; this hook
 * reads committed overlay frames from shared memory, uploads changed pixel
 * data to an SDL texture, and draws it via SDL_RenderCopy right before the
 * real SDL_RenderPresent.
 *
 * SDL functions are resolved at runtime via dlsym (no -lSDL2 linkage needed).
 *
 * Build:  gcc -std=gnu11 -O2 -fPIC -shared -o menulody_overlay.so preload.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "overlay_shm.h"

/* ── SDL constants (avoid pulling in SDL headers) ───────────────── */

#define ML_SDL_PIXELFORMAT_ARGB8888  0x16362004u
#define ML_SDL_TEXTUREACCESS_STREAMING  1
#define ML_SDL_BLENDMODE_BLEND  1

/* ── SDL function pointers (resolved lazily via dlsym) ──────────── */

typedef void  (*fn_SDL_RenderPresent)(void *);
typedef void *(*fn_SDL_CreateTexture)(void *, uint32_t, int, int, int);
typedef int   (*fn_SDL_UpdateTexture)(void *, const void *, const void *, int);
typedef int   (*fn_SDL_SetTextureBlendMode)(void *, int);
typedef int   (*fn_SDL_RenderCopy)(void *, void *, const void *, const void *);
typedef void  (*fn_SDL_DestroyTexture)(void *);

static fn_SDL_RenderPresent       real_present;
static fn_SDL_CreateTexture       pfn_CreateTexture;
static fn_SDL_UpdateTexture       pfn_UpdateTexture;
static fn_SDL_SetTextureBlendMode pfn_SetBlendMode;
static fn_SDL_RenderCopy          pfn_RenderCopy;
static fn_SDL_DestroyTexture      pfn_DestroyTexture;
static int sdl_funcs_ok;

/* ── Shared memory state (lazy init) ────────────────────────────── */

static menulody_overlay_shm_t *shm;
static int shm_fd = -1;
static int shm_ok;

/* ── Cached overlay (avoid re-reading shm every frame) ──────────── */

static uint32_t staging_pixels[MENULODY_SHM_MAX_W * MENULODY_SHM_MAX_H];
static uint32_t cached_pixels[MENULODY_SHM_MAX_W * MENULODY_SHM_MAX_H];
static int cached_x, cached_y, cached_w, cached_h;
static int cached_active;
static uint32_t cached_frame_id = UINT32_MAX;

/* ── Cached SDL texture ─────────────────────────────────────────── */

static void *overlay_texture;     /* SDL_Texture* */
static void *tex_renderer;        /* renderer that created the texture */
static int   tex_w, tex_h;        /* dimensions of current texture */
static uint32_t texture_frame_id = UINT32_MAX;

/* ── Init helpers ───────────────────────────────────────────────── */

static void init_sdl_funcs(void) {
    pfn_CreateTexture = (fn_SDL_CreateTexture)dlsym(RTLD_NEXT, "SDL_CreateTexture");
    pfn_UpdateTexture = (fn_SDL_UpdateTexture)dlsym(RTLD_NEXT, "SDL_UpdateTexture");
    pfn_SetBlendMode  = (fn_SDL_SetTextureBlendMode)dlsym(RTLD_NEXT, "SDL_SetTextureBlendMode");
    pfn_RenderCopy    = (fn_SDL_RenderCopy)dlsym(RTLD_NEXT, "SDL_RenderCopy");
    pfn_DestroyTexture = (fn_SDL_DestroyTexture)dlsym(RTLD_NEXT, "SDL_DestroyTexture");

    sdl_funcs_ok = pfn_CreateTexture && pfn_UpdateTexture &&
                   pfn_SetBlendMode && pfn_RenderCopy && pfn_DestroyTexture;
}

static void init_shm(void) {
    shm_fd = open(MENULODY_SHM_PATH, O_RDONLY);
    if (shm_fd < 0) return;

    shm = (menulody_overlay_shm_t *)mmap(
        NULL, sizeof(menulody_overlay_shm_t),
        PROT_READ, MAP_SHARED, shm_fd, 0);

    if (shm == MAP_FAILED) {
        shm = NULL; close(shm_fd); shm_fd = -1; return;
    }

    if (shm->magic != MENULODY_SHM_MAGIC) {
        munmap(shm, sizeof(menulody_overlay_shm_t));
        shm = NULL; close(shm_fd); shm_fd = -1; return;
    }

    shm_ok = 1;
}

/* ── Overlay cache + draw ───────────────────────────────────────── */

static int refresh_cache(void) {
    int attempt;
    uint32_t seq1, seq2;
    uint32_t new_frame_id;
    int new_active, new_x, new_y, new_w, new_h;

    for (attempt = 0; attempt < 4; attempt++) {
        seq1 = shm->seq;
        __sync_synchronize();
        if (seq1 & 1u)
            continue;

        new_frame_id = shm->frame_id;
        new_active = shm->active;
        new_x      = shm->x;
        new_y      = shm->y;
        new_w      = shm->w;
        new_h      = shm->h;

        if (new_frame_id != cached_frame_id && new_active) {
            if (new_w <= 0 || new_h <= 0 ||
                new_w > MENULODY_SHM_MAX_W || new_h > MENULODY_SHM_MAX_H) {
                continue;
            }
            memcpy(staging_pixels, (const void *)shm->pixels,
                   (size_t)new_w * (size_t)new_h * sizeof(uint32_t));
        }

        __sync_synchronize();
        seq2 = shm->seq;
        if (seq1 != seq2 || (seq2 & 1u))
            continue;

        if (new_frame_id == cached_frame_id)
            return 0;

        if (!new_active) {
            cached_active = 0;
            cached_x = 0;
            cached_y = 0;
            cached_w = 0;
            cached_h = 0;
            cached_frame_id = new_frame_id;
            return 1;
        }

        memcpy(cached_pixels, staging_pixels,
               (size_t)new_w * (size_t)new_h * sizeof(uint32_t));
        cached_active = 1;
        cached_x = new_x;
        cached_y = new_y;
        cached_w = new_w;
        cached_h = new_h;
        cached_frame_id = new_frame_id;
        return 1;
    }

    return 0;
}

static void draw_overlay(void *renderer) {
    typedef struct { int x, y, w, h; } SDL_Rect;
    SDL_Rect dst;

    if (!shm || !shm_ok) {
        if (shm_fd >= 0) return;           /* already tried and failed hard */
        init_shm();
        if (!shm_ok) return;
    }

    refresh_cache();

    if (!cached_active || cached_w <= 0 || cached_h <= 0)
        return;

    /* Recreate texture if renderer or dimensions changed */
    if (overlay_texture &&
        (tex_renderer != renderer || tex_w != cached_w || tex_h != cached_h)) {
        pfn_DestroyTexture(overlay_texture);
        overlay_texture = NULL;
        texture_frame_id = UINT32_MAX;
    }

    if (!overlay_texture) {
        overlay_texture = pfn_CreateTexture(
            renderer, ML_SDL_PIXELFORMAT_ARGB8888,
            ML_SDL_TEXTUREACCESS_STREAMING, cached_w, cached_h);
        if (!overlay_texture) return;
        pfn_SetBlendMode(overlay_texture, ML_SDL_BLENDMODE_BLEND);
        tex_renderer = renderer;
        tex_w = cached_w;
        tex_h = cached_h;
        texture_frame_id = UINT32_MAX;
    }

    if (texture_frame_id != cached_frame_id) {
        pfn_UpdateTexture(overlay_texture, NULL, cached_pixels,
                          cached_w * (int)sizeof(uint32_t));
        texture_frame_id = cached_frame_id;
    }

    dst.x = cached_x;
    dst.y = cached_y;
    dst.w = cached_w;
    dst.h = cached_h;
    pfn_RenderCopy(renderer, overlay_texture, NULL, &dst);
}

/* ── SDL_RenderPresent interposition ────────────────────────────── */

void SDL_RenderPresent(void *renderer) {
    if (__builtin_expect(!real_present, 0)) {
        real_present = (fn_SDL_RenderPresent)dlsym(RTLD_NEXT, "SDL_RenderPresent");
        if (!real_present) return;
        init_sdl_funcs();
    }

    /* Draw overlay INTO the renderer before presenting */
    if (__builtin_expect(sdl_funcs_ok, 1))
        draw_overlay(renderer);

    real_present(renderer);
}
