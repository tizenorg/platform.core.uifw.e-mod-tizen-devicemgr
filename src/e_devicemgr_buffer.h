#ifndef __E_DEVICEMGR_BUFFER_H__
#define __E_DEVICEMGR_BUFFER_H__

#define E_COMP_WL
#include <e.h>
#include <e_comp_wl_tbm.h>
#include <wayland-tbm-server.h>
#include <tizen-extension-server-protocol.h>
#include "e_devicemgr_drm.h"
#include "e_devicemgr_privates.h"

typedef enum _E_Devmgr_Buf_Type
{
   TYPE_TBM,
   TYPE_BO,
   TYPE_SHM,
   TYPE_HND,
} E_Devmgr_Buf_Type;

typedef enum
{
   TYPE_NONE,
   TYPE_RGB,
   TYPE_YUV422,
   TYPE_YUV420,
} E_Devmgr_Buf_Color_Type;

typedef struct _E_Devmgr_Buf
{
   uint tbmfmt;

   /* pitch contains the full buffer width.
    * width indicates the content area width.
    */
   int width;
   int height;
   uint handles[4];
   uint pitches[4];
   uint offsets[4];

   void *ptrs[4];  /* user address */
   int names[4];   /* flink_id */

   E_Devmgr_Buf_Type type;
   union {
      struct wl_shm_buffer *shm_buffer;
      struct wl_resource *tbm_resource;
      uint handle;
      tbm_bo bo[4];
   } b;

   /* for tbm_buffer */
   struct wl_listener buffer_destroy_listener;
   Eina_Bool buffer_destroying;
   E_Comp_Wl_Buffer *comp_buffer;

   Eina_List *convert_info;
   Eina_Bool showing;         /* now showing or now waiting to show. */

   uint fb_id;      /* fb_id of mbuf */

   Eina_List *free_funcs;

   Eina_Bool secure;

   Eina_List *valid_link;   /* to check valid */
   uint stamp;
   uint ref_cnt;
   char *func;

   uint put_time;
} E_Devmgr_Buf;

E_Devmgr_Buf_Color_Type e_devmgr_buffer_color_type (unsigned int tbmfmt);

E_Devmgr_Buf* _e_devmgr_buffer_create    (struct wl_resource *tbm_resource, Eina_Bool secure, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_create_fb (struct wl_resource *tbm_resource, Eina_Bool secure, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_create_shm(struct wl_shm_buffer *shm_buffer, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_create_hnd(uint handle, int width, int height, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_alloc     (int width, int height, uint tbmfmt, Eina_Bool secure, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_alloc_fb  (int width, int height, Eina_Bool secure, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_ref    (E_Devmgr_Buf *mbuf, const char *func);
void          _e_devmgr_buffer_unref  (E_Devmgr_Buf *mbuf, const char *func);
Eina_Bool     _e_devmgr_buffer_valid  (E_Devmgr_Buf *mbuf, const char *func);
void          e_devmgr_buffer_clear   (E_Devmgr_Buf *mbuf);

typedef void (*MBuf_Free_Func) (E_Devmgr_Buf *mbuf, void *data);
void         e_devmgr_buffer_free_func_add  (E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data);
void         e_devmgr_buffer_free_func_del  (E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data);

#define e_devmgr_buffer_create(t,d)      _e_devmgr_buffer_create(t,d,__FUNCTION__)
#define e_devmgr_buffer_create_fb(t,d)   _e_devmgr_buffer_create_fb(t,d,__FUNCTION__)
#define e_devmgr_buffer_create_shm(s)  _e_devmgr_buffer_create_shm(s,__FUNCTION__)
#define e_devmgr_buffer_create_hnd(d,w,h)    _e_devmgr_buffer_create_hnd(d,w,h,__FUNCTION__)
#define e_devmgr_buffer_alloc(w,h,f,d)     _e_devmgr_buffer_alloc(w,h,f,d,__FUNCTION__)
#define e_devmgr_buffer_alloc_fb(w,h,d)  _e_devmgr_buffer_alloc_fb(w,h,d,__FUNCTION__)
#define e_devmgr_buffer_ref(b)    _e_devmgr_buffer_ref(b,__FUNCTION__)
#define e_devmgr_buffer_unref(b)  _e_devmgr_buffer_unref(b,__FUNCTION__)

#define MBUF_IS_VALID(b)       _e_devmgr_buffer_valid(b,__FUNCTION__)
#define MSTAMP(b)            ((b)?(b)->stamp:0)
#define MBUF_IS_CONVERTING(b)       (eina_list_count((b)->convert_info)?EINA_TRUE:EINA_FALSE)

uint e_devmgr_buffer_get_mills(void);
void e_devmgr_buffer_dump(E_Devmgr_Buf *mbuf, const char *prefix, int nth, Eina_Bool raw);
void e_devmgr_buffer_convert(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf,
                             int sx, int sy, int sw, int sh,
                             int dx, int dy, int dw, int dh,
                             Eina_Bool over, int rotate, int hflip, int vflip);

Eina_Bool e_devmgr_buffer_copy(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf);
int e_devmgr_buffer_list_length(void);
void e_devmgr_buffer_list_print(void);

#endif
