#!/usr/bin/env python3
"""Chunked stop-and-wait pusher — the bench's copy of the upload page's protocol.

Speaks POST /chunk?name=&off=&crc=&last= exactly like the page JS (4 KB pieces,
one in flight, resync on 409/errors), so the acceptance protocol can run without
a browser, and so failure modes a browser won't produce can be injected on
purpose:

  python3 tools/chunk_push.py http://192.168.1.57 book.epub            # plain push
  python3 tools/chunk_push.py http://192.168.1.57 *.epub               # a batch
  python3 tools/chunk_push.py --piece 8192 HOST FILE                   # size experiments
  python3 tools/chunk_push.py --corrupt 5 HOST FILE    # send piece 5 with a wrong CRC once
  python3 tools/chunk_push.py --restart-at 50 HOST FILE # abandon mid-file on purpose...
  python3 tools/chunk_push.py --resume HOST FILE        # ...then continue from what the
                                                        # phone holds instead of restarting

A plain run always starts at 0, which TRUNCATES — same deliberate rule as the
page (a fresh push means a fresh file). --resume trusts the partial already on
the card and continues after it; that trust is yours to give, the prefix is not
re-verified.

Exit 0 only if every file lands with the server holding exactly len(file) bytes.
"""
import sys, os, time, zlib, argparse
from urllib.request import Request, urlopen
from urllib.parse import quote
from urllib.error import URLError, HTTPError

def held(base, name, timeout=10):
    with urlopen(Request(base + '/chunk?name=' + quote(name)), timeout=timeout) as r:
        return int(r.read().decode().strip())

def post_piece(base, name, off, data, last, crc_hex, timeout=15):
    """One multipart POST of one piece. Returns (status, body-text)."""
    boundary = 'chunkpushb0undary'
    body = (('--%s\r\nContent-Disposition: form-data; name="p"; filename="p"\r\n'
             'Content-Type: application/octet-stream\r\n\r\n') % boundary).encode() \
           + data + ('\r\n--%s--\r\n' % boundary).encode()
    url = '%s/chunk?name=%s&off=%d&crc=%s&last=%d' % (base, quote(name), off, crc_hex, 1 if last else 0)
    req = Request(url, data=body, headers={
        'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors='replace')
    except HTTPError as e:
        return e.code, e.read().decode(errors='replace')

def push(base, path, piece, corrupt_at=None, restart_at=None, resume=False, gap=0.0):
    name = os.path.basename(path)
    data = open(path, 'rb').read()
    total = len(data)
    off, tries, sent, t0 = 0, 0, 0, time.time()
    if resume:
        try:
            off = min(held(base, name), total)
        except Exception as e:
            print('  resume query failed (%s), starting from 0' % e)
        if off:
            print('  resuming %s from byte %d' % (name, off))
    corrupted = restarted = False
    while off < total:
        end = min(off + piece, total)
        buf = data[off:end]
        n = (off // piece) + 1
        if restart_at is not None and n >= restart_at and not restarted:
            restarted = True
            print('  [inject] stopping mid-file at piece %d (re-run to prove resume)' % n)
            return None
        crc = zlib.crc32(buf) & 0xFFFFFFFF
        if corrupt_at is not None and n == corrupt_at and not corrupted:
            corrupted = True
            crc ^= 0xDEADBEEF
            print('  [inject] piece %d sent with a wrong CRC' % n)
        try:
            code, text = post_piece(base, name, off, buf, end >= total, '%08x' % crc)
        except (URLError, OSError) as e:
            tries += 1
            if tries > 24:
                print('  gave up: %s' % e)
                return False
            time.sleep(min(0.5 * tries, 5.0))
            try:
                off = min(held(base, name), total)
            except Exception:
                pass
            continue
        if code == 200:
            tries = 0
            sent += len(buf)
            got = int(''.join(c for c in text if c.isdigit()) or -1)
            off = got if off < got <= total else end
            if gap:
                time.sleep(gap)
        elif code == 409:
            tries += 1
            if tries > 24:
                print('  gave up resyncing (server says: %s)' % text.strip())
                return False
            got = int(''.join(c for c in text if c.isdigit()) or -1)
            if 0 <= got <= total:
                off = got
            time.sleep(0.15)
        else:
            tries += 1
            if tries > 24:
                print('  gave up: HTTP %d %s' % (code, text.strip()))
                return False
            time.sleep(min(0.5 * tries, 5.0))
    dt = time.time() - t0
    final = held(base, name)
    okmark = 'OK' if final == total else 'SIZE MISMATCH (server %d != %d)' % (final, total)
    print('  %s: %d bytes in %.1fs (%.0f KB/s, sent %d) - %s'
          % (name, total, dt, total / dt / 1024 if dt else 0, sent, okmark))
    return final == total

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base', help='http://<phone-ip>')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--piece', type=int, default=4096)
    ap.add_argument('--corrupt', type=int, default=None, metavar='N',
                    help='send piece N with a wrong CRC once (server must 422, push must recover)')
    ap.add_argument('--restart-at', type=int, default=None, metavar='N',
                    help='abandon the push at piece N (then prove resume with --resume)')
    ap.add_argument('--resume', action='store_true',
                    help='continue after the bytes the phone already holds (no truncate)')
    ap.add_argument('--gap', type=float, default=0.0, metavar='SECS',
                    help='sleep between pieces (rate experiments)')
    a = ap.parse_args()
    base = a.base.rstrip('/')
    ok = True
    for p in a.files:
        r = push(base, p, a.piece, a.corrupt, a.restart_at, a.resume, a.gap)
        if r is None:      # deliberate mid-file stop
            return 0
        ok = ok and r
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
