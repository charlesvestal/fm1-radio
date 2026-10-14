# fm1-radio: an internet radio for the FM-1

A proof of concept that turns the M-VAVE FM-1 into a pocket internet radio. It joins a 2.4 GHz WiFi network,
plays MP3 streams from Radio Garden's stations around the world, and lets you play the radio with
punch-in effects on the keys.

**Source only.** This firmware links JieLi's closed WiFi and Bluetooth libraries (from the AC79 SDK) with
GPL-3.0 code from Felucca. A built binary can't be distributed until Hügelton Instruments grants a GPL
section 7 permission for those libraries, so there are no releases. You can build it yourself from your own
SDK checkout.

## What it does

- **WiFi.** Scan, join, save up to six networks, and type passwords on the 27 keys.
- **Radio Garden.** Stations live in the FM-1's data flash: about 3,600 plain-HTTP MP3 stations in
  850 cities. K1 to K4 pick the continent, country, city and station; SEL opens a list to browse.
- **Punch-in FX.** Hold the white keys on Home: filters, telephone, bitcrush, stutters, tape stop,
  reverse, chops, ring mod, half speed, mute and lo-fi. Hold several to mix them.
- **Debug over USB-MIDI.** `fw/tools/wifi_ctl.py` reads the crash record and the log, drives the
  radio and uploads the station table.

## Build

```sh
cd fw
PATH=~/github/fm1-parent/bin:$PATH AC79_SDK=~/fw-AC79_AIoT_SDK JIELI_TOOLCHAIN=~/.jieli/toolchain \
    python3 tools/build.py --wifi          # build/omni.fwsc
python3 tools/fm1_install.py build/omni.fwsc --yes
python3 tools/rg_sync.py                   # the station table, from Radio Garden (about 30 minutes)
python3 tools/wifi_ctl.py rgload build/rg/stations.bin
```

The station table is built on your computer and never committed: it is Radio Garden's data.

## History

The repository began as fm1-ble, which got JieLi's BLE stack running on a small cooperative OS shim inside
a Felucca firmware (BLE MIDI works). The notes in `notes/` are from that work. The WiFi radio reuses the shim.

## Licence and credits

GPL-3.0-only, as Felucca. For where each part comes from, see `fw/LICENSING.md`.
