#!/usr/bin/env python3
"""
bridge_test.py — Productor de prueba para el bridge de audio externo

Genera un tono de 440 Hz y lo escribe en el mapping de memoria compartida.
Sirve para probar ETS2CabinDSP.dll sin tener el plugin capturador real.

Uso:
  python bridge_test.py [--freq 440] [--gain 0.5] [--duration 30]
  python bridge_test.py --radio-vol 0.3     # bajar volumen de la emisora al 30%
  python bridge_test.py --mute-radio        # silenciar la emisora del juego
  python bridge_test.py --duck 0.2          # atenuar emisora al 20% cuando hay audio

Requisitos: Python 3.8+ (usa ctypes, no necesita instalar nada)
"""

import ctypes
import ctypes.wintypes
import struct
import math
import time
import argparse
import signal
import sys

# Constantes de cabin_bridge.h (VERSION 2)
CABIN_BRIDGE_NAME = "Local\\ETS2CabinBridge"
CABIN_BRIDGE_MAGIC = 0x31424345  # 'ECB1'
CABIN_BRIDGE_VERSION = 2
CABIN_BRIDGE_CAPACITY = 16384

# Tamaño del struct CabinBridgeBlock v2
# Header: 4*4 = 16 bytes
# Config: 4*4 = 16 bytes
# Control de mezcla: 4*4 = 16 bytes
# Read-back: 4*4 = 16 bytes
# Padding: 16 bytes
# write_pos: 8 + 56 pad = 64 bytes
# read_pos: 8 + 56 pad = 64 bytes
# data: 16384 * 2 * 4 = 131072 bytes
# Total: 80 + 64 + 64 + 131072 = 131280 bytes
CABIN_BRIDGE_SIZE = 131280

# Offsets en el struct (version 2)
OFF_MAGIC = 0
OFF_VERSION = 4
OFF_SAMPLE_RATE = 8
OFF_CHANNELS = 12
OFF_GAIN = 16
OFF_ENABLED = 20
OFF_SOURCE_PID = 24
OFF_CAPTURING = 28
# Nuevos campos de control (version 2)
OFF_RADIO_VOLUME = 32
OFF_MUTE_GAME_RADIO = 36
OFF_DUCK_FACTOR = 40
OFF_RESERVED1 = 44
# Read-back
OFF_PLAYING = 48
OFF_UNDERRUNS = 52
OFF_ACTUAL_RADIO_VOL = 56
OFF_RADIO_CONTROL_OK = 60
# Padding + ring buffer
OFF_WRITE_POS = 80
OFF_READ_POS = 144
OFF_DATA = 208

# Windows API
kernel32 = ctypes.windll.kernel32

FILE_MAP_ALL_ACCESS = 0xF001F

running = True

def memory_barrier():
    """Memory barrier - no-op en Python, los writes a memoria compartida son atómicos."""
    pass

def signal_handler(sig, frame):
    global running
    running = False
    print("\nDeteniendo...")

def main():
    global running

    parser = argparse.ArgumentParser(description="Genera un tono y lo escribe al bridge de audio")
    parser.add_argument("--freq", type=float, default=440.0, help="Frecuencia en Hz (default: 440)")
    parser.add_argument("--gain", type=float, default=0.5, help="Volumen audio externo 0.0-2.0 (default: 0.5)")
    parser.add_argument("--duration", type=int, default=0, help="Duración en segundos (0 = infinito)")
    # Nuevos controles de radio
    parser.add_argument("--radio-vol", type=float, default=1.0,
                        help="Volumen de la emisora del juego 0.0-1.0 (default: 1.0)")
    parser.add_argument("--mute-radio", action="store_true",
                        help="Silenciar la emisora del juego mientras hay audio externo")
    parser.add_argument("--duck", type=float, default=1.0,
                        help="Factor de atenuación de la emisora 0.0-1.0 (default: 1.0 = sin duck)")
    args = parser.parse_args()

    print(f"Bridge Test v2: tono {args.freq:.0f} Hz")
    print(f"  Audio externo: gain={args.gain:.2f}")
    print(f"  Radio del juego: vol={args.radio_vol:.2f}, mute={args.mute_radio}, duck={args.duck:.2f}")
    if args.duration > 0:
        print(f"  Duración: {args.duration} s")
    print("Presiona Ctrl+C para salir.\n")

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Abrir el mapping creado por ETS2CabinDSP.dll
    kernel32.OpenFileMappingW.argtypes = [
        ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.wintypes.LPCWSTR
    ]
    kernel32.OpenFileMappingW.restype = ctypes.wintypes.HANDLE

    # MapViewOfFile necesita restype correcto para 64-bit
    kernel32.MapViewOfFile.argtypes = [
        ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD, ctypes.wintypes.DWORD, ctypes.c_size_t
    ]
    kernel32.MapViewOfFile.restype = ctypes.c_void_p

    print("Esperando que ETS2 cree el mapping...")
    handle = None
    for attempt in range(60):  # 60 segundos max
        handle = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, CABIN_BRIDGE_NAME)
        if handle:
            break
        time.sleep(1)
        if attempt % 5 == 4:
            print(f"  ...esperando ({attempt + 1}s)")

    if not handle:
        print("Error: ETS2 no creó el mapping. ¿Está corriendo el juego con el plugin?")
        return 1

    # Mapear la memoria
    ptr = kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, CABIN_BRIDGE_SIZE)
    if not ptr:
        print(f"Error: no se pudo mapear la memoria ({kernel32.GetLastError()})")
        kernel32.CloseHandle(handle)
        return 1

    # Crear un buffer para acceder a la memoria
    buf = (ctypes.c_char * CABIN_BRIDGE_SIZE).from_address(ptr)
    pid = kernel32.GetCurrentProcessId()

    # Iniciar desde el write_pos actual (si lo hay) o 0
    write_pos = struct.unpack_from("<q", buf, OFF_WRITE_POS)[0]
    if write_pos < 0:
        write_pos = 0

    # Escribir nuestro header (el consumidor ya inicializó el mapping)
    struct.pack_into("<I", buf, OFF_SAMPLE_RATE, 48000)
    struct.pack_into("<I", buf, OFF_CHANNELS, 2)
    struct.pack_into("<f", buf, OFF_GAIN, args.gain)
    struct.pack_into("<I", buf, OFF_ENABLED, 1)
    struct.pack_into("<I", buf, OFF_SOURCE_PID, pid)
    struct.pack_into("<I", buf, OFF_CAPTURING, 1)

    # Campos de control de radio
    struct.pack_into("<f", buf, OFF_RADIO_VOLUME, args.radio_vol)
    struct.pack_into("<I", buf, OFF_MUTE_GAME_RADIO, 1 if args.mute_radio else 0)
    struct.pack_into("<f", buf, OFF_DUCK_FACTOR, args.duck)

    # Publicar magic y version para indicar que estamos conectados
    memory_barrier()
    struct.pack_into("<I", buf, OFF_MAGIC, CABIN_BRIDGE_MAGIC)
    struct.pack_into("<I", buf, OFF_VERSION, CABIN_BRIDGE_VERSION)

    print(f"Conectado al bridge (version {CABIN_BRIDGE_VERSION})")
    print("Generando audio...\n")

    # Generar el tono
    sample_rate = 48000.0
    omega = 2.0 * math.pi * args.freq / sample_rate
    phase = 0.0

    start_time = time.time()
    last_status = 0
    total_written = 0
    # write_pos ya se leyó arriba

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
        memory_barrier()
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
            actual_radio_vol = struct.unpack_from("<f", buf, OFF_ACTUAL_RADIO_VOL)[0]
            radio_ctrl_ok = struct.unpack_from("<I", buf, OFF_RADIO_CONTROL_OK)[0]

            radio_info = f"radio_vol={actual_radio_vol:.2f}" if radio_ctrl_ok else "radio_ctrl=OFF"
            print(f"  [{elapsed:3d} s] buf: {buffered:5d}, play: {playing}, "
                  f"under: {underruns}, {radio_info}")

            if args.duration > 0 and elapsed >= args.duration:
                print("\nDuración alcanzada.")
                break

        time.sleep(0.01)

    # Limpiar
    struct.pack_into("<I", buf, OFF_CAPTURING, 0)
    struct.pack_into("<I", buf, OFF_ENABLED, 0)
    memory_barrier()

    print("\nCerrando...")
    time.sleep(0.1)

    kernel32.UnmapViewOfFile(ptr)
    kernel32.CloseHandle(handle)

    print("Bridge cerrado.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
