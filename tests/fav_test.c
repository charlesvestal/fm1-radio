/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* app/fav.c alone: cc -O1 -Ibase/include -o build/fav_test tests/fav_test.c && ./build/fav_test */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../app/fav.c"

static int bad;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "  ok" : "FAIL", what);
    bad += !ok;
}
static fav_t mk(int i)
{
    fav_t e;
    memset(&e, 0, sizeof e);
    e.country = (uint16_t)i;
    snprintf(e.place, sizeof e.place, "place %d", i);
    snprintf(e.st_id, sizeof e.st_id, "id%d", i);
    snprintf(e.title, sizeof e.title, "station %d", i);
    return e;
}

int main(void)
{
    static fav_list_t l;
    fav_t e;
    int i, ok = 1;
    fav_clear(&l);
    for (i = 0; i < 16; i++) {
        e = mk(i);
        ok &= fav_add(&l, &e) == 0;
    }
    check(ok && l.n == 16, "16 added");
    e = mk(99);
    check(fav_add(&l, &e) == -1, "the 17th: full");
    fav_clear(&l);
    e = mk(1);
    fav_add(&l, &e);
    check(fav_add(&l, &e) == -2, "a station already there");
    e.st_id[0] = 0;
    check(fav_add(&l, &e) == -3, "no station id");
    e = mk(2), fav_add(&l, &e);
    e = mk(3), fav_add(&l, &e);
    fav_del(&l, 1);
    check(l.n == 2 && !strcmp(l.f[0].st_id, "id1") && !strcmp(l.f[1].st_id, "id3"), "delete the middle");
    fav_del(&l, 5);
    check(l.n == 2, "delete past the end: nothing");
    check(fav_find(&l, "id3") == 1 && fav_find(&l, "id2") == -1, "find");
    check(fav_valid(&l, (int)sizeof l), "valid");
    l.magic = 0;
    check(!fav_valid(&l, (int)sizeof l), "wrong magic");
    l.magic = FAV_MAGIC;
    check(!fav_valid(&l, (int)sizeof l - 1), "wrong length");
    l.n = 17;
    check(!fav_valid(&l, (int)sizeof l), "count over 16");
    printf(bad ? "fav_test: %d failed\n" : "fav_test: all passed\n", bad);
    return bad != 0;
}
