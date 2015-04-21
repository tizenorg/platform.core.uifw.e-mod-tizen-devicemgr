#ifndef __E_DEVICEMGR_INPUT_H__
#define __E_DEVICEMGR_INPUT_H__

#include "config.h"

#include "e.h"

#ifndef HAVE_WAYLAND_ONLY
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XI2.h>
#include <X11/extensions/XIproto.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XKB.h>

#ifndef LockMask
#define LockMask (1<<1)
#define Mod2Mask (1<<4)
#endif
#define CapsLockMask LockMask
#define NumLockMask Mod2Mask

#ifndef MAX_TOUCH
#define MAX_TOUCH 3
#endif

#define INSIDE(x, y, x1, y1, x2, y2)	(x1 <= x && x <= x2 && y1 <= y && y <= y2)

#define E_PROP_DEVICE_LIST "E_DEVICEMGR_DEVICE_LIST"

#define E_PROP_DEVICE_NAME "Device"
#define E_PROP_DEVICEMGR_INPUTWIN "DeviceMgr Input Window"
#define E_PROP_VIRTUAL_TOUCHPAD_INT "Virtual Touchpad Interaction"
#define E_PROP_X_MOUSE_CURSOR_ENABLE "X Mouse Cursor Enable"
#define E_PROP_X_MOUSE_EXIST "X Mouse Exist"
#define E_PROP_X_EXT_KEYBOARD_EXIST "X External Keyboard Exist"
#define E_PROP_HW_KEY_INPUT_STARTED "HW Keyboard Input Started"
#define E_PROP_X_EVDEV_AXIS_LABELS "Axis Labels"
#define E_PROP_XRROUTPUT "X_RR_PROPERTY_REMOTE_CONTROLLER"
#define E_PROP_VIRTUAL_TOUCHPAD "_X_Virtual_Touchpad_"
#define E_PROP_TOUCH_INPUT "X_TouchInput"
#define E_PROP_VIRTUAL_TOUCHPAD_CONFINE_REGION "Evdev Confine Region"
#define E_NEW_MASTER_NAME "New Master"
#define E_VIRTUAL_TOUCHPAD_NAME "Virtual Touchpad"
#define EVDEVMULTITOUCH_PROP_TRANSFORM "EvdevMultitouch Transform Matrix"
#define XATOM_FLOAT "FLOAT"

#define DEVICEMGR_PREFIX "/usr/lib/enlightenment/modules/e-mod-tizen-devicemgr/"

typedef enum _VirtualTouchpad_MsgType
{
   E_VIRTUAL_TOUCHPAD_NEED_TO_INIT,
   E_VIRTUAL_TOUCHPAD_DO_INIT,
   E_VIRTUAL_TOUCHPAD_AREA_INFO,
   E_VIRTUAL_TOUCHPAD_POINTED_WINDOW,
   E_VIRTUAL_TOUCHPAD_WINDOW,
   E_VIRTUAL_TOUCHPAD_MT_BEGIN,
   E_VIRTUAL_TOUCHPAD_MT_END,
   E_VIRTUAL_TOUCHPAD_MT_MATRIX_SET_DONE,
   E_VIRTUAL_TOUCHPAD_CONFINE_SET,
   E_VIRTUAL_TOUCHPAD_CONFINE_UNSET,
   E_VIRTUAL_TOUCHPAD_SHUTDOWN
} VirtualTouchpad_MsgType;

typedef enum
{
   E_DEVICEMGR_HWKEY= 1,
   E_DEVICEMGR_KEYBOARD,
   E_DEVICEMGR_MOUSE,
   E_DEVICEMGR_TOUCHSCREEN
} DeviceMgrDeviceType;

typedef struct _DeviceMgr_Device_Info DeviceMgr_Device_Info;

struct _DeviceMgr_Device_Info
{
   int id;
   const char *name;
   DeviceMgrDeviceType type;
};

typedef struct _DeviceMgr_
{
   Ecore_X_Display* disp;
   Ecore_X_Window rootWin;
   Ecore_X_Window input_window;
   int num_zones;
   Eina_List *zones;

   Eina_Bool xkb_available;

   Ecore_X_Atom atomRROutput;
   Ecore_X_Atom atomAxisLabels;
   Ecore_X_Atom atomXMouseExist;
   Ecore_X_Atom atomXMouseCursorEnable;
   Ecore_X_Atom atomXExtKeyboardExist;
   Ecore_X_Atom atomHWKbdInputStarted;
   Ecore_X_Atom atomDeviceList;
   Ecore_X_Atom atomDeviceName;
   Ecore_X_Atom atomVirtualTouchpadConfineRegion;
   Ecore_X_Atom atomVirtualTouchpad;
   Ecore_X_Atom atomTouchInput;
   Ecore_X_Atom atomInputTransform;
   Ecore_X_Atom atomFloat;
   Ecore_X_Atom atomVirtualTouchpadInt;
   Ecore_X_Atom atomDeviceMgrInputWindow;

   int num_touchscreen_devices;
   int num_pointer_devices;
   int num_keyboard_devices;
   int num_hwkey_devices;
   Eina_List *device_list;

   Ecore_Event_Handler *window_property_handler;
   Ecore_Event_Handler *event_generic_handler;
   Ecore_Event_Handler *zone_add_handler;
   Ecore_Event_Handler *zone_del_handler;
   Ecore_Event_Handler *mouse_in_handler;
   Ecore_Event_Handler *client_message_handler;
   E_Client_Hook *border_move_end_hook;
   E_Client_Hook *border_resize_end_hook;

   //variables to set XRROutputProperty
   RROutput output;
   char rroutput_buf[256];
   int rroutput_buf_len;

   //variables related to XI2
   int xi2_opcode;
   XIEventMask eventmask;

    //XIMasterPointer id(s)
   int vcp_id;
   int vck_id;
   int vcp_xtest_pointer_id;
   int vck_xtest_keyboard_id;
   int new_master_pointer_id;

   int virtual_touchpad_id;
   int virtual_multitouch_id[MAX_TOUCH];
   int virtual_touchpad_area_info[4];
   int virtual_touchpad_pointed_window_info[4];
   int virtual_multitouch_done;
   int virtual_touchpad_cursor_pos[2];

   Ecore_X_Window virtual_touchpad_window;
   Ecore_X_Window virtual_touchpad_pointed_window;

   //input transform matrix
   float tmatrix[9];
} DeviceMgr;
#endif

int e_devicemgr_input_init(void);
void e_devicemgr_input_fini(void);

#endif
