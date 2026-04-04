#include "overlay.h"
#include "overlay_shm.h"
#include "strutil.h"

#include <stdint.h>
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

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#endif

/* ── Framebuffer state ──────────────────────────────────────────── */

static int fb_fd = -1;
static uint32_t *fb_mem = NULL;
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
static uint32_t *banner_saved_bg = NULL;
static uint32_t *banner_snapshot = NULL;
static uint32_t *banner_work = NULL;
static size_t banner_buffer_pixels = 0;

/* Shared memory for preload hook */
static menulody_overlay_shm_t *overlay_shm = NULL;
static int overlay_shm_fd = -1;

#ifdef __linux__

/* Forward declarations for shared memory functions */
static void overlay_shm_clear(void);

#if defined(PLATFORM_TG5040) || defined(PLATFORM_TG5050) || defined(PLATFORM_MY355)
#define OVERLAY_PLATFORM_IS_DEVICE 1
#else
#define OVERLAY_PLATFORM_IS_DEVICE 0
#endif

#define OVERLAY_THEME_JSON_MAX           4096
#define OVERLAY_DEFAULT_ACCENT_R          155
#define OVERLAY_DEFAULT_ACCENT_G           34
#define OVERLAY_DEFAULT_ACCENT_B           87
#define OVERLAY_DEFAULT_HINT_R            255
#define OVERLAY_DEFAULT_HINT_G            255
#define OVERLAY_DEFAULT_HINT_B            255
#define OVERLAY_PILL_SIZE                  30
#define OVERLAY_BUTTON_MARGIN               5
#define OVERLAY_PILL_PADDING               10

typedef struct {
    int x;
    int y;
    int w;
    int h;
    int text_x;
    int text_y;
    char text[300];
    SDL_Surface *text_surface;
} overlay_layout_t;

static SDL_Color overlay_accent = {
    OVERLAY_DEFAULT_ACCENT_R,
    OVERLAY_DEFAULT_ACCENT_G,
    OVERLAY_DEFAULT_ACCENT_B,
    255
};
static SDL_Color overlay_hint = {
    OVERLAY_DEFAULT_HINT_R,
    OVERLAY_DEFAULT_HINT_G,
    OVERLAY_DEFAULT_HINT_B,
    255
};
static TTF_Font *overlay_font = NULL;
static SDL_Surface *overlay_status_assets = NULL;
static int overlay_device_scale = 2;
static int overlay_device_padding = 10;

static const char *nextui_settings_path(char *buf, size_t buf_size);


static uint32_t bgra8888(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t blend_bgra(uint32_t bg, uint32_t fg, int alpha) {
    int bg_b = (bg      ) & 0xFF;
    int bg_g = (bg >>  8) & 0xFF;
    int bg_r = (bg >> 16) & 0xFF;
    int fg_b = (fg      ) & 0xFF;
    int fg_g = (fg >>  8) & 0xFF;
    int fg_r = (fg >> 16) & 0xFF;
    int r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    int g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
    int b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int ensure_banner_buffers(size_t pixels) {
    if (pixels <= banner_buffer_pixels) return 0;

    uint32_t *new_saved = malloc(pixels * sizeof(uint32_t));
    uint32_t *new_snap = malloc(pixels * sizeof(uint32_t));
    uint32_t *new_work = malloc(pixels * sizeof(uint32_t));
    if (!new_saved || !new_snap || !new_work) {
        free(new_saved);
        free(new_snap);
        free(new_work);
        return -1;
    }

    if (banner_saved_bg)
        memcpy(new_saved, banner_saved_bg, banner_buffer_pixels * sizeof(uint32_t));
    if (banner_snapshot)
        memcpy(new_snap, banner_snapshot, banner_buffer_pixels * sizeof(uint32_t));
    if (banner_work)
        memcpy(new_work, banner_work, banner_buffer_pixels * sizeof(uint32_t));

    free(banner_saved_bg);
    free(banner_snapshot);
    free(banner_work);
    banner_saved_bg = new_saved;
    banner_snapshot = new_snap;
    banner_work = new_work;
    banner_buffer_pixels = pixels;
    return 0;
}

static void copy_fb_rect(int x, int y, int w, int h, uint32_t *dst) {
    for (int row = 0; row < h; row++) {
        memcpy(dst + (size_t)row * w,
               fb_mem + (size_t)(y + row) * fb_stride + x,
               (size_t)w * sizeof(uint32_t));
    }
}

static void write_fb_rect(int x, int y, int w, int h, const uint32_t *src) {
    for (int row = 0; row < h; row++) {
        memcpy(fb_mem + (size_t)(y + row) * fb_stride + x,
               src + (size_t)row * w,
               (size_t)w * sizeof(uint32_t));
    }
}

static int rect_matches_buffer(int x, int y, int w, int h, const uint32_t *buf) {
    for (int row = 0; row < h; row++) {
        const uint32_t *fb_row = fb_mem + (size_t)(y + row) * fb_stride + x;
        const uint32_t *buf_row = buf + (size_t)row * w;
        if (memcmp(fb_row, buf_row, (size_t)w * sizeof(uint32_t)) != 0)
            return 0;
    }
    return 1;
}

static void clear_drawn_overlay_if_still_visible(void) {
    overlay_shm_clear();
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
    overlay_shm_clear();
    banner_drawn = 0;
}

static void capture_background_for_redraw(void) {
    size_t pixels = (size_t)banner_w * banner_h;
    if (pixels == 0 || !banner_saved_bg || !banner_snapshot || !banner_work)
        return;

    copy_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_work);
    if (!banner_drawn) {
        memcpy(banner_saved_bg, banner_work, pixels * sizeof(uint32_t));
        return;
    }

    for (size_t i = 0; i < pixels; i++) {
        banner_saved_bg[i] = (banner_work[i] == banner_snapshot[i])
            ? banner_saved_bg[i]
            : banner_work[i];
    }
}

static SDL_Color color_from_hex(const char *hex, SDL_Color fallback) {
    SDL_Color c = fallback;
    unsigned long value;

    if (!hex || !hex[0]) return c;
    if (hex[0] == '#') hex++;
    else if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;

    value = strtoul(hex, NULL, 16);
    c.r = (Uint8)((value >> 16) & 0xFF);
    c.g = (Uint8)((value >> 8) & 0xFF);
    c.b = (Uint8)(value & 0xFF);
    c.a = 255;
    return c;
}

static const char *json_find_string(const char *json, const char *key) {
    static char value_buf[256];
    char search[128];
    const char *pos;
    int i = 0;

    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return NULL;

    pos += strlen(search);
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;
    if (*pos != '"') return NULL;
    pos++;

    while (*pos && *pos != '"' && i < (int)sizeof(value_buf) - 1)
        value_buf[i++] = *pos++;
    value_buf[i] = '\0';
    return value_buf;
}

static int json_copy_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *value;
    size_t n;

    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    value = json_find_string(json, key);
    if (!value) return 0;

    n = strlen(value);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, value, n);
    out[n] = '\0';
    return 1;
}

static int settings_copy_hex_string(const char *key, char *out, size_t out_size) {
    char settings_path[256];
    const char *path = nextui_settings_path(settings_path, sizeof(settings_path));
    FILE *fp;
    char line[256];
    size_t key_len;

    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!path || !path[0]) return 0;

    fp = fopen(path, "r");
    if (!fp) return 0;

    key_len = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        unsigned int value = 0;
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '='
            && sscanf(line + key_len + 1, "%x", &value) == 1) {
            snprintf(out, out_size, "0x%06X", value & 0xFFFFFF);
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int load_theme_from_json(const char *json) {
    char accent_buf[32] = {0};
    char hint_buf[32] = {0};
    char text_buf[32] = {0};

    if (!json || !json[0]) return -1;

    if (json_copy_string(json, "color2", accent_buf, sizeof(accent_buf)))
        overlay_accent = color_from_hex(accent_buf, overlay_accent);

    if (json_copy_string(json, "color6", hint_buf, sizeof(hint_buf)))
        overlay_hint = color_from_hex(hint_buf, overlay_hint);
    else if (json_copy_string(json, "color4", text_buf, sizeof(text_buf)))
        overlay_hint = color_from_hex(text_buf, overlay_hint);

    return 0;
}

static int load_theme_from_file(const char *path) {
    char json[OVERLAY_THEME_JSON_MAX];
    FILE *fp;
    size_t nread;

    if (!path || !path[0]) return -1;

    fp = fopen(path, "r");
    if (!fp) return -1;

    nread = fread(json, 1, sizeof(json) - 1, fp);
    fclose(fp);
    if (nread == 0) return -1;

    json[nread] = '\0';
    return load_theme_from_json(json);
}

static int load_theme_from_device_nextval(void) {
#if OVERLAY_PLATFORM_IS_DEVICE
    const char *nextval_path = NULL;
    const char *system_path_env = getenv("SYSTEM_PATH");
    char nextval_buf[256] = {0};
    char json[OVERLAY_THEME_JSON_MAX];
    FILE *fp;
    size_t total = 0;

    if (system_path_env && system_path_env[0]) {
        snprintf(nextval_buf, sizeof(nextval_buf), "%s/bin/nextval.elf", system_path_env);
        if (access(nextval_buf, X_OK) == 0) nextval_path = nextval_buf;
    }

    if (!nextval_path) {
#if defined(PLATFORM_TG5040)
        if (access("/mnt/SDCARD/.system/tg5040/bin/nextval.elf", X_OK) == 0)
            nextval_path = "/mnt/SDCARD/.system/tg5040/bin/nextval.elf";
#elif defined(PLATFORM_TG5050)
        if (access("/mnt/SDCARD/.system/tg5050/bin/nextval.elf", X_OK) == 0)
            nextval_path = "/mnt/SDCARD/.system/tg5050/bin/nextval.elf";
#elif defined(PLATFORM_MY355)
        if (access("/mnt/SDCARD/.system/my355/bin/nextval.elf", X_OK) == 0)
            nextval_path = "/mnt/SDCARD/.system/my355/bin/nextval.elf";
#endif
    }

    if (!nextval_path) return -1;

    fp = popen(nextval_path, "r");
    if (!fp) return -1;

    while (total < sizeof(json) - 1) {
        size_t n = fread(json + total, 1, sizeof(json) - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    json[total] = '\0';
    pclose(fp);

    if (total == 0) return -1;
    return load_theme_from_json(json);
#else
    return -1;
#endif
}

static void load_overlay_theme(void) {
    const char *path = getenv("AP_NEXTVAL_PATH");
    char accent_buf[32] = {0};
    char hint_buf[32] = {0};

    overlay_accent.r = OVERLAY_DEFAULT_ACCENT_R;
    overlay_accent.g = OVERLAY_DEFAULT_ACCENT_G;
    overlay_accent.b = OVERLAY_DEFAULT_ACCENT_B;
    overlay_accent.a = 255;
    overlay_hint.r = OVERLAY_DEFAULT_HINT_R;
    overlay_hint.g = OVERLAY_DEFAULT_HINT_G;
    overlay_hint.b = OVERLAY_DEFAULT_HINT_B;
    overlay_hint.a = 255;

    if (settings_copy_hex_string("color2", accent_buf, sizeof(accent_buf)))
        overlay_accent = color_from_hex(accent_buf, overlay_accent);
    if (settings_copy_hex_string("color6", hint_buf, sizeof(hint_buf)))
        overlay_hint = color_from_hex(hint_buf, overlay_hint);

    if (path && path[0] && load_theme_from_file(path) == 0)
        return;

    (void)load_theme_from_device_nextval();
}

static void resolve_device_metrics(void) {
    if (fb_width == 1024 && fb_height == 768) {
        /* TrimUI Brick (tg3040/tg5040 variant) */
        overlay_device_scale = 3;
        overlay_device_padding = 5;
    } else {
        /* Smart Pro 1280x720, Smart Pro S 1280x720, Miyoo Flip 640x480 */
        overlay_device_scale = 2;
        overlay_device_padding = 10;
    }
}

static const char *nextui_settings_path(char *buf, size_t buf_size) {
#if OVERLAY_PLATFORM_IS_DEVICE
    const char *shared = getenv("SHARED_USERDATA_PATH");
    if (!shared || !shared[0]) shared = "/mnt/SDCARD/.userdata/shared";
    snprintf(buf, buf_size, "%s/minuisettings.txt", shared);
    return buf;
#else
    const char *path = getenv("AP_MINUI_SETTINGS_PATH");
    (void)buf;
    (void)buf_size;
    return (path && path[0]) ? path : NULL;
#endif
}

static int read_nextui_setting_int(const char *key, int default_val) {
    char settings_path[256];
    const char *path = nextui_settings_path(settings_path, sizeof(settings_path));
    FILE *fp;
    char line[256];
    size_t key_len;

    if (!path || !path[0]) return default_val;

    fp = fopen(path, "r");
    if (!fp) return default_val;

    key_len = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            int value = 0;
            if (sscanf(line + key_len + 1, "%d", &value) == 1) {
                fclose(fp);
                return value;
            }
        }
    }

    fclose(fp);
    return default_val;
}

static int overlay_font_size(void) {
    int size = 12 * overlay_device_scale;
    return size < 8 ? 8 : size;
}

static const char *resolve_font_path(char *buf, size_t buf_size) {
    static const char * const search_paths[] = {
        "./font.ttf",
        "./res/font.ttf",
        "../res/font.ttf",
        "/mnt/SDCARD/.system/res/font1.ttf",
        "/mnt/SDCARD/.system/res/font2.ttf",
        "/mnt/SDCARD/.system/res/font.ttf",
        "/mnt/SDCARD/.system/tg5040/res/font.ttf",
        "/mnt/SDCARD/.system/my355/res/font.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL,
    };
    int font_id;
    const char *sdcard;
    const char *font_name;

    font_id = read_nextui_setting_int("font", 1);
    font_name = (font_id == 1) ? "font1.ttf" : "font2.ttf";

    sdcard = getenv("SDCARD_PATH");
    if (!sdcard || !sdcard[0]) sdcard = "/mnt/SDCARD";
    snprintf(buf, buf_size, "%s/.system/res/%s", sdcard, font_name);
    if (access(buf, R_OK) == 0) return buf;

    for (int i = 0; search_paths[i]; i++) {
        if (access(search_paths[i], R_OK) == 0)
            return search_paths[i];
    }

    return NULL;
}

static const char *status_assets_dir(char *buf, size_t buf_size) {
    const char *dir = getenv("AP_STATUS_ASSETS_DIR");
    if (dir && dir[0]) return dir;

#if OVERLAY_PLATFORM_IS_DEVICE
    {
        const char *sdcard = getenv("SDCARD_PATH");
        if (!sdcard || !sdcard[0]) sdcard = "/mnt/SDCARD";
        snprintf(buf, buf_size, "%s/.system/res", sdcard);
        return buf;
    }
#else
    (void)buf;
    (void)buf_size;
    return NULL;
#endif
}

static void free_overlay_resources(void) {
    if (overlay_font) {
        TTF_CloseFont(overlay_font);
        overlay_font = NULL;
    }
    if (overlay_status_assets) {
        SDL_FreeSurface(overlay_status_assets);
        overlay_status_assets = NULL;
    }
}

static void init_overlay_resources(void) {
    char font_path[512];
    const char *resolved_font;
    char assets_dir_buf[256];
    const char *assets_dir;
    char asset_path[512];

    free_overlay_resources();
    resolve_device_metrics();
    load_overlay_theme();

    if (SDL_WasInit(0) == 0 && SDL_Init(0) < 0) {
        fprintf(stderr, "menulody: overlay: SDL init failed: %s\n", SDL_GetError());
        return;
    }

    if (!TTF_WasInit() && TTF_Init() < 0) {
        fprintf(stderr, "menulody: overlay: TTF init failed: %s\n", TTF_GetError());
        return;
    }

    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
        fprintf(stderr, "menulody: overlay: IMG init failed: %s\n", IMG_GetError());
    }

    resolved_font = resolve_font_path(font_path, sizeof(font_path));
    if (!resolved_font) {
        fprintf(stderr, "menulody: overlay: no font file found\n");
    } else {
        overlay_font = TTF_OpenFont(resolved_font, overlay_font_size());
        if (!overlay_font) {
            fprintf(stderr, "menulody: overlay: font load failed: %s\n", TTF_GetError());
        } else {
            TTF_SetFontStyle(overlay_font, TTF_STYLE_BOLD);
        }
    }

    assets_dir = status_assets_dir(assets_dir_buf, sizeof(assets_dir_buf));
    if (!assets_dir || !assets_dir[0]) return;

    snprintf(asset_path, sizeof(asset_path), "%s/assets@%dx.png",
             assets_dir, overlay_device_scale);
    overlay_status_assets = IMG_Load(asset_path);
    if (!overlay_status_assets) return;

    {
        SDL_Surface *converted = SDL_ConvertSurfaceFormat(
            overlay_status_assets, SDL_PIXELFORMAT_RGBA32, 0);
        if (converted) {
            SDL_FreeSurface(overlay_status_assets);
            overlay_status_assets = converted;
        }
    }
    SDL_SetSurfaceBlendMode(overlay_status_assets, SDL_BLENDMODE_BLEND);
}

static size_t utf8_trim_boundary(const char *text, size_t len) {
    while (len > 0 && (((unsigned char)text[len] & 0xC0) == 0x80))
        len--;
    return len;
}

static void fit_text_to_width(const char *src, char *dst, size_t dst_size, int max_w) {
    static const char ellipsis[] = "...";
    int full_w = 0;
    int ellipsis_w = 0;
    char candidate[300];
    size_t len;

    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src || !src[0] || !overlay_font || max_w <= 0) return;

    if (TTF_SizeUTF8(overlay_font, src, &full_w, NULL) == 0 && full_w <= max_w) {
        str_copy_trunc(dst, dst_size, src);
        return;
    }

    if (TTF_SizeUTF8(overlay_font, ellipsis, &ellipsis_w, NULL) < 0 || ellipsis_w > max_w)
        return;

    len = strlen(src);
    while (len > 0) {
        int candidate_w = 0;

        len = utf8_trim_boundary(src, len);
        memcpy(candidate, src, len);
        candidate[len] = '\0';
        strncat(candidate, ellipsis, sizeof(candidate) - strlen(candidate) - 1);

        if (TTF_SizeUTF8(overlay_font, candidate, &candidate_w, NULL) == 0
            && candidate_w <= max_w) {
            str_copy_trunc(dst, dst_size, candidate);
            return;
        }

        if (len == 0) break;
        len--;
    }

    str_copy_trunc(dst, dst_size, ellipsis);
}

static int build_layout(overlay_layout_t *layout) {
    int pill_h;
    int inner_margin;
    int side_margin;
    int max_pill_w;
    int max_text_w;
    int font_h;

    if (!layout || !fb_mem || !banner_text[0] || !overlay_font) return -1;

    memset(layout, 0, sizeof(*layout));
    pill_h = OVERLAY_PILL_SIZE * overlay_device_scale;
    inner_margin = OVERLAY_PILL_PADDING * overlay_device_scale;
    side_margin = overlay_device_padding * overlay_device_scale;
    max_pill_w = fb_width - 2 * side_margin - 2 * pill_h;
    if (max_pill_w < pill_h)
        max_pill_w = fb_width - 2 * side_margin;
    if (max_pill_w <= 0) return -1;

    max_text_w = max_pill_w - inner_margin * 2;
    if (max_text_w <= 0) return -1;

    fit_text_to_width(banner_text, layout->text, sizeof(layout->text), max_text_w);
    if (!layout->text[0]) return -1;

    layout->text_surface = TTF_RenderUTF8_Blended(overlay_font, layout->text, overlay_hint);
    if (!layout->text_surface) return -1;

    font_h = TTF_FontHeight(overlay_font);
    SDL_SetSurfaceBlendMode(layout->text_surface, SDL_BLENDMODE_BLEND);
    layout->w = layout->text_surface->w + inner_margin * 2;
    layout->h = pill_h;
    layout->x = (fb_width - layout->w) / 2;
    layout->y = fb_height - ((overlay_device_padding + OVERLAY_PILL_SIZE) * overlay_device_scale);
    layout->text_x = (layout->w - layout->text_surface->w) / 2;
    layout->text_y = (layout->h - font_h) / 2;

    if (layout->x < 0) layout->x = 0;
    if (layout->y < 0) layout->y = 0;
    if (layout->x + layout->w > fb_width) layout->w = fb_width - layout->x;
    if (layout->y + layout->h > fb_height) layout->h = fb_height - layout->y;

    return (layout->w > 0 && layout->h > 0) ? 0 : -1;
}

static void destroy_layout(overlay_layout_t *layout) {
    if (!layout) return;
    if (layout->text_surface) {
        SDL_FreeSurface(layout->text_surface);
        layout->text_surface = NULL;
    }
}

static void put_surface_pixel(SDL_Surface *surface, int x, int y,
                              Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    Uint32 *row;

    if (!surface || x < 0 || y < 0 || x >= surface->w || y >= surface->h)
        return;

    row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
    row[x] = SDL_MapRGBA(surface->format, r, g, b, a);
}

static void draw_rounded_rect_surface(SDL_Surface *surface, int x0, int y0, int w, int h,
                                      int radius, SDL_Color color) {
    if (!surface || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int draw = 1;

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

            if (draw)
                put_surface_pixel(surface, x0 + px, y0 + py,
                                  color.r, color.g, color.b, color.a);
        }
    }
}

static void blit_tinted_surface_region(SDL_Surface *src, const SDL_Rect *src_rect,
                                       SDL_Surface *dst, int dx, int dy, SDL_Color tint) {
    int src_locked = 0;
    int dst_locked = 0;

    if (!src || !src_rect || !dst) return;

    if (SDL_MUSTLOCK(src) && SDL_LockSurface(src) == 0) src_locked = 1;
    if (SDL_MUSTLOCK(dst) && SDL_LockSurface(dst) == 0) dst_locked = 1;

    for (int y = 0; y < src_rect->h; y++) {
        for (int x = 0; x < src_rect->w; x++) {
            Uint32 *row = (Uint32 *)((Uint8 *)src->pixels + (src_rect->y + y) * src->pitch);
            Uint32 pixel = row[src_rect->x + x];
            Uint8 sr, sg, sb, sa;

            SDL_GetRGBA(pixel, src->format, &sr, &sg, &sb, &sa);
            if (sa == 0) continue;

            put_surface_pixel(dst, dx + x, dy + y,
                              (Uint8)((sr * tint.r) / 255),
                              (Uint8)((sg * tint.g) / 255),
                              (Uint8)((sb * tint.b) / 255),
                              (Uint8)((sa * tint.a) / 255));
        }
    }

    if (src_locked) SDL_UnlockSurface(src);
    if (dst_locked) SDL_UnlockSurface(dst);
}

static SDL_Surface *render_overlay_surface(const overlay_layout_t *layout) {
    SDL_Surface *surface;
    int cap_w;
    int center_w;
    SDL_Rect text_dst;

    if (!layout || layout->w <= 0 || layout->h <= 0) return NULL;

    surface = SDL_CreateRGBSurfaceWithFormat(0, layout->w, layout->h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;

    SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 0, 0, 0, 0));
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);

    cap_w = layout->h / 2;
    center_w = layout->w - 2 * cap_w;

    if (overlay_status_assets && overlay_device_scale > 0) {
        SDL_Rect left_src = {
            1 * overlay_device_scale,
            1 * overlay_device_scale,
            15 * overlay_device_scale,
            30 * overlay_device_scale
        };
        SDL_Rect right_src = {
            16 * overlay_device_scale,
            1 * overlay_device_scale,
            15 * overlay_device_scale,
            30 * overlay_device_scale
        };

        if (center_w > 0) {
            SDL_Rect center = {cap_w, 0, center_w, layout->h};
            SDL_FillRect(surface, &center,
                         SDL_MapRGBA(surface->format,
                                     overlay_accent.r, overlay_accent.g,
                                     overlay_accent.b, overlay_accent.a));
        }

        blit_tinted_surface_region(overlay_status_assets, &left_src, surface, 0, 0, overlay_accent);
        blit_tinted_surface_region(overlay_status_assets, &right_src, surface,
                                   layout->w - cap_w, 0, overlay_accent);
    } else {
        draw_rounded_rect_surface(surface, 0, 0, layout->w, layout->h, layout->h / 2, overlay_accent);
    }

    if (layout->text_surface) {
        text_dst.x = layout->text_x;
        text_dst.y = layout->text_y;
        text_dst.w = layout->text_surface->w;
        text_dst.h = layout->text_surface->h;
        SDL_BlitSurface(layout->text_surface, NULL, surface, &text_dst);
    }

    return surface;
}

static void blend_surface_over_bgra(SDL_Surface *surface, uint32_t *dst) {
    int locked = 0;

    if (!surface || !dst) return;
    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) == 0) locked = 1;

    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
            Uint32 pixel = row[x];
            Uint8 r, g, b, a;
            size_t idx = (size_t)y * surface->w + x;

            SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);
            if (a == 0) continue;

            if (a == 255) {
                dst[idx] = bgra8888(r, g, b);
            } else {
                dst[idx] = blend_bgra(dst[idx], bgra8888(r, g, b), a);
            }
        }
    }

    if (locked) SDL_UnlockSurface(surface);
}

/* ── Shared memory for preload hook ─────────────────────────────── */

static void overlay_shm_init(void) {
    overlay_shm_fd = open(MENULODY_SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (overlay_shm_fd < 0) {
        perror("menulody: overlay: shm open");
        return;
    }

    if (ftruncate(overlay_shm_fd, (off_t)sizeof(menulody_overlay_shm_t)) < 0) {
        perror("menulody: overlay: shm ftruncate");
        close(overlay_shm_fd);
        overlay_shm_fd = -1;
        return;
    }

    overlay_shm = (menulody_overlay_shm_t *)mmap(
        NULL, sizeof(menulody_overlay_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, overlay_shm_fd, 0);
    if (overlay_shm == MAP_FAILED) {
        perror("menulody: overlay: shm mmap");
        overlay_shm = NULL;
        close(overlay_shm_fd);
        overlay_shm_fd = -1;
        return;
    }

    overlay_shm->magic   = MENULODY_SHM_MAGIC;
    overlay_shm->active  = 0;
    overlay_shm->version = 0;
    __sync_synchronize();

    fprintf(stderr, "menulody: overlay: shm ready at %s\n", MENULODY_SHM_PATH);
}

static void overlay_shm_update(SDL_Surface *surface, int x, int y, int w, int h) {
    int locked = 0;

    if (!overlay_shm || !surface) return;
    if (w <= 0 || h <= 0) return;
    if (w > MENULODY_SHM_MAX_W || h > MENULODY_SHM_MAX_H) return;

    /* Bump version to signal "write in progress" */
    overlay_shm->version++;
    __sync_synchronize();

    overlay_shm->x         = x;
    overlay_shm->y         = y;
    overlay_shm->w         = w;
    overlay_shm->h         = h;
    overlay_shm->fb_width  = fb_width;
    overlay_shm->fb_height = fb_height;

    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) == 0) locked = 1;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            Uint32 *src_row = (Uint32 *)((Uint8 *)surface->pixels + row * surface->pitch);
            Uint32 pixel = src_row[col];
            Uint8 r, g, b, a;

            SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);
            /* Store as BGRA8888: 0xAARRGGBB in uint32_t on little-endian
               = bytes B, G, R, A in memory */
            overlay_shm->pixels[row * w + col] =
                ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                ((uint32_t)g << 8)  |  (uint32_t)b;
        }
    }

    if (locked) SDL_UnlockSurface(surface);

    overlay_shm->active = 1;
    __sync_synchronize();
    /* Final version bump — reader checks v1==v2 for consistency */
    overlay_shm->version++;
    __sync_synchronize();
}

static void overlay_shm_clear(void) {
    if (!overlay_shm) return;
    overlay_shm->version++;
    __sync_synchronize();
    overlay_shm->active = 0;
    __sync_synchronize();
    overlay_shm->version++;
    __sync_synchronize();
}

static void overlay_shm_cleanup(void) {
    if (overlay_shm) {
        overlay_shm->active = 0;
        __sync_synchronize();
        munmap(overlay_shm, sizeof(menulody_overlay_shm_t));
        overlay_shm = NULL;
    }
    if (overlay_shm_fd >= 0) {
        close(overlay_shm_fd);
        overlay_shm_fd = -1;
    }
    unlink(MENULODY_SHM_PATH);
}

static void draw_current_banner(void) {
    overlay_layout_t layout;
    SDL_Surface *surface = NULL;
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
    if (ensure_banner_buffers(pixels) < 0) {
        destroy_layout(&layout);
        return;
    }

    capture_background_for_redraw();
    memcpy(banner_work, banner_saved_bg, pixels * sizeof(uint32_t));

    surface = render_overlay_surface(&layout);
    if (!surface) {
        destroy_layout(&layout);
        return;
    }

    /* Publish to shared memory for the preload hook */
    overlay_shm_update(surface, banner_x, banner_y, banner_w, banner_h);

    /* Direct fb write (fallback when preload is not active) */
    blend_surface_over_bgra(surface, banner_work);
    write_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_work);
    copy_fb_rect(banner_x, banner_y, banner_w, banner_h, banner_snapshot);
    banner_drawn = 1;

    SDL_FreeSurface(surface);
    destroy_layout(&layout);
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

    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "menulody: overlay: unsupported bpp %u (need 32)\n",
                vinfo.bits_per_pixel);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("menulody: overlay: FBIOGET_FSCREENINFO");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fb_stride = (int)(finfo.line_length / 4);
    fb_mapped_size = (int)(finfo.line_length * vinfo.yres);

    fb_mem = (uint32_t *)mmap(NULL, fb_mapped_size,
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("menulody: overlay: mmap fb0");
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    init_overlay_resources();
    overlay_shm_init();

    fprintf(stderr, "menulody: overlay: fb0 %dx%d stride=%d bpp=%u\n",
            fb_width, fb_height, fb_stride, vinfo.bits_per_pixel);
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

    draw_current_banner();
#else
    (void)menu_active;
#endif
}

int overlay_is_active(void) {
    return banner_active;
}

void overlay_cleanup(void) {
#ifdef __linux__
    if (fb_mem && banner_drawn)
        restore_overlay_region_best_effort();
    overlay_shm_cleanup();
    free(banner_saved_bg);
    free(banner_snapshot);
    free(banner_work);
    banner_saved_bg = NULL;
    banner_snapshot = NULL;
    banner_work = NULL;
    banner_buffer_pixels = 0;
    free_overlay_resources();
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
    banner_expire = 0;
}
