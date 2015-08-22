#ifndef __E_DEVICEMGR_CONVERTER_H__
#define __E_DEVICEMGR_CONVERTER_H__

#include "e_devicemgr_buffer.h"

typedef struct _E_Devmgr_Cvt E_Devmgr_Cvt;

typedef struct _E_Devmgr_CvtProp
{
    uint drmfmt;

    int width;
    int height;
    Eina_Rectangle crop;

    int degree;
    Eina_Bool vflip;
    Eina_Bool hflip;
    Eina_Bool secure;
    int csc_range;
} E_Devmgr_Cvt_Prop;

Eina_Bool     e_devmgr_cvt_ensure_size (E_Devmgr_Cvt_Prop *src, E_Devmgr_Cvt_Prop *dst);

E_Devmgr_Cvt* e_devmgr_cvt_create  (void);
void          e_devmgr_cvt_destroy (E_Devmgr_Cvt *cvt);
Eina_Bool     e_devmgr_cvt_pause   (E_Devmgr_Cvt *cvt);
void          e_devmgr_cvt_debug   (E_Devmgr_Cvt *cvt, Eina_Bool enable);

Eina_Bool     e_devmgr_cvt_property_set (E_Devmgr_Cvt *cvt, E_Devmgr_Cvt_Prop *src, E_Devmgr_Cvt_Prop *dst);
void          e_devmgr_cvt_property_get (E_Devmgr_Cvt *cvt, E_Devmgr_Cvt_Prop *src, E_Devmgr_Cvt_Prop *dst);
Eina_Bool     e_devmgr_cvt_convert      (E_Devmgr_Cvt *cvt, E_Devmgr_Buf *src, E_Devmgr_Buf *dst);

typedef void (*CvtFunc) (E_Devmgr_Cvt *cvt, E_Devmgr_Buf *src, E_Devmgr_Buf *dst, void *cvt_data);
Eina_Bool     e_devmgr_cvt_cb_add (E_Devmgr_Cvt *cvt, CvtFunc func, void *data);
void          e_devmgr_cvt_cb_del (E_Devmgr_Cvt *cvt, CvtFunc func, void *data);

#endif  /* __E_DEVICEMGR_CONVERTER_H__ */
