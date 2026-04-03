#include "overlay.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#endif

/* ── Framebuffer state ──────────────────────────────────────────── */

static int fb_fd = -1;
static unsigned short *fb_mem = NULL;
static int fb_width  = 0;
static int fb_height = 0;
static int fb_stride = 0;   /* in pixels */
static int fb_mapped_size = 0;

/* Banner state */
static char   banner_text[300] = {0};
static time_t banner_expire = 0;
static int    banner_active = 0;
static int    banner_drawn = 0;
static int    banner_x = 0;
static int    banner_y = 0;
static int    banner_w = 0;
static int    banner_h = 0;
static unsigned short *banner_saved_bg = NULL;
static unsigned short *banner_snapshot = NULL;
static unsigned short *banner_work = NULL;
static size_t banner_buffer_pixels = 0;

/* ── 8x16 VGA bitmap font ─────────────────────────────────────── */

#include "font8x16.h"

#ifdef __linux__

typedef struct {
    int x;
    int y;
    int w;
    int h;
    int scale;
    int char_w;
    int char_h;
    char text[300];
} overlay_layout_t;

static unsigned short rgb565(int r, int g, int b) {
    return (unsigned short)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static unsigned short blend565(unsigned short bg, unsigned short fg, int alpha) {
    int bg_r = (bg >> 11) & 0x1F;
    int bg_g = (bg >> 5)  & 0x3F;
    int bg_b =  bg        & 0x1F;
    int fg_r = (fg >> 11) & 0x1F;
    int fg_g = (fg >> 5)  & 0x3F;
    int fg_b =  fg        & 0x1F;
    int r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    int g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
    int b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
    return (unsigned short)((r << 11) | (g << 5) | b);
}

static int ensure_banner_buffers(size_t pixels) {
    if (pixels <= banner_buffer_pixels) return 0;

    unsigned short *new_saved = malloc(pixels * sizeof(unsigned short));
    unsigned short *new_snap = malloc(pixels * sizeof(unsigned short));
    unsigned short *new_work = malloc(pixels * sizeof(unsigned short));
    if (!new_saved || !new_snap || !new_work) {
        free(new_saved);
        free(new_snap);
        free(new_work);
        return -1;
    }

    if (banner_saved_bg)
        memcpy(new_saved, banner_saved_bg, banner_buffer_pixels * sizeof(unsigned short));
    if (banner_snapshot)
        memcpy(new_snap, banner_snapshot, banner_buffer_pixels * sizeof(unsigned short));
    if (banner_work)
        memcpy(new_work, banner_work, banner_buffer_pixels * sizeof(unsigned short));

    free(banner_saved_bg);
    free(banner_snapshot);
    free(banner_work);
    banner_saved_bg = new_saved;
    banner_snapshot = new_snap;
    banner_work = new_work;
    banner_buffer_pixels = pixels;
    return 0;
}

static void copy_fb_rect(int x, int y, int w, int h, unsigned short *dst) {
    for (int row = 0; row < h; row++) {
        memcpy(dst + (size_t)row * w,
               fb_mem + (size_t)(y + row) * fb_stride + x,
               (size_t)w * sizeof(unsigned short));
    }
}

static void write_fb_rect(int x, int y, int w, int h, const unsigned short *src) {
    for (int row = 0; row < h; row++) {
        memcpy(fb_mem + (size_t)(y + row) * fb_stride + x,
               src + (size_t)row * w,
               (size_t)w * sizeof(unsigned short));
    }
}

static int rect_matches_buffer(int x, int y, int w, int h, const unsigned short *buf) {
    for (int row = 0; row < h; row++) {
        const unsigned short *fb_row = fb_mem + (size_t)(y + row) * fb_stride + x;
        const unsigned short *buf_row = buf + (size_t)row * w;
        if (memcmp(fb_row, buf_row, (size_t)w * sizeof(unsigned short)) != 0)
            return 0;
    }
    return 1;
}

static void clear_drawn_overlay_if_still_visible(void) {
    if (!banner_drawn || !banner_saved_bg || !banner_snapshot || banner_w <= 0 || banner_h <= 0)
        return;
    if (rect_matches_buffer(banner_x, banner_y, banner_w, banner_h, banner_snapshot))
        write_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_saved_bg);
    banner_drawn = 0;
}

static void restore_overlay_region_best_effort(void) {
    size_t pixels = (size_t)banner_w * banner_h;
    if (!banner_drawn || !banner_saved_bg || !banner_snapshot || !banner_work || pixels == 0)
        return;

    copy_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_work);
    for (size_t i = 0; i < pixels; i++) {
        if (banner_work[i] == banner_snapshot[i])
            banner_work[i] = banner_saved_bg[i];
    }
    write_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_work);
    banner_drawn = 0;
}

static void capture_background_for_redraw(void) {
    size_t pixels = (size_t)banner_w * banner_h;
    if (pixels == 0 || !banner_saved_bg || !banner_snapshot || !banner_work)
        return;

    copy_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_work);
    if (!banner_drawn) {
        memcpy(banner_saved_bg, banner_work, pixels * sizeof(unsigned short));
        return;
    }

    for (size_t i = 0; i < pixels; i++) {
        banner_saved_bg[i] = (banner_work[i] == banner_snapshot[i])
            ? banner_saved_bg[i]
            : banner_work[i];
    }
}

static void draw_char_scaled(int x0, int y0, char ch, unsigned short color, int scale) {
    if (ch < 32 || ch > 126) ch = '?';

    int idx = ch - 32;
    const unsigned char *glyph = font8x16_data[idx];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    int px = x0 + col * scale + dx;
                    int py = y0 + row * scale + dy;
                    if (px >= 0 && px < fb_width && py >= 0 && py < fb_height)
                        fb_mem[(size_t)py * fb_stride + px] = color;
                }
            }
        }
    }
}

static void draw_rounded_rect_alpha(int x0, int y0, int w, int h,
                                    int radius, unsigned short color, int alpha) {
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    for (int py = 0; py < h; py++) {
        int y = y0 + py;
        if (y < 0 || y >= fb_height) continue;
        for (int px = 0; px < w; px++) {
            int x = x0 + px;
            int draw = 1;
            if (x < 0 || x >= fb_width) continue;

            if (px < radius && py < radius) {
                int dx = radius - px - 1;
                int dy = radius - py - 1;
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px >= w - radius && py < radius) {
                int dx = px - (w - radius);
                int dy = radius - py - 1;
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px < radius && py >= h - radius) {
                int dx = radius - px - 1;
                int dy = py - (h - radius);
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px >= w - radius && py >= h - radius) {
                int dx = px - (w - radius);
                int dy = py - (h - radius);
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            }

            if (draw) {
                unsigned short bg = fb_mem[(size_t)y * fb_stride + x];
                fb_mem[(size_t)y * fb_stride + x] = blend565(bg, color, alpha);
            }
        }
    }
}

static int build_layout(overlay_layout_t *layout) {
    int scale;
    int char_w;
    int char_h;
    int padding_x;
    int padding_y;
    int max_pill_w;
    int max_chars;
    int text_chars;

    if (!layout || !fb_mem || !banner_text[0]) return -1;

    memset(layout, 0, sizeof(*layout));
    scale = (fb_width >= 1024) ? 2 : 1;
    char_w = 8 * scale;
    char_h = 16 * scale;
    padding_x = 16 * scale;
    padding_y = 6 * scale;
    max_pill_w = fb_width * 80 / 100;
    max_chars = (max_pill_w - padding_x * 2) / char_w;
    if (max_chars < 1) return -1;

    if ((int)strlen(banner_text) > max_chars && max_chars > 3) {
        snprintf(layout->text, sizeof(layout->text), "%.*s...", max_chars - 3, banner_text);
    } else {
        snprintf(layout->text, sizeof(layout->text), "%.*s", max_chars, banner_text);
    }

    text_chars = (int)strlen(layout->text);
    layout->scale = scale;
    layout->char_w = char_w;
    layout->char_h = char_h;
    layout->w = text_chars * char_w + padding_x * 2;
    layout->h = char_h + padding_y * 2;
    layout->x = (fb_width - layout->w) / 2;
    layout->y = 10 * scale;

    if (layout->x < 0) layout->x = 0;
    if (layout->y < 0) layout->y = 0;
    if (layout->x + layout->w > fb_width) layout->w = fb_width - layout->x;
    if (layout->y + layout->h > fb_height) layout->h = fb_height - layout->y;
    return (layout->w > 0 && layout->h > 0) ? 0 : -1;
}

static void draw_pill(const overlay_layout_t *layout) {
    unsigned short bg_color;
    unsigned short text_color;
    int text_w;
    int text_x;
    int text_y;
    int radius;

    if (!layout) return;

    bg_color = rgb565(30, 30, 40);
    text_color = rgb565(240, 240, 255);
    radius = 10 * layout->scale;
    draw_rounded_rect_alpha(layout->x, layout->y, layout->w, layout->h, radius, bg_color, 220);

    text_w = (int)strlen(layout->text) * layout->char_w;
    text_x = layout->x + (layout->w - text_w) / 2;
    text_y = layout->y + (layout->h - layout->char_h) / 2;

    for (const char *s = layout->text; *s; s++) {
        draw_char_scaled(text_x, text_y, *s, text_color, layout->scale);
        text_x += layout->char_w;
    }
}

static void draw_current_banner(void) {
    overlay_layout_t layout;
    size_t pixels;

    if (build_layout(&layout) < 0) return;

    if (banner_drawn &&
        (banner_x != layout.x || banner_y != layout.y ||
         banner_w != layout.w || banner_h != layout.h)) {
        restore_overlay_region_best_effort();
    }

    banner_x = layout.x;
    banner_y = layout.y;
    banner_w = layout.w;
    banner_h = layout.h;
    pixels = (size_t)banner_w * banner_h;
    if (ensure_banner_buffers(pixels) < 0) return;

    capture_background_for_redraw();
    draw_pill(&layout);
    copy_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_snapshot);
    banner_drawn = 1;
}

#endif /* __linux__ */

/* ── Public API ─────────────────────────────────────────────────── */

int overlay_init(void) {
#ifdef __linux__
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "menulody: overlay: cannot open /dev/fb0 (non-fatal)\n");
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("menulody: overlay: FBIOGET_VSCREENINFO");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fb_width = (int)vinfo.xres;
    fb_height = (int)vinfo.yres;

    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("menulody: overlay: FBIOGET_FSCREENINFO");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fb_stride = (int)(finfo.line_length / 2);
    fb_mapped_size = (int)(finfo.line_length * vinfo.yres);

    fb_mem = (unsigned short *)mmap(NULL, fb_mapped_size,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("menulody: overlay: mmap fb0");
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fprintf(stderr, "menulody: overlay: fb0 %dx%d stride=%d\n",
            fb_width, fb_height, fb_stride);
    return 0;
#else
    return -1;
#endif
}

void overlay_set_text(const char *text, int duration_secs) {
#ifdef __linux__
    if (fb_mem && banner_drawn)
        restore_overlay_region_best_effort();
#endif

    if (text && text[0] && duration_secs > 0) {
        str_copy_trunc(banner_text, sizeof(banner_text), text);
        banner_active = 1;
        banner_expire = time(NULL) + duration_secs;
    } else {
        banner_text[0] = '\0';
        banner_active = 0;
        banner_expire = 0;
    }

    banner_drawn = 0;
    banner_x = 0;
    banner_y = 0;
    banner_w = 0;
    banner_h = 0;
}

void overlay_tick(int menu_active) {
#ifdef __linux__
    if (!fb_mem || !banner_active) return;

    if (time(NULL) >= banner_expire) {
        clear_drawn_overlay_if_still_visible();
        banner_active = 0;
        banner_text[0] = '\0';
        banner_expire = 0;
        return;
    }

    if (!menu_active) return;

    if (!banner_drawn) {
        draw_current_banner();
        return;
    }

    if (!rect_matches_buffer(banner_x, banner_y, banner_w, banner_h, banner_snapshot))
        draw_current_banner();
#else
    (void)menu_active;
#endif
}

void overlay_cleanup(void) {
#ifdef __linux__
    if (fb_mem && banner_drawn)
        restore_overlay_region_best_effort();
    free(banner_saved_bg);
    free(banner_snapshot);
    free(banner_work);
    banner_saved_bg = NULL;
    banner_snapshot = NULL;
    banner_work = NULL;
    banner_buffer_pixels = 0;
    if (fb_mem) {
        munmap(fb_mem, fb_mapped_size);
        fb_mem = NULL;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
#endif
    banner_active = 0;
    banner_drawn = 0;
    banner_text[0] = '\0';
}
