#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
r"""
List and set the subtitles buh shows for sounds, in HEK .sound tags.

Every permutation of a sound has a subtitle data field Halo never uses. buh shows the text in it while that
permutation plays. A long sound is split into a chain of permutations ("pieces") when it's built: text on the first
piece stays up for the whole sound, or until a later piece with text of its own.

    subtitles.py list tags\sound\dialog\marine\pelican.sound
    subtitles.py set tags\sound\dialog\marine\pelican.sound "Get to the Pelican!"
    subtitles.py set tags\sound\dialog\marine\pelican.sound "Get to the Pelican!" --permutation 0
    subtitles.py set tags\sound\dialog\marine\pelican.sound "Move it!" --permutation take2
    subtitles.py set tags\sound\dialog\cortana\long.sound "...and the rest of it." --permutation 0 --piece 2
    subtitles.py clear tags\sound\dialog\marine\pelican.sound

Without --permutation, set and clear go to every permutation. Setting a whole permutation puts the text on its first
piece and clears the others; --piece sets just one piece. |n in the text starts a new line. The text is stored as
UTF-16, the way buh reads it. Then build the map (or tag pack) as usual. Give a sound you add subtitles to a path of
its own: one with a stock path may be taken from sounds.map instead, which has none.

The tag is only written if every byte of it is accounted for, and what's written is read back and checked first.
"""

import argparse
import os
import pathlib
import struct
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tagpack import Definitions, PackError  # noqa: E402

HEADER_SIZE = 0x40
SOUND_CLASS = b'snd!'
ENGINE = b'blam'
NULL_INDEX = -1


class SubtitleError(Exception):
    pass


class Piece:
    """One permutation in the tag: its own fields, and where its subtitle data is"""

    def __init__(self, index, name, next_index, size_field, data_at, data_size, text):
        self.index, self.name, self.next = index, name, next_index
        self.size_field, self.data_at, self.data_size, self.text = size_field, data_at, data_size, text


class PitchRange:
    def __init__(self, index, name, actual_count, pieces):
        self.index, self.name, self.actual_count, self.pieces = index, name, actual_count, pieces

    def chain(self, first):
        """A permutation's pieces in order, from its first"""
        result, seen, index = [], set(), first
        while 0 <= index < len(self.pieces) and index not in seen:
            seen.add(index)
            result.append(self.pieces[index])
            index = self.pieces[index].next
        return result


def decode(data):
    """Text as buh reads it: UTF-16 up to the first null"""
    if len(data) < 2:
        return None
    text = data[:len(data) // 2 * 2].decode('utf-16-le', errors='replace')
    return text.split('\0', 1)[0]


def encode(text):
    return text.encode('utf-16-le') + b'\0\0' if text else b''


class SoundTag:
    """A HEK .sound tag: a 64 byte big-endian header, then the sound, then everything its fields point to, in the
    order its fields come. A block's elements all come first, then each element's own; data and reference paths
    (with a null) come where their fields are."""

    def __init__(self, data, definitions, name='tag'):
        self.data, self.defs, self.name = bytes(data), definitions, name
        if len(self.data) < HEADER_SIZE or self.data[0x24:0x28] != SOUND_CLASS or self.data[0x3C:0x40] != ENGINE:
            raise SubtitleError(f'{name} is not a sound tag')
        if self.u32(0x2C) != HEADER_SIZE:
            raise SubtitleError(f'{name} has a header size of {self.u32(0x2C):#x}, not {HEADER_SIZE:#x}')
        self.arrays = {}
        self.subtitle_fields = {}
        end = self.walk('Sound', HEADER_SIZE, HEADER_SIZE + self.defs.struct_size('Sound'))
        if end != len(self.data):
            raise SubtitleError(f'{name}: reading it accounts for {end - HEADER_SIZE} of its {len(self.data) - HEADER_SIZE} bytes, '
                                'so it\'s left alone')
        self.pitch_ranges = self.read_pitch_ranges()

    def u32(self, at):
        return struct.unpack_from('>I', self.data, at)[0]

    def s16(self, at):
        return struct.unpack_from('>h', self.data, at)[0]

    def string(self, at, size=32):
        return self.data[at:at + size].split(b'\0', 1)[0].decode('latin1')

    def need(self, position, size, what):
        if position + size > len(self.data):
            raise SubtitleError(f'{self.name}: {what} runs past the end of the tag')

    def walk(self, struct_name, at, position):
        """Go through a struct's fields, returning where the data after everything it points to ends"""
        for field in self.defs.fields(struct_name):
            where = at + field.offset
            if field.type == 'TagReflexive':
                count = self.u32(where)
                element = field.info['struct']
                size = self.defs.struct_size(element)
                self.need(position, count * size, f'{struct_name}.{field.name} ({count} elements)')
                self.arrays[(at, field.name)] = (position, count)
                array, position = position, position + count * size
                for i in range(count):
                    position = self.walk(element, array + i * size, position)
            elif field.type == 'TagDataOffset':
                size = self.u32(where)
                self.need(position, size, f'{struct_name}.{field.name}')
                if struct_name == 'SoundPermutation' and field.name == 'subtitle data':
                    self.subtitle_fields[at] = (where, position, size)
                position += size
            elif field.type == 'TagDependency':
                length = self.u32(where + 8)
                if length:
                    self.need(position, length + 1, f'{struct_name}.{field.name}\'s path')
                    if self.data[position + length] != 0 or 0 in self.data[position:position + length]:
                        raise SubtitleError(f'{self.name}: {struct_name}.{field.name}\'s path isn\'t {length} characters and a null')
                    position += length + 1
        return position

    def read_pitch_ranges(self):
        defs = self.defs
        range_size, piece_size = defs.struct_size('SoundPitchRange'), defs.struct_size('SoundPermutation')
        array, count = self.arrays[(HEADER_SIZE, 'pitch ranges')]
        result = []
        for r in range(count):
            at = array + r * range_size
            pieces_array, piece_count = self.arrays[(at, 'permutations')]
            pieces = []
            for p in range(piece_count):
                piece_at = pieces_array + p * piece_size
                size_field, data_at, data_size = self.subtitle_fields[piece_at]
                pieces.append(Piece(p, self.string(piece_at + defs.offset_of('SoundPermutation', 'name')),
                                    self.s16(piece_at + defs.offset_of('SoundPermutation', 'next permutation index')),
                                    size_field, data_at, data_size, decode(self.data[data_at:data_at + data_size])))
            actual = self.s16(at + defs.offset_of('SoundPitchRange', 'actual permutation count'))
            result.append(PitchRange(r, self.string(at + defs.offset_of('SoundPitchRange', 'name')),
                                     max(0, min(actual, piece_count)), pieces))
        return result

    def with_texts(self, texts):
        """The tag's bytes with these pieces' subtitles replaced: {piece's size field: text or None}"""
        data = bytearray(self.data)
        operations = []
        for pitch_range in self.pitch_ranges:
            for piece in pitch_range.pieces:
                if piece.size_field in texts:
                    new = encode(texts[piece.size_field])
                    operations.append((piece.data_at, 'data', piece.data_size, new))
                    operations.append((piece.size_field, 'size', None, len(new)))
        # From the end back, so each change is made where it was found
        for position, kind, old_size, value in sorted(operations, key=lambda o: o[0], reverse=True):
            if kind == 'data':
                data[position:position + old_size] = value
            else:
                struct.pack_into('>I', data, position, value)
        # Keep the header's checksum the way the tag had it, if it had one
        body = bytes(self.data[HEADER_SIZE:])
        stored, plain = self.u32(0x28), zlib.crc32(body)
        if stored == plain:
            struct.pack_into('>I', data, 0x28, zlib.crc32(bytes(data[HEADER_SIZE:])))
        elif stored == plain ^ 0xFFFFFFFF:
            struct.pack_into('>I', data, 0x28, zlib.crc32(bytes(data[HEADER_SIZE:])) ^ 0xFFFFFFFF)
        return bytes(data)


def select(tag, permutation, pitch_range):
    """The (pitch range, first piece index) pairs a --permutation and --pitch-range pick"""
    ranges = tag.pitch_ranges if pitch_range is None else [r for r in tag.pitch_ranges if r.index == pitch_range]
    if pitch_range is not None and not ranges:
        raise SubtitleError(f'{tag.name} has no pitch range {pitch_range} (it has {len(tag.pitch_ranges)})')
    chosen = []
    for r in ranges:
        for p in range(r.actual_count):
            if permutation is None or (permutation.isdigit() and int(permutation) == p) or \
                    (not permutation.isdigit() and r.pieces[p].name.lower() == permutation.lower()):
                chosen.append((r, p))
    if not chosen:
        names = ', '.join(f'{p} ({r.pieces[p].name or "unnamed"})' for r in ranges for p in range(r.actual_count))
        raise SubtitleError(f'{tag.name} has no permutation {permutation}; it has {names or "none"}')
    return chosen


def plan(tag, chosen, text, piece):
    """The pieces to change, as {size field: text or None}"""
    texts = {}
    for r, p in chosen:
        pieces = r.chain(p)
        if piece is None:
            # The whole permutation: the text on its first piece, the rest left to show it
            for i, each in enumerate(pieces):
                texts[each.size_field] = text if i == 0 else None
        elif piece < len(pieces):
            texts[pieces[piece].size_field] = text
        else:
            hint = ' (--pitch-range picks one pitch range)' if len({r.index for r, _ in chosen}) > 1 else ''
            raise SubtitleError(f'{tag.name}: pitch range {r.index}, permutation {p} has {len(pieces)} piece'
                                f'{"" if len(pieces) == 1 else "s"}, so there\'s no piece {piece}{hint}')
    return texts


def show(tag):
    print(f'{tag.name}: {len(tag.pitch_ranges)} pitch range{"" if len(tag.pitch_ranges) == 1 else "s"}')
    for r in tag.pitch_ranges:
        print(f'  pitch range {r.index}{" (" + r.name + ")" if r.name else ""}: {r.actual_count} permutation'
              f'{"" if r.actual_count == 1 else "s"}')
        for p in range(r.actual_count):
            pieces = r.chain(p)
            split = f', {len(pieces)} pieces' if len(pieces) > 1 else ''
            print(f'    {p} {r.pieces[p].name or "(unnamed)"}{split}')
            for i, each in enumerate(pieces):
                label = f'piece {i}: ' if len(pieces) > 1 else ''
                if each.text is not None:
                    print(f'      {label}{each.text!r}')
                elif i == 0:
                    print(f'      {label}no subtitle')


def write(path, data):
    temporary = path.with_name(path.name + '.subtitles-tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)


def edit(args, definitions, text):
    path = pathlib.Path(args.tag)
    tag = SoundTag(path.read_bytes(), definitions, path.name)
    texts = plan(tag, select(tag, args.permutation, args.pitch_range), text, args.piece)
    data = tag.with_texts(texts)

    # Read what's about to be written back, and check only the subtitles changed
    after = SoundTag(data, definitions, path.name)
    for before_range, after_range in zip(tag.pitch_ranges, after.pitch_ranges):
        for before, now in zip(before_range.pieces, after_range.pieces):
            expected = texts[before.size_field] if before.size_field in texts else before.text
            expected = expected or None
            if now.text != expected or now.name != before.name or now.next != before.next:
                raise SubtitleError(f'{path.name}: piece {before.index} didn\'t come out right, so nothing was written')
    if [len(r.pieces) for r in after.pitch_ranges] != [len(r.pieces) for r in tag.pitch_ranges]:
        raise SubtitleError(f'{path.name}: the permutations didn\'t come out right, so nothing was written')

    output = pathlib.Path(args.output) if args.output else path
    write(output, data)
    changed = sum(1 for r, p in select(tag, args.permutation, args.pitch_range))
    what = 'set' if text else 'cleared'
    piece = f', piece {args.piece}' if args.piece is not None else ''
    print(f'{what} {changed} permutation{"" if changed == 1 else "s"}{piece} in {output}')
    show(SoundTag(data, definitions, output.name))


def main(argv=None):
    parser = argparse.ArgumentParser(description='List and set the subtitles buh shows for sounds, in HEK .sound tags.',
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    listing = commands.add_parser('list', help='show each permutation\'s subtitles')
    listing.add_argument('tags', nargs='+', help='.sound tags')
    for name, help_text in (('set', 'set subtitles'), ('clear', 'clear subtitles')):
        command = commands.add_parser(name, help=help_text)
        command.add_argument('tag', help='.sound tag')
        if name == 'set':
            command.add_argument('text', help='the subtitle (|n starts a new line)')
        command.add_argument('--permutation', '-p', help='a permutation, by index or name (all if not given)')
        command.add_argument('--piece', type=int, help='only this piece of a split permutation, 0 being its first')
        command.add_argument('--pitch-range', '-r', type=int, help='only permutations in this pitch range')
        command.add_argument('--output', '-o', help='write the tag here instead of over itself')
    parser.add_argument('--definitions', default=str(pathlib.Path(__file__).parent / 'definitions'), help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    # A console that can't show every character still gets the rest
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='backslashreplace')

    try:
        definitions = Definitions(args.definitions)
        if args.command == 'list':
            for name in args.tags:
                path = pathlib.Path(name)
                show(SoundTag(path.read_bytes(), definitions, path.name))
        elif args.piece is not None and args.piece < 0:
            raise SubtitleError('--piece is 0 or more')
        elif args.command == 'set':
            if not args.text:
                raise SubtitleError('give some text, or use clear')
            edit(args, definitions, args.text)
        else:
            edit(args, definitions, None)
    except (SubtitleError, PackError, OSError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
