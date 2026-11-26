#pragma once

#include "types.h"

#define USER_PRIVILEGE 3

bool tebThreadSetup(TEB *teb);
bool tebThreadTeardown(TEB *teb);
void initFpState(void);
