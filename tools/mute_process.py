#!/usr/bin/env python3
"""
mute_process.py — Mutea o desmutea todas las sesiones de audio de un proceso.

Se usa en la Fase B del test de process loopback: con capture_process.exe
corriendo, mutea Chrome y observa si la captura sigue recibiendo audio.

Uso:
  python tools/mute_process.py chrome.exe 1    (mutear)
  python tools/mute_process.py chrome.exe 0    (desmutear)

Requiere: pip install pycaw comtypes
"""

import sys

try:
    from pycaw.pycaw import AudioUtilities
except ImportError:
    print("ERROR: falta pycaw. Instala con: pip install pycaw comtypes")
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print("Uso: python tools/mute_process.py <nombre.exe> [1|0]")
        return 2

    target = sys.argv[1].lower()
    mute = 1
    if len(sys.argv) >= 3:
        mute = 1 if sys.argv[2] not in ("0", "false", "False") else 0

    found = 0
    for session in AudioUtilities.GetAllSessions():
        try:
            if not session.Process:
                continue
            name = session.Process.name()
            pid = session.ProcessId
        except Exception:
            continue

        if name.lower() != target:
            continue

        try:
            session.SimpleAudioVolume.SetMute(mute, None)
            found += 1
            print(f"{name} (PID {pid}): mute={mute}")
        except Exception as exc:
            print(f"{name} (PID {pid}): no se pudo cambiar el mute: {exc}")

    if found == 0:
        print(f"No se encontraron sesiones de audio para '{target}'.")
        return 1

    print(f"Sesiones afectadas: {found}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
