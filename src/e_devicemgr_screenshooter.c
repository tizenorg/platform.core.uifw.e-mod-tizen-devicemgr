#define E_COMP_WL
#include "e.h"
#include <wayland-server.h>
#include <Ecore_Wayland.h>
#include "e_tizen_screenshooter_server_protocol.h"
#include "e_devicemgr_screenshooter.h"

#define SW_DUMP_FPS     30
#define SW_DUMP_TIMEOUT ((double)1/SW_DUMP_FPS)

typedef struct _Mirror
{
   struct wl_resource *resource;
   struct wl_resource *shooter;
   struct wl_resource *output;

   Eina_Bool started;
   enum tizen_screenmirror_stretch stretch;
   Ecore_Timer *sw_timer;

   Eina_List *buffer_queue;
} Mirror;

typedef struct _Mirror_Buffer
{
   struct wl_resource *resource;

   Mirror *mirror;

   struct wl_shm_buffer *shm_buffer;
   int32_t stride;
   int32_t w, h;
   uint32_t format;
   void *data;
   Eina_Bool in_use;

   struct wl_signal destroy_signal;
   struct wl_listener destroy_listener;
} Mirror_Buffer;

static void _e_tizen_screenmirror_buffer_dequeue(Mirror_Buffer *buffer);

static void
_e_tizen_screenmirror_center_rect (int src_w, int src_h, int dst_w, int dst_h, Eina_Rectangle *fit)
{
   float rw, rh;

   if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !fit)
     return;

   rw = (float)src_w / dst_w;
   rh = (float)src_h / dst_h;

   if (rw > rh)
     {
        fit->w = dst_w;
        fit->h = src_h * rw;
        fit->x = 0;
        fit->y = (dst_h - fit->h) / 2;
     }
   else if (rw < rh)
     {
        fit->w = src_w * rw;
        fit->h = dst_h;
        fit->x = (dst_w - fit->w) / 2;
        fit->y = 0;
     }
   else
     {
        fit->w = dst_w;
        fit->h = dst_h;
        fit->x = 0;
        fit->y = 0;
     }
}

static void
_e_tizen_screenmirror_sw_dump(Mirror_Buffer *buffer)
{
   Mirror *mirror = buffer->mirror;
   E_Comp_Wl_Output *output = wl_resource_get_user_data(mirror->output);
   Eina_Rectangle dst = {0,};

   if (mirror->stretch == TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO)
     _e_tizen_screenmirror_center_rect(output->w, output->h, buffer->w, buffer->h, &dst);
   else
     {
        dst.x = dst.y = 0;
        dst.w = buffer->w;
        dst.h = buffer->h;
     }

   wl_shm_buffer_begin_access(buffer->shm_buffer);
   evas_render_copy(e_comp->evas, buffer->data, buffer->stride,
                    buffer->w, buffer->h, buffer->format,
                    output->x, output->y, output->w, output->h,
                    dst.x, dst.y, dst.w, dst.h);
   wl_shm_buffer_end_access(buffer->shm_buffer);
}

static Eina_Bool
_e_tizen_screenmirror_sw_do_dump(void *data)
{
   Mirror *mirror = data;
   Mirror_Buffer *buffer;

   buffer = eina_list_nth(mirror->buffer_queue, 0);
   if (buffer)
     {
        _e_tizen_screenmirror_sw_dump(buffer);
        _e_tizen_screenmirror_buffer_dequeue(buffer);
     }

   if (!eina_list_nth_list(mirror->buffer_queue, 0))
     {
        mirror->sw_timer = NULL;
        return ECORE_CALLBACK_CANCEL;
     }
   else
     return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
_e_tizen_screenmirror_buffer_queue(Mirror_Buffer *buffer)
{
   Mirror *mirror = buffer->mirror;

   if (mirror->started && !mirror->sw_timer)
     mirror->sw_timer = ecore_timer_add(SW_DUMP_TIMEOUT,
                                        _e_tizen_screenmirror_sw_do_dump, mirror);

   mirror->buffer_queue = eina_list_append(mirror->buffer_queue, buffer);

   return EINA_TRUE;
}

static void
_e_tizen_screenmirror_buffer_dequeue(Mirror_Buffer *buffer)
{
   Mirror *mirror = buffer->mirror;

   if (!eina_list_data_find_list(mirror->buffer_queue, buffer))
     return;

   mirror->buffer_queue = eina_list_remove(mirror->buffer_queue, buffer);
   tizen_screenmirror_send_dequeued(mirror->resource, buffer->resource);

   if (!eina_list_nth_list(mirror->buffer_queue, 0))
     if (mirror->sw_timer)
       {
          ecore_timer_del(mirror->sw_timer);
          mirror->sw_timer = NULL;
       }
}

static void
_e_tizen_screenmirror_buffer_cb_destroy(struct wl_listener *listener, void *data)
{
   Mirror_Buffer *buffer = container_of(listener, Mirror_Buffer, destroy_listener);

   /* first, inform to other */
   wl_signal_emit(&buffer->destroy_signal, buffer);

   if (buffer->in_use)
     printf("%s: buffer in use\n", __FUNCTION__);

   /* then, dequeue and send dequeue event */
   _e_tizen_screenmirror_buffer_dequeue(buffer);

   free(buffer);
}

static Mirror_Buffer*
_e_tizen_screenmirror_buffer_get(Mirror *mirror, struct wl_resource *resource)
{
   Mirror_Buffer *buffer = NULL;
   struct wl_listener *listener;

   listener = wl_resource_get_destroy_listener(resource, _e_tizen_screenmirror_buffer_cb_destroy);
   if (listener)
     return container_of(listener, Mirror_Buffer, destroy_listener);

   if (!(buffer = E_NEW(Mirror_Buffer, 1))) return NULL;

   buffer->mirror = mirror;
   buffer->resource = resource;

   buffer->shm_buffer = wl_shm_buffer_get(resource);
   buffer->stride = wl_shm_buffer_get_stride(buffer->shm_buffer);
   buffer->w = wl_shm_buffer_get_width(buffer->shm_buffer);
   buffer->h = wl_shm_buffer_get_height(buffer->shm_buffer);
   buffer->format = wl_shm_buffer_get_format(buffer->shm_buffer);
   buffer->data = wl_shm_buffer_get_data(buffer->shm_buffer);

   wl_signal_init(&buffer->destroy_signal);

   buffer->destroy_listener.notify = _e_tizen_screenmirror_buffer_cb_destroy;
   wl_resource_add_destroy_listener(resource, &buffer->destroy_listener);

   return buffer;
}

static Mirror*
_e_tizen_screenmirror_create(void)
{
   Mirror *mirror;

   if (!(mirror = E_NEW(Mirror, 1))) return NULL;

   return mirror;
}

static void
_e_tizen_screenmirror_destroy(Mirror *mirror)
{
   Mirror_Buffer *buffer;

   if (mirror->sw_timer) ecore_timer_del(mirror->sw_timer);

   EINA_LIST_FREE(mirror->buffer_queue, buffer)
     if (buffer) _e_tizen_screenmirror_buffer_dequeue(buffer);

   free(mirror);
}

static void
destroy_screenmirror(struct wl_resource *resource)
{
   Mirror *mirror = wl_resource_get_user_data(resource);

   _e_tizen_screenmirror_destroy(mirror);
}

static void
_e_tizen_screenmirror_cb_destroy(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void
_e_tizen_screenmirror_cb_set_stretch(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, uint32_t stretch)
{
   Mirror *mirror = wl_resource_get_user_data(resource);

   if (mirror->stretch == stretch)
     return;

   mirror->stretch = stretch;
}

static void
_e_tizen_screenmirror_cb_queue(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *buffer_resource)
{
   Mirror *mirror = wl_resource_get_user_data(resource);
   Mirror_Buffer *buffer;

   if (!wl_shm_buffer_get(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   buffer = _e_tizen_screenmirror_buffer_get(mirror, buffer_resource);
   if (!buffer)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   _e_tizen_screenmirror_buffer_queue(buffer);
}

static void
_e_tizen_screenmirror_cb_dequeue(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *buffer_resource)
{
   Mirror *mirror = wl_resource_get_user_data(resource);
   Mirror_Buffer *buffer;

   if (!wl_shm_buffer_get(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   buffer = _e_tizen_screenmirror_buffer_get(mirror, buffer_resource);
   if (!eina_list_data_find_list(mirror->buffer_queue, buffer))
     return;

   _e_tizen_screenmirror_buffer_dequeue(buffer);
}

static void
_e_tizen_screenmirror_cb_start(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   Mirror *mirror = wl_resource_get_user_data(resource);

   if (mirror->started) return;

   mirror->sw_timer = ecore_timer_add(SW_DUMP_TIMEOUT,
                                      _e_tizen_screenmirror_sw_do_dump, mirror);
   if (mirror->sw_timer)
     tizen_screenmirror_send_content(resource, TIZEN_SCREENMIRROR_CONTENT_NORMAL);

   mirror->started = EINA_TRUE;
}

static void
_e_tizen_screenmirror_cb_stop(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   Mirror *mirror = wl_resource_get_user_data(resource);

   if (!mirror->started) return;

   mirror->started = EINA_FALSE;

   tizen_screenmirror_send_stop(resource);
}

static const struct tizen_screenmirror_interface _e_tizen_screenmirror_interface = {
   _e_tizen_screenmirror_cb_destroy,
   _e_tizen_screenmirror_cb_set_stretch,
   _e_tizen_screenmirror_cb_queue,
   _e_tizen_screenmirror_cb_dequeue,
   _e_tizen_screenmirror_cb_start,
   _e_tizen_screenmirror_cb_stop
};

static void
_e_tizen_screenshooter_get_screenmirror(struct wl_client *client,
                                        struct wl_resource *resource,
                                        uint32_t id,
                                        struct wl_resource *output)
{
   int version = wl_resource_get_version(resource);
   struct wl_resource *res;
   Mirror *mirror;

   mirror = _e_tizen_screenmirror_create();
   if (!mirror)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   res = wl_resource_create(client, &tizen_screenmirror_interface, version, id);
   if (res == NULL)
     {
        _e_tizen_screenmirror_destroy(mirror);
        wl_client_post_no_memory(client);
        return;
     }

   mirror->resource = res;
   mirror->shooter = resource;
   mirror->output = output;

   wl_resource_set_implementation(res, &_e_tizen_screenmirror_interface,
                                  mirror, destroy_screenmirror);
}

static const struct tizen_screenshooter_interface _e_tizen_screenshooter_interface =
{
   _e_tizen_screenshooter_get_screenmirror
};

static void
_e_tizen_screenshooter_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   E_Comp_Data *cdata;
   struct wl_resource *res;

   if (!(cdata = data))
     {
        wl_client_post_no_memory(client);
        return;
     }

   if (!(res = wl_resource_create(client, &tizen_screenshooter_interface, MIN(version, 1), id)))
     {
        ERR("Could not create tizen_screenshooter resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_tizen_screenshooter_interface, cdata, NULL);
}

int
e_devicemgr_screenshooter_init(void)
{
   E_Comp_Data *cdata;

   if (!e_comp) return 0;
   if (!(cdata = e_comp->wl_comp_data)) return 0;
   if (!cdata->wl.disp) return 0;

   /* try to add tizen_screenshooter to wayland globals */
   if (!wl_global_create(cdata->wl.disp, &tizen_screenshooter_interface, 1,
                         cdata, _e_tizen_screenshooter_cb_bind))
     {
        ERR("Could not add tizen_screenshooter to wayland globals: %m");
        return 0;
     }

   return 1;
}

void
e_devicemgr_screenshooter_fini(void)
{
}
