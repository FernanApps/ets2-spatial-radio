#!/usr/bin/env python3
"""
audio_sessions.py — Lista las sesiones de audio activas de Windows.

Muestra PID, nombre del proceso, estado de mute y volumen. Sirve para localizar
el PID correcto de la sesión de audio de Chrome antes de lanzar
capture_process.exe.

Uso:
  python tools/audio_sessions.py
  python tools/audio_sessions.py chrome      (filtra por nombre)

Requiere: pip install pycaw comtypes
"""

import sys

try:
    from pycaw.pycaw import AudioUtilities
except ImportError:
    print("ERROR: falta pycaw. Instala con: pip install pycaw comtypes")
    sys.exit(1)


def main():
    needle = sys.argv[1].lower() if len(sys.argv) >= 2 else None
    rows = []

    for session in AudioUtilities.GetAllSessions():
        try:
            if not session.Process:
                continue
            name = session.Process.name()
            pid = session.ProcessId
        except Exception:
            continue

        if needle and needle not in name.lower():
            continue

        try:
            mute = bool(session.SimpleAudioVolume.GetMute())
            vol = float(session.SimpleAudioVolume.GetMasterVolume())
        except Exception:
            mute, vol = False, 0.0

        rows.append((pid, name, mute, vol))

    if not rows:
        print("No hay sesiones de audio" + (f" para '{needle}'." if needle else "."))
        return 1

    print(f"{'PID':>8}  {'PROCESO':<32} {'MUTE':>5}  {'VOL':>5}")
    print("-" * 58)
    for pid, name, mute, vol in sorted(rows, key=lambda r: r[1].lower()):
        print(f"{pid:8d}  {name[:32]:<32} {int(mute):>5}  {vol:5.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
