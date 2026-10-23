"""Shared helpers for the asset pipeline: sys.path setup for the existing tool folders, logging, file and
PNG helpers. Pure standard library (zlib + struct for PNG)."""
import hashlib
import json
import os
import struct
import sys
import time
import zlib

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.dirname(TOOLS)


def setup_paths():
    """formats/critic/cave go to the FRONT of sys.path (tools/rarc.py at the top level is a different,
    older RARC module and must not shadow tools/formats/rarc.py)."""
    for sub in ('cave', 'critic', 'formats'):
        p = os.path.join(TOOLS, sub)
        if p in sys.path:
            sys.path.remove(p)
        sys.path.insert(0, p)


setup_paths()


class Log(object):
    def __init__(self, verbose=False):
        self.t0 = time.time()
        self.verbose = verbose
        self.lines = []

    def __call__(self, msg, *a):
        s = msg % a if a else msg
        line = '[%6.1fs] %s' % (time.time() - self.t0, s)
        self.lines.append(line)
        print(line, flush=True)

    def debug(self, msg, *a):
        s = msg % a if a else msg
        self.lines.append('        ' + s)
        if self.verbose:
            print('        ' + s, flush=True)

    def elapsed(self):
        return time.time() - self.t0


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def write_json(path, obj):
    write(path, (json.dumps(obj, indent=1, sort_keys=False) + '\n').encode('utf-8'))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def align(v, a):
    return (v + a - 1) // a * a


def hd_path(hd, *parts):
    return os.path.join(hd, *parts)


def exists_ci(directory, name):
    """Case-insensitive file lookup (the Dusklight DVD layer matches names case-insensitively).
    Returns the real path or None."""
    if not os.path.isdir(directory):
        return None
    low = name.lower()
    for n in os.listdir(directory):
        if n.lower() == low:
            return os.path.join(directory, n)
    return None


# ----------------------------------------------------------------------------------------------- PNG
def png_bytes(w, h, rgba):
    """8-bit RGBA PNG, filter 0 on every row, zlib level 9 (deterministic)."""
    if len(rgba) != w * h * 4:
        raise ValueError('rgba size %d != %dx%dx4' % (len(rgba), w, h))
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def png_read(data):
    """Minimal reader for the PNGs written above (8-bit RGBA, non-interlaced; filters 0-4 supported).
    Returns (w, h, rgba bytes). Verifies every chunk CRC."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    o = 8
    idat = b''
    w = h = None
    while o < len(data):
        n = struct.unpack_from('>I', data, o)[0]
        t = data[o + 4:o + 8]
        d = data[o + 8:o + 8 + n]
        crc = struct.unpack_from('>I', data, o + 8 + n)[0]
        if zlib.crc32(t + d) & 0xFFFFFFFF != crc:
            raise ValueError('bad CRC in %r' % t)
        if t == b'IHDR':
            w, h, bd, ct, cm, fm, il = struct.unpack('>IIBBBBB', d)
            if (bd, ct, il) != (8, 6, 0):
                raise ValueError('unsupported PNG (bit depth %d, colour type %d, interlace %d)' % (bd, ct, il))
        elif t == b'IDAT':
            idat += d
        elif t == b'IEND':
            break
        o += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4
    if len(raw) != h * (stride + 1):
        raise ValueError('IDAT size %d != %d' % (len(raw), h * (stride + 1)))
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if ft == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ft == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out)
