# fm1-radio licensing

fm1-radio is free software under the **Mozilla Public License 2.0** (`MPL-2.0`, full text in `LICENSE`), file by
file. If you change one of its files and share the result, share that file's source under the MPL. It is built on
[fm1-base](https://github.com/charlesvestal/fm1-base) and [felucca-bsp](https://github.com/hugelton/felucca-bsp)
(`base/`, a git submodule), both MPL-2.0.

The MPL works file by file, so a firmware made of MPL files may also contain JieLi's WiFi libraries, which are
Apache-2.0 and come as binaries only. Neither they nor lwIP's sources are part of this repository: the build copies
them from each user's own AC79 SDK.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| The platform: `base/` (boot, USB, storage, the screen, the panel, WiFi on JieLi's libraries and the shim they run on, the host simulator, build and install tools) | [fm1-base](https://github.com/charlesvestal/fm1-base) and [felucca-bsp](https://github.com/hugelton/felucca-bsp), from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (@kurogedelic), Hügelton Instruments, under MPL-2.0 with his permission ([felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4)) | MPL-2.0 (the parts of `base/ble/` from JieLi's SDK: Apache-2.0) |
| `app/` (`app.c`, `app.h`, `radio.c`, `radio_ui.c`, `rg.c`, `inflate.c`, `net/radio_net.c`), `app/tools/` | fm1-radio, Charles Vestal | MPL-2.0 |
| `app/third_party/minimp3.h` | [minimp3](https://github.com/lieff/minimp3) by lieff | CC0 1.0 (public domain) |
| `app/third_party/pvaac/` (AAC; the `.cpp` files renamed `.c` and built as C with `pv_fm1.h`, one line changed, marked "fm1-radio"; `README.fm1`) | PacketVideo's AAC decoder, from [Android 4.1.2](https://android.googlesource.com/platform/frameworks/av/+/android-4.1.2_r1/media/libstagefright/codecs/aacdec) | Apache-2.0 (`NOTICE`) |
| `app/third_party/bearssl/` (unchanged; `VERSION` names the commit) | [BearSSL](https://www.bearssl.org) by Thomas Pornin | MIT (`LICENSE.txt`) |
| `app/third_party/compat/` (two small headers for minimp3) | fm1-radio | MPL-2.0 |
| The places and the stations' API calls (`app/tools/rg_places.py`, `app/net/radio_net.c`) | follow schwung-radiogarden's browser; no code copied | credit only |

## Read from your SDK at build time, not in this tree

| What | Licence |
| --- | --- |
| JieLi's libraries: `crypto_toolbox_Osize.a`, `lib_ccm_aes.a`, `wl_rf_common.a`, `wl_wifi_sta_sfc.a`, `wpasupplicant.a`, `hsm.a` (binary only) | Apache-2.0, from <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |
| lwIP 2.1.3's sources, as shipped in the SDK | BSD-3-Clause (lwIP), in the Apache-2.0 SDK |
| `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin`, placed in the package | Apache-2.0 |

## Place and station data

The FM-1 fetches Radio Garden's list of places itself, once, into its own flash (`app/places.c`), and a place's
stations when it is chosen. No place or station data is in the firmware or in this repository
(`app/tools/rg_places.py` builds a table only for the host simulator). The streams belong to their stations.

## Trademarks

Radio Garden is a name of its owners. "Felucca" and "Hügelton Instruments" are names of Hügelton Instruments.
"M-VAVE" and "FM-1" are trademarks of their respective owners. fm1-radio is independent firmware; it is not
affiliated with, endorsed by or supported by any of them.
