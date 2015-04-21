#ifndef __E_MOD_MAIN_H__
#define __E_MOD_MAIN_H__

#include "e.h"

/*** E Module ***/
EAPI extern E_Module_Api e_modapi;

EAPI void* e_modapi_init (E_Module *m);
EAPI int e_modapi_shutdown (E_Module *m);
EAPI int e_modapi_save (E_Module *m);

#endif//__E_MOD_MAIN_H__