#!/usr/bin/env python3
"""
set_volume.py — Cambia el volumen del audio externo en runtime

Uso:
  python set_volume.py 0.5    # volumen al 50%
  python set_volume.py 1.0    # volumen normal
  python set_volume.py 0.0    # silencio
"""

import ctypes
import ctypes.wintypes
import struct
import sys

CABIN_BRIDGE_NAME = "Local\\ETS2CabinBridge"
CABIN_BRIDGE_SIZE = 131280
FILE_MAP_ALL_ACCESS = 0xF001F
OFF_GAIN = 16

def main():
    if len(sys.argv) < 2:
        print("Uso: python set_volume.py <0.0-2.0>")
        return 1

    volume = float(sys.argv[1])

    kernel32 = ctypes.windll.kernel32
    kernel32.OpenFileMappingW.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.wintypes.LPCWSTR]
    kernel32.OpenFileMappingW.restype = ctypes.wintypes.HANDLE
    kernel32.MapViewOfFile.argtypes = [ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD,
                                        ctypes.wintypes.DWORD, ctypes.wintypes.DWORD, ctypes.c_size_t]
    kernel32.MapViewOfFile.restype = ctypes.c_void_p
    kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.wintypes.HANDLE]

    handle = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, CABIN_BRIDGE_NAME)
    if not handle:
        print("Error: ETS2 no está corriendo")
        return 1

    ptr = kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, CABIN_BRIDGE_SIZE)
    buf = (ctypes.c_char * CABIN_BRIDGE_SIZE).from_address(ptr)

    old = struct.unpack_from("<f", buf, OFF_GAIN)[0]
    struct.pack_into("<f", buf, OFF_GAIN, volume)
    print(f"Volumen: {old:.2f} -> {volume:.2f}")

    kernel32.UnmapViewOfFile(ptr)
    kernel32.CloseHandle(handle)
    return 0

if __name__ == "__main__":
    sys.exit(main())
