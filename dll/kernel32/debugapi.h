#pragma once

#include "types.h"

BOOL WINAPI IsDebuggerPresent(void);
void WINAPI OutputDebugStringA(LPCSTR lpOutputString);
