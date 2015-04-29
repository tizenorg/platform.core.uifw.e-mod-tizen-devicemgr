#ifndef TIZEN_SCREENSHOOTER_SERVER_PROTOCOL_H
#define TIZEN_SCREENSHOOTER_SERVER_PROTOCOL_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

struct wl_client;
struct wl_resource;

struct tizen_screenshooter;
struct tizen_screenmirror;

extern const struct wl_interface tizen_screenshooter_interface;
extern const struct wl_interface tizen_screenmirror_interface;

/**
 * tizen_screenshooter - interface for tizen-screenshooter
 * @get_screenmirror: create a screenmirror object
 *
 * Clients can get a screenmirror object from this interface.
 */
struct tizen_screenshooter_interface {
	/**
	 * get_screenmirror - create a screenmirror object
	 * @id: new screenmirror object
	 * @output: output object for screenmirror
	 *
	 * Before using screenmirror, a client should get a screenmirror
	 * object from display server.
	 */
	void (*get_screenmirror)(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t id,
				 struct wl_resource *output);
};


#ifndef TIZEN_SCREENMIRROR_CONTENT_ENUM
#define TIZEN_SCREENMIRROR_CONTENT_ENUM
enum tizen_screenmirror_content {
	TIZEN_SCREENMIRROR_CONTENT_NORMAL = 0,
	TIZEN_SCREENMIRROR_CONTENT_VIDEO = 1,
};
#endif /* TIZEN_SCREENMIRROR_CONTENT_ENUM */

#ifndef TIZEN_SCREENMIRROR_STRETCH_ENUM
#define TIZEN_SCREENMIRROR_STRETCH_ENUM
enum tizen_screenmirror_stretch {
	TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO = 0,
	TIZEN_SCREENMIRROR_STRETCH_FULLY = 1,
};
#endif /* TIZEN_SCREENMIRROR_STRETCH_ENUM */

/**
 * tizen_screenmirror - interface for screenmirror
 * @destroy: (none)
 * @set_stretch: (none)
 * @queue: queue a buffer
 * @dequeue: dequeue a buffer
 * @start: (none)
 * @stop: (none)
 *
 * A client can use this interface to get stream images of screen. Before
 * starting, queue all buffers. Then, start a screenmirror. After starting,
 * a dequeued event will occur when drawing a captured image on a buffer is
 * finished. You might need to queue the dequeued buffer again to get a new
 * image from display server.
 */
struct tizen_screenmirror_interface {
	/**
	 * destroy - (none)
	 */
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	/**
	 * set_stretch - (none)
	 * @stretch: stretch type for screenmirror
	 */
	void (*set_stretch)(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t stretch);
	/**
	 * queue - queue a buffer
	 * @buffer: buffer object for screenmirror
	 *
	 * 
	 */
	void (*queue)(struct wl_client *client,
		      struct wl_resource *resource,
		      struct wl_resource *buffer);
	/**
	 * dequeue - dequeue a buffer
	 * @buffer: buffer object for screenmirror
	 *
	 * A user can dequeue a buffer from display server when he wants
	 * to take back it from server.
	 */
	void (*dequeue)(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *buffer);
	/**
	 * start - (none)
	 */
	void (*start)(struct wl_client *client,
		      struct wl_resource *resource);
	/**
	 * stop - (none)
	 */
	void (*stop)(struct wl_client *client,
		     struct wl_resource *resource);
};

#define TIZEN_SCREENMIRROR_DEQUEUED	0
#define TIZEN_SCREENMIRROR_CONTENT	1
#define TIZEN_SCREENMIRROR_STOP	2

#define TIZEN_SCREENMIRROR_DEQUEUED_SINCE_VERSION	1
#define TIZEN_SCREENMIRROR_CONTENT_SINCE_VERSION	1
#define TIZEN_SCREENMIRROR_STOP_SINCE_VERSION	1

static inline void
tizen_screenmirror_send_dequeued(struct wl_resource *resource_, struct wl_resource *buffer)
{
	wl_resource_post_event(resource_, TIZEN_SCREENMIRROR_DEQUEUED, buffer);
}

static inline void
tizen_screenmirror_send_content(struct wl_resource *resource_, uint32_t content)
{
	wl_resource_post_event(resource_, TIZEN_SCREENMIRROR_CONTENT, content);
}

static inline void
tizen_screenmirror_send_stop(struct wl_resource *resource_)
{
	wl_resource_post_event(resource_, TIZEN_SCREENMIRROR_STOP);
}

#ifdef  __cplusplus
}
#endif

#endif
