# fm1-radio: an internet radio for the FM-1

fm1-radio turns the M-VAVE FM-1 into a pocket internet radio. It joins a 2.4 GHz WiFi network, plays
stations from around the world from Radio Garden's map, and lets you play the radio with punch-in
effects on the keys.

It is a proof of concept: it works, and the rough edges are listed at the end.

![Home: a station in Amsterdam, with the four knobs](docs/img/home.png) ![The browser: the city's stations](docs/img/browse.png)

## Licensing status

**Source only, no binaries.** The source is free software under GPL-3.0-only, the licence of
[Felucca](https://github.com/hugelton/Felucca), whose platform layer it is built on. A built firmware
links JieLi's closed WiFi libraries from the AC79 SDK. The GPL does not allow
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
python3 tools/fm1_install.py build/radio.fwsc --yes      # the FM-1 plugged in over USB
```

The build makes `fw/build/radio.fwsc` and checks the image and RAM sizes. It runs about a
minute; it keeps one toolchain container running, which stops after an hour idle.

To go back to M-VAVE's firmware, use the web installer on
[FoMni's site](https://charlesvestal.github.io/fm1-fomni/install/). An FM-1 that fails to start twice
starts in a safe mode with USB on, and holding OCT− and OCT+ for 5 seconds starts update mode.

### The places

The build packs Radio Garden's list of places into the firmware: 12,844 places in 227 countries, names
only, compressed a country at a time (78 KB). It fetches the list once, the first time you build, and
keeps it in `fw/build/rg/places.json`; `python3 tools/rg_places.py --refresh` fetches it again. The
places are Radio Garden's, so they're never committed here.

The stations aren't in the firmware. When you choose a place, the FM-1 asks Radio Garden for its stations,
and plays a station through Radio Garden's link to its stream.

## Using it

### First start: join a network

The FM-1 joins 2.4 GHz networks only, open or WPA2 with a password. It can't see 5 GHz networks.

With no network saved, the FM-1 starts on the list of networks around it, already scanning: turn
**SELECT** to yours and press **PLAY**, then type its password (steps 4 and 5 below).

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
the first one it finds. To forget one, press **Forget** next to it on `wifi.html`, or use
`python3 tools/wifi_ctl.py nets` and then `del N`.

### Listening

| Control | What it does |
|---|---|
| **PLAY** | Play or stop. |
| **KNOB 1** | Continent. |
| **KNOB 2** | Country. |
| **KNOB 3** | Place: a city or town, A to Z. |
| **KNOB 4** or **SELECT** | Station in the place. |
| **MASTER** | Volume. |
| **FX** | Tap tempo for the effects. |
| **SEL** | Browse the list of continents, countries, places and stations. |
| **GLO** | The WiFi page, and back. |
| Hold **HOME** | The controls, on the screen. |

The knobs work as a dial: turn them, and the radio tunes once you stop. A new place takes a second or
two: the FM-1 asks Radio Garden for its stations (up to 100, as Radio Garden orders them) and plays the
first. The screen shows the station, its place and country, and whether it is playing or buffering. A
station that drops out is reconnected by itself.

Some stations can't play. About a third of Radio Garden's stations send AAC, which the FM-1 can't decode,
and some are offline. The screen says so ("can't play AAC", "station offline"): turn **KNOB 4** to the
next one.

![The controls, shown while HOME is held](docs/img/help.png)

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
| 5 | Stutter, 1/8: repeats the last eighth note |
| 6 | Stutter, 1/16 |
| 7 | Stutter, 1/32 |
| 8 | Stutter, 1/64 |
| 9 | Tape stop |
| 10 | Reverse |
| 11 | Chop, 1/8: open for the first half of each eighth note, from your press |
| 12 | Chop, 1/16 |
| 13 | Ring modulator |
| 14 | Half speed |
| 15 | Mute |
| 16 | Lo-fi |

![Home with stutter 1/4 held](docs/img/fx.png)

The stutters, tape stop, reverse and half speed play back the last quarter second, in mono. Only one
of them plays at a time: with two held, the lower key wins. The filters, crushers, ring modulator,
chops and mute work on top of them.

**Tap tempo.** The stutters and chops follow a tempo, 120 BPM at start. Tap **FX** on Home in time
with the music to set it: the last four taps count, and a pause of two seconds starts a new count. The
screen shows the tempo for two seconds, and the FX light flashes on the beat. The history holds about a
quarter second, so below about 108 BPM the 1/8 stutter plays a 1/16 instead.

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
| `bpm N` | Set the tap tempo (41 to 167). |
| `url URL` | Play the stream at URL, `http://` or `https://`, until the next tune. |

`fw/tools/screenshot.py OUT.png [home|browse|wifi|help]` saves the screen as a PNG: the FM-1 redraws
the page and sends it over USB as it draws, which pauses the audio for about a second. The pictures
here were taken that way.
| `rginfo` | The places built into the firmware. |
| `reboot` | Restart. |

## HTTPS and certificates

The FM-1 plays `https://` MP3 streams as well as plain `http://` ones, and asks Radio Garden (HTTPS
only) for each place's stations, with
[BearSSL](https://www.bearssl.org)'s TLS client:

- TLS 1.2;
- ECDHE key exchange on P-256;
- AES-GCM;
- RSA or ECDSA server keys.

That covers what streaming servers offer.

**It does not check certificates.** The FM-1 has no root certificates and no clock, so it uses the
server's key as sent and doesn't verify who signed it. The connection is encrypted, but someone on
your network who wanted to could pose as the station or as Radio Garden, and send you other audio or
another list. For a radio this is a small risk. Checking certificates would take BearSSL's checker, a
set of root certificates and the date from network time, about 35–45 KB of flash, more than the 22 KB
now free.

## Known limits

- 2.4 GHz WiFi only. MP3 streams only: no AAC or HLS, about a third of Radio Garden's stations. An AAC
  decoder would need more flash and RAM than are left.
- A place shows up to 100 stations.
- Server certificates aren't checked (see above).
- The core voltage is raised to 1.50 V at start (the SDK uses 1.45 V at this clock). At 1.40 V, flash
  reads failed under WiFi load and the FM-1 crashed.
- The MP3 decoder has crashed twice, both times around a change of station, and the FM-1 restarted.
  It hasn't happened on demand since; the crash record over USB (`dump`) keeps the next one.
- RAM is full (see below).

## Where the space goes

The FM-1 has no external RAM, so WiFi has to fit in the chip's own memory beside the radio. The
figures are from the build of October 2026, rounded, measured from the linked image's symbols. FoMni,
a synth with no radio, is about 158 KB in all for comparison.

### Flash: the 581 KB app slot

The image is 560 KB, which leaves about 22 KB free.

| Part | Size |
|---|---|
| JieLi's WiFi driver (`wl_wifi_sta_sfc`) | 146 KB |
| WPA supplicant | 77 KB |
| lwIP (TCP/IP, built from the SDK's source) | 51 KB |
| JieLi's RF layer and crypto, the shim and its WiFi glue | 25 KB |
| **WiFi in all** | **about 299 KB** |
| HTTPS: BearSSL's code (22 KB), its tables and the TLS glue | 36 KB |
| Radio Garden's places (12,844 names, compressed by country) | 78 KB |
| The radio, minimp3, UI, fonts, platform, the Radio Garden client and its decompressor | about 105 KB |

### RAM: 128 KB of static data

`.data` and `.bss` use 125 KB of the chip's 128 KB of RAM, leaving about 3 KB.

| Part | Size |
|---|---|
| WiFi driver: its adapter (32 KB), transmit (20 KB) and receive (4 KB) buffers, and the rest | 61 KB |
| RF layer | 3 KB |
| lwIP | 6 KB |
| The shim's task table: 14 tasks, each with a 2 KB message queue | 30 KB |
| **WiFi in all** | **about 100 KB** |
| The decoder's window for a stream that wraps round its buffer | 6 KB |
| The platform and the radio: audio buffers, storage, USB, screen state | about 22 KB |

### The pool: 304 KB of RAM for big buffers

The pool has about 3 KB free.

| Part | Size |
|---|---|
| Task stacks: 13 of 8 KB, for the WiFi, network and timer tasks | 104 KB |
| The heap | 144 KB |
| lwIP's packet buffers | 28 KB |
| The screen's drawing buffer (240 × 48 pixels: rows are drawn 48 lines at a time) | 23 KB |

The stream task uses 6 KB of its 8 KB stack during a TLS handshake, and about 2 KB otherwise.

A separate 15 KB area keeps the crash record and the debug log through a restart. It also holds the
TLS client's state (4.5 KB).

While a station plays, the radio takes about 57 KB of the heap:

- the stream buffer (20 KB);
- the decoded audio (14 KB);
- the effects' history (12 KB);
- the decoder (11 KB).

WiFi, WPA and lwIP take about 60 KB. An HTTPS stream adds its 16.7 KB record buffer. The heap peaks at
about 138 of 144 KB on HTTPS, leaving about 9 KB of slack.

### What this means

- **WiFi is the bulk of the firmware:** about 300 KB of flash and, with its task stacks and heap, about
  three quarters of the RAM. Getting there took the SDK's `_sfc` WiFi libraries (smaller buffers
  than the default ones), a narrower screen buffer and a heap that takes nearly all of the pool.
- **Flash is nearly full again:** about 22 KB, after BLE came out and the places went in. The data
  flash area the old station table used (272 KB at 0x98000) is free, so the places could move there if
  the code needs room.
- **RAM has little:** new buffers need room found first. The candidates are fewer task stacks (most
  tasks use under 4 KB of their 8) and a smaller stream buffer on HTTPS streams.

## History

The repository began as fm1-ble, which got JieLi's BLE stack running on a small cooperative OS shim
inside a Felucca firmware, as BLE MIDI. The notes in `notes/` are from that work. The radio reuses the
shim for JieLi's WiFi libraries. The BLE stack itself was taken out of the radio's build to make room;
`fw/firmware/src/app/ble.c` keeps it behind `SHIM_NO_BLE`, and fm1-ble has the working BLE MIDI.

## Credits

- [Felucca](https://github.com/hugelton/Felucca) by Hügelton Instruments: the platform layer
  (GPL-3.0-only).
- [minimp3](https://github.com/lieff/minimp3) by lieff: the MP3 decoder (CC0).
- [BearSSL](https://www.bearssl.org) by Thomas Pornin: the TLS client (MIT).
- The JieLi AC79 SDK (Apache-2.0): the WiFi libraries, lwIP and the radio's tuning constants.
- [Radio Garden](https://radio.garden): the stations. Radio Garden isn't affiliated with this
  project.
- [SomaFM](https://somafm.com): the stream used while building it.

"M-VAVE" and "FM-1" are trademarks of their owners; fm1-radio is independent firmware, not affiliated
with or supported by them.
