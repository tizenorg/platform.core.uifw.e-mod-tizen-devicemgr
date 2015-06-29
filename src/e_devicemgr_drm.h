#ifndef __E_DEVICEMGR_DRM_H__
#define __E_DEVICEMGR_DRM_H__

#include <drm_fourcc.h>
#include <tbm_bufmgr.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>

#define C(b,m)              (((b) >> (m)) & 0xFF)
#define B(c,s)              ((((unsigned int)(c)) & 0xff) << (s))
#define FOURCC(a,b,c,d)     (B(d,24) | B(c,16) | B(b,8) | B(a,0))
#define FOURCC_STR(id)      C(id,0), C(id,8), C(id,16), C(id,24)
#define FOURCC_ID(str)      FOURCC(((char*)str)[0],((char*)str)[1],((char*)str)[2],((char*)str)[3])

/* Specific format for S.LSI
 * 2x2 subsampled Cr:Cb plane 64x32 macroblocks
 */
#define DRM_FORMAT_NV12MT   fourcc_code('T', 'M', '1', '2')
#define IS_RGB(f)           ((f) == DRM_FORMAT_XRGB8888 || (f) == DRM_FORMAT_ARGB8888)

/* securezone memory */
#define TZMEM_IOC_GET_TZMEM 0xC00C5402
struct tzmem_get_region
{
   const char   *key;
   unsigned int size;
   int          fd;
};

extern int e_devmgr_drm_fd;
extern tbm_bufmgr e_devmgr_bufmgr;


typedef void (*Drm_Vblank_Func)(unsigned int sequence,
                                unsigned int tv_sec, unsigned int tv_usec, void *data);
typedef void (*Drm_Ipp_Func)(unsigned int prop_id, unsigned int *buf_idx,
                             unsigned int  tv_sec, unsigned int  tv_usec, void *data);

int e_devicemgr_drm_init(void);
void e_devicemgr_drm_fini(void);

int e_devicemgr_drm_vblank_handler_add(Drm_Vblank_Func func, void *data);
void e_devicemgr_drm_vblank_handler_del(Drm_Vblank_Func func, void *data);
int e_devicemgr_drm_ipp_handler_add(Drm_Ipp_Func func, void *data);
void e_devicemgr_drm_ipp_handler_del(Drm_Ipp_Func func, void *data);

int e_devicemgr_drm_set_property(unsigned int obj_id, unsigned int obj_type,
                                 const char *prop_name, unsigned int value);

int e_devicemgr_drm_ipp_set(struct drm_exynos_ipp_property *property);
Eina_Bool e_devicemgr_drm_ipp_queue(struct drm_exynos_ipp_queue_buf *buf);
Eina_Bool e_devicemgr_drm_ipp_cmd(struct drm_exynos_ipp_cmd_ctrl *ctrl);

Eina_Bool e_devicemgr_drm_get_cur_msc(int pipe, uint *msc);
Eina_Bool e_devicemgr_drm_wait_vblank(int pipe, uint *target_msc, void *data);

#endif
