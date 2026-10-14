#!/usr/bin/env python3
"""Drive the WiFi firmware (tools/build.py --wifi) over USB-MIDI: F0 7D 57 <cmd> [data] F7.

  wifi_ctl.py status         WiFi state, network, address, signal
  wifi_ctl.py on | off       WiFi on (joins the saved networks) / off
  wifi_ctl.py nets           the saved networks
  wifi_ctl.py add SSID PASS  save a network (to the front) and join it if WiFi is on
  wifi_ctl.py del N          forget saved network N
  wifi_ctl.py join N         join saved network N
  wifi_ctl.py play | stop   the radio (the stream in firmware/src/app/radio.c)
  wifi_ctl.py ble            start BLE MIDI
  wifi_ctl.py scan           start a scan; scanlog: put the results in the log
  wifi_ctl.py dump           the crash record (kept through resets), the log, the tasks
  wifi_ctl.py clear          forget the crash record
  wifi_ctl.py rginfo        the places table built into the FM-1's firmware
  wifi_ctl.py radio         as PLAY on Home: the radio on / off
  wifi_ctl.py bpm N         the tap tempo (41..167)
  wifi_ctl.py fx N          as holding white key N (1..16): its punch-in FX; 0 lets go
  wifi_ctl.py turn K D      as turning KNOB K+1 (0..3: continent, country, city, station) by D
  wifi_ctl.py reboot         restart (answered even while the main loop waits in the library)

Text travels as two nibbles a byte (an SSID may hold any byte, as the iPhone's U+2019).
"""
import sys
import time

import mido

HDR = [0x7D, 0x57]


def port():
    return next((n for n in mido.get_output_names() if "FM-1" in n or "FM1" in n), None)


def nib(b):
    return [x for c in b for x in (c >> 4, c & 15)]


def unnib(d):
    return bytes((d[i] << 4) | d[i + 1] for i in range(0, len(d) - 1, 2))


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


def pack7(b):
    """8 wire bytes carry 7: a byte of the high bits, then the 7 bytes' low bits"""
    out = []
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << j for j, x in enumerate(g)))
        out += [x & 0x7F for x in g]
    return out


def put7(v, k):
    return [(v >> (7 * i)) & 0x7F for i in range(k)]


def text(body):
    return bytes(body).decode("ascii", "replace")


def main():
    a = sys.argv[1:] or ["status"]
    c = a[0]
    if c == "status":
        r = ask(0x24, want=(0x24,), timeout=3)
        if not r:
            sys.exit("wifi_ctl: no answer")
        st, ssid, ip, rssi, why = unnib(r[0][1]).decode("utf-8", "replace").split("|")
        print(f"{st}  {ssid}  {ip}  {rssi} dBm  {why}".rstrip())
    elif c == "nets":
        r = ask(0x21, want=(0x21, 0x11), timeout=3) or []
        for _, body in r:
            print(body[0], unnib(body[1:]).decode("utf-8", "replace"))
        if not r:
            print("(none)")
    elif c == "add" and len(a) == 3:
        r = ask(0x20, nib(a[1].encode() + b"\0" + a[2].encode() + b"\0"), want=(0x10,), timeout=5)
        print(text(r[0][1]) if r else "sent")
    elif c in ("del", "join") and len(a) == 2:
        r = ask(0x22 if c == "del" else 0x23, [int(a[1])], want=(0x10,) if c == "del" else None, timeout=5)
        print(text(r[0][1]) if r else "sent")
    elif c == "dump":
        r = ask(0x10, want=(0x10, 0x11), timeout=60)
        if r is None:
            sys.exit("wifi_ctl: no FM-1 MIDI port")
        print("\n".join(text(b) for _, b in r))
    elif c == "clear":
        r = ask(0x12, want=(0x10,), timeout=3)
        print(text(r[0][1]) if r else "sent")
    elif c == "reboot":
        r = ask(0x7F, want=(0x10,), timeout=3)
        print(text(r[0][1]) if r else "sent")
    elif c == "xosc" and len(a) == 3:
        r = ask(0x31, [int(a[1], 0), int(a[2], 0)], want=(0x10,), timeout=3)
        print(text(r[0][1]) if r else "sent")
    elif c == "txpwr" and len(a) == 2:
        r = ask(0x42, [int(a[1])], want=(0x10,), timeout=5)
        print(text(r[0][1]) if r else "sent")
    elif c == "url" and len(a) == 2:
        r = ask(0x43, nib(a[1].encode()), want=(0x10,), timeout=5)
        print(text(r[0][1]) if r else "sent")
    elif c == "vdc14" and len(a) == 2:
        r = ask(0x44, [int(a[1])], want=(0x10,), timeout=5)
        print(text(r[0][1]) if r else "sent")
    elif c == "rginfo":
        r = ask(0x53, want=(0x10,), timeout=5)
        print(text(r[0][1]) if r else "no answer")
    elif c == "radio":
        ask(0x45)
        print("sent")
    elif c == "bpm" and len(a) == 2:             # the tap tempo, 41..167 BPM
        ask(0x48, [max(1, min(127, int(a[1]) - 40))])
    elif c == "fx" and len(a) == 2:              # hold punch-in FX N (1..16), 0 = let go
        ask(0x47, [int(a[1]) & 0x7F])
    elif c == "turn" and len(a) == 3:
        d = int(a[2])
        ask(0x46, [int(a[1]) & 3, d & 0x7F])
        print("sent")
    elif c == "score":
        r = ask(0x32, want=(0x10,), timeout=3)
        print(text(r[0][1]) if r else "no answer")
    elif c == "leds" and len(a) == 2:
        ask(0x30, [0 if a[1] == "on" else 1])
        print("sent")
    elif c == "play" and len(a) == 2:
        ask(0x40, [{"stream": 0, "net": 1, "local": 2}[a[1]]])
        print("sent")
    elif c in ("on", "off", "ble", "scan", "scanlog", "play", "stop"):
        ask({"on": 1, "off": 5, "ble": 2, "scan": 3, "scanlog": 4, "play": 0x40, "stop": 0x41}[c])
        print("sent")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
