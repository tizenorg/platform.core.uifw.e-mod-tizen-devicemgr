#include "e.h"
#include "eina_log.h"
#include "e_mod_main.h"
#include "e_devicemgr_input.h"
#include "e_devicemgr_output.h"
#include "e_devicemgr_scale.h"
#ifdef HAVE_WAYLAND_ONLY
#include "e_devicemgr_dpms.h"
#include "e_devicemgr_screenshooter.h"
#include "e_devicemgr_video.h"
#include "e_devicemgr_tdm.h"
#include "e_devicemgr_embedded_compositor.h"
#include "e_devicemgr_device.h"
#endif
#include "e_devicemgr_privates.h"

int _log_dom = -1;

/* this is needed to advertise a label for the module IN the code (not just
 * the .desktop file) but more specifically the api version it was compiled
 * for so E can skip modules that are compiled for an incorrect API version
 * safely) */
EAPI E_Module_Api e_modapi =
{
   E_MODULE_API_VERSION,
   "DeviceMgr Module of Window Manager"
};

EAPI void *
e_modapi_init(E_Module *m)
{
   if (!eina_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ eina_init()..!\n", __FUNCTION__);
        return NULL;
     }

   _log_dom = eina_log_domain_register("e-devicemgr", EINA_COLOR_BLUE);
   if (_log_dom < 0)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ eina_log_domain_register()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_output_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_output_init()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_input_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_input_init()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_scale_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_scale_init()..!\n", __FUNCTION__);
        return NULL;
     }

#ifdef HAVE_WAYLAND_ONLY
   if (!e_devicemgr_dpms_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_dpms_init()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_tdm_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_tdm_init()..!\n", __FUNCTION__);
        return NULL;
     }

   const char *engine_name = ecore_evas_engine_name_get(e_comp->ee);
   if (!strncmp(engine_name, "drm", 3) || !strncmp(engine_name, "gl_drm", 6))
     if (!e_devicemgr_screenshooter_init())
       {
          SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_screenshooter_init()..!\n", __FUNCTION__);
          return NULL;
       }

   if (!e_devicemgr_video_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_video_init()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_embedded_compositor_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_embedded_compositor_init()..!\n", __FUNCTION__);
        return NULL;
     }

   if (!e_devicemgr_device_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ e_devicemgr_device_init()..!\n", __FUNCTION__);
        return NULL;
     }
#endif

   return m;
}

EAPI int
e_modapi_shutdown(E_Module *m EINA_UNUSED)
{
#ifdef HAVE_WAYLAND_ONLY
   e_devicemgr_dpms_fini();
   e_devicemgr_screenshooter_fini();
   e_devicemgr_video_fini();
   e_devicemgr_tdm_fini();
   e_devicemgr_embedded_compositor_fini();
   e_devicemgr_device_fini();
#endif
   e_devicemgr_scale_fini();
   e_devicemgr_input_fini();
   e_devicemgr_output_fini();

   eina_log_domain_unregister(_log_dom);
   eina_shutdown();

   return 1;
}

EAPI int
e_modapi_save(E_Module *m EINA_UNUSED)
{
   /* Do Something */
   return 1;
}
