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
   TYPE_SHM,
   TYPE_TBM,
} E_Devmgr_Buf_Type;

typedef struct _E_Devmgr_Buf
{
   /* to manage lifecycle */
   uint ref_cnt;

   /* to check valid */
   uint stamp;
   char *func;

   /* to manage wl_resource */
   struct wl_resource *resource;
   struct wl_listener buffer_destroy_listener;
   Eina_Bool buffer_destroying;

   E_Devmgr_Buf_Type type;
   tbm_surface_h tbm_surface;

   /* pitch contains the full buffer width.
    * width indicates the content area width.
    */
   tbm_format tbmfmt;
   int width;
   int height;
   uint handles[4];
   uint pitches[4];
   uint offsets[4];
   int names[4];
   void *ptrs[4];

   /* for display on screen */
   uint fb_id;

   /* to avoid reading & write at same time */
   Eina_List *convert_info;
   Eina_Bool showing;

   Eina_List *free_funcs;

   /* for debugging */
   E_Comp_Wl_Buffer *comp_buffer;
   uint put_time;
} E_Devmgr_Buf;

E_Devmgr_Buf* _e_devmgr_buffer_create     (struct wl_resource *resource, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_create_hnd (uint handle, int width, int height, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_alloc      (int width, int height, tbm_format tbmfmt, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_ref        (E_Devmgr_Buf *mbuf, const char *func);
void          _e_devmgr_buffer_unref      (E_Devmgr_Buf *mbuf, const char *func);
Eina_Bool     _e_devmgr_buffer_valid      (E_Devmgr_Buf *mbuf, const char *func);

#define e_devmgr_buffer_create(r)         _e_devmgr_buffer_create(r,__FUNCTION__)
#define e_devmgr_buffer_create_hnd(d,w,h) _e_devmgr_buffer_create_hnd(d,w,h,__FUNCTION__)
#define e_devmgr_buffer_alloc(w,h,f)      _e_devmgr_buffer_alloc(w,h,f,__FUNCTION__)
#define e_devmgr_buffer_ref(b)            _e_devmgr_buffer_ref(b,__FUNCTION__)
#define e_devmgr_buffer_unref(b)          _e_devmgr_buffer_unref(b,__FUNCTION__)
#define MBUF_IS_VALID(b)                  _e_devmgr_buffer_valid(b,__FUNCTION__)
#define MSTAMP(b)                         ((b)?(b)->stamp:0)
#define MBUF_IS_CONVERTING(b)             (eina_list_count((b)->convert_info)?EINA_TRUE:EINA_FALSE)

typedef void (*MBuf_Free_Func) (E_Devmgr_Buf *mbuf, void *data);
void      e_devmgr_buffer_free_func_add(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data);
void      e_devmgr_buffer_free_func_del(E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data);

Eina_Bool e_devmgr_buffer_add_fb(E_Devmgr_Buf *mbuf);
void      e_devmgr_buffer_clear(E_Devmgr_Buf *mbuf);
Eina_Bool e_devmgr_buffer_copy(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf);
void      e_devmgr_buffer_convert(E_Devmgr_Buf *srcbuf, E_Devmgr_Buf *dstbuf,
                                  int sx, int sy, int sw, int sh,
                                  int dx, int dy, int dw, int dh,
                                  Eina_Bool over, int rotate, int hflip, int vflip);

/* utility */
typedef enum
{
   TYPE_NONE,
   TYPE_RGB,
   TYPE_YUV422,
   TYPE_YUV420,
} E_Devmgr_Buf_Color_Type;

E_Devmgr_Buf_Color_Type e_devmgr_buffer_color_type(tbm_format tbmfmt);
void e_devmgr_buffer_dump(E_Devmgr_Buf *mbuf, const char *prefix, int nth, Eina_Bool raw);
uint e_devmgr_buffer_get_mills(void);
int  e_devmgr_buffer_list_length(void);
void e_devmgr_buffer_list_print(void);

#endif
