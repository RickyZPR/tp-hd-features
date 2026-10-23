#!/usr/bin/env python3
"""yaz0.py - Yaz0 / Yay0 decompressor (Nintendo GC/Wii/Wii U), pure Python, no deps.

CLI USAGE
  python -I yaz0.py <in_file> [out_file]
      Decompress a Yaz0 (or Yay0) file. If out_file is omitted, writes <basename>.dec
      into the CURRENT directory (never next to the input, so read-only sources stay
      untouched). If the input is not compressed, it is copied unchanged (note on stderr).
  python -I yaz0.py --info <in_file>
      Print magic and declared decompressed size only.

LIBRARY USAGE
  import yaz0                      # (sys.path.insert(0, '<tools dir>') first)
  data = yaz0.maybe_decompress(raw_bytes)   # passthrough if not Yaz0/Yay0
  data = yaz0.decompress_yaz0(raw_bytes)
  yaz0.is_yaz0(raw_bytes) -> bool

FORMAT (verified against GC TP and Wii U TP HD .arc files)
  0x00 'Yaz0'  0x04 u32 BE decompressed size  0x08..0x0F reserved/alignment (0 on GC & HD)
  then groups: 1 code byte, MSB first; bit=1 -> literal byte; bit=0 -> back-reference:
    b1 b2: dist = ((b1 & 0x0F) << 8 | b2) + 1 ; n = b1 >> 4
           n == 0 -> length = next_byte + 0x12  else length = n + 2
"""
import struct
import sys
import os


def is_yaz0(data):
    return len(data) >= 16 and data[:4] == b"Yaz0"


def is_yay0(data):
    return len(data) >= 16 and data[:4] == b"Yay0"


def decompress_yaz0(src):
    if not is_yaz0(src):
        raise ValueError("not Yaz0 data")
    out_size = struct.unpack_from(">I", src, 4)[0]
    dst = bytearray(out_size)
    sp = 16
    dp = 0
    slen = len(src)
    while dp < out_size:
        if sp >= slen:
            raise ValueError("Yaz0: truncated input at dst %d/%d" % (dp, out_size))
        code = src[sp]
        sp += 1
        for bit in range(8):
            if dp >= out_size:
                break
            if code & (0x80 >> bit):
                dst[dp] = src[sp]
                dp += 1
                sp += 1
            else:
                b1 = src[sp]
                b2 = src[sp + 1]
                sp += 2
                dist = (((b1 & 0x0F) << 8) | b2) + 1
                n = b1 >> 4
                if n == 0:
                    length = src[sp] + 0x12
                    sp += 1
                else:
                    length = n + 2
                cp = dp - dist
                if cp < 0:
                    raise ValueError("Yaz0: back-reference before start (dp=%d dist=%d)" % (dp, dist))
                if length > out_size - dp:
                    length = out_size - dp
                if dist >= length:
                    dst[dp:dp + length] = dst[cp:cp + length]
                    dp += length
                else:
                    for _ in range(length):
                        dst[dp] = dst[cp]
                        dp += 1
                        cp += 1
    return bytes(dst)


def decompress_yay0(src):
    if not is_yay0(src):
        raise ValueError("not Yay0 data")
    out_size, link_off, chunk_off = struct.unpack_from(">III", src, 4)
    dst = bytearray(out_size)
    mask_p = 16
    link_p = link_off
    chunk_p = chunk_off
    dp = 0
    mask = 0
    bits = 0
    while dp < out_size:
        if bits == 0:
            mask = struct.unpack_from(">I", src, mask_p)[0]
            mask_p += 4
            bits = 32
        if mask & 0x80000000:
            dst[dp] = src[chunk_p]
            dp += 1
            chunk_p += 1
        else:
            link = struct.unpack_from(">H", src, link_p)[0]
            link_p += 2
            dist = (link & 0xFFF) + 1
            n = link >> 12
            if n == 0:
                length = src[chunk_p] + 0x12
                chunk_p += 1
            else:
                length = n + 2
            cp = dp - dist
            for _ in range(min(length, out_size - dp)):
                dst[dp] = dst[cp]
                dp += 1
                cp += 1
        mask = (mask << 1) & 0xFFFFFFFF
        bits -= 1
    return bytes(dst)


def maybe_decompress(data):
    """Return decompressed bytes if data is Yaz0/Yay0, else data unchanged."""
    if is_yaz0(data):
        return decompress_yaz0(data)
    if is_yay0(data):
        return decompress_yay0(data)
    return data


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    if argv[1] == "--info":
        with open(argv[2], "rb") as f:
            hdr = f.read(16)
        if hdr[:4] in (b"Yaz0", b"Yay0"):
            print("%s magic=%s decompressed_size=%d" % (argv[2], hdr[:4].decode(), struct.unpack_from(">I", hdr, 4)[0]))
        else:
            print("%s not compressed (magic=%r)" % (argv[2], hdr[:4]))
        return 0
    inp = argv[1]
    outp = argv[2] if len(argv) > 2 else os.path.basename(inp) + ".dec"
    with open(inp, "rb") as f:
        raw = f.read()
    if not (is_yaz0(raw) or is_yay0(raw)):
        sys.stderr.write("note: input is not Yaz0/Yay0, copying unchanged\n")
    data = maybe_decompress(raw)
    with open(outp, "wb") as f:
        f.write(data)
    print("%s -> %s (%d -> %d bytes)" % (inp, outp, len(raw), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
