#define E_COMP_WL
#include <e.h>
#include <Ecore_Drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_drm.h"
#include "e_devicemgr_dpms.h"

int e_devmgr_drm_fd = -1;
tbm_bufmgr e_devmgr_bufmgr = NULL;

static int
_e_devicemgr_drm_fd_get(void)
{
   Eina_List *devs;
   Ecore_Drm_Device *dev;

   if (e_devmgr_drm_fd >= 0)
     return e_devmgr_drm_fd;

   devs = eina_list_clone(ecore_drm_devices_get());
   EINA_SAFETY_ON_NULL_RETURN_VAL(devs, -1);

   if ((dev = eina_list_nth(devs, 0)))
     {
        e_devmgr_drm_fd = ecore_drm_device_fd_get(dev);
        if (e_devmgr_drm_fd >= 0)
          {
             eina_list_free(devs);
             return e_devmgr_drm_fd;
          }
     }

   eina_list_free(devs);
   return -1;
}

int
e_devicemgr_drm_init(void)
{
   drmVersionPtr drm_info;

   if (_e_devicemgr_drm_fd_get() < 0)
      return 0;

   e_devmgr_bufmgr = tbm_bufmgr_init(e_devmgr_drm_fd);
   if (!e_devmgr_bufmgr)
     {
        ERR("bufmgr init failed");
        e_devmgr_drm_fd = -1;
        return 0;
     }

   drm_info = drmGetVersion(e_devmgr_drm_fd);
   DBG("drm name: %s", drm_info->name);

   if (drm_info->name && !strncmp (drm_info->name, "exynos", 6))
     {
        e_comp->wl_comp_data->available_hw_accel.underlay = EINA_TRUE;
        DBG("enable HW underlay");
        e_comp->wl_comp_data->available_hw_accel.scaler = EINA_TRUE;
        DBG("enable HW scaler");
     }

   drmFreeVersion(drm_info);

   return 1;
}

void
e_devicemgr_drm_fini(void)
{
   tbm_bufmgr_deinit(e_devmgr_bufmgr);
   e_devmgr_drm_fd = -1;
}
