#include "e.h"
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_input.h"
#include "e_devicemgr_output.h"
#include "e_devicemgr_scale.h"

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

   return m;
}

EAPI int
e_modapi_shutdown(E_Module *m EINA_UNUSED)
{
   e_devicemgr_scale_fini();
   e_devicemgr_input_fini();
   e_devicemgr_output_fini();

   return 1;
}

EAPI int
e_modapi_save(E_Module *m EINA_UNUSED)
{
   /* Do Something */
   return 1;
}
