#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface tizen_screenmirror_interface;
extern const struct wl_interface wl_buffer_interface;
extern const struct wl_interface wl_output_interface;

static const struct wl_interface *types[] = {
	NULL,
	&tizen_screenmirror_interface,
	&wl_output_interface,
	&wl_buffer_interface,
	&wl_buffer_interface,
	&wl_buffer_interface,
};

static const struct wl_message tizen_screenshooter_requests[] = {
	{ "get_screenmirror", "no", types + 1 },
};

WL_EXPORT const struct wl_interface tizen_screenshooter_interface = {
	"tizen_screenshooter", 1,
	1, tizen_screenshooter_requests,
	0, NULL,
};

static const struct wl_message tizen_screenmirror_requests[] = {
	{ "destroy", "", types + 0 },
	{ "set_stretch", "u", types + 0 },
	{ "queue", "o", types + 3 },
	{ "dequeue", "o", types + 4 },
	{ "start", "", types + 0 },
	{ "stop", "", types + 0 },
};

static const struct wl_message tizen_screenmirror_events[] = {
	{ "dequeued", "o", types + 5 },
	{ "content", "u", types + 0 },
	{ "stop", "", types + 0 },
};

WL_EXPORT const struct wl_interface tizen_screenmirror_interface = {
	"tizen_screenmirror", 1,
	6, tizen_screenmirror_requests,
	3, tizen_screenmirror_events,
};

