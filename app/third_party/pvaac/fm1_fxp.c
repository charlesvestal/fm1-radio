/* SPDX-License-Identifier: Apache-2.0
 * fm1-radio: the one copy of each 64-bit multiply helper (fxp_mul32_c_equivalent.h, FXP64): elsewhere they are
 * declared inline but never inlined, so every multiply is a call to these */
#define FXP64 __attribute__((noinline))
#include "fxp_mul32.h"
