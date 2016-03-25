#include "e.h"
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_input.h"

#include <Ecore_Drm.h>
static Eina_List *handlers = NULL;
static Eina_Bool remapped = EINA_FALSE;

static Eina_Bool
_cb_input_dev_add(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;
   Eina_List *devices, *l, *l2, *l3;
   Ecore_Drm_Device *dev;
   Ecore_Drm_Seat *seat;
   Ecore_Drm_Evdev *edev;
   Eina_Bool ret = EINA_FALSE;
   const char *name;

   int from_keycode = KEY_EXIT;
   int to_keycode = KEY_BACK;

   if (remapped) return ECORE_CALLBACK_PASS_ON;
   if (!(e = event)) return ECORE_CALLBACK_PASS_ON;
   if (!(e->clas == ECORE_DEVICE_CLASS_KEYBOARD)) return ECORE_CALLBACK_PASS_ON;
   if (!(e->name) || strcmp(e->name, "au0828 IR (Hauppauge HVR950Q)"))
     return ECORE_CALLBACK_PASS_ON;

   devices = eina_list_clone(ecore_drm_devices_get());
   EINA_LIST_FOREACH(devices, l, dev)
     {
        EINA_LIST_FOREACH(dev->seats, l2, seat)
          {
             EINA_LIST_FOREACH(ecore_drm_seat_evdev_list_get(seat), l3, edev)
               {
                  name = ecore_drm_evdev_name_get(edev);

                  if ((name) && (!strcmp(name, e->name)))
                    {
                       ret = ecore_drm_evdev_key_remap_enable(edev, EINA_TRUE);

                       if (!ret)
                         {
                            DBG("[DEVMGR] Failed to enable ecore_drm_evdev_key_remap !");
                            eina_list_free(devices);
                            return ECORE_CALLBACK_PASS_ON;
                         }

                       ret = ecore_drm_evdev_key_remap_set(edev, &from_keycode, &to_keycode, 1);

                       if (!ret)
                         {
                            DBG("[DEVMGR] Failed to enable ecore_drm_evdev_key_remap_set !");
                            eina_list_free(devices);
                            return ECORE_CALLBACK_PASS_ON;
                         }

                       remapped = EINA_TRUE;
                       eina_list_free(devices);
                       return ECORE_CALLBACK_PASS_ON;
                    }
               }
          }
     }
   eina_list_free(devices);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_cb_input_dev_del(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if(!(e = event)) return ECORE_CALLBACK_PASS_ON;

   if (e->name && !strcmp(e->name, "au0828 IR (Hauppauge HVR950Q)"))
     remapped = EINA_FALSE;

   return ECORE_CALLBACK_PASS_ON;
}

int
e_devicemgr_input_init(void)
{
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_ADD, _cb_input_dev_add, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_DEL, _cb_input_dev_del, NULL);

   return 1;
}

void
e_devicemgr_input_fini(void)
{
   E_FREE_LIST(handlers, ecore_event_handler_del);
}
