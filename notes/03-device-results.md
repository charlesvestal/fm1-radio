# 03: on the device (stopped here)

Experiment firmware: FoMni-1 + the shim + JieLi's BLE stack (`fw/`), identity FM-1_899, BT started by
hand from the Setup page (KNOB 4), never at boot. Felucca's boot guard, safe mode and update path stay in
charge; every failure below ended in a clean restart into FoMni-1.

## What works

- The whole BLE stack links with no JieLi OS: 87 functions shimmed (115 with the radio library),
  ~125 KB of code (image 285 KB with FoMni-1), RAM 49 KB + pool 254 KB.
- `btstack_init()` runs: `le_config:1111`, `btctrler_task_init`, tasks created on the cooperative
  scheduler (shim/os.c), the radio's crystal trim read (`wifi_get_xosc 11/11`, the SDK board defaults),
  the PLL bank calibration completes (`bak ... x47`, `setbak:32`).

## What failed, in order

| Build | Result | Learned |
|---|---|---|
| first | reset during calibration | the calibration loops for seconds on delays; nothing fed the watchdog |
| watchdog fed from delays/printf | crash in om_render (audio ISR), store into guard | first thought: 4 KiB task stacks too small |
| 8 KiB stacks in the pool | crash in om_render, `[r2+r3<<2] = r0`, dbg 0x10 (bus) | ISRs run on the system stack (sp 0x01C7BEB4), not the task stacks |
| reverb table checked each block | crash, PC 0x02800470, dbg 0x1000 (PC limit), from om_render's call site | the reverb table was never corrupted: the ISR's own state / return was |
| render silenced while BT runs | crash, PC 0x00000470, dbg 0x80000 (bus), sp 0x01C7BFFC, rets in fm1__p33_op (WDT feed) | not the render: an interrupt-level fault after calibration |

Two wild PCs ending in 0x470 suggest one corrupted jump target. JieLi's ISRs save psr/rets/reti/r0-r15
and use no hardware loops (`rep`); only FoMni's render does. IRQ numbering matches Felucca's (11, 63).

## Where to look next, if anyone picks it up

- What the controller does right after `setbak`: which hardware it arms (baseband interrupts, IRQ 41
  BT_CLKN, 42 BT_DBG; Wi-Fi coexistence), and whether something expects JieLi's OS interrupt entry/exit
  (their FreeRTOS port may wrap ISRs: nesting / `reti` handling we do not reproduce).
- Run with only the BLE IRQs at JieLi's own priorities (no remap above the audio), audio off.
- Compare the stock M-VAVE firmware's flash (its VM area) for the FM-1's real RF calibration.
- Licensing is unchanged: shipping needs Hügelton's linking exception, or a user-side build.
