#!/usr/bin/env python3
"""
bridge_consumer_test.py — Consumidor de prueba para el bridge de audio

Lee del mapping de memoria compartida y reproduce por los altavoces.
Sirve para probar el flujo IPC completo sin necesidad de ETS2.

Uso:
  1. En una terminal: python bridge_test.py
  2. En otra terminal: python bridge_consumer_test.py

Requisitos: Python 3.8+ con pyaudio
  pip install pyaudio
"""

import ctypes
import struct
import time
import sys

try:
    import pyaudio
except ImportError:
    print("Error: necesitas instalar pyaudio")
    print("  pip install pyaudio")
    sys.exit(1)

# Constantes de cabin_bridge.h
CABIN_BRIDGE_NAME = "Local\\ETS2CabinBridge"
CABIN_BRIDGE_MAGIC = 0x31424345
CABIN_BRIDGE_VERSION = 1
CABIN_BRIDGE_CAPACITY = 16384
CABIN_BRIDGE_SIZE = 131280

# Offsets
OFF_MAGIC = 0
OFF_VERSION = 4
OFF_SAMPLE_RATE = 8
OFF_CHANNELS = 12
OFF_GAIN = 16
OFF_ENABLED = 20
OFF_CAPTURING = 28
OFF_PLAYING = 32
OFF_UNDERRUNS = 36
OFF_WRITE_POS = 80
OFF_READ_POS = 144
OFF_DATA = 208

kernel32 = ctypes.windll.kernel32
FILE_MAP_ALL_ACCESS = 0xF001F

def main():
    print("Bridge Consumer Test")
    print("Esperando que bridge_test.py cree el mapping...\n")

    # Intentar abrir el mapping
    handle = None
    ptr = None

    for attempt in range(30):
        handle = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, CABIN_BRIDGE_NAME)
        if handle:
            break
        print(f"  Intento {attempt + 1}/30: mapping no encontrado, esperando...")
        time.sleep(1)

    if not handle:
        print("Error: no se pudo abrir el mapping. ¿Está corriendo bridge_test.py?")
        return 1

    ptr = kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, CABIN_BRIDGE_SIZE)
    if not ptr:
        print(f"Error: no se pudo mapear ({kernel32.GetLastError()})")
        kernel32.CloseHandle(handle)
        return 1

    buf = (ctypes.c_char * CABIN_BRIDGE_SIZE).from_address(ptr)

    # Validar header
    magic = struct.unpack_from("<I", buf, OFF_MAGIC)[0]
    version = struct.unpack_from("<I", buf, OFF_VERSION)[0]

    if magic != CABIN_BRIDGE_MAGIC or version != CABIN_BRIDGE_VERSION:
        print(f"Error: header inválido (magic={hex(magic)}, version={version})")
        kernel32.UnmapViewOfFile(ptr)
        kernel32.CloseHandle(handle)
        return 1

    sample_rate = struct.unpack_from("<I", buf, OFF_SAMPLE_RATE)[0]
    channels = struct.unpack_from("<I", buf, OFF_CHANNELS)[0]

    print(f"Mapping abierto: {sample_rate} Hz, {channels} canales")
    print("Reproduciendo... (Ctrl+C para salir)\n")

    # Inicializar PyAudio
    pa = pyaudio.PyAudio()
    stream = pa.open(
        format=pyaudio.paFloat32,
        channels=channels,
        rate=sample_rate,
        output=True,
        frames_per_buffer=1024
    )

    read_pos = struct.unpack_from("<q", buf, OFF_WRITE_POS)[0]  # empezar desde el final
    underruns = 0
    frames_played = 0
    start_time = time.time()
    last_status = 0

    # Marcar que estamos reproduciendo
    struct.pack_into("<I", buf, OFF_PLAYING, 1)

    try:
        while True:
            write_pos = struct.unpack_from("<q", buf, OFF_WRITE_POS)[0]
            capturing = struct.unpack_from("<I", buf, OFF_CAPTURING)[0]

            if not capturing:
                print("\nProductor detenido.")
                break

            avail = write_pos - read_pos

            if avail <= 0:
                # Underrun: escribir silencio
                silence = b'\x00' * (1024 * channels * 4)
                stream.write(silence)
                underruns += 1
                continue

            # Control de deriva
            if avail > CABIN_BRIDGE_CAPACITY // 2:
                skip = avail - 2880  # objetivo ~60ms
                read_pos += skip
                avail = 2880

            # Leer hasta 1024 frames
            to_read = min(avail, 1024)
            samples = []

            gain = struct.unpack_from("<f", buf, OFF_GAIN)[0]

            for i in range(to_read):
                idx = ((read_pos + i) % CABIN_BRIDGE_CAPACITY) * 2
                offset = OFF_DATA + idx * 4
                L = struct.unpack_from("<f", buf, offset)[0] * gain
                R = struct.unpack_from("<f", buf, offset + 4)[0] * gain
                samples.append(L)
                samples.append(R)

            read_pos += to_read
            frames_played += to_read

            # Publicar read_pos
            struct.pack_into("<q", buf, OFF_READ_POS, read_pos)
            struct.pack_into("<I", buf, OFF_UNDERRUNS, underruns)

            # Reproducir
            data = struct.pack(f"<{len(samples)}f", *samples)
            stream.write(data)

            # Status cada segundo
            now = time.time()
            if now - last_status >= 1.0:
                last_status = now
                elapsed = int(now - start_time)
                buffered = write_pos - read_pos
                print(f"  [{elapsed:3d} s] reproducido: {frames_played} frames, "
                      f"buffer: {buffered}, underruns: {underruns}")

    except KeyboardInterrupt:
        print("\nDetenido por usuario.")

    finally:
        struct.pack_into("<I", buf, OFF_PLAYING, 0)
        stream.stop_stream()
        stream.close()
        pa.terminate()
        kernel32.UnmapViewOfFile(ptr)
        kernel32.CloseHandle(handle)

    print("Consumer cerrado.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
