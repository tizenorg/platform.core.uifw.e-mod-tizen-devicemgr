#ifndef _E_DEVICEMGR_PRIVATE_H_
#define _E_DEVICEMGR_PRIVATE_H_

#include "e.h"
#define LOG_TAG	"DEVICEMGR"
#include "dlog.h"

#undef SLOG
#define SLOG(priority, tag, format, arg...) printf(format, ##arg)

#endif//_E_DEVICEMGR_PRIVATE_H_
