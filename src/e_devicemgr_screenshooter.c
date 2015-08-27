#define E_COMP_WL
#include "e.h"
#include <wayland-server.h>
#include <Ecore_Wayland.h>
#include <Ecore_Drm.h>
#include <screenshooter-server-protocol.h>
#include <tizen-extension-server-protocol.h>
#include "e_devicemgr_screenshooter.h"
#include "e_devicemgr_video.h"
#include "e_devicemgr_buffer.h"
#include "e_devicemgr_converter.h"
#include "e_devicemgr_dpms.h"

#define DUMP_FPS     30

typedef struct _E_Mirror
{
   struct wl_resource *resource;
   struct wl_resource *shooter;
   struct wl_resource *output;

   Eina_Bool started;
   enum tizen_screenmirror_stretch stretch;

   Eina_List *buffer_queue;
   unsigned int crtc_id;
   E_Comp_Wl_Output *wl_output;
   Ecore_Drm_Output *drm_output;

   /* vblank info */
   int pipe;
   uint next_msc;
   int per_vblank;
   Eina_Bool wait_vblank;

   /* timer info when dpms off */
   Ecore_Timer *timer;

   /* converter info */
   E_Devmgr_Cvt *cvt;
   Eina_List *src_buffer_list;
   Eina_List *dst_buffer_list;

   struct wl_listener client_destroy_listener;
} E_Mirror;

typedef struct _E_Mirror_Buffer
{
   /* either shm_buffer or drm_buffer */
   struct wl_resource *resource;

   E_Mirror *mirror;

   Eina_Bool in_use;
   Eina_Bool dirty;

   /* in case of shm buffer */
   struct wl_listener destroy_listener;
} E_Mirror_Buffer;

static uint mirror_format_table[] =
{
   TIZEN_BUFFER_POOL_FORMAT_ARGB8888,
   TIZEN_BUFFER_POOL_FORMAT_XRGB8888,
   TIZEN_BUFFER_POOL_FORMAT_NV12,
   TIZEN_BUFFER_POOL_FORMAT_NV21,
};

#define NUM_MIRROR_FORMAT   (sizeof(mirror_format_table) / sizeof(mirror_format_table[0]))

static void _e_tz_screenmirror_destroy(E_Mirror *mirror);
static void _e_tz_screenmirror_buffer_dequeue(E_Mirror_Buffer *buffer);
static void _e_tz_screenmirror_vblank_handler(uint sequence, uint tv_sec, uint tv_usec, void *data);

static void
_e_tz_screenmirror_center_rect (int src_w, int src_h, int dst_w, int dst_h, Eina_Rectangle *fit)
{
   float rw, rh;

   if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !fit)
     return;

   rw = (float)src_w / dst_w;
   rh = (float)src_h / dst_h;

   if (rw > rh)
     {
        fit->w = dst_w;
        fit->h = src_h / rw;
        fit->x = 0;
        fit->y = (dst_h - fit->h) / 2;
     }
   else if (rw < rh)
     {
        fit->w = src_w / rh;
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

void
_e_tz_screenmirror_rect_scale (int src_w, int src_h, int dst_w, int dst_h, Eina_Rectangle *scale)
{
   float ratio;
   Eina_Rectangle center;

   if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !scale)
     return;

   _e_tz_screenmirror_center_rect (src_w, src_h, dst_w, dst_h, &center);

   ratio = (float)center.w / src_w;

   scale->x = scale->x * ratio + center.x;
   scale->y = scale->y * ratio + center.y;
   scale->w = scale->w * ratio;
   scale->h = scale->h * ratio;
}

static Eina_Bool
_e_tz_screenmirror_cb_timeout(void *data)
{
   E_Mirror *mirror = data;

   _e_tz_screenmirror_vblank_handler(0, 0, 0, (void*)mirror);

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
_e_tz_screenmirror_watch_vblank(E_Mirror *mirror)
{
   uint curr_msc;

    /* If not DPMS_ON, we call vblank handler directly to dump screen */
   if (e_devicemgr_dpms_get(mirror->drm_output))
     {
        if (!mirror->timer)
          mirror->timer = ecore_timer_add((double)1/DUMP_FPS,
                                          _e_tz_screenmirror_cb_timeout, mirror);
        EINA_SAFETY_ON_NULL_RETURN_VAL(mirror->timer, EINA_FALSE);

        return EINA_TRUE;
     }
   else if (mirror->timer)
     {
        ecore_timer_del(mirror->timer);
        mirror->timer = NULL;
     }

   if (mirror->wait_vblank)
     return EINA_TRUE;

   if (!e_devicemgr_drm_get_cur_msc(mirror->pipe, &curr_msc))
     {
         ERR("failed: e_devicemgr_drm_get_cur_msc");
         return EINA_FALSE;
     }

   DBG("next:%u curr:%u", mirror->next_msc, curr_msc);

   if (mirror->next_msc > curr_msc)
      curr_msc = mirror->next_msc;

   if (!e_devicemgr_drm_wait_vblank(mirror->pipe, &curr_msc, mirror))
     {
         ERR("failed: e_devicemgr_drm_wait_vblank");
         return EINA_FALSE;
     }

   mirror->next_msc = curr_msc + mirror->per_vblank;

   mirror->wait_vblank = EINA_TRUE;

   return EINA_TRUE;
}

static Eina_Bool
_e_tz_screenmirror_buffer_check(struct wl_resource *resource)
{
   if (wl_shm_buffer_get(resource) || e_drm_buffer_get(resource))
     return EINA_TRUE;

   ERR("unrecognized buffer");

   return EINA_FALSE;
}

static void
_e_tz_screenmirror_ui_buffer_cb_destroy(E_Devmgr_Buf *mbuf, void *data)
{
   E_Mirror *mirror = (E_Mirror*)data;
   mirror->src_buffer_list = eina_list_remove(mirror->src_buffer_list, mbuf);
}

static E_Devmgr_Buf*
_e_tz_screenmirror_ui_buffer_get(E_Mirror *mirror)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;
   uint handle;
   int w, h;

   /* TODO: should find the better way to find current framebuffer */
   ecore_drm_output_current_fb_info_get(mirror->drm_output, &handle, &w, &h, NULL);

   EINA_LIST_FOREACH(mirror->src_buffer_list, l, mbuf)
     if (mbuf->handles[0] == handle)
       return mbuf;

   mbuf = e_devmgr_buffer_create_hnd(handle, w, h);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   e_devmgr_buffer_free_func_add(mbuf, _e_tz_screenmirror_ui_buffer_cb_destroy, mirror);
   mirror->src_buffer_list = eina_list_append(mirror->src_buffer_list, mbuf);

   return mbuf;
}

static void
_e_tz_screenmirror_dst_buffer_cb_destroy(E_Devmgr_Buf *mbuf, void *data)
{
   E_Mirror *mirror = (E_Mirror*)data;
   mirror->dst_buffer_list = eina_list_remove(mirror->dst_buffer_list, mbuf);
}

static E_Devmgr_Buf*
_e_tz_screenmirror_dst_buffer_get(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   E_Devmgr_Buf *mbuf = NULL;
   Eina_List *l;

   if (wl_shm_buffer_get(buffer->resource))
     {
        struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(buffer->resource);
        
        EINA_SAFETY_ON_NULL_RETURN_VAL(shm_buffer, NULL);

        EINA_LIST_FOREACH(mirror->dst_buffer_list, l, mbuf)
          if (mbuf->b.shm_buffer == shm_buffer)
            return mbuf;

        mbuf = e_devmgr_buffer_create_shm(shm_buffer);

        DBG("capture buffer: %c%c%c%c %dx%d (%d)",
            FOURCC_STR(mbuf->drmfmt), mbuf->width, mbuf->height, mbuf->pitches[0]);
     }
   else if (e_drm_buffer_get(buffer->resource))
     {
        E_Drm_Buffer *drm_buffer = e_drm_buffer_get(buffer->resource);
        Tizen_Buffer *tizen_buffer = drm_buffer->driver_buffer;

        EINA_SAFETY_ON_NULL_RETURN_VAL(tizen_buffer, NULL);

        EINA_LIST_FOREACH(mirror->dst_buffer_list, l, mbuf)
          if (mbuf->b.tizen_buffer == tizen_buffer)
            return mbuf;

        mbuf = e_devmgr_buffer_create(tizen_buffer, EINA_FALSE);

        DBG("capture buffer: %c%c%c%c (%d,%d,%d) %dx%d (%d,%d,%d) (%d,%d,%d)",
            FOURCC_STR(drm_buffer->format),
            drm_buffer->name[0], drm_buffer->name[1], drm_buffer->name[2],
            drm_buffer->width, drm_buffer->height,
            drm_buffer->stride[0], drm_buffer->stride[1], drm_buffer->stride[2],
            drm_buffer->offset[0], drm_buffer->offset[1], drm_buffer->offset[2]);

     }

   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   e_devmgr_buffer_free_func_add(mbuf, _e_tz_screenmirror_dst_buffer_cb_destroy, mirror);
   mirror->dst_buffer_list = eina_list_append(mirror->dst_buffer_list, mbuf);

   return mbuf;
}

static void
_e_tz_screenmirror_cvt_callback(E_Devmgr_Cvt *cvt, E_Devmgr_Buf *src, E_Devmgr_Buf *dst, void *cvt_data)
{
   E_Mirror *mirror = (E_Mirror*)cvt_data;
   E_Mirror_Buffer *buffer;
   Eina_List *l;

   EINA_LIST_FOREACH(mirror->buffer_queue, l, buffer)
     if (dst->b.tizen_buffer->drm_buffer->resource == buffer->resource)
       {
          _e_tz_screenmirror_buffer_dequeue(buffer);
          break;
       }

#if 0
   static int i = 0;
   e_devmgr_buffer_dump(src, "in", i, 0);
   e_devmgr_buffer_dump(dst, "out", i++, 0);
#endif
}

static void
_e_tz_screenmirror_cvt_destroy(E_Mirror *mirror)
{
   if (!mirror->cvt)
     return;

   e_devmgr_cvt_destroy(mirror->cvt);
   mirror->cvt = NULL;
}

static Eina_Bool
_e_tz_screenmirror_cvt_create(E_Mirror *mirror, E_Devmgr_Buf *src, E_Devmgr_Buf *dst)
{
   E_Devmgr_Cvt_Prop sprop, dprop;

   if (mirror->cvt)
     return EINA_TRUE;

   mirror->cvt = e_devmgr_cvt_create();
   EINA_SAFETY_ON_NULL_RETURN_VAL(mirror->cvt, EINA_FALSE);

   CLEAR(sprop);
   CLEAR(dprop);
   sprop.drmfmt = src->drmfmt;
   sprop.width = src->width;
   sprop.height = src->height;
   sprop.crop.x = sprop.crop.y = 0;
   sprop.crop.w = src->width;
   sprop.crop.h = src->height;

   dprop.drmfmt = dst->drmfmt;
   dprop.width = dst->width;
   dprop.height = dst->height;
   if (mirror->stretch == TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO)
     _e_tz_screenmirror_center_rect(src->width, src->height, dst->width, dst->height, &dprop.crop);
   else
     {
        dprop.crop.x = dprop.crop.y = 0;
        dprop.crop.w = dst->width;
        dprop.crop.h = dst->height;
     }

   if (!e_devmgr_cvt_property_set(mirror->cvt, &sprop, &dprop))
       goto failed;

   e_devmgr_cvt_cb_add(mirror->cvt, _e_tz_screenmirror_cvt_callback, mirror);

   return EINA_TRUE;

failed:
   _e_tz_screenmirror_cvt_destroy(mirror);
   return EINA_FALSE;
}

static void
_e_tz_screenmirror_shm_dump(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   Eina_Rectangle dst = {0,};
   struct wl_shm_buffer *shm_buffer;
   int32_t stride;
   int32_t width, height;
   uint32_t format;
   void *data;

   shm_buffer = wl_shm_buffer_get(buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN(shm_buffer);

   stride = wl_shm_buffer_get_stride(shm_buffer);
   width = wl_shm_buffer_get_width(shm_buffer);
   height = wl_shm_buffer_get_height(shm_buffer);
   format = wl_shm_buffer_get_format(shm_buffer);
   data = wl_shm_buffer_get_data(shm_buffer);

   if (mirror->stretch == TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO)
     _e_tz_screenmirror_center_rect(mirror->wl_output->w, mirror->wl_output->h, width, height, &dst);
   else
     {
        dst.x = dst.y = 0;
        dst.w = width;
        dst.h = height;
     }

   wl_shm_buffer_begin_access(shm_buffer);
   evas_render_copy(e_comp->evas, data, stride, width, height, format,
                    mirror->wl_output->x, mirror->wl_output->y, mirror->wl_output->w, mirror->wl_output->h,
                    dst.x, dst.y, dst.w, dst.h);
   wl_shm_buffer_end_access(shm_buffer);

   DBG("dump src(%d,%d,%d,%d) dst(%d,%d %d,%d,%d,%d)",
       mirror->wl_output->x, mirror->wl_output->y, mirror->wl_output->w, mirror->wl_output->h,
       width, height, dst.x, dst.y, dst.w, dst.h);
}

static Eina_Bool
_e_tz_screenmirror_drm_dump(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   E_Devmgr_Buf *ui, *dst;

   dst = _e_tz_screenmirror_dst_buffer_get(buffer);
   EINA_SAFETY_ON_NULL_RETURN_VAL(dst, EINA_FALSE);

   if (e_devicemgr_dpms_get(mirror->drm_output))
     {
        if (buffer->dirty)
          {
             e_devmgr_buffer_clear(dst);
             buffer->dirty = EINA_FALSE;
             DBG("clear buffer");
#if 0
             static int i = 0;
             e_devmgr_buffer_dump(dst, "clear", i++, 0);
#endif
          }

        /* buffer has been cleared. return false to dequeue buffer immediately */
        return EINA_FALSE;
     }

   ui = _e_tz_screenmirror_ui_buffer_get(mirror);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ui, EINA_FALSE);

   if (!mirror->cvt)
     if (!_e_tz_screenmirror_cvt_create(mirror, ui, dst))
       return EINA_FALSE;

   if (!e_devmgr_cvt_convert(mirror->cvt, ui, dst))
     return EINA_FALSE;

   buffer->dirty = EINA_TRUE;

   return EINA_TRUE;
}

static void
_e_tz_screenmirror_dump_still(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;
   E_Devmgr_Buf *video, *ui, *dst;
   Eina_Rectangle vr, ur;
   Eina_List *video_list, *l;
   E_Video *vdo;

   dst = _e_tz_screenmirror_dst_buffer_get(buffer);
   EINA_SAFETY_ON_NULL_RETURN(dst);

   /* get ui buffer */
   ui = _e_tz_screenmirror_ui_buffer_get(mirror);
   EINA_SAFETY_ON_NULL_RETURN(ui);

   video_list = e_devicemgr_video_list_get();
   EINA_LIST_FOREACH(video_list, l, vdo)
     {
        if (mirror->drm_output != e_devicemgr_video_drm_output_get(vdo))
          continue;

        /* get video buffer */
        video = e_devicemgr_video_fb_get(vdo);
        if (!video)
          continue;

        e_devicemgr_video_pos_get(vdo, &vr.x, &vr.y);
        vr.w = video->width;
        vr.h = video->height;

        _e_tz_screenmirror_rect_scale(ui->width, ui->height, dst->width, dst->height, &vr);

        /* dump video buffer */
        e_devmgr_buffer_convert(video, dst,
                                0, 0, video->width, video->height,
                                vr.x, vr.y, vr.w, vr.h,
                                EINA_FALSE, 0, 0, 0);
     }

   ur.x = ur.y = 0;
   ur.w = ui->width;
   ur.h = ui->height;

   _e_tz_screenmirror_center_rect(ui->width, ui->height, dst->width, dst->height, &ur);

   /* dump ui buffer */
   e_devmgr_buffer_convert(ui, dst,
                           0, 0, ui->width, ui->height,
                           ur.x, ur.y, ur.w, ur.h,
                           EINA_TRUE, 0, 0, 0);
}

static void
_e_tz_screenmirror_buffer_queue(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;

   mirror->buffer_queue = eina_list_append(mirror->buffer_queue, buffer);

   if (mirror->started)
     _e_tz_screenmirror_watch_vblank(mirror);
}

static void
_e_tz_screenmirror_buffer_dequeue(E_Mirror_Buffer *buffer)
{
   E_Mirror *mirror = buffer->mirror;

   if (!eina_list_data_find_list(mirror->buffer_queue, buffer))
     return;

   mirror->buffer_queue = eina_list_remove(mirror->buffer_queue, buffer);

   /* resource == shooter means that we're using weston screenshooter
    * In case of wetson screenshooter, send a done event. Otherwise, send
    * a dequeued event for tizen_screenmirror.
    */
   if (mirror->resource == mirror->shooter)
     screenshooter_send_done(mirror->resource);
   else
     tizen_screenmirror_send_dequeued(mirror->resource, buffer->resource);
}

static void
_e_tz_screenmirror_buffer_cb_destroy(struct wl_listener *listener, void *data)
{
   E_Mirror_Buffer *buffer = container_of(listener, E_Mirror_Buffer, destroy_listener);

   if (buffer->in_use)
     NEVER_GET_HERE();

   /* then, dequeue and send dequeue event */
   _e_tz_screenmirror_buffer_dequeue(buffer);

   wl_list_remove(&buffer->destroy_listener.link);
   free(buffer);
}

static E_Mirror_Buffer*
_e_tz_screenmirror_buffer_get(E_Mirror *mirror, struct wl_resource *resource)
{
   E_Mirror_Buffer *buffer = NULL;
   struct wl_listener *listener;

   listener = wl_resource_get_destroy_listener(resource, _e_tz_screenmirror_buffer_cb_destroy);
   if (listener)
     return container_of(listener, E_Mirror_Buffer, destroy_listener);

   if (!(buffer = E_NEW(E_Mirror_Buffer, 1)))
      return NULL;

   buffer->mirror = mirror;
   buffer->resource = resource;

   buffer->destroy_listener.notify = _e_tz_screenmirror_buffer_cb_destroy;
   wl_resource_add_destroy_listener(resource, &buffer->destroy_listener);

   return buffer;
}

static void
_e_tz_screenmirror_vblank_handler(uint sequence, uint tv_sec, uint tv_usec, void *data)
{
   E_Mirror *mirror = data;
   E_Mirror_Buffer *buffer;

   DBG("seq: %u", sequence);

   mirror->wait_vblank = EINA_FALSE;

   buffer = eina_list_nth(mirror->buffer_queue, 0);

   /* can be null when client doesn't queue a buffer previously */
   if (!buffer)
     return;

   /* in case of wl_shm_buffer */
   if (wl_shm_buffer_get(buffer->resource))
     {
        _e_tz_screenmirror_shm_dump(buffer);
        _e_tz_screenmirror_buffer_dequeue(buffer);
     }
   else if (e_drm_buffer_get(buffer->resource))
     {
        /* If _e_tz_screenmirror_drm_dump is failed, we dequeue buffer at now.
         * Otherwise, _e_tz_screenmirror_buffer_dequeue will be called in cvt callback
         */
        if (!_e_tz_screenmirror_drm_dump(buffer))
          _e_tz_screenmirror_buffer_dequeue(buffer);
     }

   /* timer is a substitution for vblank during dpms off. so if timer is running,
    * we don't watch vblank events recursively.
    */
   if (!mirror->timer)
     _e_tz_screenmirror_watch_vblank(mirror);
}

static void
_e_tz_screenmirror_cb_client_destroy(struct wl_listener *listener, void *data)
{
   E_Mirror *mirror = container_of(listener, E_Mirror, client_destroy_listener);

   _e_tz_screenmirror_destroy(mirror);
}

static E_Mirror*
_e_tz_screenmirror_create(struct wl_client *client, struct wl_resource *shooter_resource, struct wl_resource *output_resource)
{
   E_Mirror *mirror = NULL;
   drmModeResPtr mode_res = NULL;
   Ecore_Drm_Output *drm_output;
   Ecore_Drm_Device *dev;
   Eina_List *devs;
   Eina_List *l, *ll;
   int i;

   mirror = E_NEW(E_Mirror, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mirror, NULL);

   mirror->stretch = TIZEN_SCREENMIRROR_STRETCH_KEEP_RATIO;
   mirror->shooter = shooter_resource;
   mirror->output = output_resource;
   mirror->wl_output = wl_resource_get_user_data(mirror->output);
   EINA_SAFETY_ON_NULL_GOTO(mirror->wl_output, fail_create);

   mode_res = drmModeGetResources(e_devmgr_drm_fd);
   EINA_SAFETY_ON_NULL_GOTO(mode_res, fail_create);

   for (i = 0; i < mode_res->count_crtcs; i++)
     {
        drmModeCrtcPtr crtc = drmModeGetCrtc(e_devmgr_drm_fd, mode_res->crtcs[i]);
        if (!crtc) continue;
        if (crtc->x != mirror->wl_output->x || crtc->y != mirror->wl_output->y)
          {
             drmModeFreeCrtc(crtc);
             continue;
          }
        mirror->crtc_id = crtc->crtc_id;
        mirror->pipe = i;
        mirror->per_vblank = (crtc->mode.vrefresh / DUMP_FPS);
        drmModeFreeCrtc(crtc);
        break;
     }
   drmModeFreeResources(mode_res);
   EINA_SAFETY_ON_FALSE_GOTO(mirror->crtc_id > 0, fail_create);

   devs = ecore_drm_devices_get();
   EINA_LIST_FOREACH(devs, l, dev)
     EINA_LIST_FOREACH(dev->outputs, ll, drm_output)
       {
          if (ecore_drm_output_crtc_id_get(drm_output) != mirror->crtc_id) continue;
          mirror->drm_output = drm_output;
          break;
       }
   EINA_SAFETY_ON_NULL_GOTO(mirror->drm_output, fail_create);

   e_devicemgr_drm_vblank_handler_add(_e_tz_screenmirror_vblank_handler, mirror);

   mirror->client_destroy_listener.notify = _e_tz_screenmirror_cb_client_destroy;
   wl_client_add_destroy_listener(client, &mirror->client_destroy_listener);

   return mirror;
fail_create:
   if (mirror) E_FREE(mirror);
   return NULL;
}

static void
_e_tz_screenmirror_destroy(E_Mirror *mirror)
{
   E_Mirror_Buffer *buffer;
   Eina_List *l, *ll;
   E_Devmgr_Buf *mbuf;

   if (!mirror)
     return;

   if (mirror->timer)
     ecore_timer_del(mirror->timer);

   if (mirror->client_destroy_listener.notify)
     {
        wl_list_remove(&mirror->client_destroy_listener.link);
        mirror->client_destroy_listener.notify = NULL;
     }

   wl_resource_set_destructor(mirror->resource, NULL);

   _e_tz_screenmirror_cvt_destroy(mirror);

   e_devicemgr_drm_vblank_handler_del(_e_tz_screenmirror_vblank_handler, mirror);

   EINA_LIST_FOREACH_SAFE(mirror->buffer_queue, l, ll, buffer)
     _e_tz_screenmirror_buffer_dequeue(buffer);

   EINA_LIST_FOREACH_SAFE(mirror->src_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   EINA_LIST_FOREACH_SAFE(mirror->dst_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   free(mirror);

#if 0
   if (e_devmgr_buffer_list_length() > 0)
     e_devmgr_buffer_list_print();
#endif
}

static void
destroy_tz_screenmirror(struct wl_resource *resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);

   _e_tz_screenmirror_destroy(mirror);
}

static void
_e_tz_screenmirror_cb_destroy(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void
_e_tz_screenmirror_cb_set_stretch(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, uint32_t stretch)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);

   if (mirror->stretch == stretch)
     return;

   mirror->stretch = stretch;
}

static void
_e_tz_screenmirror_cb_queue(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *buffer_resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);
   E_Mirror_Buffer *buffer;

   if (!_e_tz_screenmirror_buffer_check(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   buffer = _e_tz_screenmirror_buffer_get(mirror, buffer_resource);
   if (!buffer)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   _e_tz_screenmirror_buffer_queue(buffer);
}

static void
_e_tz_screenmirror_cb_dequeue(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *buffer_resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);
   E_Mirror_Buffer *buffer;

   if (!_e_tz_screenmirror_buffer_check(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   buffer = _e_tz_screenmirror_buffer_get(mirror, buffer_resource);
   if (!eina_list_data_find_list(mirror->buffer_queue, buffer))
     return;

   _e_tz_screenmirror_buffer_dequeue(buffer);
}

static void
_e_tz_screenmirror_cb_start(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);

   if (mirror->started) return;

   mirror->started = EINA_TRUE;

   if (!mirror->buffer_queue)
     return;

   _e_tz_screenmirror_watch_vblank(mirror);
}

static void
_e_tz_screenmirror_cb_stop(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   E_Mirror *mirror = wl_resource_get_user_data(resource);

   if (!mirror->started) return;

   mirror->started = EINA_FALSE;

   _e_tz_screenmirror_cvt_destroy(mirror);
   tizen_screenmirror_send_stop(resource);
}

static const struct tizen_screenmirror_interface _e_tz_screenmirror_interface = {
   _e_tz_screenmirror_cb_destroy,
   _e_tz_screenmirror_cb_set_stretch,
   _e_tz_screenmirror_cb_queue,
   _e_tz_screenmirror_cb_dequeue,
   _e_tz_screenmirror_cb_start,
   _e_tz_screenmirror_cb_stop
};

static void
_e_tz_screenshooter_get_screenmirror(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t id,
                                     struct wl_resource *output)
{
   int version = wl_resource_get_version(resource);
   E_Mirror *mirror;

   mirror = _e_tz_screenmirror_create(client, resource, output);
   if (!mirror)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   mirror->resource = wl_resource_create(client, &tizen_screenmirror_interface, version, id);
   if (mirror->resource == NULL)
     {
        _e_tz_screenmirror_destroy(mirror);
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(mirror->resource, &_e_tz_screenmirror_interface,
                                  mirror, destroy_tz_screenmirror);

   tizen_screenmirror_send_content(mirror->resource, TIZEN_SCREENMIRROR_CONTENT_NORMAL);
}

static const struct tizen_screenshooter_interface _e_tz_screenshooter_interface =
{
   _e_tz_screenshooter_get_screenmirror
};

static void
_e_tz_screenshooter_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
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

   wl_resource_set_implementation(res, &_e_tz_screenshooter_interface, cdata, NULL);
}

static void
_e_screenshooter_cb_shoot(struct wl_client *client,
                          struct wl_resource *resource,
                          struct wl_resource *output_resource,
                          struct wl_resource *buffer_resource)
{
   E_Mirror *mirror;
   E_Mirror_Buffer *buffer;

   if (!_e_tz_screenmirror_buffer_check(buffer_resource))
     {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "tizen_screenshooter failed: wrong buffer resource");
        return;
     }

   mirror = _e_tz_screenmirror_create(client, resource, output_resource);
   if (!mirror)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   /* resource == shooter means that we're using weston screenshooter */
   mirror->resource = mirror->shooter;

   buffer = _e_tz_screenmirror_buffer_get(mirror, buffer_resource);
   if (!buffer)
     {
        wl_resource_post_no_memory(resource);
        _e_tz_screenmirror_destroy(mirror);
        return;
     }

   _e_tz_screenmirror_buffer_queue(buffer);
   _e_tz_screenmirror_dump_still(buffer);
   _e_tz_screenmirror_buffer_dequeue(buffer);
   _e_tz_screenmirror_destroy(mirror);
}

static const struct screenshooter_interface _e_screenshooter_interface =
{
   _e_screenshooter_cb_shoot
};

static void
_e_screenshooter_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   E_Comp_Data *cdata;
   struct wl_resource *res;

   if (!(cdata = data))
     {
        wl_client_post_no_memory(client);
        return;
     }

   if (!(res = wl_resource_create(client, &screenshooter_interface, MIN(version, 1), id)))
     {
        ERR("Could not create screenshooter resource");
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_screenshooter_interface, cdata, NULL);
}

static uint
_e_screenmirror_buffer_get_capbilities(void *user_data)
{
   return TIZEN_BUFFER_POOL_CAPABILITY_SCREENMIRROR;
}

static uint*
_e_screenmirror_buffer_get_formats(void *user_data, int *format_cnt)
{
   uint *fmts;

   EINA_SAFETY_ON_NULL_RETURN_VAL(format_cnt, NULL);

   *format_cnt = 0;

   fmts = malloc(NUM_MIRROR_FORMAT * sizeof(uint));
   EINA_SAFETY_ON_NULL_RETURN_VAL(fmts, NULL);

   memcpy(fmts, mirror_format_table, NUM_MIRROR_FORMAT * sizeof(uint));

   *format_cnt = NUM_MIRROR_FORMAT;

   return fmts;
}

static void
_e_screenmirror_buffer_reference_buffer(void *user_data, E_Drm_Buffer *drm_buffer)
{
   Tizen_Buffer *tizen_buffer = NULL;
   int i;

   tizen_buffer = calloc(1, sizeof *tizen_buffer);
   EINA_SAFETY_ON_NULL_RETURN(tizen_buffer);

   tizen_buffer->drm_buffer = drm_buffer;
   drm_buffer->driver_buffer = tizen_buffer;

   for (i = 0; i < 3; i++)
     if (drm_buffer->name[i] > 0)
       {
         tizen_buffer->bo[i] = tbm_bo_import(e_devmgr_bufmgr, drm_buffer->name[i]);
         tizen_buffer->name[i] = drm_buffer->name[i];
       }
}

static void
_e_screenmirror_buffer_release_buffer(void *user_data, E_Drm_Buffer *drm_buffer)
{
   Tizen_Buffer *tizen_buffer = drm_buffer->driver_buffer;
   int i;

   if (!tizen_buffer) return;

   for (i = 0; i < 3; i++)
     if (tizen_buffer->bo[i])
       tbm_bo_unref(tizen_buffer->bo[i]);

   free(tizen_buffer);
   drm_buffer->driver_buffer = NULL;
}

static E_Drm_Buffer_Callbacks _e_screenmirror_buffer_callbacks =
{
   _e_screenmirror_buffer_get_capbilities,
   _e_screenmirror_buffer_get_formats,
   _e_screenmirror_buffer_reference_buffer,
   _e_screenmirror_buffer_release_buffer
};

int
e_devicemgr_screenshooter_init(void)
{
   E_Comp_Data *cdata;

   if (!e_comp) return 0;
   if (!(cdata = e_comp->wl_comp_data)) return 0;
   if (!cdata->wl.disp) return 0;

   /* try to add screenshooter to wayland globals */
   if (!wl_global_create(cdata->wl.disp, &screenshooter_interface, 1,
                         cdata, _e_screenshooter_cb_bind))
     {
        ERR("Could not add screenshooter to wayland globals");
        return 0;
     }

   /* try to add tizen_screenshooter to wayland globals */
   if (!wl_global_create(cdata->wl.disp, &tizen_screenshooter_interface, 1,
                         cdata, _e_tz_screenshooter_cb_bind))
     {
        ERR("Could not add tizen_screenshooter to wayland globals");
        return 0;
     }

   if (!e_drm_buffer_pool_init(cdata->wl.disp, &_e_screenmirror_buffer_callbacks, NULL))
     {
        ERR("Could not init e_drm_buffer_pool_init");
        return 0;
     }

   return 1;
}

void
e_devicemgr_screenshooter_fini(void)
{
}
