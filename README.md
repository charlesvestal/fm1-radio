# fm1-ble: Bluetooth MIDI on the FM-1, an experiment

PRIVATE. Never push this repository anywhere public: it works with JieLi's closed Bluetooth
libraries, which are not GPL-compatible. The question it answers: can JieLi's BLE stack
(`btctrler.a`, `btstack.a` from the AC79 SDK) run inside a Felucca-based bare-metal firmware,
without JieLi's operating system, well enough to be a BLE MIDI peripheral?

If yes, the licence route is decided separately (a linking exception from Hügelton, or the
stack on the second core as a separate program, or a build the user does themselves).

## Safety

The experiment never runs JieLi's SDK firmware on the FM-1. Bluetooth starts inside our own
firmware, after a normal boot, outside safe mode, so Felucca's boot guard, safe mode and web
installer stay in charge: a crash or hang ends in safe mode, from where the installer works.

## Steps

1. Link probe (desk only): what the libraries need, without JieLi's OS. `notes/01-symbols.md`
2. OS shim: tasks, semaphores, queues, timers, IRQ registration, the radio's setup.
3. Device: advertise; then a BLE MIDI GATT service; then MIDI both ways.
