/*
 * menulody_overlay.so — LD_PRELOAD hook for flicker-free overlay rendering.
 *
 * Intercepts SDL_RenderPresent so we can blit the Menulody now-playing pill
 * onto the framebuffer after every NextUI frame.  The Menulody daemon renders
 * the pill into a shared-memory file; this hook reads it and alpha-blends it
 * onto /dev/fb0.
 *
 * Build:  gcc -std=gnu11 -O2 -fPIC -shared -o menulody_overlay.so preload.c -ldl
 * No SDL dependency — only libc and libdl.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "overlay_shm.h"

/* ── Real SDL_RenderPresent ─────────────────────────────────────── */

typedef void (*sdl_render_present_fn)(void *);
static sdl_render_present_fn real_present;

/* ── Framebuffer state (lazy init) ──────────────────────────────── */

static uint32_t *fb_mem;
static int fb_fd       = -1;
static int fb_width;
static int fb_height;
static int fb_stride;       /* pixels per row */
static int fb_mapped_size;
static int fb_ok;

/* ── Shared memory state (lazy init) ────────────────────────────── */

static menulody_overlay_shm_t *shm;
static int shm_fd = -1;
static int shm_ok;

/* ── Cached overlay (avoid re-reading shm every frame) ──────────── */

static uint32_t cached_pixels[MENULODY_SHM_MAX_W * MENULODY_SHM_MAX_H];
static int cached_x, cached_y, cached_w, cached_h;
static int cached_active;
static int cached_version = -1;

/* ── Init helpers ───────────────────────────────────────────────── */

static void init_fb(void) {
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return;

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        vinfo.bits_per_pixel != 32) {
        close(fb_fd); fb_fd = -1; return;
    }

    fb_width  = (int)vinfo.xres;
    fb_height = (int)vinfo.yres;

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(fb_fd); fb_fd = -1; return;
    }

    fb_stride      = (int)(finfo.line_length / 4);
    fb_mapped_size = (int)(finfo.line_length * vinfo.yres);

    fb_mem = (uint32_t *)mmap(NULL, (size_t)fb_mapped_size,
                              PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        fb_mem = NULL; close(fb_fd); fb_fd = -1; return;
    }

    fb_ok = 1;
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

/* ── Overlay blit ───────────────────────────────────────────────── */

static void refresh_cache(void) {
    int v1, v2;

    v1 = shm->version;
    __sync_synchronize();

    cached_active = shm->active;
    cached_x      = shm->x;
    cached_y      = shm->y;
    cached_w      = shm->w;
    cached_h      = shm->h;

    if (cached_active && cached_w > 0 && cached_h > 0 &&
        cached_w <= MENULODY_SHM_MAX_W && cached_h <= MENULODY_SHM_MAX_H) {
        memcpy(cached_pixels, (const void *)shm->pixels,
               (size_t)cached_w * (size_t)cached_h * sizeof(uint32_t));
    }

    __sync_synchronize();
    v2 = shm->version;

    if (v1 != v2) {
        /* Writer was active during our read — stale, skip this frame. */
        cached_active = 0;
        return;
    }

    cached_version = v1;
}

static void blit_overlay(void) {
    int x0, y0, w, h;
    const uint32_t *src;
    uint32_t *dst_row;

    if (!shm || !shm_ok) {
        /* Try (re)opening shm — daemon may have started after us. */
        if (shm_fd >= 0) return;           /* already tried and failed hard */
        init_shm();
        if (!shm_ok) return;
    }

    if (shm->version != cached_version)
        refresh_cache();

    if (!cached_active) return;

    x0 = cached_x;
    y0 = cached_y;
    w  = cached_w;
    h  = cached_h;

    /* Sanity checks */
    if (w <= 0 || h <= 0) return;
    if (x0 < 0 || y0 < 0) return;
    if (x0 + w > fb_width || y0 + h > fb_height) return;

    src = cached_pixels;
    for (int row = 0; row < h; row++) {
        dst_row = fb_mem + (size_t)(y0 + row) * fb_stride + x0;
        for (int col = 0; col < w; col++) {
            uint32_t px = src[row * w + col];
            uint32_t a  = (px >> 24) & 0xFF;

            if (a == 0) continue;

            if (a == 255) {
                dst_row[col] = px;
            } else {
                /* Alpha blend over framebuffer pixel */
                uint32_t bg  = dst_row[col];
                uint32_t inv = 255 - a;
                uint32_t rb  = (((px & 0x00FF00FFu) * a +
                                 (bg & 0x00FF00FFu) * inv) >> 8) & 0x00FF00FFu;
                uint32_t g   = (((px & 0x0000FF00u) * a +
                                 (bg & 0x0000FF00u) * inv) >> 8) & 0x0000FF00u;
                dst_row[col] = 0xFF000000u | rb | g;
            }
        }
    }
}

/* ── SDL_RenderPresent interposition ────────────────────────────── */

void SDL_RenderPresent(void *renderer) {
    if (__builtin_expect(!real_present, 0)) {
        real_present = (sdl_render_present_fn)dlsym(RTLD_NEXT, "SDL_RenderPresent");
        if (!real_present) return;
    }

    real_present(renderer);

    if (__builtin_expect(!fb_ok, 0))
        init_fb();

    if (fb_ok)
        blit_overlay();
}
