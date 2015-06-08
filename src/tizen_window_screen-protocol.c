#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface wl_surface_interface;

static const struct wl_interface *types[] = {
	&wl_surface_interface,
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
};

static const struct wl_message tizen_window_screen_requests[] = {
	{ "set_mode", "ou", types + 0 },
};

static const struct wl_message tizen_window_screen_events[] = {
	{ "done", "ouu", types + 2 },
};

WL_EXPORT const struct wl_interface tizen_window_screen_interface = {
	"tizen_window_screen", 1,
	1, tizen_window_screen_requests,
	1, tizen_window_screen_events,
};

