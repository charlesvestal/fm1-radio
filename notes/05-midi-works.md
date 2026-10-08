# 05: BLE MIDI works (2026-10-08)

The FM-1 advertises as "FM-1" with the BLE MIDI service; a Mac connects through Audio MIDI Setup; strums
arrive as MIDI (note-offs included, batched per packet); MIDI from the Mac reaches the engine.

## What the crashes were

Every crash in `03` and the ones here: an interrupt vectoring to a wrong address, system stack empty,
the core spinning in the radio's receiver calibration (`rd_adc_peak`, about 0.7 s into start). The last
one vectored to interrupt 0 with nothing pending: a spurious interrupt. The RF calibration disturbs the
interrupt controller; the mechanism itself is still open.

Measured and dead: priority 7 is maskable by `cli` (the probe read 2); JieLi's handlers never ran before
the crashes; clocks, SFC and the P33 rails (VDC14 1.40 V, SYSVDD 1.26 V: the SDK's values) were constant.

## What was changed

- `ble.c`: Felucca's audio and timer interrupts stay masked from BT start until the stack says init ok
  (or 15 s); the shim's clock is TIMER4 (`fm1_uptime_ms`) so nothing depends on the tick meanwhile.
  Audio pauses about a second when Bluetooth starts. A workaround, not a fix.
- `shim/misc.c`: `low_power_get` returns a real (dummy) handle: the controller asserts on null;
  `config_asser` is the SDK's `const int`.
- `ble/midi/le_midi.c` replaces the SDK's `le_trans_data.c`: the MIDI GATT database, advertising with
  the 128-bit service UUID and the name, BLE MIDI packet parsing in, batching out, the demo's connection
  handling. `ble.c` glues it to FoMni's MIDI queues (`plat_midi_out` tap, `midi_in_q`).
- Diagnostics kept: the entry trampolines on the BLE vectors, the seven-line crash note (now with the
  pending bits seen during the run, `seen`), the live `irq/c/rx/tx` line.

## Open

- The spurious-interrupt root cause (if it recurs, the `seen` line names the pending sources).
- Occasional disconnects were seen before batching; the log now prints the HCI reason.
- The goal is a generic BLE MIDI prototype, not a FoMni feature: `ble/shim`, `ble/midi` and the glue in
  `ble.c` are the parts to lift; the FoMni queues are the only coupling.
- Licensing as before.
