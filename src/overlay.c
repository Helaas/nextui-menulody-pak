#include "overlay.h"

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
static char    banner_text[300] = {0};
static time_t  banner_expire = 0;
static int     banner_active = 0;

/* ── 8x16 VGA bitmap font ─────────────────────────────────────── */
/* Subset covering ASCII 32-126 (printable chars). Each glyph is 8 wide × 16 tall.
   Stored as 16 bytes per glyph (one byte per row, MSB=left). */

#include "font8x16.h"

#ifdef __linux__

/* Pack RGB888 to RGB565 */
static unsigned short rgb565(int r, int g, int b) {
    return (unsigned short)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Alpha-blend a pixel on RGB565. alpha: 0-255 */
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

/* Draw a filled rectangle with alpha blending */
static void draw_rect_alpha(int x0, int y0, int w, int h,
                            unsigned short color, int alpha) {
    for (int y = y0; y < y0 + h && y < fb_height; y++) {
        if (y < 0) continue;
        for (int x = x0; x < x0 + w && x < fb_width; x++) {
            if (x < 0) continue;
            unsigned short bg = fb_mem[y * fb_stride + x];
            fb_mem[y * fb_stride + x] = blend565(bg, color, alpha);
        }
    }
}

/* Measure text width in pixels (at 1x scale) */
static int measure_text(const char *str) {
    int w = 0;
    while (*str) {
        w += 8; /* fixed-width 8px per char */
        str++;
    }
    return w;
}

/* Draw a single character at given position. Returns advance. */
static int draw_char(int x0, int y0, char ch, unsigned short color) {
    if (ch < 32 || ch > 126) ch = '?';
    int idx = ch - 32;
    const unsigned char *glyph = font8x16_data[idx];

    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                int px = x0 + col;
                int py = y0 + row;
                if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                    fb_mem[py * fb_stride + px] = color;
                }
            }
        }
    }
    return 8;
}

/* Draw string at position */
static void draw_string(int x, int y, const char *str, unsigned short color) {
    while (*str) {
        x += draw_char(x, y, *str, color);
        str++;
    }
}

/* Draw the centered pill with text */
static void draw_pill(void) {
    if (!fb_mem || !banner_text[0]) return;

    int scale = (fb_width >= 1024) ? 2 : 1;
    int char_w = 8 * scale;
    int char_h = 16 * scale;

    int text_w = (int)strlen(banner_text) * char_w;
    int padding_x = 16 * scale;
    int padding_y = 6 * scale;
    int pill_w = text_w + padding_x * 2;
    int pill_h = char_h + padding_y * 2;

    /* Max width: 80% of screen */
    if (pill_w > fb_width * 80 / 100) {
        pill_w = fb_width * 80 / 100;
        text_w = pill_w - padding_x * 2;
    }

    /* Center horizontally, position near top (below status bar) */
    int pill_x = (fb_width - pill_w) / 2;
    int pill_y = 40 * scale;

    /* Draw semi-transparent pill background */
    unsigned short bg_color = rgb565(30, 30, 40);
    draw_rect_alpha(pill_x, pill_y, pill_w, pill_h, bg_color, 200);

    /* Draw rounded corners (simple: just darken the corner pixels more) */
    int radius = 4 * scale;
    for (int cy = 0; cy < radius; cy++) {
        for (int cx = 0; cx < radius; cx++) {
            int dx = radius - cx - 1;
            int dy = radius - cy - 1;
            if (dx * dx + dy * dy > radius * radius) {
                /* Outside the corner radius — restore background */
                int corners[4][2] = {
                    {pill_x + cx, pill_y + cy},
                    {pill_x + pill_w - 1 - cx, pill_y + cy},
                    {pill_x + cx, pill_y + pill_h - 1 - cy},
                    {pill_x + pill_w - 1 - cx, pill_y + pill_h - 1 - cy},
                };
                for (int i = 0; i < 4; i++) {
                    int px = corners[i][0], py = corners[i][1];
                    if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                        /* Make corner transparent by restoring what was there
                           (we can't truly restore, so just darken less) */
                        fb_mem[py * fb_stride + px] = rgb565(0, 0, 0);
                    }
                }
            }
        }
    }

    /* Draw text centered in pill */
    int text_x = pill_x + padding_x;
    int text_y = pill_y + padding_y;
    unsigned short text_color = rgb565(240, 240, 255);

    /* Truncate text to fit */
    int max_chars = text_w / char_w;
    char truncated[300];
    if ((int)strlen(banner_text) > max_chars && max_chars > 3) {
        snprintf(truncated, sizeof(truncated), "%.*s...",
                 max_chars - 3, banner_text);
    } else {
        snprintf(truncated, sizeof(truncated), "%.*s",
                 max_chars, banner_text);
    }

    if (scale == 1) {
        draw_string(text_x, text_y, truncated, text_color);
    } else {
        /* 2x scale: draw each pixel as 2x2 block */
        const char *s = truncated;
        int x = text_x;
        while (*s) {
            if (*s < 32 || *s > 126) { s++; x += char_w; continue; }
            int idx = *s - 32;
            const unsigned char *glyph = font8x16_data[idx];
            for (int row = 0; row < 16; row++) {
                unsigned char bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        for (int dy = 0; dy < scale; dy++) {
                            for (int dx = 0; dx < scale; dx++) {
                                int px = x + col * scale + dx;
                                int py = text_y + row * scale + dy;
                                if (px >= 0 && px < fb_width &&
                                    py >= 0 && py < fb_height) {
                                    fb_mem[py * fb_stride + px] = text_color;
                                }
                            }
                        }
                    }
                }
            }
            x += char_w;
            s++;
        }
    }
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
        close(fb_fd); fb_fd = -1;
        return -1;
    }

    fb_width  = (int)vinfo.xres;
    fb_height = (int)vinfo.yres;

    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("menulody: overlay: FBIOGET_FSCREENINFO");
        close(fb_fd); fb_fd = -1;
        return -1;
    }

    fb_stride = (int)(finfo.line_length / 2); /* 16-bit pixels */
    fb_mapped_size = (int)(finfo.line_length * vinfo.yres);

    fb_mem = (unsigned short *)mmap(NULL, fb_mapped_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("menulody: overlay: mmap fb0");
        fb_mem = NULL;
        close(fb_fd); fb_fd = -1;
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
    if (text) {
        snprintf(banner_text, sizeof(banner_text), "%s", text);
    } else {
        banner_text[0] = '\0';
    }
    banner_active = (text && text[0]) ? 1 : 0;
    banner_expire = time(NULL) + duration_secs;
}

void overlay_tick(int menu_active) {
#ifdef __linux__
    if (!fb_mem) return;

    if (banner_active && time(NULL) >= banner_expire) {
        banner_active = 0;
        banner_text[0] = '\0';
        return;
    }

    if (banner_active && menu_active) {
        draw_pill();
    }
#else
    (void)menu_active;
#endif
}

void overlay_cleanup(void) {
#ifdef __linux__
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
    banner_text[0] = '\0';
}
