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
   Eina_Rectangle visible_r;        /* frame buffer's visible rect */

   E_Video *video;

   /* in case of non-converting */
   E_Comp_Wl_Buffer_Ref buffer_ref;
} E_Video_Fb;

struct _E_Video
{
   E_Client *ec;

   struct wl_listener client_destroy_listener;

   /* input info */
   uint drmfmt;
   Eina_List *input_buffer_list;

   /* plane info */
   int pipe;
   int plane_id;
   int crtc_id;
   int zpos;
   int crtc_w, crtc_h;

   struct
     {
        int input_w, input_h;    /* input buffer's size */
        Eina_Rectangle input_r;  /* input buffer's content rect */
        Eina_Rectangle output_r; /* video plane rect */
        int hflip, vflip;        /* horizontal & vertical flip */
        int degree;              /* rotation degree */
     } geo, old_geo;

   /* converter info */
   void *cvt;
   Eina_Bool cvt_need_configure;
   Eina_Rectangle cvt_r;    /* converter dst content rect */
   Eina_List *cvt_buffer_list;

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
static void _e_video_frame_buffer_show(E_Video *video, E_Video_Fb *vfb);
static void _e_video_frame_buffer_destroy(E_Video_Fb *vfb);

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
find_video_from_ec(E_Client *ec)
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

static E_Video*
find_video_from_input_buffer(E_Devmgr_Buf *mbuf)
{
   E_Video *video;
   Eina_List *l;

   EINA_LIST_FOREACH(video_list, l, video)
     if (eina_list_data_find(video->input_buffer_list, mbuf))
       return video;

   return NULL;
}

static void
_e_video_input_buffer_cb_destroy(struct wl_listener *listener, void *data)
{
   E_Devmgr_Buf *mbuf = container_of(listener, E_Devmgr_Buf, buffer_destroy_listener);
   E_Video *video = find_video_from_input_buffer(mbuf);

   video->input_buffer_list = eina_list_remove(video->input_buffer_list, mbuf);

   if (mbuf->buffer_destroy_listener.notify)
     {
        wl_list_remove(&mbuf->buffer_destroy_listener.link);
        mbuf->buffer_destroy_listener.notify = NULL;
     }
   e_devmgr_buffer_unref(mbuf);

   /* in case input_buffer is framebuffer */
   if (!video->cvt)
     {
        E_Video_Fb *temp;
        Eina_List *l, *ll;

        /* if current fb is destroyed */
        if (video->current_fb && video->current_fb->mbuf == mbuf)
          {
             _e_video_frame_buffer_show(video, NULL);
             _e_video_frame_buffer_destroy(video->current_fb);
             video->current_fb = NULL;
             DBG("current fb destroyed");
             return;
          }

        /* if waiting fb is destroyed */
        EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, temp)
          if (temp->mbuf == mbuf)
            {
                video->waiting_list = eina_list_remove(video->waiting_list, temp);
               _e_video_frame_buffer_destroy(temp);
               return;
            }
     }
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

   video->input_buffer_list = eina_list_append(video->input_buffer_list, mbuf);

   if (tizen_buffer->buffer)
     {
        mbuf->buffer_destroy_listener.notify = _e_video_input_buffer_cb_destroy;
        wl_signal_add(&tizen_buffer->buffer->destroy_signal, &mbuf->buffer_destroy_listener);
     }

   return mbuf;
}

static E_Devmgr_Buf*
_e_video_cvt_buffer_get(E_Video *video, int width, int height)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   EINA_LIST_FOREACH(video->cvt_buffer_list, l, mbuf)
     {
        if (!MBUF_IS_CONVERTING(mbuf) && !mbuf->showing)
          return mbuf;
     }

   mbuf = e_devmgr_buffer_alloc_fb(width, height, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   video->cvt_buffer_list = eina_list_append(video->cvt_buffer_list, mbuf);

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

   /* input geometry */
   switch (vp->buffer.transform)
     {
      case WL_OUTPUT_TRANSFORM_NORMAL:
      case WL_OUTPUT_TRANSFORM_180:
      case WL_OUTPUT_TRANSFORM_FLIPPED:
      case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      default:
        video->geo.input_w = ec->comp_data->width_from_buffer * vp->buffer.scale;
        video->geo.input_h = ec->comp_data->height_from_buffer * vp->buffer.scale;
        break;
      case WL_OUTPUT_TRANSFORM_90:
      case WL_OUTPUT_TRANSFORM_270:
      case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        video->geo.input_w = ec->comp_data->height_from_buffer * vp->buffer.scale;
        video->geo.input_h = ec->comp_data->width_from_buffer * vp->buffer.scale;
        break;
     }

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

   video->geo.input_r.x = (tx1 <= tx2) ? tx1 : tx2;
   video->geo.input_r.y = (ty1 <= ty2) ? ty1 : ty2;
   video->geo.input_r.w = (tx1 <= tx2) ? tx2 - tx1 : tx1 - tx2;
   video->geo.input_r.h = (ty1 <= ty2) ? ty2 - ty1 : ty1 - ty2;

   /* output geometry */
   if ((sdata = ec->comp_data->sub.data))
     {
        video->geo.output_r.x = sdata->parent->x + sdata->position.x;
        video->geo.output_r.y = sdata->parent->y + sdata->position.y;
     }
   else
     {
        video->geo.output_r.x = ec->x;
        video->geo.output_r.y = ec->y;
     }
   video->geo.output_r.w = ec->comp_data->width_from_viewport;
   video->geo.output_r.h = ec->comp_data->height_from_viewport;

   switch (vp->buffer.transform)
     {
      case WL_OUTPUT_TRANSFORM_NORMAL:
      default:
        video->geo.hflip = 0, video->geo.vflip = 0, video->geo.degree = 0;
        break;
      case WL_OUTPUT_TRANSFORM_90:
        video->geo.hflip = 0, video->geo.vflip = 0, video->geo.degree = 270;
        break;
      case WL_OUTPUT_TRANSFORM_180:
        video->geo.hflip = 0, video->geo.vflip = 0, video->geo.degree = 180;
        break;
      case WL_OUTPUT_TRANSFORM_270:
        video->geo.hflip = 0, video->geo.vflip = 0, video->geo.degree = 90;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED:
        video->geo.hflip = 1, video->geo.vflip = 0, video->geo.degree = 0;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        video->geo.hflip = 1, video->geo.vflip = 0, video->geo.degree = 270;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        video->geo.hflip = 1, video->geo.vflip = 0, video->geo.degree = 180;
        break;
      case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        video->geo.hflip = 1, video->geo.vflip = 0, video->geo.degree = 90;
        break;
     }

   DBG("geometry(%dx%d %d,%d,%d,%d %d,%d,%d,%d %d&%d %d)",
       video->geo.input_w, video->geo.input_h,
       video->geo.input_r.x, video->geo.input_r.y, video->geo.input_r.w, video->geo.input_r.h,
       video->geo.output_r.x, video->geo.output_r.y, video->geo.output_r.w, video->geo.output_r.h,
       video->geo.hflip, video->geo.vflip, video->geo.degree);
}

static void
_e_video_geometry_calculate(E_Video *video)
{
   if ((video->geo.output_r.x + video->geo.output_r.w) > video->crtc_w)
     {
        int orig = video->geo.output_r.w;
        video->geo.output_r.w = video->crtc_w - video->geo.output_r.x;
        video->geo.input_r.w = video->geo.input_r.w * ((float)video->geo.output_r.w / orig);
     }
   if ((video->geo.output_r.y + video->geo.output_r.h) > video->crtc_h)
     {
        int orig = video->geo.output_r.h;
        video->geo.output_r.h = video->crtc_h - video->geo.output_r.y;
        video->geo.input_r.h = video->geo.input_r.h * ((float)video->geo.output_r.h / orig);
     }

   if ((video->geo.input_r.x + video->geo.input_r.w) > video->geo.input_w)
     video->geo.input_r.w = video->geo.input_w - video->geo.input_r.x;
   if ((video->geo.input_r.y + video->geo.input_r.h) > video->geo.input_h)
     video->geo.input_r.h = video->geo.input_h - video->geo.input_r.y;
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

static E_Video_Fb*
_e_video_frame_buffer_create(E_Video *video, E_Devmgr_Buf *mbuf, Eina_Rectangle *visible)
{
   E_Video_Fb *vfb = calloc(1, sizeof(E_Video_Fb));
   EINA_SAFETY_ON_NULL_RETURN_VAL(vfb, NULL);

   vfb->mbuf = e_devmgr_buffer_ref(mbuf);
   vfb->video = video;
   vfb->visible_r = *visible;

   if (mbuf->type == TYPE_TB)
     {
        E_Comp_Wl_Buffer *buffer = mbuf->b.tizen_buffer->buffer;
        e_comp_wl_buffer_reference(&vfb->buffer_ref, buffer);
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
   e_comp_wl_buffer_reference(&vfb->buffer_ref, NULL);
   free (vfb);
}

static void
_e_video_frame_buffer_show(E_Video *video, E_Video_Fb *vfb)
{
   uint32_t fx, fy, fw, fh;
   uint target_msc;
   int new_src_x, new_src_w;
   int new_dst_x, new_dst_w;

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

   /* check hw restriction*/
   if (!check_hw_restriction(video->crtc_w, vfb->mbuf->pitches[0]>>2,
                             vfb->visible_r.x, vfb->visible_r.w,
                             video->geo.output_r.x, vfb->visible_r.w,
                             &new_src_x, &new_src_w, &new_dst_x, &new_dst_w))
     {
        return;
     }

   if (vfb->visible_r.x != new_src_x)
     DBG("src_x changed: %d => %d", vfb->visible_r.x, new_src_x);
   if (vfb->visible_r.w != new_src_w)
     DBG("src_w changed: %d => %d", vfb->visible_r.w, new_src_w);
   if (video->geo.output_r.x != new_dst_x)
     DBG("dst_x changed: %d => %d", video->geo.output_r.x, new_dst_x);
   if (vfb->visible_r.w != new_dst_w)
     DBG("dst_w changed: %d => %d", vfb->visible_r.w, new_dst_w);

   /* Source values are 16.16 fixed point */
   fx = ((unsigned int)new_src_x) << 16;
   fy = ((unsigned int)vfb->visible_r.y) << 16;
   fw = ((unsigned int)new_src_w) << 16;
   fh = ((unsigned int)vfb->visible_r.h) << 16;

   if (drmModeSetPlane(e_devmgr_drm_fd, video->plane_id, video->crtc_id, vfb->mbuf->fb_id, 0,
                       new_dst_x, video->geo.output_r.y,
                       new_dst_w, vfb->visible_r.h,
                       fx, fy, fw, fh))
     {
         ERR("failed: plane(%d) crtc(%d) pos(%d) on: fb(%d,%dx%d,[%d,%d,%d,%d]=>[%d,%d,%d,%d])",
             video->plane_id, video->crtc_id, video->zpos,
             vfb->mbuf->fb_id, vfb->mbuf->width, vfb->mbuf->height,
             new_src_x, vfb->visible_r.y, new_src_w, vfb->visible_r.h,
             new_dst_x, video->geo.output_r.y, new_dst_w, vfb->visible_r.h);
         return;
     }

   DBG("plane(%d) crtc(%d) pos(%d) on: fb(%d,%dx%d,[%d,%d,%d,%d]=>[%d,%d,%d,%d])",
       video->plane_id, video->crtc_id, video->zpos,
       vfb->mbuf->fb_id, vfb->mbuf->width, vfb->mbuf->height,
       new_src_x, vfb->visible_r.y, new_src_w, vfb->visible_r.h,
       new_dst_x, video->geo.output_r.y, new_dst_w, vfb->visible_r.h);

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
_e_video_buffer_show(E_Video *video, E_Devmgr_Buf *mbuf, Eina_Rectangle *visible)
{
   E_Video_Fb *vfb = _e_video_frame_buffer_create(video, mbuf, visible);
   EINA_SAFETY_ON_NULL_RETURN(vfb);

   video->waiting_list = eina_list_append(video->waiting_list, vfb);

   vfb->mbuf->showing = EINA_TRUE;
   DBG("%d: waiting", MSTAMP(vfb->mbuf));

   /* There are waiting fbs more than 2 */
   if (eina_list_nth(video->waiting_list, 1))
     return;

   _e_video_frame_buffer_show(video, vfb);
}

static void
_e_video_cvt_callback(E_Devmgr_Cvt *cvt,
                      E_Devmgr_Buf *input_buffer,
                      E_Devmgr_Buf *cvt_buffer,
                      void *cvt_data)
{
   E_Video *video = (E_Video*)cvt_data;

   _e_video_buffer_show(video, cvt_buffer, &video->cvt_r);

#if 0
   char file[128];
   static int i;
   sprintf(file, "dump/in_%dx%d_%03d", src->width, src->height, i);
   e_devmgr_buffer_dump(src, file, 0);
   sprintf(file, "dump/out_%dx%d_%03d", dst->width, dst->height, i++);
   e_devmgr_buffer_dump(dst, file, 0);
#endif
}

static Eina_Bool
_e_video_cvt_configure(E_Video *video, E_Devmgr_Buf *cvt_buffer)
{
   E_Devmgr_Cvt_Prop src, dst;

   if (!e_devmgr_cvt_pause(video->cvt))
     return EINA_FALSE;

   CLEAR(src);
   src.drmfmt = video->drmfmt;
   src.width = video->geo.input_w;
   src.height = video->geo.input_h;
   src.crop = video->geo.input_r;

   CLEAR(dst);
   dst.drmfmt = DRM_FORMAT_XRGB8888;
   dst.width = cvt_buffer->pitches[0] >> 2;
   dst.height = cvt_buffer->height;
   dst.crop.w = cvt_buffer->width;
   dst.crop.h = cvt_buffer->height;

   dst.hflip = video->geo.hflip;
   dst.vflip = video->geo.vflip;
   dst.degree = video->geo.degree;

   if (!e_devmgr_cvt_ensure_size (&src, &dst))
     return EINA_FALSE;

   if (!e_devmgr_cvt_property_set(video->cvt, &src, &dst))
     return EINA_FALSE;

   video->cvt_r = dst.crop;

   return EINA_TRUE;
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

   _e_video_format_info_get(video);

   if (!_e_video_plane_info_get(video))
     {
        ERR("failed to find crtc & plane");
        _e_video_destroy(video);
        return NULL;
     }

   video_list = eina_list_append(video_list, video);

   e_devicemgr_drm_vblank_handler_add(_e_video_vblank_handler, video);

   video->client_destroy_listener.notify = _e_video_cb_client_destroy;
   wl_client_add_destroy_listener(client, &video->client_destroy_listener);

   return video;
}

static void
_e_video_destroy(E_Video *video)
{
   E_Devmgr_Buf *mbuf;
   E_Video_Fb *vfb;

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
     {
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
     }

   EINA_LIST_FREE(video->waiting_list, vfb)
     _e_video_frame_buffer_destroy(vfb);

   /* destroy converter second */
   if (video->cvt)
     e_devmgr_cvt_destroy(video->cvt);

   /* others */
   EINA_LIST_FREE(video->input_buffer_list, mbuf)
     {
        if (mbuf->buffer_destroy_listener.notify)
          {
             wl_list_remove(&mbuf->buffer_destroy_listener.link);
             mbuf->buffer_destroy_listener.notify = NULL;
          }
        e_devmgr_buffer_unref(mbuf);
     }

   EINA_LIST_FREE(video->cvt_buffer_list, mbuf)
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
   E_Devmgr_Buf *cvt_buffer = NULL;
   E_Devmgr_Buf *input_buffer = NULL;

   buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN(buffer);

   drm_buffer = e_drm_buffer_get(buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN(drm_buffer);

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

   DBG("video buffer: %c%c%c%c %dx%d (%d,%d,%d) (%d,%d,%d) (%d,%d,%d)",
       FOURCC_STR(drm_buffer->format), drm_buffer->width, drm_buffer->height,
       drm_buffer->name[0], drm_buffer->name[1], drm_buffer->name[2],
       drm_buffer->stride[0], drm_buffer->stride[1], drm_buffer->stride[2],
       drm_buffer->offset[0], drm_buffer->offset[1], drm_buffer->offset[2]);

   if (video->cvt)
     {
        int cvt_w = video->geo.output_r.w;
        int cvt_h = video->geo.output_r.h;

        input_buffer = _e_video_input_buffer_get(video, tizen_buffer, EINA_FALSE);
        EINA_SAFETY_ON_NULL_GOTO(input_buffer, render_fail);

        cvt_buffer = _e_video_cvt_buffer_get(video, cvt_w, cvt_h);
        EINA_SAFETY_ON_NULL_GOTO(cvt_buffer, render_fail);

        /* configure a converter if needed */
        if (video->cvt_need_configure)
          {
             if (!_e_video_cvt_configure(video, cvt_buffer))
               goto render_fail;
             video->cvt_need_configure = EINA_FALSE;
          }

        if (!e_devmgr_cvt_convert(video->cvt, input_buffer, cvt_buffer))
          goto render_fail;
     }
   else
     {
        input_buffer = _e_video_input_buffer_get(video, tizen_buffer, EINA_TRUE);
        EINA_SAFETY_ON_NULL_GOTO(input_buffer, render_fail);

        _e_video_buffer_show(video, input_buffer, &video->geo.input_r);
#if 0
         char file[128];
         static int i;
         sprintf(file, "dump/noncvt_%dx%d_%03d", input_buffer->width, input_buffer->height, i++);
         e_devmgr_buffer_dump(input_buffer, file, 1);
#endif
     }

   return;

render_fail:
   if (input_buffer)
     {
        video->input_buffer_list = eina_list_remove(video->input_buffer_list, input_buffer);
        if (input_buffer->buffer_destroy_listener.notify)
          {
             wl_list_remove(&input_buffer->buffer_destroy_listener.link);
             input_buffer->buffer_destroy_listener.notify = NULL;
          }
        e_devmgr_buffer_unref(input_buffer);
     }

   if (cvt_buffer)
     {
        video->cvt_buffer_list = eina_list_remove(video->cvt_buffer_list, cvt_buffer);
        e_devmgr_buffer_unref(cvt_buffer);
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
   video = find_video_from_ec(ec);
   if (!video)
     {
        video = _e_video_create(ec);
        EINA_SAFETY_ON_NULL_RETURN_VAL(video, ECORE_CALLBACK_PASS_ON);
     }

   /* 1. get geometry information with buffer scale, transform and viewport */
   _e_video_geometry_info_get(video);

   /* 2. In case a buffer is RGB and the size of input/output *WHICH CLIENT SENT*
    * is same, we don't need to convert video. Otherwise, we should need a converter.
    */
   if (!IS_RGB(video->drmfmt) ||
       video->geo.input_r.w != video->geo.output_r.w ||
       video->geo.input_r.h != video->geo.output_r.h ||
       video->geo.hflip || video->geo.vflip || video->geo.degree)
     if (!video->cvt)
       {
          video->cvt = e_devmgr_cvt_create();
          EINA_SAFETY_ON_NULL_RETURN_VAL(video->cvt, EINA_FALSE);
  
          e_devmgr_cvt_cb_add(video->cvt, _e_video_cvt_callback, video);
       }

   /* 3. change geometry information */
   _e_video_geometry_calculate(video);

   if (memcmp(&video->old_geo, &video->geo, sizeof(video->geo)))
      video->cvt_need_configure = EINA_TRUE;

   /* 4. render */
   _e_video_render(video);
   video->old_geo = video->geo;

   DBG("======================================...");

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_video_cb_ec_remove(void *data, int type, void *event)
{
   E_Event_Client *ev = event;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   _e_video_destroy(find_video_from_ec(ev->ec));

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
