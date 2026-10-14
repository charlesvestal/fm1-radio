#!/usr/bin/env python3
"""A screenshot of the FM-1's screen over USB-MIDI (the radio firmware, tools/build.py --wifi).

    screenshot.py OUT.png [PAGE]      PAGE: home, browse, wifi, help (default: the page on screen)

The FM-1 redraws the page whole and sends every fill and strip as it draws them (main_wifi.c shot_tap):
F0 7D 57 63 x y w h colour F7 for a fill, F0 7D 57 61 x y w <row> F7 for one row of a blit, the row's
RGB565 bytes (high byte first) packed 7 in 8, and F0 7D 57 62 "end" F7 at the end. Needs mido with
python-rtmidi, and Pillow.
"""
import sys
import time

import mido
from PIL import Image

HDR = [0x7D, 0x57]
PAGES = {"home": 1, "browse": 2, "wifi": 3, "help": 4}


def port():
    return next((n for n in mido.get_output_names() if "FM-1" in n or "FM1" in n), None)


def v14(d, i):
    return d[i] | d[i + 1] << 7


def unpack7(d):
    out = bytearray()
    i = 0
    while i < len(d):
        hi, chunk = d[i], d[i + 1:i + 8]
        out += bytes(b | ((hi >> k) & 1) << 7 for k, b in enumerate(chunk))
        i += 8
    return out


def rgb(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def main():
    if len(sys.argv) < 2 or (len(sys.argv) > 2 and sys.argv[2] not in PAGES):
        sys.exit(__doc__)
    page = PAGES.get(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    name = port()
    if not name:
        sys.exit("screenshot: no FM-1 MIDI port")
    img = Image.new("RGB", (240, 240))
    px = img.load()
    with mido.open_input(name) as i, mido.open_output(name) as o:
        while i.poll():
            pass
        o.send(mido.Message("sysex", data=HDR + [0x60, page]))
        end = time.time() + 20
        rows = 0
        while time.time() < end:
            m = i.poll()
            if m is None:
                time.sleep(0.001)
                continue
            if m.type != "sysex" or list(m.data[:2]) != HDR:
                continue
            d, cmd = list(m.data[3:]), m.data[2]
            if cmd == 0x62:
                break
            if cmd == 0x63:                      # a fill
                x, y, w, h = v14(d, 0), v14(d, 2), v14(d, 4), v14(d, 6)
                c = rgb(d[8] | d[9] << 7 | d[10] << 14)
                for yy in range(y, min(240, y + h)):
                    for xx in range(x, min(240, x + w)):
                        px[xx, yy] = c
            elif cmd == 0x61:                    # one row of a blit
                x, y, w = v14(d, 0), v14(d, 2), v14(d, 4)
                b = unpack7(d[6:])
                for k in range(min(w, len(b) // 2)):
                    if x + k < 240 and y < 240:
                        px[x + k, y] = rgb(b[2 * k] << 8 | b[2 * k + 1])
                rows += 1
        else:
            sys.exit("screenshot: no end from the FM-1 (is it running the radio?)")
    img.save(sys.argv[1])
    print(f"{sys.argv[1]}: {rows} rows")


if __name__ == "__main__":
    main()
