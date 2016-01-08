#include "e_devicemgr_device.h"
#include <tizen-extension-server-protocol.h>

static Eina_List *handlers = NULL;

static void
_e_device_mgr_device_cb_axes_select(struct wl_client *client, struct wl_resource *resource, struct wl_array *axes)
{
   return;
}

static void
_e_devicemgr_device_cb_release(struct wl_client *client, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static const struct tizen_input_device_interface _e_devicemgr_device_interface =
{
   _e_device_mgr_device_cb_axes_select,
   _e_devicemgr_device_cb_release,
};

static void
_e_devicemgr_del_device(const char *name, const char *identifier, const char *seatname, Ecore_Drm_Seat_Capabilities cap)
{
   E_Comp_Wl_Input_Device *dev;
   struct wl_client *wc;
   Eina_List *l, *ll, *lll;
   struct wl_resource *res, *seat_res, *dev_mgr_res;
   uint32_t serial;

   if (!e_comp) return;
   if (!e_comp_wl) return;
   if (!e_comp_wl->wl.disp);

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
     {
        if ((dev->capability == cap) && (!strcmp(dev->name, name))  && (!strcmp(dev->identifier, identifier)))
          break;
     }
   if (!dev) return;

   if (dev->name) eina_stringshare_del(dev->name);
   if (dev->identifier) eina_stringshare_del(dev->identifier);

   serial = wl_display_next_serial(e_comp_wl->wl.disp);

   /* TODO: find the seat corresponding to event */
   EINA_LIST_FOREACH(e_comp_wl->seat.resources, l, seat_res)
     {
        wc = wl_resource_get_client(seat_res);
        EINA_LIST_FOREACH(e_comp_wl->input_device_manager.resources, ll, dev_mgr_res)
          {
             if (wl_resource_get_client(dev_mgr_res) != wc) continue;
             EINA_LIST_FOREACH(dev->resources, lll, res)
               {
                  if (wl_resource_get_client(res) != wc) continue;
                  tizen_input_device_manager_send_device_remove(dev_mgr_res, serial, dev->identifier, res, seat_res);
               }
          }
     }

   EINA_LIST_FREE(dev->resources, res)
     wl_resource_destroy(res);

   e_comp_wl->input_device_manager.device_list = eina_list_remove(e_comp_wl->input_device_manager.device_list, dev);

   free(dev);
}

static void
_e_devicemgr_device_cb_device_unbind(struct wl_resource *resource)
{
   E_Comp_Wl_Input_Device *dev;

   if (!(dev = wl_resource_get_user_data(resource))) return;

   dev->resources = eina_list_remove(dev->resources, resource);
}

static void
_e_devicemgr_add_device(const char *name, const char *identifier, const char *seatname, Ecore_Drm_Seat_Capabilities cap)
{
   E_Comp_Wl_Input_Device *dev;
   struct wl_client *wc;
   Eina_List *l, *ll;
   struct wl_resource *res, *seat_res, *dev_mgr_res;
   uint32_t serial;
   struct wl_array axes;

   if (!e_comp) return;
   if (!e_comp_wl) return;
   if (!e_comp_wl->wl.disp) return;

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
     {
        if ((dev->capability == cap) && (!strcmp(dev->identifier, identifier)))
          return;
     }

   if (!(dev = E_NEW(E_Comp_Wl_Input_Device, 1))) return;
   dev->name = eina_stringshare_add(name);
   dev->identifier = eina_stringshare_add(identifier);
   dev->capability = cap;

   wl_array_init(&axes);

   /* TODO: find the seat corresponding to event */
   serial = wl_display_next_serial(e_comp_wl->wl.disp);

   EINA_LIST_FOREACH(e_comp_wl->seat.resources, l, seat_res)
     {
        wc = wl_resource_get_client(seat_res);
        res = wl_resource_create(wc, &tizen_input_device_interface, 1, 0);
        if (!res)
          {
             ERR("Could not create tizen_input_device resource");
             return;
          }

        dev->resources = eina_list_append(dev->resources, res);
        wl_resource_set_implementation(res, &_e_devicemgr_device_interface, dev,
                                      _e_devicemgr_device_cb_device_unbind);

        EINA_LIST_FOREACH(e_comp_wl->input_device_manager.resources, ll, dev_mgr_res)
          {
             if (wl_resource_get_client(dev_mgr_res) != wc) continue;
             tizen_input_device_manager_send_device_add(dev_mgr_res, serial, dev->identifier, res, seat_res);
          }
        tizen_input_device_send_device_info(res, dev->capability, TIZEN_INPUT_DEVICE_SUBCLASS_NONE, &axes);
     }

   e_comp_wl->input_device_manager.device_list = eina_list_append(e_comp_wl->input_device_manager.device_list, dev);
}

static Eina_Bool
_cb_device_add(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if (!(e = event)) return ECORE_CALLBACK_PASS_ON;

   if (e->caps & EVDEV_SEAT_POINTER)
     _e_devicemgr_add_device(e->name, e->identifier, e->seatname, EVDEV_SEAT_POINTER);
   if (e->caps & EVDEV_SEAT_KEYBOARD)
     _e_devicemgr_add_device(e->name, e->identifier, e->seatname, EVDEV_SEAT_KEYBOARD);
   if (e->caps & EVDEV_SEAT_TOUCH)
     _e_devicemgr_add_device(e->name, e->identifier, e->seatname, EVDEV_SEAT_TOUCH);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_cb_device_del(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if(!(e = event)) return ECORE_CALLBACK_PASS_ON;

   if (e->caps & EVDEV_SEAT_POINTER)
     _e_devicemgr_del_device(e->name, e->identifier, e->seatname, EVDEV_SEAT_POINTER);
   if (e->caps & EVDEV_SEAT_KEYBOARD)
     _e_devicemgr_del_device(e->name, e->identifier, e->seatname, EVDEV_SEAT_KEYBOARD);
   if (e->caps & EVDEV_SEAT_TOUCH)
     _e_devicemgr_del_device(e->name, e->identifier, e->seatname, EVDEV_SEAT_TOUCH);

   return ECORE_CALLBACK_PASS_ON;
}

static void
_e_devicemgr_device_mgr_cb_unbind(struct wl_resource *resource)
{
   if(!e_comp_wl) return;

   e_comp_wl->input_device_manager.resources = eina_list_remove(e_comp_wl->input_device_manager.resources, resource);
}

static void
_e_devicemgr_device_mgr_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res, *seat_res, *device_res;
   Eina_List *l;
   uint32_t serial;
   E_Comp_Wl_Input_Device *dev;
   struct wl_array axes;

   if (!e_comp_wl) return;
   if (!e_comp_wl->wl.disp) return;

   if (!(res = wl_resource_create(client, &tizen_input_device_manager_interface, MIN(version, 1), id)))
     {
        ERR("Could not create tizen_devices_interface resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   e_comp_wl->input_device_manager.resources = eina_list_append(e_comp_wl->input_device_manager.resources, res);

   wl_resource_set_implementation(res, NULL, NULL,
                                 _e_devicemgr_device_mgr_cb_unbind);

   EINA_LIST_FOREACH(e_comp_wl->seat.resources, l, seat_res)
     if (wl_resource_get_client(seat_res) == client) break;

   if (!seat_res)
     {
        ERR("Could not find seat resource bound to the tizen_input_device_manager");
        return;
     }

   wl_array_init(&axes);
   serial = wl_display_next_serial(e_comp_wl->wl.disp);

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
     {
        device_res = wl_resource_create(client, &tizen_input_device_interface, 1, 0);
        if (!device_res)
          {
             ERR("Could not create tizen_input_device resource: %m");
             return;
          }

        dev->resources = eina_list_append(dev->resources, device_res);

        wl_resource_set_implementation(device_res, &_e_devicemgr_device_interface, dev,
                                      _e_devicemgr_device_cb_device_unbind);

        tizen_input_device_manager_send_device_add(res, serial, dev->identifier, device_res, seat_res);
        tizen_input_device_send_device_info(device_res, dev->capability, TIZEN_INPUT_DEVICE_SUBCLASS_NONE, &axes);
     }
}

int
e_devicemgr_device_init(void)
{
   if (!e_comp) return 0;
   if (!e_comp_wl) return 0;
   if (!e_comp_wl->wl.disp) return 0;

   /* try to add tizen_input_device_manager to wayland globals */
   e_comp_wl->input_device_manager.global = wl_global_create(e_comp_wl->wl.disp, &tizen_input_device_manager_interface, 1,
                         NULL, _e_devicemgr_device_mgr_cb_bind);
   if (!e_comp_wl->input_device_manager.global)
     {
        ERR("Could not add tizen_input_device_manager to wayland globals");
        return 0;
     }
   e_comp_wl->input_device_manager.resources = NULL;
   e_comp_wl->input_device_manager.device_list = NULL;

   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_ADD, _cb_device_add, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_DEL, _cb_device_del, NULL);

   return 1;
}

void
e_devicemgr_device_fini(void)
{
   E_FREE_LIST(handlers, ecore_event_handler_del);
}
