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

   TRACE_BEGIN(_e_devicemgr_del_device);

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
     {
        if ((dev->capability == cap) && (!strcmp(dev->name, name))  && (!strcmp(dev->identifier, identifier)))
          break;
     }
   if (!dev)
     {
        TRACE_END();
        return;
     }

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
   TRACE_END();
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

   TRACE_BEGIN(_e_devicemgr_add_device);

   EINA_LIST_FOREACH(e_comp_wl->input_device_manager.device_list, l, dev)
     {
        if ((dev->capability == cap) && (!strcmp(dev->identifier, identifier)))
          {
             TRACE_END();
             return;
          }
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
             TRACE_END();
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
        tizen_input_device_send_device_info(res, dev->capability, TIZEN_INPUT_DEVICE_SUBCLAS_NONE, &axes);
     }

   e_comp_wl->input_device_manager.device_list = eina_list_append(e_comp_wl->input_device_manager.device_list, dev);
   TRACE_END();
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
_e_input_devmgr_request_client_remove(struct wl_client *client)
{
   input_devmgr_data->block_devtype = 0x0;
   if (input_devmgr_data->duration_timer)
     {
        ecore_timer_del(input_devmgr_data->duration_timer);
        input_devmgr_data->duration_timer = NULL;
     }
   input_devmgr_data->block_client = NULL;
}

static Eina_Bool
_e_input_devmgr_cb_block_timer(void *data)
{
   struct wl_resource *resource = (struct wl_resource *)data;
   struct wl_client *client = wl_resource_get_client(resource);

   if ((input_devmgr_data->block_client) && (input_devmgr_data->block_client != client))
     {
        return ECORE_CALLBACK_CANCEL;
     }

   _e_input_devmgr_request_client_remove(client);
   tizen_input_device_manager_send_block_expired(resource);

   return ECORE_CALLBACK_CANCEL;
}

static void
_e_input_devmgr_client_cb_destroy(struct wl_listener *l, void *data)
{
   struct wl_client *client = (struct wl_client *)data;

   if (!input_devmgr_data->block_client) return;

   _e_input_devmgr_request_client_remove(client);
}

static void
_e_input_devmgr_request_client_add(struct wl_client *client, struct wl_resource *resource, uint32_t clas, uint32_t duration)
{
   struct wl_listener *destroy_listener = NULL;
   double milli_duration = duration / 1000;

   /* Last request of block can renew timer time */
   if (input_devmgr_data->duration_timer)
     ecore_timer_del(input_devmgr_data->duration_timer);
   input_devmgr_data->duration_timer = ecore_timer_add(milli_duration, _e_input_devmgr_cb_block_timer, resource);

   input_devmgr_data->block_devtype |= clas;

   if (input_devmgr_data->block_client) return;
   input_devmgr_data->block_client = client;

   destroy_listener = E_NEW(struct wl_listener, 1);
   destroy_listener->notify = _e_input_devmgr_client_cb_destroy;
   wl_client_add_destroy_listener(client, destroy_listener);
}

static void
_e_input_devmgr_cb_block_events(struct wl_client *client, struct wl_resource *resource,
                             uint32_t serial, uint32_t clas, uint32_t duration)
{
   uint32_t all_class = TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE |
                        TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD |
                        TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN;
   /* TODO: Only permitted client could block input devices.
    *       Check privilege in here
    */
   if ((input_devmgr_data->block_client) && (input_devmgr_data->block_client != client))
     {
        tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_BLOCKED_ALREADY);
        return;
     }
   if (!(clas & all_class))
     {
        tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_INVALID_CLASS);
        return;
     }

   _e_input_devmgr_request_client_add(client, resource, clas, duration);

   /* TODO: Release pressed button or key */
}

static void
_e_input_devmgr_cb_unblock_events(struct wl_client *client, struct wl_resource *resource,
                             uint32_t serial)
{
   /* TODO: Only permitted client could block input devices.
    *       Check privilege in here
    */

  if ((input_devmgr_data->block_client) && (input_devmgr_data->block_client != client))
    {
       tizen_input_device_manager_send_error(resource, TIZEN_INPUT_DEVICE_MANAGER_ERROR_BLOCKED_ALREADY);
       return;
    }

   _e_input_devmgr_request_client_remove(client);
}

static const struct tizen_input_device_manager_interface _e_input_devmgr_implementation = {
   _e_input_devmgr_cb_block_events,
   _e_input_devmgr_cb_unblock_events,
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
        tizen_input_device_send_device_info(device_res, dev->capability, TIZEN_INPUT_DEVICE_SUBCLAS_NONE, &axes);
     }
}

static Eina_Bool
_e_devicemgr_block_check_pointer(int type, void *event)
{
   Ecore_Event_Mouse_Button *ev;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   if ((input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_MOUSE) ||
       (input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_TOUCHSCREEN))
     {
        return ECORE_CALLBACK_DONE;
     }
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_devicemgr_block_check_keyboard(int type, void *event)
{
   Ecore_Event_Key *ev;

   ev = event;
   EINA_SAFETY_ON_NULL_RETURN_VAL(ev, ECORE_CALLBACK_PASS_ON);

   if (input_devmgr_data->block_devtype & TIZEN_INPUT_DEVICE_MANAGER_CLAS_KEYBOARD)
     {
        return ECORE_CALLBACK_DONE;
     }
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_devicemgr_event_filter(void *data, void *loop_data EINA_UNUSED, int type, void *event)
{
   (void) data;

   /* Filter only for key down/up event */
   if (ECORE_EVENT_KEY_DOWN == type || ECORE_EVENT_KEY_UP == type)
     {
        return _e_devicemgr_block_check_keyboard(type, event);
     }
   else if(ECORE_EVENT_MOUSE_BUTTON_DOWN == type ||
           ECORE_EVENT_MOUSE_BUTTON_UP == type ||
           ECORE_EVENT_MOUSE_MOVE == type)
     {
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

   TRACE_BEGIN(e_devicemgr_device_init);

   /* try to add tizen_input_device_manager to wayland globals */
   e_comp_wl->input_device_manager.global = wl_global_create(e_comp_wl->wl.disp, &tizen_input_device_manager_interface, 1,
                         NULL, _e_devicemgr_device_mgr_cb_bind);
   if (!e_comp_wl->input_device_manager.global)
     {
        ERR("Could not add tizen_input_device_manager to wayland globals");
        TRACE_END();
        return 0;
     }
   e_comp_wl->input_device_manager.resources = NULL;
   e_comp_wl->input_device_manager.device_list = NULL;

   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_ADD, _cb_device_add, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_EVENT_DEVICE_DEL, _cb_device_del, NULL);

   ecore_event_filter_add(NULL, _e_devicemgr_event_filter, NULL, NULL);

   input_devmgr_data = E_NEW(e_devicemgr_input_devmgr_data, 1);
   EINA_SAFETY_ON_NULL_RETURN_VAL(input_devmgr_data, 0);

   input_devmgr_data->block_devtype = 0x0;

   TRACE_END();
   return 1;
}

void
e_devicemgr_device_fini(void)
{
   E_FREE_LIST(handlers, ecore_event_handler_del);
}
