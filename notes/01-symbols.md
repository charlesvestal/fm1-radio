# 01: what the BT libraries need

- `btctrler.a` (107 members, 5.1 MB) and `btstack.a` (71 members, 3.0 MB) are LLVM **bitcode**
  (LTO), plus tiny ELF version objects. Read with a modern `llvm-nm` (emscripten's).
- Together they define ~4800 symbols and reference **386** they don't define.
- Providers among the SDK's libraries: `system.a` 60 (JieLi's OS layer over FreeRTOS:
  os_sem/mutex/taskq, task_create, sys_timer/timeout, lbuf/cbuf, malloc, request_irq, printf),
  `cpu.a` 28 (clocks, low power, udelay, CRC, chip id, RF DC), `crypto_toolbox_Osize.a` 20
  (uECC, AES-CMAC), `wl_rf_common.a` 10 (radio setup shared with Wi-Fi), a few others.
- 246 are defined by no library: 157 in the SDK's source (config constants, log tags,
  `bt_profile_config.c`), the rest belong to features a BLE MIDI peripheral never pulls in
  (TWS earbud pairing, A2DP, ISO/BIS/CIS).
- The SDK runs the stack as two OS tasks: `btctrler` (prio 19) and `btstack` (prio 18).
- Real closure: the link probe (`probe/`), which pulls only the objects BLE needs.
