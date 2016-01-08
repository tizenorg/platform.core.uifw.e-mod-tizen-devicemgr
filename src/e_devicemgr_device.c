#include "e_devicemgr_device.h"
#include <tizen-extension-server-protocol.h>

static Eina_List *handlers = NULL;

static void
_e_devicemgr_del_device(const char *name, const char *identifier, Ecore_Drm_Seat_Capabilities cap)
{
   E_Comp_Wl_Input_Device *dev;
   Eina_List *l, *ll;
   struct wl_resource *res;
   struct wl_resource *seat_res;
   E_Comp_Data *cdata;

   if (!e_comp) return;
   if (!(cdata = e_comp->wl_comp_data)) return;

   EINA_LIST_FOREACH(cdata->input_device_mgr.device_list, l, dev)
     {
        if ((dev->capability == cap) && (!strcmp(dev->name, name))  && (!strcmp(dev->identifier, identifier)))
          break;
     }
   if (!dev) return;

   if (dev->name) eina_stringshare_del(dev->name);
   if (dev->identifier) eina_stringshare_del(dev->identifier);

   cdata->input_device_mgr.device_list = eina_list_remove_list(cdata->input_device_mgr.device_list, l);
   free(dev);

   EINA_LIST_FOREACH(cdata->input_device_mgr.resources, l, res)
     {
        seat_res = NULL;
        EINA_LIST_FOREACH(cdata->seat.resources, ll, seat_res)
          if (wl_resource_get_client(seat_res) == wl_resource_get_client(res))
            break;
        if (!seat_res) continue;

        if (cap & EVDEV_SEAT_POINTER)
          tizen_input_device_mgr_send_device_remove(res, name, identifier, TIZEN_INPUT_DEVICE_MGR_CAPABILITY_POINTER, seat_res);
        if (cap & EVDEV_SEAT_KEYBOARD)
          tizen_input_device_mgr_send_device_remove(res, name, identifier, TIZEN_INPUT_DEVICE_MGR_CAPABILITY_KEYBOARD, seat_res);
        if (cap & EVDEV_SEAT_TOUCH)
          tizen_input_device_mgr_send_device_remove(res, name, identifier, TIZEN_INPUT_DEVICE_MGR_CAPABILITY_TOUCH, seat_res);
     }
}

static void
_e_devicemgr_add_device(const char *name, const char *identifier, Ecore_Drm_Seat_Capabilities cap)
{
   E_Comp_Wl_Input_Device *dev;
   Eina_List *l, *ll;
   struct wl_resource *res;
   struct wl_resource *seat_res;
   E_Comp_Data *cdata;

   if (!e_comp) return;
   if (!(cdata = e_comp->wl_comp_data)) return;

   EINA_LIST_FOREACH(cdata->input_device_mgr.device_list, l, dev)
     {
        if ((dev->capability == cap) && (!strcmp(dev->identifier, identifier)))
          return;
     }

   if (!(dev = E_NEW(E_Comp_Wl_Input_Device, 1))) return;
   dev->name = eina_stringshare_add(name);
   dev->identifier = eina_stringshare_add(identifier);
   dev->capability = cap;
   cdata->input_device_mgr.device_list = eina_list_append(cdata->input_device_mgr.device_list, dev);

   EINA_LIST_FOREACH(cdata->input_device_mgr.resources, l, res)
     {
        seat_res = NULL;
        EINA_LIST_FOREACH(cdata->seat.resources, ll, seat_res)
          if (wl_resource_get_client(seat_res) == wl_resource_get_client(res))
            break;
        if (!seat_res) continue;

        if (cap & EVDEV_SEAT_POINTER)
          tizen_input_device_mgr_send_device_add(res, name, identifier, TIZEN_INPUT_DEVICE_MGR_CAPABILITY_POINTER, seat_res);
        if (cap & EVDEV_SEAT_KEYBOARD)
          tizen_input_device_mgr_send_device_add(res, name, identifier, TIZEN_INPUT_DEVICE_MGR_CAPABILITY_KEYBOARD, seat_res);
        if (cap & EVDEV_SEAT_TOUCH)
          tizen_input_device_mgr_send_device_add(res, name, identifier, TIZEN_INPUT_DEVICE_MGR_CAPABILITY_TOUCH, seat_res);
     }
}

static Eina_Bool
_cb_device_add(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if (!(e = event)) return ECORE_CALLBACK_PASS_ON;

   if (e->caps & EVDEV_SEAT_POINTER)
     _e_devicemgr_add_device(e->name, e->identifier, EVDEV_SEAT_POINTER);
   if (e->caps & EVDEV_SEAT_KEYBOARD)
     _e_devicemgr_add_device(e->name, e->identifier, EVDEV_SEAT_KEYBOARD);
   if (e->caps & EVDEV_SEAT_TOUCH)
     _e_devicemgr_add_device(e->name, e->identifier, EVDEV_SEAT_TOUCH);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_cb_device_del(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Event_Device_Info *e;

   if(!(e = event)) return ECORE_CALLBACK_PASS_ON;

   if (e->caps & EVDEV_SEAT_POINTER)
     _e_devicemgr_del_device(e->name, e->identifier, EVDEV_SEAT_POINTER);
   if (e->caps & EVDEV_SEAT_KEYBOARD)
     _e_devicemgr_del_device(e->name, e->identifier, EVDEV_SEAT_KEYBOARD);
   if (e->caps & EVDEV_SEAT_TOUCH)
     _e_devicemgr_del_device(e->name, e->identifier, EVDEV_SEAT_TOUCH);

   return ECORE_CALLBACK_PASS_ON;
}

static void
_e_devicemgr_device_cb_destroy(struct wl_client *client, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static const struct tizen_input_device_interface _e_devicemgr_device_interface =
{
   _e_devicemgr_device_cb_destroy,
};

static void
_e_devicemgr_device_cb_device_unbind(struct wl_resource *resource)
{
   E_Comp_Wl_Input_Device *dev;

   /* get compositor data */
   if (!(dev = wl_resource_get_user_data(resource))) return;

   dev->resources = eina_list_remove(dev->resources, resource);
}

static void
_e_devicemgr_device_mgr_cb_devices_info_get(struct wl_client *client, struct wl_resource *resource,
                                                                            struct wl_resource *seat_resource, uint32_t capabilities)
{
   E_Comp_Data *cdata;
   E_Comp_Wl_Input_Device *dev;
   struct wl_resource *res;
   Eina_List *l, *ll, *lll;
   struct wl_resource *seat_res;

   if (!(cdata = wl_resource_get_user_data(resource))) return;

   EINA_LIST_FOREACH(cdata->input_device_mgr.device_list, l, dev)
     EINA_LIST_FOREACH(cdata->input_device_mgr.resources, ll, res)
       {
         if (wl_resource_get_client(res) != client) continue;

          seat_res = NULL;
          EINA_LIST_FOREACH(cdata->seat.resources, lll, seat_res)
            if (wl_resource_get_client(seat_res) == client)
              break;
          if (!seat_res) continue;

          if (dev->capability & capabilities)
            tizen_input_device_mgr_send_device_add(res, dev->name, dev->identifier, dev->capability, seat_res);
       }
}

static void
_e_devicemgr_device_mgr_cb_device_get(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                                                            const char *identifier, uint32_t capability, struct wl_resource *seat)
{
   int version = wl_resource_get_version(resource);
   struct wl_resource *res;
   E_Comp_Data *cdata;
   E_Comp_Wl_Input_Device *dev = NULL;
   Eina_List *l;

   if (!(cdata = wl_resource_get_user_data(resource))) return;

   res = wl_resource_create(client, &tizen_input_device_interface, version, id);
   if (!res)
     {
        ERR("Could not create device on devices: %m");
        wl_client_post_no_memory(client);
        return;
     }

   EINA_LIST_FOREACH(cdata->input_device_mgr.device_list, l, dev)
     {
        if ((dev->capability == capability) && (!strcmp(dev->identifier, identifier)))
          break;
     }

   if (!dev) return;
   dev->resources = eina_list_append(dev->resources, res);
   wl_resource_set_implementation(res, &_e_devicemgr_device_interface, dev,
                                 _e_devicemgr_device_cb_device_unbind);
}

static const struct tizen_input_device_mgr_interface _e_devicemgr_device_mgr_interface =
{
   _e_devicemgr_device_mgr_cb_devices_info_get,
   _e_devicemgr_device_mgr_cb_device_get,
};

static void
_e_devicemgr_device_mgr_cb_unbind(struct wl_resource *resource)
{
   E_Comp_Data *cdata;

   if (!(cdata = wl_resource_get_user_data(resource))) return;

   cdata->input_device_mgr.resources = eina_list_remove(cdata->input_device_mgr.resources, resource);
}

static void
_e_devicemgr_device_mgr_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   struct wl_resource *res;
   E_Comp_Data *cdata;

   if (!(cdata = data)) return;
   if (!cdata->seat.global) return;

   if (!(res = wl_resource_create(client, &tizen_input_device_mgr_interface, MIN(version, 1), id)))
     {
        ERR("Could not create tizen_devices_interface resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   cdata->input_device_mgr.resources = eina_list_append(cdata->input_device_mgr.resources, res);

   wl_resource_set_implementation(res, &_e_devicemgr_device_mgr_interface, cdata,
                                 _e_devicemgr_device_mgr_cb_unbind);
}

int
e_devicemgr_device_init(void)
{
   E_Comp_Data *cdata;

   if (!e_comp) return 0;
   if (!(cdata = e_comp->wl_comp_data)) return 0;
   if (!cdata->wl.disp) return 0;

   /* try to add tizen_screenshooter to wayland globals */
   cdata->input_device_mgr.global = wl_global_create(cdata->wl.disp, &tizen_input_device_mgr_interface, 1,
                         cdata, _e_devicemgr_device_mgr_cb_bind);
   if (!cdata->input_device_mgr.global)
     {
        ERR("Could not add tizen_devices to wayland globals");
        return 0;
     }
   cdata->input_device_mgr.resources = NULL;
   cdata->input_device_mgr.device_list = NULL;

   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_ADD, _cb_device_add, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_DEL, _cb_device_del, NULL);

   return 1;
}

void
e_devicemgr_device_fini(void)
{
   E_FREE_LIST(handlers, ecore_event_handler_del);
}
