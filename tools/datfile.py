"""Read Screamer's .DAT containers.

Format (from openscreamer docs/NOTES.md): [data][N x 32-byte records][u32 dir_size].
Record: 16-byte name XOR 0x3A, then u32 offset, stored_size, uncompressed_size, method.
Method 0 is stored verbatim, 1-3 are LZW (not needed yet, so not implemented here).
"""
import struct


class Entry:
    def __init__(self, name, offset, stored, size, method):
        self.name, self.offset, self.stored, self.size, self.method = name, offset, stored, size, method


def entries(data):
    dir_size = struct.unpack_from("<I", data, len(data) - 4)[0]
    out = []
    for pos in range(len(data) - 4 - dir_size, len(data) - 4, 32):
        raw = bytes(b ^ 0x3A for b in data[pos:pos + 16])
        name = raw.split(b"\0")[0].decode("latin-1")
        if not name:
            break
        out.append(Entry(name, *struct.unpack_from("<4I", data, pos + 16)))
    return out


def read(path):
    with open(path, "rb") as fh:
        return fh.read()


def extract(data, name):
    for e in entries(data):
        if e.name.upper() == name.upper():
            if e.method != 0:
                raise ValueError(f"{name}: compressed (method {e.method}), LZW not implemented")
            return data[e.offset:e.offset + e.size]
    raise KeyError(name)
