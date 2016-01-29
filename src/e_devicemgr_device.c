#include "e_devicemgr_device.h"
#include <tizen-extension-server-protocol.h>

static Eina_List *handlers = NULL;

e_devicemgr_input_devmgr_data *input_devmgr_data;

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
_e_input_devmgr_request_client_remove(struct wl_client *client, struct wl_resource *surface,
                                  Eina_List **list_client, Eina_List **list_surface,
                                  enum tizen_input_device_manager_class device_class,
                                  Eina_Bool blocked)
{
   Eina_List *l;
   struct wl_client *db_client;
   struct wl_resource *db_surface;
   Ecore_Drm_Device *drm_dev;

   if (client)
     {
        EINA_LIST_FOREACH(*list_client, l, db_client)
          {
             if (db_client == client)
               {
                  ERR("[jeon] %s client(%p) is dead\n", blocked?"blocked":"disable", client);
                  *list_client = eina_list_remove(*list_client, db_client);
               }
          }
     }
   else
     {
        EINA_LIST_FOREACH(*list_surface, l, db_surface)
          {
             if (db_surface == surface)
               {
                  ERR("[jeon] %s client(%p) is dead\n", blocked?"blocked":"disable", surface);
                  *list_surface = eina_list_remove(*list_surface, db_surface);
               }
          }
     }

   if (!*list_client && !*list_surface)
     {
        ERR("[jeon] All 0x%x %s client is dead.\n", device_class, blocked?"blocked":"disable");
        if (blocked == EINA_TRUE)
          e_comp_wl->input_device_manager.block_devtype &= ~device_class;
        else
          {
             e_comp_wl->input_device_manager.disable_devtype &= ~device_class;
             if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
               {
                  EINA_LIST_FOREACH((Eina_List *)ecore_drm_devices_get(), l, drm_dev)
                    {
                       ERR("[jeon] request enable keyboard\n");
                       ecore_drm_device_device_enable(drm_dev, 0, 0);
                    }
               }
             if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
               {
                  EINA_LIST_FOREACH((Eina_List *)ecore_drm_devices_get(), l, drm_dev)
                    {
                       ERR("[jeon] request enable keyboard\n");
                       ecore_drm_device_device_enable(drm_dev, 0, 1);
                    }
               }
             if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
               {
                  EINA_LIST_FOREACH((Eina_List *)ecore_drm_devices_get(), l, drm_dev)
                    {
                       ERR("[jeon] request enable keyboard\n");
                       ecore_drm_device_device_enable(drm_dev, 0, 2);
                    }
               }
          }
     }
}

static void
_e_input_devmgr_request_client_remove_all(struct wl_client *client, struct wl_resource *surface)
{
   
   if (e_comp_wl->input_device_manager.block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
     _e_input_devmgr_request_client_remove(client, surface,
              &input_devmgr_data->block_clients_kbd, &input_devmgr_data->block_surfaces_kbd,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD, EINA_TRUE);

   if (e_comp_wl->input_device_manager.block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
     _e_input_devmgr_request_client_remove(client, surface,
              &input_devmgr_data->block_clients_mouse, &input_devmgr_data->block_surfaces_mouse,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE, EINA_TRUE);

   if (e_comp_wl->input_device_manager.block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
     _e_input_devmgr_request_client_remove(client, surface,
              &input_devmgr_data->block_clients_touch, &input_devmgr_data->block_surfaces_touch,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN, EINA_TRUE);

   if (e_comp_wl->input_device_manager.disable_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
     _e_input_devmgr_request_client_remove(client, surface,
              &input_devmgr_data->disable_clients_kbd, &input_devmgr_data->disable_surfaces_kbd,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD, EINA_FALSE);

   if (e_comp_wl->input_device_manager.disable_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
     _e_input_devmgr_request_client_remove(client, surface,
              &input_devmgr_data->disable_clients_mouse, &input_devmgr_data->disable_surfaces_mouse,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE, EINA_FALSE);

   if (e_comp_wl->input_device_manager.disable_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
     _e_input_devmgr_request_client_remove(client, surface,
              &input_devmgr_data->disable_clients_touch, &input_devmgr_data->disable_surfaces_touch,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN, EINA_FALSE);
}


static void
_e_input_devmgr_client_cb_destroy(struct wl_listener *l, void *data)
{
   struct wl_client *client = data;

   ERR("[jeon] %p client is dead\n", client);

   _e_input_devmgr_request_client_remove_all(client, NULL);
}

static void
_e_input_devmgr_surface_cb_destroy(struct wl_listener *l, void *data)
{
   struct wl_resource *surface = data;

   ERR("[jeon] %p surface is dead\n", surface);

   _e_input_devmgr_request_client_remove_all(NULL, surface);
}

static void
_e_input_devmgr_request_client_add(struct wl_client *client, struct wl_resource *surface,
                                   Eina_List **list_client, Eina_List **list_surface)
{
   struct wl_client *db_client;
   struct wl_resource *db_surface;
   Eina_List *l;
   Eina_Bool found = EINA_FALSE;
   struct wl_listener *destroy_listener = NULL;

   if (client)
     {
        EINA_LIST_FOREACH(*list_client, l, db_client)
          {
             if (db_client == client)
               {
                  ERR("[jeon] client %p is already request block\n", client);
                  found = EINA_TRUE;
                  break;
               }
          }
        if (!found)
          {
             ERR("[jeon] Add client %p to block/disable list\n", client);
             *list_client = eina_list_append(*list_client, client);

             destroy_listener = E_NEW(struct wl_listener, 1);
             destroy_listener->notify = _e_input_devmgr_client_cb_destroy;
             wl_client_add_destroy_listener(client, destroy_listener);
          }
     }
   else
     {
        EINA_LIST_FOREACH(*list_surface, l, db_surface)
          {
             if (db_surface == surface)
               {
                  ERR("[jeon] surface %p is already request block/disable\n", surface);
                  found = EINA_TRUE;
                  break;
               }
          }
        if (!found)
          {
             ERR("[jeon] Add surface %p to block/disable list\n", surface);
             *list_surface = eina_list_append(*list_surface, surface);

             destroy_listener = E_NEW(struct wl_listener, 1);
             destroy_listener->notify = _e_input_devmgr_surface_cb_destroy;
             wl_resource_add_destroy_listener(surface, destroy_listener);
          }
     }
}

static void
_e_input_devmgr_cb_block(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface,
                             uint32_t devclass, struct wl_resource *dev)
{
   enum tizen_input_device_manager_class device_class = devclass;
   struct tizen_input_device *device = (struct tizen_input_device *)dev;
   struct wl_client *cur_client = NULL;

   ERR("[jeon] block request. client: %pm surface: %p, class: 0x%x(0x%x), device: %p(%p)\n",
        client, surface, devclass, device_class, dev, device);

   if (!surface)
     {
        /* TODO: Only permitted client could block input devices.
         *       Check privilege in here
         */
        cur_client = client;
     }

   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
     {
        _e_input_devmgr_request_client_add(cur_client, surface, &input_devmgr_data->block_clients_kbd, &input_devmgr_data->block_surfaces_kbd);
     }
   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
     {
        _e_input_devmgr_request_client_add(cur_client, surface, &input_devmgr_data->block_clients_mouse, &input_devmgr_data->block_surfaces_mouse);
     }
   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
     {
        _e_input_devmgr_request_client_add(cur_client, surface, &input_devmgr_data->block_clients_touch, &input_devmgr_data->block_surfaces_touch);
     }   

   e_comp_wl->input_device_manager.block_devtype |= device_class;

   /* TODO: Release pressed button or key */
}

static void
_e_input_devmgr_cb_unblock(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface,
                             uint32_t devclass, struct wl_resource *dev)
{
   enum tizen_input_device_manager_class device_class = devclass;
   struct tizen_input_device *device = (struct tizen_input_device *)dev;
   struct wl_client *cur_client = NULL;

   ERR("[jeon] unblock request. client: %p surface: %p, class: 0x%x(0x%x), device: %p(%p)\n",
        client, surface, devclass, device_class, dev, device);

   if (!surface)
     {
        /* TODO: Only permitted client could block input devices.
         *       Check privilege in here
         */
        cur_client = client;
     }

   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
     _e_input_devmgr_request_client_remove(cur_client, surface,
              &input_devmgr_data->block_clients_kbd, &input_devmgr_data->block_surfaces_kbd,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD, EINA_TRUE);

   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
     _e_input_devmgr_request_client_remove(cur_client, surface,
              &input_devmgr_data->block_clients_mouse, &input_devmgr_data->block_surfaces_mouse,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE, EINA_TRUE);

   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
     _e_input_devmgr_request_client_remove(cur_client, surface,
              &input_devmgr_data->block_clients_touch, &input_devmgr_data->block_surfaces_touch,
              TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN, EINA_TRUE);
}

static void
_e_input_devmgr_cb_enable(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface,
                             uint32_t devclass, struct wl_resource *dev)
{
   enum tizen_input_device_manager_class device_class = devclass;
   struct tizen_input_device *device = (struct tizen_input_device *)dev;
   Ecore_Drm_Device *drm_dev;
   Eina_List *l;
   struct wl_client *cur_client = NULL;

   ERR("[jeon] enable request. client: %p surface: %p, class: 0x%x(0x%x), device: %p(%p)\n",
        client, surface, devclass, device_class, dev, device);

   if (!surface)
     {
        /* TODO: Only permitted client could block input devices.
         *       Check privilege in here
         */
        cur_client = client;
     }

   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
     {
        EINA_LIST_FOREACH((Eina_List *)ecore_drm_devices_get(), l, drm_dev)
          {
             ERR("[jeon] request enable keyboard\n");
             ecore_drm_device_device_enable(drm_dev, 0, 0);
          }

        _e_input_devmgr_request_client_remove(cur_client, surface,
                 &input_devmgr_data->disable_clients_kbd, &input_devmgr_data->disable_surfaces_kbd,
                 TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD, EINA_FALSE);
     }
   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
     {
        EINA_LIST_FOREACH((Eina_List *)ecore_drm_devices_get(), l, drm_dev)
          {
             ERR("[jeon] request enable keyboard\n");
             ecore_drm_device_device_enable(drm_dev, 0, 1);
          }

        _e_input_devmgr_request_client_remove(cur_client, surface,
                 &input_devmgr_data->disable_clients_mouse, &input_devmgr_data->disable_surfaces_mouse,
                 TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE, EINA_FALSE);
     }
   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
     {
        EINA_LIST_FOREACH((Eina_List *)ecore_drm_devices_get(), l, drm_dev)
          {
             ERR("[jeon] request enable keyboard\n");
             ecore_drm_device_device_enable(drm_dev, 0, 2);
          }

        _e_input_devmgr_request_client_remove(cur_client, surface,
                 &input_devmgr_data->disable_clients_touch, &input_devmgr_data->disable_surfaces_touch,
                 TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN, EINA_FALSE);
     }
}

static void
_e_input_devmgr_cb_disable(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface,
                             uint32_t devclass, struct wl_resource *dev)
{
   enum tizen_input_device_manager_class device_class = devclass;
   struct tizen_input_device *device = (struct tizen_input_device *)dev;
   Ecore_Drm_Device *drm_dev;
   Eina_List *l;
   struct wl_client *cur_client = NULL;

   ERR("[jeon] disable request. client: %p surface: %p, class: 0x%x(0x%x), device: %p(%p)\n",
        client, surface, devclass, device_class, dev, device);

   EINA_LIST_FOREACH((Eina_List *)ecore_drm_devices_get(), l, drm_dev)
     {
        if (devclass & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
          {
             ERR("[jeon] request disable keyboard\n");
             ecore_drm_device_device_enable(drm_dev, 1, 0);
          }
        if (devclass & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
          {
             ERR("[jeon] request disable mouse\n");
             ecore_drm_device_device_enable(drm_dev, 1, 1);
          }
        if (devclass & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
          {
             ERR("[jeon] request disable touch\n");
             ecore_drm_device_device_enable(drm_dev, 1, 2);
          }
     }

   if (!surface)
     {
        /* TODO: Only permitted client could block input devices.
         *       Check privilege in here
         */
        cur_client = client;
     }

   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
     {
        _e_input_devmgr_request_client_add(cur_client, surface, &input_devmgr_data->disable_clients_kbd, &input_devmgr_data->disable_surfaces_kbd);
     }
   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
     {
        _e_input_devmgr_request_client_add(cur_client, surface, &input_devmgr_data->disable_clients_mouse, &input_devmgr_data->disable_surfaces_mouse);
     }
   if (device_class & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
     {
        _e_input_devmgr_request_client_add(cur_client, surface, &input_devmgr_data->disable_clients_touch, &input_devmgr_data->disable_surfaces_touch);
     }
   e_comp_wl->input_device_manager.disable_devtype |= device_class;
}

static const struct tizen_input_device_manager_interface _e_input_devmgr_implementation = {
   _e_input_devmgr_cb_block,
   _e_input_devmgr_cb_unblock,
   _e_input_devmgr_cb_enable,
   _e_input_devmgr_cb_disable
};

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

   wl_resource_set_implementation(res, &_e_input_devmgr_implementation, NULL,
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
#if 0
static Eina_Bool
_e_devicemgr_block_device_check(Ecore_Input_Window *lookup, const char *dev_name)
{
   Evas_Device *dev, *data;
   Eina_List *l;

   dev = _ecore_event_get_evas_device(lookup->evas, dev_name);
   if (dev) evas_device_push(lookup->evas, dev);

   ERR("[jeon] checked device: name: %s, evas_dev_name :%s, evas_dev_desc: %s\n",
        dev_name, evas_device_name_get(dev), evas_device_description_get(dev));

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.block_devices, l, data)
     {
        ERR("[jeon] in block device list: %s, desc: %s\n", evas_device_name_get(dev), evas_device_description_get(dev));
        if (!strcmp(evas_device_name_get(dev), evas_device_name_get(data)))
          {
             ERR("[jeon] %s device is blocked device\n", evas_device_name_get(data));
             return ECORE_CALLBACK_DONE;
          }
     }
   evas_device_pop(lookup->evas);

   return ECORE_CALLBACK_PASS_ON;
}
#endif
#if 0
static Eina_Bool
_e_devicemgr_block_check_touch(int type, void *event)
{
   Ecore_Event_Key *ev;
   Ecore_Input_Window *lookup;
   Evas_Device *dev;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   if (e_comp_wl->input_device_manager.block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_TOUCHSCREEN)
     {
        ERR("[jeon] Block keyboard: 0x%x\n", e_comp_wl->input_device_manager.block_devtype);
        return ECORE_CALLBACK_DONE;
     }

   lookup = _ecore_event_window_match(ev->event_window);
   if (!lookup) return ECORE_CALLBACK_PASS_ON;

   return _e_devicemgr_block_device_check(lookup, ev->dev_name);
}
#endif
static Eina_Bool
_e_devicemgr_block_check_pointer(int type, void *event)
{
   Ecore_Event_Mouse_Button *ev;
//   Ecore_Input_Window *lookup;
//   Evas_Device *dev;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   if (e_comp_wl->input_device_manager.block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_MOUSE)
     {
        ERR("[jeon] Block mouse: 0x%x\n", e_comp_wl->input_device_manager.block_devtype);
        return ECORE_CALLBACK_DONE;
     }
   return ECORE_CALLBACK_PASS_ON;

//   lookup = _ecore_event_window_match(ev->event_window);
//   if (!lookup) return ECORE_CALLBACK_PASS_ON;

//   return _e_devicemgr_block_device_check(lookup, ev->dev_name);
}

static Eina_Bool
_e_devicemgr_block_check_keyboard(int type, void *event)
{
   Ecore_Event_Key *ev;
//   Ecore_Input_Window *lookup;
//   Evas_Device *dev;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   if (e_comp_wl->input_device_manager.block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLASS_KEYBOARD)
     {
        ERR("[jeon] Block keyboard: 0x%x\n", e_comp_wl->input_device_manager.block_devtype);
        return ECORE_CALLBACK_DONE;
     }
   return ECORE_CALLBACK_PASS_ON;

//   lookup = _ecore_event_window_match(ev->event_window);
//   if (!lookup) return ECORE_CALLBACK_PASS_ON;

//   return _e_devicemgr_block_device_check(lookup, ev->dev_name);
}

static Eina_Bool
_e_devicemgr_event_filter(void *data, void *loop_data EINA_UNUSED, int type, void *event)
{
   (void) data;
   (void) type;
   (void) event;

   /* Filter only for key down/up event */
   if (ECORE_EVENT_KEY_DOWN == type || ECORE_EVENT_KEY_UP == type)
     {
        Ecore_Event_Key *ev = event;
        ERR("[jeon] key!!!\n");
        
        ERR("[jeon] name: %s, key: %s, string: %s, compose: %s, code: %d\n", ev->keyname, ev->key, ev->string, ev->compose, ev->keycode);

        return _e_devicemgr_block_check_keyboard(type, event);
     }
   else if(ECORE_EVENT_MOUSE_BUTTON_DOWN == type ||
           ECORE_EVENT_MOUSE_BUTTON_UP == type ||
           ECORE_EVENT_MOUSE_MOVE == type)
     {
        ERR("[jeon] Mouse\n");
        return _e_devicemgr_block_check_pointer(type, event);
     }

   return ECORE_CALLBACK_PASS_ON;
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
   e_comp_wl->input_device_manager.block_devtype = 0x0;
   e_comp_wl->input_device_manager.block_devices = NULL;
   e_comp_wl->input_device_manager.disable_devtype = 0x0;
   e_comp_wl->input_device_manager.disable_devices = NULL;

   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_ADD, _cb_device_add, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_DEL, _cb_device_del, NULL);

   ecore_event_filter_add(NULL, _e_devicemgr_event_filter, NULL, NULL);

   input_devmgr_data = E_NEW(e_devicemgr_input_devmgr_data, 1);

   return 1;
}

void
e_devicemgr_device_fini(void)
{
   E_FREE_LIST(handlers, ecore_event_handler_del);
}
