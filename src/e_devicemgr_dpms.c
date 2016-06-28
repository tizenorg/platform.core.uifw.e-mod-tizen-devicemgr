#include <e.h>
#include <Ecore_Drm.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_dpms.h"

typedef enum _E_Devicemgr_Dpms_Mode
{
   E_DEVICEMGR_DPMS_MODE_ON      = 0,
   E_DEVICEMGR_DPMS_MODE_STANDBY = 1,
   E_DEVICEMGR_DPMS_MODE_SUSPEND = 2,
   E_DEVICEMGR_DPMS_MODE_OFF     = 3
} E_Devicemgr_Dpms_Mode;

#define BUS "org.enlightenment.wm"
#define PATH "/org/enlightenment/wm"
#define INTERFACE "org.enlightenment.wm.dpms"

static Ecore_Drm_Output *dpms_output;
static unsigned int dpms_value;

static Eldbus_Connection *conn;
static Eldbus_Service_Interface *iface;

static Eldbus_Message *
_e_devicemgr_dpms_set_cb(const Eldbus_Service_Interface *iface, const Eldbus_Message *msg)
{
   Eldbus_Message *reply = eldbus_message_method_return_new(msg);
   unsigned int uint32 = -1;
   int result = -1;

   DBG("[devicemgr] got DPMS request");

   if (eldbus_message_arguments_get(msg, "u", &uint32) && uint32< 4)
     {
        Ecore_Drm_Device *dev;
        Ecore_Drm_Output *output;
        Eina_List *devs, *l, *ll;
        E_Zone *zone;
        Eina_List *zl;

        DBG("[devicemgr] DPMS value: %d", uint32);

        devs = eina_list_clone(ecore_drm_devices_get());
        EINA_LIST_FOREACH(devs, l, dev)
          EINA_LIST_FOREACH(dev->outputs, ll, output)
            {
               int x;
               ecore_drm_output_position_get(output, &x, NULL);

               EINA_LIST_FOREACH(e_comp->zones, zl, zone)
                 {
                    if (uint32 == E_DEVICEMGR_DPMS_MODE_ON)
                      e_zone_display_state_set(zone, E_ZONE_DISPLAY_STATE_ON);
                    else if (uint32 == E_DEVICEMGR_DPMS_MODE_OFF)
                      e_zone_display_state_set(zone, E_ZONE_DISPLAY_STATE_OFF);
                 }

               /* only for main output */
               if (x != 0)
                 continue;

               DBG("[devicemgr] set DPMS");

               dpms_output = output;
               dpms_value = uint32;
               ecore_drm_output_dpms_set(output, uint32);
            }

        result = uint32;
        if (devs) eina_list_free(devs);
     }

   eldbus_message_arguments_append(reply, "i", result);

   return reply;
}

static Eldbus_Message *
_e_devicemgr_dpms_get_cb(const Eldbus_Service_Interface *iface, const Eldbus_Message *msg)
{
   Eldbus_Message *reply = eldbus_message_method_return_new(msg);

   DBG("[devicemgr] got DPMS 'get' request");

   eldbus_message_arguments_append(reply, "i", dpms_value);

   return reply;
}

static const Eldbus_Method methods[] = {
   {"set", ELDBUS_ARGS({"u", "uint32"}), ELDBUS_ARGS({"i", "int32"}), _e_devicemgr_dpms_set_cb, 0},
   {"get", NULL, ELDBUS_ARGS({"i", "int32"}), _e_devicemgr_dpms_get_cb, 0},
   {}
};

static const Eldbus_Service_Interface_Desc iface_desc = {
   INTERFACE, methods, NULL, NULL, NULL, NULL
};

static void
_e_devicemgr_dpms_name_request_cb(void *data, const Eldbus_Message *msg, Eldbus_Pending *pending EINA_UNUSED)
{
   unsigned int reply;

   if (eldbus_message_error_get(msg, NULL, NULL))
     {
        printf("error on on_name_request\n");
        return;
     }

   if (!eldbus_message_arguments_get(msg, "u", &reply))
     {
        printf("error geting arguments on on_name_request\n");
        return;
     }
}

static Eina_Bool
_e_devicemgr_dpms_dbus_init(void *data)
{
   if (conn)
      return ECORE_CALLBACK_CANCEL;

   if (!conn)
     conn = eldbus_connection_get(ELDBUS_CONNECTION_TYPE_SYSTEM);

   if (!conn)
     {
        ERR("eldbus_connection_get fail..");
        ecore_timer_add(1.0, _e_devicemgr_dpms_dbus_init, NULL);
        return ECORE_CALLBACK_CANCEL;
     }

   INF("eldbus_connection_get success..");

   iface = eldbus_service_interface_register(conn, PATH, &iface_desc);
   EINA_SAFETY_ON_NULL_GOTO(iface, failed);

   eldbus_name_request(conn, BUS, ELDBUS_NAME_REQUEST_FLAG_DO_NOT_QUEUE,
                      _e_devicemgr_dpms_name_request_cb, NULL);

   return ECORE_CALLBACK_CANCEL;
failed:
   if (conn)
     {
        eldbus_name_release(conn, BUS, NULL, NULL);
        eldbus_connection_unref(conn);
        conn = NULL;
     }

   return ECORE_CALLBACK_CANCEL;
}

int
e_devicemgr_dpms_init(void)
{
   if (eldbus_init() == 0) return 0;

   _e_devicemgr_dpms_dbus_init(NULL);

   return 1;
}

void
e_devicemgr_dpms_fini(void)
{
   if (iface)
     {
        eldbus_service_interface_unregister(iface);
        iface = NULL;
     }
   if (conn)
     {
        eldbus_name_release(conn, BUS, NULL, NULL);
        eldbus_connection_unref(conn);
        conn = NULL;
     }

   eldbus_shutdown();
}

unsigned int
e_devicemgr_dpms_get(Ecore_Drm_Output *output)
{
   if (dpms_output == output)
     return dpms_value;

   return DRM_MODE_DPMS_ON;
}
