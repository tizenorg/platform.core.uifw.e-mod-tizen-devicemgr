#define E_COMP_WL
#include <e.h>
#include "e_devicemgr_window_screen.h"
#include "e_devicemgr_dpms.h"

#include <wayland-server.h>
#include "tizen_window_screen-server-protocol.h"

typedef struct _Window_Screen_Mode
{
   uint32_t mode;
   struct wl_resource *surface;
   struct wl_resource *interface;
} Window_Screen_Mode;

static Eina_Hash *_hash_window_screen_modes = NULL;
static Eina_List *_window_screen_modes = NULL;
static Eina_List *_handlers = NULL;
static Eina_List *_hooks = NULL;
static uint32_t _current_screen_mode = TIZEN_WINDOW_SCREEN_MODE_DEFAULT;
static Eldbus_Connection *_conn = NULL;

static Eina_Bool
_win_scr_mode_get(E_Client *ec, uint32_t *mode)
{
   Window_Screen_Mode *wsm;
   E_Comp_Client_Data *cdata;
   struct wl_resource *surface;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ec, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(mode, EINA_FALSE);
   if (e_object_is_del(E_OBJECT(ec))) return EINA_FALSE;

   cdata = e_pixmap_cdata_get(ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN_VAL(cdata, EINA_FALSE);
   surface = cdata->wl_surface;
   EINA_SAFETY_ON_NULL_RETURN_VAL(surface, EINA_FALSE);

   wsm = eina_hash_find(_hash_window_screen_modes, &surface);
   EINA_SAFETY_ON_NULL_RETURN_VAL(wsm, EINA_FALSE);

   *mode = wsm->mode;

   return EINA_TRUE;
}

void _win_scr_mode_send(uint32_t mode)
{
   Eldbus_Message *msg = NULL;
   unsigned int timeout = 0;

   if(!_conn) return;

   if(mode == TIZEN_WINDOW_SCREEN_MODE_ALWAYS_ON)
     {
        msg = eldbus_message_method_call_new("org.tizen.system.deviced",
                                             "/Org/Tizen/System/DeviceD/Display",
                                             "org.tizen.system.deviced.display",
                                             "lockstate");
        if(!msg)
          {
             ERR("ERR Could not create DBus Message: %m");
             return;
          }
        eldbus_message_arguments_append(msg,
                                        "s", "lcdon",
                                        "s", "staycurstate",
                                        "s", "",
                                        "i", timeout);

        _current_screen_mode = TIZEN_WINDOW_SCREEN_MODE_ALWAYS_ON;
     }
   else if(mode == TIZEN_WINDOW_SCREEN_MODE_DEFAULT)
     {
        msg = eldbus_message_method_call_new("org.tizen.system.deviced",
                                             "/Org/Tizen/System/DeviceD/Display",
                                             "org.tizen.system.deviced.display",
                                             "unlockstate");
        if(!msg)
          {
             ERR("ERR Could not create DBus Message: %m");
             return;
          }
        eldbus_message_arguments_append(msg,
                                        "s", "lcdon",
                                        "s", "sleepmargin");

        _current_screen_mode = TIZEN_WINDOW_SCREEN_MODE_DEFAULT;
     }

   if (!eldbus_connection_send(_conn, msg, NULL, NULL, -1))
     {
        ERR("ERR Could not send DBus Message: %m");
        eldbus_message_unref(msg);
     }
}

static void
_window_screen_mode_apply(void)
{
   E_Client *ec;
   E_Zone *zone;
   uint32_t cur_mode, new_mode;
   Eina_Bool apply_mode = EINA_FALSE;

   cur_mode = _current_screen_mode;

   E_CLIENT_REVERSE_FOREACH(e_comp, ec)
     {
        zone = ec->zone;
        // Add check zone id
        if (E_INTERSECTS(ec->x, ec->y, ec->w, ec->h,
                         zone->x, zone->y, zone->w, zone->h))
          {
             if (ec->visibility.obscured)
               {
                  break;
               }
             else
               {
                  uint32_t _mode;
                  if (!_win_scr_mode_get(ec, &_mode)) continue;

                  new_mode = _mode;
                  apply_mode = EINA_TRUE;

                  if (_mode == TIZEN_WINDOW_SCREEN_MODE_ALWAYS_ON)
                    break;// mode is always on case
               }
          }
     }

   if (apply_mode)
     {
        if (new_mode == TIZEN_WINDOW_SCREEN_MODE_ALWAYS_ON)
          {
             if (cur_mode != TIZEN_WINDOW_SCREEN_MODE_ALWAYS_ON)
               _win_scr_mode_send(TIZEN_WINDOW_SCREEN_MODE_ALWAYS_ON);
          }
        else
          {
             if (cur_mode != TIZEN_WINDOW_SCREEN_MODE_DEFAULT)
               _win_scr_mode_send(TIZEN_WINDOW_SCREEN_MODE_DEFAULT);
          }
     }
   else
     {
        if (cur_mode != TIZEN_WINDOW_SCREEN_MODE_DEFAULT)
          _win_scr_mode_send(TIZEN_WINDOW_SCREEN_MODE_DEFAULT);
     }
}

static void
_e_tizen_window_screen_set_mode_cb(struct wl_client   *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *surface,
                                   uint32_t            mode)
{
   E_Pixmap *ep;
   Window_Screen_Mode *wsm;

   /* get the pixmap from this surface so we can find the client */
   if (!(ep = wl_resource_get_user_data(surface)))
     {
        wl_resource_post_error(surface,
                               WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "No Pixmap Set On Surface");
        return;
     }

   /* make sure it's a wayland pixmap */
   if (e_pixmap_type_get(ep) != E_PIXMAP_TYPE_WL) return;

   wsm = eina_hash_find(_hash_window_screen_modes, &surface);
   if (!wsm)
     {
        wsm = E_NEW(Window_Screen_Mode, 1);
        EINA_SAFETY_ON_NULL_RETURN(wsm);
        eina_hash_add(_hash_window_screen_modes, &surface, wsm);
        _window_screen_modes = eina_list_append(_window_screen_modes, wsm);
     }

   wsm->mode = mode;
   wsm->surface = surface;
   wsm->interface = resource;

   _window_screen_mode_apply();

   /* Add other error handling code on window_screen send done. */
   tizen_window_screen_send_done(resource, surface, mode, TIZEN_WINDOW_SCREEN_ERROR_STATE_NONE);
}

static const struct tizen_window_screen_interface _e_tizen_window_screen_interface =
{
   _e_tizen_window_screen_set_mode_cb
};

static void
_e_tizen_window_screen_destroy(struct wl_resource *resource)
{
   Window_Screen_Mode *wsm;
   Eina_List *l;

   if (!resource) return;

   EINA_LIST_FOREACH(_window_screen_modes, l, wsm)
     {
        if (wsm->interface == resource)
          {
             _window_screen_modes =  eina_list_remove(_window_screen_modes, wsm);
             eina_hash_del_by_key(_hash_window_screen_modes, &(wsm->surface));
          }
     }
}

static void
_e_tizen_window_screen_cb_bind(struct wl_client *client,
                               void             *data,
                               uint32_t          version,
                               uint32_t          id)
{
   E_Comp_Data *cdata;
   struct wl_resource *res;

   if (!(cdata = data))
     {
        wl_client_post_no_memory(client);
        return;
     }

   if (!(res = wl_resource_create(client, &tizen_window_screen_interface, version, id)))
     {
        ERR("Could not create tizen_window_screen resource: %m");
        wl_client_post_no_memory(client);
        return;
     }
   wl_resource_set_implementation(res, &_e_tizen_window_screen_interface,
                                  cdata, _e_tizen_window_screen_destroy);
}

static void
_hook_client_del(void *d EINA_UNUSED, E_Client *ec)
{
   Window_Screen_Mode *wsm;
   E_Comp_Client_Data *cdata;
   struct wl_resource *surface;

   EINA_SAFETY_ON_NULL_RETURN(ec);
   cdata = e_pixmap_cdata_get(ec->pixmap);
   EINA_SAFETY_ON_NULL_RETURN(cdata);
   surface = cdata->wl_surface;
   EINA_SAFETY_ON_NULL_RETURN(surface);

   //remove window_screen_mode from hash
   wsm = eina_hash_find(_hash_window_screen_modes, &surface);
   if (wsm)
     {
        _window_screen_modes =  eina_list_remove(_window_screen_modes, wsm);
        eina_hash_del_by_key(_hash_window_screen_modes, &surface);
     }
}

static Eina_Bool
_cb_client_visibility_change(void *data EINA_UNUSED,
                             int type   EINA_UNUSED,
                             void      *event)
{
   _window_screen_mode_apply();

   return ECORE_CALLBACK_PASS_ON;
}

#undef E_CLIENT_HOOK_APPEND
#define E_CLIENT_HOOK_APPEND(l, t, cb, d) \
  do                                      \
    {                                     \
       E_Client_Hook *_h;                 \
       _h = e_client_hook_add(t, cb, d);  \
       assert(_h);                        \
       l = eina_list_append(l, _h);       \
    }                                     \
  while (0)

Eina_Bool
e_devicemgr_window_screen_init(void)
{
   E_Comp_Data *cdata;

   EINA_SAFETY_ON_NULL_RETURN_VAL(e_comp, EINA_FALSE);

   cdata = e_comp->wl_comp_data;
   EINA_SAFETY_ON_NULL_RETURN_VAL(cdata, EINA_FALSE);
   EINA_SAFETY_ON_NULL_RETURN_VAL(cdata->wl.disp, EINA_FALSE);

   _conn = eldbus_connection_get(ELDBUS_CONNECTION_TYPE_SYSTEM);

   if (!_conn)
     {
        ERR("Could not get DBus SESSION bus: %m");
        return EINA_FALSE;
     }

   if (!wl_global_create(cdata->wl.disp, &tizen_window_screen_interface, 1,
                         cdata, _e_tizen_window_screen_cb_bind))
     {
        ERR("Could not add tizen_window_screen to wayland globals: %m");
        return EINA_FALSE;
     }

   _hash_window_screen_modes = eina_hash_pointer_new(free);


   E_CLIENT_HOOK_APPEND(_hooks, E_CLIENT_HOOK_DEL,
                        _hook_client_del, NULL);

   E_LIST_HANDLER_APPEND(_handlers, E_EVENT_CLIENT_VISIBILITY_CHANGE,
                         _cb_client_visibility_change, NULL);

   return EINA_TRUE;
}

void
e_devicemgr_window_screen_fini(void)
{
   if (_conn)
     {
        eldbus_connection_unref(_conn);
        _conn = NULL;
     }

   eina_list_free(_window_screen_modes);
   E_FREE_LIST(_hooks, e_client_hook_del);
   E_FREE_LIST(_handlers, ecore_event_handler_del);
   E_FREE_FUNC(_hash_window_screen_modes, eina_hash_free);
}
