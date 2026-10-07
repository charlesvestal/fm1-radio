/* probe 2 only: the platform hooks and the libc the firmware would give */
#include <stdint.h>
#include <stddef.h>
uint32_t shim_ms(void) { return 0; }
uint32_t shim_irq_save(void) { return 0; }
void shim_irq_restore(uint32_t f) { (void)f; }
int shim_in_irq(void) { return 0; }
void shim_idle(void) {}
void shim_request_irq(int i, int p, void (*h)(void)) { (void)i; (void)p; (void)h; }
void shim_reboot(void) {}
void shim_log(const char *s) { (void)s; }
int shim_store_load(void *d, uint32_t m) { (void)d; (void)m; return -1; }
int shim_store_save(const void *s, uint32_t l) { (void)s; (void)l; return 0; }
void shim_bt_event(int f, const void *e, int l) { (void)f; (void)e; (void)l; }
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
