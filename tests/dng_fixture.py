"""Independent tiny Bayer DNG writer for CLI regressions; standard library only."""
from pathlib import Path
import struct


def write_dng_fixture(path, shutter=(1, 100), width=96, height=64, scene=None):
    if scene is None:
        scene = lambda x, y: .002 if x < width//3 else .1 if x < 2*width//3 else 1.6
    u16 = lambda *v: struct.pack('<'+'H'*len(v), *v)
    u32 = lambda v: struct.pack('<I', v)
    rational = lambda a, b: struct.pack('<II', a, b)
    fields = {
        256: (4, 1, u32(width)), 257: (4, 1, u32(height)), 258: (3, 1, u16(16)),
        259: (3, 1, u16(1)), 262: (3, 1, u16(32803)),
        271: (2, 10, b'photo-ops\0'), 272: (2, 15, b'Python fixture\0'),
        273: (4, 1, b'\0'*4), 274: (3, 1, u16(1)), 277: (3, 1, u16(1)),
        278: (4, 1, u32(height)), 279: (4, 1, u32(width*height*2)), 284: (3, 1, u16(1)),
        33421: (3, 2, u16(2, 2)), 33422: (1, 4, bytes([0, 1, 1, 2])),
        34665: (4, 1, b'\0'*4), 50706: (1, 4, bytes([1, 4, 0, 0])),
        50707: (1, 4, bytes([1, 4, 0, 0])),
        50714: (5, 1, rational(512, 1)), 50717: (4, 1, u32(16383)),
        50721: (10, 9, b''.join(struct.pack('<ii', int(i % 4 == 0), 1) for i in range(9))),
        50728: (5, 3, rational(1, 1)*3), 50778: (3, 1, u16(21)),
    }
    model = b'photo-ops Python fixture\0'
    fields[50708] = (2, len(model), model)
    base = 8+2+12*len(fields)+4
    data = bytearray(base)
    data[:10] = b'II'+struct.pack('<HIH', 42, 8, len(fields))

    def append(payload):
        while len(data) % 4:
            data.append(0)
        offset = len(data)
        data.extend(payload)
        return offset

    records = []
    for tag, (kind, count, payload) in sorted(fields.items()):
        encoded = payload.ljust(4, b'\0') if len(payload) <= 4 else u32(append(payload))
        records.append([tag, kind, count, encoded])
    exif_offset = append(bytes(2+4*12+4))
    exif = [(33434, 5, 1, u32(append(rational(*shutter)))),
            (33437, 5, 1, u32(append(rational(4, 1)))),
            (34855, 3, 1, u16(100)+b'\0\0'),
            (37386, 5, 1, u32(append(rational(85, 1))))]
    struct.pack_into('<H', data, exif_offset, len(exif))
    for i, (tag, kind, count, value) in enumerate(exif):
        struct.pack_into('<HHI4s', data, exif_offset+2+12*i, tag, kind, count, value)
    signal_scale = shutter[0]/shutter[1]/.01
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            value = max(0, min(16383, round(512+15871*scene(x, y)*signal_scale)))
            pixels.extend(u16(value))
    pixel_offset = append(pixels)
    for index, (tag, kind, count, value) in enumerate(records):
        if tag == 273:
            value = u32(pixel_offset)
        if tag == 34665:
            value = u32(exif_offset)
        struct.pack_into('<HHI4s', data, 10+12*index, tag, kind, count, value)
    Path(path).write_bytes(data)
