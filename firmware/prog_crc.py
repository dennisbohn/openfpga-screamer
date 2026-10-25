"""Write the CRC-32 of program.bin's bytes after its 16-byte header into header word 2
(start_prog.S), and check magic and size. Usage: python3 prog_crc.py program.bin"""
import struct
import sys
import zlib

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
magic, size = struct.unpack_from("<2I", data, 0)
if magic != 0x50524353 or size != len(data):
    sys.exit(f"{path}: header {magic:08x}, size {size}, file {len(data)} bytes")
struct.pack_into("<I", data, 8, zlib.crc32(data[16:]))
open(path, "wb").write(data)
