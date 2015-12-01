#include "e_devicemgr_video.h"
#include "e_devicemgr_dpms.h"
#include "e_devicemgr_converter.h"

#define BUFFER_MAX_COUNT   3
#define MIN_WIDTH   32

#define VER(fmt,arg...)   ERR("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)
#define VWR(fmt,arg...)   WRN("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)
#define VIN(fmt,arg...)   INF("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)
#define VDB(fmt,arg...)   DBG("window(0x%08"PRIxPTR"): "fmt, video->window, ##arg)

typedef struct _E_Video_Fb
{
   E_Devmgr_Buf *mbuf;
   Eina_Rectangle visible_r;        /* frame buffer's visible rect */

   E_Video *video;

   Eina_Bool fake;

   /* in case of non-converting */
   E_Comp_Wl_Buffer_Ref buffer_ref;
} E_Video_Fb;

struct _E_Video
{
   struct wl_resource *video_object;
   struct wl_resource *surface;

   E_Client *ec;
   Ecore_Window window;

   /* input info */
   tbm_format tbmfmt;
   Eina_List *input_buffer_list;

   /* plane info */
   Ecore_Drm_Output *drm_output;
   int pipe;
   int plane_id;
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
   Eina_Rectangle cvt_r;    /* converter dst content rect */
   Eina_List *cvt_buffer_list;
   Eina_List *next_buffer;

   /* vblank handling */
   Eina_Bool   wait_vblank;
   Eina_List  *waiting_list;
   E_Video_Fb *current_fb;

   Eina_Bool   need_render;
   Eina_List  *fake_buffer_list;
};

static uint video_format_table[] =
{
   TBM_FORMAT_ARGB8888,
   TBM_FORMAT_XRGB8888,
   TBM_FORMAT_YUYV,
   TBM_FORMAT_UYVY,
   TBM_FORMAT_NV12,
   TBM_FORMAT_NV21,
   TBM_FORMAT_YUV420,
   TBM_FORMAT_YVU420
};

#define NUM_VIDEO_FORMAT   (sizeof(video_format_table) / sizeof(video_format_table[0]))

static Eina_List *video_list;

static void _e_video_destroy(E_Video *video);
static void _e_video_render(E_Video *video, Eina_Bool fake);
static void _e_video_frame_buffer_show(E_Video *video, E_Video_Fb *vfb);
static void _e_video_frame_buffer_destroy(E_Video_Fb *vfb);
static void _e_video_wait_vblank(E_Video *video);

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
find_video_with_surface(struct wl_resource *surface)
{
   E_Video *video;
   Eina_List *l;
   EINA_LIST_FOREACH(video_list, l, video)
     {
        if (video->surface == surface)
          return video;
     }
   return NULL;
}

static int
_e_video_set_property(unsigned int obj_id, unsigned int obj_type,
                      const char *prop_name, unsigned int value)
{
   drmModeObjectPropertiesPtr props = NULL;
   unsigned int i;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(obj_id > 0, 0);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(obj_type > 0, 0);
   EINA_SAFETY_ON_NULL_RETURN_VAL(prop_name, 0);

   props = drmModeObjectGetProperties(e_devmgr_drm_fd, obj_id, obj_type);
   if (!props)
     {
        ERR("error: drmModeObjectGetProperties. (%m)");
        return 0;
     }
   for (i = 0; i < props->count_props; i++)
     {
        drmModePropertyPtr prop = drmModeGetProperty(e_devmgr_drm_fd, props->props[i]);
        int ret;
        if (!prop)
          {
             ERR("error: drmModeGetProperty. (%m)");
             drmModeFreeObjectProperties(props);
             return 0;
          }
        if (!strcmp(prop->name, prop_name))
          {
             ret = drmModeObjectSetProperty(e_devmgr_drm_fd, obj_id, obj_type, prop->prop_id, value);
             if (ret < 0)
               {
                  ERR("error: drmModeObjectSetProperty. (%m)");
                  drmModeFreeProperty(prop);
                  drmModeFreeObjectProperties(props);
                  return 0;
               }
             drmModeFreeProperty(prop);
             drmModeFreeObjectProperties(props);

             return 1;
          }
        drmModeFreeProperty(prop);
     }

   ERR("error: drm set property.");
   drmModeFreeObjectProperties(props);
   return 0;
}
static void
_e_video_input_buffer_cb_destroy(E_Devmgr_Buf *mbuf, void *data)
{
   E_Video *video = data;
   E_Video_Fb *vfb;
   Eina_List *l, *ll;

   video->input_buffer_list = eina_list_remove(video->input_buffer_list, mbuf);
   video->fake_buffer_list = eina_list_remove(video->fake_buffer_list, mbuf);

   if (mbuf->comp_buffer)
     VDB("wl_buffer@%d done", wl_resource_get_id(mbuf->comp_buffer->resource));

   /* if cvt exists, it means that input buffer is not a frame buffer. */
   if (video->cvt)
     return;

   /* if current fb is destroyed */
   if (video->current_fb && video->current_fb->mbuf == mbuf)
     {
        _e_video_frame_buffer_show(video, NULL);
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
        VDB("current fb destroyed");
        return;
     }

   /* if waiting fb is destroyed */
   EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, vfb)
     if (vfb->mbuf == mbuf)
       {
           video->waiting_list = eina_list_remove(video->waiting_list, vfb);
          _e_video_frame_buffer_destroy(vfb);
          return;
       }
}

static E_Devmgr_Buf*
_e_video_input_buffer_get(E_Video *video, E_Comp_Wl_Buffer *comp_buffer, Eina_Bool is_fb, Eina_Bool fake)
{
   E_Devmgr_Buf *mbuf;

   mbuf = e_devmgr_buffer_create(comp_buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   if (is_fb)
     if (!e_devmgr_buffer_add_fb(mbuf))
       {
          e_devmgr_buffer_unref(mbuf);
          return NULL;
       }

   mbuf->comp_buffer = comp_buffer;

   video->input_buffer_list = eina_list_append(video->input_buffer_list, mbuf);
   e_devmgr_buffer_free_func_add(mbuf, _e_video_input_buffer_cb_destroy, video);

   return mbuf;
}

static void
_e_video_input_buffer_valid(E_Video *video, E_Comp_Wl_Buffer *comp_buffer)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   EINA_LIST_FOREACH(video->input_buffer_list, l, mbuf)
     {
        tbm_surface_h tbm_surf;
        tbm_bo bo;
        uint32_t size = 0, offset = 0, pitch = 0;

        if (!mbuf->comp_buffer) continue;
        if (mbuf->resource == comp_buffer->resource)
          {
             WRN("got wl_buffer@%d twice", wl_resource_get_id(comp_buffer->resource));
             return;
          }

        tbm_surf = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, comp_buffer->resource);
        bo = tbm_surface_internal_get_bo(tbm_surf, 0);
        tbm_surface_internal_get_plane_data(tbm_surf, 0, &size, &offset, &pitch);

        if (mbuf->names[0] == tbm_bo_export(bo) && mbuf->offsets[0] == offset)
          {
             WRN("can tearing: wl_buffer@%d, wl_buffer@%d are same. gem_name(%d)",
                 wl_resource_get_id(mbuf->resource),
                 wl_resource_get_id(comp_buffer->resource), mbuf->names[0]);
             return;
          }
     }
}

static void
_e_video_cvt_buffer_cb_destroy(E_Devmgr_Buf *mbuf, void *data)
{
   E_Video *video = data;

   video->cvt_buffer_list = eina_list_remove(video->cvt_buffer_list, mbuf);

   VDB("mbuf(%d) list_remove", MSTAMP(mbuf));
}

static E_Devmgr_Buf*
_e_video_cvt_buffer_get(E_Video *video, int width, int height)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;
   int i = 0;

   if (video->cvt_buffer_list)
     {
        mbuf = eina_list_data_get(video->cvt_buffer_list);
        EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

        /* if we need bigger cvt_buffers, destroy all cvt_buffers and create */
        if (width > (mbuf->pitches[0] >> 2))
          {
             E_Video_Fb *vfb;
             Eina_List *ll;

             EINA_LIST_FOREACH_SAFE(video->cvt_buffer_list, l, ll, mbuf)
               {
                  video->cvt_buffer_list = eina_list_remove(video->cvt_buffer_list, mbuf);
                  e_devmgr_buffer_unref(mbuf);
               }
             if (video->cvt_buffer_list)
               NEVER_GET_HERE();

             EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, vfb)
               {
                  video->waiting_list = eina_list_remove(video->waiting_list, vfb);
                  _e_video_frame_buffer_destroy(vfb);
               }
             if (video->waiting_list)
               NEVER_GET_HERE();
          }
     }

   if (!video->cvt_buffer_list)
     {
        for (i = 0; i < BUFFER_MAX_COUNT; i++)
          {
             mbuf = e_devmgr_buffer_alloc(width, height, TBM_FORMAT_ARGB8888);
             EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

             if (!e_devmgr_buffer_add_fb(mbuf))
               {
                  e_devmgr_buffer_unref(mbuf);
                  return NULL;
               }

             e_devmgr_buffer_free_func_add(mbuf, _e_video_cvt_buffer_cb_destroy, video);

             video->cvt_buffer_list = eina_list_append(video->cvt_buffer_list, mbuf);
          }

        video->next_buffer = video->cvt_buffer_list;
     }

   EINA_SAFETY_ON_NULL_RETURN_VAL(video->cvt_buffer_list, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(video->next_buffer, NULL);

   l = video->next_buffer;
   while ((mbuf = video->next_buffer->data))
     {
        video->next_buffer = (video->next_buffer->next) ? video->next_buffer->next : video->cvt_buffer_list;

        if (!MBUF_IS_CONVERTING(mbuf) && !mbuf->showing)
          return mbuf;

        if (l == video->next_buffer)
          {
             VWR("all video framebuffers in use (max:%d)", BUFFER_MAX_COUNT);
             return NULL;
          }
     }

   return NULL;
}

static Ecore_Drm_Output*
_e_video_plane_drm_output_find(unsigned int crtc_id)
{
   Ecore_Drm_Device *dev;
   Ecore_Drm_Output *output;
   Eina_List *devs;
   Eina_List *l, *ll;

   devs = eina_list_clone(ecore_drm_devices_get());
   EINA_LIST_FOREACH(devs, l, dev)
     EINA_LIST_FOREACH(dev->outputs, ll, output)
       if (ecore_drm_output_crtc_id_get(output) == crtc_id)
         {
            eina_list_free(devs);
            return output;
         }
   eina_list_free(devs);
   return NULL;
}

static drmModePropertyPtr
_e_video_plane_get_zpos_property(uint plane_id, uint *zpos)
{
   drmModeObjectPropertiesPtr props = NULL;
   drmModePropertyPtr prop = NULL;
   int i;

   if (!plane_id)
      return NULL;

   props = drmModeObjectGetProperties(e_devmgr_drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
   if (props)
     {
        for (i = 0; i < props->count_props; i++)
          {
             prop = drmModeGetProperty(e_devmgr_drm_fd, props->props[i]);
             if (prop)
               {
                  if (!strcmp(prop->name, "zpos"))
                    {
                       *zpos = (uint)props->prop_values[i];
                       drmModeFreeObjectProperties(props);
                       return prop;
                    }
                  drmModeFreeProperty(prop);
               }
          }
        drmModeFreeObjectProperties(props);
     }

     return NULL;
}

static Eina_Bool
_e_video_plane_info_get(E_Video *video)
{
   drmModeResPtr mode_res = NULL;
   drmModePlaneResPtr plane_res = NULL;
   int zpos = -1;
   uint crtc_id = 0, plane_id = 0, immutable_zpos = 0;
   uint connector_type = 0;
   int i, j;

   EINA_SAFETY_ON_NULL_RETURN_VAL(video->ec, EINA_FALSE);

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
        uint plane_zpos = -1;
        drmModePlanePtr plane = drmModeGetPlane(e_devmgr_drm_fd, plane_res->planes[i]);
        drmModePropertyPtr prop;

        EINA_SAFETY_ON_NULL_GOTO(plane, failed);

        prop = _e_video_plane_get_zpos_property(plane->plane_id, &plane_zpos);
        if (prop)
          {
             immutable_zpos = ((prop->flags & DRM_MODE_PROP_IMMUTABLE) ? 1 : 0);
             zpos = plane_zpos;
             drmModeFreeProperty(prop);
          }
        if (plane->crtc_id == 0 && ((1 << video->pipe) & plane->possible_crtcs) && (!immutable_zpos || zpos == 1))
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

   if (!immutable_zpos)
     {
       /* TODO: get zpos */
       if (connector_type)
         zpos = 1;
       EINA_SAFETY_ON_FALSE_GOTO(zpos >= 0, failed);

       if (!_e_video_set_property(plane_id, DRM_MODE_OBJECT_PLANE, "zpos", zpos))
         goto failed;
     }

   video->drm_output = _e_video_plane_drm_output_find(crtc_id);
   video->plane_id = plane_id;
   video->zpos = zpos;

   VDB("crtc_id(%d) plane_id(%d) zpos(%d)", crtc_id, plane_id, zpos);

   drmModeFreeResources(mode_res);
   drmModeFreePlaneResources(plane_res);
   return EINA_TRUE;
failed:
   drmModeFreeResources(mode_res);
   if (plane_res)
      drmModeFreePlaneResources(plane_res);
   return EINA_FALSE;
}

static Eina_Bool
_e_video_geometry_cal_viewport(E_Video *video)
{
   E_Client *ec = video->ec;
   E_Comp_Wl_Buffer_Viewport *vp = &ec->comp_data->scaler.buffer_viewport;
   E_Comp_Wl_Subsurf_Data *sdata;
   int x1, y1, x2, y2;
   int tx1, ty1, tx2, ty2;
   E_Comp_Wl_Buffer *comp_buffer;
   tbm_surface_h tbm_surf;
   uint32_t size = 0, offset = 0, pitch = 0;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ec, EINA_FALSE);

   comp_buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN_VAL(comp_buffer, EINA_FALSE);

   tbm_surf = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, comp_buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN_VAL(tbm_surf, EINA_FALSE);

   tbm_surface_internal_get_plane_data(tbm_surf, 0, &size, &offset, &pitch);

   /* input geometry */
   if (IS_RGB(video->tbmfmt))
      video->geo.input_w = pitch / 4;
   else
      video->geo.input_w = pitch;

   video->geo.input_h = tbm_surface_get_height(tbm_surf);

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

   VDB("transform(%d) scale(%d) buffer(%dx%d) src(%d,%d %d,%d) viewport(%dx%d)",
       vp->buffer.transform, vp->buffer.scale,
       ec->comp_data->width_from_buffer, ec->comp_data->height_from_buffer,
       x1, y1, x2 - x1, y2 - y1,
       ec->comp_data->width_from_viewport, ec->comp_data->height_from_viewport);

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
        if (sdata->parent)
          {
             video->geo.output_r.x = sdata->parent->x + sdata->position.x;
             video->geo.output_r.y = sdata->parent->y + sdata->position.y;
          }
        else
          {
             video->geo.output_r.x = sdata->position.x;
             video->geo.output_r.y = sdata->position.y;
          }
     }
   else
     {
        video->geo.output_r.x = ec->x;
        video->geo.output_r.y = ec->y;
     }
   video->geo.output_r.w = ec->comp_data->width_from_viewport;
   video->geo.output_r.h = ec->comp_data->height_from_viewport;

   e_comp_object_frame_xy_unadjust(ec->frame,
                                   video->geo.output_r.x, video->geo.output_r.y,
                                   &video->geo.output_r.x, &video->geo.output_r.y);
   e_comp_object_frame_wh_unadjust(ec->frame,
                                   video->geo.output_r.w, video->geo.output_r.h,
                                   &video->geo.output_r.w, &video->geo.output_r.h);

   /* flip & rotate */
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

   VDB("geometry(%dx%d %d,%d,%d,%d %d,%d,%d,%d %d&%d %d)",
       video->geo.input_w, video->geo.input_h,
       video->geo.input_r.x, video->geo.input_r.y, video->geo.input_r.w, video->geo.input_r.h,
       video->geo.output_r.x, video->geo.output_r.y, video->geo.output_r.w, video->geo.output_r.h,
       video->geo.hflip, video->geo.vflip, video->geo.degree);

   return EINA_TRUE;
}

static void
_e_video_geometry_cal_map(E_Video *video)
{
   E_Client *ec = video->ec;
   const Evas_Map *m;
   Evas_Coord x1, x2, y1, y2;

   EINA_SAFETY_ON_NULL_RETURN(ec);
   EINA_SAFETY_ON_NULL_RETURN(video->ec->frame);

   m = evas_object_map_get(ec->frame);
   if (!m) return;

   /* If frame has map, it means that ec's geometry is decided by map's geometry.
    * ec->x,y,w,h and ec->client.x,y,w,h is not useful.
    */

   evas_map_point_coord_get(m, 0, &x1, &y1, NULL);
   evas_map_point_coord_get(m, 2, &x2, &y2, NULL);

   VDB("frame(%p) m(%p) output(%d,%d %dx%d) => (%d,%d %dx%d)",
       ec->frame, m, video->geo.output_r.x, video->geo.output_r.y,
       video->geo.output_r.w, video->geo.output_r.h,
       x1, y1, x2 - x1, y2 - y1);

   video->geo.output_r.x = x1;
   video->geo.output_r.y = y1;
   video->geo.output_r.w = x2 - x1;
   video->geo.output_r.h = y2 - y1;
}

static void
_e_video_format_info_get(E_Video *video)
{
   E_Comp_Wl_Buffer *comp_buffer;
   tbm_surface_h tbm_surf;

   comp_buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN(comp_buffer);

   tbm_surf = wayland_tbm_server_get_surface(e_comp->wl_comp_data->tbm.server, comp_buffer->resource);
   EINA_SAFETY_ON_NULL_RETURN(tbm_surf);

   video->tbmfmt = tbm_surface_get_format(tbm_surf);
}

static E_Video_Fb*
_e_video_frame_buffer_create(E_Video *video, E_Devmgr_Buf *mbuf, Eina_Rectangle *visible)
{
   E_Video_Fb *vfb = calloc(1, sizeof(E_Video_Fb));
   EINA_SAFETY_ON_NULL_RETURN_VAL(vfb, NULL);

   vfb->mbuf = e_devmgr_buffer_ref(mbuf);
   vfb->video = video;
   vfb->visible_r = *visible;

   if (mbuf->comp_buffer)
     e_comp_wl_buffer_reference(&vfb->buffer_ref, mbuf->comp_buffer);

   return vfb;
}

static void
_e_video_frame_buffer_destroy(E_Video_Fb *vfb)
{
   E_Video *video;

   if (!vfb) return;

   video = vfb->video;
   if (vfb->mbuf)
     {
        vfb->mbuf->showing = EINA_FALSE;
        VDB("mbuf(%d) hidden", MSTAMP(vfb->mbuf));
        e_devmgr_buffer_unref(vfb->mbuf);
     }
   e_comp_wl_buffer_reference(&vfb->buffer_ref, NULL);
   free (vfb);
}

static void
_e_video_frame_buffer_show(E_Video *video, E_Video_Fb *vfb)
{
   uint32_t fx, fy, fw, fh;
   int new_src_x, new_src_w;
   int new_dst_x, new_dst_w;
   int crtc_id;

   if (video->plane_id <= 0 || !video->drm_output)
      return;

   crtc_id = ecore_drm_output_crtc_id_get(video->drm_output);

   if (!vfb)
     {
        drmModeSetPlane (e_devmgr_drm_fd, video->plane_id, crtc_id,
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
     VDB("src_x changed: %d => %d", vfb->visible_r.x, new_src_x);
   if (vfb->visible_r.w != new_src_w)
     VDB("src_w changed: %d => %d", vfb->visible_r.w, new_src_w);
   if (video->geo.output_r.x != new_dst_x)
     VDB("dst_x changed: %d => %d", video->geo.output_r.x, new_dst_x);
   if (vfb->visible_r.w != new_dst_w)
     VDB("dst_w changed: %d => %d", vfb->visible_r.w, new_dst_w);

   /* Source values are 16.16 fixed point */
   fx = ((unsigned int)new_src_x) << 16;
   fy = ((unsigned int)vfb->visible_r.y) << 16;
   fw = ((unsigned int)new_src_w) << 16;
   fh = ((unsigned int)vfb->visible_r.h) << 16;

   if (drmModeSetPlane(e_devmgr_drm_fd, video->plane_id, crtc_id, vfb->mbuf->fb_id, 0,
                       new_dst_x, video->geo.output_r.y,
                       new_dst_w, vfb->visible_r.h,
                       fx, fy, fw, fh))
     {
         VER("failed: plane(%d) crtc(%d) pos(%d) on: fb(%d,%dx%d,[%d,%d,%d,%d]=>[%d,%d,%d,%d])",
             video->plane_id, crtc_id, video->zpos,
             vfb->mbuf->fb_id, vfb->mbuf->width, vfb->mbuf->height,
             new_src_x, vfb->visible_r.y, new_src_w, vfb->visible_r.h,
             new_dst_x, video->geo.output_r.y, new_dst_w, vfb->visible_r.h);
         return;
     }

   VDB("plane(%d) crtc(%d) pos(%d) on: fb(%d,%dx%d,[%d,%d,%d,%d]=>[%d,%d,%d,%d])",
       video->plane_id, crtc_id, video->zpos,
       vfb->mbuf->fb_id, vfb->mbuf->width, vfb->mbuf->height,
       new_src_x, vfb->visible_r.y, new_src_w, vfb->visible_r.h,
       new_dst_x, video->geo.output_r.y, new_dst_w, vfb->visible_r.h);
}

static void
_e_video_buffer_show(E_Video *video, E_Devmgr_Buf *mbuf, Eina_Rectangle *visible, Eina_Bool fake)
{
   Eina_List *l, *ll;
   E_Video_Fb *vfb;

   EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, vfb)
     {
        if (!vfb->fake) continue;

        video->waiting_list = eina_list_remove(video->waiting_list, vfb);
        _e_video_frame_buffer_destroy(vfb);
     }

   vfb = _e_video_frame_buffer_create(video, mbuf, visible);
   EINA_SAFETY_ON_NULL_RETURN(vfb);
   vfb->fake = fake;

   video->waiting_list = eina_list_append(video->waiting_list, vfb);

   vfb->mbuf->showing = EINA_TRUE;
   VDB("mbuf(%d) waiting", MSTAMP(vfb->mbuf));

   /* There are waiting fbs more than 2 */
   if (eina_list_nth(video->waiting_list, 1))
     return;

   _e_video_frame_buffer_show(video, vfb);
   _e_video_wait_vblank(video);
}

static void
_e_video_cvt_callback(E_Devmgr_Cvt *cvt,
                      E_Devmgr_Buf *input_buffer,
                      E_Devmgr_Buf *cvt_buffer,
                      void *cvt_data)
{
   E_Video *video = (E_Video*)cvt_data;
   Eina_Bool fake = EINA_FALSE;

   if (eina_list_data_find(video->fake_buffer_list, input_buffer))
     {
        e_devmgr_buffer_unref(input_buffer);
        fake = EINA_TRUE;
     }

   _e_video_buffer_show(video, cvt_buffer, &video->cvt_r, fake);

   e_devmgr_buffer_unref(input_buffer);
#if 0
   static int i;
   e_devmgr_buffer_dump(cvt_buffer, "out", i++, 0);
#endif
}

static Eina_Bool
_e_video_cvt_configure(E_Video *video, E_Devmgr_Buf *cvt_buffer)
{
   E_Devmgr_Cvt_Prop src, dst;

   if (!e_devmgr_cvt_pause(video->cvt))
     return EINA_FALSE;

   CLEAR(src);
   src.tbmfmt = video->tbmfmt;
   src.width = video->geo.input_w;
   src.height = video->geo.input_h;
   src.crop = video->geo.input_r;

   CLEAR(dst);
   dst.tbmfmt = TBM_FORMAT_XRGB8888;
   dst.width = cvt_buffer->pitches[0] >> 2;
   dst.height = cvt_buffer->height;
   dst.crop.w = video->geo.output_r.w;
   dst.crop.h = video->geo.output_r.h;

   dst.hflip = video->geo.hflip;
   dst.vflip = video->geo.vflip;
   dst.degree = video->geo.degree;

   if (!e_devmgr_cvt_property_set(video->cvt, &src, &dst))
     return EINA_FALSE;

   video->cvt_r = dst.crop;

   return EINA_TRUE;
}

static void
_e_video_vblank_handler(void *data)
{
   E_Video *video = data;
   E_Video_Fb *vfb;

   video->wait_vblank = EINA_FALSE;

   if (video->need_render)
     _e_video_render(video, EINA_TRUE);

   vfb = eina_list_nth(video->waiting_list, 0);
   if (!vfb) return;

   video->waiting_list = eina_list_remove(video->waiting_list, vfb);

   if (video->current_fb)
     {
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
     }

   video->current_fb = vfb;

   VDB("mbuf(%d) showing", MSTAMP(vfb->mbuf));

   if ((vfb = eina_list_nth(video->waiting_list, 0)))
     {
        _e_video_frame_buffer_show(video, vfb);
        _e_video_wait_vblank(video);
     }
}

static void
_e_video_wait_vblank(E_Video *video)
{
    /* If not DPMS_ON, we call vblank handler directory to do post-process
     * for video frame buffer handling.
     */
   if (e_devicemgr_dpms_get(video->drm_output))
     {
        _e_video_vblank_handler((void*)video);
        return;
     }

   if (video->wait_vblank) return;
   video->wait_vblank = EINA_TRUE;

   if (!ecore_drm_output_wait_vblank(video->drm_output, 1, _e_video_vblank_handler, video))
     {
         VER("failed: ecore_drm_output_wait_vblank");
         return;
     }
}

static void
_e_video_evas_cb_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   E_Video *video;
   E_Client *ec;

   if (!(video = data)) return;
   if (!(ec = video->ec)) return;
   if (e_object_is_del(E_OBJECT(ec))) return;

   video->need_render = EINA_TRUE;

   _e_video_wait_vblank(video);
}

static E_Video*
_e_video_create(struct wl_resource *video_object, struct wl_resource *surface)
{
   E_Video *video = NULL;
   E_Pixmap *ep;
   E_Comp_Wl_Client_Data *cdata;
   struct wl_client *client;

   ep = wl_resource_get_user_data(surface);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ep, NULL);

   cdata = e_pixmap_cdata_get(ep);
   EINA_SAFETY_ON_NULL_RETURN_VAL(cdata, NULL);

   client = wl_resource_get_client(cdata->wl_surface);
   EINA_SAFETY_ON_NULL_RETURN_VAL(client, NULL);

   video = calloc(1, sizeof *video);
   EINA_SAFETY_ON_NULL_RETURN_VAL(video, NULL);

   video->video_object = video_object;
   video->surface = surface;
   cdata->video_client = EINA_TRUE;

   video_list = eina_list_append(video_list, video);

   return video;
}

static Eina_Bool
_e_video_prepare(E_Video *video, E_Client *ec)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(ec, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ec->frame, EINA_FALSE);

   video->ec = ec;
   video->window = e_client_util_win_get(ec);

   ec->comp_data->video_client = EINA_TRUE;

   _e_video_format_info_get(video);

   if (!_e_video_plane_info_get(video))
     {
        VER("failed to find crtc & plane");
        return EINA_FALSE;
     }

   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_MOVE, _e_video_evas_cb_move, video);

   return EINA_TRUE;
}

static void
_e_video_destroy(E_Video *video)
{
   E_Devmgr_Buf *mbuf;
   E_Video_Fb *vfb;
   Eina_List *l = NULL, *ll = NULL;

   if (!video)
      return;

   wl_resource_set_destructor(video->video_object, NULL);

   if (video->ec && video->ec->frame)
     evas_object_event_callback_del(video->ec->frame, EVAS_CALLBACK_MOVE, _e_video_evas_cb_move);

   /* hide video plane first */
   _e_video_frame_buffer_show(video, NULL);

   if (video->current_fb)
     {
        _e_video_frame_buffer_destroy(video->current_fb);
        video->current_fb = NULL;
     }

   EINA_LIST_FOREACH_SAFE(video->waiting_list, l, ll, vfb)
     _e_video_frame_buffer_destroy(vfb);

   /* destroy converter second */
   if (video->cvt)
     e_devmgr_cvt_destroy(video->cvt);

   /* others */
   EINA_LIST_FOREACH_SAFE(video->input_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   EINA_LIST_FOREACH_SAFE(video->cvt_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   EINA_LIST_FOREACH_SAFE(video->fake_buffer_list, l, ll, mbuf)
     e_devmgr_buffer_unref(mbuf);

   video_list = eina_list_remove(video_list, video);

   VIN("stop");

   free(video);

#if 0
   if (e_devmgr_buffer_list_length() > 0)
     e_devmgr_buffer_list_print();
#endif
}

static void
_e_video_render(E_Video *video, Eina_Bool fake)
{
   E_Comp_Wl_Buffer *comp_buffer;
   E_Devmgr_Buf *cvt_buffer = NULL;
   E_Devmgr_Buf *input_buffer = NULL;

   EINA_SAFETY_ON_NULL_RETURN(video->ec);

   video->need_render = EINA_FALSE;

   /* get geometry information with buffer scale, transform and viewport. */
   if (!_e_video_geometry_cal_viewport(video))
     return;

   _e_video_geometry_cal_map(video);

   comp_buffer = e_pixmap_resource_get(video->ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN(comp_buffer);

   if (fake)
     {
        E_Devmgr_Buf *last = eina_list_last_data_get(video->input_buffer_list);

        /* If a fake buffer is not the same with last buffer, we skip rendering.
         * The fake buffer which surface has currnetly will be rendered by
         * _e_video_cb_ec_buffer_change.
         */
        if (!last || (last && last->comp_buffer != comp_buffer))
          return;
     }
   else
     _e_video_input_buffer_valid(video, comp_buffer);

   /* 2. In case a buffer is RGB and the size of input/output *WHICH CLIENT SENT*
    * is same, we don't need to convert video. Otherwise, we should need a converter.
    */
   if (!IS_RGB(video->tbmfmt) ||
       video->geo.input_r.w != video->geo.output_r.w ||
       video->geo.input_r.h != video->geo.output_r.h ||
       video->geo.hflip || video->geo.vflip || video->geo.degree)
     if (!video->cvt)
       {
          video->cvt = e_devmgr_cvt_create();
          EINA_SAFETY_ON_NULL_GOTO(video->cvt, render_fail);

//          e_devmgr_cvt_debug(video->cvt, EINA_TRUE);

          e_devmgr_cvt_cb_add(video->cvt, _e_video_cvt_callback, video);
       }

   if (video->cvt)
     {
        input_buffer = _e_video_input_buffer_get(video, comp_buffer, EINA_FALSE, fake);
        EINA_SAFETY_ON_NULL_GOTO(input_buffer, render_fail);

        cvt_buffer = _e_video_cvt_buffer_get(video, video->geo.output_r.w, video->geo.output_r.h);
        EINA_SAFETY_ON_NULL_GOTO(cvt_buffer, render_fail);

        /* configure a converter if needed */
        if (video->old_geo.input_w != video->geo.input_w || video->old_geo.input_h != video->geo.input_h ||
            video->old_geo.input_r.x != video->geo.input_r.x || video->old_geo.input_r.y != video->geo.input_r.y ||
            video->old_geo.input_r.w != video->geo.input_r.w || video->old_geo.input_r.h != video->geo.input_r.h ||
            video->old_geo.output_r.w != video->geo.output_r.w || video->old_geo.output_r.h != video->geo.output_r.h ||
            video->old_geo.hflip != video->geo.hflip || video->old_geo.vflip != video->geo.vflip ||
            video->old_geo.degree != video->geo.degree)
          {
             VDB("geometry(%dx%d %d,%d,%d,%d %d,%d,%d,%d %d&%d %d)",
                 video->geo.input_w, video->geo.input_h,
                 video->geo.input_r.x, video->geo.input_r.y, video->geo.input_r.w, video->geo.input_r.h,
                 video->geo.output_r.x, video->geo.output_r.y, video->geo.output_r.w, video->geo.output_r.h,
                 video->geo.hflip, video->geo.vflip, video->geo.degree);

             if (!_e_video_cvt_configure(video, cvt_buffer))
               goto render_fail;
          }
#if 0
        static int i;
        e_devmgr_buffer_dump(input_buffer, "in", i++, 0);
#endif

        if (!e_devmgr_cvt_convert(video->cvt, input_buffer, cvt_buffer))
          goto render_fail;

        if (fake)
          {
             video->fake_buffer_list = eina_list_append(video->fake_buffer_list, input_buffer);
             e_devmgr_buffer_ref(input_buffer);
          }
     }
   else
     {
        input_buffer = _e_video_input_buffer_get(video, comp_buffer, EINA_TRUE, fake);
        EINA_SAFETY_ON_NULL_GOTO(input_buffer, render_fail);

        _e_video_buffer_show(video, input_buffer, &video->geo.input_r, fake);

#if 0
         static int i;
         e_devmgr_buffer_dump(input_buffer, "render", i++, 0);
#endif
     }

   video->old_geo = video->geo;

   return;

render_fail:
   if (input_buffer)
     e_devmgr_buffer_unref(input_buffer);

   if (cvt_buffer)
     e_devmgr_buffer_unref(cvt_buffer);
}

static Eina_Bool
_e_video_cb_ec_buffer_change(void *data, int type, void *event)
{
   E_Client *ec;
   E_Event_Client *ev = event;
   E_Comp_Wl_Buffer *comp_buffer;
   E_Video *video;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;

   video = find_video_with_surface(ec->comp_data->surface);
   if (!video) return ECORE_CALLBACK_PASS_ON;

   if (!ec->pixmap) return ECORE_CALLBACK_PASS_ON;

   comp_buffer = e_pixmap_resource_get(ec->pixmap);

   /* buffer can be NULL when camera/video's mode changed. Do nothing and
    * keep previous frame in this case.
    */
   if (!comp_buffer) return ECORE_CALLBACK_PASS_ON;

   /* not interested in other buffer type */
   if (!wayland_tbm_server_get_surface(NULL, comp_buffer->resource))
      return ECORE_CALLBACK_PASS_ON;

   DBG("======================================");
   if (!video->ec && !_e_video_prepare(video, ec))
      return ECORE_CALLBACK_PASS_ON;

   _e_video_render(video, EINA_FALSE);
   DBG("======================================.");

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_video_cb_ec_remove(void *data, int type, void *event)
{
   E_Event_Client *ev = event;
   E_Client *ec;
   E_Video *video;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev->ec, ECORE_CALLBACK_PASS_ON);

   ec = ev->ec;
   if (!ec->comp_data)
      return ECORE_CALLBACK_PASS_ON;

   video = find_video_with_surface(ec->comp_data->surface);

   _e_video_destroy(video);

   return ECORE_CALLBACK_PASS_ON;
}

static void
_e_devicemgr_video_object_destroy(struct wl_resource *resource)
{
   E_Video *video = wl_resource_get_user_data(resource);

   _e_video_destroy(video);
}

static void
_e_devicemgr_video_object_cb_destroy(struct wl_client *client, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void
_e_devicemgr_video_object_cb_set_attribute(struct wl_client *client,
                                           struct wl_resource *resource,
                                           const char *name,
                                           int32_t value)
{
}

static const struct tizen_video_object_interface _e_devicemgr_video_object_interface =
{
   _e_devicemgr_video_object_cb_destroy,
   _e_devicemgr_video_object_cb_set_attribute,
};

static void
_e_devicemgr_video_cb_get_object(struct wl_client *client,
                                 struct wl_resource *resource,
                                 uint32_t id,
                                 struct wl_resource *surface)
{
   int version = wl_resource_get_version(resource);
   E_Video *video = wl_resource_get_user_data(resource);
   struct wl_resource *res;

   if (video)
     {
        wl_resource_post_error(resource,
                               TIZEN_VIDEO_ERROR_OBJECT_EXISTS,
                               "a video object for that surface already exists");
        return;
     }

   res = wl_resource_create(client, &tizen_video_object_interface, version, id);
   if (res == NULL)
     {
        wl_client_post_no_memory(client);
        return;
     }

   if (!(video = _e_video_create(res, surface)))
     {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_devicemgr_video_object_interface,
                                  video, _e_devicemgr_video_object_destroy);
}

static const struct tizen_video_interface _e_devicemgr_video_interface =
{
   _e_devicemgr_video_cb_get_object
};

static void
_e_devicemgr_video_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res;
   int i;

   if (!(res = wl_resource_create(client, &tizen_video_interface, MIN(version, 1), id)))
     {
        ERR("Could not create tizen_video_interface resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_devicemgr_video_interface, NULL, NULL);

   for (i = 0; i < NUM_VIDEO_FORMAT; i++)
     tizen_video_send_format(res, video_format_table[i]);
}

static Eina_List *video_hdlrs;

int
e_devicemgr_video_init(void)
{
   E_Comp_Data *cdata;

   if (!e_comp) return 0;
   if (!(cdata = e_comp->wl_comp_data)) return 0;
   if (!cdata->wl.disp) return 0;

   /* try to add tizen_screenshooter to wayland globals */
   if (!wl_global_create(cdata->wl.disp, &tizen_video_interface, 1,
                         NULL, _e_devicemgr_video_cb_bind))
     {
        ERR("Could not add tizen_screenshooter to wayland globals");
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

Eina_List*
e_devicemgr_video_list_get(void)
{
   return video_list;
}

E_Video*
e_devicemgr_video_get(struct wl_resource *surface_resource)
{
   return find_video_with_surface(surface_resource);

}

E_Devmgr_Buf*
e_devicemgr_video_fb_get(E_Video *video)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(video, NULL);

   return (video->current_fb) ? video->current_fb->mbuf : NULL;
}

void
e_devicemgr_video_pos_get(E_Video *video, int *x, int *y)
{
   EINA_SAFETY_ON_NULL_RETURN(video);

   if (x) *x = video->geo.output_r.x;
   if (y) *y = video->geo.output_r.y;
}

Ecore_Drm_Output*
e_devicemgr_video_drm_output_get(E_Video *video)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(video, NULL);

   return video->drm_output;
}
