/* sys_timer (periodic) and sys_timeout (one-shot), include_lib/system/timer.h, on the firmware's
 * millisecond clock. Run from the main loop (shim_timers), or, for the *_add_to_task kind, posted
 * to that task as [Q_CALLBACK, fn, priv] (os_taskq_pend runs it there). */
#include <stdint.h>
#include <string.h>
#include "shim.h"

#define NTIMER 24
typedef struct {
    void (*fn)(void *);
    void *priv;
    const char *task;                 /* 0: run from the main loop */
    uint32_t period, due;
    uint16_t id;
    uint8_t used, once;
} stimer_t;
static stimer_t tm[NTIMER];
static uint16_t next_id = 1;

int os_taskq_post_type(const char *name, int type, int argc, int *argv);

static u16 add(const char *task, void *priv, void (*fn)(void *), u32 msec, int once)
{
    int i;
    uint32_t f = shim_irq_save();
    for (i = 0; i < NTIMER; i++)
        if (!tm[i].used) {
            tm[i].fn = fn;
            tm[i].priv = priv;
            tm[i].task = task;
            tm[i].period = msec ? msec : 1;
            tm[i].due = shim_ms() + tm[i].period;
            tm[i].once = (uint8_t)once;
            do {
                tm[i].id = next_id++;
            } while (!tm[i].id);
            tm[i].used = 1;
            shim_irq_restore(f);
            return tm[i].id;
        }
    shim_irq_restore(f);
    return 0;
}
static stimer_t *get(u16 id)
{
    int i;
    for (i = 0; i < NTIMER; i++)
        if (tm[i].used && tm[i].id == id)
            return &tm[i];
    return 0;
}

u16 sys_timer_add(void *priv, void (*fn)(void *), u32 msec) { return add(0, priv, fn, msec, 0); }
u16 sys_timeout_add(void *priv, void (*fn)(void *), u32 msec) { return add(0, priv, fn, msec, 1); }
u16 sys_timer_add_to_task(const char *task, void *priv, void (*fn)(void *), u32 msec) { return add(task, priv, fn, msec, 0); }
u16 sys_timeout_add_to_task(const char *task, void *priv, void (*fn)(void *), u32 msec) { return add(task, priv, fn, msec, 1); }
u16 usr_timeout_add(void *priv, void (*fn)(void *), u32 msec, u8 prio) { (void)prio; return add(0, priv, fn, msec, 1); }
void sys_timer_del(u16 id) { stimer_t *t = get(id); if (t) t->used = 0; }
void sys_timeout_del(u16 id) { sys_timer_del(id); }
void usr_timer_del(u16 id) { sys_timer_del(id); }
int sys_timer_modify(u16 id, u32 msec)
{
    stimer_t *t = get(id);
    if (!t)
        return -1;
    t->period = msec ? msec : 1;
    t->due = shim_ms() + t->period;
    return 0;
}
void sys_timer_re_run(u16 id) { stimer_t *t = get(id); if (t) t->due = shim_ms() + t->period; }
void sys_timer_set_user_data(u16 id, void *priv) { stimer_t *t = get(id); if (t) t->priv = priv; }
void *sys_timer_get_user_data(u16 id) { stimer_t *t = get(id); return t ? t->priv : 0; }
u32 sys_timer_get_ms(void) { return shim_ms(); }

void shim_timers(void)
{
    int i;
    uint32_t now = shim_ms();
    for (i = 0; i < NTIMER; i++) {
        stimer_t *t = &tm[i];
        if (!t->used || (int32_t)(now - t->due) < 0)
            continue;
        if (t->once)
            t->used = 0;
        else
            t->due = now + t->period;
        if (t->task) {
            int msg[2] = {(int)(uintptr_t)t->fn, (int)(uintptr_t)t->priv};
            os_taskq_post_type(t->task, Q_CALLBACK, 2, msg);
        } else {
            t->fn(t->priv);
        }
    }
}
