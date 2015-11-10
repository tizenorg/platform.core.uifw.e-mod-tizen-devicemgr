#define E_COMP_WL
#include <e.h>
#include <Ecore_Drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>
#include "e_mod_main.h"
#include "e_devicemgr_privates.h"
#include "e_devicemgr_drm.h"
#include "e_devicemgr_dpms.h"

/* use drm event handler of ecore_drm */
//#define USE_DRM_USER_HANDLER

struct drm_handler_info
{
   void *func;
   void *data;
};

#ifdef USE_DRM_USER_HANDLER
static Eina_List *drm_vblank_handlers;
static Eina_List *drm_ipp_handlers;
static Ecore_Fd_Handler *drm_hdlr;

#endif /* USE_DRM_USER_HANDLER */
int e_devmgr_drm_fd = -1;
tbm_bufmgr e_devmgr_bufmgr = NULL;

typedef struct _Drm_Event_Context {
    void (*page_flip_handler)(int fd,
                              unsigned int sequence,
                              unsigned int tv_sec,
                              unsigned int tv_usec,
                              void *user_data);
    void (*vblank_handler)(int fd,
                           unsigned int sequence,
                           unsigned int tv_sec,
                           unsigned int tv_usec,
                           void *user_data);
    void (*ipp_handler)(int fd,
                        unsigned int  prop_id,
                        unsigned int *buf_idx,
                        unsigned int  tv_sec,
                        unsigned int  tv_usec,
                        void *user_data);
} Drm_Event_Context;

#ifdef USE_DRM_USER_HANDLER
static void
_e_devicemgr_drm_cb_page_flip(int fd, unsigned int sequence,
                              unsigned int tv_sec, unsigned int tv_usec,
                              void *user_data)
{
   Ecore_Drm_Event_Page_Flip *e;

   e = calloc(1, sizeof(Ecore_Drm_Event_Page_Flip));
   if (!e)
     return;

   e->fd = fd;
   e->sequence = sequence;
   e->sec = tv_sec;
   e->usec = tv_usec;
   e->data = user_data;
   ecore_event_add(ECORE_DRM_EVENT_PAGE_FLIP, e, NULL, NULL);
}

static void
_e_devicemgr_drm_cb_vblank(int fd, unsigned int sequence,
                           unsigned int tv_sec, unsigned int tv_usec,
                           void *user_data)
{
   struct drm_handler_info *info;
   Eina_List *l;

   EINA_LIST_FOREACH(drm_vblank_handlers, l, info)
     {
        if (info->data == user_data && info->func)
          ((Drm_Vblank_Func)(info->func))(sequence, tv_sec, tv_usec, user_data);
     }

}

static void
_e_devicemgr_drm_cb_ipp(int fd, unsigned int prop_id, unsigned int *buf_idx,
                        unsigned int  tv_sec, unsigned int  tv_usec,
                        void *user_data)
{
   struct drm_handler_info *info;
   Eina_List *l;

   EINA_LIST_FOREACH(drm_ipp_handlers, l, info)
     {
        if (info->data == user_data && info->func)
          ((Drm_Ipp_Func)(info->func))(prop_id, buf_idx, tv_sec, tv_usec, user_data);
     }
}

static int
_e_devicemgr_drm_handle_event(int fd, Drm_Event_Context *evctx)
{
#define MAX_BUF_SIZE    1024

    char buffer[MAX_BUF_SIZE];
    unsigned int len, i;
    struct drm_event *e;

    /* The DRM read semantics guarantees that we always get only
     * complete events. */
    len = read(fd, buffer, sizeof buffer);
    if (len == 0)
    {
        ERR("warning: the size of the drm_event is 0.");
        return 0;
    }
    if (len < sizeof *e)
    {
        ERR("warning: the size of the drm_event is less than drm_event structure.");
        return -1;
    }
    if (len > MAX_BUF_SIZE - sizeof(struct drm_exynos_ipp_event))
    {
        ERR("warning: the size of the drm_event can be over the maximum size.");
        return -1;
    }

    i = 0;
    while (i < len)
    {
        e = (struct drm_event *) &buffer[i];
        switch (e->type)
        {
            case DRM_EVENT_VBLANK:
                {
                    struct drm_event_vblank *vblank;

                    if (evctx->vblank_handler == NULL)
                        break;

                    vblank = (struct drm_event_vblank *) e;
                    DBG("******* VBLANK *******");
                    evctx->vblank_handler (fd,
                            vblank->sequence,
                            vblank->tv_sec,
                            vblank->tv_usec,
                            (void *)((unsigned long)vblank->user_data));
                    DBG("******* VBLANK *******...");
                }
                break;
            case DRM_EVENT_FLIP_COMPLETE:
                {
                    struct drm_event_vblank *vblank;

                    if (evctx->page_flip_handler == NULL)
                        break;

                    vblank = (struct drm_event_vblank *) e;
                    evctx->page_flip_handler (fd,
                            vblank->sequence,
                            vblank->tv_sec,
                            vblank->tv_usec,
                            (void *)((unsigned long)vblank->user_data));
                }
                break;
            case DRM_EXYNOS_IPP_EVENT:
                {
                    struct drm_exynos_ipp_event *ipp;

                    if (evctx->ipp_handler == NULL)
                        break;

                    ipp = (struct drm_exynos_ipp_event *) e;
                    DBG("******* IPP *******");
                    evctx->ipp_handler (fd,
                            ipp->prop_id,
                            ipp->buf_id,
                            ipp->tv_sec,
                            ipp->tv_usec,
                            (void *)((unsigned long)ipp->user_data));
                    DBG("******* IPP *******...");
                }
                break;
            default:
                break;
        }
        i += e->length;
    }

    return 0;
}

static Eina_Bool
_e_devicemgr_drm_cb_event(void *data, Ecore_Fd_Handler *hdlr EINA_UNUSED)
{
   Drm_Event_Context ctx;

   memset(&ctx, 0, sizeof(Drm_Event_Context));

   ctx.page_flip_handler = _e_devicemgr_drm_cb_page_flip;
   ctx.vblank_handler = _e_devicemgr_drm_cb_vblank;
   ctx.ipp_handler = _e_devicemgr_drm_cb_ipp;

   _e_devicemgr_drm_handle_event(e_devmgr_drm_fd, &ctx);

   return ECORE_CALLBACK_RENEW;
}

static int
_e_devicemgr_drm_fd_get(void)
{
   Eina_List *devs;
   Ecore_Drm_Device *dev;

   if (e_devmgr_drm_fd >= 0)
     return e_devmgr_drm_fd;

   devs = eina_list_clone(ecore_drm_devices_get());
   EINA_SAFETY_ON_NULL_RETURN_VAL(devs, -1);

   if ((dev = eina_list_nth(devs, 0)))
     {
        e_devmgr_drm_fd = ecore_drm_device_fd_get(dev);
        if (e_devmgr_drm_fd >= 0)
          return e_devmgr_drm_fd;
     }

   eina_list_free(devs);
   return -1;
}
#endif /* USE_DRM_USER_HANDLER */

int
e_devicemgr_drm_init(void)
{
#ifdef USE_DRM_USER_HANDLER
   drmVersionPtr drm_info;

   if (!getenv("ECORE_DRM_DEVICE_USER_HANDLER"))
      return 1;

   if (_e_devicemgr_drm_fd_get() < 0)
      return 0;

   e_devmgr_bufmgr = tbm_bufmgr_init(e_devmgr_drm_fd);
   EINA_SAFETY_ON_NULL_RETURN_VAL(e_devmgr_bufmgr, 0);

   DBG("enable DRM user handler");

   drm_hdlr =
     ecore_main_fd_handler_add(e_devmgr_drm_fd, ECORE_FD_READ,
                               _e_devicemgr_drm_cb_event, NULL, NULL, NULL);
   EINA_SAFETY_ON_NULL_RETURN_VAL(drm_hdlr, 0);

   drm_info = drmGetVersion(e_devmgr_drm_fd);
   DBG("drm name: %s", drm_info->name);

   if (drm_info->name && !strncmp (drm_info->name, "exynos", 6))
     {
        e_comp->wl_comp_data->available_hw_accel.underlay = EINA_TRUE;
        DBG("enable HW underlay");
        e_comp->wl_comp_data->available_hw_accel.scaler = EINA_TRUE;
        DBG("enable HW scaler");
     }

   return 1;
#endif /* USE_DRM_USER_HANDLER */
   return 0;
}

void
e_devicemgr_drm_fini(void)
{
}

int
e_devicemgr_drm_vblank_handler_add(Drm_Vblank_Func func, void *data)
{
#ifdef USE_DRM_USER_HANDLER
   struct drm_handler_info *info;

   EINA_SAFETY_ON_NULL_RETURN_VAL(func, 0);
   EINA_SAFETY_ON_NULL_RETURN_VAL(data, 0);

   info = malloc(sizeof(struct drm_handler_info));
   EINA_SAFETY_ON_NULL_RETURN_VAL(info, 0);

   info->func = (void*)func;
   info->data = data;

   drm_vblank_handlers = eina_list_append(drm_vblank_handlers, info);

   return 1;
#endif /* USE_DRM_USER_HANDLER */
   return 0;
}

void
e_devicemgr_drm_vblank_handler_del(Drm_Vblank_Func func, void *data)
{
#ifdef USE_DRM_USER_HANDLER
   struct drm_handler_info *info;
   Eina_List *l, *ll;

   EINA_SAFETY_ON_NULL_RETURN(func);
   EINA_SAFETY_ON_NULL_RETURN(data);

   EINA_LIST_FOREACH_SAFE(drm_vblank_handlers, l, ll, info)
     {
        if (info->func == func && info->data == data)
          {
             drm_vblank_handlers = eina_list_remove(drm_vblank_handlers, info);
             free(info);
             return;
          }
     }
#endif /* USE_DRM_USER_HANDLER */
}

int
e_devicemgr_drm_ipp_handler_add(Drm_Ipp_Func func, void *data)
{
#ifdef USE_DRM_USER_HANDLER
   struct drm_handler_info *info;

   EINA_SAFETY_ON_NULL_RETURN_VAL(func, 0);
   EINA_SAFETY_ON_NULL_RETURN_VAL(data, 0);

   info = malloc(sizeof(struct drm_handler_info));
   EINA_SAFETY_ON_NULL_RETURN_VAL(info, 0);

   info->func = (void*)func;
   info->data = data;

   drm_ipp_handlers = eina_list_append(drm_ipp_handlers, info);

   return 1;
#endif /* USE_DRM_USER_HANDLER */
   return 0;
}

void
e_devicemgr_drm_ipp_handler_del(Drm_Ipp_Func func, void *data)
{
#ifdef USE_DRM_USER_HANDLER
   struct drm_handler_info *info;
   Eina_List *l, *ll;

   EINA_SAFETY_ON_NULL_RETURN(func);
   EINA_SAFETY_ON_NULL_RETURN(data);

   EINA_LIST_FOREACH_SAFE(drm_ipp_handlers, l, ll, info)
     {
        if (info->func == func && info->data == data)
          {
             drm_ipp_handlers = eina_list_remove(drm_ipp_handlers, info);
             free(info);
             return;
          }
     }
#endif /* USE_DRM_USER_HANDLER */
}

int
e_devicemgr_drm_set_property(unsigned int obj_id, unsigned int obj_type,
                             const char *prop_name, unsigned int value)
{
#ifdef USE_DRM_USER_HANDLER
   drmModeObjectPropertiesPtr props = NULL;
   unsigned int i;

   EINA_SAFETY_ON_FALSE_RETURN_VAL(obj_id > 0, 0);
   EINA_SAFETY_ON_FALSE_RETURN_VAL(obj_type > 0, 0);
   EINA_SAFETY_ON_NULL_RETURN_VAL(prop_name, 0);

   props = drmModeObjectGetProperties(e_devmgr_drm_fd, obj_id, obj_type);
   if (!props)
     {
        char errbuf[128] = {0,};
        strerror_r(errno, errbuf, sizeof(errbuf));
        ERR("error: drmModeObjectGetProperties. (%s)", errbuf);
        return 0;
     }
   for (i = 0; i < props->count_props; i++)
     {
        drmModePropertyPtr prop = drmModeGetProperty(e_devmgr_drm_fd, props->props[i]);
        int ret;
        if (!prop)
          {
             char errbuf[128] = {0,};
             strerror_r(errno, errbuf, sizeof(errbuf));
             ERR("error: drmModeGetProperty. (%s)", errbuf);
             drmModeFreeObjectProperties(props);
             return 0;
          }
        if (!strcmp(prop->name, prop_name))
          {
             ret = drmModeObjectSetProperty(e_devmgr_drm_fd, obj_id, obj_type, prop->prop_id, value);
             if (ret < 0)
               {
                  char errbuf[128] = {0,};
                  strerror_r(errno, errbuf, sizeof(errbuf));
                  ERR("error: drmModeObjectSetProperty. (%s)", errbuf);
                  drmModeFreeProperty(prop);
                  drmModeFreeObjectProperties(props);
                  return 0;
               }
             drmModeFreeProperty(prop);
             drmModeFreeObjectProperties(props);

             return 1;
          }
        drmModeFreeProperty(prop);
     }

   ERR("error: drm set property.");
   drmModeFreeObjectProperties(props);
#endif /* USE_DRM_USER_HANDLER */
   return 0;
}

int
e_devicemgr_drm_ipp_set(struct drm_exynos_ipp_property *property)
{
#ifdef USE_DRM_USER_HANDLER
    int ret = 0;

    EINA_SAFETY_ON_NULL_RETURN_VAL(property, -1);

    if (property->prop_id == (__u32)-1)
        property->prop_id = 0;

    DBG("src : flip(%x) deg(%d) fmt(%c%c%c%c) sz(%dx%d) pos(%d,%d %dx%d)  ",
        property->config[0].flip, property->config[0].degree, FOURCC_STR(property->config[0].fmt),
        property->config[0].sz.hsize, property->config[0].sz.vsize,
        property->config[0].pos.x, property->config[0].pos.y, property->config[0].pos.w, property->config[0].pos.h);
    DBG("dst : flip(%x) deg(%d) fmt(%c%c%c%c) sz(%dx%d) pos(%d,%d %dx%d)  ",
        property->config[1].flip, property->config[1].degree, FOURCC_STR(property->config[1].fmt),
        property->config[1].sz.hsize, property->config[1].sz.vsize,
        property->config[1].pos.x, property->config[1].pos.y, property->config[1].pos.w, property->config[1].pos.h);

    ret = ioctl(e_devmgr_drm_fd, DRM_IOCTL_EXYNOS_IPP_SET_PROPERTY, property);
    if (ret)
    {
        char errbuf[128] = {0,};
        strerror_r(errno, errbuf, sizeof(errbuf));
        ERR("failed. (%s)", errbuf);
        return -1;
    }

    DBG("success. prop_id(%d) ", property->prop_id);

    return property->prop_id;
#endif /* USE_DRM_USER_HANDLER */
    return -1;
}

Eina_Bool
e_devicemgr_drm_ipp_queue(struct drm_exynos_ipp_queue_buf *buf)
{
#ifdef USE_DRM_USER_HANDLER
    int ret = 0;

    EINA_SAFETY_ON_NULL_RETURN_VAL(buf, EINA_FALSE);

    DBG("prop_id(%d) ops_id(%d) ctrl(%d) id(%d) handles(%x %x %x). ",
        buf->prop_id, buf->ops_id, buf->buf_type, buf->buf_id,
        buf->handle[0], buf->handle[1], buf->handle[2]);

    ret = ioctl(e_devmgr_drm_fd, DRM_IOCTL_EXYNOS_IPP_QUEUE_BUF, buf);
    if (ret)
    {
        char errbuf[128] = {0,};
        strerror_r(errno, errbuf, sizeof(errbuf));
        ERR("failed. prop_id(%d) op(%d) buf(%d) id(%d). (%s)",
            buf->prop_id, buf->ops_id, buf->buf_type, buf->buf_id, errbuf);
        return EINA_FALSE;
    }

    DBG("success. prop_id(%d) ", buf->prop_id);

    return EINA_TRUE;
#endif /* USE_DRM_USER_HANDLER */
    return EINA_FALSE;
}

Eina_Bool
e_devicemgr_drm_ipp_cmd(struct drm_exynos_ipp_cmd_ctrl *ctrl)
{
#ifdef USE_DRM_USER_HANDLER
   int ret = 0;

   EINA_SAFETY_ON_NULL_RETURN_VAL(ctrl, EINA_FALSE);

   DBG("prop_id(%d) ctrl(%d). ", ctrl->prop_id, ctrl->ctrl);

   ret = ioctl(e_devmgr_drm_fd, DRM_IOCTL_EXYNOS_IPP_CMD_CTRL, ctrl);
   if (ret)
     {
        char errbuf[128] = {0,};
        strerror_r(errno, errbuf, sizeof(errbuf));
        ERR("failed. prop_id(%d) ctrl(%d). (%s)", ctrl->prop_id, ctrl->ctrl, errbuf);
        return EINA_FALSE;
     }

   DBG("success. prop_id(%d) ", ctrl->prop_id);

   return EINA_TRUE;
#endif /* USE_DRM_USER_HANDLER */
   return EINA_FALSE;
}

Eina_Bool
e_devicemgr_drm_get_cur_msc (int pipe, uint *msc)
{
#ifdef USE_DRM_USER_HANDLER
   drmVBlank vbl;

   EINA_SAFETY_ON_NULL_RETURN_VAL(msc, EINA_FALSE);

   vbl.request.type = DRM_VBLANK_RELATIVE;
   if (pipe > 0)
     vbl.request.type |= DRM_VBLANK_SECONDARY;

   vbl.request.sequence = 0;
   if (drmWaitVBlank(e_devmgr_drm_fd, &vbl))
     {
        char errbuf[128] = {0,};
        strerror_r(errno, errbuf, sizeof(errbuf));
        ERR("first get vblank counter failed: %s", errbuf);
        *msc = 0;
        return EINA_FALSE;
     }

   *msc = vbl.reply.sequence;

   return EINA_TRUE;
#endif /* USE_DRM_USER_HANDLER */
   return EINA_FALSE;
}

Eina_Bool
e_devicemgr_drm_wait_vblank(int pipe, uint *target_msc, void *data)
{
#ifdef USE_DRM_USER_HANDLER
   drmVBlank vbl;

   EINA_SAFETY_ON_NULL_RETURN_VAL(target_msc, EINA_FALSE);

   vbl.request.type =  DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
   if (pipe > 0)
     vbl.request.type |= DRM_VBLANK_SECONDARY;

   vbl.request.sequence = *target_msc;
   vbl.request.signal = (unsigned long)(uintptr_t)data;

   if (drmWaitVBlank(e_devmgr_drm_fd, &vbl))
     {
        char errbuf[128] = {0,};
        strerror_r(errno, errbuf, sizeof(errbuf));
        ERR("first get vblank counter failed: %s", errbuf);
        return EINA_FALSE;
     }

   *target_msc = vbl.reply.sequence;

   return EINA_TRUE;
#endif /* USE_DRM_USER_HANDLER */
   return EINA_FALSE;
}
