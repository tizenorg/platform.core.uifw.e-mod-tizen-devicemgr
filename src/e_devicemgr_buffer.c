#include <sys/mman.h>
#include <e.h>
#include <Ecore_Drm.h>
#include <pixman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>
#include <png.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_drm.h"
#include "e_devicemgr_buffer.h"

//#define DEBUG_LIFECYCLE

#define PNG_DEPTH 8

#define ALIGN_TO_16B(x)    ((((x) + (1 <<  4) - 1) >>  4) <<  4)
#define ALIGN_TO_32B(x)    ((((x) + (1 <<  5) - 1) >>  5) <<  5)
#define ALIGN_TO_128B(x)   ((((x) + (1 <<  7) - 1) >>  7) <<  7)
#define ALIGN_TO_2KB(x)    ((((x) + (1 << 11) - 1) >> 11) << 11)
#define ALIGN_TO_8KB(x)    ((((x) + (1 << 13) - 1) >> 13) << 13)
#define ALIGN_TO_64KB(x)   ((((x) + (1 << 16) - 1) >> 16) << 16)

#define BER(fmt,arg...)   ERR("%d: "fmt, mbuf->stamp, ##arg)
#define BWR(fmt,arg...)   WRN("%d: "fmt, mbuf->stamp, ##arg)
#define BIN(fmt,arg...)   INF("%d: "fmt, mbuf->stamp, ##arg)
#define BDB(fmt,arg...)   DBG("%d: "fmt, mbuf->stamp, ##arg)

#define MBUF_RETURN_IF_FAIL(cond) \
   {if (!(cond)) { BER("'%s' failed. (%s)", #cond, func); return; }}
#define MBUF_RETURN_VAL_IF_FAIL(cond, val) \
   {if (!(cond)) { BER("'%s' failed. (%s)", #cond, func); return val; }}

#ifdef DEBUG_LIFECYCLE
#undef BDB
#define BDB BIN
#endif

typedef struct _MBufFreeFuncInfo
{
   MBuf_Free_Func  func;
   void             *data;
} MBufFreeFuncInfo;

typedef struct _ColorTable
{
   unsigned int            drmfmt;
   E_Devmgr_Buf_Color_Type type;
} ColorTable;

static ColorTable color_table[] =
{
   { DRM_FORMAT_ARGB8888,  TYPE_RGB    },
   { DRM_FORMAT_XRGB8888,  TYPE_RGB    },
   { DRM_FORMAT_YVU420,    TYPE_YUV420 },
   { DRM_FORMAT_YUV420,    TYPE_YUV420 },
   { DRM_FORMAT_NV12MT,    TYPE_YUV420 },
   { DRM_FORMAT_NV12,      TYPE_YUV420 },
   { DRM_FORMAT_NV21,      TYPE_YUV420 },
   { DRM_FORMAT_YUYV,      TYPE_YUV422 },
   { DRM_FORMAT_UYVY,      TYPE_YUV422 },
};

static void _e_devmgr_buffer_free(E_Devmgr_Buf *mbuf, const char *func);
#define e_devmgr_buffer_free(b) _e_devmgr_buffer_free(b,__FUNCTION__)

static Eina_List *mbuf_lists;

int
e_devmgr_buf_image_attr(uint drmfmt, int w, int h, uint *pitches, uint *lengths)
{
   int size = 0, tmp = 0;

   switch (drmfmt)
     {
      case DRM_FORMAT_ARGB8888:
      case DRM_FORMAT_XRGB8888:
        size += (w << 2);
        if (pitches) pitches[0] = size;
        size *= h;
        if (lengths) lengths[0] = size;
        break;
      /* YUV422, packed */
      case DRM_FORMAT_YUV422:
      case DRM_FORMAT_YVU422:
        size = w << 1;
        if (pitches) pitches[0] = size;
        size *= h;
        if (lengths) lengths[0] = size;
        break;
      /* YUV420, 3 planar */
      case DRM_FORMAT_YUV420:
      case DRM_FORMAT_YVU420:
        size = ROUNDUP(w, 4);
        if (pitches) pitches[0] = size;
        size *= h;
        if (lengths) lengths[0] = size;
        tmp = ROUNDUP((w >> 1), 4);
        if (pitches) pitches[1] = pitches[2] = tmp;
        tmp *= (h >> 1);
        size += tmp;
        if (lengths) lengths[1] = tmp;
        size += tmp;
        if (lengths) lengths[2] = tmp;
        break;
      /* YUV420, 2 planar */
      case DRM_FORMAT_NV12:
      case DRM_FORMAT_NV21:
        if (pitches) pitches[0] = w;
        size = w * h;
        if (lengths) lengths[0] = size;
        if (pitches) pitches[1] = w;
        tmp = (w) * (h >> 1);
        size += tmp;
        if (lengths) lengths[1] = tmp;
        break;
      /* YUV420, 2 planar, tiled */
      case DRM_FORMAT_NV12MT:
        if (pitches) pitches[0] = w;
        size = ALIGN_TO_8KB(ALIGN_TO_128B(w) * ALIGN_TO_32B(h));
        if (lengths) lengths[0] = size;
        if (pitches) pitches[1] = w;
        tmp = ALIGN_TO_8KB(ALIGN_TO_128B(w) * ALIGN_TO_32B(h >> 1));
        size += tmp;
        if (lengths) lengths[1] = tmp;
        break;
      default:
        return 0;
     }

   return size;
}

static tbm_bo
_e_devmgr_buf_normal_alloc(int size, int flags)
{
   return tbm_bo_alloc(e_devmgr_bufmgr, size, flags);
}

static tbm_bo
_e_devmgr_buf_secure_alloc(int size, int flags)
{
   struct tzmem_get_region tzmem_get = {0,};
   struct drm_prime_handle arg_handle = {0,};
   struct drm_gem_flink arg_flink = {0,};
   struct drm_gem_close arg_close = {0,};
   tbm_bo bo = NULL;
   int tzmem_fd;

   tzmem_fd = -1;
   tzmem_get.fd = -1;

   tzmem_fd = open("/dev/tzmem", O_EXCL);
   EINA_SAFETY_ON_FALSE_GOTO(tzmem_fd >= 0, done_secure_buffer);

   tzmem_get.key = "fimc";
   tzmem_get.size = size;
   if (ioctl(tzmem_fd, TZMEM_IOC_GET_TZMEM, &tzmem_get))
     {
        ERR("failed : create tzmem (%d)", size);
        goto done_secure_buffer;
     }
   EINA_SAFETY_ON_FALSE_GOTO(tzmem_get.fd >= 0, done_secure_buffer);

   arg_handle.fd = (__s32)tzmem_get.fd;
   if (drmIoctl(e_devmgr_drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg_handle))
     {
        ERR("failed : convert to gem (%d)", tzmem_get.fd);
        goto done_secure_buffer;
     }
   EINA_SAFETY_ON_FALSE_GOTO(arg_handle.handle > 0, done_secure_buffer);

   arg_flink.handle = arg_handle.handle;
   if (drmIoctl(e_devmgr_drm_fd, DRM_IOCTL_GEM_FLINK, &arg_flink))
     {
        ERR("failed : flink gem (%d)", arg_handle.handle);
        goto done_secure_buffer;
     }
   EINA_SAFETY_ON_FALSE_GOTO(arg_flink.name > 0, done_secure_buffer);

   bo = tbm_bo_import(e_devmgr_bufmgr, arg_flink.name);
   EINA_SAFETY_ON_FALSE_GOTO(bo != NULL, done_secure_buffer);

   done_secure_buffer:
   if (arg_handle.handle > 0)
     {
        arg_close.handle = arg_handle.handle;
        if (drmIoctl(e_devmgr_drm_fd, DRM_IOCTL_GEM_CLOSE, &arg_close))
          ERR("failed : close gem (%d)", arg_handle.handle);
     }

   if (tzmem_get.fd >= 0)
     close(tzmem_get.fd);

   if (tzmem_fd >= 0)
     close(tzmem_fd);

   return bo;
}

E_Devmgr_Buf_Color_Type
e_devmgr_buffer_color_type(uint drmfmt)
{
   int i, size;

   size = sizeof(color_table) / sizeof(ColorTable);

   for (i = 0; i < size; i++)
     if (color_table[i].drmfmt == drmfmt)
       return color_table[i].type;

   return TYPE_NONE;
}

static E_Devmgr_Buf*
_find_mbuf(uint stamp)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   if (!mbuf_lists)
     return NULL;

   EINA_LIST_FOREACH(mbuf_lists, l, mbuf)
     {
        if (mbuf->stamp == stamp)
          return mbuf;
     }

   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create(Tizen_Buffer *tizen_buffer, Eina_Bool secure, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   uint stamp, pitches[4] = {0,};
   int i;

   EINA_SAFETY_ON_NULL_RETURN_VAL(tizen_buffer, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(tizen_buffer->drm_buffer, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(tizen_buffer->drm_buffer->resource, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, create_fail);

   if (tizen_buffer->drm_buffer->format == TIZEN_BUFFER_POOL_FORMAT_ST12)
     mbuf->drmfmt = DRM_FORMAT_NV12MT;
   else if (tizen_buffer->drm_buffer->format == TIZEN_BUFFER_POOL_FORMAT_SN12)
     mbuf->drmfmt = DRM_FORMAT_NV12;
   else
     mbuf->drmfmt = tizen_buffer->drm_buffer->format;

   mbuf->width = tizen_buffer->drm_buffer->width;
   mbuf->height = tizen_buffer->drm_buffer->height;
   for (i = 0; i < 3; i++)
     {
        if (tizen_buffer->bo[i])
          {
            mbuf->handles[i] = tbm_bo_get_handle(tizen_buffer->bo[i], TBM_DEVICE_DEFAULT).u32;
            mbuf->pitches[i] = tizen_buffer->drm_buffer->stride[i];
            mbuf->offsets[i] = tizen_buffer->drm_buffer->offset[i];
            if (!secure)
              mbuf->ptrs[i] = tbm_bo_get_handle(tizen_buffer->bo[i], TBM_DEVICE_CPU).ptr;
          }
     }

   e_devmgr_buf_image_attr(mbuf->drmfmt, mbuf->width, mbuf->height, pitches, NULL);
   if (!IS_RGB(mbuf->drmfmt))
     {
        if (mbuf->pitches[1] == 0 && pitches[1] > 0)
          mbuf->pitches[1] = pitches[1];
        if (mbuf->pitches[2] == 0 && pitches[2] > 0)
          mbuf->pitches[2] = pitches[2];
     }

   mbuf->type = TYPE_TB;
   mbuf->b.tizen_buffer = tizen_buffer;

   mbuf->secure = secure;

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   stamp = e_devmgr_buffer_get_mills();
   while (_find_mbuf(stamp))
     stamp++;
   mbuf->stamp = stamp;

   mbuf->func = strdup(func);
   mbuf->ref_cnt = 1;

   BDB("%dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d): %s",
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2], func);

   return mbuf;

create_fail:
   e_devmgr_buffer_free(mbuf);

   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create_fb(Tizen_Buffer *tizen_buffer, Eina_Bool secure, const char *func)
{
   E_Devmgr_Buf *mbuf = _e_devmgr_buffer_create(tizen_buffer, secure, func);

   if (drmModeAddFB2(e_devmgr_drm_fd, mbuf->width, mbuf->height, mbuf->drmfmt,
                     mbuf->handles, mbuf->pitches, mbuf->offsets, &mbuf->fb_id, 0))
     BER("%dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d): %s",
         mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
         mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
         mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
         mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2], func);

   EINA_SAFETY_ON_FALSE_GOTO(mbuf->fb_id > 0, create_fail);

   BDB("fb_id(%d): %s", mbuf->fb_id, func);

   return mbuf;
create_fail:
   e_devmgr_buffer_free(mbuf);
   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create_shm(struct wl_shm_buffer *shm_buffer, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   uint stamp;
   uint32_t format;

   EINA_SAFETY_ON_NULL_RETURN_VAL(shm_buffer, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, create_fail);

   format = wl_shm_buffer_get_format(shm_buffer);

   if (format == WL_SHM_FORMAT_ARGB8888)
     mbuf->drmfmt = DRM_FORMAT_ARGB8888;
   else if (format == WL_SHM_FORMAT_XRGB8888)
     mbuf->drmfmt = DRM_FORMAT_XRGB8888;
   else
     mbuf->drmfmt = format;

   mbuf->width = wl_shm_buffer_get_width(shm_buffer);
   mbuf->height = wl_shm_buffer_get_height(shm_buffer);

   mbuf->pitches[0] = wl_shm_buffer_get_stride(shm_buffer);
   mbuf->ptrs[0] = wl_shm_buffer_get_data(shm_buffer);

   mbuf->type = TYPE_SHM;
   mbuf->b.shm_buffer = shm_buffer;

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   stamp = e_devmgr_buffer_get_mills();
   while (_find_mbuf(stamp))
     stamp++;
   mbuf->stamp = stamp;

   mbuf->func = strdup(func);
   mbuf->ref_cnt = 1;

   BDB("%dx%d, %c%c%c%c, (%d), (%d): %s",
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
       mbuf->handles[0], mbuf->pitches[0], func);

   return mbuf;

create_fail:
   e_devmgr_buffer_free(mbuf);

   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create_hnd(uint handle, int width, int height, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   struct drm_mode_map_dumb arg = {0,};
   uint stamp;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(handle > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(width > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(height > 0, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, create_fail);

   mbuf->drmfmt = DRM_FORMAT_ARGB8888;
   mbuf->width = width;
   mbuf->height = height;
   mbuf->pitches[0] = (width << 2);

   mbuf->type = TYPE_HND;
   mbuf->b.handle = mbuf->handles[0] = arg.handle = handle;

   if (drmIoctl(e_devmgr_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &arg))
     {
        ERR("cannot map handle(%d)\n", handle);
        free(mbuf);
        return NULL;
     }

   mbuf->ptrs[0] = mmap(NULL, mbuf->width * mbuf->height * 4,
                        PROT_READ|PROT_WRITE, MAP_SHARED, e_devmgr_drm_fd, arg.offset);
   if (mbuf->ptrs[0] == MAP_FAILED)
      {
         ERR("cannot mmap handle(%d)\n", handle);
         free(mbuf);
         return NULL;
      }

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   stamp = e_devmgr_buffer_get_mills();
   while(_find_mbuf(stamp))
     stamp++;
   mbuf->stamp = stamp;

   mbuf->func = strdup(func);
   mbuf->ref_cnt = 1;

   BDB("%dx%d, %c%c%c%c, (%d) (%d): %s",
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
       mbuf->handles[0], mbuf->pitches[0], func);

   return mbuf;

create_fail:
   e_devmgr_buffer_free(mbuf);
   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_alloc(int width, int height, uint drmfmt, Eina_Bool secure, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   uint stamp;
   int size;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(width > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(height > 0, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, alloc_fail);

   mbuf->drmfmt = drmfmt;
   mbuf->width = width;
   mbuf->height = height;

   size = e_devmgr_buf_image_attr(drmfmt, ROUNDUP(width, 16), height, mbuf->pitches, NULL);

   mbuf->type = TYPE_BO;

   if (!secure)
     mbuf->b.bo[0] = _e_devmgr_buf_normal_alloc(size, TBM_BO_DEFAULT);
   else
     mbuf->b.bo[0] = _e_devmgr_buf_secure_alloc(size, TBM_BO_DEFAULT);
   EINA_SAFETY_ON_FALSE_GOTO(mbuf->b.bo[0] != NULL, alloc_fail);

   mbuf->handles[0] = tbm_bo_get_handle(mbuf->b.bo[0], TBM_DEVICE_DEFAULT).u32;
   EINA_SAFETY_ON_FALSE_GOTO(mbuf->handles[0] > 0, alloc_fail);

   if (!secure)
     mbuf->ptrs[0] = tbm_bo_get_handle(mbuf->b.bo[0], TBM_DEVICE_CPU).ptr;

   mbuf->secure = secure;

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   stamp = e_devmgr_buffer_get_mills();
   while(_find_mbuf(stamp))
     stamp++;
   mbuf->stamp = stamp;

   mbuf->func = strdup(func);
   mbuf->ref_cnt = 1;

   BDB("%dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d): %s",
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2], func);

   return mbuf;

alloc_fail:
   e_devmgr_buffer_free(mbuf);

   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_alloc_fb(int width, int height, Eina_Bool secure, const char *func)
{
   E_Devmgr_Buf *mbuf = _e_devmgr_buffer_alloc(width, height, DRM_FORMAT_ARGB8888, secure, func);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mbuf, NULL);

   if (drmModeAddFB2(e_devmgr_drm_fd, mbuf->width, mbuf->height, mbuf->drmfmt,
                     mbuf->handles, mbuf->pitches, mbuf->offsets, &mbuf->fb_id, 0))
     BER("%dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d): %s",
         mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
         mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
         mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
         mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2], func);
   EINA_SAFETY_ON_FALSE_GOTO(mbuf->fb_id > 0, alloc_fail);

   BDB("fb_id(%d): %s", mbuf->fb_id, func);

   return mbuf;

alloc_fail:
   e_devmgr_buffer_free(mbuf);

   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_ref(E_Devmgr_Buf *mbuf, const char *func)
{
   if (!mbuf)
     return NULL;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(MBUF_IS_VALID(mbuf), NULL);

   mbuf->ref_cnt++;
   BDB("count(%d) ref: %s", mbuf->ref_cnt, func);

   return mbuf;
}

void
_e_devmgr_buffer_unref(E_Devmgr_Buf *mbuf, const char *func)
{
   if (!mbuf)
     return;

   MBUF_RETURN_IF_FAIL(_e_devmgr_buffer_valid(mbuf, func));

   mbuf->ref_cnt--;
   BDB("count(%d) unref: %s", mbuf->ref_cnt, func);

   if (mbuf->ref_cnt == 0)
     _e_devmgr_buffer_free(mbuf, func);
}

static void
_e_devmgr_buffer_free(E_Devmgr_Buf *mbuf, const char *func)
{
   MBufFreeFuncInfo *info;
   Eina_List *l, *ll;
   int i;

   if (!mbuf)
     return;

   /* make sure all operation is done */
   MBUF_RETURN_IF_FAIL(mbuf->ref_cnt == 0);
   MBUF_RETURN_IF_FAIL(_e_devmgr_buffer_valid(mbuf, func));
   MBUF_RETURN_IF_FAIL(!MBUF_IS_CONVERTING(mbuf));
   MBUF_RETURN_IF_FAIL(mbuf->showing == EINA_FALSE);

   EINA_LIST_FOREACH_SAFE(mbuf->free_funcs, l, ll, info)
     {
        /* call before tmb_bo_unref and drmModeRmFB. */
        mbuf->free_funcs = eina_list_remove_list(mbuf->free_funcs, l);
        if (info->func)
            info->func(mbuf, info->data);
        free(info);
     }

   if (mbuf->fb_id > 0)
     {
        BDB("fb_id(%d) removed. ", mbuf->fb_id);
        drmModeRmFB(e_devmgr_drm_fd, mbuf->fb_id);
     }

   if (mbuf->type == TYPE_HND && mbuf->ptrs[0])
      munmap(mbuf->ptrs[0], mbuf->width * mbuf->height * 4);

   for (i = 0; i < 4; i++)
     {
        if (mbuf->type == TYPE_BO && mbuf->b.bo[i])
          tbm_bo_unref(mbuf->b.bo[i]);
     }

   mbuf_lists = eina_list_remove(mbuf_lists, mbuf);

   BDB("freed: %s", func);

   mbuf->stamp = 0;

   if (mbuf->func)
     free(mbuf->func);

   free(mbuf);
}

void
e_devmgr_buffer_clear(E_Devmgr_Buf *mbuf)
{
}

Eina_Bool
_e_devmgr_buffer_valid(E_Devmgr_Buf *mbuf, const char *func)
{
   E_Devmgr_Buf *temp;
   Eina_List *l;

   MBUF_RETURN_VAL_IF_FAIL(mbuf != NULL, EINA_FALSE);
   MBUF_RETURN_VAL_IF_FAIL(mbuf->stamp != 0, EINA_FALSE);

   EINA_LIST_FOREACH(mbuf_lists, l, temp)
     {
        if (temp->stamp == mbuf->stamp)
            return EINA_TRUE;
     }

   return EINA_FALSE;
}

static MBufFreeFuncInfo*
_e_devmgr_buffer_free_func_find(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data)
{
   MBufFreeFuncInfo *info;
   Eina_List *l;

   EINA_LIST_FOREACH(mbuf->free_funcs, l, info)
     {
        if (info->func == func && info->data == data)
            return info;
     }

   return NULL;
}

void
e_devmgr_buffer_free_func_add(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data)
{
   MBufFreeFuncInfo *info;

   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(mbuf));
   EINA_SAFETY_ON_NULL_RETURN(func);

   info = _e_devmgr_buffer_free_func_find(mbuf, func, data);
   if (info)
     return;

   info = calloc(1, sizeof(MBufFreeFuncInfo));
   EINA_SAFETY_ON_NULL_RETURN(info);

   info->func = func;
   info->data = data;

   mbuf->free_funcs = eina_list_append(mbuf->free_funcs, info);
}

void
e_devmgr_buffer_free_func_del(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data)
{
   MBufFreeFuncInfo *info;

   EINA_SAFETY_ON_FALSE_RETURN(MBUF_IS_VALID(mbuf));
   EINA_SAFETY_ON_NULL_RETURN(func);

   info = _e_devmgr_buffer_free_func_find(mbuf, func, data);
   if (!info)
     return;

   mbuf->free_funcs = eina_list_remove(mbuf->free_funcs, info);

   free(info);
}

static pixman_format_code_t
_e_devmgr_buffer_pixman_format_get(E_Devmgr_Buf *mbuf)
{
   switch(mbuf->drmfmt)
     {
      case DRM_FORMAT_ARGB8888:
        return PIXMAN_a8r8g8b8;
      case DRM_FORMAT_XRGB8888:
        return PIXMAN_x8r8g8b8;
      default:
        return 0;
     }
   return 0;
}

void
e_devmgr_buffer_convert(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf,
                        int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh,
                        Eina_Bool over, int rotate, int hflip, int vflip)
{
   pixman_image_t *src_img = NULL, *dst_img = NULL;
   pixman_format_code_t src_format, dst_format;
   double scale_x, scale_y;
   int rotate_step;
   pixman_transform_t t;
   struct pixman_f_transform ft;
   pixman_op_t op;
   int src_stride, dst_stride;
   int buf_width;

   /* not handle buffers which have 2 more gem handles */
   EINA_SAFETY_ON_NULL_GOTO(srcbuf->ptrs[0], cant_convert);
   EINA_SAFETY_ON_NULL_GOTO(dstbuf->ptrs[0], cant_convert);
   EINA_SAFETY_ON_FALSE_RETURN(!srcbuf->ptrs[1]);
   EINA_SAFETY_ON_FALSE_RETURN(!dstbuf->ptrs[1]);

   src_format = _e_devmgr_buffer_pixman_format_get(srcbuf);
   EINA_SAFETY_ON_FALSE_GOTO(src_format > 0, cant_convert);
   dst_format = _e_devmgr_buffer_pixman_format_get(dstbuf);
   EINA_SAFETY_ON_FALSE_GOTO(dst_format > 0, cant_convert);

   buf_width = IS_RGB(srcbuf->drmfmt)?(srcbuf->pitches[0]/4):srcbuf->pitches[0];
   src_stride = buf_width * (PIXMAN_FORMAT_BPP(src_format) / 8);
   src_img = pixman_image_create_bits(src_format, buf_width, srcbuf->height,
                                      (uint32_t*)srcbuf->ptrs[0], src_stride);
   EINA_SAFETY_ON_NULL_GOTO(src_img, cant_convert);

   buf_width = IS_RGB(dstbuf->drmfmt)?(dstbuf->pitches[0]/4):dstbuf->pitches[0];
   dst_stride = buf_width * (PIXMAN_FORMAT_BPP(dst_format) / 8);
   dst_img = pixman_image_create_bits(dst_format, buf_width, dstbuf->height,
                                      (uint32_t*)dstbuf->ptrs[0], dst_stride);
   EINA_SAFETY_ON_NULL_GOTO(dst_img, cant_convert);

   pixman_f_transform_init_identity(&ft);

   if (hflip)
     {
        pixman_f_transform_scale(&ft, NULL, -1, 1);
        pixman_f_transform_translate(&ft, NULL, dw, 0);
     }

   if (vflip)
     {
        pixman_f_transform_scale(&ft, NULL, 1, -1);
        pixman_f_transform_translate(&ft, NULL, 0, dh);
     }

   rotate_step = (rotate + 360) / 90 % 4;
   if (rotate_step > 0)
     {
        int c, s, tx = 0, ty = 0;
        switch (rotate_step)
          {
           case 1:
              c = 0, s = -1, tx = -dw;
              break;
           case 2:
              c = -1, s = 0, tx = -dw, ty = -dh;
              break;
           case 3:
              c = 0, s = 1, ty = -dh;
              break;
           default:
              c = 0, s = 0;
              break;
          }
        pixman_f_transform_translate(&ft, NULL, tx, ty);
        pixman_f_transform_rotate(&ft, NULL, c, s);
     }

   if (rotate_step % 2 == 0)
     {
        scale_x = (double)sw / dw;
        scale_y = (double)sh / dh;
     }
   else
     {
        scale_x = (double)sw / dh;
        scale_y = (double)sh / dw;
     }

   pixman_f_transform_scale(&ft, NULL, scale_x, scale_y);
   pixman_f_transform_translate(&ft, NULL, sx, sy);
   pixman_transform_from_pixman_f_transform(&t, &ft);
   pixman_image_set_transform(src_img, &t);

   if (!over) op = PIXMAN_OP_SRC;
   else op = PIXMAN_OP_OVER;

   pixman_image_composite(op, src_img, NULL, dst_img, 0, 0, 0, 0,
                          dx, dy, dw, dh);
cant_convert:
   if (src_img) pixman_image_unref(src_img);
   if (dst_img) pixman_image_unref(dst_img);
}

static void
_copy_image_YUV422(unsigned char *s, uint *s_pitches, unsigned char *d, uint *d_pitches, int height)
{
   int i;

   for (i = 0; i < height; i++)
     {
        memcpy(d, s, s_pitches[i]);
        s += s_pitches[i];
        d += d_pitches[i];
     }
}

static void
_copy_image_YUV420(unsigned char *s, uint *s_pitches, unsigned char *d, uint *d_pitches, int height)
{
   int i, j, c_height;

   for (i = 0; i < 3; i++)
     {
        c_height = (i == 0) ? height : height / 2;

        for (j = 0; j < c_height; j++)
          {
             memcpy(d, s, s_pitches[i]);
             s += s_pitches[i];
             d += d_pitches[i];
          }
     }
}

static void
_copy_image_NV12(unsigned char *s, uint *s_pitches, unsigned char *d, uint *d_pitches, int height)
{
   int i, j, c_height;

   for (i = 0; i < 2; i++)
     {
        c_height = (i == 0) ? height : height / 2;

        for (j = 0; j < c_height; j++)
          {
             memcpy(d, s, s_pitches[i]);
             s += s_pitches[i];
             d += d_pitches[i];
          }
     }
}

Eina_Bool
e_devmgr_buffer_copy(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf)
{
   switch (srcbuf->drmfmt)
     {
      case DRM_FORMAT_YUV422:
      case DRM_FORMAT_YVU422:
        _copy_image_YUV422((unsigned char*)srcbuf->ptrs[0], srcbuf->pitches,
                           (unsigned char*)dstbuf->ptrs[0], dstbuf->pitches,
                           srcbuf->height);
        break;
      case DRM_FORMAT_YUV420:
      case DRM_FORMAT_YVU420:
        _copy_image_YUV420((unsigned char*)srcbuf->ptrs[0], srcbuf->pitches,
                           (unsigned char*)dstbuf->ptrs[0], dstbuf->pitches,
                           srcbuf->height);
        break;
      case DRM_FORMAT_NV12:
      case DRM_FORMAT_NV21:
        _copy_image_NV12((unsigned char*)srcbuf->ptrs[0], srcbuf->pitches,
                         (unsigned char*)dstbuf->ptrs[0], dstbuf->pitches,
                         srcbuf->height);
        break;
      default:
        ERR("not implemented for %c%c%c%c", FOURCC_STR(srcbuf->drmfmt));
        return EINA_FALSE;
     }

   return EINA_TRUE;
}

uint
e_devmgr_buffer_get_mills(void)
{
   struct timespec tp;

   clock_gettime(CLOCK_MONOTONIC, &tp);

   return (tp.tv_sec * 1000) + (tp.tv_nsec / 1000000L);
}

static void
_dump_raw(const char * file, void *data1, int size1, void *data2, int size2)
{
   unsigned int * blocks;
   FILE * fp = fopen(file, "w+");
   EINA_SAFETY_ON_NULL_RETURN(fp);

   blocks = (unsigned int*)data1;
   fwrite(blocks, 1, size1, fp);

   if (size2 > 0)
     {
        blocks = (unsigned int*)data2;
        fwrite(blocks, 1, size2, fp);
     }

   fclose(fp);
}

static void
_dump_png(const char* file, const void * data, int width, int height)
{
   FILE *fp = fopen(file, "wb");
   EINA_SAFETY_ON_NULL_RETURN(fp);

   png_structp pPngStruct = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   EINA_SAFETY_ON_NULL_RETURN(pPngStruct);

   png_infop pPngInfo = png_create_info_struct(pPngStruct);
   EINA_SAFETY_ON_NULL_RETURN(pPngInfo);

   png_init_io(pPngStruct, fp);
   png_set_IHDR(pPngStruct,
                pPngInfo,
                width,
                height,
                PNG_DEPTH,
                PNG_COLOR_TYPE_RGBA,
                PNG_INTERLACE_NONE,
                PNG_COMPRESSION_TYPE_DEFAULT,
                PNG_FILTER_TYPE_DEFAULT);

   png_set_bgr(pPngStruct);
   png_write_info(pPngStruct, pPngInfo);

   const int pixel_size = 4; // RGBA
   png_bytep *row_pointers = png_malloc(pPngStruct, height * sizeof(png_byte *));

   unsigned int* blocks = (unsigned int*)data;
   int y = 0;
   int x = 0;

   for (; y < height; ++y)
     {
        png_bytep row = png_malloc(pPngStruct, sizeof(png_byte)*width * pixel_size);
        row_pointers[y] = (png_bytep)row;
        for (x = 0; x < width; ++x)
          {
             unsigned int curBlock = blocks[y * width + x];
             row[x * pixel_size] = (curBlock & 0xFF);
             row[1 + x * pixel_size] = (curBlock >> 8) & 0xFF;
             row[2 + x * pixel_size] = (curBlock >> 16) & 0xFF;
             row[3 + x * pixel_size] = 0xFF;
          }
     }

   png_write_image(pPngStruct, row_pointers);
   png_write_end(pPngStruct, pPngInfo);

   for (y = 0; y < height; y++)
     png_free(pPngStruct, row_pointers[y]);
   png_free(pPngStruct, row_pointers);

   png_destroy_write_struct(&pPngStruct, &pPngInfo);

   fclose(fp);
}

void
e_devmgr_buffer_dump(E_Devmgr_Buf *mbuf, const char *prefix, int nth, Eina_Bool raw)
{
   char path[128];
   tbm_bo bo[3] = {0,};
   const char *dir = "/tmp/dump";

   if (!mbuf) return;
   if (mbuf->secure)
     {
        BER("can't dump a secure buffer");
        return;
     }

   if (IS_RGB(mbuf->drmfmt))
     snprintf(path, sizeof(path), "%s/%s_%c%c%c%c_%dx%d_%03d.%s", dir, prefix,
              FOURCC_STR(mbuf->drmfmt), mbuf->pitches[0] / 4, mbuf->height,
              nth, raw?"raw":"png");
   else
     snprintf(path, sizeof(path), "%s/%s_%c%c%c%c_%dx%d_%03d.yuv", dir, prefix,
              FOURCC_STR(mbuf->drmfmt), mbuf->pitches[0], mbuf->height, nth);

   if (IS_RGB(mbuf->drmfmt))
     {
        if (raw)
          _dump_raw(path, mbuf->ptrs[0], mbuf->pitches[0] * mbuf->height, NULL, 0);
        else
          _dump_png(path, mbuf->ptrs[0], mbuf->pitches[0] / 4, mbuf->height);
     }
   else
     {
        int size;
        switch(mbuf->drmfmt)
          {
             case DRM_FORMAT_YVU420:
             case DRM_FORMAT_YUV420:
             case DRM_FORMAT_NV12:
             case DRM_FORMAT_NV21:
                size = mbuf->pitches[0] * mbuf->height * 1.5;
                break;
             case DRM_FORMAT_YUYV:
             case DRM_FORMAT_UYVY:
                size = mbuf->pitches[0] * mbuf->height * 2;
                break;
             default:
                size = tbm_bo_size(bo[0]);
                break;
          }
        if (!bo[1])
          _dump_raw(path, mbuf->ptrs[0], size, NULL, 0);
        else
          _dump_raw(path, mbuf->ptrs[0], mbuf->pitches[0] * mbuf->height,
                    mbuf->ptrs[1], mbuf->pitches[1] * mbuf->height);
     }

   BDB("dump %s", path);
}

int
e_devmgr_buffer_list_length(void)
{
   return eina_list_count(mbuf_lists);
}

void
e_devmgr_buffer_list_print(void)
{
   E_Devmgr_Buf *mbuf;
   Eina_List *l;

   INF("* Devicemgr Buffers:");
   INF("stamp\tsize\tformat\thandles\tpitches\toffsets\tcreator\tconverting\tshowing");
   EINA_LIST_FOREACH(mbuf_lists, l, mbuf)
     {
        INF("%d\t%dx%d\t%c%c%c%c\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d\t%s\t%d\t%d",
            mbuf->stamp, mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
            mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
            mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
            mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
            mbuf->func, MBUF_IS_CONVERTING(mbuf), mbuf->showing);
     }
}
