#include <e.h>
#include <Ecore_Drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>
#include <png.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_drm.h"
#include "e_devicemgr_buffer.h"

#define PNG_DEPTH 8

#define MBUF_RETURN_IF_FAIL(cond) \
   {if (!(cond)) { ERR("%d: '%s' failed. (%s)", mbuf->stamp, #cond, func); return; }}
#define MBUF_RETURN_VAL_IF_FAIL(cond, val) \
   {if (!(cond)) { ERR("%d: '%s' failed. (%s)", mbuf->stamp, #cond, func); return val; }}

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
   uint stamp;
   int i;

   EINA_SAFETY_ON_NULL_RETURN_VAL(tizen_buffer, NULL);

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
          }
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

   DBG("%d(%d) create: %s", mbuf->stamp, mbuf->ref_cnt, func);

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
     ERR("error: get fb_id %dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d)",
         mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
         mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
         mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
         mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2]);

   EINA_SAFETY_ON_FALSE_GOTO(mbuf->fb_id > 0, create_fail);

   DBG("%dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d): fb_id(%d)",
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
       mbuf->fb_id);

   return mbuf;
create_fail:
   e_devmgr_buffer_free(mbuf);
   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_create_ext(uint handle, int width, int height, uint drmfmt, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   uint stamp;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(handle > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(width > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(height > 0, NULL);
   if (!IS_RGB(drmfmt))
     {
        ERR("not supported format: %c%c%c%c", FOURCC_STR(drmfmt));
        return NULL;
     }

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, create_fail);

   mbuf->drmfmt = drmfmt;
   mbuf->width = width;
   mbuf->height = height;
   mbuf->pitches[0] = (width << 2);

   mbuf->type = TYPE_EXT;
   mbuf->handles[0] = handle;

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   stamp = e_devmgr_buffer_get_mills();
   while(_find_mbuf(stamp))
     stamp++;
   mbuf->stamp = stamp;

   mbuf->func = strdup(func);
   mbuf->ref_cnt = 1;

   DBG("%d(%d) create_ext: %s", mbuf->stamp, mbuf->ref_cnt, func);

   return mbuf;

create_fail:
   e_devmgr_buffer_free(mbuf);
   return NULL;
}

E_Devmgr_Buf*
_e_devmgr_buffer_alloc_fb(int width, int height, Eina_Bool secure, const char *func)
{
   E_Devmgr_Buf *mbuf = NULL;
   uint stamp;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(width > 0, NULL);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(height > 0, NULL);

   mbuf = calloc(1, sizeof(E_Devmgr_Buf));
   EINA_SAFETY_ON_FALSE_GOTO(mbuf != NULL, alloc_fail);

   mbuf->drmfmt = DRM_FORMAT_ARGB8888;
   mbuf->width = width;
   mbuf->height = height;
   mbuf->pitches[0] = ((width + 15) & (~0xF)) << 2;

   mbuf->type = TYPE_BO;
   if (!secure)
     mbuf->b.bo[0] = _e_devmgr_buf_normal_alloc(mbuf->pitches[0] * mbuf->height, TBM_BO_DEFAULT);
   else
     mbuf->b.bo[0] = _e_devmgr_buf_secure_alloc(mbuf->pitches[0] * mbuf->height, TBM_BO_DEFAULT);
   EINA_SAFETY_ON_FALSE_GOTO(mbuf->b.bo[0] != NULL, alloc_fail);

   mbuf->handles[0] = tbm_bo_get_handle(mbuf->b.bo[0], TBM_DEVICE_DEFAULT).u32;
   EINA_SAFETY_ON_FALSE_GOTO(mbuf->handles[0] > 0, alloc_fail);

   if (drmModeAddFB2(e_devmgr_drm_fd, mbuf->width, mbuf->height, mbuf->drmfmt,
                     mbuf->handles, mbuf->pitches, mbuf->offsets, &mbuf->fb_id, 0))
     ERR("error: get fb_id %dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d)",
         mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
         mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
         mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
         mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2]);
   EINA_SAFETY_ON_FALSE_GOTO(mbuf->fb_id > 0, alloc_fail);

   mbuf->secure = secure;

   mbuf_lists = eina_list_append(mbuf_lists, mbuf);

   stamp = e_devmgr_buffer_get_mills();
   while(_find_mbuf(stamp))
     stamp++;
   mbuf->stamp = stamp;

   mbuf->func = strdup(func);
   mbuf->ref_cnt = 1;

   DBG("%d(%d) alloc: %s", mbuf->stamp, mbuf->ref_cnt, func);
   DBG("%dx%d, %c%c%c%c, (%d,%d,%d), (%d,%d,%d), (%d,%d,%d): fb_id(%d)",
       mbuf->width, mbuf->height, FOURCC_STR(mbuf->drmfmt),
       mbuf->handles[0], mbuf->handles[1], mbuf->handles[2],
       mbuf->pitches[0], mbuf->pitches[1], mbuf->pitches[2],
       mbuf->offsets[0], mbuf->offsets[1], mbuf->offsets[2],
       mbuf->fb_id);

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
   DBG("%d(%d) ref: %s", mbuf->stamp, mbuf->ref_cnt, func);

   return mbuf;
}

void
_e_devmgr_buffer_unref(E_Devmgr_Buf *mbuf, const char *func)
{
   if (!mbuf)
     return;

   MBUF_RETURN_IF_FAIL(_e_devmgr_buffer_valid(mbuf, func));

   mbuf->ref_cnt--;
   DBG("%d(%d) unref: %s", mbuf->stamp, mbuf->ref_cnt, func);

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
        DBG("mbuf(%d) fb_id(%d) removed. ", mbuf->stamp, mbuf->fb_id);
        drmModeRmFB(e_devmgr_drm_fd, mbuf->fb_id);
     }

   for (i = 0; i < 4; i++)
     {
        if (mbuf->type == TYPE_BO && mbuf->b.bo[i])
          tbm_bo_unref(mbuf->b.bo[i]);
     }

   mbuf_lists = eina_list_remove(mbuf_lists, mbuf);

   DBG("%d freed: %s", mbuf->stamp, func);

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
e_devmgr_buffer_dump(E_Devmgr_Buf *mbuf, const char *file, Eina_Bool raw)
{
   void *ptr;
   char path[128];
   tbm_bo bo[3];

   if (!mbuf) return;

   if (IS_RGB(mbuf->drmfmt))
     snprintf(path, sizeof(path), "/tmp/%s.%s", file, raw?"raw":"png");
   else
     snprintf(path, sizeof(path), "/tmp/%s.yuv", file);

   if (mbuf->type == TYPE_TB)
     memcpy(bo, mbuf->b.tizen_buffer->bo, sizeof(tbm_bo)*3);
   else if (mbuf->type == TYPE_BO)
     memcpy(bo, mbuf->b.bo, sizeof(tbm_bo)*3);
   else
     {
        DBG("not support");
        return;
     }

   ptr = tbm_bo_get_handle(bo[0], TBM_DEVICE_CPU).ptr;
   if (IS_RGB(mbuf->drmfmt))
     {
        if (raw)
          _dump_raw(path, ptr, mbuf->width * mbuf->height * 4, NULL, 0);
        else
          _dump_png(path, ptr, mbuf->width, mbuf->height);
     }
   else
     {
        if (!bo[1])
          _dump_raw(path, ptr, mbuf->pitches[0] * mbuf->height, NULL, 0);
        else
          _dump_raw(path, ptr, mbuf->pitches[0] * mbuf->height,
                    tbm_bo_get_handle(bo[1], TBM_DEVICE_CPU).ptr,
                    mbuf->pitches[1] * mbuf->height);
     }

   DBG("dump %s", path);
}
