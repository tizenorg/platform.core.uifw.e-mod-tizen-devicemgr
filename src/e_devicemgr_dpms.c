#include <e.h>
#include <Ecore_Drm.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_dpms.h"

#define BUS "org.enlightenment.wm"
#define PATH "/org/enlightenment/wm"
#define INTERFACE "org.enlightenment.wm.dpms"

static unsigned int dpms_value;

static Eldbus_Connection *conn;
static Eldbus_Service_Interface *iface;

static Eldbus_Message *
_e_devicemgr_dpms_dpms_set_cb(const Eldbus_Service_Interface *iface, const Eldbus_Message *msg)
{
   Eldbus_Message *reply = eldbus_message_method_return_new(msg);
   unsigned int uint32 = -1;
   int result = -1;

   DBG("[devicemgr] got DPMS request");

   if (eldbus_message_arguments_get(msg, "u", &uint32) && uint32< 4)
     {
        Ecore_Drm_Device *dev;
        Ecore_Drm_Output *output;
        Eina_List *devs = ecore_drm_devices_get();
        Eina_List *l, *ll;

        DBG("[devicemgr] DPMS value: %d", uint32);

        EINA_LIST_FOREACH(devs, l, dev)
          EINA_LIST_FOREACH(dev->outputs, ll, output)
            {
               int x;
               ecore_drm_output_position_get(output, &x, NULL);

               /* only for main output */
               if (x != 0)
                 continue;

               DBG("[devicemgr] set DPMS");

               dpms_value = uint32;
               ecore_drm_output_dpms_set(output, uint32);
               ecore_evas_manual_render_set(e_comp->ee,
                                            (uint32 > 0) ? EINA_TRUE : EINA_FALSE);
            }

        result = uint32;
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
   {"set", ELDBUS_ARGS({"u", "uint32"}), ELDBUS_ARGS({"i", "int32"}), _e_devicemgr_dpms_dpms_set_cb, 0},
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

int
e_devicemgr_dpms_init(void)
{
   if (eldbus_init() == 0) return 0;

   conn = eldbus_connection_get(ELDBUS_CONNECTION_TYPE_SYSTEM);
   EINA_SAFETY_ON_NULL_GOTO(conn, failed);

   iface = eldbus_service_interface_register(conn, PATH, &iface_desc);
   EINA_SAFETY_ON_NULL_GOTO(iface, failed);

   eldbus_name_request(conn, BUS, ELDBUS_NAME_REQUEST_FLAG_DO_NOT_QUEUE,
                      _e_devicemgr_dpms_name_request_cb, NULL);

   return 1;

failed:
   e_devicemgr_dpms_fini();

   return 0;
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
        eldbus_connection_unref(conn);
        conn = NULL;
     }

   eldbus_shutdown();
}
