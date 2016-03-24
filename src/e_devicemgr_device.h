#ifndef __E_DEVICEMGR_DEVICE_H__
#define __E_DEVICEMGR_DEVICE_H__

#define E_COMP_WL
#include "e.h"
#include <Ecore_Drm.h>
#include "e_comp_wl.h"
#include <wayland-server.h>

#include <linux/uinput.h>
#include <xkbcommon/xkbcommon.h>

#ifdef TRACE_INPUT_BEGIN
#undef TRACE_INPUT_BEGIN
#endif
#ifdef TRACE_INPUT_END
#undef TRACE_INPUT_END
#endif

#ifdef ENABLE_TTRACE
#include <ttrace.h>

#define TRACE_INPUT_BEGIN(NAME) traceBegin(TTRACE_TAG_INPUT, "INPUT:DEVMGR:"#NAME)
#define TRACE_INPUT_END() traceEnd(TTRACE_TAG_INPUT)
#else
#define TRACE_INPUT_BEGIN(NAME)
#define TRACE_INPUT_END()
#endif

#define DMERR(msg, ARG...) ERR("[tizen_devicemgr][%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)
#define DMWRN(msg, ARG...) WRN("[tizen_devicemgr][%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)
#define DMINF(msg, ARG...) INF("[tizen_devicemgr][%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)
#define DMDBG(msg, ARG...) DBG("[tizen_devicemgr][%s:%d] "msg, __FUNCTION__, __LINE__, ##ARG)

#ifdef ENABLE_CYNARA
#include <cynara-session.h>
#include <cynara-client.h>
#include <cynara-creds-socket.h>
#endif

typedef struct _e_devicemgr_input_devmgr_data e_devicemgr_input_devmgr_data;

struct _e_devicemgr_input_devmgr_data
{
   unsigned int block_devtype;
   struct wl_client *block_client;
   Ecore_Timer *duration_timer;
   Ecore_Event_Filter *event_filter;
#ifdef ENABLE_CYNARA
   cynara *p_cynara;
   Eina_Bool cynara_initialized;
#endif

   unsigned int pressed_button;

   struct
   {
      struct uinput_user_dev uinp;
      int uinp_fd;
      char *uinp_identifier;
      unsigned int ref;
   }inputgen;

   struct
   {
      char *identifier;
      int wheel_click_angle;
   }detent;
};

int e_devicemgr_device_init(void);
void e_devicemgr_device_fini(void);

#endif
