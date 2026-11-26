#pragma once

#include "types.h"
#include "errhandlingapi.h"

void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
