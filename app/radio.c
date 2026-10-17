/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Internet radio: radio_net (net/radio_net.c) fetches an HTTP MP3 stream into a ring, the main loop decodes it with
 * minimp3 (lieff, CC0) into a ring of 16-bit stereo frames, and the audio interrupt plays those at 44.1 kHz (24-bit
 * out, MASTER as the gain). Part of the app's unity build (app.c). */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "third_party/minimp3.h"
#include "third_party/pvaac/pv_fm1.h"                /* PacketVideo's AAC decoder (Apache-2.0): AAC-LC in ADTS */
#include "third_party/pvaac/pvmp4audiodecoder_api.h"

extern void *malloc(unsigned long n);
extern void free(void *p);

#ifdef RADIO_LOCAL_TEST
#include "test_mp3.h"                            /* build/gen: the decoder test's input (mode 2) */
#endif

#define RADIO_URL "http://ice4.somafm.com/groovesalad-128-mp3"
static char radio_url_buf[160] __attribute__((section(".noinit")));   /* the wire's url (0x43); RAM is full */
static const char *radio_url = RADIO_URL;
#define NET_RING (20u * 1024u)                    /* ~1.25 s of 128 kbit/s (36 KB stalled as often: a weak link's gaps
                                                   * last seconds); the WiFi driver needs ~20 KiB of the heap left
                                                   * over, or its sends fail (connect: 113) */
#define PCM_FRAMES (4608u)                        /* ~104 ms: an HE-AAC frame adds 2048 at once, and at least
                                                   * ~58 ms stays queued for the main loop's pauses (3456 left 32) */
#define LIN_MAX (6u * 1024u)                      /* the decoder's window when the ring wraps */

static volatile uint32_t pcm_r, pcm_w;            /* frames: the ISR reads, the main loop writes */
extern volatile int radio_net_state;
extern volatile int radio_net_aac;
extern volatile uint32_t radio_rd, radio_wr;
extern char radio_net_why[32], radio_net_name[48];
int radio_net_start(const char *u, uint8_t *buf, uint32_t size);

/* for the https client's random generator (radio_net.c): no hardware generator is set up, so the time, MASTER's
 * reading, the audio's progress and the network's timing so far, mixed. BearSSL hashes it (HMAC-DRBG). Weak by a
 * security standard; fine for keeping a radio stream private-ish. */
void radio_entropy(uint8_t *p, unsigned n)
{
    static uint32_t carry = 0x9E3779B9u;
    uint32_t h = carry ^ plat_ms() ^ radio_wr * 0x85EBCA6Bu;
    unsigned i, k;
    for (i = 0; i < n; i++) {
        for (k = 0; k < 8u; k++) {
            h ^= plat_ms() + plat_master() * 0x27D4EB2Fu + pcm_r * 0x165667B1u;
            h = (h ^ (h >> 15)) * 0x2C1B3C6Du;
            h ^= h >> 12;
        }
        p[i] = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
    }
    carry = h;
}
void radio_net_stop(void);
int radio_net_running(void);
enum { RN_IDLE, RN_CONNECTING, RN_STREAMING, RN_ERROR, RN_ENDED, RN_UNPLAYABLE };
static uint8_t radio_unplayable;                  /* the station is not MP3: no point trying again */

enum { RS_OFF, RS_CONNECTING, RS_BUFFERING, RS_PLAYING, RS_ERROR };
static uint8_t radio_st;
/* the isolation tests: 0 the stream, 1 the stream fetched and dropped (no decoding), 2 the built-in MP3
 * decoded in a loop (no network) */
static uint8_t radio_mode;
static uint32_t local_pos;
static uint32_t radio_dropped;
static uint8_t *net_buf;
static uint8_t lin_buf[LIN_MAX];                  /* in RAM (BLE's room): the heap's goes to https */
static int16_t *pcm_buf;                          /* PCM_FRAMES x 2 */
static mp3dec_t *mp3;
static volatile uint32_t radio_underruns;
static uint32_t radio_frames, radio_hz, radio_kbps;
static volatile uint32_t radio_step = 1u << 16;  /* stream frames per output frame, Q16 */
static uint32_t radio_frac;                      /* the render's position between two frames, Q16 (its own) */
static uint32_t radio_dec_us, radio_dec_n, radio_dec_max;   /* decode time, summed, frames timed, the longest */
#ifdef PLAT_HOST
static uint32_t dec_us_now(void) { return plat_ms() * 1000u; }
#else
static uint32_t dec_us_now(void) { return fm1_ticks() / FM1_TICKS_PER_US; }   /* (TIMER4: wraps every 179 s) */
#endif
static void dec_timed(uint32_t t0)                /* a frame decoded, from t0 */
{
    uint32_t d = dec_us_now() - t0;
    radio_dec_us += d;
    if (d > radio_dec_max)
        radio_dec_max = d;
    radio_dec_n++;
}
static uint32_t radio_rebuffers;
static uint32_t radio_reconnects, radio_retry_at;  /* the stream dropped: when to fetch it again (ms) */
static char radio_why[32];
static int16_t *pcm_tmp;                          /* MINIMP3_MAX_SAMPLES_PER_FRAME, from the heap */

#define FX_HIST 6144u                             /* the FX history, half-rate samples: ~279 ms */
static int16_t *fx_hist;

/* the decoders, one at a time (the RAM): MP3's (minimp3, 11 KB) or AAC's (PacketVideo's, 53 KB), taken when a
 * stream's format is known (its Content-Type: radio_net_aac), the other's given back */
static uint8_t dec_kind;                          /* 0 none, 1 MP3, 2 AAC */
static tPVMP4AudioDecoderExternal *aac_ext;
static void *aac_mem;
static int16_t *aac_out;                          /* a frame: 1024 stereo */
static uint32_t radio_aac_errs;
static void dec_drop(void)
{
    free(mp3);
    free(pcm_tmp);
    free(aac_ext);
    free(aac_mem);
    free(aac_out);
    mp3 = 0;
    pcm_tmp = 0;
    aac_ext = 0;
    aac_mem = 0;
    aac_out = 0;
    dec_kind = 0;
}
/* the AAC decoder, as new: after every bad frame too, as the one call that left its state broken (a damaged
 * frame's settings kept: the frames after it failed, or worse) */
static int aac_reset(void)
{
    memset(aac_ext, 0, sizeof *aac_ext);
    aac_ext->outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
    aac_ext->desiredChannels = 2;
    aac_ext->aacPlusEnabled = 1;                  /* HE-AAC's SBR (built without PS: an HE-AAC v2 stream plays mono) */
    return PVMP4AudioDecoderInitLibrary(aac_ext, aac_mem) ? -1 : 0;
}
static int dec_use(uint8_t kind)                  /* 0 = ready */
{
    if (kind == dec_kind)
        return 0;
    dec_drop();
    if (kind == 1) {
        mp3 = malloc(sizeof *mp3);
        pcm_tmp = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof *pcm_tmp);
        if (!mp3 || !pcm_tmp) {
            dec_drop();
            return -1;
        }
        mp3dec_init(mp3);
    } else {
        aac_ext = malloc(sizeof *aac_ext);
        aac_mem = malloc(PVMP4AudioDecoderGetMemRequirements() + 8u);   /* (SBR's bit reader reads ahead 3 bytes past
                                                                           * its data, at the end of this) */
        aac_out = malloc(4096u * sizeof *aac_out);   /* a frame: 1024 stereo, 2048 with SBR */
        if (!aac_ext || !aac_mem || !aac_out) {
            dec_drop();
            return -1;
        }
        if (aac_reset()) {
            dec_drop();
            return -1;
        }
        radio_aac_errs = 0;
    }
    dec_kind = kind;
    return 0;
}

static int radio_alloc(void)
{
    if (net_buf)
        return 0;
    net_buf = malloc(NET_RING);
    pcm_buf = malloc(PCM_FRAMES * 4u);
    if (!net_buf || !pcm_buf) {
        free(net_buf); free(pcm_buf);
        net_buf = 0;
        pcm_buf = 0;
        return -1;
    }
    if (!fx_hist) {                               /* the FX history, kept: without it, no stutters */
        fx_hist = malloc(FX_HIST * sizeof *fx_hist);
        if (fx_hist)
            memset(fx_hist, 0, FX_HIST * sizeof *fx_hist);
    }
    return 0;
}

static void radio_play(uint8_t mode)
{
    if (radio_st != RS_OFF && radio_st != RS_ERROR)
        return;
    radio_why[0] = 0;
    radio_mode = mode;
    local_pos = 0;
    radio_dropped = 0;
    if (mode == 2) {
        if (radio_alloc()) {
            w_cpy(radio_why, "out of memory", sizeof radio_why);
            radio_st = RS_ERROR;
            return;
        }
        dec_drop();                               /* (the stream's format picks a decoder: dec_use) */
        pcm_r = pcm_w = 0;
        radio_frames = radio_hz = radio_kbps = 0;
        radio_dec_us = radio_dec_n = radio_rebuffers = 0;
        radio_underruns = 0;
        shim_log("radio: local mp3");
        radio_st = RS_BUFFERING;
        return;
    }
    if (wifi_st != WS_CONNECTED) {
        w_cpy(radio_why, "WiFi not connected", sizeof radio_why);
        radio_st = RS_ERROR;
        return;
    }
    if (radio_alloc()) {
        w_cpy(radio_why, "out of memory", sizeof radio_why);
        radio_st = RS_ERROR;
        return;
    }
    dec_drop();                                   /* (the stream's format picks a decoder: dec_use) */
    pcm_r = pcm_w = 0;
    radio_frames = radio_hz = radio_kbps = 0;
    radio_dec_us = radio_dec_n = radio_rebuffers = 0;
    radio_reconnects = radio_retry_at = 0;
    radio_underruns = 0;
    if (radio_net_start(radio_url, net_buf, NET_RING)) {
        w_cpy(radio_why, "busy, try again", sizeof radio_why);
        radio_st = RS_ERROR;
        return;
    }
    shim_log("radio: start");
    radio_unplayable = 0;
    radio_st = RS_CONNECTING;
}

static void radio_stop(void)
{
    if (radio_st == RS_OFF)
        return;
    if (radio_mode != 2)
        radio_net_stop();
    radio_st = RS_OFF;
    pcm_r = pcm_w;
    shim_log("radio: stop");
}

/* the main loop's: decode while there is input and room, for up to 15 ms a call, letting the network
 * tasks run between frames */
static void radio_poll(void)
{
    uint32_t t_start = plat_ms();
    if (radio_st == RS_OFF || radio_st == RS_ERROR)
        return;
    if (radio_mode == 1) {                        /* the network alone: count what arrives, drop it */
        if (radio_st == RS_CONNECTING && radio_net_state == RN_STREAMING)
            radio_st = RS_PLAYING;
        if (radio_net_state == RN_ERROR) {
            w_cpy(radio_why, radio_net_why, sizeof radio_why);
            radio_st = RS_ERROR;
        }
        radio_dropped += radio_wr - radio_rd;
        radio_rd = radio_wr;
        return;
    }
    if (radio_mode != 2 && radio_net_state == RN_UNPLAYABLE && !radio_net_running()) {
        w_cpy(radio_why, radio_net_why, sizeof radio_why);   /* "can't play AAC": said, not retried */
        radio_unplayable = 1;
        radio_st = RS_ERROR;
        return;
    }
    /* the stream dropped (stalled, ended, refused): what is buffered plays on, and it is fetched again,
     * a few times with a pause; mp3 frames resynchronise by themselves */
    if (radio_mode != 2 && (radio_net_state == RN_ERROR || radio_net_state == RN_ENDED) && !radio_net_running()) {
        if (!radio_retry_at) {
            radio_retry_at = plat_ms() + 2000u;
            shim_log(radio_net_why);
        } else if ((int32_t)(plat_ms() - radio_retry_at) >= 0) {
            radio_retry_at = 0;
            if (++radio_reconnects > 20u) {
                w_cpy(radio_why, radio_net_why, sizeof radio_why);
                radio_st = RS_ERROR;
                return;
            }
            {   /* the ring starts over: drop what was left, keep the decoded audio */
                uint32_t keep_r = radio_rd;
                (void)keep_r;
                if (radio_net_start(radio_url, net_buf, NET_RING) == 0) {
                    shim_log("radio: reconnect");
                    if (radio_st == RS_PLAYING && pcm_w == pcm_r)
                        radio_st = RS_BUFFERING;
                }
            }
        }
    }
    if (radio_st == RS_CONNECTING && radio_net_state == RN_STREAMING)
        radio_st = RS_BUFFERING;
    while (plat_ms() - t_start < 15u) {
        uint32_t avail = radio_wr - radio_rd, at = radio_rd % NET_RING, n, t0;
        const uint8_t *in;
        mp3dec_frame_info_t info;
        int samples;
        const int16_t *src = pcm_tmp;
        if (PCM_FRAMES - (pcm_w - pcm_r) < (dec_kind == 2 ? 2048u : 1152u))
            break;                                /* no room for a frame */
#ifdef RADIO_LOCAL_TEST
        if (radio_mode == 2) {                    /* the built-in MP3, round and round */
            if (local_pos >= sizeof test_mp3)
                local_pos = 0;
            in = test_mp3 + local_pos;
            n = sizeof test_mp3 - local_pos;
            if (n > LIN_MAX)
                n = LIN_MAX;
            t0 = dec_us_now();
            samples = mp3dec_decode_frame(mp3, in, (int)n, pcm_tmp, &info);
            dec_timed(t0);
            local_pos += info.frame_bytes ? (uint32_t)info.frame_bytes : n;
            goto decoded;
        }
#endif
        if (avail < (radio_net_state == RN_STREAMING ? 2048u : 1u))
            break;                                /* wait for input (or the end) */
        n = avail < LIN_MAX ? avail : LIN_MAX;
        if (at + n <= NET_RING) {
            in = net_buf + at;
        } else {                                  /* wraps: one linear copy */
            uint32_t a = NET_RING - at;
            memcpy(lin_buf, net_buf + at, a);
            memcpy(lin_buf + a, net_buf, n - a);
            in = lin_buf;
        }
        if (dec_use(radio_mode != 2 && radio_net_aac ? 2 : 1)) {   /* (mode 2: the built-in MP3) */
            w_cpy(radio_why, "out of memory", sizeof radio_why);
            radio_st = RS_ERROR;
            return;
        }
        t0 = dec_us_now();
        if (dec_kind == 2) {                      /* AAC: an ADTS frame (the decoder finds its header) */
            int err;
            aac_ext->pInputBuffer = (UChar *)in;
            aac_ext->inputBufferCurrentLength = (Int)n;
            aac_ext->inputBufferMaxLength = 0;
            aac_ext->inputBufferUsedLength = 0;
            aac_ext->remainderBits = 0;
            aac_ext->pOutputBuffer = aac_out;
            aac_ext->pOutputBuffer_plus = &aac_out[2048];   /* (SBR's second half, after the first) */
            aac_ext->repositionFlag = 0;
            err = PVMP4AudioDecodeFrame(aac_ext, aac_mem);
            dec_timed(t0);
            if (err == MP4AUDEC_INCOMPLETE_FRAME) {
                if (n >= LIN_MAX)
                    radio_rd += n / 2u;           /* no frame in a full window: skip garbage */
                break;
            }
            if (err != MP4AUDEC_SUCCESS) {        /* a bad frame: past it (or a byte), the decoder finds sync */
                radio_rd += aac_ext->inputBufferUsedLength > 0 ? (uint32_t)aac_ext->inputBufferUsedLength : 1u;
                radio_aac_errs++;
                aac_reset();
                continue;
            }
            radio_rd += (uint32_t)aac_ext->inputBufferUsedLength;
            samples = aac_ext->frameLength * (aac_ext->aacPlusUpsamplingFactor == 2 ? 2 : 1);
            info.hz = (int)aac_ext->samplingRate;
            info.channels = 2;                    /* (mono comes copied into both) */
            info.bitrate_kbps = (int)(aac_ext->bitRate / 1000);
            src = aac_out;
        } else {
        samples = mp3dec_decode_frame(mp3, in, (int)n, pcm_tmp, &info);
        dec_timed(t0);
        if (!info.frame_bytes) {
            if (n >= LIN_MAX)
                radio_rd += n / 2u;               /* no frame in a full window: skip garbage */
            break;
        }
        radio_rd += (uint32_t)info.frame_bytes;
        src = pcm_tmp;
        }
#ifdef RADIO_LOCAL_TEST
    decoded:
#endif
        if (samples <= 0)
            continue;                             /* ID3 or junk skipped */
        radio_frames++;
        if (radio_hz != (uint32_t)info.hz && info.hz > 0)   /* the stream's rate to the output's 44.1 kHz */
            radio_step = ((uint32_t)info.hz << 16) / 44100u;
        radio_hz = (uint32_t)info.hz;
        radio_kbps = (uint32_t)info.bitrate_kbps;
        {
            uint32_t i, w = pcm_w;
            for (i = 0; i < (uint32_t)samples; i++, w++) {
                int16_t *d = &pcm_buf[(w % PCM_FRAMES) * 2u];
                if (info.channels == 2) {
                    d[0] = src[2u * i];
                    d[1] = src[2u * i + 1u];
                } else {
                    d[0] = d[1] = src[i];
                }
            }
            RING_PUBLISH();
            pcm_w = w;
        }
#ifdef RADIO_RUN_BETWEEN
        shim_run();                               /* the network side gets its turn between frames */
#endif
    }
    /* play once half the stream buffer and the decoded audio are full; dry: pause and refill */
    if (radio_st == RS_BUFFERING && pcm_w - pcm_r >= PCM_FRAMES - (dec_kind == 2 ? 2048u : 1152u) &&
        (radio_mode == 2 || radio_wr - radio_rd >= NET_RING / 2u || radio_net_state != RN_STREAMING))
        radio_st = RS_PLAYING;
    else if (radio_mode != 2 && radio_st == RS_PLAYING && pcm_w == pcm_r && radio_wr - radio_rd < 2048u &&
             radio_net_state == RN_STREAMING) {
        radio_st = RS_BUFFERING;
        radio_rebuffers++;
    }

}

/* ---- punch-in FX: held keys (fx_held, a bit per effect, the main loop's), applied in the audio interrupt.
 * Sources (one at a time, the lowest held): stutters, tape stop, reverse, half speed, played from a history
 * of the last ~280 ms (mono, 22 kHz, frozen while a source plays). Then the filters, the crushers, the ring
 * modulator, the chops and the mute. */
enum { FX_LP, FX_HP, FX_PHONE, FX_CRUSH, FX_ST4, FX_ST8, FX_ST16, FX_ST32, FX_TAPE, FX_REV, FX_CHOP8, FX_CHOP16,
       FX_RING, FX_HALF, FX_MUTE, FX_LOFI, FX_N };
static const char *const FX_NAME[FX_N] = {"low-pass", "high-pass", "telephone", "crush", "stutter 1/8",
    "stutter 1/16", "stutter 1/32", "stutter 1/64", "tape stop", "reverse", "chop 1/8", "chop 1/16", "ring mod",
    "half speed", "mute", "lo-fi"};
#define FX_SOURCES ((1u << FX_ST4) | (1u << FX_ST8) | (1u << FX_ST16) | (1u << FX_ST32) | (1u << FX_TAPE) | \
                    (1u << FX_REV) | (1u << FX_HALF))
static volatile uint32_t fx_held;
static volatile uint32_t fx_beat = 22050u;        /* a beat in output samples (44.1 kHz): the tap tempo, 120 BPM */
static struct {
    uint32_t hw, half;                            /* history write index, the half-rate phase */
    uint32_t src, start;                          /* the source playing (FX_N: none), where its window starts */
    float pos, speed;                             /* read position in the history (samples), its rate */
    float lp[2][2], hp[2][2], bp[2][2];           /* the filters' states: [channel][low, band] */
    float gate, ring_c, ring_s;                   /* the chop / mute gain; the ring oscillator */
    uint32_t crush_n, chop_n, len, held_was;      /* len: the stutter's loop, history samples */
    float crush_l, crush_r;
} fx __attribute__((section(".pool")));                                     /* set up by fx_reset */

static void fx_reset(void)
{
    memset(&fx, 0, sizeof fx);
    fx.src = FX_N;
    fx.gate = 1.0f;
    fx.ring_c = 1.0f;
}

/* a Chamberlin state-variable filter: f = 2 sin(pi fc / fs), q = 1 / Q; returns low, band or high */
static inline float fx_svf(float *st, float x, float f, float q, int out)
{
    float lo = st[0] + f * st[1], hi = x - lo - q * st[1], bd = f * hi + st[1];
    st[0] = lo;
    st[1] = bd;
    return out == 0 ? lo : out == 1 ? bd : hi;
}

static float fx_hist_at(float p)                  /* the history at position p (samples, wrapping), linear */
{
    uint32_t i = (uint32_t)p;
    float t = p - (float)i, a = fx_hist[i % FX_HIST], b = fx_hist[(i + 1u) % FX_HIST];
    return (a + (b - a) * t) * (1.0f / 32768.0f);
}

/* one stereo sample through the held effects (x: -1..1) */
static void fx_process(float *l, float *r)
{
    uint32_t held = fx_held, src = FX_N, k;
    float m;
    for (k = 0; fx_hist && k < FX_N; k++)                    /* the lowest held source */
        if ((held >> k & 1u) && (FX_SOURCES >> k & 1u)) {
            src = k;
            break;
        }
    if (src != fx.src) {                          /* a source starts (or ends): its window in the history */
        fx.src = src;
        if (src >= FX_ST4 && src <= FX_ST32) {    /* a 1/8 .. 1/64 note at the tempo, halved while too long */
            uint32_t n = fx_beat >> (2u + (src - FX_ST4));   /* half a beat at 44.1 kHz = a beat >> 2 at 22.05 */
            while (n > FX_HIST - 64u)
                n >>= 1;
            fx.len = n ? n : 1u;
            fx.start = (fx.hw + FX_HIST - fx.len) % FX_HIST;
            fx.pos = (float)fx.start;
            fx.speed = 0.5f;
        } else if (src == FX_TAPE || src == FX_HALF) {
            fx.start = (fx.hw + 1u) % FX_HIST;    /* the oldest: plays the last ~280 ms */
            fx.pos = (float)fx.start;
            fx.speed = src == FX_TAPE ? 0.5f : 0.25f;
        } else if (src == FX_REV) {
            fx.pos = (float)((fx.hw + FX_HIST - 1u) % FX_HIST);
            fx.speed = -0.5f;
        }
    }
    if (src == FX_N) {                            /* the live sound into the history, at half rate */
        if (fx_hist && ++fx.half & 1u)
            fx_hist[fx.hw = (fx.hw + 1u) % FX_HIST] = (int16_t)((*l + *r) * 16383.0f);
    } else {
        float v = fx_hist_at(fx.pos);
        *l = *r = v;
        fx.pos += fx.speed;
        if (src >= FX_ST4 && src <= FX_ST32) {
            float end = (float)fx.start + (float)fx.len;
            if (fx.pos >= end)
                fx.pos -= (float)fx.len;
        } else if (src == FX_TAPE) {
            fx.speed *= 0.99996f;                 /* to a stop in about a second */
        }
        if (fx.pos >= (float)FX_HIST)
            fx.pos -= (float)FX_HIST;
        if (fx.pos < 0.0f)
            fx.pos += (float)FX_HIST;
    }
    if (held >> FX_LP & 1u) {                     /* 500 Hz */
        *l = fx_svf(fx.lp[0], *l, 0.0712f, 0.5f, 0);
        *r = fx_svf(fx.lp[1], *r, 0.0712f, 0.5f, 0);
    }
    if (held >> FX_HP & 1u) {                     /* 1.5 kHz */
        *l = fx_svf(fx.hp[0], *l, 0.2133f, 0.7f, 2);
        *r = fx_svf(fx.hp[1], *r, 0.2133f, 0.7f, 2);
    }
    if (held >> FX_PHONE & 1u) {                  /* a band around 1 kHz, mono, driven */
        m = fx_svf(fx.bp[0], (*l + *r) * 0.5f, 0.1424f, 0.6f, 1) * 2.5f;
        m = m > 0.8f ? 0.8f : m < -0.8f ? -0.8f : m;
        *l = *r = m;
    }
    if (held & (1u << FX_CRUSH | 1u << FX_LOFI)) {   /* sample-and-hold, then fewer levels */
        uint32_t hold = held >> FX_CRUSH & 1u ? 6u : 2u;
        float lv = held >> FX_CRUSH & 1u ? 12.0f : 48.0f;
        if (++fx.crush_n >= hold) {
            fx.crush_n = 0;
            fx.crush_l = (float)(int32_t)(*l * lv) / lv;
            fx.crush_r = (float)(int32_t)(*r * lv) / lv;
        }
        *l = fx.crush_l;
        *r = fx.crush_r;
    }
    if (held >> FX_RING & 1u) {                   /* x a 330 Hz sine (a rotating phasor) */
        float c = fx.ring_c, sn = fx.ring_s, e = 0.04700f;   /* 2 pi 330 / 44100 */
        fx.ring_c = c - e * sn;
        fx.ring_s = sn + e * fx.ring_c;
        *l *= fx.ring_s;
        *r *= fx.ring_s;
    }
    {   /* the chops (1/8 or 1/16 notes at the tempo, open for the first half, from the press) and the mute: a
         * gain that moves in 2 ms, not a click */
        float target = 1.0f;
        uint32_t chops = held & (1u << FX_CHOP8 | 1u << FX_CHOP16);
        if (chops & ~fx.held_was)
            fx.chop_n = 0;                        /* a chop pressed: it starts open, on the press */
        fx.held_was = held;
        fx.chop_n++;
        if (chops) {
            uint32_t period = held >> FX_CHOP16 & 1u ? fx_beat >> 2 : fx_beat >> 1;
            if (period < 64u)
                period = 64u;
            target = fx.chop_n % period < period / 2u ? 1.0f : 0.0f;
        }
        if (held >> FX_MUTE & 1u)
            target = 0.0f;
        fx.gate += (target - fx.gate) * 0.011f;
        *l *= fx.gate;
        *r *= fx.gate;
    }
}

/* the audio interrupt's: n frames into o (24-bit in 32, stereo) */
static void radio_render(int32_t *o, uint32_t n)
{
    uint32_t i, r = pcm_r, g = plat_master();     /* Q12 */
    int playing = radio_st == RS_PLAYING;
    float gain = (float)g * 2048.0f;              /* -1..1 -> 24 bit, the MASTER pot */
    uint32_t step = radio_step, w = pcm_w;
    for (i = 0; i < n; i++) {
        float l = 0.0f, rr = 0.0f;
        if (playing && r != w) {                  /* linear between frame r and the next */
            const int16_t *s = &pcm_buf[(r % PCM_FRAMES) * 2u];
            const int16_t *s1 = r + 1u != w ? &pcm_buf[((r + 1u) % PCM_FRAMES) * 2u] : s;
            float t = (float)radio_frac * (1.0f / 65536.0f);
            uint32_t adv;
            l = ((float)s[0] + (float)(s1[0] - s[0]) * t) * (1.0f / 32768.0f);
            rr = ((float)s[1] + (float)(s1[1] - s[1]) * t) * (1.0f / 32768.0f);
            radio_frac += step;
            adv = radio_frac >> 16;
            radio_frac &= 0xFFFFu;
            r += adv < w - r ? adv : w - r;
        } else if (playing) {
            radio_underruns++;
        }
        if (playing)
            fx_process(&l, &rr);
        o[2u * i] = (int32_t)(l * gain);
        o[2u * i + 1u] = (int32_t)(rr * gain);
    }
    pcm_r = r;
}

static const char *radio_status_str(void)
{
    static const char *const S[] = {"OFF", "CONNECTING", "BUFFERING", "PLAYING", "STOPPED"};
    return radio_st < 5u ? S[radio_st] : "?";
}
