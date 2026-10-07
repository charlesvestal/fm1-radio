#!/usr/bin/env python3
"""Extract a GNU ar archive (long names in //) to a directory: tools_arx.py LIB.a OUTDIR"""
import sys, os
d = open(sys.argv[1], 'rb').read(); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
assert d[:8] == b'!<arch>\n'
i, longnames, n = 8, b'', 0
while i < len(d):
    h = d[i:i + 60]; name = h[:16].decode().strip(); size = int(h[48:58]); body = d[i + 60:i + 60 + size]
    i += 60 + size + (size & 1)
    if name == '/': continue
    if name == '//': longnames = body; continue
    if name.startswith('/') and name[1:].isdigit():
        o = int(name[1:]); name = longnames[o:longnames.index(b'/\n', o)].decode()
    name = name.rstrip('/')
    k = 0
    while os.path.exists(os.path.join(out, name if not k else f"{k}_{name}")): k += 1
    open(os.path.join(out, name if not k else f"{k}_{name}"), 'wb').write(body); n += 1
print(n, "members")
