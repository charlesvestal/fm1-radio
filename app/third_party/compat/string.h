/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* The few C library declarations minimp3 and places.c ask for, matching fm1-base's device/libc.c (and the shim's memmove). */
#pragma once
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
void *memmove(void *d, const void *s, unsigned n);
unsigned long strlen(const char *s);
int strcmp(const char *a, const char *b);
