#!/usr/bin/env python3
"""Build the FM-1's Radio Garden places table ("RGP1"): every place Radio Garden has, by continent and
country, names only. The stations are not in it: the FM-1 asks Radio Garden for a place's stations when
you open it (radio_net.c), finding the place by name with Radio Garden's search.

    python3 tools/rg_places.py            # fetches the places once (1.9 MB, cached in build/rg/places.json)
    python3 tools/rg_places.py --refresh  # fetches them again

tools/build.py --wifi runs it with --c-out build/gen/rg_places_data.h: the table goes into the firmware
image, read in place (firmware/src/app/rg.c). Without --c-out it writes build/rg/places.bin.

Layout (little-endian):
  header, 64 bytes: magic "RGP1", version u16, continents u16, countries u16, places u16 (total),
      offsets of the continents, countries, strings and blocks (u32 each), total length, CRC-32 of
      [64, total), created (unix time), padding
  continent, 8 bytes: name (string offset) u32, first country u16, countries u16
  country, 16 bytes: name u32, block offset u32, block length u32, places u16, continent u8, 0
  strings: NUL-terminated ASCII
  blocks: a country's place names, sorted without case, front coded (a byte: how many leading characters
      it shares with the name before, then the rest, then NUL) and compressed with raw deflate in a
      1 KB window (zlib wbits -10), so the FM-1 unpacks a country at a time in little RAM.
Standard library only.
"""
import argparse
import json
import os
import struct
import time
import unicodedata
import urllib.request
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLACES_URL = 'https://radio.garden/api/ara/content/places'
UA = 'Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36'
MAX_TABLE = 0x44000                     # the table's flash area (rg.c RG_MAX)
NAME_MAX = 31                           # the FM-1 keeps names in 32 bytes

# Radio Garden's country names, by continent (the continents' names as schwung-radiogarden's browser.js)
CONTINENT = {
    'Africa': '''Algeria Angola Benin Botswana Burkina_Faso Burundi Cameroon Cape_Verde Central_African_Republic
        Chad Comoros Democratic_Republic_of_the_Congo Djibouti Egypt Equatorial_Guinea Eritrea Eswatini Ethiopia
        Gabon Ghana Guinea Guiné-Bissau Côte_d'Ivoire Kenya Lesotho Liberia Libya Madagascar Malawi Mali
        Mauritania Mauritius Mayotte Morocco Mozambique Namibia Niger Nigeria Republic_of_the_Congo Réunion
        Rwanda Saint_Helena São_Tomé_and_Príncipe Senegal Seychelles Sierra_Leone Somalia South_Africa
        South_Sudan Sudan Tanzania The_Gambia Togo Tunisia Uganda Western_Sahara Zambia Zimbabwe''',
    'Asia': '''Afghanistan Armenia Azerbaijan Bahrain Bangladesh Bhutan Brunei Cambodia China Cyprus Georgia
        Hong_Kong India Indonesia Iran Iraq Israel Japan Jordan Kazakhstan Kuwait Kyrgyzstan Laos Lebanon Macau
        Malaysia Maldives Mongolia Myanmar_(Burma) Nepal North_Korea Oman Pakistan Palestine Philippines Qatar
        Saudi_Arabia Singapore South_Korea Sri_Lanka Syria Taiwan Tajikistan Thailand Timor-Leste Türkiye
        Turkmenistan United_Arab_Emirates Uzbekistan Vietnam Yemen''',
    'Europe': '''Åland_Islands Albania Andorra Austria Azores Bailiwick_of_Guernsey Bailiwick_of_Jersey Belarus
        Belgium Bosnia_and_Herzegovina Bulgaria Croatia Czechia Denmark Estonia Faroe_Islands Finland France
        Germany Gibraltar Greece Hungary Iceland Ireland Isle_of_Man Italy Kosovo Latvia Liechtenstein Lithuania
        Luxembourg Madeira Malta Moldova Monaco Montenegro Netherlands North_Macedonia Norway Poland Portugal
        Romania Russia San_Marino Serbia Slovakia Slovenia Spain Sweden Switzerland Ukraine United_Kingdom
        Vatican_City''',
    'N. America': '''American_Samoa Anguilla Antigua_and_Barbuda Aruba Barbados Belize Bermuda Bonaire
        British_Virgin_Islands Canada Cayman_Islands Collectivity_of_Saint_Martin Costa_Rica Cuba Curaçao
        Dominica Dominican_Republic El_Salvador Greenland Grenada Guadeloupe Guatemala Haiti Honduras Jamaica
        Martinique Mexico Montserrat Nicaragua Panama Puerto_Rico Saint_Barthélemy Saint_Kitts_and_Nevis
        Saint_Lucia Saint-Pierre_et_Miquelon Saint_Vincent_and_the_Grenadines Sint_Maarten The_Bahamas
        Trinidad_and_Tobago Turks_and_Caicos_Islands U.S._Virgin_Islands United_States''',
    'Oceania': '''Australia Cook_Islands Federated_States_of_Micronesia Fiji Guam Kiribati Marshall_Islands Nauru
        New_Calédonia New_Zealand Niue Northern_Mariana_Islands Palau Papua_New_Guinea Samoa Solomon_Islands
        Tahiti Tonga Tuvalu Vanuatu Wallis_and_Futuna''',
    'S. America': '''Argentina Bolivia Brazil Chile Colombia Ecuador Falkland_Islands French_Guiana Guyana Paraguay
        Peru Suriname Uruguay Venezuela''',
}
COUNTRY_CONTINENT = {c.replace('_', ' '): k for k, v in CONTINENT.items() for c in v.split()}


def ascii_name(s):
    s = unicodedata.normalize('NFKD', s or '')
    s = ''.join(c for c in s if not unicodedata.combining(c) and 32 <= ord(c) <= 126)
    return ' '.join(s.split())[:NAME_MAX].rstrip()


def fetch_places(path, refresh):
    if not refresh and os.path.exists(path):
        with open(path) as f:
            return json.load(f)['data']['list']
    req = urllib.request.Request(PLACES_URL, headers={'User-Agent': UA, 'Referer': 'https://radio.garden/',
                                                      'Accept': 'application/json'})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    return json.loads(data)['data']['list']


def front_code(names):
    out, prev = bytearray(), ''
    for n in names:
        k = 0
        while k < len(n) and k < len(prev) and k < 255 and n[k] == prev[k]:
            k += 1
        out += bytes([k]) + n[k:].encode('ascii') + b'\0'
        prev = n
    return bytes(out)


def deflate(b):
    c = zlib.compressobj(9, zlib.DEFLATED, -10, 9)
    return c.compress(b) + c.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cache', default=os.path.join(HERE, 'build/rg/places.json'))
    ap.add_argument('--out', default=os.path.join(HERE, 'build/rg/places.bin'))
    ap.add_argument('--refresh', action='store_true', help='fetch the places again')
    ap.add_argument('--c-out', help='write the table as a C header (the firmware build) instead')
    args = ap.parse_args()

    places = fetch_places(args.cache, args.refresh)
    created = int(os.path.getmtime(args.cache))   # when the places were fetched
    by = {}
    for p in places:
        name = ascii_name(p.get('title'))
        if name:
            by.setdefault(p['country'], set()).add(name)
    unknown = sorted(c for c in by if c not in COUNTRY_CONTINENT)
    if unknown:
        raise SystemExit('rg_places: countries with no continent (add them to CONTINENT): %s' % unknown)

    tree = {}
    for country, names in by.items():
        tree.setdefault(COUNTRY_CONTINENT[country], []).append(country)
    conts = sorted(tree)
    strings, str_off = bytearray(), {}

    def s(x):
        x = ascii_name(x) or '?'
        if x not in str_off:
            str_off[x] = len(strings)
            strings.extend(x.encode('ascii') + b'\0')
        return str_off[x]

    cont_rows, country_rows, blocks = [], [], bytearray()
    total_places = 0
    for ci, cont in enumerate(conts):
        countries = sorted(tree[cont], key=lambda c: ascii_name(c).lower())
        cont_rows.append((s(cont), len(country_rows), len(countries)))
        for country in countries:
            names = sorted(by[country], key=lambda n: (n.lower(), n))
            blk = deflate(front_code(names))
            country_rows.append((s(country), len(blocks), len(blk), len(names), ci))
            blocks += blk
            total_places += len(names)

    off_cont = 64
    off_country = off_cont + 8 * len(cont_rows)
    off_str = off_country + 16 * len(country_rows)
    off_blocks = (off_str + len(strings) + 3) & ~3
    body = bytearray()
    body += b''.join(struct.pack('<IHH', *r) for r in cont_rows)
    body += b''.join(struct.pack('<IIIHBB', n, off_blocks + o, ln, cnt, ci, 0) for n, o, ln, cnt, ci in country_rows)
    body += strings
    body += bytes(off_blocks - 64 - len(body))
    body += blocks
    total = 64 + len(body)
    if total > MAX_TABLE:
        raise SystemExit('rg_places: %d bytes, more than the table area (%d)' % (total, MAX_TABLE))
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    hdr = struct.pack('<4sHHHHIIIIIII', b'RGP1', 1, len(cont_rows), len(country_rows), total_places & 0xFFFF,
                      off_cont, off_country, off_str, off_blocks, total, crc, created)
    hdr += bytes(64 - len(hdr))
    blob = hdr + body
    if args.c_out:
        rows = ['    ' + ', '.join('0x%02x' % x for x in blob[i:i + 24]) + ',' for i in range(0, len(blob), 24)]
        text = ('/* generated by tools/rg_places.py from Radio Garden\'s places: do not edit */\n'
                'static const uint8_t RG_PLACES[%d] __attribute__((aligned(4))) = {\n%s\n};\n'
                % (len(blob), '\n'.join(rows)))
        os.makedirs(os.path.dirname(os.path.abspath(args.c_out)), exist_ok=True)
        old = open(args.c_out).read() if os.path.exists(args.c_out) else None
        if old != text:                           # unchanged: left alone (no rebuild needed)
            with open(args.c_out, 'w') as f:
                f.write(text)
        args.out = args.c_out
    else:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, 'wb') as f:
            f.write(blob)
    biggest = max(country_rows, key=lambda r: r[3])
    print('%d continents, %d countries, %d places; %d bytes (names %d B compressed) -> %s' %
          (len(cont_rows), len(country_rows), total_places, total, len(blocks), args.out))
    print('the most places in a country: %d' % biggest[3])


if __name__ == '__main__':
    main()
