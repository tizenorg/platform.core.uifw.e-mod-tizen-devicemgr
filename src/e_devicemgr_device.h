#ifndef __E_DEVICEMGR_DEVICE_H__
#define __E_DEVICEMGR_DEVICE_H__

#define E_COMP_WL
#include "e.h"
#include <Ecore_Drm.h>
#include "e_comp_wl.h"
#include <wayland-server.h>

#ifdef TRACE_BEGIN
#undef TRACE_BEGIN
#endif
#ifdef TRACE_END
#undef TRACE_END
#endif

#ifdef ENABLE_TTRACE
#include <ttrace.h>

#define TRACE_BEGIN(NAME) traceBegin(TTRACE_TAG_INPUT, "INPUT:DEVMGR:"#NAME)
#define TRACE_END() traceEnd(TTRACE_TAG_INPUT)
#else
#define TRACE_BEGIN(NAME)
#define TRACE_END()
#endif

typedef struct _e_devicemgr_input_devmgr_data e_devicemgr_input_devmgr_data;

struct _e_devicemgr_input_devmgr_data
{
   unsigned int block_devtype;
   struct wl_client *block_client;
   Ecore_Timer *duration_timer;
};

int e_devicemgr_device_init(void);
void e_devicemgr_device_fini(void);

#endif
