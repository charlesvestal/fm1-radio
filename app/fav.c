/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Favorites: up to 16 stations in their own store object (OBJ_FAV), loaded only while their page (SAVE) is open.
 * The list's operations here, apart from the store and the screen, so tests/fav_test.c can try them alone. Part of
 * the app's unity build (app.c, before radio_ui.c). */
#include <stdint.h>                              /* (memset, strcmp: the app's; tests/fav_test.c includes string.h) */

#define FAV_MAX 16u
#define FAV_MAGIC 0x46415631u                     /* "FAV1" */
typedef struct {
    uint16_t country;                             /* the places table's index */
    char place[32], st_id[12], title[32];
} fav_t;
typedef struct {
    uint32_t magic;
    uint16_t n, rsv;
    fav_t f[FAV_MAX];
} fav_list_t;

static fav_list_t *fav;                           /* the page's copy, from the heap; 0 while it is closed */
static int fav_sel;                               /* 0 the add row, 1.. the favorites */

static void fav_clear(fav_list_t *l)
{
    memset(l, 0, sizeof *l);
    l->magic = FAV_MAGIC;
}
static int fav_valid(const fav_list_t *l, int len)   /* a loaded object, whole and ours */
{
    return len == (int)sizeof *l && l->magic == FAV_MAGIC && l->n <= FAV_MAX;
}
static int fav_find(const fav_list_t *l, const char *st_id)
{
    uint32_t i;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->f[i].st_id, st_id))
            return (int)i;
    return -1;
}
static int fav_add(fav_list_t *l, const fav_t *e)  /* 0 added; -1 full, -2 there already, -3 no station */
{
    if (!e->st_id[0])
        return -3;
    if (fav_find(l, e->st_id) >= 0)
        return -2;
    if (l->n >= FAV_MAX)
        return -1;
    l->f[l->n++] = *e;
    return 0;
}
static void fav_del(fav_list_t *l, uint32_t i)
{
    if (i >= l->n)
        return;
    for (; i + 1u < l->n; i++)
        l->f[i] = l->f[i + 1u];
    l->n--;
}
