#ifndef __E_DEVICEMGR_BUFFER_H__
#define __E_DEVICEMGR_BUFFER_H__

#define E_COMP_WL
#include <e.h>
#include <e_drm_buffer_pool.h>
#include <e_drm_buffer_pool_server_protocol.h>
#include "e_devicemgr_drm.h"
#include "e_devicemgr_privates.h"

typedef struct _Tizen_Buffer
{
   E_Drm_Buffer *drm_buffer;
   tbm_bo bo[3];
   uint name[3];

   /* will set when attached */
   E_Comp_Wl_Buffer *buffer;
} Tizen_Buffer;

typedef enum _E_Devmgr_Buf_Type
{
   TYPE_TB,
   TYPE_BO,
   TYPE_EXT,
} E_Devmgr_Buf_Type;

typedef enum
{
   TYPE_NONE,
   TYPE_RGB,
   TYPE_YUV444,
   TYPE_YUV422,
   TYPE_YUV420,
} E_Devmgr_Buf_Color_Type;

typedef struct _E_Devmgr_Buf
{
   uint drmfmt;
   int width;
   int height;
   uint handles[4];
   uint pitches[4];
   uint offsets[4];
   uint lengths[4];
   uint size;

   E_Devmgr_Buf_Type type;
   union {
      /* in case of surface's buffer */
      Tizen_Buffer *tizen_buffer;
      /* in case of converting */
      tbm_bo bo[4];
   } b;
   /* for tizen_buffer */
   struct wl_listener buffer_destroy_listener;

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

E_Devmgr_Buf_Color_Type e_devmgr_buffer_color_type (unsigned int drmfmt);

E_Devmgr_Buf* _e_devmgr_buffer_create    (Tizen_Buffer *tizen_buffer, Eina_Bool secure, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_create_fb (Tizen_Buffer *tizen_buffer, Eina_Bool secure, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_create_ext(uint handle, int width, int height, uint drmfmt, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_alloc_fb  (int width, int height, Eina_Bool secure, const char *func);
E_Devmgr_Buf* _e_devmgr_buffer_ref    (E_Devmgr_Buf *mbuf, const char *func);
void          _e_devmgr_buffer_unref  (E_Devmgr_Buf *mbuf, const char *func);
void          _e_devmgr_buffer_free   (E_Devmgr_Buf *mbuf, const char *func);
Eina_Bool     _e_devmgr_buffer_valid  (E_Devmgr_Buf *mbuf, const char *func);
void          e_devmgr_buffer_clear   (E_Devmgr_Buf *mbuf);

typedef void (*MBuf_Free_Func) (E_Devmgr_Buf *mbuf, void *data);
void         e_devmgr_buffer_free_func_add  (E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data);
void         e_devmgr_buffer_free_func_del  (E_Devmgr_Buf *mbuf, MBuf_Free_Func func, void *data);

#define e_devmgr_buffer_create(t,d)      _e_devmgr_buffer_create(t,d,__FUNCTION__)
#define e_devmgr_buffer_create_fb(t,d)   _e_devmgr_buffer_create_fb(t,d,__FUNCTION__)
#define e_devmgr_buffer_create_ext(d,w,h,f)    _e_devmgr_buffer_create_ext(d,w,h,f,__FUNCTION__)
#define e_devmgr_buffer_alloc_fb(w,h,d)  _e_devmgr_buffer_alloc_fb(w,h,d,__FUNCTION__)
#define e_devmgr_buffer_ref(b)    _e_devmgr_buffer_ref(b,__FUNCTION__)
#define e_devmgr_buffer_unref(b)  _e_devmgr_buffer_unref(b,__FUNCTION__)
#define e_devmgr_buffer_free(b)   _e_devmgr_buffer_free(b,__FUNCTION__)
#define MBUF_IS_VALID(b)       _e_devmgr_buffer_valid(b,__FUNCTION__)
#define MSTAMP(b)            ((b)?(b)->stamp:0)
#define MBUF_IS_CONVERTING(b)       (eina_list_nth((b)->convert_info, 0)?EINA_TRUE:EINA_FALSE)

uint e_devmgr_buffer_get_mills(void);
void e_devmgr_buffer_dump(E_Devmgr_Buf *mbuf, const char *file, Eina_Bool raw);

#endif
