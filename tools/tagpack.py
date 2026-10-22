#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
Make a buh tag pack out of a Halo Custom Edition map.

A tag pack is a set of tags (and everything they reference) that buh adds to whatever map is loaded, so they can be
spawned and used without rebuilding that map. This does the hard part once, here: it copies the tags out of the map
into one block, resolves anything the map left in bitmaps.map, sounds.map or loc.map, and lists every place in the
block that needs fixing up when buh drops it into memory (pointers, tag IDs, and offsets into raw and model data).
buh then only has to copy the block and apply that list.

    tagpack.py armory.map -o armory.tagpack                 every tag the scenario's object palettes use
    tagpack.py bloodgulch.map -o hog.tagpack --tag vehicles\\warthog\\mp_warthog
    tagpack.py armory.tagpack --info

Tag layouts come from Invader's tag definitions in definitions/.
"""

import argparse
import json
import pathlib
import struct
import sys

TAG_DATA_ADDRESS = 0x40440000
NULL_ID = 0xFFFFFFFF
PACK_MAGIC = b'BUHP'
PACK_VERSION = 1
PACK_HEADER = struct.Struct('<4sI2I5I3I5I32s')
PACK_TAG = struct.Struct('<5I')

# Invader's class names and Halo's four character codes
CLASS_CODES = {
    'actor': 'actr', 'actor_variant': 'actv', 'antenna': 'ant!', 'biped': 'bipd', 'bitmap': 'bitm',
    'camera_track': 'trak', 'color_table': 'colo', 'continuous_damage_effect': 'cdmg', 'contrail': 'cont',
    'damage_effect': 'jpt!', 'decal': 'deca', 'detail_object_collection': 'dobc', 'device': 'devi',
    'device_control': 'ctrl', 'device_light_fixture': 'lifi', 'device_machine': 'mach', 'dialogue': 'udlg',
    'effect': 'effe', 'equipment': 'eqip', 'flag': 'flag', 'fog': 'fog ', 'font': 'font', 'garbage': 'garb',
    'gbxmodel': 'mod2', 'globals': 'matg', 'glow': 'glw!', 'grenade_hud_interface': 'grhi', 'hud_globals': 'hudg',
    'hud_message_text': 'hmt ', 'hud_number': 'hud#', 'input_device_defaults': 'devc', 'item': 'item',
    'item_collection': 'itmc', 'lens_flare': 'lens', 'light': 'ligh', 'light_volume': 'mgs2', 'lightning': 'elec',
    'material_effects': 'foot', 'meter': 'metr', 'model': 'mode', 'model_animations': 'antr',
    'model_collision_geometry': 'coll', 'multiplayer_scenario_description': 'mply', 'object': 'obje',
    'particle': 'part', 'particle_system': 'pctl', 'physics': 'phys', 'placeholder': 'plac',
    'point_physics': 'pphy', 'preferences_network_game': 'ngpr', 'projectile': 'proj', 'scenario': 'scnr',
    'scenario_structure_bsp': 'sbsp', 'scenery': 'scen', 'shader': 'shdr', 'shader_environment': 'senv',
    'shader_model': 'soso', 'shader_transparent_chicago': 'schi', 'shader_transparent_chicago_extended': 'scex',
    'shader_transparent_generic': 'sotr', 'shader_transparent_glass': 'sgla', 'shader_transparent_meter': 'smet',
    'shader_transparent_plasma': 'spla', 'shader_transparent_water': 'swat', 'sky': 'sky ', 'sound': 'snd!',
    'sound_environment': 'snde', 'sound_looping': 'lsnd', 'sound_scenery': 'ssce', 'string_list': 'str#',
    'tag_collection': 'tagc', 'ui_widget_definition': 'DeLa', 'unicode_string_list': 'ustr', 'unit': 'unit',
    'unit_hud_interface': 'unhi', 'vehicle': 'vehi', 'virtual_keyboard': 'vcky', 'weapon': 'weap',
    'weapon_hud_interface': 'wphi', 'weather_particle_system': 'rain', 'wind': 'wind',
}

# These belong to the map a pack is loaded into, so references to them are dropped
EXCLUDED_CLASSES = {'scnr', 'sbsp', 'matg', 'hudg', 'metr'}

# Sizes of the types Invader's definitions use that aren't structs, enums or bitfields
TYPE_SIZES = {
    'int8': 1, 'uint8': 1, 'int16': 2, 'uint16': 2, 'int32': 4, 'uint32': 4, 'float': 4, 'Angle': 4, 'Fraction': 4,
    'Index': 2, 'TagString': 32, 'TagFourCC': 4, 'TagID': 4, 'Pointer': 4, 'ColorARGBInt': 4, 'ColorRGB': 12,
    'ColorARGB': 16, 'Point2D': 8, 'Point2DInt': 4, 'Point3D': 12, 'Vector2D': 8, 'Vector3D': 12, 'Euler2D': 8,
    'Euler3D': 12, 'Rectangle2D': 8, 'Quaternion': 16, 'Matrix': 36, 'Plane2D': 12, 'Plane3D': 16,
    'ScenarioScriptNodeValue': 4, 'TagReflexive': 12, 'TagDependency': 16, 'TagDataOffset': 20,
}

# Fields that need more than copying
SPECIAL_FIELDS = {
    ('BitmapData', 'pixel data offset'): 'bitmap pixels',
    ('BitmapData', 'bitmap tag id'): 'self',
    ('BitmapData', 'texture_cache_id'): 'null',
    ('SoundPermutation', 'unknown tag id'): 'null',
    ('SoundPermutation', 'samples pointer'): 'zero',
    ('SoundPermutation', 'sound tag id 0'): 'self',
    ('SoundPermutation', 'sound tag id 1'): 'self',
    ('ModelGeometryPart', 'vertex offset'): 'model vertices',
    ('ModelGeometryPart', 'triangle offset'): 'model indices',
    ('ModelGeometryPart', 'triangle offset 2'): 'model indices',
}

BITMAP_DATA_EXTERNAL = 0x100
DATA_OFFSET_EXTERNAL = 0x1

# Bytes per vertex by vertex type, from Halo's own table
VERTEX_STRIDES = [56, 32, 20, 8, 68, 32, 24, 36, 24, 16, 16, 20, 32, 8, 32, 32]


class PackError(Exception):
    pass


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def put32(data, offset, value):
    struct.pack_into('<I', data, offset, value & 0xFFFFFFFF)


def c_string(data, offset):
    end = data.find(b'\0', offset)
    if end < 0:
        raise PackError(f'unterminated string at {offset:#x}')
    return bytes(data[offset:end])


class Field:
    __slots__ = ('offset', 'name', 'type', 'info', 'owner')

    def __init__(self, offset, name, type_, info, owner):
        self.offset, self.name, self.type, self.info, self.owner = offset, name, type_, info, owner


class Definitions:
    """Invader's tag definitions, flattened to every leaf field's offset in each struct"""

    def __init__(self, folder):
        self.entries = {}
        for path in sorted(pathlib.Path(folder).glob('*.json')):
            for entry in json.loads(path.read_text(encoding='utf-8')):
                self.entries[entry['name']] = entry
        self.flat = {}
        self.class_struct = {}
        self.class_ancestors = {}
        for entry in self.entries.values():
            if entry.get('type') == 'struct' and 'class' in entry:
                self.class_struct[CLASS_CODES[entry['class']]] = entry['name']
        # A class's ancestors, to check what a dependency allows (e.g. "object" takes any object)
        for code, name in self.class_struct.items():
            chain, entry = [code], self.entries[name]
            while entry.get('inherits'):
                entry = self.entries[entry['inherits']]
                if 'class' in entry:
                    chain.append(CLASS_CODES[entry['class']])
            self.class_ancestors[code] = chain

    def size_of(self, type_, field=None):
        if type_ == 'pad':
            return field['size']
        if type_ in TYPE_SIZES:
            return TYPE_SIZES[type_]
        entry = self.entries.get(type_)
        if entry is None:
            raise PackError(f'unknown type {type_}')
        if entry['type'] == 'enum':
            return 2
        if entry['type'] == 'bitfield':
            return entry['width'] // 8
        return self.struct_size(type_)

    def struct_size(self, name):
        return self.entries[name]['size']

    def fields(self, name):
        """Leaf fields of a struct, inherited ones first, with arrays and inline structs expanded"""
        if name in self.flat:
            return self.flat[name]
        entry = self.entries[name]
        result = []
        offset = 0
        if entry.get('inherits'):
            result.extend(self.fields(entry['inherits']))
            offset = self.struct_size(entry['inherits'])
        for field in entry['fields']:
            type_ = field['type']
            # Bounds are a pair of values (from, to)
            count = field.get('count', 1) * (2 if field.get('bounds') else 1)
            size = self.size_of(type_, field)
            for i in range(count):
                at = offset + i * size
                inner = self.entries.get(type_)
                if inner is not None and inner.get('type') == 'struct':
                    for leaf in self.fields(type_):
                        result.append(Field(at + leaf.offset, leaf.name, leaf.type, leaf.info, leaf.owner))
                elif type_ != 'pad':
                    result.append(Field(at, field.get('name'), type_, field, name))
            offset += size * count
        if offset != entry['size']:
            raise PackError(f'{name} adds up to {offset:#x} bytes, but its definition says {entry["size"]:#x}')
        self.flat[name] = result
        return result

    def allows(self, dependency_field, target_class):
        allowed = {CLASS_CODES[c] for c in dependency_field.info.get('classes', [])}
        # Custom Edition uses gbxmodels where the definitions say model
        if 'mode' in allowed:
            allowed.add('mod2')
        return not allowed or bool(allowed & set(self.class_ancestors.get(target_class, [target_class])))

    def offset_of(self, struct_name, field_name):
        for field in self.fields(struct_name):
            if field.name == field_name:
                return field.offset
        raise PackError(f'{struct_name} has no {field_name}')


class Space:
    """Where a tag's structures are read from: the map's tag data (absolute pointers) and, for tags the map left in
    a resource map, that resource's data (pointers relative to rel_base within it)"""

    def __init__(self, tag_data, buffer=None, rel_base=0):
        self.tag_data, self.buffer, self.rel_base = tag_data, buffer, rel_base

    def locate(self, pointer, size):
        if TAG_DATA_ADDRESS <= pointer and pointer + size <= TAG_DATA_ADDRESS + len(self.tag_data):
            return self.tag_data, pointer - TAG_DATA_ADDRESS
        if self.buffer is not None and 0 <= pointer + self.rel_base and pointer + self.rel_base + size <= len(self.buffer):
            return self.buffer, pointer + self.rel_base
        raise PackError(f'pointer {pointer:#x} ({size:#x} bytes) is outside the tag data')

    def read(self, pointer, size):
        data, offset = self.locate(pointer, size)
        return data[offset:offset + size]

    def string(self, pointer):
        data, offset = self.locate(pointer, 1)
        return c_string(data, offset)


class ResourceMap:
    def __init__(self, path):
        self.path = path
        self.data = pathlib.Path(path).read_bytes()
        _, paths, resources, count = struct.unpack_from('<4I', self.data, 0)
        self.entries = []
        for i in range(count):
            path_offset, size, offset = struct.unpack_from('<3I', self.data, resources + i * 12)
            name = c_string(self.data, paths + path_offset).decode('latin1')
            self.entries.append((name, size, offset))

    def resource(self, index):
        if index >= len(self.entries):
            raise PackError(f'{pathlib.Path(self.path).name} has no resource {index}')
        _, size, offset = self.entries[index]
        return self.data[offset:offset + size]

    def find(self, name, odd_only):
        for i, entry in enumerate(self.entries):
            if entry[0] == name and (not odd_only or i % 2 == 1):
                return i
        return None


class CacheMap:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.data = self.path.read_bytes()
        head, version, _, _, tag_offset, tag_size = struct.unpack_from('<6I', self.data, 0)
        if head != 0x68656164 or u32(self.data, 0x7FC) != 0x666F6F74:
            raise PackError(f'{self.path.name} is not a Halo cache file')
        if version != 609:
            raise PackError(f'{self.path.name} is not an uncompressed Custom Edition map (version {version})')
        self.name = c_string(self.data, 0x20).decode('latin1')
        self.tag_data = self.data[tag_offset:tag_offset + tag_size]
        (array, self.scenario_id, _, count, _, self.model_offset, _, self.vertex_size,
         self.model_size) = struct.unpack_from('<9I', self.tag_data, 0)
        self.tags = []
        for i in range(count):
            primary, secondary, tertiary, tag_id, path, data, indexed, _ = struct.unpack_from('<8I', self.tag_data, array - TAG_DATA_ADDRESS + i * 32)
            name = Space(self.tag_data).string(path).decode('latin1')
            self.tags.append({'classes': (primary, secondary, tertiary), 'class': struct.pack('>I', primary).decode('latin1'),
                              'id': tag_id, 'path': name, 'data': data, 'indexed': indexed, 'index': i})

    def tag_by_id(self, tag_id):
        index = tag_id & 0xFFFF
        if tag_id == NULL_ID or index >= len(self.tags) or self.tags[index]['id'] != tag_id:
            return None
        return self.tags[index]


class PackBuilder:
    def __init__(self, definitions, cache_map, maps_folder):
        self.defs, self.map, self.maps_folder = definitions, cache_map, pathlib.Path(maps_folder)
        self.blob = bytearray()
        self.strings = {}
        self.pointers, self.tag_ids, self.file_offsets, self.model_vertices, self.model_indices = [], [], [], [], []
        self.raw = bytearray()
        self.raw_copies = {}
        self.vertices, self.indices = bytearray(), bytearray()
        self.vertex_copies, self.index_copies = {}, {}
        self.part_offsets = {name: definitions.offset_of('ModelGeometryPart', name) for name in (
            'do not crash the game', 'triangle count', 'triangle offset', 'triangle offset 2', 'vertex type',
            'vertex count', 'vertex pointer', 'vertex offset')}
        self.order = []
        self.local = {}
        self.entries = []
        self.warnings = []
        self.resource_maps = {}

    def warn(self, message):
        if message not in self.warnings:
            self.warnings.append(message)

    def resource_map(self, name):
        if name not in self.resource_maps:
            path = self.maps_folder / name
            if not path.exists():
                raise PackError(f'{name} is needed for tags the map keeps in it, but it isn\'t in {self.maps_folder}')
            self.resource_maps[name] = ResourceMap(path)
        return self.resource_maps[name]

    def alloc(self, size):
        while len(self.blob) % 4:
            self.blob.append(0)
        offset = len(self.blob)
        self.blob.extend(bytes(size))
        return offset

    def string(self, text):
        if text not in self.strings:
            offset = self.alloc(len(text) + 1)
            self.blob[offset:offset + len(text)] = text
            self.strings[text] = offset
        return self.strings[text]

    def raw_copy(self, file_data, offset, size):
        key = (id(file_data), offset, size)
        if key not in self.raw_copies:
            if offset + size > len(file_data):
                raise PackError(f'raw data at {offset:#x} ({size:#x} bytes) is past the end of its file')
            while len(self.raw) % 16:
                self.raw.append(0)
            self.raw_copies[key] = len(self.raw)
            self.raw.extend(file_data[offset:offset + size])
        return self.raw_copies[key]

    def reference(self, tag_id):
        """The local index of a referenced tag, queueing it, or None if it's dropped"""
        tag = self.map.tag_by_id(tag_id)
        if tag is None:
            return None
        if tag['class'] in EXCLUDED_CLASSES:
            self.warn(f'dropped a reference to {tag["path"]}.{tag["class"]}, which belongs to the map')
            return None
        if tag['index'] not in self.local:
            self.local[tag['index']] = len(self.order)
            self.order.append(tag)
        return self.local[tag['index']]

    def emit_array(self, struct_name, space, pointer, count, context):
        size = self.defs.struct_size(struct_name)
        if count > 0x100000:
            raise PackError(f'a {struct_name} block claims {count} elements')
        source = space.read(pointer, size * count)
        out = self.alloc(size * count)
        self.blob[out:out + len(source)] = source
        for i in range(count):
            self.fix_struct(struct_name, space, out + i * size, context)
        return out

    def fix_struct(self, struct_name, space, out, context):
        for field in self.defs.fields(struct_name):
            at = out + field.offset
            special = SPECIAL_FIELDS.get((field.owner, field.name))
            if field.type == 'TagReflexive':
                count, pointer = u32(self.blob, at), u32(self.blob, at + 4)
                if count == 0:
                    put32(self.blob, at + 4, 0)
                    continue
                child = self.emit_array(field.info['struct'], space, pointer, count, context)
                put32(self.blob, at + 4, child)
                self.pointers.append(at + 4)
            elif field.type == 'TagDependency':
                pointer, tag_id = u32(self.blob, at + 4), u32(self.blob, at + 12)
                if pointer:
                    put32(self.blob, at + 4, self.string(space.string(pointer)))
                    self.pointers.append(at + 4)
                self.tag_id_field(at + 12, tag_id, field)
            elif field.type == 'TagDataOffset':
                size, external, file_offset, pointer = struct.unpack_from('<4I', self.blob, at)
                if field.info.get('non_cached'):
                    # Editing data that maps don't carry
                    self.blob[at:at + 20] = bytes(20)
                    continue
                if size == 0:
                    continue
                if field.info.get('file_offset'):
                    # Sound samples: in the map file, or in sounds.map when external
                    source = self.resource_map('sounds.map').data if external & DATA_OFFSET_EXTERNAL else self.map.data
                    put32(self.blob, at + 4, external & ~DATA_OFFSET_EXTERNAL)
                    put32(self.blob, at + 8, self.raw_copy(source, file_offset, size))
                    put32(self.blob, at + 12, 0)
                    self.file_offsets.append(at + 8)
                else:
                    child = self.alloc(size)
                    self.blob[child:child + size] = space.read(pointer, size)
                    put32(self.blob, at + 12, child)
                    self.pointers.append(at + 12)
            elif special == 'bitmap pixels':
                flags = struct.unpack_from('<H', self.blob, out + 0xE)[0]
                size = u32(self.blob, at + 4)
                source = self.resource_map('bitmaps.map').data if flags & BITMAP_DATA_EXTERNAL else self.map.data
                struct.pack_into('<H', self.blob, out + 0xE, flags & ~BITMAP_DATA_EXTERNAL)
                put32(self.blob, at, self.raw_copy(source, u32(self.blob, at), size))
                self.file_offsets.append(at)
            elif special == 'model vertices':
                self.model_part(out)
            elif special == 'model indices':
                pass
            elif special == 'self':
                self.tag_ids.append((at, context['local']))
            elif special == 'null':
                put32(self.blob, at, NULL_ID)
            elif special == 'zero' or field.type == 'Pointer':
                put32(self.blob, at, 0)
            elif field.type == 'TagID':
                self.tag_id_field(at, u32(self.blob, at), field)

    def model_part(self, out):
        """Copy a model part's vertices and indices out of the map's model data, and point the part at the copies"""
        offsets = self.part_offsets
        index_type = struct.unpack_from('<H', self.blob, out + offsets['do not crash the game'])[0]
        triangle_count = u32(self.blob, out + offsets['triangle count'])
        triangle_offset, triangle_offset_2 = u32(self.blob, out + offsets['triangle offset']), u32(self.blob, out + offsets['triangle offset 2'])
        vertex_type = struct.unpack_from('<H', self.blob, out + offsets['vertex type'])[0]
        vertex_count, vertex_offset = u32(self.blob, out + offsets['vertex count']), u32(self.blob, out + offsets['vertex offset'])
        if vertex_type >= len(VERTEX_STRIDES):
            raise PackError(f'a model part has vertex type {vertex_type}')
        # Halo makes an index buffer of triangle_count * 3 indices, or triangle_count + 2 for a strip
        index_bytes = triangle_count * 6 if index_type == 0 else (triangle_count + 2) * 2 if index_type == 1 else 0
        vertex_bytes = vertex_count * VERTEX_STRIDES[vertex_type]
        model = self.map.data[self.map.model_offset:self.map.model_offset + self.map.model_size]
        indices_at = self.map.vertex_size + triangle_offset_2
        if vertex_offset + vertex_bytes > self.map.vertex_size or indices_at + index_bytes > self.map.model_size:
            raise PackError('a model part points past the end of the map\'s model data')
        new_vertices = self.model_copy(self.vertices, self.vertex_copies, model, vertex_offset, vertex_bytes)
        new_indices = self.model_copy(self.indices, self.index_copies, model, indices_at, index_bytes)
        put32(self.blob, out + offsets['vertex offset'], new_vertices)
        put32(self.blob, out + offsets['triangle offset 2'], new_indices)
        put32(self.blob, out + offsets['triangle offset'], triangle_offset - triangle_offset_2 + new_indices)
        put32(self.blob, out + offsets['vertex pointer'], 0)
        self.model_vertices.append(out + offsets['vertex offset'])
        self.model_indices.append(out + offsets['triangle offset'])
        self.model_indices.append(out + offsets['triangle offset 2'])

    @staticmethod
    def model_copy(block, copies, model, offset, size):
        key = (offset, size)
        if key not in copies:
            while len(block) % 4:
                block.append(0)
            copies[key] = len(block)
            block.extend(model[offset:offset + size])
        return copies[key]

    def tag_id_field(self, at, tag_id, field):
        if tag_id == NULL_ID:
            return
        local = self.reference(tag_id)
        if local is None:
            put32(self.blob, at, NULL_ID)
            return
        if field.type == 'TagDependency' and not self.defs.allows(field, self.order[local]['class']):
            self.warn(f'{self.order[local]["path"]}.{self.order[local]["class"]} is referenced where {field.info.get("classes")} is expected')
        self.tag_ids.append((at, local))

    def tag_source(self, tag):
        """The space a tag's structures come from, and its root pointer in it"""
        tag_data = self.map.tag_data
        if not tag['indexed']:
            return Space(tag_data), tag['data']
        if tag['class'] == 'bitm':
            meta = self.resource_map('bitmaps.map').resource(tag['data'])
            return Space(tag_data, meta, 0), 0
        if tag['class'] == 'snd!':
            sounds = self.resource_map('sounds.map')
            index = sounds.find(tag['path'], odd_only=True)
            if index is None:
                raise PackError(f'{tag["path"]} isn\'t in sounds.map')
            meta = bytearray(sounds.resource(index))
            # The map has the sound's own struct; sounds.map has its pitch ranges right after a copy of it, with
            # pointers relative to them. Take the format fields and pitch ranges from sounds.map like Chimera does.
            base = bytearray(Space(tag_data).read(tag['data'], 0xA4))
            for offset, size in ((0x06, 2), (0x6C, 2), (0x6E, 2), (0x84, 4)):
                base[offset:offset + size] = meta[offset:offset + size]
            put32(base, 0x98, u32(meta, 0x98))
            put32(base, 0x9C, 0)
            meta[0:0xA4] = base
            return Space(tag_data, meta, 0xA4), -0xA4
        # loc.map: fonts, unicode string lists, HUD message text
        meta = self.resource_map('loc.map').resource(tag['data'])
        return Space(tag_data, meta, 0), 0

    def build(self, roots):
        for tag_id in roots:
            self.reference(tag_id)
        done = 0
        while done < len(self.order):
            tag = self.order[done]
            local = done
            done += 1
            struct_name = self.defs.class_struct.get(tag['class'])
            if struct_name is None:
                raise PackError(f'{tag["path"]}.{tag["class"]}: no definition for that class')
            space, root = self.tag_source(tag)
            data = self.emit_array(struct_name, space, root, 1, {'local': local})
            path = self.string(tag['path'].encode('latin1'))
            self.entries.append((tag['classes'], path, data))
        # The parts' vertices, then their indices, like a map's model data
        while len(self.vertices) % 4:
            self.vertices.append(0)
        self.model = bytes(self.vertices + self.indices)
        self.vertex_size, self.index_size = len(self.vertices), len(self.indices)

    def write(self, path, name):
        tags = b''.join(PACK_TAG.pack(*classes, path_offset, data_offset) for classes, path_offset, data_offset in self.entries)
        relocations = b''.join([
            struct.pack(f'<{len(self.pointers)}I', *self.pointers),
            b''.join(struct.pack('<2I', at, local) for at, local in self.tag_ids),
            struct.pack(f'<{len(self.file_offsets)}I', *self.file_offsets),
            struct.pack(f'<{len(self.model_vertices)}I', *self.model_vertices),
            struct.pack(f'<{len(self.model_indices)}I', *self.model_indices),
        ])
        sections = [tags, bytes(self.blob), relocations, bytes(self.raw), self.model]
        offsets, at = [], (PACK_HEADER.size + 15) // 16 * 16
        for section in sections:
            offsets.append(at)
            at = (at + len(section) + 15) // 16 * 16
        header = PACK_HEADER.pack(PACK_MAGIC, PACK_VERSION, len(self.entries), len(self.blob), len(self.pointers),
                                  len(self.tag_ids), len(self.file_offsets), len(self.model_vertices), len(self.model_indices),
                                  len(self.raw), self.vertex_size, self.index_size, *offsets, name.encode('latin1')[:31])
        out = bytearray(at)
        out[0:len(header)] = header
        for offset, section in zip(offsets, sections):
            out[offset:offset + len(section)] = section
        pathlib.Path(path).write_bytes(out)


class Pack:
    """A tag pack read back"""

    def __init__(self, path):
        data = pathlib.Path(path).read_bytes()
        fields = PACK_HEADER.unpack_from(data, 0)
        (magic, version, self.tag_count, self.tag_data_size, pointers, tag_ids, file_offsets, model_vertices,
         model_indices, self.raw_size, self.vertex_size, self.index_size, tags_at, data_at, relocations_at, raw_at,
         model_at, name) = fields
        if magic != PACK_MAGIC or version != PACK_VERSION:
            raise PackError(f'{path} is not a version {PACK_VERSION} tag pack')
        self.name = name.rstrip(b'\0').decode('latin1')
        self.tags = [PACK_TAG.unpack_from(data, tags_at + i * PACK_TAG.size) for i in range(self.tag_count)]
        self.tag_data = data[data_at:data_at + self.tag_data_size]
        at = relocations_at
        self.pointers = list(struct.unpack_from(f'<{pointers}I', data, at)); at += pointers * 4
        pairs = struct.unpack_from(f'<{tag_ids * 2}I', data, at); at += tag_ids * 8
        self.tag_ids = list(zip(pairs[0::2], pairs[1::2]))
        self.file_offsets = list(struct.unpack_from(f'<{file_offsets}I', data, at)); at += file_offsets * 4
        self.model_vertices = list(struct.unpack_from(f'<{model_vertices}I', data, at)); at += model_vertices * 4
        self.model_indices = list(struct.unpack_from(f'<{model_indices}I', data, at))
        self.raw = data[raw_at:raw_at + self.raw_size]
        self.model = data[model_at:model_at + self.vertex_size + self.index_size]

    def path(self, i):
        return c_string(self.tag_data, self.tags[i][3]).decode('latin1')

    def tag_class(self, i):
        return struct.pack('>I', self.tags[i][0]).decode('latin1')


def palette_roots(cache_map, definitions):
    """Every tag the scenario's palettes use"""
    scenario = cache_map.tag_by_id(cache_map.scenario_id)
    if scenario is None:
        raise PackError('the map has no scenario')
    space = Space(cache_map.tag_data)
    roots = []
    for field in definitions.fields('Scenario'):
        if field.type == 'TagReflexive' and field.name and field.name.endswith('palette'):
            count, pointer = struct.unpack_from('<2I', space.read(scenario['data'] + field.offset, 8))
            element = definitions.struct_size(field.info['struct'])
            for i in range(count):
                for leaf in definitions.fields(field.info['struct']):
                    if leaf.type == 'TagDependency':
                        tag_id = u32(space.read(pointer + i * element + leaf.offset + 12, 4), 0)
                        if tag_id != NULL_ID:
                            roots.append(tag_id)
    return roots


def main(argv=None):
    parser = argparse.ArgumentParser(description='Make a buh tag pack out of a Halo Custom Edition map.')
    parser.add_argument('input', help='the map to take tags from, or a tag pack with --info')
    parser.add_argument('-o', '--output', help='the tag pack to write (default: the map\'s name with .tagpack)')
    parser.add_argument('--tag', action='append', default=[], help='take this tag (by path, any class) and what it uses '
                        'instead of the scenario\'s palettes; can be given more than once')
    parser.add_argument('--maps', help='where bitmaps.map, sounds.map and loc.map are (default: next to the map)')
    parser.add_argument('--info', action='store_true', help='list what a tag pack holds')
    parser.add_argument('--definitions', default=str(pathlib.Path(__file__).parent / 'definitions'), help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    try:
        if args.info:
            pack = Pack(args.input)
            print(f'{pack.name}: {pack.tag_count} tags, {pack.tag_data_size / 2**20:.2f} MiB of tag data, '
                  f'{pack.raw_size / 2**20:.2f} MiB of bitmaps and sounds, {(pack.vertex_size + pack.index_size) / 2**20:.2f} MiB of models')
            for i in range(pack.tag_count):
                print(f'  {pack.path(i)}.{pack.tag_class(i).strip()}')
            return 0

        definitions = Definitions(args.definitions)
        cache_map = CacheMap(args.input)
        if args.tag:
            roots = []
            for wanted in args.tag:
                found = [tag['id'] for tag in cache_map.tags if tag['path'].lower() == wanted.replace('/', '\\').lower()]
                if not found:
                    raise PackError(f'{wanted} isn\'t in {cache_map.path.name}')
                roots.extend(found)
        else:
            roots = palette_roots(cache_map, definitions)
            if not roots:
                raise PackError('the scenario\'s palettes are empty, so there is nothing to take (or use --tag)')
        builder = PackBuilder(definitions, cache_map, args.maps or cache_map.path.parent)
        builder.build(roots)
        output = args.output or cache_map.path.with_suffix('.tagpack')
        builder.write(output, cache_map.name)
        for warning in builder.warnings:
            print(f'warning: {warning}', file=sys.stderr)
        classes = {}
        for tag in builder.order:
            classes[tag['class'].strip()] = classes.get(tag['class'].strip(), 0) + 1
        print(f'{output}: {len(builder.order)} tags ({", ".join(f"{n} {c}" for c, n in sorted(classes.items()))}), '
              f'{len(builder.blob) / 2**20:.2f} MiB of tag data, {len(builder.raw) / 2**20:.2f} MiB of bitmaps and sounds, '
              f'{len(builder.model) / 2**20:.2f} MiB of models')
        return 0
    except PackError as error:
        print(f'error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
