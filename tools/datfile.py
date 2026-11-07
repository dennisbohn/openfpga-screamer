"""A library that reads Screamer's .DAT containers. The other tools in this folder import it.

Entry points: read(path) gives the file's bytes, entries(data) lists the records, and
extract(data, name) returns one file, unpacked.

Format (from openscreamer docs/NOTES.md): [data][N x 32-byte records][u32 dir_size].
A record is the 16-byte name XOR 0x3A, then u32 offset, stored_size, uncompressed_size, method.
Method 0 is stored verbatim; methods 1-3 are LZW, as OpenScreamer worked it out from the game
(tools/lzw.py there): codes of 9 to 12 bits, packed LSB first. Code 256 clears the table and 257
ends the stream. New codes start at 258. The code width grows when the next free code reaches
512, 1024 and 2048.
"""
import struct


class Entry:
    """One record of a container's directory."""
    def __init__(self, name, offset, stored, size, method):
        self.name, self.offset, self.stored, self.size, self.method = name, offset, stored, size, method


def entries(data):
    """The directory of the container bytes data, as a list of Entry."""
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
    """The bytes of a file."""
    with open(path, "rb") as fh:
        return fh.read()


def lzw(src, size):
    """Decodes one LZW stream to at most size bytes."""
    prefix = [0] * 4096
    suffix = list(range(256)) + [0] * (4096 - 256)
    out = bytearray()
    bitpos, limit = 0, len(src) * 8
    width, threshold, free, prev = 9, 512, 258, -1
    while len(out) < size and bitpos + width <= limit:
        b = bitpos >> 3
        code = (int.from_bytes(src[b:b + 3].ljust(3, b"\0"), "little") >> (bitpos & 7)) & ((1 << width) - 1)
        bitpos += width
        if code == 257:
            break
        if code == 256:
            width, threshold, free, prev = 9, 512, 258, -1
            continue
        c = prev if code >= free else code         # code == free: the previous string + its first byte
        if c < 0:
            raise ValueError("LZW: bad first code")
        chars = []
        while c > 255:
            chars.append(suffix[c])
            c = prefix[c]
        chars.append(c)
        first = c
        chars.reverse()
        if code >= free:
            chars.append(first)
        out += bytes(chars)
        if prev >= 0 and free < 4096:
            prefix[free], suffix[free] = prev, first
            free += 1
            if free >= threshold and width < 12:
                width += 1
                threshold *= 2
        prev = code
    return bytes(out[:size])


def extract(data, name):
    """The unpacked bytes of the file name (case does not matter) in the container data.

    Raises KeyError if there is no such file and ValueError if it does not unpack to its size."""
    for e in entries(data):
        if e.name.upper() == name.upper():
            blob = data[e.offset:e.offset + e.stored]
            out = blob[:e.size] if e.method == 0 else lzw(blob, e.size)
            if len(out) != e.size:
                raise ValueError(f"{name}: {len(out)} bytes, expected {e.size}")
            return out
    raise KeyError(name)
