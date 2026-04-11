#ifndef MENULODY_VARNISH_CLIENT_H
#define MENULODY_VARNISH_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool pak_installed;
    bool enabled;
    bool startup_installed;
    bool boot_installed;
    bool daemon_running;
} varnish_client_status_t;

bool varnish_client_is_running(void);
bool varnish_client_is_installed(void);
void varnish_client_get_status(varnish_client_status_t *out);
bool varnish_client_is_enabled(const varnish_client_status_t *status);
void varnish_client_format_status(const varnish_client_status_t *status,
                                  char *out, size_t size);
int  varnish_client_show_pill(const char *text, int duration_secs);
int  varnish_client_hide(void);

#endif /* MENULODY_VARNISH_CLIENT_H */
