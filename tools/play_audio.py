#!/usr/bin/env python3
"""
play_audio.py — Reproduce un archivo de audio por el bridge de ETS2

Uso:
  python play_audio.py archivo.mp3
  python play_audio.py archivo.wav

Requisitos: ffmpeg en PATH
"""

import ctypes
import ctypes.wintypes
import struct
import subprocess
import sys
import time
import signal

CABIN_BRIDGE_NAME = "Local\\ETS2CabinBridge"
CABIN_BRIDGE_MAGIC = 0x31424345
CABIN_BRIDGE_VERSION = 2
CABIN_BRIDGE_CAPACITY = 16384
CABIN_BRIDGE_SIZE = 131280
FILE_MAP_ALL_ACCESS = 0xF001F

OFF_MAGIC = 0
OFF_VERSION = 4
OFF_SAMPLE_RATE = 8
OFF_CHANNELS = 12
OFF_GAIN = 16
OFF_ENABLED = 20
OFF_SOURCE_PID = 24
OFF_CAPTURING = 28
OFF_PLAYING = 48
OFF_WRITE_POS = 80
OFF_READ_POS = 144
OFF_DATA = 208

kernel32 = ctypes.windll.kernel32
running = True

def signal_handler(sig, frame):
    global running
    running = False
    print("\nDeteniendo...")

def main():
    global running

    if len(sys.argv) < 2:
        print("Uso: python play_audio.py <archivo.mp3|wav> [volumen 0.0-1.0]")
        return 1

    source = sys.argv[1]
    volume = float(sys.argv[2]) if len(sys.argv) >= 3 else 1.0
    print(f"Reproduciendo: {source} (volumen {volume:.2f})")

    signal.signal(signal.SIGINT, signal_handler)

    # Configurar ctypes para 64-bit
    kernel32.OpenFileMappingW.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.wintypes.LPCWSTR]
    kernel32.OpenFileMappingW.restype = ctypes.wintypes.HANDLE
    kernel32.MapViewOfFile.argtypes = [ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD,
                                        ctypes.wintypes.DWORD, ctypes.wintypes.DWORD, ctypes.c_size_t]
    kernel32.MapViewOfFile.restype = ctypes.c_void_p

    # Abrir mapping
    handle = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, CABIN_BRIDGE_NAME)
    if not handle:
        print("Error: ETS2 no está corriendo o el plugin no está cargado")
        return 1

    ptr = kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, CABIN_BRIDGE_SIZE)
    if not ptr:
        print("Error: no se pudo mapear la memoria")
        return 1

    buf = (ctypes.c_char * CABIN_BRIDGE_SIZE).from_address(ptr)

    # Iniciar ffmpeg para decodificar a PCM float 48kHz stereo
    cmd = [
        'ffmpeg', '-hide_banner', '-loglevel', 'error',
        '-i', source,
        '-f', 'f32le',
        '-acodec', 'pcm_f32le',
        '-ar', '48000',
        '-ac', '2',
        '-'
    ]

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        print("Error: ffmpeg no encontrado. Instálalo y agrégalo al PATH.")
        return 1

    # Escribir header
    pid = kernel32.GetCurrentProcessId()

    # Empezar desde la posición actual
    write_pos = struct.unpack_from("<q", buf, OFF_WRITE_POS)[0]
    if write_pos < 0:
        write_pos = 0

    struct.pack_into("<I", buf, OFF_SAMPLE_RATE, 48000)
    struct.pack_into("<I", buf, OFF_CHANNELS, 2)
    struct.pack_into("<f", buf, OFF_GAIN, volume)
    struct.pack_into("<I", buf, OFF_ENABLED, 1)
    struct.pack_into("<I", buf, OFF_SOURCE_PID, pid)
    struct.pack_into("<I", buf, OFF_CAPTURING, 1)
    struct.pack_into("<I", buf, OFF_MAGIC, CABIN_BRIDGE_MAGIC)
    struct.pack_into("<I", buf, OFF_VERSION, CABIN_BRIDGE_VERSION)

    print("Conectado. Reproduciendo... (Ctrl+C para detener)\n")

    start_time = time.time()
    last_status = 0

    while running:
        # Leer samples de ffmpeg
        data = proc.stdout.read(1024 * 8)  # 1024 frames stereo float32
        if not data:
            print("\nFin del archivo.")
            break

        frames = len(data) // 8

        # Control de flujo: mantener buffer PEQUEÑO (por debajo del umbral de
        # deriva del DSP = TARGET_LATENCY*2 = 5760). Si lo superamos, el DSP
        # descarta muestras y el audio suena acelerado.
        while running:
            read_pos = struct.unpack_from("<q", buf, OFF_READ_POS)[0]
            buffered = write_pos - read_pos
            if buffered < 2880:  # ~60ms
                break
            time.sleep(0.003)

        if not running:
            break

        # Escribir al ring buffer
        for i in range(frames):
            idx = ((write_pos + i) % CABIN_BRIDGE_CAPACITY) * 2
            offset = OFF_DATA + idx * 4
            buf[offset:offset+8] = data[i*8:(i+1)*8]

        write_pos += frames
        struct.pack_into("<q", buf, OFF_WRITE_POS, write_pos)

        # Status cada segundo
        now = time.time()
        if now - last_status >= 1.0:
            last_status = now
            elapsed = now - start_time
            read_pos = struct.unpack_from("<q", buf, OFF_READ_POS)[0]
            playing = struct.unpack_from("<I", buf, OFF_PLAYING)[0]
            buffered = write_pos - read_pos
            mins = int(elapsed) // 60
            secs = int(elapsed) % 60
            print(f"  [{mins:02d}:{secs:02d}] buf:{buffered:5d} play:{playing}")

    # Limpiar
    struct.pack_into("<I", buf, OFF_CAPTURING, 0)
    struct.pack_into("<I", buf, OFF_ENABLED, 0)

    proc.terminate()
    proc.wait()

    print("Cerrado.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
