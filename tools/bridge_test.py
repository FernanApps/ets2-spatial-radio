#!/usr/bin/env python3
"""
bridge_test.py — Productor de prueba para el bridge de audio externo

Genera un tono de 440 Hz y lo escribe en el mapping de memoria compartida.
Sirve para probar ETS2CabinDSP.dll sin tener el plugin capturador real.

Uso:
  python bridge_test.py [--freq 440] [--gain 0.5] [--duration 30]

Requisitos: Python 3.8+ (usa ctypes, no necesita instalar nada)
"""

import ctypes
import struct
import math
import time
import argparse
import signal
import sys

# Constantes de cabin_bridge.h
CABIN_BRIDGE_NAME = "Local\\ETS2CabinBridge"
CABIN_BRIDGE_MAGIC = 0x31424345  # 'ECB1'
CABIN_BRIDGE_VERSION = 1
CABIN_BRIDGE_CAPACITY = 16384

# Tamaño del struct CabinBridgeBlock
# Header: 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 40 = 80 bytes
# write_pos: 8 + 56 pad = 64 bytes
# read_pos: 8 + 56 pad = 64 bytes
# data: 16384 * 2 * 4 = 131072 bytes
# Total: 80 + 64 + 64 + 131072 = 131280 bytes
CABIN_BRIDGE_SIZE = 131280

# Offsets en el struct
OFF_MAGIC = 0
OFF_VERSION = 4
OFF_SAMPLE_RATE = 8
OFF_CHANNELS = 12
OFF_GAIN = 16
OFF_ENABLED = 20
OFF_SOURCE_PID = 24
OFF_CAPTURING = 28
OFF_PLAYING = 32
OFF_UNDERRUNS = 36
OFF_WRITE_POS = 80
OFF_READ_POS = 144
OFF_DATA = 208

# Windows API
kernel32 = ctypes.windll.kernel32

PAGE_READWRITE = 0x04
FILE_MAP_ALL_ACCESS = 0xF001F
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

running = True

def signal_handler(sig, frame):
    global running
    running = False
    print("\nDeteniendo...")

def main():
    global running

    parser = argparse.ArgumentParser(description="Genera un tono y lo escribe al bridge de audio")
    parser.add_argument("--freq", type=float, default=440.0, help="Frecuencia en Hz (default: 440)")
    parser.add_argument("--gain", type=float, default=0.5, help="Volumen 0.0-1.0 (default: 0.5)")
    parser.add_argument("--duration", type=int, default=30, help="Duración en segundos (default: 30)")
    args = parser.parse_args()

    print(f"Bridge Test: tono {args.freq:.0f} Hz, gain {args.gain:.2f}, duración {args.duration} s")
    print("Presiona Ctrl+C para salir.\n")

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Crear el mapping
    handle = kernel32.CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        None,
        PAGE_READWRITE,
        0,
        CABIN_BRIDGE_SIZE,
        CABIN_BRIDGE_NAME
    )

    if not handle:
        print(f"Error: no se pudo crear el mapping ({kernel32.GetLastError()})")
        return 1

    # Mapear la memoria
    ptr = kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, CABIN_BRIDGE_SIZE)
    if not ptr:
        print(f"Error: no se pudo mapear la memoria ({kernel32.GetLastError()})")
        kernel32.CloseHandle(handle)
        return 1

    # Crear un buffer para acceder a la memoria
    buf = (ctypes.c_char * CABIN_BRIDGE_SIZE).from_address(ptr)

    # Inicializar el header
    pid = kernel32.GetCurrentProcessId()

    # Escribir campos del header
    struct.pack_into("<I", buf, OFF_SAMPLE_RATE, 48000)
    struct.pack_into("<I", buf, OFF_CHANNELS, 2)
    struct.pack_into("<f", buf, OFF_GAIN, args.gain)
    struct.pack_into("<I", buf, OFF_ENABLED, 1)
    struct.pack_into("<I", buf, OFF_SOURCE_PID, pid)
    struct.pack_into("<I", buf, OFF_CAPTURING, 1)
    struct.pack_into("<q", buf, OFF_WRITE_POS, 0)
    struct.pack_into("<q", buf, OFF_READ_POS, 0)

    # Publicar magic y version al final
    ctypes.windll.kernel32.MemoryBarrier()
    struct.pack_into("<I", buf, OFF_MAGIC, CABIN_BRIDGE_MAGIC)
    struct.pack_into("<I", buf, OFF_VERSION, CABIN_BRIDGE_VERSION)

    print(f"Mapping creado: {CABIN_BRIDGE_NAME}")
    print("Esperando que ETS2CabinDSP.dll lo abra...\n")

    # Generar el tono
    sample_rate = 48000.0
    omega = 2.0 * math.pi * args.freq / sample_rate
    phase = 0.0

    start_time = time.time()
    last_status = 0
    total_written = 0
    write_pos = 0

    while running:
        # Leer read_pos del consumidor
        read_pos = struct.unpack_from("<q", buf, OFF_READ_POS)[0]

        # Cuántos frames podemos escribir
        avail = CABIN_BRIDGE_CAPACITY - (write_pos - read_pos)
        if avail <= 0:
            time.sleep(0.005)
            continue

        # Escribir hasta 1024 frames
        to_write = min(avail, 1024)

        for i in range(to_write):
            sample = math.sin(phase)
            idx = ((write_pos + i) % CABIN_BRIDGE_CAPACITY) * 2
            offset = OFF_DATA + idx * 4
            struct.pack_into("<f", buf, offset, sample)      # L
            struct.pack_into("<f", buf, offset + 4, sample)  # R
            phase += omega
            if phase > 2.0 * math.pi:
                phase -= 2.0 * math.pi

        # Publicar write_pos
        ctypes.windll.kernel32.MemoryBarrier()
        write_pos += to_write
        struct.pack_into("<q", buf, OFF_WRITE_POS, write_pos)
        total_written += to_write

        # Status cada segundo
        now = time.time()
        if now - last_status >= 1.0:
            last_status = now
            elapsed = int(now - start_time)
            buffered = write_pos - read_pos
            playing = struct.unpack_from("<I", buf, OFF_PLAYING)[0]
            underruns = struct.unpack_from("<I", buf, OFF_UNDERRUNS)[0]

            print(f"  [{elapsed:3d} s] escrito: {total_written} frames, "
                  f"buffer: {buffered}, playing: {playing}, underruns: {underruns}")

            if args.duration > 0 and elapsed >= args.duration:
                print("\nDuración alcanzada.")
                break

        time.sleep(0.01)

    # Limpiar
    struct.pack_into("<I", buf, OFF_CAPTURING, 0)
    struct.pack_into("<I", buf, OFF_ENABLED, 0)
    ctypes.windll.kernel32.MemoryBarrier()

    print("\nCerrando...")
    time.sleep(0.1)

    kernel32.UnmapViewOfFile(ptr)
    kernel32.CloseHandle(handle)

    print("Bridge cerrado.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
