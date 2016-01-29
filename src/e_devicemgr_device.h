#ifndef __E_DEVICEMGR_DEVICE_H__
#define __E_DEVICEMGR_DEVICE_H__

#define E_COMP_WL
#include "e.h"
#include <Ecore_Drm.h>
#include "e_comp_wl.h"
#include <wayland-server.h>

typedef struct _e_devicemgr_input_devmgr_data e_devicemgr_input_devmgr_data;

struct _e_devicemgr_input_devmgr_data
{
   Eina_List *block_clients_kbd;
   Eina_List *block_clients_mouse;
   Eina_List *block_clients_touch;

   Eina_List *block_surfaces_kbd;
   Eina_List *block_surfaces_mouse;
   Eina_List *block_surfaces_touch;

   Eina_List *disable_clients_kbd;
   Eina_List *disable_clients_mouse;
   Eina_List *disable_clients_touch;

   Eina_List *disable_surfaces_kbd;
   Eina_List *disable_surfaces_mouse;
   Eina_List *disable_surfaces_touch;
};

int e_devicemgr_device_init(void);
void e_devicemgr_device_fini(void);

#endif
