#ifndef __E_DEVICEMGR_DRM_H__
#define __E_DEVICEMGR_DRM_H__

#include <drm_fourcc.h>
#include <tbm_bufmgr.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>
#include <tbm_surface.h>
#include <tbm_surface_internal.h>

#define C(b,m)              (((b) >> (m)) & 0xFF)
#define B(c,s)              ((((unsigned int)(c)) & 0xff) << (s))
#define FOURCC(a,b,c,d)     (B(d,24) | B(c,16) | B(b,8) | B(a,0))
#define FOURCC_STR(id)      C(id,0), C(id,8), C(id,16), C(id,24)
#define FOURCC_ID(str)      FOURCC(((char*)str)[0],((char*)str)[1],((char*)str)[2],((char*)str)[3])

#define IS_RGB(f)           ((f) == TBM_FORMAT_XRGB8888 || (f) == TBM_FORMAT_ARGB8888)
#define ROUNDUP(s,c)        (((s) + (c-1)) & ~(c-1))
#define ALIGN_TO_16B(x)     ((((x) + (1 <<  4) - 1) >>  4) <<  4)
#define ALIGN_TO_32B(x)     ((((x) + (1 <<  5) - 1) >>  5) <<  5)
#define ALIGN_TO_128B(x)    ((((x) + (1 <<  7) - 1) >>  7) <<  7)
#define ALIGN_TO_2KB(x)     ((((x) + (1 << 11) - 1) >> 11) << 11)
#define ALIGN_TO_8KB(x)     ((((x) + (1 << 13) - 1) >> 13) << 13)
#define ALIGN_TO_64KB(x)    ((((x) + (1 << 16) - 1) >> 16) << 16)

/* currently tbm_format is same with drm_format */
#define DRM_FORMAT(tbmfmt)  (tbmfmt)

/* securezone memory */
extern int e_devmgr_drm_fd;
extern tbm_bufmgr e_devmgr_bufmgr;

int e_devicemgr_drm_init(void);
void e_devicemgr_drm_fini(void);

#endif
