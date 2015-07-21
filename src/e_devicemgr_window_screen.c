#define E_COMP_WL
#include <e.h>
#include "e_devicemgr_window_screen.h"

#include <wayland-server.h>
#include "tizen_window_screen-server-protocol.h"

typedef struct _Window_Screen_Mode
{
   uint32_t mode;
   E_Client *ec;
   struct wl_resource *interface;
} Window_Screen_Mode;

static Eina_Hash *_hash_window_screen_modes = NULL;
static Eina_List *_window_screen_modes = NULL;
static Eina_List *_handlers = NULL;
static Eina_List *_hooks = NULL;

static void
_window_screen_mode_apply(void)
{
   //traversal e_client loop
   // if  e_client is visible then apply screen_mode
   // if all e_clients are default mode then set default screen_mode
   return;
}

static void
_e_tizen_window_screen_set_mode_cb(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface, uint32_t mode)
{
   E_Client *ec;
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

   if (!(ec = e_pixmap_client_get(ep)))
     {
        wl_resource_post_error(surface,
                               WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "No Pixmap Set On Surface");
        return;
     }

   /* make sure it's a wayland pixmap */
   if (e_pixmap_type_get(ep) != E_PIXMAP_TYPE_WL) return;

   wsm = eina_hash_find(_hash_window_screen_modes, &ec);
   if (!wsm)
     {
        wsm = E_NEW(Window_Screen_Mode, 1);
        if (!wsm)
          {
             wl_resource_post_error(surface,
                                    WL_DISPLAY_ERROR_INVALID_OBJECT,
                                    "No Pixmap Set On Surface");
             return;
          }

        eina_hash_add(_hash_window_screen_modes, &ec, wsm);
        _window_screen_modes = eina_list_append(_window_screen_modes, wsm);
     }

   wsm->mode = mode;
   wsm->ec = ec;
   wsm->interface = resource;

   _window_screen_mode_apply();

   /* Add other error handling code on window_screen send done. */
   tizen_window_screen_send_done(resource,
                                 surface,
                                 mode,
                                 TIZEN_WINDOW_SCREEN_ERROR_STATE_NONE);
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
             _window_screen_modes = eina_list_remove(_window_screen_modes, wsm);
             eina_hash_del_by_key(_hash_window_screen_modes, &(wsm->ec));
          }
     }
}

static void
_e_tizen_window_screen_cb_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   E_Comp_Data *cdata;
   struct wl_resource *res;

   if (!(cdata = data))
     {
        wl_client_post_no_memory(client);
        return;
     }

   res = wl_resource_create(client,
                            &tizen_window_screen_interface,
                            version,
                            id);
   if (!res)
     {
        ERR("Could not create tizen_window_screen resource: %m");
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res,
                                  &_e_tizen_window_screen_interface,
                                  cdata,
                                  _e_tizen_window_screen_destroy);
}

static void
_hook_client_del(void *d EINA_UNUSED, E_Client *ec)
{
   Window_Screen_Mode *wsm;

   //remove window_screen_mode from hash
   wsm = eina_hash_find(_hash_window_screen_modes, &ec);
   if (wsm)
     {
        _window_screen_modes = eina_list_remove(_window_screen_modes, wsm);
        eina_hash_del_by_key(_hash_window_screen_modes, &ec);
     }
}

static Eina_Bool
_cb_client_visibility_change(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   //E_Event_Client *ev;
   //ev = event;

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

   if (!wl_global_create(cdata->wl.disp,
                         &tizen_window_screen_interface,
                         1,
                         cdata,
                         _e_tizen_window_screen_cb_bind))
     {
        ERR("Could not add tizen_policy to wayland globals: %m");
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
   eina_list_free(_window_screen_modes);
   E_FREE_LIST(_hooks, e_client_hook_del);
   E_FREE_LIST(_handlers, ecore_event_handler_del);
   E_FREE_FUNC(_hash_window_screen_modes, eina_hash_free);
}
