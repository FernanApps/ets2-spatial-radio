# SESSION — Inyección de audio externo en la radio de ETS2

Fecha: 2026-11-07

## Objetivo

Hacer que audio externo (archivo local, y a futuro Chrome/YouTube/Spotify) suene dentro
de la radio de Euro Truck Simulator 2, pasando por el efecto espacial 3D de cabina, con
control de volumen en runtime y silenciando la radio nativa del juego.

## Estado final: FUNCIONA

- Audio externo **reemplaza** la radio del juego (no se suma) y pasa por el DSP de cabina 3D.
- La radio nativa del juego se silencia/atenúa automáticamente cuando hay audio externo.
- Al terminar el productor, la radio del juego se restaura sola.
- Volumen del audio externo ajustable **en runtime** sin reiniciar nada.
- Validado con un MP3 real (Los Kjarkas) a velocidad y pitch correctos.

---

## Arquitectura

```
Productor (Python)                Memoria compartida           Consumidor (DLL en el juego)
------------------                ------------------           ---------------------------
play_audio.py  ──escribe PCM──►   Local\ETS2CabinBridge   ──►  ETS2CabinDSP.dll
(ffmpeg decodifica MP3)           ring buffer SPSC              DSP de cabina lee y
set_volume.py  ──gain──────────►  float32 48kHz stereo         REEMPLAZA la radio
                                                                + silencia radio nativa
```

- **DLL (consumidor):** crea el *file mapping* al arrancar, engancha FMOD, y en su DSP
  de cabina lee el ring buffer y sustituye el audio de la radio por el externo.
- **Python (productor):** abre el mapping, escribe PCM al ring buffer.
- Contrato del bridge documentado en `cabin_bridge.h` y en `TEST_PROCESS_LOOPBACK.md`.

---

## Problemas encontrados y soluciones

1. **El DLL cargaba pero `scs_telemetry_init` no se ejecutaba.**
   Causa: firma incorrecta. Usábamos `int __cdecl` cuando el SDK de SCS exige
   `SCSAPI_RESULT __stdcall` con los tipos correctos.
   Fix: se creó `scs_sdk_c.h` (definiciones mínimas del SCS SDK en C puro) y se corrigió
   la firma a `scs_telemetry_init(const scs_u32_t, const scs_telemetry_init_params_v100_t*)`
   con convención `__stdcall`.

2. **El log del plugin no aparecía donde buscábamos.**
   El log va a `%LOCALAPPDATA%\ETS2 Cabin Radio\live-dsp.log`, no a Documents ni al
   game.log del juego.

3. **`createSound` fallaba siempre con `FMOD_ERR_FORMAT` (31).**
   Causa raíz: FMOD del juego es **2.3.11** (no 2.0.x), y los offsets manuales del
   `FMOD_CREATESOUNDEXINFO` no coincidían. No se pudieron obtener los offsets correctos.
   Solución: **se abandonó `createSound`**. En su lugar, el audio externo se mezcla
   directamente en el DSP de cabina que ya se adjunta al bus de radio. Más simple y sin
   depender de offsets frágiles.

4. **Python crasheaba con segfault al mapear memoria.**
   Causa: `MapViewOfFile` devolvía un puntero de 64-bit truncado a 32.
   Fix: `kernel32.MapViewOfFile.restype = ctypes.c_void_p` (y argtypes explícitos).

5. **`MemoryBarrier` / `InterlockedExchange` no existen en kernel32 desde Python.**
   Fix: se eliminaron las barreras en el productor Python (no críticas para este caso).

6. **El audio externo se sumaba a la radio (se oía doble).**
   Fix: en el DSP, `in_L = bridge` en vez de `in_L += bridge` → reemplaza, no mezcla.

7. **El audio externo no pasaba por el efecto 3D.**
   Fix: la mezcla se movió al INICIO del loop del DSP (sobre `in_L`/`in_R`), antes de la
   espacialización, no al final sobre `out_L`/`out_R`.

8. **Bug `g_bridge->enabled` vs `g_bridge_enabled`.**
   El DSP comprobaba el flag del productor en vez de la variable local del consumidor.

9. **El MP3 sonaba acelerado (~6x).**
   Dos causas:
   a) **Múltiples procesos Python** (hasta 10) escribían al mismo buffer a la vez por no
      matar instancias previas → `write_pos` avanzaba múltiple. Fix: matar instancias
      antes de lanzar.
   b) **Conflicto de umbrales:** el productor llenaba el buffer hasta 8000 frames, pero el
      *drift control* del DSP descarta muestras si el buffer pasa de ~5760
      (`TARGET_LATENCY*2`). Fix: el productor mantiene `buffered < 2880`.

---

## Archivos

### Modificados
- **`LiveCabinDSP.c`** — firmas SCS correctas; mezcla del bridge dentro del DSP de cabina
  (reemplaza la radio, antes de la espacialización); contadores de debug (`mix_count`,
  `dsp_call_count`); se abandonó la vía `createSound`.
- **`tools/bridge_test.py`** — `MapViewOfFile.restype=c_void_p`; sin `MemoryBarrier`;
  abre el mapping creado por el DLL (ya no lo crea).

### Nuevos
- **`scs_sdk_c.h`** — definiciones mínimas del SCS Telemetry SDK para C puro.
- **`tools/play_audio.py`** — reproduce un archivo local (MP3/WAV) vía ffmpeg al bridge.
  Uso: `python tools/play_audio.py "ruta.mp3" [volumen]`
- **`tools/set_volume.py`** — cambia el volumen del audio externo en runtime.
  Uso: `python tools/set_volume.py 0.5`
- **`TEST_PROCESS_LOOPBACK.md`** — spec autocontenida para implementar y testear la
  captura de audio por proceso (Chrome/YouTube) y validar si se puede mutear el proceso
  sin perder la captura.

---

## Cómo usar (archivo local)

1. Iniciar ETS2 con el plugin en `bin/win_x64/plugins/ETS2CabinDSP.dll` y la radio encendida.
2. Reproducir: `python tools/play_audio.py "C:/.../cancion.mp3" 0.7`
3. Ajustar volumen en vivo: `python tools/set_volume.py 1.0`
4. Requiere `ffmpeg` en PATH (solo para archivos locales).

Log de diagnóstico: `%LOCALAPPDATA%\ETS2 Cabin Radio\live-dsp.log`

---

## Pendiente / próximos pasos

- **Captura por proceso (Chrome/YouTube):** ver `TEST_PROCESS_LOOPBACK.md`. Decide la
  viabilidad de integrar al master de plugins. Pregunta clave: ¿mutear `chrome.exe` deja
  intacta la captura por *process loopback*? Si no, Plan B con VB-Cable.
- **Error cosmético** `UnmapViewOfFile: int too long to convert` al cerrar `play_audio.py`
  (no afecta la reproducción; falta `argtypes` en esa llamada, ya resuelto en `set_volume.py`).
- **Contadores de debug** (`mix_count`, `dsp_call_count`) y logs periódicos siguen en el
  DLL; conviene bajarlos de verbosidad para una versión final.
- `scs_sdk/` (SDK completo del template) se copió pero NO se usa; el proyecto usa
  `scs_sdk_c.h`. Se puede borrar.
