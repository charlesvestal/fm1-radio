# 02: the link probe

`probe/build.sh`: the SDK's BLE peripheral example (`apps/common/ble/le_trans_data.c`), its
profile config and log-tag files, linked from `btstack_init()` + `bt_ble_init()` against
`btctrler.a`, `btstack.a`, `crypto_toolbox_Osize.a`, `lib_ccm_aes.a` (in one --start-group:
the stack calls the controller) **without** JieLi's OS (`system.a`, `cpu.a`, `event.a`).
JieLi's `lto-wrapper` is a Python script around `ld --plugin LLVMgold.so`; we call ld directly.

## What is missing: 87 (`02-undefined-bt-only.txt`), 115 with the radio's `wl_rf_common.a`

| Group | Symbols | Ours to write |
|---|---|---|
| libc | memcpy memset memcmp strcmp strcpy strlen malloc free zalloc ram_malloc ram_free printf puts putchar put_buf printf_buf log_print | trivial (a small heap) |
| interrupts | local_irq_disable/enable, __local_irq_*, sys_local_irq_*, cpu_in_irq, cpu_irq_disabled, request_irq | Felucca's irq HAL; request_irq needs the BT vectors |
| OS | os_sem_create/pend/post, os_mutex_create/pend/post/del/valid, os_taskq_pend, os_taskq_post_type, os_time_dly, os_current_task(_rom), task_create | **the real work**: two tasks that block. A small cooperative scheduler with stack switching |
| timers | sys_timer_add/del/modify/re_run/set_user_data, sys_timeout_add/del, *_add_to_task | on the 1 ms tick, run in the stack's task |
| buffers, CRC | lbuf_* (alloc, free, realloc, real_size, free_check), cbuf_*, chip_crc16, crc_get_16bit/32bit, CRC16 | small; must match JieLi's semantics (headers in include_lib/utils) |
| power | low_power_* (get, put, request, on, trace_drift, exit_request, hw_unsleep_*), power_is_poweroff_post, P33_SYSTEM_RESET | stubs: never sleep |
| storage | syscfg_read / syscfg_write | the BT address and bonding keys, in our flash store |
| hooks | bt_event_notify, a2dp_* begin/end, config_asser, cpu_assert_debug, aes_hw_lock/unlock, keep_BT_* | stubs / constants |
| radio (`wl_rf_common.a`) | wf_rf_common_init, wlc_main, wf_get_phcom_cnt; and its own: wifi_get_rf_trim_data, wifi_get_pa_trim_data, wifi_get_xosc, wifi_get_mcs_dgain, adc_*, clk_get, delay_us, RF tuning constants (STF_*, DS_*, DSSS_*), soft-double helpers | **the unknown**: the factory calibration (trim, crystal) normally comes from JieLi's cfg area / efuse; must read it the same way |

## Size (gc'd from the probe's entry, no debug)

code ~97 KB (.bt_stack_code 22 KB, .text 20 KB, ...), rodata ~18 KB, bss ~4 KB. A BLE MIDI build
would fit beside FoMni-1 (158 KB) in the 581 KB app slot.

## Next

- Read the SDK headers for each group (exact semantics: os_* return codes, lbuf layout, sys_timer).
- Find where the radio's trim / crystal values come from on the FM-1 (the stock firmware's cfg).
- Write the shim, link it into a Felucca firmware, and get the first advertisement.
