#include "e.h"
#include "e_devicemgr_privates.h"
#include "e_mod_main.h"
#include <string.h>
#include <X11/XKBlib.h>

#undef SLOG
#define SLOG(priority, tag, format, arg...) printf(format, ##arg)

extern char *strcasestr(const char *s, const char *find);
DeviceMgr e_devicemgr;

static int _e_devicemgr_init(void);
static void _e_devicemgr_fini(void);

static int _e_devicemgr_cb_client_message  (void* data, int type, void* event);

static int _e_devicemgr_xinput_init(void);

static int _e_devicemgr_cb_window_property(void *data, int ev_type, void *ev);
static int _e_devicemgr_cb_event_generic(void *data, int ev_type, void *event);
static int _e_devicemgr_cb_zone_add(void *data, int ev_type, void *event);
static int _e_devicemgr_cb_zone_del(void *data, int ev_type, void *event);
static Eina_Bool _e_devicemgr_cb_mouse_in(void *data, int type, void *event);
static void _e_devicemgr_hook_border_move_end(void *data, E_Client *ec);
static void _e_devicemgr_hook_border_resize_end(void *data, E_Client *ec);
static E_Zone* _e_devicemgr_get_nth_zone(int index);

static void _e_devicemgr_update_input_transform_matrix(Eina_Bool reset);
static void _e_devicemgr_init_transform_matrix(void);
static void _e_devicemgr_init_output(void);
static void _e_devicemgr_init_input(void);

static int _e_devicemgr_check_device_type(int deviceid, DeviceMgrDeviceType type, const char* devname);
static void _e_devicemgr_xi2_device_changed_handler(XIDeviceChangedEvent *event);
static void _e_devicemgr_xi2_device_hierarchy_handler(XIHierarchyEvent *event);
static void _e_devicemgr_device_enabled(int id, int type);
static void _e_devicemgr_device_disabled(int id, int type);
static void _e_devicemgr_master_pointer_added(int id);
static void _e_devicemgr_master_pointer_removed(int id);
static void _e_devicemgr_slave_switched(int deviceid, int sourceid);
static void _e_devicemgr_device_changed(int deviceid, int sourceid);
static void _e_devicemgr_set_confine_information(int deviceid, E_Zone *zone, Eina_Bool isset, int region[4], Eina_Bool pointer_warp, Eina_Bool confine);
static void _e_devicemgr_set_mouse_exist(unsigned int val, int propset);
static void _e_devicemgr_set_keyboard_exist(unsigned int val, int is_connected);

static int _e_devicemgr_xkb_init(void);
static int _e_devicemgr_get_lockmodifier_mask(void);
static int _e_devicemgr_xkb_set_on(unsigned int mask);
static int _e_devicemgr_lockmodifier_set(void);

static Eina_Bool _e_devicemgr_create_master_device(char* master_name);
static Eina_Bool _e_devicemgr_remove_master_device(int master_id);
static Eina_Bool _e_devicemgr_reattach_slave(int slave_id, int master_id);
static Eina_Bool _e_devicemgr_virtual_touchpad_helper_enable(Eina_Bool is_enable);
static void _e_devicemgr_virtual_multitouch_helper_init(int deviceid);
static void _e_devicemgr_virtual_multitouch_helper_fini(void);
static void _e_devicemgr_show_device_list(unsigned int val);

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
   if (!_e_devicemgr_init())
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed @ _e_devicemgr_init()..!\n", __FUNCTION__);
        return NULL;
     }

   /* add handlers */
   e_devicemgr.client_message_handler = ecore_event_handler_add (ECORE_X_EVENT_CLIENT_MESSAGE, (Ecore_Event_Handler_Cb)_e_devicemgr_cb_client_message, NULL);
   e_devicemgr.window_property_handler = ecore_event_handler_add(ECORE_X_EVENT_WINDOW_PROPERTY, (Ecore_Event_Handler_Cb)_e_devicemgr_cb_window_property, NULL);
   e_devicemgr.event_generic_handler = ecore_event_handler_add(ECORE_X_EVENT_GENERIC, (Ecore_Event_Handler_Cb)_e_devicemgr_cb_event_generic, NULL);
   e_devicemgr.zone_add_handler = ecore_event_handler_add(E_EVENT_ZONE_ADD, (Ecore_Event_Handler_Cb)_e_devicemgr_cb_zone_add, NULL);
   e_devicemgr.zone_del_handler = ecore_event_handler_add(E_EVENT_ZONE_DEL, (Ecore_Event_Handler_Cb)_e_devicemgr_cb_zone_del, NULL);

   if (!e_devicemgr.window_property_handler) SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to add ECORE_X_EVENT_WINDOW_PROPERTY handler\n", __FUNCTION__);
   if (!e_devicemgr.event_generic_handler) SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to add ECORE_X_EVENT_GENERIC handler\n", __FUNCTION__);
   if (!e_devicemgr.zone_add_handler) SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to add E_EVENT_ZONE_ADD handler\n", __FUNCTION__);
   if (!e_devicemgr.zone_del_handler) SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to add E_EVENT_ZONE_DEL handler\n", __FUNCTION__);

   return m;
}

EAPI int
e_modapi_shutdown(E_Module *m EINA_UNUSED)
{
   ecore_event_handler_del(e_devicemgr.window_property_handler);
   ecore_event_handler_del(e_devicemgr.event_generic_handler);
   ecore_event_handler_del(e_devicemgr.zone_add_handler);
   ecore_event_handler_del(e_devicemgr.zone_del_handler);
   e_devicemgr.window_property_handler = NULL;
   e_devicemgr.event_generic_handler = NULL;
   e_devicemgr.zone_add_handler = NULL;
   e_devicemgr.zone_del_handler = NULL;

   _e_devicemgr_fini();

   return 1;
}

EAPI int
e_modapi_save(E_Module *m EINA_UNUSED)
{
   /* Do Something */
   return 1;
}

static int
_e_devicemgr_init(void)
{
   unsigned int val = 1;
   int res, ret = 1;

   memset(&e_devicemgr, 0, sizeof(DeviceMgr));

   e_devicemgr.disp = ecore_x_display_get();

   if (!e_devicemgr.disp)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[32m[e_devicemgr] Failed to open display..![0m\n");
        ret = 0;
        goto out;
     }

   e_devicemgr.rootWin = ecore_x_window_root_first_get();

   /* init data structure */
   e_devicemgr.vcp_id = -1;
   e_devicemgr.vcp_xtest_pointer_id = -1;
   e_devicemgr.vck_xtest_keyboard_id = -1;
   e_devicemgr.new_master_pointer_id = -1;
   e_devicemgr.virtual_touchpad_id = -1;
   e_devicemgr.virtual_multitouch_done = 1;
   e_devicemgr.device_list = NULL;
   e_devicemgr.atomRROutput = ecore_x_atom_get(E_PROP_XRROUTPUT);
   e_devicemgr.atomDeviceName = ecore_x_atom_get(E_PROP_DEVICE_NAME);
   e_devicemgr.atomDeviceList = ecore_x_atom_get(E_PROP_DEVICE_LIST);
   e_devicemgr.atomXMouseExist = ecore_x_atom_get(E_PROP_X_MOUSE_EXIST);
   e_devicemgr.atomXMouseCursorEnable = ecore_x_atom_get(E_PROP_X_MOUSE_CURSOR_ENABLE);
   e_devicemgr.atomXExtKeyboardExist = ecore_x_atom_get(E_PROP_X_EXT_KEYBOARD_EXIST);
   e_devicemgr.atomHWKbdInputStarted = ecore_x_atom_get(E_PROP_HW_KEY_INPUT_STARTED);
   e_devicemgr.atomAxisLabels = ecore_x_atom_get(E_PROP_X_EVDEV_AXIS_LABELS);
   e_devicemgr.atomVirtualTouchpadConfineRegion = ecore_x_atom_get(E_PROP_VIRTUAL_TOUCHPAD_CONFINE_REGION);
   e_devicemgr.atomVirtualTouchpad = ecore_x_atom_get(E_PROP_VIRTUAL_TOUCHPAD);
   e_devicemgr.atomVirtualTouchpadInt = ecore_x_atom_get(E_PROP_VIRTUAL_TOUCHPAD_INT);
   e_devicemgr.atomDeviceMgrInputWindow = ecore_x_atom_get(E_PROP_DEVICEMGR_INPUTWIN);
   e_devicemgr.atomTouchInput = ecore_x_atom_get(E_PROP_TOUCH_INPUT);
   e_devicemgr.atomFloat = ecore_x_atom_get(XATOM_FLOAT);
   e_devicemgr.atomInputTransform = ecore_x_atom_get(EVDEVMULTITOUCH_PROP_TRANSFORM);

   ecore_x_window_prop_card32_set(e_devicemgr.rootWin, e_devicemgr.atomTouchInput, &val, 1);
   memset(&e_devicemgr.virtual_touchpad_area_info, -1, sizeof(e_devicemgr.virtual_touchpad_area_info));
   memset(&e_devicemgr.virtual_multitouch_id, -1, sizeof(e_devicemgr.virtual_multitouch_id));
   e_devicemgr.virtual_touchpad_pointed_window = 0;
   e_devicemgr.zones = NULL;

   e_devicemgr.input_window = ecore_x_window_input_new(e_devicemgr.rootWin, -1, -1, 1, 1);

   if (!e_devicemgr.input_window)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr] Failed to create input_window !\n");
     }
   else
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr] Succeed to create input_window (0x%x)!\n", e_devicemgr.input_window);
        ecore_x_window_prop_property_set(e_devicemgr.rootWin, e_devicemgr.atomDeviceMgrInputWindow, ECORE_X_ATOM_WINDOW, 32, &e_devicemgr.input_window, 1);
     }

   res = _e_devicemgr_xinput_init();
   if (!res)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr] Failed to initialize XInput Extension !\n");
        ret =0;
        goto out;
     }

   res = _e_devicemgr_xkb_init();
   if (!res)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr] Failed to initialize XKB Extension !\n");
        ret = 0;
        goto out;
     }

   _e_devicemgr_init_transform_matrix();
   _e_devicemgr_init_input();
   _e_devicemgr_init_output();

out:
   return ret;
}

static void
_e_devicemgr_fini(void)
{
}

static int
_e_devicemgr_cb_client_message (void* data, int type, void* event)
{
   Ecore_X_Event_Client_Message* ev = event;
   int pos[2];
   int cx, cy;
   int x1, y1, x2, y2;
   int w, h, px = 0, py = 0;
   int vw, vh, pw = 0, ph = 0;
   int region[5];

   if (ev->message_type == e_devicemgr.atomVirtualTouchpadInt)
     {
        if (e_devicemgr.num_zones < 2)
          {
             ecore_x_client_message32_send(e_devicemgr.virtual_touchpad_window, e_devicemgr.atomVirtualTouchpadInt,
                                                                     ECORE_X_EVENT_MASK_NONE, E_VIRTUAL_TOUCHPAD_SHUTDOWN, 0, 0, 0, 0);
             return 1;
          }

        if (ev->data.l[0] == E_VIRTUAL_TOUCHPAD_NEED_TO_INIT)
          {
             ecore_x_client_message32_send(e_devicemgr.virtual_touchpad_window, e_devicemgr.atomVirtualTouchpadInt,
                                                                     ECORE_X_EVENT_MASK_NONE, E_VIRTUAL_TOUCHPAD_DO_INIT, 0, 0, 0, 0);
          }
        else if (ev->data.l[0] == E_VIRTUAL_TOUCHPAD_AREA_INFO)
          {
             e_devicemgr.virtual_touchpad_area_info[0] = ev->data.l[1];
             e_devicemgr.virtual_touchpad_area_info[1] = ev->data.l[2];
             e_devicemgr.virtual_touchpad_area_info[2] = ev->data.l[3];
             e_devicemgr.virtual_touchpad_area_info[3] = ev->data.l[4];
             SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][cb_client_message] virtual_touchpad_area_info=%d %d %d %d\n",
			 	e_devicemgr.virtual_touchpad_area_info[0], e_devicemgr.virtual_touchpad_area_info[1],
			 	e_devicemgr.virtual_touchpad_area_info[2], e_devicemgr.virtual_touchpad_area_info[3]);
          }
        else if (ev->data.l[0] == E_VIRTUAL_TOUCHPAD_WINDOW)
          {
             e_devicemgr.virtual_touchpad_window = (Ecore_X_Window)ev->data.l[1];
             SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][cb_client_message] virtual_touchpad_window=0x%x\n", e_devicemgr.virtual_touchpad_window);
          }
        else if (ev->data.l[0] == E_VIRTUAL_TOUCHPAD_CONFINE_SET)
          {
             _e_devicemgr_set_confine_information(e_devicemgr.virtual_touchpad_id, _e_devicemgr_get_nth_zone(2), EINA_TRUE, NULL, EINA_FALSE, EINA_TRUE);
             SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][cb_client_message] E_VIRTUAL_TOUCHPAD_CONFINE_SET\n");
          }
        else if (ev->data.l[0] == E_VIRTUAL_TOUCHPAD_CONFINE_UNSET)
          {
             _e_devicemgr_set_confine_information(e_devicemgr.virtual_touchpad_id, NULL, EINA_FALSE, NULL, EINA_FALSE, EINA_FALSE);
             SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][cb_client_message] E_VIRTUAL_TOUCHPAD_CONFINE_UNSET\n");
          }
        else if (ev->data.l[0] == E_VIRTUAL_TOUCHPAD_MT_BEGIN)
          {
             SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][cb_client_message] E_VIRTUAL_TOUCHPAD_MT_BEGIN !virtual_multitouch_done=%d\n", e_devicemgr.virtual_multitouch_done);

             if (0 != e_devicemgr.virtual_touchpad_pointed_window)
               {
                  XISetClientPointer(e_devicemgr.disp, e_devicemgr.virtual_touchpad_pointed_window, e_devicemgr.virtual_touchpad_id);
                  XSync(e_devicemgr.disp, 0);
                  ecore_x_pointer_xy_get(e_devicemgr.virtual_touchpad_pointed_window, &pos[0], &pos[1]);
                  SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][_cb_client_message] cursor pos x=%d, y=%d\n", pos[0], pos[1]);

                  if (pos[0] < 0 || pos[1] < 0 ) return 1;

                  e_devicemgr.virtual_multitouch_done = 0;

                  x1 = e_devicemgr.virtual_touchpad_pointed_window_info[0];
                  y1 = e_devicemgr.virtual_touchpad_pointed_window_info[1];
                  w = e_devicemgr.virtual_touchpad_pointed_window_info[2];
                  h = e_devicemgr.virtual_touchpad_pointed_window_info[3];

                  x2 = x1+w;
                  y2 = y1+h;
                  cx = x1 + pos[0];
                  cy = y1 + pos[1];

                  if (INSIDE(cx, cy, x1, y1, x1+(w/2), y1+(h/2)))
                    {
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 1st box (x1=%d, y1=%d, x2=%d, y2=%d)!\n", x1, y1, x1+(w/2), y1+(h/2));
                       pw = pos[0]*2;
                       ph = pos[1]*2;
                       px = x1;
                       py = y1;
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 1st box (effective area = %d, %d, %d, %d)!\n", px, py, pw, ph);
                    }
                  else if (INSIDE(cx, cy, x1+(w/2), y1, x2, y1+(h/2)))
                    {
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 2nd box (x1=%d, y1=%d, x2=%d, y2=%d)!\n", x1+(w/2), y1, x2, y1+(h/2));
                       pw = (w-pos[0])*2;
                       ph = pos[1]*2;
                       px = x2-pw;
                       py = y1;
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 2nd box (effective area = %d, %d, %d, %d)!\n", px, py, pw, ph);
                    }
                  else if (INSIDE(cx, cy, x1, y1+(h/2), x1+(w/2), y2))
                    {
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 3rd box (x1=%d, y1=%d, x2=%d, y2=%d)!\n", x1, y1+(h/2), x1+(w/2), y2);
                       pw = pos[0]*2;
                       ph = (h-pos[1])*2;
                       px = x1;
                       py = y2-ph;
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 3rd box (effective area = %d, %d, %d, %d)!\n", px, py, pw, ph);
                    }
                  else if (INSIDE(cx, cy, x1+(w/2), y1+(h/2), x2, y2))
                    {
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 4th box (x1=%d, y1=%d, x2=%d, y2=%d)!\n", x1+(w/2), y1+(h/2), x2, y2);
                       pw = (w-pos[0])*2;
                       ph = (h-pos[1])*2;
                       px = x2-pw;
                       py = y2-ph;
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] 4th box (effective area = %d, %d, %d, %d)!\n", px, py, pw, ph);
                    }
                  else
                    {
                       SLOG(LOG_DEBUG, "DEVICEMGR", "[_client_message] !!! pointer is not in 4 boxes !!!\n");
                    }

                  vw = e_devicemgr.virtual_touchpad_area_info[2] -e_devicemgr.virtual_touchpad_area_info[0];
                  vh = e_devicemgr.virtual_touchpad_area_info[3] -e_devicemgr.virtual_touchpad_area_info[1];

                  if (vw > pw) e_devicemgr.tmatrix[0] = (float)vw/pw;
                  else e_devicemgr.tmatrix[0] = (float)pw/vw;
                  if (vh > ph) e_devicemgr.tmatrix[4] = (float)vh/ph;
                  else e_devicemgr.tmatrix[4] = (float)ph/vh;

                  e_devicemgr.virtual_touchpad_cursor_pos[0] = pos[0];
                  e_devicemgr.virtual_touchpad_cursor_pos[1] = pos[1];
                  e_devicemgr.tmatrix[2] = (float)px*e_devicemgr.tmatrix[0]*(-1);
                  e_devicemgr.tmatrix[5] = (float)py*e_devicemgr.tmatrix[4]*(-1);
                  _e_devicemgr_update_input_transform_matrix(EINA_FALSE);

                  region[0] = e_devicemgr.virtual_touchpad_pointed_window_info[0] + 10;
                  region[1] = e_devicemgr.virtual_touchpad_pointed_window_info[1] + 10;
                  region[2] = e_devicemgr.virtual_touchpad_pointed_window_info[2] - 20;
                  region[3] = e_devicemgr.virtual_touchpad_pointed_window_info[3] - 20;
                  region[4] = 0;
                  _e_devicemgr_set_confine_information(e_devicemgr.virtual_touchpad_id, NULL, EINA_TRUE, &region[0], EINA_FALSE, EINA_TRUE);
                  ecore_x_client_message32_send(e_devicemgr.virtual_touchpad_window, e_devicemgr.atomVirtualTouchpadInt,
                                                                          ECORE_X_EVENT_MASK_NONE, E_VIRTUAL_TOUCHPAD_MT_MATRIX_SET_DONE, 0, 0, 0, 0);
               }
          }
        else if (ev->data.l[0] == E_VIRTUAL_TOUCHPAD_MT_END)
          {
             e_devicemgr.virtual_multitouch_done = 1;
             SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][cb_client_message] E_VIRTUAL_TOUCHPAD_MT_END !virtual_multitouch_done=%d\n", e_devicemgr.virtual_multitouch_done);
             if (0 != e_devicemgr.virtual_touchpad_pointed_window)
               {
                  _e_devicemgr_set_confine_information(e_devicemgr.virtual_touchpad_id, NULL, EINA_FALSE, NULL, EINA_FALSE, EINA_FALSE);
                  //_e_devicemgr_set_confine_information(e_devicemgr.virtual_touchpad_id, _e_devicemgr_get_nth_zone(2), EINA_TRUE, NULL, EINA_FALSE);
                  if (e_devicemgr.virtual_touchpad_cursor_pos[0] >= 0 && e_devicemgr.virtual_touchpad_cursor_pos[1] >= 0)
                    {
                       ecore_x_pointer_warp(e_devicemgr.virtual_touchpad_pointed_window, e_devicemgr.virtual_touchpad_cursor_pos[0], e_devicemgr.virtual_touchpad_cursor_pos[1]);
                       e_devicemgr.virtual_touchpad_cursor_pos[0] = -1;
                       e_devicemgr.virtual_touchpad_cursor_pos[1] = -1;
                  }
               }
          }

        return 1;
     }

   return 1;
}

static int
_e_devicemgr_xinput_init(void)
{
   int event, error;
   int major = 2, minor = 0;

   if (!XQueryExtension(e_devicemgr.disp, "XInputExtension", &e_devicemgr.xi2_opcode, &event, &error))
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] XInput Extension isn't supported.\n", __FUNCTION__);
        goto fail;
     }

   if (XIQueryVersion(e_devicemgr.disp, &major, &minor) == BadRequest)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to query XI version.\n", __FUNCTION__);
        goto fail;
     }

   memset(&e_devicemgr.eventmask, 0L, sizeof(XIEventMask));
   e_devicemgr.eventmask.deviceid = XIAllDevices;
   e_devicemgr.eventmask.mask_len = XIMaskLen(XI_RawMotion);
   e_devicemgr.eventmask.mask = calloc(e_devicemgr.eventmask.mask_len, sizeof(char));

   /* Events we want to listen for all */
   XISetMask(e_devicemgr.eventmask.mask, XI_DeviceChanged);
   XISetMask(e_devicemgr.eventmask.mask, XI_HierarchyChanged);
   XISelectEvents(e_devicemgr.disp, e_devicemgr.rootWin, &e_devicemgr.eventmask, 1);

   return 1;

fail:
   e_devicemgr.xi2_opcode = -1;
   return 0;
}

static int
_e_devicemgr_xkb_init(void)
{
   int xkb_opcode, xkb_event, xkb_error;
   int xkb_lmaj = XkbMajorVersion;
   int xkb_lmin = XkbMinorVersion;

   if (!(XkbLibraryVersion(&xkb_lmaj, &xkb_lmin)
	 && XkbQueryExtension(e_devicemgr.disp, &xkb_opcode, &xkb_event, &xkb_error, &xkb_lmaj, &xkb_lmin)))
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][xkb_init] Failed to initialize XKB extension !\n");
        e_devicemgr.xkb_available = EINA_FALSE;
        return 0;
     }

   e_devicemgr.xkb_available = EINA_TRUE;
   return 1;
}

static int
_e_devicemgr_cb_window_property(void *data, int ev_type, void *ev)
{
   int res;
   unsigned int ret_val = 0;
   Ecore_X_Event_Window_Property *e = ev;

   if (e->atom == e_devicemgr.atomDeviceList && e->win == e_devicemgr.rootWin)
     {
        res = ecore_x_window_prop_card32_get(e->win, e_devicemgr.atomDeviceList, &ret_val, 1);

        if (res == 1) _e_devicemgr_show_device_list(ret_val);
        goto out;
     }

   if (e->atom == e_devicemgr.atomVirtualTouchpad && e->win == e_devicemgr.rootWin)
     {
        res = ecore_x_window_prop_card32_get(e->win, e_devicemgr.atomVirtualTouchpad, &ret_val, 1);

        if (res == 1 && e_devicemgr.virtual_touchpad_id > 0)
          {
             ecore_x_client_message32_send(e_devicemgr.virtual_touchpad_window, e_devicemgr.atomVirtualTouchpadInt,
                                                                     ECORE_X_EVENT_MASK_NONE, E_VIRTUAL_TOUCHPAD_SHUTDOWN, 0, 0, 0, 0);
          }
        goto out;
     }

out:
   return 1;
}

static int
_e_devicemgr_cb_event_generic(void *data, int ev_type, void *event)
{
   Ecore_X_Event_Generic *e = (Ecore_X_Event_Generic *)event;
   XIDeviceEvent *evData = (XIDeviceEvent *)(e->data);

   if (e->extension != e_devicemgr.xi2_opcode)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Invalid event !(extension:%d, evtype:%d)\n", __FUNCTION__, e->extension, e->evtype);
        return 1;
     }

   if (!evData || evData->send_event)
   {
      SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Generic event data is not available or the event was sent via XSendEvent (and will be ignored) !\n", __FUNCTION__);
      return 1;
   }

   switch (e->evtype)
   {
      case XI_HierarchyChanged:
         _e_devicemgr_xi2_device_hierarchy_handler((XIHierarchyEvent *)evData);
         break;

      case XI_DeviceChanged:
         _e_devicemgr_xi2_device_changed_handler((XIDeviceChangedEvent *)evData);
         break;
   }

   return 1;
}

static int
_e_devicemgr_cb_zone_add(void *data, int ev_type, void *event)
{
   E_Event_Zone_Add *ev;
   E_Zone *zone;
   Eina_List *l;

   ev = event;
   zone = ev->zone;
   if (!zone || !zone->name) return 1;

   l = eina_list_data_find_list(e_devicemgr.zones, zone);
   if (l)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_add] zone exists already in zone list !\n");
        return 1;
     }

   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_add] z->name=%s, z->w=%d, z->h=%d, z->x=%d, z->y=%d\n",
       zone->name, zone->w, zone->h, zone->x, zone->y);
   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_add] z->useful_geometry.w=%d, z->useful_geometry.h=%d, z->useful_geometry.x=%d, z->useful_geometry.y=%d\n",
       zone->useful_geometry.w, zone->useful_geometry.h, zone->useful_geometry.x, zone->useful_geometry.y);

   e_devicemgr.zones = eina_list_append(e_devicemgr.zones, zone);
   e_devicemgr.num_zones++;

   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_add] num_zones=%d\n", e_devicemgr.num_zones);

   return 1;
}

static int
_e_devicemgr_cb_zone_del(void *data, int ev_type, void *event)
{
   E_Event_Zone_Del *ev;
   E_Zone *zone;
   Eina_List *l;

   ev = event;
   zone = ev->zone;
   if (!zone || !zone->name) return 1;

   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_del] z->name=%s, z->w=%d, z->h=%d, z->x=%d, z->y=%d\n",
       zone->name, zone->w, zone->h, zone->x, zone->y);
   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_del] z->useful_geometry.w=%d, z->useful_geometry.h=%d, z->useful_geometry.x=%d, z->useful_geometry.y=%d\n",
       zone->useful_geometry.w, zone->useful_geometry.h, zone->useful_geometry.x, zone->useful_geometry.y);

   if (e_devicemgr.num_zones < 2)//basic zone + additional zone(s)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_del] Zone list needs to be checked into !\n");
        return 1;
     }

   l = eina_list_data_find_list(e_devicemgr.zones, zone);
   if (!l)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_del] zone doesn't exist in zone list !\n");
        return 1;
     }

   if (e_devicemgr.num_zones == 2 && e_devicemgr.virtual_touchpad_id > 0)
     {
        ecore_x_client_message32_send(e_devicemgr.virtual_touchpad_window, e_devicemgr.atomVirtualTouchpadInt,
                                                                ECORE_X_EVENT_MASK_NONE, E_VIRTUAL_TOUCHPAD_SHUTDOWN, 0, 0, 0, 0);
     }

   e_devicemgr.zones = eina_list_remove(e_devicemgr.zones, zone);
   e_devicemgr.num_zones--;

   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][zone_del] num_zones=%d\n", e_devicemgr.num_zones);

   return 1;
}

static void
_e_devicemgr_hook_border_resize_end(void *data, E_Client *ec)
{
   int l = 0, t = 0;

   if (!ec) return;

   if (ec->frame)
      e_comp_object_frame_geometry_get(ec->frame, &l, NULL, &t, NULL);

   e_devicemgr.virtual_touchpad_pointed_window_info[0] = ec->x + l;
   e_devicemgr.virtual_touchpad_pointed_window_info[1] = ec->y + t;
   e_devicemgr.virtual_touchpad_pointed_window_info[2] = ec->client.w;
   e_devicemgr.virtual_touchpad_pointed_window_info[3] = ec->client.h;

   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][hook_border_resize_end] application win=0x%x, px=%d, py=%d, pw=%d, ph=%d\n", e_client_util_win_get(ec),
              e_devicemgr.virtual_touchpad_pointed_window_info[0], e_devicemgr.virtual_touchpad_pointed_window_info[1],
              e_devicemgr.virtual_touchpad_pointed_window_info[2], e_devicemgr.virtual_touchpad_pointed_window_info[3]);
}

static void
_e_devicemgr_hook_border_move_end(void *data, E_Client *ec)
{
   int l = 0, t = 0;

   if (!ec) return;

   if (ec->frame)
      e_comp_object_frame_geometry_get(ec->frame, &l, NULL, &t, NULL);

   e_devicemgr.virtual_touchpad_pointed_window_info[0] = ec->x + l;
   e_devicemgr.virtual_touchpad_pointed_window_info[1] = ec->y + t;
   e_devicemgr.virtual_touchpad_pointed_window_info[2] = ec->client.w;
   e_devicemgr.virtual_touchpad_pointed_window_info[3] = ec->client.h;

   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][hook_border_move_end] application win=0x%x, px=%d, py=%d, pw=%d, ph=%d\n", e_client_util_win_get(ec),
              e_devicemgr.virtual_touchpad_pointed_window_info[0], e_devicemgr.virtual_touchpad_pointed_window_info[1],
              e_devicemgr.virtual_touchpad_pointed_window_info[2], e_devicemgr.virtual_touchpad_pointed_window_info[3]);
}

static Eina_Bool
_e_devicemgr_cb_mouse_in(void *data, int type, void *event)
{
   E_Client *ec;
   Ecore_X_Event_Mouse_In *ev = event;
   int l = 0, t = 0;

   if (!e_devicemgr.virtual_multitouch_done) return ECORE_CALLBACK_PASS_ON;

   ec = e_pixmap_find_client(E_PIXMAP_TYPE_X, ev->event_win);
   if (!ec)
     {
        if (e_devicemgr.rootWin == ecore_x_window_parent_get(ev->event_win))
          {

             if (!e_devicemgr.virtual_multitouch_done && e_devicemgr.virtual_touchpad_pointed_window != 0)
               return ECORE_CALLBACK_PASS_ON;

             e_devicemgr.virtual_touchpad_pointed_window = 0;
             ecore_x_client_message32_send(e_devicemgr.virtual_touchpad_window, e_devicemgr.atomVirtualTouchpadInt,
                                                                     ECORE_X_EVENT_MASK_NONE, E_VIRTUAL_TOUCHPAD_POINTED_WINDOW, 0, 0, 0, 0);
             _e_devicemgr_update_input_transform_matrix(EINA_TRUE/* reset */);
             return ECORE_CALLBACK_PASS_ON;
          }

        return ECORE_CALLBACK_PASS_ON;
     }

   if (ec->zone->id == 0)
     {
        return ECORE_CALLBACK_PASS_ON;
     }

   if (ec->frame)
      e_comp_object_frame_geometry_get(ec->frame, &l, NULL, &t, NULL);

   e_devicemgr.virtual_touchpad_pointed_window_info[0] = ec->x + l;
   e_devicemgr.virtual_touchpad_pointed_window_info[1] = ec->y + t;
   e_devicemgr.virtual_touchpad_pointed_window_info[2] = ec->client.w;
   e_devicemgr.virtual_touchpad_pointed_window_info[3] = ec->client.h;
   e_devicemgr.virtual_touchpad_pointed_window = e_client_util_win_get(ec);

   SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][cb_mouse_in] application win=0x%x, px=%d, py=%d, pw=%d, ph=%d\n", e_client_util_win_get(ec),
              e_devicemgr.virtual_touchpad_pointed_window_info[0], e_devicemgr.virtual_touchpad_pointed_window_info[1],
              e_devicemgr.virtual_touchpad_pointed_window_info[2], e_devicemgr.virtual_touchpad_pointed_window_info[3]);
   ecore_x_client_message32_send(e_devicemgr.virtual_touchpad_window, e_devicemgr.atomVirtualTouchpadInt,
                                                           ECORE_X_EVENT_MASK_NONE, E_VIRTUAL_TOUCHPAD_POINTED_WINDOW,
                                                           e_devicemgr.virtual_touchpad_pointed_window, 0, 0, 0);

   return ECORE_CALLBACK_PASS_ON;
}

static int
_e_devicemgr_marshalize_string (char* buf, int num, char* srcs[])
{
   int i;
   char * p = buf;

   for (i=0; i<num; i++)
     {
        p += sprintf (p, "%s", srcs[i]);
        *p = '\0';
        p++;
     }

   *p = '\0';
   p++;

   return (p - buf);
}

static void
_e_devicemgr_init_output(void)
{
   int i;
   XRROutputInfo *output_info = NULL;


   XRRScreenResources* res = XRRGetScreenResources (e_devicemgr.disp, e_devicemgr.rootWin);
   e_devicemgr.output = 0;

   if (res && (res->noutput != 0))
     {
        for ( i = 0 ; i  <res->noutput ; i++ )
          {
             output_info = XRRGetOutputInfo(e_devicemgr.disp, res, res->outputs[i]);
             if (output_info && output_info->name && !strncmp(output_info->name, "LVDS1", 5))
               {
                  e_devicemgr.output = res->outputs[i];
                  SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][_e_devicemgr_init_output] LVDS1 was found !\n");
                  XRRFreeOutputInfo(output_info);
                  break;
               }
             else
               {
                  e_devicemgr.output = res->outputs[i];
                  SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][_e_devicemgr_init_output] LVDS1 was not found yet !\n");
               }

             if (output_info) XRRFreeOutputInfo(output_info);
          }
     }

   if (!e_devicemgr.output)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][_e_devicemgr_init_output] Failed to init output !\n");
     }
}

static void
_e_devicemgr_init_transform_matrix(void)
{
   memset(e_devicemgr.tmatrix, 0, sizeof(e_devicemgr.tmatrix));

   e_devicemgr.tmatrix[8] = 1.0f;
   e_devicemgr.tmatrix[0] = 1.0f;
   e_devicemgr.tmatrix[4] = 1.0f;
}

static void
_e_devicemgr_init_input(void)
{
   int i;
   int ndevices;
   XIDeviceInfo *dev, *info = NULL;
   DeviceMgr_Device_Info *data = NULL;

   if (e_devicemgr.xi2_opcode < 0)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to initialize input !\n", __FUNCTION__);
        return;
     }

   info = XIQueryDevice(e_devicemgr.disp, XIAllDevices, &ndevices);

   if (!info)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] There is no queried XI device.\n", __FUNCTION__);
        return;
     }

   for (i = 0; i < ndevices ; i++)
     {
        dev = &info[i];

        switch (dev->use)
          {
             case XISlavePointer:
                if (strcasestr(dev->name, "Virtual core XTEST pointer"))
                  {
                     e_devicemgr.vcp_xtest_pointer_id = dev->deviceid;
                     continue;
                  }
                if (strcasestr(dev->name, "XTEST")) continue;
                if (strcasestr(dev->name, "keyboard")) goto handle_keyboard;

                data = malloc(sizeof(DeviceMgr_Device_Info));

                if (!data)
                  {
                     SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to allocate memory for device info !\n", __FUNCTION__);
                     goto out;
                  }

                data->id = dev->deviceid;
                data->name = eina_stringshare_add(dev->name);
                if (1==_e_devicemgr_check_device_type(dev->deviceid, E_DEVICEMGR_TOUCHSCREEN, dev->name))
                  {
                     //Now we have a touchscreen device.
                     data->type = E_DEVICEMGR_TOUCHSCREEN;
                     e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                     e_devicemgr.num_touchscreen_devices++;
                     SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Slave touchscreen device (id=%d, name=%s, num_touchscreen_devices=%d) was added/enabled !\n",
                                 __FUNCTION__, dev->deviceid, dev->name, e_devicemgr.num_touchscreen_devices);
                  }
                else
                  {
                     //Now we have a mouse.
                     data->type = E_DEVICEMGR_MOUSE;
                     e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                     e_devicemgr.num_pointer_devices++;

                     if (1==e_devicemgr.num_pointer_devices) _e_devicemgr_set_mouse_exist(1, 1);
                     SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Slave pointer device (id=%d, name=%s, num_pointer_devices=%d) was added/enabled !\n",
                                 __FUNCTION__, dev->deviceid, dev->name, e_devicemgr.num_pointer_devices);
                  }
                break;

             case XISlaveKeyboard:
                if (strcasestr(dev->name, "Virtual core XTEST keyboard"))
                  {
                     e_devicemgr.vck_xtest_keyboard_id = dev->deviceid;
                     continue;
                  }

                if (strcasestr(dev->name, "XTEST")) continue;
handle_keyboard:
                data = malloc(sizeof(DeviceMgr_Device_Info));

                if (!data)
                  {
                     SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to allocate memory for device info !\n", __FUNCTION__);
                     goto out;
                  }

                if (strcasestr(dev->name, "keyboard") && !strcasestr(dev->name, "XTEST" ))//keyboard
                  {
                     //Now we have a keyboard device
                     data->id = dev->deviceid;
                     data->name = eina_stringshare_add(dev->name);
                     data->type = E_DEVICEMGR_KEYBOARD;

                     e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                     e_devicemgr.num_keyboard_devices++;
                     _e_devicemgr_lockmodifier_set();

                     if (e_devicemgr.num_keyboard_devices >= 1)
                       {
                          _e_devicemgr_set_keyboard_exist((unsigned int)e_devicemgr.num_keyboard_devices, 1);
                       }

                     SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave keyboard device (id=%d, name=%s, num_keyboard_devices=%d) was added/enabled !\n",
                                 __FUNCTION__, dev->deviceid, dev->name, e_devicemgr.num_keyboard_devices);
                  }
                else//HW key
                  {
                     data->type = E_DEVICEMGR_HWKEY;
                     data->id = dev->deviceid;
                     data->name = eina_stringshare_add(dev->name);

                     e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                     e_devicemgr.num_hwkey_devices++;
                  }
                break;

             case XIFloatingSlave:
                break;

             case XIMasterPointer:
                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] XIMasterPointer (VCP) (id=%d, name=%s)\n", __FUNCTION__, dev->deviceid, dev->name);
                e_devicemgr.vcp_id = dev->deviceid;
                break;

             case XIMasterKeyboard:
                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] XIMasterKeyboard (VCK) (id=%d, name=%s)\n", __FUNCTION__, dev->deviceid, dev->name);
                e_devicemgr.vck_id = dev->deviceid;
                break;
          }
     }

out:
   XIFreeDeviceInfo(info);
}

static void
_e_devicemgr_xi2_device_changed_handler(XIDeviceChangedEvent *event)
{
   if (event->reason == XISlaveSwitch)
     {
        _e_devicemgr_slave_switched(event->deviceid, event->sourceid);
     }
   else if (event->reason == XIDeviceChange)
     {
        _e_devicemgr_device_changed(event->deviceid, event->sourceid);
     }
}

static void
_e_devicemgr_xi2_device_hierarchy_handler(XIHierarchyEvent *event)
{
   int i;

   if (event->flags & XIMasterAdded || event->flags & XIMasterRemoved)
     {
        for( i = 0 ; i < event->num_info ; i++ )
          {
             if (event->info[i].flags & XIMasterAdded)
               {
                  _e_devicemgr_master_pointer_added(event->info[i].deviceid);
               }
             else if (event->info[i].flags & XIMasterRemoved)
               {
                  _e_devicemgr_master_pointer_removed(event->info[i].deviceid);
               }
          }
     }

   if (event->flags & XIDeviceEnabled || event->flags & XIDeviceDisabled)
     {
        for( i = 0 ; i < event->num_info ; i++ )
          {
             if (event->info[i].flags & XIDeviceEnabled)
               {
                  _e_devicemgr_device_enabled(event->info[i].deviceid, event->info[i].use);
               }
             else if (event->info[i].flags & XIDeviceDisabled)
               {
                  _e_devicemgr_device_disabled(event->info[i].deviceid, event->info[i].use);
               }
          }
     }
}

static int
_e_devicemgr_check_device_type(int deviceid, DeviceMgrDeviceType type, const char* devname)
{
   char *tmp = NULL;
   Atom act_type;
   unsigned long nitems, bytes_after;
   unsigned char *data, *ptr;
   int j, act_format, ret = 0;

   if (type != E_DEVICEMGR_TOUCHSCREEN && type != E_DEVICEMGR_MOUSE) return 0;

   if (XIGetProperty(e_devicemgr.disp, deviceid, e_devicemgr.atomAxisLabels, 0, 1000, False,
                              XA_ATOM, &act_type, &act_format,
                              &nitems, &bytes_after, &data) != Success)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][check_device_type] Failed to get XI2 device property !(deviceid=%d)\n", deviceid);
        goto out;
     }

   if (!nitems) goto out;

   ptr = data;

   for (j = 0; j < nitems; j++)
     {
        switch(act_type)
          {
             case XA_ATOM:
               {
                  Ecore_X_Atom atomTemp = *(Ecore_X_Atom*)ptr;
                  if (!atomTemp) goto out;

                  tmp = ecore_x_atom_name_get(atomTemp);
                  if ((type == E_DEVICEMGR_TOUCHSCREEN) && (strcasestr(tmp, "Abs X") || strcasestr(tmp, "Abs MT Position X")) && !strcasestr(devname, "maru"))
                    {
                       ret = 1;
                       goto out;
                    }
                  else if (type == E_DEVICEMGR_MOUSE && strcasestr(tmp, "Rel X"))
                    {
                       ret = 1;
                       goto out;
                    }
               }
             break;
          }

        ptr += act_format/8;
     }

out:
   if (data) XFree(data);
   if (tmp) free(tmp);

   return ret;
}

static void
_e_devicemgr_device_enabled(int id, int type)
{
   int ndevices;
   XIDeviceInfo *info = NULL;
   DeviceMgr_Device_Info *data = NULL;

   info = XIQueryDevice(e_devicemgr.disp, id, &ndevices);

   if (!info || ndevices <= 0)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] There is no queried XI device. (device id=%d, type=%d)\n", __FUNCTION__, id, type);
        goto out;
     }

   switch(info->use)
     {
        case XISlavePointer:
           if (strcasestr(info->name, "XTEST")) goto out;
           if (strcasestr(info->name, "keyboard")) goto handle_keyboard;

           data = malloc(sizeof(DeviceMgr_Device_Info));

           if (!data)
             {
                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Failed to allocate memory for device info !\n", __FUNCTION__);
                goto out;
             }

           data->id = id;
           data->name = eina_stringshare_add(info->name);

           if (1==_e_devicemgr_check_device_type(id, E_DEVICEMGR_TOUCHSCREEN, info->name))
             {
                //Now we have a touchscreen device.
                data->type = E_DEVICEMGR_TOUCHSCREEN;
                if (strcasestr(data->name, "virtual") && strcasestr(data->name, "multitouch"))
                  {
                     _e_devicemgr_virtual_multitouch_helper_init(id);
                  }
                e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                e_devicemgr.num_touchscreen_devices++;
                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave touchscreen device (id=%d, name=%s, num_touchscreen_devices=%d) was added/enabled !\n",
                            __FUNCTION__, id, info->name, e_devicemgr.num_touchscreen_devices);
             }
           else
             {
                //Now we have a mouse.
                data->type = E_DEVICEMGR_MOUSE;
                e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                e_devicemgr.num_pointer_devices++;
                if (1==e_devicemgr.num_pointer_devices) _e_devicemgr_set_mouse_exist(1, 1);
                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave pointer device (id=%d, name=%s, num_pointer_devices=%d) was added/enabled !\n",
                           __FUNCTION__, id, info->name, e_devicemgr.num_pointer_devices);
                if (strcasestr(info->name, E_VIRTUAL_TOUCHPAD_NAME))
                  {
                     e_devicemgr.virtual_touchpad_id = id;
                     _e_devicemgr_virtual_touchpad_helper_enable(EINA_TRUE);
                  }
             }
           break;

        case XISlaveKeyboard:
           if (strcasestr(info->name, "XTEST")) goto out;
handle_keyboard:
           data = malloc(sizeof(DeviceMgr_Device_Info));

           if (!data)
             {
                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Failed to allocate memory for device info !\n", __FUNCTION__);
                goto out;
             }

           if (strcasestr(info->name, "keyboard"))//keyboard
             {
                //Now we have a keyboard device.
                data->id = id;
                data->name = eina_stringshare_add(info->name);
                data->type = E_DEVICEMGR_KEYBOARD;

                e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                e_devicemgr.num_keyboard_devices++;

                if (e_devicemgr.num_keyboard_devices >= 1)
                  _e_devicemgr_set_keyboard_exist((unsigned int)e_devicemgr.num_keyboard_devices, 1);

                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave keyboard device (id=%d, name=%s, num_keyboard_devices=%d) was added/enabled !\n",
                           __FUNCTION__, id, info->name, e_devicemgr.num_keyboard_devices);
             }
           else//HW key
             {
                data->type = E_DEVICEMGR_HWKEY;
                data->id = id;
                data->name = eina_stringshare_add(info->name);

                e_devicemgr.device_list = eina_list_append(e_devicemgr.device_list, data);
                e_devicemgr.num_hwkey_devices++;
             }

           _e_devicemgr_lockmodifier_set();
           break;

        case XIFloatingSlave:
           if (1 == _e_devicemgr_check_device_type(id, E_DEVICEMGR_TOUCHSCREEN, info->name))
             {
                //Now we have a floating touchscreen device.
                if (strcasestr(info->name, "virtual") && strcasestr(info->name, "multitouch"))
                  {
                     _e_devicemgr_virtual_multitouch_helper_init(id);
                  }
                SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] FloatingSlave touchscreen device (id=%d, name=%s) was added/enabled !\n",
                            __FUNCTION__, id, info->name);
             }
           break;
     }

out:
   if (info) XIFreeDeviceInfo(info);
}

static void
_e_devicemgr_device_disabled(int id, int type)
{
   Eina_List* l;
   DeviceMgr_Device_Info *data;

   if (!e_devicemgr.device_list)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] device list is empty ! something's wrong ! (id=%d, type=%d)\n", __FUNCTION__, id, type);
        goto out;
     }

   EINA_LIST_FOREACH(e_devicemgr.device_list, l, data)
     {
        if (data && data->id == id)
          {
             switch( data->type )
               {
                  case E_DEVICEMGR_HWKEY:
                     e_devicemgr.num_hwkey_devices--;
                     SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave H/W key device (id=%d, name=%s, type=%d) was removed/disabled !\n",
                                 __FUNCTION__, id, data->name, type);

                     e_devicemgr.device_list = eina_list_remove(e_devicemgr.device_list, data);
                     free(data);
                     goto out;

                  case E_DEVICEMGR_KEYBOARD:
                     e_devicemgr.num_keyboard_devices--;

                     if (e_devicemgr.num_keyboard_devices <= 0)
                       {
                          e_devicemgr.num_keyboard_devices = 0;
                          _e_devicemgr_set_keyboard_exist(0, 0);
                       }

                     SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave keyboard device (id=%d, name=%s, type=%d, num_keyboard_devices=%d) was removed/disabled !\n",
                                __FUNCTION__, id, data->name, type, e_devicemgr.num_keyboard_devices);

                     e_devicemgr.device_list = eina_list_remove(e_devicemgr.device_list, data);
                     free(data);
                     goto out;

                  case E_DEVICEMGR_MOUSE:
                     e_devicemgr.num_pointer_devices--;

                     if (e_devicemgr.num_pointer_devices <= 0)
                       {
                          e_devicemgr.num_pointer_devices = 0;
                          _e_devicemgr_set_mouse_exist(0, 1);
                       }

                     SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave pointer device (id=%d, name=%s, type=%d, num_pointer_devices=%d) was removed/disabled !\n",
                                __FUNCTION__, id, data->name, type, e_devicemgr.num_pointer_devices);
                     if (e_devicemgr.virtual_touchpad_id == id)
                       {
                          e_devicemgr.virtual_touchpad_id = -1;
                          _e_devicemgr_virtual_touchpad_helper_enable(EINA_FALSE);
                       }
                     e_devicemgr.device_list = eina_list_remove(e_devicemgr.device_list, data);
                     free(data);
                     goto out;

                  case E_DEVICEMGR_TOUCHSCREEN:
                     if (strcasestr(data->name, "virtual") && strcasestr(data->name, "multitouch"))
                       _e_devicemgr_virtual_multitouch_helper_fini();
                     e_devicemgr.num_touchscreen_devices--;
                     SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Slave touchscreen device (id=%d, name=%s, type=%d, num_touchscreen_devices=%d) was removed/disabled !\n",
                                __FUNCTION__, id, data->name, type, e_devicemgr.num_touchscreen_devices);

                     e_devicemgr.device_list = eina_list_remove(e_devicemgr.device_list, data);
                     free(data);
                     goto out;

                  default:
                     SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Unknown type of device ! (id=%d, type=%d, name=%s, device type=%d)\n",
                                __FUNCTION__, data->id, type, data->name, data->type);
                     e_devicemgr.device_list = eina_list_remove(e_devicemgr.device_list, data);
                     free(data);
                     goto out;
               }
          }
     }

out:
   return;
}

static void
_e_devicemgr_master_pointer_added(int id)
{
   int ndevices;
   XIDeviceInfo *info = NULL;

   info = XIQueryDevice(e_devicemgr.disp, id, &ndevices);

   if (!info || ndevices <= 0)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][master_pointer_added] There is no queried XI device. (device id=%d)\n", id);
        goto out;
     }

   if (info->use != XIMasterPointer) goto out;

   //Now we have a MasterPointer.
   if (strcasestr(E_NEW_MASTER_NAME" pointer", info->name))
     {
        //SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][master_pointer_added] XIMasterPointer is added !(id=%d, name=%s)\n", info->deviceid, info->name);
        e_devicemgr.new_master_pointer_id = info->deviceid;

        Eina_List *l;
        Eina_List *l2;
        int touchscreen_id = -1;
        DeviceMgr_Device_Info *devinfo;

        EINA_LIST_FOREACH_SAFE(e_devicemgr.device_list, l, l2, devinfo)
          {
             if (!devinfo) continue;
             if (devinfo->type == E_DEVICEMGR_TOUCHSCREEN)
               {
                  touchscreen_id = devinfo->id;
                  break;
               }
          }

        if (touchscreen_id != -1)
          _e_devicemgr_reattach_slave
             (touchscreen_id, e_devicemgr.new_master_pointer_id);
     }

out:
   if (info) XIFreeDeviceInfo(info);
}

static void
_e_devicemgr_master_pointer_removed(int id)
{
   if (e_devicemgr.new_master_pointer_id == id)
     {
        //SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][master_pointer_removed] XIMasterPointer was removed ! (id=%d, name=%s)\n",
        //           id, E_NEW_MASTER_NAME" pointer");
        e_devicemgr.new_master_pointer_id = -1;

        Eina_List *l;
        Eina_List *l2;
        int touchscreen_id = -1;
        DeviceMgr_Device_Info *devinfo;

        EINA_LIST_FOREACH_SAFE(e_devicemgr.device_list, l, l2, devinfo)
          {
             if (!devinfo) continue;
             if (devinfo->type == E_DEVICEMGR_TOUCHSCREEN)
               {
                  touchscreen_id = devinfo->id;
                  break;
               }
          }

        if (touchscreen_id != -1)
          _e_devicemgr_reattach_slave
             (touchscreen_id, e_devicemgr.vcp_id);
     }
}

static void
_e_devicemgr_slave_switched(int deviceid, int sourceid)
{
   unsigned int val;
   Eina_List *l;
   DeviceMgr_Device_Info *device_info;

   EINA_LIST_FOREACH(e_devicemgr.device_list, l, device_info)
     {
        if (!device_info || device_info->id!=sourceid) continue;
        if (device_info->type==E_DEVICEMGR_TOUCHSCREEN)
          {
             val = 1;
             ecore_x_window_prop_card32_set(e_devicemgr.rootWin, e_devicemgr.atomTouchInput, &val, 1);
          }
        else if (device_info->type==E_DEVICEMGR_MOUSE)
          {
             val = 0;
             ecore_x_window_prop_card32_set(e_devicemgr.rootWin, e_devicemgr.atomTouchInput, &val, 1);
          }
     }
}

static void
_e_devicemgr_device_changed(int deviceid, int sourceid)
{
   //SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][device_change_handler] deviceid:%d, sourceid:%d\n", deviceid, sourceid);
}

static void
_e_devicemgr_set_confine_information(int deviceid, E_Zone *zone, Eina_Bool isset, int region[4], Eina_Bool pointer_warp, Eina_Bool confine)
{
   int confine_region[6];

   if (isset && !zone && !region)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][set_confine_information] zone or region is needed for setting confine information !\n");
        return;
     }

   if (isset)
     {
        if (zone)
          {
             confine_region[0] = zone->x;
             confine_region[1] = zone->y;
             confine_region[2] = zone->x + zone->w;
             confine_region[3] = zone->y + zone->h;
             //SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][set_confine_information][zone] x=%d, y=%d, w=%d, h=%d\n", confine_region[0], confine_region[1], confine_region[2], confine_region[3]);
          }
        else
          {
             confine_region[0] = region[0];
             confine_region[1] = region[1];
             confine_region[2] = region[2];
             confine_region[3] = region[3];
             //SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][set_confine_information][region] x=%d, y=%d, w=%d, h=%d\n", confine_region[0], confine_region[1], confine_region[2], confine_region[3]);
          }
        if (pointer_warp) confine_region[4] = 1;
        else confine_region[4] = 0;
        if (confine) confine_region[5] = 1;
        else confine_region[5] = 0;
        XIChangeProperty(e_devicemgr.disp, deviceid, e_devicemgr.atomVirtualTouchpadConfineRegion,
                                       XA_INTEGER, 32, PropModeReplace, (unsigned char*)&confine_region[0], 6);
        XFlush(e_devicemgr.disp);
        XSync(e_devicemgr.disp, False);
     }
   else
     {
        confine_region[0] = 0;
        XIChangeProperty(e_devicemgr.disp, deviceid, e_devicemgr.atomVirtualTouchpadConfineRegion,
                                       XA_INTEGER, 32, PropModeReplace, (unsigned char*)&confine_region[0], 1);
        XFlush(e_devicemgr.disp);
        XSync(e_devicemgr.disp, False);
     }
}

static void
_e_devicemgr_set_mouse_exist(unsigned int val, int propset)
{
   if (!val)
     {
        char* cmds[] = {"e_devicemgr", "cursor_enable", "0", NULL };
        e_devicemgr.rroutput_buf_len = _e_devicemgr_marshalize_string (e_devicemgr.rroutput_buf,3, cmds);

        XRRChangeOutputProperty(e_devicemgr.disp, e_devicemgr.output, e_devicemgr.atomRROutput, XA_CARDINAL, 8, PropModeReplace, (unsigned char *)e_devicemgr.rroutput_buf, e_devicemgr.rroutput_buf_len);
        XSync(e_devicemgr.disp, False);

        if (propset) ecore_x_window_prop_card32_set(e_devicemgr.rootWin, e_devicemgr.atomXMouseExist, &val, 1);
     }
   else if (1 == val)
     {
        char* cmds[] = {"e_devicemgr", "cursor_enable", "1", NULL };
        e_devicemgr.rroutput_buf_len = _e_devicemgr_marshalize_string (e_devicemgr.rroutput_buf,3, cmds);

        XRRChangeOutputProperty(e_devicemgr.disp, e_devicemgr.output, e_devicemgr.atomRROutput, XA_CARDINAL, 8, PropModeReplace, (unsigned char *)e_devicemgr.rroutput_buf, e_devicemgr.rroutput_buf_len);
        XSync(e_devicemgr.disp, False);

        if (propset) ecore_x_window_prop_card32_set(e_devicemgr.rootWin, e_devicemgr.atomXMouseExist, &val, 1);
     }
   else
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][%s] Invalid value for enabling cursor !(val=%d)\n", __FUNCTION__, val);
     }
}

static void
_e_devicemgr_set_keyboard_exist(unsigned int val, int is_connected)
{
   ecore_x_window_prop_card32_set(e_devicemgr.rootWin, e_devicemgr.atomXExtKeyboardExist, &val, 1);
}

static int
_e_devicemgr_get_lockmodifier_mask(void)
{
   Window dummy1, dummy2;
   int dummy3, dummy4, dummy5, dummy6;
   unsigned int mask;

   XQueryPointer(e_devicemgr.disp, DefaultRootWindow(e_devicemgr.disp), &dummy1, &dummy2,
                            &dummy3, &dummy4, &dummy5, &dummy6, &mask);
   return (mask & (NumLockMask | CapsLockMask));
}

static int
_e_devicemgr_xkb_set_on(unsigned int mask)
{
   if (!mask) return 0;

   XkbLockModifiers(e_devicemgr.disp, XkbUseCoreKbd, mask, mask);
   return 1;
}

static int
_e_devicemgr_lockmodifier_set(void)
{
   unsigned int mask;

   if (e_devicemgr.xkb_available != EINA_TRUE) return -1;

   //Get current numlock/capslock status from Xserver
   mask = _e_devicemgr_get_lockmodifier_mask();
   SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][lockmodifier_set] NumLock mask=%d, CapsLock mask=%d\n",
               NumLockMask & mask, CapsLockMask & mask);

   //If one of lockmodiers is set, try to turn it on for all keyboard devices.
   if (mask && _e_devicemgr_xkb_set_on(mask)) return 1;

   return 0;
}

static Eina_Bool
_e_devicemgr_create_master_device(char* master_name)
{
   int ret;
   XIAddMasterInfo c;

   if (!master_name)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][create_master_device] name of master device is needed !\n");
        return EINA_FALSE;
     }

   c.type = XIAddMaster;
   c.name = master_name;
   c.send_core = 1;
   c.enable = 1;

   ret = XIChangeHierarchy(e_devicemgr.disp, (XIAnyHierarchyChangeInfo*)&c, 1);
   XFlush(e_devicemgr.disp);
   XSync(e_devicemgr.disp, False);

   if (ret!=Success) return EINA_FALSE;

   SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][create_master_device] new master (%s) was created !\n", E_NEW_MASTER_NAME);

   return EINA_TRUE;
}

static Eina_Bool
_e_devicemgr_remove_master_device(int master_id)
{
   int ret;
   XIRemoveMasterInfo r;

   if (master_id < 0)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][remove_master_device] master_id(%d) is invalid !\n", master_id);
	 return EINA_FALSE;
     }

   r.type = XIRemoveMaster;
   r.deviceid = master_id;
   r.return_mode = XIFloating;

   ret = XIChangeHierarchy(e_devicemgr.disp, (XIAnyHierarchyChangeInfo*)&r, 1);
   XFlush(e_devicemgr.disp);
   XSync(e_devicemgr.disp, False);

   if (ret!=Success) return EINA_FALSE;

   SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][remove_master_device] new master (%s) was removed !\n", E_NEW_MASTER_NAME);

   return EINA_TRUE;
}

static Eina_Bool
_e_devicemgr_reattach_slave(int slave_id, int master_id)
{
   int ret;
   XIAttachSlaveInfo attach;

   attach.type = XIAttachSlave;
   attach.deviceid = slave_id;
   attach.new_master = master_id;

   ret = XIChangeHierarchy(e_devicemgr.disp, (XIAnyHierarchyChangeInfo*)&attach, 1);
   XFlush(e_devicemgr.disp);
   XSync(e_devicemgr.disp, False);

   if (ret!=Success) return EINA_FALSE;

   SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][reattach_slave] slave (id=%d) was reattached to master (id:%d) !\n", slave_id, master_id);

   return EINA_TRUE;
}

static void
_e_devicemgr_show_device_list(unsigned int val)
{
   SLOG(LOG_DEBUG, "DEVICEMGR",  "\n[e_devicemgr] - Device List = Start =====================\n");

   if (e_devicemgr.device_list)
     {
        Eina_List* l;
        DeviceMgr_Device_Info *data;

        EINA_LIST_FOREACH(e_devicemgr.device_list, l, data)
          {
             if (data)
               {
                  SLOG(LOG_DEBUG, "DEVICEMGR",  "Device id : %d Name : %s\n", data->id, data->name);
                  switch (data->type)
                    {
                       case E_DEVICEMGR_HWKEY:
                          SLOG(LOG_DEBUG, "DEVICEMGR",  " : type : H/W Key\n");
                          break;

                       case E_DEVICEMGR_KEYBOARD:
                          SLOG(LOG_DEBUG, "DEVICEMGR",  " : type : Keyboard\n");
                          break;

                       case E_DEVICEMGR_MOUSE:
                          SLOG(LOG_DEBUG, "DEVICEMGR",  " : type : Mouse\n");
                          break;

                       case E_DEVICEMGR_TOUCHSCREEN:
                          SLOG(LOG_DEBUG, "DEVICEMGR",  " : type : Touchscreen\n");
                          break;

                       default:
                          SLOG(LOG_DEBUG, "DEVICEMGR",  " : type : Unknown\n");
                    }
               }
          }
     }
   else
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "No input devices...\n");
     }

   SLOG(LOG_DEBUG, "DEVICEMGR",  "\n[e_devicemgr] - Device List = End =====================\n");
}

static Eina_Bool
_e_devicemgr_virtual_touchpad_helper_enable(Eina_Bool is_enable)
{
   Eina_Bool result;

   if (is_enable)
     {
        if (e_devicemgr.num_zones < 2) return EINA_FALSE;
        result = _e_devicemgr_create_master_device(E_NEW_MASTER_NAME);
        if (EINA_FALSE==result)
          {
             SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][virtual_touchpad_helper_enable] Failed to create master device ! (name=%s)\n",
                        E_NEW_MASTER_NAME);
             return EINA_FALSE;
          }
        _e_devicemgr_set_confine_information(e_devicemgr.virtual_touchpad_id, _e_devicemgr_get_nth_zone(2), EINA_TRUE, NULL, EINA_TRUE, EINA_FALSE);
     }
   else
     {
        result = _e_devicemgr_remove_master_device(e_devicemgr.new_master_pointer_id);
        if (EINA_FALSE==result)
          {
             SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][virtual_touchpad_helper_enable] Failed to remove master device ! (id=%d)\n",
                        e_devicemgr.new_master_pointer_id);
             return EINA_FALSE;
          }
        //_e_devicemgr_set_confine_information(e_devicemgr.virtual_touchpad_id, NULL, EINA_FALSE, NULL, EINA_FALSE);
     }

   return EINA_TRUE;
}

static E_Zone*
_e_devicemgr_get_nth_zone(int index)
{
   Eina_List *l;
   E_Zone *zone;
   int count = 0;

   if (e_devicemgr.num_zones < index)
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][get_nth_zone] %d th zone doesn't exist ! (num_zones=%d)\n",
                   index, e_devicemgr.num_zones);
        return NULL;
     }

   EINA_LIST_FOREACH(e_devicemgr.zones, l, zone)
     {
        if (zone)
          {
             if (count==(index-1)) return zone;
             else count++;
          }
     }

   return NULL;
}

static void
_e_devicemgr_virtual_multitouch_helper_init(int deviceid)
{
   int i;

   for ( i=0 ; i < MAX_TOUCH ; i++ )
     {
        if (e_devicemgr.virtual_multitouch_id[i] < 0)
          {
             e_devicemgr.virtual_multitouch_id[i] = deviceid;
             break;
          }
     }

   if (i < MAX_TOUCH-1) return;

   SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][virtual_multitouch_helper_init] virtual touchscreen device were attached !\n");

   e_devicemgr.mouse_in_handler = ecore_event_handler_add(ECORE_X_EVENT_MOUSE_IN, (Ecore_Event_Handler_Cb)_e_devicemgr_cb_mouse_in, NULL);
   e_devicemgr.border_move_end_hook = e_client_hook_add(E_CLIENT_HOOK_MOVE_END, _e_devicemgr_hook_border_move_end, NULL);
   e_devicemgr.border_resize_end_hook = e_client_hook_add(E_CLIENT_HOOK_MOVE_END, _e_devicemgr_hook_border_resize_end, NULL);

   if (!e_devicemgr.mouse_in_handler) SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to add ECORE_X_EVENT_MOUSE_IN handler\n", __FUNCTION__);
   if (!e_devicemgr.border_move_end_hook) SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to add E_BORDER_HOOK_MOVE_END hook\n", __FUNCTION__);
   if (!e_devicemgr.border_resize_end_hook) SLOG(LOG_DEBUG, "DEVICEMGR", "[e_devicemgr][%s] Failed to add E_BORDER_HOOK_RESIZE_END hook\n", __FUNCTION__);
}

static void
_e_devicemgr_virtual_multitouch_helper_fini(void)
{
   SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][virtual_multitouch_helper_init] virtual touchscreen device(s) were removed !\n");
   memset(&e_devicemgr.virtual_multitouch_id, -1, sizeof(e_devicemgr.virtual_multitouch_id));

   if (e_devicemgr.mouse_in_handler) ecore_event_handler_del(e_devicemgr.mouse_in_handler);
   if (e_devicemgr.border_move_end_hook) e_client_hook_del(e_devicemgr.border_move_end_hook);
   if (e_devicemgr.border_resize_end_hook) e_client_hook_del(e_devicemgr.border_resize_end_hook);

   e_devicemgr.mouse_in_handler = NULL;
   e_devicemgr.border_move_end_hook = NULL;
   e_devicemgr.border_resize_end_hook = NULL;
}

static void
_e_devicemgr_update_input_transform_matrix(Eina_Bool reset)
{
   int i;

   static float identity_matrix[] = { 1.0f, 0, 0, 0, 1.0f, 0, 0, 0, 1.0f };

   if (0 > e_devicemgr.virtual_multitouch_id[0])
     {
        SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][update_input_transform_matrix] e_devicemgr.virtual_multitouch_id is invalid !\n");
        return;
     }

   for( i = 0 ; i < 3 ; i++ )
     {
        if (reset)
          XIChangeProperty(e_devicemgr.disp, e_devicemgr.virtual_multitouch_id[i], e_devicemgr.atomInputTransform,
                                         e_devicemgr.atomFloat, 32, PropModeReplace, (unsigned char*)&identity_matrix, 9);
        else
          XIChangeProperty(e_devicemgr.disp, e_devicemgr.virtual_multitouch_id[i], e_devicemgr.atomInputTransform,
                                         e_devicemgr.atomFloat, 32, PropModeReplace, (unsigned char*)&e_devicemgr.tmatrix[0], 9);
     }

   XFlush(e_devicemgr.disp);
   XSync(e_devicemgr.disp, False);

   if (reset) SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][update_input_transform_matrix] transform matrix was reset to identity_matrix !\n");
   else SLOG(LOG_DEBUG, "DEVICEMGR",  "[e_devicemgr][update_input_transform_matrix] transform matrix was updated !\n");
}

