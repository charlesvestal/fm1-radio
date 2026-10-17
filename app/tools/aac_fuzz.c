/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Damaged streams through the AAC decoder (third_party/pvaac), on the computer, as radio.c drives it: a 6 KB window,
 * the decoder new after every bad frame. Each round damages a copy of an ADTS capture (bit flips, random bytes, runs
 * of garbage, gaps), tunes in somewhere in its first 5 KB and decodes it to the end. Built with AddressSanitizer
 * (aac_fuzz.sh), a read or write outside the decoder's memory stops it with the place; a decode call that takes
 * more than 30 s is a hang (on the FM-1: the watchdog).
 *
 *   curl -sL -m 90 -A "FM-1 Net/0.1" -o /tmp/s.aac STREAM_URL      a capture of your own (an HE-AAC stream)
 *   app/tools/aac_fuzz.sh /tmp/s.aac [ROUNDS [SEED]]
 *
 * With ROUNDS 0 it decodes the capture once, undamaged, and writes the PCM to OUT (if set): the same output from
 * two builds = a change that keeps good streams as they were. */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pvmp4audiodecoder_api.h"

#define WINDOW 6144u                                  /* radio.c LIN_MAX */

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static void hang(int sig) { (void)sig; fprintf(stderr, "aac_fuzz: a decode call hung\n"); _exit(3); }

static void damage(uint8_t *b, size_t len, int mode)
{
    uint32_t i, n = 1u + rnd() % (mode == 0 ? 2000u : 50u);
    for (i = 0; i < n; i++) {
        uint32_t at = rnd() % len, k, m;
        if (mode == 0) {
            b[at] ^= (uint8_t)(1u << (rnd() % 8u));                         /* bit flips */
        } else if (mode == 1) {
            b[at] = (uint8_t)rnd();                                          /* random bytes */
        } else if (mode == 2) {
            m = rnd() % 400u;                                                /* a run of garbage */
            for (k = 0; k < m && at + k < len; k++)
                b[at + k] = (uint8_t)rnd();
        } else {
            m = rnd() % 3000u;                                               /* a gap */
            if (at + m < len)
                memmove(b + at, b + at + m, len - at - m);
        }
    }
}

static int reset(tPVMP4AudioDecoderExternal *ext, void *mem)          /* radio.c aac_reset */
{
    memset(ext, 0, sizeof *ext);
    ext->outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
    ext->desiredChannels = 2;
    ext->aacPlusEnabled = 1;
    return PVMP4AudioDecoderInitLibrary(ext, mem);
}

int main(int argc, char **argv)
{
    static uint8_t src[4u << 20];
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : 0, *out = 0;
    size_t len;
    int rounds = argc > 2 ? atoi(argv[2]) : 100, it;
    if (!f) {
        fprintf(stderr, "usage: aac_fuzz CAPTURE.aac [ROUNDS [SEED]]\n");
        return 2;
    }
    len = fread(src, 1, sizeof src, f);
    fclose(f);
    rs = argc > 3 ? (uint32_t)atoi(argv[3]) : 1u;
    if (!rounds && getenv("OUT"))
        out = fopen(getenv("OUT"), "wb");
    signal(SIGALRM, hang);
    for (it = 0; it < (rounds ? rounds : 1); it++) {
        uint8_t *buf = malloc(len);
        tPVMP4AudioDecoderExternal *ext = malloc(sizeof *ext);
        void *mem = malloc(PVMP4AudioDecoderGetMemRequirements());
        int16_t *pcm = malloc(4096u * sizeof *pcm);
        int mode = (int)(rnd() % 4u);
        size_t rd = rounds ? rnd() % 5000u : 0;
        uint32_t ok = 0, bad = 0;
        memcpy(buf, src, len);
        if (rounds)
            damage(buf, len, mode);
        reset(ext, mem);
        while (rd < len) {
            size_t n = len - rd < WINDOW ? len - rd : WINDOW;
            uint8_t *in = malloc(n);                  /* exactly the window: AddressSanitizer sees a read past it */
            int err;
            memcpy(in, buf + rd, n);
            ext->pInputBuffer = in;
            ext->inputBufferCurrentLength = (int)n;
            ext->inputBufferMaxLength = 0;
            ext->inputBufferUsedLength = 0;
            ext->remainderBits = 0;
            ext->pOutputBuffer = pcm;
            ext->pOutputBuffer_plus = &pcm[2048];
            ext->repositionFlag = 0;
            alarm(30);
            err = PVMP4AudioDecodeFrame(ext, mem);
            alarm(0);
            free(in);
            if (err == MP4AUDEC_INCOMPLETE_FRAME) {
                if (n < WINDOW)
                    break;
                rd += n / 2u;
            } else if (err != MP4AUDEC_SUCCESS) {
                rd += ext->inputBufferUsedLength > 0 ? (size_t)ext->inputBufferUsedLength : 1u;
                bad++;
                reset(ext, mem);
            } else {
                rd += (size_t)ext->inputBufferUsedLength;
                ok++;
                if (out)
                    fwrite(pcm, sizeof *pcm, 4096u, out);
            }
        }
        if (!rounds || it % 20 == 0 || it == rounds - 1)
            fprintf(stderr, "round %d (damage %d): %u frames, %u bad\n", it, rounds ? mode : -1, ok, bad);
        free(buf); free(ext); free(mem); free(pcm);
    }
    if (out)
        fclose(out);
    return 0;
}
