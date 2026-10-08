# 06: handoff, the BLE MIDI controller proof of concept

Written 2026-10-08 for whoever (or whichever model) picks this up. Read `05-midi-works.md` first: BLE MIDI
works on the FM-1 in the experiment firmware in `fw/` (FoMni + the BLE pieces). The job now is a firmware
whose point is Bluetooth MIDI: a controller, not a synth.

## The goal

A generic BLE MIDI controller firmware for the FM-1: keys to notes, knobs to CC, out over BLE, USB and
TRS; MIDI in over BLE forwarded to USB and TRS (a bridge); one or two setup pages; light, friendly UI
(FoMni's cream / coral / teal), simple over complete. Private repo until the licence is settled.

## How to start it

Follow `fm1-parent/CLAUDE.md`, "Starting a new firmware": copy `fm1-fomni` (`git ls-files`, minus docs, site
pages, tests/scenarios), drop `firmware/src/dsp` and write a small `ui.c`; keep `hal/`, `loader/`, `usb.c`,
`storage.c`, `ota.c`, `main_fm1.c`, `panel.c`, host simulator, web emulator. Own identity (`FM-1_` + a free
digit + version digits; 9, 8, 7 are taken) and USB name. Then lift from `fm1-ble/fw`:

| From | What | Notes |
|---|---|---|
| `ble/shim/` | JieLi's OS API on a cooperative scheduler: `os.c`, `systimer.c`, `misc.c` (libc, heap, lbuf, CRC, interrupts, power stubs, syscfg, radio constants), `ctx.S`, `trace.S`, `shim.h`, `rf_const.c` | self-contained; the firmware supplies the `shim_*` platform hooks (see `shim.h`) |
| `ble/midi/le_midi.c` | the BLE MIDI profile: GATT db, advertising ("FM-1" + service UUID), packets in (`ble_midi_receive`), batched out (`ble_midi_send_many`) | replaces the SDK demo `le_trans_data.c` |
| `firmware/src/app/ble.c` | the glue: the `shim_*` hooks, start-up, the MIDI queues, the diagnostics | the FoMni coupling is only `midi_in_q` / `plat_midi_out` (`plat_fm1.c`) |
| `firmware/hal/fm1_irq.h` | additions: crash record fields, `fm1_irq_unmask`, `fm1_ipnd_or`, soft-IRQ helpers, `fm1_uptime_ms`, `fm1_clk_snapshot` | HAL only (the build checks register access) |
| `tools/build.py` | `ble_units()`: copies SDK headers and libs into `build/ble/sdk`, compiles shim + midi + `bt_profile_config.c` with the SDK's flags, links the libs in one group | `BLE_LIBS`, `BLE_CF`, `BLE_INC` |
| `firmware/app.ld` | the BT sections into XIP, `.volatile_ram_code` into RAMTEXT | |

JieLi's libraries never go into git (`.gitignore`); the build copies them from `~/fw-AC79_AIoT_SDK`.

## Things that are not obvious

- **Start-up masking is required.** Every crash was an interrupt vectoring to a wrong address while the
  radio's receiver calibration ran (0.7 s into start); the last one was interrupt 0 with nothing pending.
  `ble_start` masks Felucca's audio and timer interrupts until the stack reports init ok (or 15 s), and
  the shim's clock is TIMER4 (`fm1_uptime_ms`) so nothing waits on the tick. In a controller there is no
  audio ISR, but the input timer (TIMER5) still is one: keep the masking, and keep `shim_ms` on TIMER4.
  Root cause unknown; the crash note's `seen` line would name pending sources if it recurs.
- **BT interrupts run at priority 7**, mapped from the SDK's 2 (`shim_request_irq`, `prio + 5`). Priority
  7 is maskable by `cli` on this chip as booted (probed). The SDK's own handlers save everything and end
  in `rti`; they can nest inside Felucca's wrappers.
- **The stack's context is the main loop**: `ble_poll()` runs the shim's timers and every BT task that can
  go; `btstack_init` is called from the main loop and the BT tasks are created on the shim's scheduler.
  Library calls (`ble_midi_send_many`, anything `ble_op_*`) only from the main loop, never from an ISR.
  MIDI out from an ISR goes through a ring (`ble_midi_tap` -> `ble_midi_drain`).
- **`low_power_get` must return non-null** (the controller asserts); `config_asser` is a `const int`.
- **Felucca's P33 rails are already the SDK's** (VDC14 1.40 V, SYSVDD 1.26 V); no power setup needed.
- **Costs**: +124 KB code, +20 KB static RAM, +48 KB pool (3 task stacks of 8 KiB, 24 KiB heap). The
  stacks' real depth is in the log (`shim_stack_depth`); they can likely shrink.
- **Diagnostics to keep while it is new**: the BLE log on the Setup page (`.noinit`, survives resets),
  the crash note, the live `irq/c/rx/tx` line, `trace.S`. Drop them when it is boring.
- **Pairing**: `sm_set_request_security(0)`; a Mac connects without pairing. iOS may want bonding; the
  SM handler answers just-works.
- **The host simulator in `fw/` is broken** (ui.c references the BLE page hooks the simulator never
  defines). In the new firmware, give the simulator stubs for them from the start.
- **Licensing**: JieLi's SDK is Apache-2.0, binary-only libs. A GPLv3 (Felucca-derived) binary linking
  them cannot offer corresponding source, so distribution needs a GPLv3 section 7 additional permission
  from Felucca's copyright holders (asked), or an MIT platform rewrite, or source-only distribution.

## Scope for the first cut

1. Keys: white and black keys as notes (a fixed layout, octave on OCT-/OCT+), velocity fixed.
2. Knobs: four CCs, numbers on a setup page.
3. Out: BLE, USB MIDI and TRS at once. In: BLE -> USB and TRS; USB -> BLE and TRS.
4. A BLE page: on/off, state, connected, counters. Bluetooth off at boot until it is trusted.
5. Tests: the host simulator (with BLE stubs), the web build.

## Status (2026-10-08)

Started as `fm1-parent/fm1-ctl` ("Ctl", identity FM-1_6, a local git repo with no remote). Scope 1-5 done
as far as the desk goes: the firmware builds (218 KB) and its checks pass; host tests, three simulator
scenarios (notes, thru routing, Bluetooth, persistence) and the web build pass. Not yet run on the FM-1.
Differences from the plan above: the TRS jack is MIDI in only on the FM-1, so TRS is forwarded, never
sent to; at BT start only TIMER5 is masked (no ALNK0 in a controller); BLE MIDI in has its own queue
(main loop) instead of sharing `midi_in_q` with the TIMER5 producers; output to BLE is dropped while
no central listens.

## The crash's likely root cause (2026-10-08, fm1-ctl)

Ctl crashed at its first BT start like fm1-ble used to: 728 ms in, inside `adc_pp_tune` / `rd_adc_peak`,
returning to 0x030339BC instead of 0x020339BC (one high bit wrong, as in every crash in 03-05; JieLi's ISRs
never ran, nothing pending). JieLi's demo configs say why: `CONFIG_RF_TRIM_CODE_AT_RAM` puts the RF trim code
in RAM "so that flash access during the RF trim's ~500 ms of high current does not make the flash fail".
fm1-ble's app.ld put `*.rf.text.cache.L2.rf_trim` in XIP; the masking in 05 only made it less likely. fm1-ctl
now runs that section, and the shim's delays (TIMER4, watchdog fed inline), from RAM. Pending a device run.
