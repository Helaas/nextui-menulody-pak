#ifndef MENULODY_VARNISH_CLIENT_H
#define MENULODY_VARNISH_CLIENT_H

#include <stdbool.h>

bool varnish_client_is_running(void);
bool varnish_client_is_installed(void);
int  varnish_client_show_pill(const char *text, int duration_secs);
int  varnish_client_hide(void);

#endif /* MENULODY_VARNISH_CLIENT_H */
