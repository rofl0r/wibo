#pragma once

#include "types.h"

GUEST_PTR WINAPI SysAllocString(LPCWSTR psz);
void WINAPI SysFreeString(LPWSTR bstrString);
UINT WINAPI SysStringLen(LPCWSTR pbstr);
