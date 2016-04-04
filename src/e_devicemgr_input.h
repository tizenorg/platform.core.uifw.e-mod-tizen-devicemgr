#ifndef __E_DEVICEMGR_INPUT_H__
#define __E_DEVICEMGR_INPUT_H__

#include "config.h"

#include "e.h"

/* FIXME: Optional values will be set in configuration file. */
/* FIXME: keycode will be get using keymap */
#define E_DEVICEMGR_INPUT_BACK_KEYCODE 166
#define E_DEVICEMGR_INPUT_MOUSE_REMAP_ENABLED EINA_TRUE

int e_devicemgr_input_init(void);
void e_devicemgr_input_fini(void);

#endif
