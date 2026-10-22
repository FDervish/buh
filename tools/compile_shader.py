"""Compile a pixel shader into a C header, with the d3dcompiler_47.dll Windows has.

    python tools/compile_shader.py src/loading/loading_screen_shader.hlsl LoadScreenPS loading_screen_shader

writes src/loading/loading_screen_shader.h next to the source, holding the ps_3_0 bytecode as a DWORD array.
"""
import ctypes
import pathlib
import struct
import sys

D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15


def blob_bytes(blob):
    # ID3DBlob: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
    table = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(table[4])(blob)
    data = ctypes.string_at(pointer, size)
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(table[2])(blob)
    return data


def compile_shader(source, entry, target):
    compiler = ctypes.WinDLL('d3dcompiler_47.dll')
    code, errors = ctypes.c_void_p(), ctypes.c_void_p()
    text = source.read_bytes()
    result = compiler.D3DCompile(text, len(text), str(source.name).encode(), None, None, entry.encode(), target.encode(),
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ctypes.byref(code), ctypes.byref(errors))
    messages = blob_bytes(errors).decode(errors='replace') if errors.value else ''
    if result < 0 or not code.value:
        raise SystemExit(f'{source.name} did not compile ({result & 0xFFFFFFFF:#x}):\n{messages}')
    if messages:
        print(messages, file=sys.stderr)
    return blob_bytes(code)


def main():
    source, entry, name = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    bytecode = compile_shader(source, entry, 'ps_3_0')
    words = struct.unpack(f'<{len(bytecode) // 4}I', bytecode)
    lines = [', '.join(f'0x{w:08X}' for w in words[i:i + 8]) for i in range(0, len(words), 8)]
    header = source.with_suffix('.h')
    guard = f'BUH_{name.upper()}_H'
    with open(header, 'w', newline='\n') as out:
        out.write(f'// SPDX-License-Identifier: GPL-3.0-only\n//\n// {source.name} ({entry}, ps_3_0), compiled by '
                  f'tools/compile_shader.py. Edit the source and run that again instead.\n\n'
                  f'#ifndef {guard}\n#define {guard}\n\n#include <windows.h>\n\n'
                  f'static const DWORD {name}[] = {{\n    ' + ',\n    '.join(lines) + '\n};\n\n#endif\n')
    print(f'{header}: {len(bytecode)} bytes')


if __name__ == '__main__':
    main()
