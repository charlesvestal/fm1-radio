/* JieLi's OS API (include_lib/system/os/os_api.h), the part the BT libraries call, on a small
 * cooperative scheduler. Task 0 is the firmware's main loop; task_create() adds the BT tasks
 * (btctrler, btstack, btencry), each with its own stack and message queue. A task runs until it
 * waits (os_taskq_pend, os_sem_pend, os_mutex_pend, os_time_dly); the main loop calls
 * shim_run() to let every task that can go do so. Posts may come from interrupts (the radio's):
 * the queues and counts are changed with interrupts off.
 *
 * Messages are int arrays: msg[0] the type (Q_MSG / Q_EVENT / Q_CALLBACK / Q_USER | n), then the
 * arguments. os_taskq_pend returns OS_TASKQ (13) with one copied out, as the libraries expect. */
#include <stdint.h>
#include <string.h>
#include "shim.h"

#define NTASK 4                       /* main + btctrler, btstack, btencry */
#define QLEN 512                      /* ints per task queue */
#define STACK_WORDS 2048              /* 8 KiB per task: an interrupt (the audio render) nests on top */

enum { W_NONE, W_TASKQ, W_SEM, W_MUTEX, W_TIME };
typedef struct {
    const char *name;
    void (*fn)(void *);
    void *arg;
    uint32_t sp;
    uint8_t used, wait;
    void *wobj;                       /* the semaphore / mutex waited for */
    uint32_t deadline;                /* ms, 0 = forever */
    int q[QLEN];                      /* ring of messages: [len, type, args...] */
    uint32_t qr, qw;
} task_t;

static task_t tasks[NTASK];
static uint32_t stacks[NTASK - 1][STACK_WORDS] __attribute__((aligned(8), section(".pool")));
uint32_t shim_stacks_lo(void) { return (uint32_t)(uintptr_t)&stacks[0][0]; }
task_t *shim_cur = &tasks[0];
static uint32_t ticks_ms(void) { return shim_ms(); }

void shim_os_init(void)
{
    memset(tasks, 0, sizeof tasks);
    tasks[0].name = "app_core";
    tasks[0].used = 1;
    shim_cur = &tasks[0];
}

static task_t *find(const char *name)
{
    int i;
    for (i = 0; i < NTASK; i++)
        if (tasks[i].used && tasks[i].name && !strcmp(tasks[i].name, name))
            return &tasks[i];
    return 0;
}

/* ---- the queue: one ring of ints per task, each message stored as its length then its words */
static uint32_t q_used(const task_t *t) { return t->qw - t->qr; }

static int q_put(task_t *t, int type, int argc, const int *argv)
{
    uint32_t f = shim_irq_save(), i;
    if (QLEN - q_used(t) < (uint32_t)argc + 2u) {
        shim_irq_restore(f);
        return OS_Q_FULL;
    }
    t->q[t->qw++ % QLEN] = argc + 1;
    t->q[t->qw++ % QLEN] = type;
    for (i = 0; i < (uint32_t)argc; i++)
        t->q[t->qw++ % QLEN] = argv[i];
    shim_irq_restore(f);
    return OS_NO_ERR;
}

static int q_get(task_t *t, int *argv, int argc)
{
    uint32_t f = shim_irq_save();
    int n, i;
    if (!q_used(t)) {
        shim_irq_restore(f);
        return 0;
    }
    n = t->q[t->qr++ % QLEN];
    for (i = 0; i < n; i++) {
        int v = t->q[t->qr++ % QLEN];
        if (i < argc)
            argv[i] = v;
    }
    shim_irq_restore(f);
    return 1;
}

/* ---- scheduling */
static int can_run(const task_t *t)
{
    if (!t->used)
        return 0;
    if (t->deadline && (int32_t)(ticks_ms() - t->deadline) >= 0)
        return 1;
    switch (t->wait) {
    case W_NONE: return 1;
    case W_TASKQ: return q_used(t) != 0;
    case W_SEM: return ((shim_sem_t *)t->wobj)->count > 0;
    case W_MUTEX: return ((shim_sem_t *)t->wobj)->owner == 0 || ((shim_sem_t *)t->wobj)->owner == t;
    case W_TIME: return 0;            /* only the deadline */
    default: return 1;
    }
}

extern void shim_ctx_switch(uint32_t *save_sp, uint32_t new_sp);

static void switch_to(task_t *n)
{
    task_t *o = shim_cur;
    if (n == o)
        return;
    shim_cur = n;
    shim_ctx_switch(&o->sp, n->sp);
}

/* the current task waits: run the others (round robin) until it can go on */
static void block(void)
{
    for (;;) {
        int k, i = (int)(shim_cur - tasks);
        for (k = 1; k <= NTASK; k++) {
            task_t *t = &tasks[(i + k) % NTASK];
            if (t == shim_cur) {
                if (can_run(t))
                    return;
            } else if (can_run(t)) {
                switch_to(t);
                break;
            }
        }
        if (k > NTASK)               /* nobody can run: only the main loop may idle */
            shim_idle();
        if (can_run(shim_cur))
            return;
    }
}

/* the main loop's slice: every BT task that can go runs until it waits again */
void shim_run(void)
{
    int pass, i;
    for (pass = 0; pass < 8; pass++) {
        int ran = 0;
        for (i = 1; i < NTASK; i++)
            if (can_run(&tasks[i])) {
                switch_to(&tasks[i]);
                ran = 1;
            }
        if (!ran)
            break;
    }
}

static void wait_on(int what, void *obj, int timeout_ticks)
{
    shim_cur->wait = (uint8_t)what;
    shim_cur->wobj = obj;
    shim_cur->deadline = timeout_ticks > 0 ? ticks_ms() + (uint32_t)timeout_ticks * 10u : 0;
    if (shim_cur->deadline == 0 && timeout_ticks > 0)
        shim_cur->deadline = 1;
    block();
    shim_cur->wait = W_NONE;
    shim_cur->deadline = 0;
}

void shim_task_entry(void)
{
    task_t *t = shim_cur;
    t->fn(t->arg);
    t->used = 0;                      /* a task that returns ends */
    block();
}

int task_create(void (*fn)(void *), void *p, const char *name)
{
    int i, k;
    for (i = 1; i < NTASK; i++)
        if (!tasks[i].used) {
            task_t *t = &tasks[i];
            uint32_t *top = &stacks[i - 1][STACK_WORDS];
            memset(t, 0, sizeof *t);
            t->name = name;
            t->fn = fn;
            t->arg = p;
            for (k = 0; k < STACK_WORDS - 13; k++)
                stacks[i - 1][k] = 0x5354414Bu;          /* "STAK": how deep it went (shim_os_dump) */
            for (k = 1; k <= 13; k++)
                top[-k] = (uint32_t)(uintptr_t)shim_task_entry;
            t->sp = (uint32_t)(uintptr_t)(top - 13);
            t->used = 1;
            return 0;
        }
    return OS_NO_MORE_TCB;
}

int task_kill(const char *name)
{
    task_t *t = find(name);
    if (t && t != &tasks[0])
        t->used = 0;
    return 0;
}

/* task's deepest stack so far, in 256-byte units (the "STAK" fill left untouched) */
int shim_stack_depth(int i)
{
    int u = 0;
    if (i < 1 || i >= NTASK || !tasks[i].used)
        return 0;
    while (u < STACK_WORDS && stacks[i - 1][u] == 0x5354414Bu)
        u++;
    return (STACK_WORDS - u) * 4 / 256;
}

/* for the screen: each task and what it waits for */
void shim_os_dump(void)
{
    static const char *const W[] = {"run", "taskq", "sem", "mutex", "time"};
    int i;
    for (i = 0; i < NTASK; i++)
        if (tasks[i].used) {
            char b[40];
            int n = 0;
            const char *s = tasks[i].name ? tasks[i].name : "?";
            while (*s && n < 14)
                b[n++] = *s++;
            b[n++] = ' ';
            s = W[tasks[i].wait < 5 ? tasks[i].wait : 0];
            while (*s && n < 22)
                b[n++] = *s++;
            b[n++] = ' ';
            b[n++] = 'q';
            b[n++] = (char)('0' + (q_used(&tasks[i]) > 9 ? 9 : q_used(&tasks[i])));
            if (i) {                                    /* stack used, of 8 KiB, in 256-byte steps */
                int u = 0;
                while (u < STACK_WORDS && stacks[i - 1][u] == 0x5354414Bu)
                    u++;
                u = (STACK_WORDS - u) * 4 / 256;
                b[n++] = ' ';
                b[n++] = 's';
                b[n++] = (char)('0' + u / 10 % 10);
                b[n++] = (char)('0' + u % 10);
            }
            if (&tasks[i] == shim_cur)
                b[n++] = '*';
            b[n] = 0;
            shim_log(b);
        }
}

const char *os_current_task(void) { return shim_cur->name; }
const char *os_current_task_rom(void) { return shim_cur->name; }

void os_time_dly(int ticks)
{
    wait_on(W_TIME, 0, ticks > 0 ? ticks : 1);
}

int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    task_t *t = find(name);
    if (!t)
        return OS_TASK_NOT_EXIST;
    return q_put(t, type, argc, argv);
}

int os_taskq_pend(const char *fmt, int *argv, int argc)
{
    (void)fmt;
    for (;;) {
        if (!q_get(shim_cur, argv, argc))
            wait_on(W_TASKQ, 0, 0);
        else if (argv[0] == Q_CALLBACK) {      /* ours: [Q_CALLBACK, fn, priv] runs here */
            ((void (*)(void *))(uintptr_t)argv[1])((void *)(uintptr_t)argv[2]);
        } else {
            return OS_TASKQ;
        }
    }
}

int os_sem_create(OS_SEM *s, int cnt)
{
    shim_sem_t *m = (shim_sem_t *)s;
    m->magic = SHIM_SEM_MAGIC;
    m->count = cnt;
    m->owner = 0;
    return OS_NO_ERR;
}
int os_sem_set(OS_SEM *s, u16 cnt) { ((shim_sem_t *)s)->count = cnt; return OS_NO_ERR; }
int os_sem_post(OS_SEM *s)
{
    uint32_t f = shim_irq_save();
    ((shim_sem_t *)s)->count++;
    shim_irq_restore(f);
    return OS_NO_ERR;
}
int os_sem_pend(OS_SEM *s, int timeout)
{
    shim_sem_t *m = (shim_sem_t *)s;
    for (;;) {
        uint32_t f = shim_irq_save();
        if (m->count > 0) {
            m->count--;
            shim_irq_restore(f);
            return OS_NO_ERR;
        }
        shim_irq_restore(f);
        if (shim_in_irq())
            return OS_ERR_PEND_ISR;
        wait_on(W_SEM, m, timeout);
        if (timeout > 0 && m->count <= 0)
            return OS_TIMEOUT;
    }
}

int os_mutex_create(OS_MUTEX *x)
{
    shim_sem_t *m = (shim_sem_t *)x;
    m->magic = SHIM_SEM_MAGIC;
    m->count = 0;
    m->owner = 0;
    return OS_NO_ERR;
}
int os_mutex_valid(OS_MUTEX *x) { return ((shim_sem_t *)x)->magic == SHIM_SEM_MAGIC; }
int os_mutex_del(OS_MUTEX *x, int force) { (void)force; ((shim_sem_t *)x)->magic = 0; return OS_NO_ERR; }
int os_mutex_pend(OS_MUTEX *x, int timeout)
{
    shim_sem_t *m = (shim_sem_t *)x;
    for (;;) {
        if (m->owner == 0 || m->owner == shim_cur) {
            m->owner = shim_cur;
            m->count++;                            /* recursive */
            return OS_NO_ERR;
        }
        wait_on(W_MUTEX, m, timeout);
        if (timeout > 0 && m->owner && m->owner != shim_cur)
            return OS_TIMEOUT;
    }
}
int os_mutex_post(OS_MUTEX *x)
{
    shim_sem_t *m = (shim_sem_t *)x;
    if (m->owner == shim_cur && --m->count <= 0) {
        m->count = 0;
        m->owner = 0;
    }
    return OS_NO_ERR;
}
