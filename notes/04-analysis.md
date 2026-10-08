# 04: reading the crashes (desk work; the device run is pending)

Picked up from `03-device-results.md`. Everything here is from the image (`fw/build/omni.dis`), the SDK's
sources and JieLi's `system.a` bitcode (`llvm-dis` from Homebrew's llvm reads it); nothing ran on the FM-1.

## What the two wild PCs are

The last two crashes jumped to `0x02800470` and `0x00000470`. `0x02000470` is the second instruction of
Felucca's `isr_timer5` (prio 4): the one right after `[--sp] = {psr, rets, reti}`. The low 16 bits match
twice; the odds of chance are nil. So in both cases the return address a handler restored was that
address with one high bit wrong (bit 23 set, or bit 25 cleared). The faults themselves say the same:
`dbg 0x1000` is `c0_pc_limit_err` (fetch above `_etext`), `dbg 0x80000` is `c0_if_bus_inv` (fetch from
unmapped space); `0x10` from the earlier om_render crash is `axi_rd_inv` (names: SDK `cpu/wl82/debug.c`).

The last crash's `sp 0x01C7BFFC` is the system stack with only the fatal stub's push on it: the bad
return happened from a handler whose frame was alone at the top of the system stack, so its saved
`reti` lived at `0x01C7BFF4`, twelve bytes under `fm1_crash` (the first `.noinit` object, at
`_sstack_top`). The Felucca ISRs (`isr_alnk0`, `isr_timer5`) and JieLi's two handlers all save
`{psr, rets, reti}` there when they interrupt the main loop.

## What JieLi's side looks like

- `ble_event_irq_handler` (IRQ 45) and `ble_rx_irq_handler` (IRQ 29) are C with
  `__attribute__((interrupt))`: `[--sp] = {psr, rets, reti}; [--sp] = {r15-r0}; sp += -12 ... rti`.
  No `csync` before `rti` (Felucca's wrappers have one). They are the only `rti`s in the image besides
  Felucca's two. They call only library-internal functions and `hw_dma_empty`; no `os_*`, no shim.
- JieLi's `request_irq` (system.a, `hwi.c.o`) does what the shim does: handler straight into the vector
  table at `0x1C7FE00`, `ICFG = (prio << 1) | 1`. So installation is not the difference.
- The SDK runs these two at priority 2 (above its own peripherals, below only the exception at 7). The
  shim maps them to **7** (`prio + 5`), above Felucca's audio (3) and input timer (4), so they nest
  inside Felucca's wrappers constantly, which the SDK never does.
- Priority 7 and `cli`: with `CONFIG_IPMASK_ENABLE` the SDK writes `IPMASK = 7` ("interrupts of
  priority >= 7 become non-maskable", `apps/common/system/init.c`) and its rules for such handlers are:
  code and constants in internal RAM, no flash writes while they can fire, no system API. Felucca never
  touches IPMASK, so whether 7 is non-maskable on the FM-1 as booted is unknown: the probe below
  measures it. If it is, every `cli` critical section (the shim's queues and heap, Felucca's flash
  driver with XIP off during `syscfg_write`, the ISR epilogues) is open to the BT handlers.
- The BT libraries write the settings store at start (`bt_get_mac_addr`, `sm_init`, `ble_list_init`,
  `tx_rx_bw_set -> wifi_set_rf_trim_data` after calibration): flash programming with XIP off.

## The instrumented build (`fw/`, unflashed: no FM-1 was attached)

- `ble/shim/trace.S`: the two BLE vectors point at trampolines that note each entry (`reti`, sp) in a
  ring of 16 in `.noinit`, then jump to the real handler through a stack slot with every register as the
  interrupt left it (psr is saved before any arithmetic). The handlers are library-internal symbols, so
  their addresses are taken from `request_irq` at run time.
- The crash note (Setup page, seven lines): `pc v emu` / `dbg sp rets` / `etm` x2 (the core's branch
  trace, four PCs) / `psr icfg` / the last BT ISR entry's count, `reti` and entry sp / the one before.
- At BT start one line: `ipm <IPMASK> p7 <probe> p6 <probe> icfg <icfg>`. The probe raises soft
  interrupt 0 at that priority under `cli`, then after `sti`: 1 = fired under cli (non-maskable),
  2 = only after sti (maskable), 3 = both, 0 = never.

## Next

1. Flash `fw/build/omni.fwsc`, Setup page, KNOB 4, wait for the crash, read the seven lines.
2. If `p7` is 1 or 3: priority 7 is non-maskable; map the BT IRQs to 5 (still above audio and the
   input timer, below the exception) and run again. One variable at a time.
3. If the `etm` PCs show the bad jump leaving JieLi's `rti` at `0x203c980` / `0x203cf84` with the
   previous entry's `reti` = `0x02000470`, the nesting of JieLi's compiled handler inside Felucca's
   prologue is the mechanism; then try the BT IRQs at or below 4, or give them Felucca-style wrappers.
