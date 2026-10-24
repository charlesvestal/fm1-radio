#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
"""Drive Radio over USB-MIDI: F0 7D 57 <cmd> [data] F7 (app/app.c app_sysex). The networks are fm1-base's:
base/tools/fm1_remote.py wifi join SSID PASSWORD, wifi status, ...; the crash and the log: fm1_remote.py log.

  radio_ctl.py dump          the radio: the place, the station, the stream, the decoder, the main loop's rate
  radio_ctl.py radio         as PLAY on Home: the radio on / off
  radio_ctl.py play [MODE]   the stream (MODE: stream, net = fetched and dropped, local = the built-in MP3)
  radio_ctl.py stop          the stream stops
  radio_ctl.py url [URL]     the stream's address (none: read it back)
  radio_ctl.py turn K D      as turning KNOB K+1 (0..3: continent, country, place, station) by D
  radio_ctl.py fx N          as holding white key N (1..16): its punch-in FX; 0 lets go
  radio_ctl.py bpm N         the tap tempo (41..167)
  radio_ctl.py rginfo        the places table the FM-1 fetched
  radio_ctl.py places        fetch the places again (Radio Garden's list, a few minutes)

Text travels as two nibbles a byte.
"""
import sys
import time

import mido

HDR = [0x7D, 0x57]


def port():
    return next((n for n in mido.get_output_names() if "FM-1" in n or "FM1" in n), None)


def nib(b):
    return [x for c in b for x in (c >> 4, c & 15)]


def ask(cmd, data=(), want=None, timeout=20.0):
    """send; want: the reply codes to collect until 0x11 (or the first one, for a single reply)"""
    name = port()
    if not name:
        return None
    got = []
    try:
        with mido.open_input(name) as i, mido.open_output(name) as o:
            while i.poll():
                pass
            o.send(mido.Message("sysex", data=HDR + [cmd] + list(data)))
            if not want:
                return []
            end = time.time() + timeout
            while time.time() < end:
                m = i.poll()
                if m is None:
                    time.sleep(0.002)
                    continue
                if m.type != "sysex" or list(m.data[:2]) != HDR:
                    continue
                code, body = m.data[2], list(m.data[3:])
                if code == 0x11:
                    return got
                if code in want:
                    got.append((code, body))
                    if 0x11 not in want:
                        return got
    except (OSError, IOError):
        return None
    return got


def text(body):
    return bytes(body).decode("ascii", "replace")


def main():
    a = sys.argv[1:] or ["dump"]
    c = a[0]
    if c == "dump":
        r = ask(0x10, want=(0x10, 0x11), timeout=10)
        if r is None:
            sys.exit("radio_ctl: no FM-1 MIDI port")
        print("\n".join(text(b) for _, b in r))
    elif c == "url":
        r = ask(0x43, nib(a[1].encode()) if len(a) > 1 else [], want=(0x10, 0x11), timeout=5)
        print("\n".join(text(b) for _, b in r) if r else "sent")
    elif c == "rginfo":
        r = ask(0x53, want=(0x10, 0x11), timeout=3)     # (every line: the counts, the size)
        print("\n".join(text(b) for _, b in r) if r else "no answer")
    elif c == "places":
        ask(0x54)
        print("sent: the places are fetched again (Home shows the time left)")
    elif c == "radio":
        ask(0x45)
        print("sent")
    elif c == "play":
        ask(0x40, [{"stream": 0, "net": 1, "local": 2}[a[1] if len(a) > 1 else "stream"]])
        print("sent")
    elif c == "stop":
        ask(0x41)
        print("sent")
    elif c == "bpm" and len(a) == 2:             # the tap tempo, 41..167 BPM
        ask(0x48, [max(1, min(127, int(a[1]) - 40))])
    elif c == "fx" and len(a) == 2:              # hold punch-in FX N (1..16), 0 = let go
        ask(0x47, [int(a[1]) & 0x7F])
    elif c == "turn" and len(a) == 3:
        ask(0x46, [int(a[1]) & 3, int(a[2]) & 0x7F])
        print("sent")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
