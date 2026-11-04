"""Write a neutral LUT strip, the colour grading table that changes nothing, as an image to grade.

    python tools/neutral_lut.py lut.png [size]

writes a PNG size * size wide and size tall (32, so 1024 by 32, by default). It has a square for each step of blue from
left to right, with red going across each square and green going down, which is how buh_color_grading_lut reads one.
Put it into a screenshot, grade the whole picture in an image editor, then cut the strip back out and save it as a PNG
of its own.
"""
import struct
import sys
import zlib


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)


def neutral_rows(size):
    rows = []
    for green in range(size):
        # Each row starts with its filter, none
        row = bytearray([0])
        for blue in range(size):
            for red in range(size):
                row += bytes(round(value * 255 / (size - 1)) for value in (red, green, blue))
        rows.append(bytes(row))
    return b''.join(rows)


def main():
    if not 2 <= len(sys.argv) <= 3:
        raise SystemExit('usage: neutral_lut.py <output.png> [size]')
    path = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) == 3 else 32
    if not 2 <= size <= 64:
        raise SystemExit('the size goes from 2 to 64')
    # 8 bits a channel, RGB
    header = struct.pack('>IIBBBBB', size * size, size, 8, 2, 0, 0, 0)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(neutral_rows(size), 9))
           + chunk(b'IEND', b''))
    with open(path, 'wb') as out:
        out.write(png)
    print(f'{path}: {size * size} by {size}')


if __name__ == '__main__':
    main()
