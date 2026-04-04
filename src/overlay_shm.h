#ifndef MENULODY_OVERLAY_SHM_H
#define MENULODY_OVERLAY_SHM_H

#include <stdint.h>

#define MENULODY_SHM_PATH    "/tmp/menulody_overlay.dat"
#define MENULODY_SHM_MAGIC   0x4D4C4F56u  /* "MLOV" */
#define MENULODY_SHM_MAX_W   512
#define MENULODY_SHM_MAX_H   128

typedef struct {
    uint32_t     magic;           /* MENULODY_SHM_MAGIC when valid */
    volatile int version;         /* Incremented after each update */
    int          active;          /* 1 = draw overlay, 0 = skip */
    int          x, y, w, h;     /* Position and size on screen */
    int          fb_width;        /* Expected screen width (sanity) */
    int          fb_height;       /* Expected screen height (sanity) */
    uint32_t     pixels[MENULODY_SHM_MAX_W * MENULODY_SHM_MAX_H]; /* BGRA8888 */
} menulody_overlay_shm_t;

#endif /* MENULODY_OVERLAY_SHM_H */
