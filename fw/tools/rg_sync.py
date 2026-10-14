#!/usr/bin/env python3
"""Build a compact Radio Garden station table ("RGT1") for the FM-1's internet radio.

The device plays plain-HTTP MP3 streams only (no TLS, no AAC, no HLS), so every
Radio Garden station is resolved to its real stream URL and probed; only
http:// + MP3 survive. The city list comes from schwung-radiogarden's
browser.js (CITIES), and the API calls copy that module's (placeIdFrom,
stationsFrom).

Standard library only. Every API answer and probe result is cached
(--cache, keyed by URL), so reruns are cheap and an interrupted run resumes.

    python3 tools/rg_sync.py --limit-cities 15      # quick test
    python3 tools/rg_sync.py                        # everything

Output: build/rg/stations.bin (the table, layout in write_table) and
build/rg/stations.json (the same tree, readable).
"""

import argparse
import concurrent.futures
import http.client
import json
import os
import re
import socket
import ssl
import struct
import sys
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zlib

BROWSER_JS = '/Users/charlesvestal/github/schwung-parent/schwung-radiogarden/src/browser.js'
RG_API = 'https://radio.garden/api'
UA = 'Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36'
TIMEOUT = 8.0
RETRIES = 3
WORKERS = 8
MAX_TABLE = 0x44000  # the 0x45000 area minus its 4 KiB commit record
MAX_URL = 200
MAX_REDIRECTS = 6
PROBE_BYTES = 512

MAGIC = 0x31544752  # "RGT1"
VERSION = 1
HDR_SIZE = 64

MP3_TYPES = ('audio/mpeg', 'audio/mp3', 'audio/mpeg3', 'audio/x-mpeg')


def log(*a):
    print(*a, flush=True)


# ── cache ────────────────────────────────────────────────────────────────────

class Cache:
    def __init__(self, path, refresh):
        self.path = path
        self.lock = threading.Lock()
        self.data = {}
        self.dirty = 0
        if not refresh and os.path.exists(path):
            try:
                with open(path) as f:
                    self.data = json.load(f)
            except (OSError, ValueError) as e:
                log('cache unreadable (%s), starting empty' % e)

    def get(self, key):
        with self.lock:
            return self.data.get(key)

    def put(self, key, value):
        with self.lock:
            self.data[key] = value
            self.dirty += 1
            if self.dirty >= 100:
                self._save()

    def save(self):
        with self.lock:
            self._save()

    def _save(self):
        d = os.path.dirname(self.path)
        if d:
            os.makedirs(d, exist_ok=True)
        tmp = self.path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.data, f, separators=(',', ':'))
        os.replace(tmp, self.path)
        self.dirty = 0


# ── the city list ────────────────────────────────────────────────────────────

CITY_RE = re.compile(
    r"\{\s*continent:\s*'((?:[^'\\]|\\.)*)'\s*,\s*country:\s*'((?:[^'\\]|\\.)*)'\s*,"
    r"\s*city:\s*'((?:[^'\\]|\\.)*)'\s*\}")


def load_cities(path):
    with open(path, encoding='utf-8') as f:
        src = f.read()
    start = src.index('const CITIES = [')
    end = src.index('];', start)
    unq = lambda s: re.sub(r'\\(.)', r'\1', s)
    return [(unq(a), unq(b), unq(c)) for a, b, c in CITY_RE.findall(src[start:end])]


# ── Radio Garden API (as browser.js) ─────────────────────────────────────────

def url_encode(s):
    # browser.js urlEncode: unreserved kept, space -> '+', the rest %XX (per UTF-16
    # unit there; UTF-8 bytes here, which is what the server wants anyway)
    return urllib.parse.quote_plus(s, safe='-_.~')


def search_url(city, country):
    return RG_API + '/search?q=' + url_encode(city + ' ' + country)


def channels_url(place_id):
    return RG_API + '/ara/content/page/' + place_id + '/channels'


def listen_url(ch_id):
    return RG_API + '/ara/content/listen/' + ch_id + '/channel.mp3'


def place_id_from(data):
    hits = (data or {}).get('hits', {}).get('hits') if isinstance(data, dict) else None
    if not isinstance(hits, list):
        return None
    for hit in hits:
        src = hit.get('_source', hit)
        if src.get('type') == 'place':
            pid = ((src.get('page') or {}).get('url') or '').split('/')[-1]
            if pid and len(pid) > 2:
                return pid
    for hit in hits:
        src = hit.get('_source', hit)
        place = (src.get('page') or {}).get('place') or {}
        if src.get('type') == 'channel' and place.get('id'):
            return place['id']
    return None


def stations_from(data):
    out = []
    content = ((data or {}).get('data') or {}).get('content') or []
    for section in content:
        for item in section.get('items') or []:
            page = item.get('page') or {}
            parts = (page.get('url') or '').split('/')
            cid = parts[-1] if len(parts) >= 3 else None
            if not cid:
                continue
            out.append({'title': page.get('title') or '(Unknown Station)', 'id': cid})
    return out


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


_opener = urllib.request.build_opener(NoRedirect)


def rg_request(url, want_json=True):
    """GET a radio.garden URL. JSON -> parsed; else the redirect Location.
    Returns (ok, value). Retries with backoff on errors and 429."""
    headers = {'User-Agent': UA, 'Referer': 'https://radio.garden/',
               'Accept': 'application/json, text/plain, */*'}
    delay = 1.0
    last = None
    for attempt in range(RETRIES + 1):
        try:
            req = urllib.request.Request(url, headers=headers,
                                         method='GET' if want_json else 'HEAD')
            with _opener.open(req, timeout=TIMEOUT) as r:
                if want_json:
                    return True, json.loads(r.read().decode('utf-8'))
                return True, None  # 200 with no redirect: no stream URL
        except urllib.error.HTTPError as e:
            if not want_json and e.code in (301, 302, 303, 307, 308):
                return True, e.headers.get('Location')
            last = 'HTTP %d' % e.code
            if e.code == 404:
                return True, None
            if e.code == 429:
                ra = e.headers.get('Retry-After')
                time.sleep(float(ra) if ra and ra.isdigit() else delay * 4)
        except (urllib.error.URLError, socket.timeout, OSError, ValueError,
                http.client.HTTPException) as e:
            last = str(e)
        time.sleep(delay)
        delay *= 2
    return False, last


# ── stream probing ───────────────────────────────────────────────────────────

_ssl_ctx = ssl.create_default_context()
_ssl_ctx.check_hostname = False
_ssl_ctx.verify_mode = ssl.CERT_NONE


def probe_once(url):
    """One GET; read the status line + headers (+ a few body bytes), then close.
    Speaks raw sockets so SHOUTcast's 'ICY 200 OK' parses too.
    Returns {'status', 'headers' (lower-case keys), 'body' (hex of first bytes)}."""
    u = urllib.parse.urlsplit(url)
    secure = u.scheme == 'https'
    host = u.hostname
    port = u.port or (443 if secure else 80)
    path = u.path or '/'
    if u.query:
        path += '?' + u.query
    hosthdr = u.netloc.split('@')[-1]
    req = ('GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n'
           'Icy-MetaData: 0\r\nConnection: close\r\n\r\n' % (path, hosthdr, UA)).encode('latin-1')
    s = socket.create_connection((host, port), timeout=TIMEOUT)
    try:
        if secure:
            s = _ssl_ctx.wrap_socket(s, server_hostname=host)
        s.settimeout(TIMEOUT)
        s.sendall(req)
        buf = b''
        deadline = time.time() + TIMEOUT
        while b'\r\n\r\n' not in buf and b'\n\n' not in buf and len(buf) < 16384:
            if time.time() > deadline:
                break
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        sep = buf.find(b'\r\n\r\n')
        cut = 4
        if sep < 0:
            sep = buf.find(b'\n\n')
            cut = 2
        if sep < 0:
            raise OSError('no header end')
        head, body = buf[:sep], buf[sep + cut:]
        while len(body) < 4 and time.time() < deadline:
            try:
                chunk = s.recv(PROBE_BYTES)
            except OSError:
                break
            if not chunk:
                break
            body += chunk
    finally:
        try:
            s.close()
        except OSError:
            pass
    lines = head.decode('latin-1').replace('\r\n', '\n').split('\n')
    status_line = lines[0].strip()
    m = re.match(r'(?:HTTP/\d(?:\.\d)?|ICY)\s+(\d{3})', status_line)
    status = int(m.group(1)) if m else 0
    headers = {}
    for ln in lines[1:]:
        if ':' in ln:
            k, v = ln.split(':', 1)
            headers[k.strip().lower()] = v.strip()
    return {'status': status, 'line': status_line[:60], 'headers': headers,
            'body': body[:16].hex()}


def probe(cache, url):
    key = 'probe:' + url
    hit = cache.get(key)
    if hit is not None:
        return hit
    res = None
    for attempt in range(2):
        try:
            res = probe_once(url)
            break
        except (OSError, socket.timeout, ssl.SSLError, ValueError, UnicodeError) as e:
            res = {'status': 0, 'error': str(e)[:100]}
            time.sleep(0.5)
    cache.put(key, res)
    return res


def is_mp3(p):
    ct = (p.get('headers') or {}).get('content-type', '').split(';')[0].strip().lower()
    if ct in MP3_TYPES:
        return True
    if ct in ('', 'application/octet-stream'):
        b = bytes.fromhex(p.get('body') or '')
        return b[:3] == b'ID3' or (len(b) >= 2 and b[0] == 0xFF and (b[1] & 0xE6) == 0xE2)
    return False


def follow(cache, url):
    """Follow redirects by hand from url. Returns (chain of urls, final probe)."""
    chain = [url]
    for _ in range(MAX_REDIRECTS):
        p = probe(cache, url)
        loc = (p.get('headers') or {}).get('location')
        if p.get('status') in (301, 302, 303, 307, 308) and loc:
            url = urllib.parse.urljoin(url, loc)
            chain.append(url)
            continue
        return chain, p
    return chain, {'status': 0, 'error': 'too many redirects'}


def url_ok(url):
    b = url.encode('utf-8', 'replace')
    return len(b) <= MAX_URL and all(33 <= c <= 126 for c in b)


# Per-listener tokens that expire in seconds (Zeno "zt", Radiojar/Revma "rj-tok", ...):
# a URL carrying one is useless tomorrow, so the stable URL that redirects to it is kept.
TOKEN_RE = re.compile(r'[?&](rj-tok|zt|token|tok|auth|sig|signature|expires|exp|hash|key)=', re.I)


def http_variant(url):
    return 'http://' + url[len('https://'):] if url.startswith('https://') else url


def pick(chain, p):
    """A chain that ends in 200 MP3 over http:// -> (url to store, redirect hops) or None."""
    final = chain[-1]
    if not final.startswith('http://') or '.m3u8' in final.lower():
        return None
    if p.get('status') != 200 or not is_mp3(p):
        return None
    if not TOKEN_RE.search(final):
        return final, 0
    # earliest URL from which every hop stays on plain http and carries no token
    i = len(chain) - 1
    while i > 0 and chain[i - 1].startswith('http://') and not TOKEN_RE.search(chain[i - 1]):
        i -= 1
    if i == len(chain) - 1:
        return None  # only the tokenised URL itself is plain http
    return chain[i], len(chain) - 1 - i


def resolve_station(cache, ch_id):
    """-> dict(url, type, redirects, ...) or dict(url=None, why)."""
    key = 'listen:' + ch_id
    loc = cache.get(key)
    if loc is None:
        ok, loc = rg_request(listen_url(ch_id), want_json=False)
        if not ok:
            return {'url': None, 'why': 'rg: %s' % loc}
        cache.put(key, loc or '')
    if not loc:
        return {'url': None, 'why': 'no redirect'}
    if '.m3u8' in loc.lower():
        return {'url': None, 'why': 'hls'}
    chain, p = follow(cache, loc)
    tries = [(chain, p)]
    final = chain[-1]
    if final.startswith('https://') and not TOKEN_RE.search(final):
        alt = http_variant(final)
        tries.append(([alt], probe(cache, alt)))
    if loc.startswith('https://'):
        tries.append(follow(cache, http_variant(loc)))
    for ch, pr in tries:
        got = pick(ch, pr)
        if not got:
            continue
        u, hops = got
        if not url_ok(u):
            return {'url': None, 'why': 'url too long / bad bytes'}
        h = pr.get('headers') or {}
        return {'url': u, 'type': h.get('content-type', '(sniffed)'), 'redirects': hops,
                'final': ch[-1] if hops else None,
                'chunked': 'chunked' in h.get('transfer-encoding', '').lower(),
                'icy': pr.get('line', '').startswith('ICY')}
    ct = (p.get('headers') or {}).get('content-type', '')
    return {'url': None, 'why': '%s %s %s' % (final[:5], p.get('status'), ct or p.get('error', ''))}


# ── names ────────────────────────────────────────────────────────────────────

def ascii_name(s, cap, fallback='?'):
    """the screen's ASCII: accents dropped, other scripts left out (not shown as '?'), the separators they
    leave behind tidied; nothing left: the fallback"""
    s = unicodedata.normalize('NFKD', s or '')
    s = ''.join(c for c in s if not unicodedata.combining(c))
    s = ''.join(c if 32 <= ord(c) <= 126 else ' ' for c in s)
    s = re.sub(r'\s+', ' ', s).strip()
    s = re.sub(r'^[\s\-|:/,.]+|[\s\-|:/,(]+$', '', s)
    s = re.sub(r'\(\s*\)', '', s).strip()
    s = s[:cap].rstrip()
    return s if re.search(r'[A-Za-z0-9]', s) else fallback


# ── per city ─────────────────────────────────────────────────────────────────

def cached_json(cache, url):
    hit = cache.get(url)
    if hit is not None:
        return True, hit
    ok, v = rg_request(url, want_json=True)
    if ok:
        cache.put(url, v)
    return ok, v


def do_city(cache, entry, max_per_city):
    continent, country, city = entry
    ok, data = cached_json(cache, search_url(city, country))
    if not ok:
        return entry, None, 'search failed: %s' % data
    pid = place_id_from(data)
    if not pid:
        return entry, [], 'no place'
    ok, data = cached_json(cache, channels_url(pid))
    if not ok:
        return entry, None, 'channels failed: %s' % data
    chans = stations_from(data)
    out = []
    for ch in chans:
        if len(out) >= max_per_city:
            break
        r = resolve_station(cache, ch['id'])
        if r.get('url'):
            out.append({'name': ch['title'], 'id': ch['id'], 'url': r['url'],
                        'type': r['type'], 'redirects': r['redirects'],
                        'chunked': r['chunked'], 'icy': r['icy']})
    return entry, out, '%d/%d channels playable' % (len(out), len(chans))


# ── the table ────────────────────────────────────────────────────────────────

def build_tree(results, per_city):
    """results: {(continent,country,city): [stations]} -> sorted nested tree."""
    tree = {}
    for (co, cn, ci), sts in results.items():
        sts = sts[:per_city]
        if not sts:
            continue
        a, b, c = ascii_name(co, 32), ascii_name(cn, 32), ascii_name(ci, 32)
        tree.setdefault(a, {}).setdefault(b, {}).setdefault(c, []).extend(sts)
    key = lambda s: (s.lower(), s)
    out = []
    for co in sorted(tree, key=key):
        cl = []
        for cn in sorted(tree[co], key=key):
            cil = []
            for ci in sorted(tree[co][cn], key=key):
                cil.append({'city': ci, 'stations': tree[co][cn][ci]})
            cl.append({'country': cn, 'cities': cil})
        out.append({'continent': co, 'countries': cl})
    return out


def write_table(tree, created):
    conts, ctrs, cits, stas = [], [], [], []
    for co in tree:
        conts.append([co['continent'], len(ctrs), len(co['countries'])])
        for cn in co['countries']:
            ctrs.append([cn['country'], len(cits), len(cn['cities'])])
            for ci in cn['cities']:
                cits.append([ci['city'], len(stas), len(ci['stations'])])
                for k, st in enumerate(ci['stations']):
                    stas.append([ascii_name(st['name'], 40, f"{ci['city']} {k + 1}"), st['url']])
    for n in (len(conts), len(ctrs), len(cits), len(stas)):
        if n > 0xFFFF:
            raise SystemExit('too many entries for u16: %d' % n)
    off_cont = HDR_SIZE
    off_ctr = off_cont + 8 * len(conts)
    off_cit = off_ctr + 8 * len(ctrs)
    off_sta = off_cit + 8 * len(cits)
    off_str = off_sta + 8 * len(stas)
    strings = bytearray()
    index = {}

    def sref(s):
        if s not in index:
            index[s] = off_str + len(strings)
            strings.extend(s.encode('ascii') + b'\0')
        return index[s]

    body = bytearray()
    for name, first, n in conts + ctrs + cits:
        body += struct.pack('<IHH', sref(name), first, n)
    for name, url in stas:
        body += struct.pack('<II', sref(name), sref(url))
    body += strings
    total = HDR_SIZE + len(body)
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    hdr = struct.pack('<IHHHHHHIIIIIIII', MAGIC, VERSION, len(conts), len(ctrs), len(cits),
                      len(stas), 0, off_cont, off_ctr, off_cit, off_sta, off_str,
                      total, crc, created)
    hdr += b'\0' * (HDR_SIZE - len(hdr))
    assert len(hdr) == HDR_SIZE
    return bytes(hdr) + bytes(body), (len(conts), len(ctrs), len(cits), len(stas))


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cache', default=os.path.join(here, 'build/rg/cache.json'))
    ap.add_argument('--out', default=os.path.join(here, 'build/rg/stations.bin'))
    ap.add_argument('--max-per-city', type=int, default=6)
    ap.add_argument('--limit-cities', type=int, default=0)
    ap.add_argument('--refresh', action='store_true', help='ignore the cache')
    ap.add_argument('--cities-js', default=BROWSER_JS)
    ap.add_argument('--workers', type=int, default=WORKERS)
    args = ap.parse_args()

    t0 = time.time()
    cities = load_cities(args.cities_js)
    log('%d cities in %s' % (len(cities), args.cities_js))
    if args.limit_cities:
        cities = cities[:args.limit_cities]
    cache = Cache(args.cache, args.refresh)
    results = {}
    failed = []
    try:
        with concurrent.futures.ThreadPoolExecutor(args.workers) as ex:
            futs = [ex.submit(do_city, cache, c, args.max_per_city) for c in cities]
            for i, f in enumerate(concurrent.futures.as_completed(futs), 1):
                entry, sts, msg = f.result()
                if sts is None:
                    failed.append(entry)
                else:
                    results[entry] = sts
                log('[%d/%d] %s, %s: %s' % (i, len(cities), entry[2], entry[1], msg))
    finally:
        cache.save()

    created = int(time.time())
    per = args.max_per_city
    while True:
        tree = build_tree(results, per)
        blob, counts = write_table(tree, created)
        if len(blob) <= MAX_TABLE or per <= 1:
            break
        log('table %d bytes > %d: trying %d stations per city' % (len(blob), MAX_TABLE, per - 1))
        per -= 1
    if len(blob) > MAX_TABLE:
        raise SystemExit('table still %d bytes at 1 station per city' % len(blob))
    if per != args.max_per_city:
        log('NOTE: reduced to %d stations per city to fit 0x%X bytes' % (per, MAX_TABLE))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(blob)
    jpath = os.path.splitext(args.out)[0] + '.json'
    with open(jpath, 'w') as f:
        json.dump({'created': created, 'max_per_city': per, 'tree': tree}, f, indent=1)

    nst = counts[3]
    every = [s for co in tree for cn in co['countries'] for ci in cn['cities']
             for s in ci['stations']]
    chunked = sum(1 for s in every if s.get('chunked'))
    redir = sum(1 for s in every if s.get('redirects'))
    log('')
    log('cities searched:              %d (%d failed, rerun to retry)' % (len(cities), len(failed)))
    log('cities with playable stations: %d' % counts[2])
    log('countries / continents:        %d / %d' % (counts[1], counts[0]))
    log('stations:                      %d (%d per city max; %d chunked; %d need a redirect '
        'followed: tokenised streams)' % (nst, per, chunked, redir))
    log('table:                         %d bytes (limit %d) -> %s' % (len(blob), MAX_TABLE, args.out))
    log('json:                          %s' % jpath)
    log('time:                          %.0f s' % (time.time() - t0))


if __name__ == '__main__':
    main()
