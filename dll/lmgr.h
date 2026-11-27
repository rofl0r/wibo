#pragma once

#include "types.h"

int CDECL lc_checkout(void);
int CDECL lc_set_attr(void);
int CDECL lc_new_job(void);
int CDECL lc_free_job(void);
int CDECL lc_checkin(void);

int CDECL lp_checkout(int a, int b, LPCSTR c, LPCSTR d, int e, LPCSTR f, int *out);
int CDECL lp_checkin(void);
