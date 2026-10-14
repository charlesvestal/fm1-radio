# fm1-radio: an internet radio for the FM-1

fm1-radio turns the M-VAVE FM-1 into a pocket internet radio. It joins a 2.4 GHz WiFi network, plays
stations from around the world from Radio Garden's map, and lets you play the radio with punch-in
effects on the keys.

It is a proof of concept: it works, and the rough edges are listed at the end.

## Licensing status

**Source only, no binaries.** The source is free software under GPL-3.0-only, the licence of
[Felucca](https://github.com/hugelton/Felucca), whose platform layer it is built on. A built firmware
links JieLi's closed WiFi and Bluetooth libraries from the AC79 SDK. The GPL does not allow
distributing that combination without an additional permission under its section 7 from Felucca's
copyright holder, Hügelton Instruments. That permission has been asked for and not yet given. Until
then:

- this repository has no releases and no firmware files;
- you may build the firmware for your own FM-1, but you may not share the built file.

For where each part of the code comes from, see [`fw/LICENSING.md`](fw/LICENSING.md).

## Building

### What you need

- macOS or Linux, with Python 3.
- Pillow with libraqm (the build draws its font with it). Homebrew's Python has both:
  `brew install python pillow`. Use that Python to run the build.
- mido and python-rtmidi, to install and to talk to the FM-1: `pip3 install mido python-rtmidi`.
- Docker or podman. JieLi's toolchain runs on Linux x86-64 only, so on a Mac the build runs each
  tool in a `linux/amd64` container. With podman, put a `docker` script that runs
  `exec podman "$@"` first on your PATH. On Linux x86-64 no container is needed.
- JieLi's Linux toolchain:

  ```sh
  fw/tools/get_toolchain.sh              # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0), version 1.2.1. The build takes JieLi's libraries, lwIP's sources
  and three boot files from it. None of those are in this repository.

  ```sh
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

### Build and install

```sh
cd fw
AC79_SDK=~/fw-AC79_AIoT_SDK JIELI_TOOLCHAIN=~/.jieli/toolchain \
    /opt/homebrew/bin/python3 tools/build.py --wifi
python3 tools/fm1_install.py build/omni.fwsc --yes      # the FM-1 plugged in over USB
```

The build makes `fw/build/omni.fwsc` and checks the image and RAM sizes. It runs about a
minute; it keeps one toolchain container running, which stops after an hour idle.

To go back to M-VAVE's firmware, use the web installer on
[FoMni's site](https://charlesvestal.github.io/fm1-fomni/install/). An FM-1 that fails to start twice
starts in a safe mode with USB on, and holding OCT− and OCT+ for 5 seconds starts update mode.

### The stations

The firmware holds the stations in its data flash, but they don't come with it: they are Radio Garden's,
so you build the list on your own computer and send it over USB.

```sh
cd fw
python3 tools/rg_sync.py                               # about 30 minutes the first time
python3 tools/wifi_ctl.py rgload build/rg/stations.bin # about 30 seconds
```

`rg_sync.py` asks Radio Garden for each city's stations, follows each to its real stream and keeps
the ones the FM-1 can play: plain `http://` MP3 streams (no HTTPS, AAC or HLS). It keeps up to six
stations a city, about 3,600 in all. Everything it fetches is cached in `build/rg/`, so a second
run takes a second. `--refresh` asks again.

## Using it

### First start: join a network

The FM-1 joins 2.4 GHz networks only, open or WPA2 with a password. It can't see 5 GHz networks.

The easiest way to add a network is from the browser: open [`fw/web/wifi.html`](fw/web/wifi.html) in
Chrome or Edge with the FM-1 plugged in, press **Connect to the FM-1**, type the network's name and
password, and press **Save and join**.

On the FM-1 itself:

1. Press **GLO** for the WiFi page.
2. Press **PLAY** to turn WiFi on, then **SEL** to see the networks around you.
3. Turn **SELECT** to a network.
4. For an open network or one you've saved, press **PLAY** to join. Otherwise press **SAVE** to type
   its password.
5. Type the password on the keys and press **PLAY** to save it and join. **HOME** cancels.

The keys type left to right, every key in turn, black ones too: the first 26 are a to z, and the last
key deletes.

- Hold **OCT−** and press a key for a capital.
- Hold **OCT+** and press a key for the digits 0 to 9 and then `space ! @ # $ % & * - _ . , ? + / :`.

The screen shows a small keyboard with what each key types.

The FM-1 keeps six networks, newest first. With networks saved, it turns WiFi on at start and joins
the first one it finds. To forget one, use `python3 tools/wifi_ctl.py nets` and then `del N`.

### Listening

| Control | What it does |
|---|---|
| **PLAY** | Play or stop. |
| **KNOB 1** | Continent. |
| **KNOB 2** | Country. |
| **KNOB 3** | City. |
| **KNOB 4** or **SELECT** | Station in the city. |
| **MASTER** | Volume. |
| **SEL** | Browse the list of continents, countries, cities and stations. |
| **GLO** | The WiFi page, and back. |
| Hold **HOME** | The controls, on the screen. |

The knobs work as a dial: turn them, and the radio tunes once you stop. The screen shows the station,
its city and country, and whether it is playing or buffering. A station that drops out is reconnected
by itself.

In the browser, turn **SELECT** to move, press **SEL** to open a place or to play a station, and
press **HOME** to go up a level.

The FM-1 remembers the station and whether it was playing, and picks up there at the next start.

### Punch-in effects

While the radio plays, the 16 white keys light up. Hold a white key for its effect, and let go to
stop it. Hold several to mix them. The screen shows what is held, and a held key's light goes out.

| White key | Effect |
|---|---|
| 1 | Low-pass filter |
| 2 | High-pass filter |
| 3 | Telephone: a narrow, driven band |
| 4 | Bitcrush |
| 5 | Stutter, 1/4: repeats the last 250 ms |
| 6 | Stutter, 1/8: the last 125 ms |
| 7 | Stutter, 1/16: the last 62 ms |
| 8 | Stutter, 1/32: the last 31 ms |
| 9 | Tape stop |
| 10 | Reverse |
| 11 | Chop, 1/8 |
| 12 | Chop, 1/16 |
| 13 | Ring modulator |
| 14 | Half speed |
| 15 | Mute |
| 16 | Lo-fi |

The stutters, tape stop, reverse and half speed play back the last quarter second, in mono. Only one
of them plays at a time: with two held, the lower key wins. The filters, crushers, ring modulator,
chops and mute work on top of them. The chops run at about 120 BPM: a stream has no tempo to follow.

## Debugging over USB

`fw/tools/wifi_ctl.py` talks to the FM-1 over USB-MIDI, even while the firmware waits on the network.

| Command | What it does |
|---|---|
| `status` | WiFi state, network, address and signal. |
| `on`, `off` | WiFi on or off. |
| `nets`, `add SSID PASS`, `del N`, `join N` | The saved networks. |
| `dump` | The state, the last crash (kept through resets) and the log. |
| `clear` | Forget the crash record. |
| `radio` | As PLAY: play or stop. |
| `turn K D` | As turning knob K+1 (0 to 3) by D. |
| `fx N` | As holding white key N (1 to 16); `fx 0` lets go. |
| `rgload FILE`, `rginfo` | Send the station table; show the one the FM-1 holds. |
| `reboot` | Restart. |

## Known limits

- 2.4 GHz WiFi only, and plain-HTTP MP3 streams only.
- The core voltage is raised to 1.50 V at start (the SDK uses 1.45 V at this clock). At 1.40 V, flash
  reads failed under WiFi load and the FM-1 crashed.
- RAM is nearly full. The Bluetooth code from fm1-ble is still linked but unused.

## History

The repository began as fm1-ble, which got JieLi's BLE stack running on a small cooperative OS shim
inside a Felucca firmware, as BLE MIDI. The notes in `notes/` are from that work. The radio reuses the
shim for JieLi's WiFi libraries.

## Credits

- [Felucca](https://github.com/hugelton/Felucca) by Hügelton Instruments: the platform layer
  (GPL-3.0-only).
- [minimp3](https://github.com/lieff/minimp3) by lieff: the MP3 decoder (CC0).
- The JieLi AC79 SDK (Apache-2.0): the WiFi libraries, lwIP and the radio's tuning constants.
- [Radio Garden](https://radio.garden): the stations. Radio Garden isn't affiliated with this
  project.
- [SomaFM](https://somafm.com): the stream used while building it.

"M-VAVE" and "FM-1" are trademarks of their owners; fm1-radio is independent firmware, not affiliated
with or supported by them.
