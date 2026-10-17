# AAC crashes on the FM-1 (2026-10-17)

AAC streams crashed the FM-1 every few minutes (Nova FM 91.9, HE-AAC 48 kbit/s), always in the decoder's
fixed-point loops (SBR's analysis filterbank, the FFT, the DCTs). MP3 played for hours.

## What it is

A CPU register sometimes takes a wrong value in code where a 32x32->64-bit multiply (`r9_r8 = r3 * r15 (s)`) is
inlined among 16-bit loads (`r4 = h[r5+-380] (u)`). Each crash record shows one register off by a single high bit:
the loop index 0x12 read as 0x00800012, a counter 0 as 0x00800000, or `sp` off by a word so a return pops a
neighbouring value (0x470). Memory itself stays right.

What the tests on the device showed, each in local mode (WiFi off, `radio_ctl.py play local`):

| Test | Result |
| --- | --- |
| HE-AAC from a clip built into the image, WiFi off | crashed (WiFi and its current are not the cause) |
| the same, each decode with interrupts off | crashed (not an interrupt) |
| the decoder built without instruction pairing (`-pi32v2-parallel-pack=false`) | crashed |
| SBR's filterbank alone, on fixed input, at `-O2` | crashed within a minute, every time |
| the same at `minsize` (its multiplies become calls) | 5 min clean |
| the same at `-O2` with a `nop` after each multiply | crashed |
| the same at `-O2` with the multiply helpers out of line | 8 min clean |
| 1.2 billion 64-bit multiplies, checked | no error |
| 2 billion 16-bit loads, checked | no error |
| 16-bit loads beside 64-bit multiplies | crashed within a minute |
| 32-bit loads beside 64-bit multiplies | 2 errors in 4 billion |
| SYSVDD 13 (1.32 V) instead of 15 | crashed as fast |

The fix: PacketVideo's multiply helpers (`fxp_mul32_c_equivalent.h`) are no longer inlined; one copy each lives in
`fm1_fxp.c`. In a function of its own the multiply's result is used at once, so it completes before any load that
follows. The decoder is about 50% slower (HE-AAC 7.8 ms a frame of 46, was 5.1) and the output is the same, sample
for sample.

Other code with inlined 64-bit multiplies: BearSSL's P-256 and x25519 field arithmetic (`mul_f256`, `square_f256`,
run in TLS handshakes, mostly 32-bit loads). minimp3 uses floats and has none.

## The test patches

Not applied anywhere; apply with `git apply` in the directory named:

- `aac-crash-instrumentation-crashlog-bsp.patch` (base/bsp/) and `-crashlog-base.patch` (base/): every register,
  `usp` and two of its stack slots at the last four crashes, printed by `fm1_remote.py log`.
- `aac-crash-instrumentation-base.patch` (base/): large allocations with their callers, `free()` refusing double
  and foreign frees, a free-list check, the WiFi interrupt at priority 2 (and an older one-crash register log).
- `aac-crash-instrumentation-app.patch` (app/): guards around the decoder's memory.
