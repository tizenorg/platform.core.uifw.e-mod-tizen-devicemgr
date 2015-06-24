#define E_COMP_WL
#include "e.h"
#include "e_comp_wl.h"
#include <wayland-server.h>
#include <Ecore_Drm.h>
#include "e_devicemgr_video.h"
#include "e_devicemgr_buffer.h"
#include "e_devicemgr_converter.h"

#define MIN_WIDTH   32

typedef struct _E_Video E_Video;

typedef struct _E_Video_Fb
{
   E_Devmgr_Buf *mbuf;
   E_Video *video;

   /* in case of non-converting */
   E_Comp_Wl_Buffer_Ref buffer_ref;
   struct wl_listener buffer_destroy_listener;
} E_Video_Fb;

struct _E_Video
{
   E_Client *ec;

   struct wl_listener client_destroy_listener;

   /* input info */
   uint drmfmt;
   int iw, ih;
   Eina_Rectangle ir;
   Eina_List *input_buffer_list;

   /* converter info */
   void *cvt;
   int ow, oh;
   Eina_List *output_buffer_list;

   /* plane info */
   int pipe;
   int plane_id;
   int crtc_id;
   int zpos;
   int px, py;
   int crtc_w, crtc_h;

   /* vblank handling */
   Eina_List  *waiting_list;
   E_Video_Fb *current_fb;
};

static uint video_format_table[] =
{
   TIZEN_BUFFER_POOL_FORMAT_ARGB8888,
   TIZEN_BUFFER_POOL_FORMAT_XRGB8888,
   TIZEN_BUFFER_POOL_FORMAT_YUYV,
   TIZEN_BUFFER_POOL_FORMAT_UYVY,
   TIZEN_BUFFER_POOL_FORMAT_NV12,
   TIZEN_BUFFER_POOL_FORMAT_NV21,
   TIZEN_BUFFER_POOL_FORMAT_YUV420,
   TIZEN_BUFFER_POOL_FORMAT_YVU420,
   TIZEN_BUFFER_POOL_FORMAT_SN12
};

#define NUM_VIDEO_FORMAT   (sizeof(video_format_table) / sizeof(video_format_table[0]))

static Eina_List *video_list;

static void _e_video_destroy(E_Video *video);

static Eina_Bool
check_hw_restriction(int crtc_w, int buf_w,
                     int src_x, int src_w, int dst_x, int dst_w,
                     int *new_src_x, int *new_src_w,
                     int *new_dst_x, int *new_dst_w)
{
   int start, end, diff;
   Eina_Bool virtual_screen;

   *new_src_x = src_x;
   *new_src_w = src_w;
   *new_dst_x = dst_x;
   *new_dst_w = dst_w;

   if (buf_w < MIN_WIDTH || buf_w % 2)
     {
        ERR("buf_w(%d) not 2's multiple or less than %d", buf_w, MIN_WIDTH);
        return EINA_FALSE;
     }

   if (src_x > dst_x || ((dst_x - src_x) + buf_w) > crtc_w)
     virtual_screen = EINA_TRUE;
   else
     virtual_screen = EINA_FALSE;

   start = (dst_x < 0) ? 0 : dst_x;
   end = ((dst_x + dst_w) > crtc_w) ? crtc_w : (dst_x + dst_w);

   /* check window minimun width */
   if ((end - start) < MIN_WIDTH)
     {
        ERR("visible_w(%d) less than %d", end-start, MIN_WIDTH);
        return EINA_FALSE;
     }

   if (!virtual_screen)
     {
        /* Pagewidth of window (= 8 byte align / bytes-per-pixel ) */
        if ((end - start) % 2) end--;
     }
   else
     {
        /* You should align the sum of PAGEWIDTH_F and OFFSIZE_F double-word (8 byte) boundary. */
        if (end % 2) end--;
     }

   *new_dst_x = start;
   *new_dst_w = end - start;
   *new_src_w = *new_dst_w;
   diff = start - dst_x;
   *new_src_x += diff;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(*new_src_w > 0, EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(*new_dst_w > 0, EINA_FALSE);

   if (src_x != *new_src_x || src_w != *new_src_w || dst_x != *new_dst_x || dst_w != *new_dst_w)
     DBG("=> buf_w(%d) src(%d,%d) dst(%d,%d), virt(%d) start(%d) end(%d)",
         buf_w, *new_src_x, *new_src_w, *new_dst_x, *new_dst_w, virtual_screen, start, end);

   return EINA_TRUE;
}

static void
buffer_transform(int width, int height, uint32_t transform, int32_t scale,
                 int sx, int sy, int *dx, int *dy)
{
   switch (transform)
     {
      case WL_OUTPUT_TRANSFORM_NORMAL:
      default:
        *dx = sx, *dy = sy;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED:
        *dx = width - sx, *dy = sy;
        break;
      case WL_OUTPUT_TRANSFORM_90:
        *dx = height - sy, *dy = sx;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        *dx = height - sy, *dy = width - sx;
        break;
      case WL_OUTPUT_TRANSFORM_180:
        *dx = width - sx, *dy = height - sy;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        *dx = sx, *dy = height - sy;
        break;
      case WL_OUTPUT_TRANSFORM_270:
        *dx = sy, *dy = width - sx;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        *dx = sy, *dy = sx;
        break;
     }

   *dx *= scale;
   *dy *= scale;
}

static E_Video*
get_video(E_Client *ec)
{
   E_Video *video;
   Eina_List *l;
   EINA_LIST_FOREACH(video_list, l, video)
     {
        if (video->ec == ec)
          return video;
     }
   return NULL;
}

static void
_e_video_input_buffer_cb_destroy(E_Devmgr_Buf *mbuf, void *data)
{
   E_Video *video = (E_Video *)data;
   video->input_buffer_list = eina_list_remove(video->input_buffer_list, mbuf);
}

static E_Devmgr_Buf*
_e_video_input_buffer_get(E_Video *video, Tizen_Buffer *tizen_buffer, Eina_Bool is_fb)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   EINA_LIST_FOREACH(video->input_buffer_list, l, mbuf)
     {
        if (mbuf->b.tizen_buffer == tizen_buffer)
          {
             if (!MBUF_IS_CONVERTING(mbuf) && !mbuf->showing)
               return mbuf;
             else
               {
                  ERR("error: got (%d,%d,%d)buffer twice", mbuf->b.tizen_buffer->name[0],
                      mbuf->b.tizen_buffer->name[1], mbuf->b.tizen_buffer->name[2]);
                  return NULL;
               }
          }
     }

   if (is_fb)
     mbuf = e_devmgr_buffer_create_fb(tizen_buffer, EINA_FALSE);
   else
     mbuf = e_devmgr_buffer_create(tizen_buffer, EINA_FALSE);

   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   e_devmgr_buffer_free_func_add(mbuf, _e_video_input_buffer_cb_destroy, video);
   video->input_buffer_list = eina_list_append(video->input_buffer_list, mbuf);

   return mbuf;
}

static void
_e_video_output_buffer_cb_destroy(E_Devmgr_Buf *mbuf, void *data)
{
   E_Video *video = (E_Video *)data;
   video->output_buffer_list = eina_list_remove(video->output_buffer_list, mbuf);
}

static E_Devmgr_Buf*
_e_video_output_buffer_get(E_Video *video)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   EINA_LIST_FOREACH(video->output_buffer_list, l, mbuf)
     {
        if (mbuf->b.tizen_buffer && !MBUF_IS_CONVERTING(mbuf) && !mbuf->showing)
          return mbuf;
     }

   mbuf = e_devmgr_buffer_alloc_fb(video->ow, video->oh, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   e_devmgr_buffer_free_func_add(mbuf, _e_video_output_buffer_cb_destroy, video);
   video->output_buffer_list = eina_list_append(video->output_buffer_list, mbuf);

   return mbuf;
}

static Eina_Bool
_e_video_plane_info_get(E_Video *video)
{
   drmModeResPtr mode_res = NULL;
   drmModePlaneResPtr plane_res = NULL;
   uint crtc_id = 0, plane_id = 0, zpos = -1;
   uint connector_type = 0;
   int i, j;

   /* get crtc_id */
   mode_res = drmModeGetResources(e_devmgr_drm_fd);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mode_res, EINA_FALSE);
   for (i = 0; i < mode_res->count_crtcs; i++)
     {
        drmModeCrtcPtr crtc = drmModeGetCrtc(e_devmgr_drm_fd, mode_res->crtcs[i]);
        if (crtc && crtc->x <= video->ec->x && crtc->y <= video->ec->y &&
            video->ec->x < crtc->x + crtc->width && video->ec->y < crtc->y + crtc->height)
          {
             crtc_id = crtc->crtc_id;
             video->pipe = i;
             video->crtc_w = crtc->width;
             video->crtc_h = crtc->height;
             drmModeFreeCrtc(crtc);
             break;
          }
        drmModeFreeCrtc(crtc);
     }
   EINA_SAFETY_ON_FALSE_GOTO(crtc_id > 0, failed);

   /* get plane_id */
   plane_res = drmModeGetPlaneResources(e_devmgr_drm_fd);
   EINA_SAFETY_ON_FALSE_GOTO(plane_res, failed);
   for (i = 0; i < plane_res->count_planes; i++)
     {
        drmModePlanePtr plane = drmModeGetPlane(e_devmgr_drm_fd, plane_res->planes[i]);
        if (plane && plane->crtc_id == 0)
          {
             plane_id = plane->plane_id;
             drmModeFreePlane(plane);
             break;
          }
        drmModeFreePlane(plane);
     }
   EINA_SAFETY_ON_FALSE_GOTO(plane_id > 0, failed);

   /* get connector type */
   for (i = 0; i < mode_res->count_encoders; i++)
     {
        drmModeEncoderPtr encoder = drmModeGetEncoder(e_devmgr_drm_fd, mode_res->encoders[i]);
        if (encoder && encoder->crtc_id == crtc_id)
          {
             for (j = 0; j < mode_res->count_connectors; j++)
               {
                  drmModeConnectorPtr connector = drmModeGetConnector(e_devmgr_drm_fd, mode_res->connectors[j]);
                  if (connector && connector->encoder_id == encoder->encoder_id)
                    {
                       connector_type = connector->connector_type;
                       drmModeFreeConnector(connector);
                       break;
                    }
                  drmModeFreeConnector(connector);
               }
             drmModeFreeEncoder(encoder);
             break;
          }
        drmModeFreeEncoder(encoder);
     }

   /* TODO: get zpos */
   if (connector_type)
      zpos = 1;
   EINA_SAFETY_ON_FALSE_GOTO(zpos >= 0, failed);

   if (!e_devicemgr_drm_set_property(plane_id, DRM_MODE_OBJECT_PLANE, "zpos", zpos))
     goto failed;

   video->crtc_id = crtc_id;
   video->plane_id = plane_id;
   video->zpos = zpos;

   DBG("crtc_id(%d) plane_id(%d) zpos(%d)", crtc_id, plane_id, zpos);

   drmModeFreeResources(mode_res);
   drmModeFreePlaneResources(plane_res);
   return EINA_TRUE;
failed:
   if (mode_res)
      drmModeFreeResources(mode_res);
   if (plane_res)
      drmModeFreePlaneResources(plane_res);
   return EINA_FALSE;
}

static void
_e_video_geometry_info_get(E_Video *video)
{
   E_Client *ec = video->ec;
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;
   E_Comp_Wl_Subsurf_Data *sdata;
   int x1, y1, x2, y2;
   int tx1, ty1, tx2, ty2;
   int new_src_x, new_src_w;
   int new_dst_x, new_dst_w;

   /* iw, ih */
   video->iw = ec->comp_data->width_from_buffer;
   video->ih = ec->comp_data->height_from_buffer;

   /* ir */
   if (vp->buffer.src_width == wl_fixed_from_int(-1))
     {
        x1 = 0.0;
        y1 = 0.0;
        x2 = ec->comp_data->width_from_buffer;
        y2 = ec->comp_data->height_from_buffer;
     }
   else
     {
        x1 = wl_fixed_to_int(vp->buffer.src_x);
        y1 = wl_fixed_to_int(vp->buffer.src_y);
        x2 = wl_fixed_to_int(vp->buffer.src_x + vp->buffer.src_width);
        y2 = wl_fixed_to_int(vp->buffer.src_y + vp->buffer.src_height);
     }
   buffer_transform(ec->comp_data->width_from_buffer, ec->comp_data->height_from_buffer,
                    vp->buffer.transform, vp->buffer.scale, x1, y1, &tx1, &ty1);
   buffer_transform(ec->comp_data->width_from_buffer, ec->comp_data->height_from_buffer,
                    vp->buffer.transform, vp->buffer.scale, x2, y2, &tx2, &ty2);
   video->ir.x = tx1;
   video->ir.y = ty1;
   video->ir.w = tx2 - tx1;
   video->ir.h = ty2 - tx1;

   /* ow, oh */
   video->ow = ec->comp_data->width_from_viewport;
   video->oh = ec->comp_data->height_from_viewport;

   /* px, py */
   if ((sdata = ec->comp_data->sub.data))
     {
        video->px = sdata->parent->x + sdata->position.x;
        video->py = sdata->parent->y + sdata->position.y;
     }
   else
     {
        video->px = ec->x;
        video->py = ec->y;
     }

   DBG("geometry(%dx%d %d,%d,%d,%d %d,%d,%d,%d)",
       video->iw, video->ih, video->ir.x, video->ir.y, video->ir.w, video->ir.h,
       video->px, video->py, video->ow, video->oh);

   /* check hw restriction*/
   if (!check_hw_restriction(video->crtc_w, video->iw,
                             video->ir.x, video->ir.w, video->px, video->ow,
                             &new_src_x, &new_src_w, &new_dst_x, &new_dst_w))
     {
        return;
     }

   if (video->ir.x != new_src_x)
     {
        DBG("src_x changed: %d => %d", video->ir.x, new_src_x);
        video->ir.x = new_src_x;
     }
   if (video->ir.w != new_src_w)
     {
        DBG("src_w changed: %d => %d", video->ir.w, new_src_w);
        video->ir.w = new_src_w;
     }
   if (video->px != new_dst_x)
     {
        DBG("dst_x changed: %d => %d", video->px, new_dst_x);
        video->px = new_dst_x;
     }
   if (video->ow != new_dst_w)
     {
        DBG("dst_w changed: %d => %d", video->ow, new_dst_w);
        video->ow = new_dst_w;
     }

#if 0 //TODO
        if (!e_devmgr_cvt_ensure_size(&src, &dst))
            goto failed;
#endif
}

static void
_e_video_format_info_get(E_Video *video)
{
   E_Comp_Wl_Buffer *buffer;
   E_Drm_Buffer *drm_buffer;

   buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN(buffer);

   drm_buffer = e_drm_buffer_get(buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN(drm_buffer);

   if (drm_buffer->format == TIZEN_BUFFER_POOL_FORMAT_ST12)
     video->drmfmt = DRM_FORMAT_NV12MT;
   else if (drm_buffer->format == TIZEN_BUFFER_POOL_FORMAT_SN12)
     video->drmfmt = DRM_FORMAT_NV12;
   else
     video->drmfmt = drm_buffer->format;
}

static void
_e_video_buffer_cb_tb_destroy(struct wl_listener *listener, void *data)
{
   NEVER_GET_HERE();
}

static E_Video_Fb*
_e_video_frame_buffer_create(E_Video *video, E_Devmgr_Buf *mbuf)
{
   E_Video_Fb *vfb = calloc(1, sizeof(E_Video_Fb));
   EINA_SAFETY_ON_NULL_RETURN_VAL(vfb, NULL);

   vfb->mbuf = e_devmgr_buffer_ref(mbuf);
   vfb->video = video;
   if (mbuf->type == TYPE_TB)
     {
        E_Comp_Wl_Buffer *buffer = mbuf->b.tizen_buffer->buffer;
        e_comp_wl_buffer_reference(&vfb->buffer_ref, buffer);

        vfb->buffer_destroy_listener.notify = _e_video_buffer_cb_tb_destroy;
        wl_signal_add(&buffer->destroy_signal, &vfb->buffer_destroy_listener);
     }

   return vfb;
}

static void
_e_video_frame_buffer_destroy(E_Video_Fb *vfb)
{
   if (!vfb) return;
   if (vfb->mbuf)
     {
        vfb->mbuf->showing = EINA_FALSE;
        DBG("%d: hidden", MSTAMP(vfb->mbuf));
        e_devmgr_buffer_unref(vfb->mbuf);
     }
   if (vfb->buffer_destroy_listener.notify)
     wl_list_remove(&vfb->buffer_destroy_listener.link);
   e_comp_wl_buffer_reference(&vfb->buffer_ref, NULL);
   free (vfb);
}

static void
_e_video_frame_buffer_show(E_Video *video, E_Video_Fb *vfb)
{
   uint32_t fx, fy, fw, fh;
   uint target_msc;

   if (video->plane_id <= 0 || video->crtc_id <= 0)
      return;

   if (!vfb)
     {
        drmModeSetPlane (e_devmgr_drm_fd, video->plane_id, video->crtc_id,
                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        return;
     }

   EINA_SAFETY_ON_NULL_RETURN(vfb->mbuf);
   EINA_SAFETY_ON_FALSE_RETURN(vfb->mbuf->fb_id > 0);

   /* Source values are 16.16 fixed point */
   fx = ((unsigned int)video->ir.x) << 16;
   fy = ((unsigned int)video->ir.y) << 16;
   fw = ((unsigned int)video->ir.w) << 16;
   fh = ((unsigned int)video->ir.h) << 16;

   if (drmModeSetPlane(e_devmgr_drm_fd, video->plane_id, video->crtc_id, vfb->mbuf->fb_id, 0,
                       video->px, video->py, video->ow, video->oh, fx, fy, fw, fh))
     {
         ERR("drmModeSetPlane failed. plane(%d) crtc(%d) pos(%d) on: fb(%d,%dx%d,[%d,%d,%d,%d]=>[%d,%d,%d,%d])",
             video->plane_id, video->crtc_id, video->zpos,
             vfb->mbuf->fb_id, video->iw, video->ih,
             video->ir.x, video->ir.y, video->ir.w, video->ir.h,
             video->px, video->py, video->ow, video->oh);
         return;
     }

   DBG("plane(%d) crtc(%d) pos(%d) on: fb(%d,%dx%d,[%d,%d,%d,%d]=>[%d,%d,%d,%d])",
       video->plane_id, video->crtc_id, video->zpos,
       vfb->mbuf->fb_id, video->iw, video->ih,
       video->ir.x, video->ir.y, video->ir.w, video->ir.h,
       video->px, video->py, video->ow, video->oh);

   if (!e_devicemgr_drm_get_cur_msc(video->pipe, &target_msc))
     {
         ERR("failed: e_devicemgr_drm_get_cur_msc");
         return;
     }

   target_msc++;

   if (!e_devicemgr_drm_wait_vblank(video->pipe, &target_msc, video))
     {
         ERR("failed: e_devicemgr_drm_wait_vblank");
         return;
     }
}

static void
_e_video_buffer_show(E_Video *video, E_Devmgr_Buf *mbuf)
{
   E_Video_Fb *vfb = _e_video_frame_buffer_create(video, mbuf);
   EINA_SAFETY_ON_NULL_RETURN(vfb);

   video->waiting_list = eina_list_append(video->waiting_list, vfb);

   vfb->mbuf->showing = EINA_TRUE;
   DBG("%d: waiting", MSTAMP(vfb->mbuf));

   /* There are waiting fbs more than 2 */
   if (eina_list_nth(video->waiting_list, 1))
     return;

   _e_video_frame_buffer_show(video, vfb);
}

static Eina_Bool
_e_video_cvt_need(E_Video *video)
{
   if (video->drmfmt != DRM_FORMAT_XRGB8888 &&
       video->drmfmt != DRM_FORMAT_ARGB8888)
     return EINA_TRUE;

   if (video->ir.w != video->ow || video->ir.h != video->oh)
     return EINA_TRUE;

   return EINA_FALSE;
}

static void
_e_video_cvt_callback(E_Devmgr_Cvt *cvt, E_Devmgr_Buf *src, E_Devmgr_Buf *dst, void *cvt_data)
{
   E_Video *video = (E_Video*)cvt_data;

   _e_video_buffer_show(video, dst);

#if 0
   char file[128];
   static int i;
   sprintf(file, "dump/in_%dx%d_%03d", src->width, src->height, i);
   e_devmgr_buffer_dump(src, file, 0);
   sprintf(file, "dump/out_%dx%d_%03d", dst->width, dst->height, i++);
   e_devmgr_buffer_dump(dst, file, 0);
#endif
}

static void
_e_video_vblank_handler(unsigned int sequence,
                        unsigned int tv_sec, unsigned int tv_usec, void *data)
{
   E_Video *video = data;
   E_Video_Fb *vfb;

   if (video->current_fb)
     {
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
     }

   vfb = eina_list_nth(video->waiting_list, 0);
   EINA_SAFETY_ON_NULL_RETURN(vfb);
   video->waiting_list = eina_list_remove(video->waiting_list, vfb);
   video->current_fb = vfb;

   DBG("%d: showing", MSTAMP(vfb->mbuf));

   if ((vfb = eina_list_nth(video->waiting_list, 0)))
      _e_video_frame_buffer_show(video, vfb);
}

static void
_e_video_cb_client_destroy(struct wl_listener *listener, void *data)
{
   E_Video *video = container_of(listener, E_Video, client_destroy_listener);

   _e_video_destroy(video);
}

static E_Video*
_e_video_create(E_Client *ec)
{
   E_Video *video = NULL;
   E_Comp_Wl_Client_Data *cdata;
   struct wl_client *client;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ec, NULL);

   cdata = e_pixmap_cdata_get(ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN_VAL(cdata, NULL);

   client = wl_resource_get_client(cdata->wl_surface);
   EINA_SAFETY_ON_NULL_RETURN_VAL(client, NULL);

   video = calloc(1, sizeof *video);
   EINA_SAFETY_ON_NULL_RETURN_VAL(video, NULL);

   video->ec = ec;

   if (!_e_video_plane_info_get(video))
     {
        ERR("failed to find crtc & plane");
        _e_video_destroy(video);
        return NULL;
     }

   _e_video_geometry_info_get(video);
   _e_video_format_info_get(video);

   if (_e_video_cvt_need(video))
     {
        E_Devmgr_Cvt_Prop src, dst;

        video->cvt = e_devmgr_cvt_create();
        if (!video->cvt)
          {
             ERR("failed to create converter");
             _e_video_destroy(video);
             return NULL;
          }

        CLEAR(src);
        CLEAR(dst);
        src.drmfmt = video->drmfmt;
        src.width = video->iw;
        src.height = video->ih;
        src.crop = video->ir;
        dst.drmfmt = DRM_FORMAT_XRGB8888;
        dst.width = video->ow;
        dst.height = video->oh;
        dst.crop.x = dst.crop.y = 0;
        dst.crop.w = video->ow;
        dst.crop.h = video->oh;

        if (!e_devmgr_cvt_property_set(video->cvt, &src, &dst))
            goto failed;

        e_devmgr_cvt_cb_add(video->cvt, _e_video_cvt_callback, video);
     }

   video_list = eina_list_append(video_list, video);

   e_devicemgr_drm_vblank_handler_add(_e_video_vblank_handler, video);

   video->client_destroy_listener.notify = _e_video_cb_client_destroy;
   wl_client_add_destroy_listener(client, &video->client_destroy_listener);

   return video;
failed:
   if (video)
     _e_video_destroy(video);
   return NULL;
}

static void
_e_video_destroy(E_Video *video)
{
   E_Devmgr_Buf *mbuf;
   E_Video_Fb *vfb;
   Eina_List *l, *ll;

   if (!video)
      return;

   if (video->client_destroy_listener.notify)
     {
        wl_list_remove(&video->client_destroy_listener.link);
        video->client_destroy_listener.notify = NULL;
     }

   /* hide video plane first */
   _e_video_frame_buffer_show(video, NULL);

   if (video->current_fb)
     _e_video_frame_buffer_destroy(video->current_fb);

   EINA_LIST_FREE(video->waiting_list, vfb)
     _e_video_frame_buffer_destroy(vfb);

   /* destroy converter second */
   if (video->cvt)
     e_devmgr_cvt_destroy(video->cvt);

   /* others */
   EINA_LIST_FOREACH_SAFE(video->input_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   EINA_LIST_FOREACH_SAFE(video->output_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   e_devicemgr_drm_vblank_handler_del(_e_video_vblank_handler, video);

   video_list = eina_list_remove(video_list, video);

   free(video);
}

static void
_e_video_render(E_Video *video)
{
   E_Comp_Wl_Buffer *buffer;
   E_Drm_Buffer *drm_buffer;
   Tizen_Buffer *tizen_buffer;

   buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN(buffer);

   drm_buffer = e_drm_buffer_get(buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN(drm_buffer);

   DBG("video buffer: %c%c%c%c %dx%d (%d,%d,%d) (%d,%d,%d) (%d,%d,%d)",
       FOURCC_STR(drm_buffer->format), drm_buffer->width, drm_buffer->height,
       drm_buffer->name[0], drm_buffer->name[1], drm_buffer->name[2],
       drm_buffer->stride[0], drm_buffer->stride[1], drm_buffer->stride[2],
       drm_buffer->offset[0], drm_buffer->offset[1], drm_buffer->offset[2]);

   tizen_buffer = drm_buffer->driver_buffer;
   EINA_SAFETY_ON_NULL_RETURN(tizen_buffer);

   /* set tizen_buffer->buffer */
   if (!tizen_buffer->buffer)
     tizen_buffer->buffer = buffer;
   else if (tizen_buffer->buffer != buffer)
     {
        ERR("error: buffer mismatch");
        return;
     }

   DBG("got tbm buffer: %d %d %d", tizen_buffer->name[0], tizen_buffer->name[1], tizen_buffer->name[2]);

   if (video->cvt)
     {
        E_Devmgr_Buf *input_buffer = _e_video_input_buffer_get(video, tizen_buffer, EINA_FALSE);
        EINA_SAFETY_ON_NULL_RETURN(input_buffer);

        E_Devmgr_Buf *output_buffer = _e_video_output_buffer_get(video);
        EINA_SAFETY_ON_NULL_RETURN(output_buffer);

        e_devmgr_cvt_convert(video->cvt, input_buffer, output_buffer);
     }
   else
     {
        E_Devmgr_Buf *input_buffer = _e_video_input_buffer_get(video, tizen_buffer, EINA_TRUE);
        EINA_SAFETY_ON_NULL_RETURN(input_buffer);

        _e_video_buffer_show(video, input_buffer);
#if 0
         char file[128];
         static int i;
         sprintf(file, "dump/noncvt_%dx%d_%03d", input_buffer->width, input_buffer->height, i++);
         e_devmgr_buffer_dump(input_buffer, file, 1);
#endif
     }
}

static Eina_Bool
_e_video_cb_ec_buffer_change(void *data, int type, void *event)
{
   E_Client *ec;
   E_Event_Client *ev = event;
   E_Comp_Wl_Buffer *buffer;
   E_Video *video;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ec, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ec->pixmap, ECORE_CALLBACK_PASS_ON);

   buffer = e_pixmap_resource_get(ec->pixmap);

   /* buffer can be NULL in case of camera/video's mode change. Do nothing and
    * keep previous frame in this case.
    */
   if (!buffer)
      return ECORE_CALLBACK_PASS_ON;

   /* not interested in other buffer type */
   if (buffer->type != E_COMP_WL_BUFFER_TYPE_DRM)
      return ECORE_CALLBACK_PASS_ON;
   DBG("======================================");
   /* find video */
   video = get_video(ec);
   if (!video)
     {
        video = _e_video_create(ec);
        EINA_SAFETY_ON_NULL_RETURN_VAL(video, ECORE_CALLBACK_PASS_ON);
     }

   /* render */
   _e_video_render(video);
   DBG("======================================...");
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_video_cb_ec_remove(void *data, int type, void *event)
{
   E_Event_Client *ev = event;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   _e_video_destroy(get_video(ev->ec));

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_List *video_hdlrs;

static uint
_e_video_buffer_pool_cb_get_capbilities(void *user_data)
{
   return TIZEN_BUFFER_POOL_CAPABILITY_VIDEO;
}

static uint*
_e_video_buffer_pool_cb_get_formats(void *user_data, int *format_cnt)
{
   uint *fmts;

   EINA_SAFETY_ON_NULL_RETURN_VAL(format_cnt, NULL);

   *format_cnt = 0;

   fmts = malloc(NUM_VIDEO_FORMAT * sizeof(uint));
   EINA_SAFETY_ON_NULL_RETURN_VAL(fmts, NULL);

   memcpy(fmts, video_format_table, NUM_VIDEO_FORMAT * sizeof(uint));

   *format_cnt = NUM_VIDEO_FORMAT;

   return fmts;
}

static void
_e_video_buffer_pool_cb_reference_buffer(void *user_data, E_Drm_Buffer *drm_buffer)
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
_e_video_buffer_pool_cb_release_buffer(void *user_data, E_Drm_Buffer *drm_buffer)
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

static E_Drm_Buffer_Callbacks _e_video_buffer_pool_callbacks =
{
   _e_video_buffer_pool_cb_get_capbilities,
   _e_video_buffer_pool_cb_get_formats,
   _e_video_buffer_pool_cb_reference_buffer,
   _e_video_buffer_pool_cb_release_buffer
};

int
e_devicemgr_video_init(void)
{
   E_Comp_Data *cdata;

   if (!e_comp) return 0;
   if (!(cdata = e_comp->wl_comp_data)) return 0;
   if (!cdata->wl.disp) return 0;

   if (!e_drm_buffer_pool_init(cdata->wl.disp, &_e_video_buffer_pool_callbacks, NULL))
     {
        ERR("Could not init e_drm_buffer_pool_init");
        return 0;
     }

   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_BUFFER_CHANGE,
                         _e_video_cb_ec_buffer_change, NULL);
   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_REMOVE,
                         _e_video_cb_ec_remove, NULL);
   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_HIDE,
                         _e_video_cb_ec_remove, NULL);
#if 0 //TODO
   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_MOVE,
                         _e_devicemgr_video_cb_buffer_change, NULL);
   E_LIST_HANDLER_APPEND(video_hdlrs, E_EVENT_CLIENT_SHOW,
                         _e_devicemgr_video_cb_buffer_change, NULL);
#endif

   return 1;
}

void
e_devicemgr_video_fini(void)
{
}
