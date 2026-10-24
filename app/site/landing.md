# Radio

Radio turns the M-VAVE FM-1 into a pocket internet radio. It joins a 2.4 GHz WiFi network, plays stations from around
the world from Radio Garden's map, MP3 and AAC (HE-AAC included), and lets you play the radio with punch-in effects on
the keys.

![Home: a station in Amsterdam, with the four knobs](img/home.png "Radio on the FM-1's screen") ![The browser: the city's stations](img/browse.png)

Version {{VERSION}}. Installing is at your own risk; the installer can put M-VAVE's own firmware back.

- [Install](install/): from Chrome or Edge, with the FM-1 connected by USB.
- [Try it in the browser](emu/): the stations play, with the effects on the keys, through Radio's own decoders.
- [Manual](manual/): joining a network, the knobs, favorites, the effects, the synth, recording over USB.
- [Download {{PKG}}](firmware/{{PKG}}): for the command-line installer, `python3 base/tools/fm1_install.py {{PKG}}`.
- [Source](https://github.com/charlesvestal/fm1-radio): MPL-2.0. Built on fm1-base and felucca-bsp, from Felucca by
  Leo Kuroshita, Hügelton Instruments.

Radio Garden isn't affiliated with Radio. M-VAVE and FM-1 are trademarks of their owners. Radio is not affiliated
with M-VAVE.
