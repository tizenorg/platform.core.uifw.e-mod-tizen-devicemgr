#include <sys/ioctl.h>
#define E_COMP_WL
#include <e.h>
#include <e_comp_wl.h>
#include <drm_fourcc.h>
#include <Ecore_Drm.h>
#include <exynos_drm.h>
#include <e_drm_buffer_pool.h>

#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_drm.h"
#include "e_devicemgr_converter.h"

//#define INCREASE_NUM 1
#define DEQUEUE_FORCE 1

#if INCREASE_NUM
#define CVT_BUF_MAX    6
#endif

typedef enum
{
   CVT_TYPE_SRC,
   CVT_TYPE_DST,
   CVT_TYPE_MAX,
} E_Devmgr_CvtType;

typedef struct _ConvertInfo
{
   void *cvt;
   struct wl_list link;
} ConvertInfo;

typedef struct _E_Devmgr_CvtFuncData
{
   CvtFunc  func;
   void    *data;
   struct wl_list   link;
} E_Devmgr_CvtFuncData;

typedef struct _E_Devmgr_CvtBuf
{
   E_Devmgr_CvtType type;
   int   index;
   uint  handles[4];
   uint  begin;

   E_Devmgr_Buf  *mbuf;

   E_Devmgr_Cvt *cvt;
   E_Comp_Wl_Buffer_Ref buffer_ref;
   struct wl_listener buffer_destroy_listener;

   struct wl_list link;
} E_Devmgr_CvtBuf;

struct _E_Devmgr_Cvt
{
   uint stamp;

   int prop_id;

   E_Devmgr_Cvt_Prop props[CVT_TYPE_MAX];

   struct wl_list func_datas;
   struct wl_list src_bufs;
   struct wl_list dst_bufs;

#if INCREASE_NUM
   int src_index;
   int dst_index;
#endif

   Eina_Bool started;
   Eina_Bool first_event;

   struct wl_list   link;
};

static struct wl_list cvt_list;

static void
init_list(void)
{
   static Eina_Bool inited = EINA_FALSE;

   if (inited)
     return;

   wl_list_init(&cvt_list);

   inited = EINA_TRUE;
}

static E_Devmgr_Cvt*
find_cvt(uint stamp)
{
   E_Devmgr_Cvt *cur = NULL, *next = NULL;

   init_list();

   if (cvt_list.next != NULL)
     {
        wl_list_for_each_safe(cur, next, &cvt_list, link)
         {
            if (cur->stamp == stamp)
              return cur;
         }
     }

   return NULL;
}

static enum drm_exynos_degree
drm_degree(int degree)
{
   switch (degree % 360)
   {
   case 90:
     return EXYNOS_DRM_DEGREE_90;
   case 180:
     return EXYNOS_DRM_DEGREE_180;
   case 270:
     return EXYNOS_DRM_DEGREE_270;
   default:
     return EXYNOS_DRM_DEGREE_0;
   }
}

static void
fill_config(E_Devmgr_CvtType type, E_Devmgr_Cvt_Prop *prop, struct drm_exynos_ipp_config *config)
{
   config->ops_id = (type == CVT_TYPE_SRC) ? EXYNOS_DRM_OPS_SRC : EXYNOS_DRM_OPS_DST;

   if (prop->hflip)
     config->flip |= EXYNOS_DRM_FLIP_HORIZONTAL;
   if (prop->vflip)
     config->flip |= EXYNOS_DRM_FLIP_VERTICAL;

   config->degree = drm_degree(prop->degree);
   config->fmt = prop->drmfmt;
   config->sz.hsize = (__u32)prop->width;
   config->sz.vsize = (__u32)prop->height;
   config->pos.x = (__u32)prop->crop.x;
   config->pos.y = (__u32)prop->crop.y;
   config->pos.w = (__u32)prop->crop.w;
   config->pos.h = (__u32)prop->crop.h;
}

static Eina_Bool
set_mbuf_converting(E_Devmgr_Buf *mbuf, E_Devmgr_Cvt *cvt, Eina_Bool converting)
{
   if (!converting)
     {
        ConvertInfo *cur = NULL, *next = NULL;
        wl_list_for_each_safe(cur, next, &mbuf->convert_info, link)
          {
             if (cur->cvt == (void*)cvt)
               {
                  wl_list_remove(&cur->link);
                  free(cur);
                  return EINA_TRUE;
               }
          }
        return EINA_TRUE;
     }
   else
     {
       ConvertInfo *info = NULL, *next = NULL;
       wl_list_for_each_safe(info, next, &mbuf->convert_info, link)
         {
            if (info->cvt == (void*)cvt)
              {
                 ERR("failed: %d already converting %d.", cvt->stamp, mbuf->stamp);
                 return EINA_FALSE;
              }
         }
       info = calloc(1, sizeof(ConvertInfo));
       EINA_SAFETY_ON_NULL_RETURN_VAL(info, EINA_FALSE);
       info->cvt = (void*)cvt;
       wl_list_insert(&mbuf->convert_info, &info->link);
       return EINA_TRUE;
     }
}

#if 0
static void
_printBufIndices(E_Devmgr_Cvt *cvt, E_Devmgr_CvtType type, char *str)
{
   struct wl_list *bufs;
   E_Devmgr_CvtBuf *cur, *next;
   char nums[128];

   bufs = (type == CVT_TYPE_SRC) ? &cvt->src_bufs : &cvt->dst_bufs;

   snprintf(nums, 128, "bufs:");

   wl_list_for_each_reverse_safe(cur, next, bufs, link)
     {
       snprintf(nums, 128, "%s %d", nums, cur->index);
     }

   ErrorF("%s: cvt(%p) %s(%s). ", str, cvt,
          (type == CVT_TYPE_SRC)?"SRC":"DST", nums);
}
#endif

static void
_e_devmgr_cvt_cb_tb_destroy(struct wl_listener *listener, void *data)
{
   NEVER_GET_HERE();
}

static int
_e_devmgr_cvt_get_empty_index(E_Devmgr_Cvt *cvt, E_Devmgr_CvtType type)
{
#if INCREASE_NUM
   int ret;

   if (type == CVT_TYPE_SRC)
     {
        ret = cvt->src_index++;
        if (cvt->src_index >= CVT_BUF_MAX)
          cvt->src_index = 0;
     }
   else
     {
        ret = cvt->dst_index++;
        if (cvt->dst_index >= CVT_BUF_MAX)
          cvt->dst_index = 0;
     }

   return ret;
#else
   struct wl_list *bufs;
   E_Devmgr_CvtBuf *cur = NULL, *next = NULL;
   int ret = 0;

   bufs = (type == CVT_TYPE_SRC) ? &cvt->src_bufs : &cvt->dst_bufs;

   while (1)
     {
        Eina_Bool found = EINA_FALSE;
        wl_list_for_each_safe(cur, next, bufs, link)
          {
             if (ret == cur->index)
               {
                  found = EINA_TRUE;
                  break;
               }
          }
        if (!found)
          break;
        ret++;
     }

   return ret;
#endif
}

static E_Devmgr_CvtBuf*
_e_devmgr_cvt_find_buf(E_Devmgr_Cvt *cvt, E_Devmgr_CvtType type, int index)
{
   struct wl_list *bufs;
   E_Devmgr_CvtBuf *cur = NULL, *next = NULL;

   bufs = (type == CVT_TYPE_SRC) ? &cvt->src_bufs : &cvt->dst_bufs;

   wl_list_for_each_safe(cur, next, bufs, link)
     {
        if (index == cur->index)
          return cur;
     }

   ERR("cvt(%p), type(%d), index(%d) not found.", cvt, type, index);

   return NULL;
}

static Eina_Bool
_e_devmgr_cvt_queue(E_Devmgr_Cvt *cvt, E_Devmgr_CvtBuf *cbuf)
{
   struct drm_exynos_ipp_queue_buf buf = {0,};
   struct wl_list *bufs;
   int i;
   int index;

   if (!set_mbuf_converting(cbuf->mbuf, cvt, EINA_TRUE))
     return EINA_FALSE;

   index = _e_devmgr_cvt_get_empty_index(cvt, cbuf->type);

   buf.prop_id = cvt->prop_id;
   buf.ops_id = (cbuf->type == CVT_TYPE_SRC) ? EXYNOS_DRM_OPS_SRC : EXYNOS_DRM_OPS_DST;
   buf.buf_type = IPP_BUF_ENQUEUE;
   buf.buf_id = cbuf->index = index;
   buf.user_data = (__u64)(uintptr_t)cvt;

   for (i = 0; i < EXYNOS_DRM_PLANAR_MAX; i++)
     buf.handle[i] = (__u32)cbuf->handles[i];

   if (!e_devicemgr_drm_ipp_queue(&buf))
     {
        set_mbuf_converting(cbuf->mbuf, cvt, EINA_FALSE);
        return EINA_FALSE;
     }

   bufs = (cbuf->type == CVT_TYPE_SRC) ? &cvt->src_bufs : &cvt->dst_bufs;
   wl_list_insert(bufs, &cbuf->link);

   if (cbuf->mbuf->type == TYPE_TB && cbuf->mbuf->b.tizen_buffer->buffer)
     {
        E_Comp_Wl_Buffer *buffer = cbuf->mbuf->b.tizen_buffer->buffer;
        e_comp_wl_buffer_reference(&cbuf->buffer_ref, buffer);

        cbuf->cvt = cvt;
        cbuf->buffer_destroy_listener.notify = _e_devmgr_cvt_cb_tb_destroy;
        wl_signal_add(&buffer->destroy_signal, &cbuf->buffer_destroy_listener);
     }

#if 0
   if (cbuf->type == CVT_TYPE_SRC)
     _printBufIndices(cvt, CVT_TYPE_SRC, "in");
#endif

   DBG("cvt(%p), cbuf(%p), type(%d), index(%d) mbuf(%p) converting(%d)",
       cvt, cbuf, cbuf->type, index, cbuf->mbuf, MBUF_IS_CONVERTING(cbuf->mbuf));

   return EINA_TRUE;
}

static void
_e_devmgr_cvt_dequeue(E_Devmgr_Cvt *cvt, E_Devmgr_CvtBuf *cbuf)
{
   struct drm_exynos_ipp_queue_buf buf = {0,};
   int i;

   if (!_e_devmgr_cvt_find_buf(cvt, cbuf->type, cbuf->index))
     {
        ERR("cvt(%p) type(%d), index(%d) already dequeued!", cvt, cbuf->type, cbuf->index);
        return;
     }

   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(cbuf->mbuf));

   buf.prop_id = cvt->prop_id;
   buf.ops_id = (cbuf->type == CVT_TYPE_SRC) ? EXYNOS_DRM_OPS_SRC : EXYNOS_DRM_OPS_DST;
   buf.buf_type = IPP_BUF_DEQUEUE;
   buf.buf_id = cbuf->index;
   buf.user_data = (__u64)(uintptr_t)cvt;

   for (i = 0; i < EXYNOS_DRM_PLANAR_MAX; i++)
     buf.handle[i] = (__u32)cbuf->handles[i];

   if (!e_devicemgr_drm_ipp_queue(&buf))
     return;
}

static void
_e_devmgr_cvt_dequeued(E_Devmgr_Cvt *cvt, E_Devmgr_CvtType type, int index)
{
   E_Devmgr_CvtBuf *cbuf = _e_devmgr_cvt_find_buf(cvt, type, index);

   if (!cbuf)
     {
        ERR("cvt(%p) type(%d), index(%d) already dequeued!", cvt, type, index);
        return;
     }

   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(cbuf->mbuf));

   set_mbuf_converting(cbuf->mbuf, cvt, EINA_FALSE);

   DBG("cvt(%p) type(%d) index(%d) mbuf(%p) converting(%d)",
       cvt, type, index, cbuf->mbuf, MBUF_IS_CONVERTING(cbuf->mbuf));

   wl_list_remove(&cbuf->link);

   if (cbuf->mbuf->type == TYPE_TB)
     {
        if (cbuf->buffer_destroy_listener.notify)
          {
             wl_list_remove(&cbuf->buffer_destroy_listener.link);
             cbuf->buffer_destroy_listener.notify = NULL;
          }
        e_comp_wl_buffer_reference(&cbuf->buffer_ref, NULL);
     }

#if 0
   if (cbuf->type == CVT_TYPE_SRC)
     _printBufIndices(cvt, CVT_TYPE_SRC, "out");
#endif

   e_devmgr_buffer_unref(cbuf->mbuf);
   free(cbuf);
}

static void
_e_devmgr_cvt_dequeue_all(E_Devmgr_Cvt *cvt)
{
   E_Devmgr_CvtBuf *cur = NULL, *next = NULL;

   wl_list_for_each_safe(cur, next, &cvt->src_bufs, link)
     _e_devmgr_cvt_dequeue(cvt, cur);
   wl_list_for_each_safe(cur, next, &cvt->dst_bufs, link)
     _e_devmgr_cvt_dequeue(cvt, cur);
}

static void
_e_devmgr_cvt_dequeued_all(E_Devmgr_Cvt *cvt)
{
   E_Devmgr_CvtBuf *cur = NULL, *next = NULL;

   wl_list_for_each_safe(cur, next, &cvt->src_bufs, link)
     _e_devmgr_cvt_dequeued(cvt, EXYNOS_DRM_OPS_SRC, cur->index);
   wl_list_for_each_safe(cur, next, &cvt->dst_bufs, link)
     _e_devmgr_cvt_dequeued(cvt, EXYNOS_DRM_OPS_DST, cur->index);
}

static void
_e_devmgr_cvt_stop(E_Devmgr_Cvt *cvt)
{
   struct drm_exynos_ipp_cmd_ctrl ctrl = {0,};

   EINA_SAFETY_ON_NULL_RETURN(cvt);

   if (!cvt->started)
     return;

   _e_devmgr_cvt_dequeue_all(cvt);

   ctrl.prop_id = cvt->prop_id;
   ctrl.ctrl = IPP_CTRL_STOP;

   e_devicemgr_drm_ipp_cmd(&ctrl);

   _e_devmgr_cvt_dequeued_all(cvt);

   DBG("cvt(%p)", cvt);

   cvt->prop_id = -1;

   memset(cvt->props, 0, sizeof(E_Devmgr_Cvt_Prop) * CVT_TYPE_MAX);

#if INCREASE_NUM
   cvt->src_index = 0;
   cvt->dst_index = 0;
#endif
   cvt->started = EINA_FALSE;

   return;
}

static void
_e_devmgr_cvt_ipp_handler(unsigned int prop_id, unsigned int *buf_idx,
                          unsigned int tv_sec, unsigned int tv_usec, void *data)
{
   E_Devmgr_Cvt *cvt = (E_Devmgr_Cvt *)data;
   E_Devmgr_CvtBuf *src_cbuf, *dst_cbuf;
   E_Devmgr_Buf *src_vbuf, *dst_vbuf;
   E_Devmgr_CvtFuncData *curr = NULL, *next = NULL;

   EINA_SAFETY_ON_NULL_RETURN(buf_idx);

   cvt = find_cvt(cvt->stamp);
   if (!cvt)
     {
        ERR("invalid cvt's stamp(%d).", cvt->stamp);
        return;
     }

   DBG("cvt(%p) index(%d, %d)", cvt, buf_idx[EXYNOS_DRM_OPS_SRC], buf_idx[EXYNOS_DRM_OPS_DST]);

#if 0
   char temp[64];
   snprintf(temp, 64, "%d,%d", buf_idx[EXYNOS_DRM_OPS_SRC], buf_idx[EXYNOS_DRM_OPS_DST]);
   _printBufIndices(cvt, CVT_TYPE_SRC, temp);
#endif

#if DEQUEUE_FORCE
   E_Devmgr_CvtBuf *cur = NULL, *prev = NULL;

   wl_list_for_each_reverse_safe(cur, prev, &cvt->src_bufs, link)
     {
        if (buf_idx[EXYNOS_DRM_OPS_SRC] != cur->index)
          ERR("cvt(%p) event(%d,%d) has been skipped!! ", cvt, cur->index, cur->index);
        else
            break;
     }
#endif

   src_cbuf = _e_devmgr_cvt_find_buf(cvt, EXYNOS_DRM_OPS_SRC, buf_idx[EXYNOS_DRM_OPS_SRC]);
   EINA_SAFETY_ON_NULL_RETURN(src_cbuf);

   dst_cbuf = _e_devmgr_cvt_find_buf(cvt, EXYNOS_DRM_OPS_DST, buf_idx[EXYNOS_DRM_OPS_DST]);
   EINA_SAFETY_ON_NULL_RETURN(dst_cbuf);

   uint curm, sub;
   curm = e_devmgr_buffer_get_mills();
   sub = curm - src_cbuf->begin;
   DBG("cvt(%p)   ipp interval  : %6d ms", cvt, sub);

   src_vbuf = src_cbuf->mbuf;
   dst_vbuf = dst_cbuf->mbuf;

   DBG("<== ipp(%d,%d,%d : %d,%d,%d) ",
       src_vbuf->stamp, MBUF_IS_CONVERTING(src_vbuf), src_vbuf->showing,
       dst_vbuf->stamp, MBUF_IS_CONVERTING(dst_vbuf), dst_vbuf->showing);

   if (!cvt->first_event)
     {
        DBG("cvt(%p) got a IPP event. ", cvt);
        cvt->first_event = EINA_TRUE;
     }

   wl_list_for_each_safe(curr, next, &cvt->func_datas, link)
     {
        if (curr->func)
          curr->func(cvt, src_vbuf, dst_vbuf, curr->data);
     }

   _e_devmgr_cvt_dequeued(cvt, EXYNOS_DRM_OPS_SRC, buf_idx[EXYNOS_DRM_OPS_SRC]);
   _e_devmgr_cvt_dequeued(cvt, EXYNOS_DRM_OPS_DST, buf_idx[EXYNOS_DRM_OPS_DST]);
}

Eina_Bool
e_devmgr_cvt_ensure_size(E_Devmgr_Cvt_Prop *src, E_Devmgr_Cvt_Prop *dst)
{
   if (src)
     {
        int type = e_devmgr_buffer_color_type(src->drmfmt);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(src->width >= 16, EINA_FALSE);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(src->height >= 8, EINA_FALSE);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(src->crop.w >= 16, EINA_FALSE);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(src->crop.h >= 8, EINA_FALSE);
        if (type == TYPE_YUV420 && src->height % 2)
          ERR("src's height(%d) is not multiple of 2!!!", src->height);
        if (type == TYPE_YUV420 || type == TYPE_YUV422)
          {
             src->crop.x = src->crop.x & (~0x1);
             src->crop.w = src->crop.w & (~0x1);
          }
        if (type == TYPE_YUV420)
          src->crop.h = src->crop.h & (~0x1);
        if (src->crop.x + src->crop.w > src->width)
          src->crop.w = src->width - src->crop.x;
        if (src->crop.y + src->crop.h > src->height)
          src->crop.h = src->height - src->crop.y;
     }

   if (dst)
     {
        int type = e_devmgr_buffer_color_type(dst->drmfmt);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(dst->width >= 16, EINA_FALSE);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(dst->height >= 8, EINA_FALSE);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(dst->crop.w >= 16, EINA_FALSE);
        EINA_SAFETY_ON_FALSE_RETURN_VAL(dst->crop.h >= 4, EINA_FALSE);
        if (dst->width % 16)
          {
             int new_width = (dst->width + 16) & (~0xF);
             DBG("dst's width : %d to %d.", dst->width, new_width);
             dst->width = new_width;
          }
        dst->height = dst->height & (~0x1);
        if (type == TYPE_YUV420 && dst->height % 2)
          ERR("dst's height(%d) is not multiple of 2!!!", dst->height);
        if (type == TYPE_YUV420 || type == TYPE_YUV422)
          {
             dst->crop.x = dst->crop.x & (~0x1);
             dst->crop.w = dst->crop.w & (~0x1);
          }
        if (type == TYPE_YUV420)
          dst->crop.h = dst->crop.h & (~0x1);
        if (dst->crop.x + dst->crop.w > dst->width)
          dst->crop.w = dst->width - dst->crop.x;
        if (dst->crop.y + dst->crop.h > dst->height)
          dst->crop.h = dst->height - dst->crop.y;
     }

   return EINA_TRUE;
}

E_Devmgr_Cvt*
e_devmgr_cvt_create(void)
{
   E_Devmgr_Cvt *cvt;
   uint stamp = e_devmgr_buffer_get_mills();

   init_list();

   while(find_cvt(stamp))
     stamp++;

   cvt = calloc(1, sizeof(E_Devmgr_Cvt));
   EINA_SAFETY_ON_NULL_RETURN_VAL(cvt, NULL);

   cvt->stamp = stamp;

   cvt->prop_id = -1;

   wl_list_init(&cvt->func_datas);
   wl_list_init(&cvt->src_bufs);
   wl_list_init(&cvt->dst_bufs);

   DBG("cvt(%p) stamp(%d)", cvt, stamp);

   wl_list_insert(&cvt_list, &cvt->link);

   e_devicemgr_drm_ipp_handler_add(_e_devmgr_cvt_ipp_handler, cvt);

   return cvt;
}

void
e_devmgr_cvt_destroy(E_Devmgr_Cvt *cvt)
{
   E_Devmgr_CvtFuncData *cur = NULL, *next = NULL;

   if (!cvt)
     return;

   _e_devmgr_cvt_stop(cvt);

   wl_list_remove(&cvt->link);

   wl_list_for_each_safe(cur, next, &cvt->func_datas, link)
     {
       wl_list_remove(&cur->link);
       free(cur);
     }

   e_devicemgr_drm_ipp_handler_del(_e_devmgr_cvt_ipp_handler, cvt);

   DBG("cvt(%p)", cvt);

   free(cvt);
}

Eina_Bool
e_devmgr_cvt_property_set(E_Devmgr_Cvt *cvt, E_Devmgr_Cvt_Prop *src, E_Devmgr_Cvt_Prop *dst)
{
   if (cvt->started)
     return EINA_TRUE;

   struct drm_exynos_ipp_property property;

   EINA_SAFETY_ON_NULL_RETURN_VAL(cvt, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(src, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(dst, EINA_FALSE);

   if (!e_devmgr_cvt_ensure_size(src, dst))
     return EINA_FALSE;

   if (dst->crop.x + dst->crop.w > dst->width)
     {
         DBG("dst(%d+%d > %d). !", dst->crop.x, dst->crop.w, dst->width);
     }

   memcpy(&cvt->props[CVT_TYPE_SRC], src, sizeof(E_Devmgr_Cvt_Prop));
   memcpy(&cvt->props[CVT_TYPE_DST], dst, sizeof(E_Devmgr_Cvt_Prop));

   CLEAR(property);
   fill_config(CVT_TYPE_SRC, &cvt->props[CVT_TYPE_SRC], &property.config[0]);
   fill_config(CVT_TYPE_DST, &cvt->props[CVT_TYPE_DST], &property.config[1]);
   property.cmd = IPP_CMD_M2M;
   property.prop_id = cvt->prop_id;
   //    property.protect = dst->secure;
   property.range = dst->csc_range;

   DBG("cvt(%p) src('%c%c%c%c', '%c%c%c%c', %dx%d, %d,%d %dx%d, %d, %d&%d, %d, %d)",
       cvt, FOURCC_STR(src->drmfmt), FOURCC_STR(src->drmfmt),
       src->width, src->height,
       src->crop.x, src->crop.y, src->crop.w, src->crop.h,
       src->degree, src->hflip, src->vflip,
       src->secure, src->csc_range);

   DBG("cvt(%p) dst('%c%c%c%c', '%c%c%c%c',%dx%d, %d,%d %dx%d, %d, %d&%d, %d, %d)",
       cvt, FOURCC_STR(dst->drmfmt), FOURCC_STR(dst->drmfmt),
       dst->width, dst->height,
       dst->crop.x, dst->crop.y, dst->crop.w, dst->crop.h,
       dst->degree, dst->hflip, dst->vflip,
       dst->secure, dst->csc_range);

   cvt->prop_id = e_devicemgr_drm_ipp_set(&property);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(cvt->prop_id >= 0, EINA_FALSE);

   return EINA_TRUE;
}

void
e_devmgr_cvt_property_get(E_Devmgr_Cvt *cvt, E_Devmgr_Cvt_Prop *src, E_Devmgr_Cvt_Prop *dst)
{
   EINA_SAFETY_ON_NULL_RETURN(cvt);

   if (src)
     *src = cvt->props[CVT_TYPE_SRC];

   if (dst)
     *dst = cvt->props[CVT_TYPE_DST];
}

Eina_Bool
e_devmgr_cvt_convert(E_Devmgr_Cvt *cvt, E_Devmgr_Buf *src, E_Devmgr_Buf *dst)
{
   E_Devmgr_CvtBuf *src_cbuf = NULL, *dst_cbuf = NULL;

   EINA_SAFETY_ON_NULL_RETURN_VAL(cvt, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(src, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(dst, EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(MBUF_IS_VALID(src), EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(MBUF_IS_VALID(dst), EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(cvt->prop_id >= 0, EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(src->handles[0] > 0, EINA_FALSE);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(dst->handles[0] > 0, EINA_FALSE);

   src_cbuf = calloc(1, sizeof(E_Devmgr_CvtBuf));
   EINA_SAFETY_ON_FALSE_GOTO(src_cbuf != NULL, fail);
   dst_cbuf = calloc(1, sizeof(E_Devmgr_CvtBuf));
   EINA_SAFETY_ON_FALSE_GOTO(dst_cbuf != NULL, fail);

   src_cbuf->type = CVT_TYPE_SRC;
   src_cbuf->mbuf = e_devmgr_buffer_ref(src);
   memcpy(src_cbuf->handles, src->handles, sizeof(uint) * 4);

   dst_cbuf->type = CVT_TYPE_DST;
   dst_cbuf->mbuf = e_devmgr_buffer_ref(dst);
   memcpy(dst_cbuf->handles, dst->handles, sizeof(uint) * 4);

   if (!_e_devmgr_cvt_queue(cvt, src_cbuf))
     {
        ERR("error: queue src buffer");
        goto fail;
     }

   DBG("cvt(%p) srcbuf(%p) converting(%d)", cvt, src, MBUF_IS_CONVERTING(src));

   if (!_e_devmgr_cvt_queue(cvt, dst_cbuf))
     {
         ERR("error: queue dst buffer");
         goto fail_queue_dst;
     }

   DBG("cvt(%p) dstbuf(%p) converting(%d)", cvt, dst, MBUF_IS_CONVERTING(dst));

   DBG("==> ipp(%d,%d,%d : %d,%d,%d) ",
       src->stamp, MBUF_IS_CONVERTING(src), src->showing,
       dst->stamp, MBUF_IS_CONVERTING(dst), dst->showing);

   if (!cvt->started)
     {
        struct drm_exynos_ipp_cmd_ctrl ctrl = {0,};
        ctrl.prop_id = cvt->prop_id;
        ctrl.ctrl = IPP_CTRL_PLAY;
        if (!e_devicemgr_drm_ipp_cmd(&ctrl))
            goto fail_cmd;
        DBG("cvt(%p) start. prop_id(%d)", cvt, ctrl.prop_id);
        cvt->started = EINA_TRUE;
     }

   src_cbuf->begin = e_devmgr_buffer_get_mills();

   return EINA_TRUE;

fail_cmd:
   _e_devmgr_cvt_dequeue(cvt, dst_cbuf);
   _e_devmgr_cvt_dequeued(cvt, dst_cbuf->type, dst_cbuf->index);
fail_queue_dst:
   _e_devmgr_cvt_dequeue(cvt, src_cbuf);
   _e_devmgr_cvt_dequeued(cvt, src_cbuf->type, src_cbuf->index);

   _e_devmgr_cvt_stop(cvt);
   return EINA_FALSE;
fail:
   if (src_cbuf)
     {
        e_devmgr_buffer_unref(src_cbuf->mbuf);
        free(src_cbuf);
     }
   if (dst_cbuf)
     {
        e_devmgr_buffer_unref(dst_cbuf->mbuf);
        free(dst_cbuf);
     }

   _e_devmgr_cvt_stop(cvt);
   return EINA_FALSE;
}

Eina_Bool
e_devmgr_cvt_cb_add(E_Devmgr_Cvt *cvt, CvtFunc func, void *data)
{
   E_Devmgr_CvtFuncData *func_data;

   EINA_SAFETY_ON_NULL_RETURN_VAL(cvt, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(func, EINA_FALSE);

   func_data = calloc(1, sizeof(E_Devmgr_CvtFuncData));
   EINA_SAFETY_ON_NULL_RETURN_VAL(func_data, EINA_FALSE);

   wl_list_insert(&cvt->func_datas, &func_data->link);

   func_data->func = func;
   func_data->data = data;

   return EINA_TRUE;
}

void
e_devmgr_cvt_cb_del(E_Devmgr_Cvt *cvt, CvtFunc func, void *data)
{
   E_Devmgr_CvtFuncData *cur = NULL, *next = NULL;

   EINA_SAFETY_ON_NULL_RETURN(cvt);
   EINA_SAFETY_ON_NULL_RETURN(func);

   wl_list_for_each_safe(cur, next, &cvt->func_datas, link)
     {
        if (cur->func == func && cur->data == data)
          {
             wl_list_remove(&cur->link);
             free(cur);
          }
     }
}
