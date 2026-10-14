/* The few C library declarations minimp3 asks for, matching firmware/src/libc.c (and the shim's memmove). */
#pragma once
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
void *memmove(void *d, const void *s, unsigned n);
