#ifndef TIZEN_WINDOW_SCREEN_SERVER_PROTOCOL_H
#define TIZEN_WINDOW_SCREEN_SERVER_PROTOCOL_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

struct wl_client;
struct wl_resource;

struct tizen_window_screen;

extern const struct wl_interface tizen_window_screen_interface;

#ifndef TIZEN_WINDOW_SCREEN_MODE_ENUM
#define TIZEN_WINDOW_SCREEN_MODE_ENUM
enum tizen_window_screen_mode {
	TIZEN_WINDOW_SCREEN_MODE_DEFAULT = 0,
	TIZEN_WINDOW_SCREEN_MODE_ALWAYS_ON = 1,
};
#endif /* TIZEN_WINDOW_SCREEN_MODE_ENUM */

#ifndef TIZEN_WINDOW_SCREEN_ERROR_STATE_ENUM
#define TIZEN_WINDOW_SCREEN_ERROR_STATE_ENUM
enum tizen_window_screen_error_state {
	TIZEN_WINDOW_SCREEN_ERROR_STATE_NONE = 0,
	TIZEN_WINDOW_SCREEN_ERROR_STATE_PERMISSION_DENIED = 1,
};
#endif /* TIZEN_WINDOW_SCREEN_ERROR_STATE_ENUM */

struct tizen_window_screen_interface {
	/**
	 * set_mode - (none)
	 * @surface: (none)
	 * @mode: (none)
	 */
	void (*set_mode)(struct wl_client *client,
			 struct wl_resource *resource,
			 struct wl_resource *surface,
			 uint32_t mode);
};

#define TIZEN_WINDOW_SCREEN_DONE	0

#define TIZEN_WINDOW_SCREEN_DONE_SINCE_VERSION	1

static inline void
tizen_window_screen_send_done(struct wl_resource *resource_, struct wl_resource *surface, uint32_t mode, uint32_t error_state)
{
	wl_resource_post_event(resource_, TIZEN_WINDOW_SCREEN_DONE, surface, mode, error_state);
}

#ifdef  __cplusplus
}
#endif

#endif
