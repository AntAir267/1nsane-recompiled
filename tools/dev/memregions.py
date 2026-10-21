#!/usr/bin/env python3
# (development script) Summarise a --dump-memory file so two machines' dumps can be compared
# by text alone: one line per region (base, size, sha256 prefix), and with --chunks one line
# per 64 KB piece of each region. Run it on both dumps and diff the outputs.
# usage: memregions.py DUMP [--chunks]
import hashlib, struct, sys

CHUNK = 0x10000


def main():
    path = sys.argv[1]
    chunks = "--chunks" in sys.argv[2:]
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    while off + 8 <= len(data):
        base, size = struct.unpack_from("<II", data, off)
        off += 8
        body = data[off:off + size]
        off += size
        print("%08x %08x %s" % (base, size, hashlib.sha256(body).hexdigest()[:16]))
        if chunks:
            for c in range(0, size, CHUNK):
                piece = body[c:c + CHUNK]
                print("  %08x %s" % (base + c, hashlib.sha256(piece).hexdigest()[:16]))


if __name__ == "__main__":
    main()
