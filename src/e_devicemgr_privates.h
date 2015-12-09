#ifndef _E_DEVICEMGR_PRIVATE_H_
#define _E_DEVICEMGR_PRIVATE_H_

#include "e.h"
#define LOG_TAG	"DEVICEMGR"
#include "dlog.h"

extern int _log_dom;
#undef ERR
#undef DBG
#undef INF
#undef WRN
#undef CRI
#define CRI(...) EINA_LOG_DOM_CRIT(_log_dom, __VA_ARGS__)
#define ERR(...) EINA_LOG_DOM_ERR(_log_dom, __VA_ARGS__)
#define WRN(...) EINA_LOG_DOM_WARN(_log_dom, __VA_ARGS__)
#define INF(...) EINA_LOG_DOM_INFO(_log_dom, __VA_ARGS__)
#define DBG(...) EINA_LOG_DOM_DBG(_log_dom, __VA_ARGS__)

#undef SLOG
#define SLOG(priority, tag, format, arg...) printf(format, ##arg)

#ifndef CLEAR
#define CLEAR(x) memset(&(x), 0, sizeof (x))
#endif

#undef NEVER_GET_HERE
#define NEVER_GET_HERE()     CRI("** need to improve more **")

#if 0
#undef DBG
#define DBG(fmt, ARG...)      printf("@@@ [%s:%d] "fmt"\n", __FUNCTION__, __LINE__, ##ARG)
#endif

#endif//_E_DEVICEMGR_PRIVATE_H_
