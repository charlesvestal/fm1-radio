/* sys_timer (periodic) and sys_timeout (one-shot), include_lib/system/timer.h, on the firmware's
 * millisecond clock. As JieLi's (system.a timer.c / sys_timer_task.c): a timer belongs to the task that
 * added it (or the named one, *_add_to_task) and its callback is posted to that task as [Q_CALLBACK, fn,
 * priv], run when the owner waits on its queue; a periodic one is not posted again until it has run. The
 * main loop's own timers run from the main loop (shim_timers), never from inside another task's wait. */
#include <stdint.h>
#include <string.h>
#include "shim.h"

#define NTIMER 48
typedef struct {
    void (*fn)(void *);
    void *priv;
    const char *task;                 /* a named owner (*_add_to_task), else ... */
    int owner;                        /* ... the slot of the task that added it (0: the main loop) */
    uint32_t period, due;
    uint16_t id;
    uint8_t used, once, pending;
} stimer_t;
static stimer_t tm[NTIMER];
static uint16_t next_id = 1;

int os_taskq_post_type(const char *name, int type, int argc, int *argv);
int shim_cur_index(void);
int shim_task_post_idx(int i, int type, int argc, int *argv);

static u16 add(const char *task, void *priv, void (*fn)(void *), u32 msec, int once)
{
    int i;
    uint32_t f = shim_irq_save();
    for (i = 0; i < NTIMER; i++)
        if (!tm[i].used) {
            tm[i].fn = fn;
            tm[i].priv = priv;
            tm[i].task = task;
            tm[i].owner = shim_cur_index();
            tm[i].pending = 0;
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
    shim_log("timer table full");
    return 0;
}
int shim_timers_used(void)
{
    int i, n = 0;
    for (i = 0; i < NTIMER; i++)
        n += tm[i].used;
    return n;
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
/* usr timers are JieLi's hardware-timer ones (a priority, no owner task): the main loop runs them */
u16 usr_timeout_add(void *priv, void (*fn)(void *), u32 msec, u8 prio)
{
    u16 id = add(0, priv, fn, msec, 1);
    stimer_t *t = get(id);
    (void)prio;
    if (t)
        t->owner = 0;
    return id;
}
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

extern volatile unsigned long jiffies;

/* a posted timer's callback, run by its owner: [index << 16 | id] */
static void fire(void *arg)
{
    uint32_t v = (uint32_t)(uintptr_t)arg, i = v >> 16;
    stimer_t *t = i < NTIMER ? &tm[i] : 0;
    void (*fn)(void *);
    void *priv;
    if (!t || !t->used || t->id != (uint16_t)v)
        return;                                   /* deleted meanwhile */
    fn = t->fn;
    priv = t->priv;
    t->pending = 0;
    if (t->once)
        t->used = 0;
    fn(priv);
}

/* due timers: posted to their owners; the main loop's run here when main_ok (the main loop calls,
 * not a task's idle) */
static void timers_run(int main_ok)
{
    int i;
    uint32_t now = shim_ms();
    jiffies = now / 10u;
    for (i = 0; i < NTIMER; i++) {
        stimer_t *t = &tm[i];
        int msg[2];
        if (!t->used || t->pending || (int32_t)(now - t->due) < 0)
            continue;
        if (!t->task && t->owner == 0) {
            if (!main_ok)
                continue;
            t->due = now + t->period;
            if (t->once)
                t->used = 0;
            t->fn(t->priv);
            continue;
        }
        t->due = now + t->period;
        t->pending = 1;
        msg[0] = (int)(uintptr_t)fire;
        msg[1] = (int)(i << 16 | t->id);
        if ((t->task ? os_taskq_post_type(t->task, Q_CALLBACK, 2, msg)
                     : shim_task_post_idx(t->owner, Q_CALLBACK, 2, msg)) != OS_NO_ERR)
            t->pending = 0;                       /* owner gone or its queue full: try at the next due */
    }
}
void shim_timers(void) { timers_run(1); }        /* the main loop's */
void shim_timers_post(void) { timers_run(0); }   /* a task's idle: post only */

/* a task ended: its timers go with it (JieLi: sys_timer_del_before_kill_owner_task) */
void shim_timers_drop_owner(int owner)
{
    int i;
    for (i = 0; i < NTIMER; i++)
        if (tm[i].used && !tm[i].task && tm[i].owner == owner && owner != 0)
            tm[i].used = 0;
}
