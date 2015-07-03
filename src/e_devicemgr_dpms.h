#ifndef __E_DEVICEMGR_DPMS_H__
#define __E_DEVICEMGR_DPMS_H__

#include "e.h"
#include <Ecore_Drm.h>

int e_devicemgr_dpms_init(void);
void e_devicemgr_dpms_fini(void);

unsigned int e_devicemgr_dpms_get(Ecore_Drm_Output *output);

#endif
