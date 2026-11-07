# Test de viabilidad: captura de audio por proceso + mute

## Objetivo del experimento

Determinar si es posible **capturar el audio de un proceso concreto (ej. `chrome.exe`
reproduciendo YouTube) mientras ese proceso está MUTEADO en Windows**, de modo que la
música suene SOLO dentro del juego Euro Truck Simulator 2 y no por los altavoces.

La pregunta exacta a responder con SÍ/NO:

> Si silencio `chrome.exe` con `ISimpleAudioVolume::SetMute(TRUE)` (o en el Mezclador de
> volumen de Windows), ¿el *process loopback* sigue entregando las muestras de audio de
> Chrome?

- **SÍ** → el plugin es viable sin instalar drivers. Se integra al master de plugins.
- **NO** → se necesita un dispositivo de audio virtual (VB-Cable). Plan B al final.

Este documento es autocontenido. Un LLM que lo lea debe poder implementar el capturador
y ejecutar el test sin más contexto.

---

## Contexto: qué YA existe y funciona

En `T:\FernanApps\Euro-Truck-Simulator-2-Lane-Assist-Plugin\ets2-spatial-radio` hay un
plugin funcional que inyecta audio externo en la radio de ETS2:

- **`ETS2CabinDSP.dll`** — plugin del juego (ya compilado y copiado a
  `bin/win_x64/plugins/`). Al cargarse: crea un *file mapping* de memoria compartida,
  engancha FMOD, y en su DSP de cabina **reemplaza** el audio de la radio del juego por
  el audio que encuentra en el ring buffer del mapping. También silencia/atenúa la radio
  nativa del juego. **Este DLL NO se toca.**
- **`tools/play_audio.py`** — productor de referencia que YA funciona. Decodifica un MP3
  con `ffmpeg` y escribe PCM al mapping. El capturador nuevo debe comportarse como este
  productor, pero tomando el audio de un proceso en vez de un archivo.
- **`tools/set_volume.py`** — cambia el campo `gain` del mapping en runtime.

El flujo probado y validado: `play_audio.py` escribe al mapping → el DSP lee y reproduce
en la radio del juego con efecto espacial 3D. **La parte del consumidor (juego) ya está
resuelta. Solo falta un productor que capture un proceso.**

---

## Contrato del bridge (memoria compartida)

El capturador debe ABRIR (no crear) este mapping; lo crea el DLL del juego al arrancar.

- **Nombre del mapping:** `Local\ETS2CabinBridge`
- **Tamaño total:** `131280` bytes
- **Formato de audio:** float32, estéreo intercalado (L,R,L,R…), 48000 Hz
- **Capacidad del ring buffer:** `16384` frames (potencia de 2)
- **Apertura:** `OpenFileMappingW(FILE_MAP_ALL_ACCESS=0xF001F, FALSE, "Local\\ETS2CabinBridge")`
  luego `MapViewOfFile(..., 131280)`. **En 64-bit, `MapViewOfFile.restype` DEBE ser
  `c_void_p`; si se trunca el puntero hay segfault.**

### Offsets (little-endian)

| Offset | Tipo | Campo | Quién escribe |
|-------:|------|-------|---------------|
| 0   | uint32 | `magic` = `0x31424345` | productor |
| 4   | uint32 | `version` = `2` | productor |
| 8   | uint32 | `sample_rate` = `48000` | productor |
| 12  | uint32 | `channels` = `2` | productor |
| 16  | float  | `gain` (0.0–2.0, volumen externo) | productor |
| 20  | uint32 | `enabled` = `1` | productor |
| 24  | uint32 | `source_pid` (PID del capturador) | productor |
| 28  | uint32 | `capturing` = `1` mientras entrega audio | productor |
| 32  | float  | `radio_volume` (factor sobre radio original) | productor |
| 36  | uint32 | `mute_game_radio` (1 = silenciar radio juego) | productor |
| 40  | float  | `duck_factor` (atenuación extra radio) | productor |
| 48  | uint32 | `playing` (1 = DSP sonando) | **consumidor** |
| 52  | uint32 | `underruns` | consumidor |
| 56  | float  | `actual_radio_vol` | consumidor |
| 60  | uint32 | `radio_control_ok` | consumidor |
| 80  | int64  | `write_pos` (frames escritos, monótono) | productor |
| 144 | int64  | `read_pos` (frames leídos, monótono) | **consumidor** |
| 208 | float[] | `data` (ring buffer, 16384*2 floats) | productor |

### Protocolo de escritura (SPSC: un productor, un consumidor)

1. Al iniciar: escribir header (`sample_rate`, `channels`, `gain`, `source_pid`),
   poner `enabled=1`, `capturing=1`, y por último `magic` y `version`.
2. Leer `read_pos` (offset 144) para saber cuánto consumió el juego.
3. `buffered = write_pos - read_pos`. **Mantener `buffered` por debajo de 2880 frames.**
   Si se pasa de ~5760, el DSP descarta muestras y el audio suena ACELERADO
   (bug ya diagnosticado). Dormir ~3 ms si `buffered >= 2880` antes de escribir más.
4. Escribir frames en `data[(write_pos+i) % 16384 * 2]` (índice en floats).
5. Publicar el nuevo `write_pos` (offset 80) DESPUÉS de copiar las muestras.
6. Al terminar: `capturing=0`, `enabled=0`. El juego restaura su radio automáticamente.

> Índice en `data`: `float_index = ((frame_pos) % 16384) * 2`, byte offset
> `= 208 + float_index * 4`. Cada frame son 8 bytes (2 floats).

---

## Qué construir: `capture_process.exe` (o `.py` con ctypes)

Un productor que, en vez de leer un archivo, capture el audio de un proceso por PID y lo
escriba al bridge con el protocolo de arriba.

### API de Windows requerida: Process Loopback

Windows 10 versión 2004 (build 19041) o superior. No existe wrapper de Python maduro,
así que la ruta recomendada es **C/C++** (o `ctypes`/`comtypes` si se insiste en Python).

Pasos de la API:

1. Construir `AUDIOCLIENT_ACTIVATION_PARAMS`:
   - `ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
   - `ProcessLoopbackParams.TargetProcessId = <PID de chrome.exe>`
   - `ProcessLoopbackParams.ProcessLoopbackMode =
     PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`
2. Envolver en `PROPVARIANT` (`VT_BLOB`).
3. Llamar `ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, IID_IAudioClient,
   &activationParams, &completionHandler, &asyncOp)`. El device id literal es:
   `L"VAD\\Process_Loopback"` (constante `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`).
4. En el callback `ActivateCompleted`, obtener el `IAudioClient`.
5. `IAudioClient::Initialize` con:
   - `AUDCLNT_SHAREMODE_SHARED`
   - flags `AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK`
   - un `WAVEFORMATEX` de captura (ej. 48000 Hz, 2 ch, float32 / IEEE_FLOAT).
6. `GetService(IID_IAudioCaptureClient)`, arrancar con `Start()`.
7. Bucle: `GetBuffer` → copiar muestras → resamplear a 48000/stereo/float32 si hace falta
   → escribir al bridge (protocolo arriba) → `ReleaseBuffer`.

Referencia oficial: ejemplo "ApplicationLoopback" del repo
`microsoft/Windows-classic-samples` (buscar `ApplicationLoopback`).

### Argumentos sugeridos del ejecutable

```
capture_process.exe <PID> [volumen 0.0-2.0]
```

Obtener el PID de Chrome con la sesión de audio activa (ver test abajo).

---

## Procedimiento de test (lo que el humano hará)

### Preparación
1. Iniciar ETS2 y entrar a conducir con la **radio encendida** (el plugin ya está en
   `bin/win_x64/plugins/ETS2CabinDSP.dll`).
2. Abrir Chrome y reproducir una canción en YouTube (que se oiga por los altavoces).
3. Averiguar el PID de la sesión de audio de Chrome:
   ```bash
   python -c "from pycaw.pycaw import AudioUtilities; [print(s.Process.name(), s.ProcessId) for s in AudioUtilities.GetAllSessions() if s.Process]"
   ```

### Fase A — ¿captura sin mutear?
4. Ejecutar `capture_process.exe <PID_chrome>`.
5. **Esperado:** la música de YouTube se oye AHORA dentro de la radio del juego (con el
   efecto de cabina). En este punto puede oírse doble (altavoces + juego); es normal.
6. Confirmar en el log del juego (`%LOCALAPPDATA%\ETS2 Cabin Radio\live-dsp.log`) que
   aparece `mix=...` subiendo ~48000/s y `calls=94/s`.

### Fase B — LA PRUEBA CLAVE: mutear Chrome
7. Con el capturador aún corriendo, mutear Chrome por proceso:
   ```bash
   python -c "from pycaw.pycaw import AudioUtilities; [s.SimpleAudioVolume.SetMute(1,None) for s in AudioUtilities.GetAllSessions() if s.Process and s.Process.name()=='chrome.exe']"
   ```
   (o botón derecho en el icono de volumen → Mezclador de volumen → silenciar Chrome).
8. **Observar y anotar:**
   - ¿La música dejó de salir por los **altavoces**? (debería: Chrome está muteado)
   - ¿La música SIGUE sonando **dentro del juego**? ← ESTO ES LO QUE DECIDE TODO
   - En el log, ¿`mix=` sigue subiendo ~48000/s o se congela/baja?

### Interpretación
- **Altavoces en silencio + juego sigue sonando + `mix` sigue subiendo** →
  ✅ **VIABLE.** El loopback por proceso captura antes del mute del endpoint.
  El plugin se puede integrar al master sin drivers.
- **Al mutear Chrome el juego también se queda en silencio** (o `mix` deja de subir) →
  ❌ El mute mata la captura. Ir al Plan B.

### Fase C — limpieza
9. Cerrar el capturador (Ctrl+C). El juego debe restaurar su radio nativa solo.
10. Desmutear Chrome:
    ```bash
    python -c "from pycaw.pycaw import AudioUtilities; [s.SimpleAudioVolume.SetMute(0,None) for s in AudioUtilities.GetAllSessions() if s.Process and s.Process.name()=='chrome.exe']"
    ```

---

## Criterios de aceptación

| Resultado | Veredicto |
|-----------|-----------|
| Chrome muteado en altavoces, pero su audio sigue entrando al juego | ✅ Viable sin drivers — integrar al master |
| Al mutear Chrome se corta también la captura | ❌ No viable por esta vía — usar Plan B |
| El audio en el juego suena acelerado | Bug de buffer: `buffered` supera 2880, revisar control de flujo del productor |

---

## Plan B (si la Fase B falla): dispositivo de audio virtual

1. Instalar **VB-Audio Virtual Cable** (driver gratuito).
2. En Chrome / Windows, enviar la salida de Chrome al dispositivo "CABLE Input".
   Así Chrome NO suena por los altavoces reales.
3. El capturador hace loopback del dispositivo **"CABLE Output"** (endpoint normal, no por
   proceso) y escribe al bridge igual que antes.
4. Ventaja: 100% fiable, sin depender del comportamiento del mute. Desventaja: el usuario
   final debe instalar el driver una vez.

> Para Plan B el loopback es de ENDPOINT (más simple), y en Python existe
> `pyaudiowpatch` (fork de PyAudio con WASAPI loopback) que lo hace sin código nativo.

---

## Notas de implementación

- Reutilizar el control de flujo de `tools/play_audio.py` (mantener `buffered < 2880`,
  `sleep(0.003)`).
- El capturador debe resamplear a 48000 Hz estéreo float32. El formato nativo del loopback
  suele ser el mix format del endpoint (a menudo 48000 Hz float ya), pero NO asumirlo:
  leer el `WAVEFORMATEX` real y convertir si difiere.
- Un solo productor a la vez. Si se lanzan varios, el `write_pos` avanza múltiple y el
  audio se corrompe (bug ya observado con procesos Python duplicados). Matar instancias
  previas antes de lanzar.
- No tocar `ETS2CabinDSP.dll` ni el DSP; el contrato del bridge es estable.

---

## Resultado del test (2026-11-07)

Implementado y ejecutado `tools/capture_process.exe` en modo autónomo (`--test`, sin
DLL ni juego) contra Chrome reproduciendo YouTube.

| Escenario | Paquetes/s | frames/s | RMS | Pico | Resultado |
|-----------|-----------:|---------:|----:|-----:|-----------|
| Sin mute, volumen 1.0 | ~100 | 48000 | ≈ −16 dBFS | 0.59 | Captura correcta |
| Mute de **sesión** (`SetMute`) | ~100 | 48000 | −96.7 dBFS | 0.000 | ❌ El mute mata el contenido |
| **Volumen de sesión** 0.5 | ~100 | 48000 | ≈ −22 dBFS | 0.32 | Escala −6 dB |
| **Volumen de sesión** 0.1 | ~100 | 48000 | ≈ −36 dBFS | 0.07 | Escala −20 dB |
| **Volumen de sesión** 0.01 | ~100 | 48000 | ≈ −57 dBFS | 0.005 | Escala −40 dB |
| **Volumen de sesión** 0.00001 + boost 100 dB | ~100 | 48000 | ≈ −16 dBFS | 0.5 | ✅ Audio recuperado |
| Mute del **endpoint** de salida | ~100 | 48000 | ≈ −17 dBFS | 0.55 | ✅ La captura sobrevive |

**Conclusión:** el *process loopback* toma el audio **después del volumen de sesión** y
**antes del mute del endpoint**. Por eso:

- `SetMute` / volumen 0 → silencio digital (no recuperable).
- Bajar el volumen de sesión a un valor ínfimo pero **no cero** → la captura conserva la
  señal, atenuada; se puede recuperar con ganancia digital.
- Mutear el endpoint → la captura no se entera.

### Solución sin drivers, sin VB-Cable, sin tabs y con **una sola salida**

Bajar la sesión de Chrome a `0.00001` (−100 dBFS, inaudible en cualquier altavoz) y
amplificar la captura +100 dB. El motor de audio de Windows mezcla en float32, así que a
−100 dB la señal sigue teniendo resolución suficiente. Validado: RMS restaurado a
≈ −16 dBFS con picos limpios (sin clipping con 1.5 dB de margen).

`capture_process.exe --duck` hace todo automático:

1. Enumera las sesiones de audio del árbol de procesos del PID.
2. Guarda el volumen/mute originales.
3. Los baja a `1e-5` (o `--duck-volume V`) y los desmutea.
4. Captura en float32 y aplica el boost compensatorio.
5. Al salir (Ctrl+C) **restaura** el volumen y mute originales.
6. Re-enumera cada 1 s para duckear sesiones nuevas y **re-aplicar** el volumen a las
   existentes si alguien lo sube a mano.
7. Protección anti-picos: limitador por paquete (pico de salida ≤ 0.95) y *gate* que
   silencia el paquete si el pico capturado es mucho mayor de lo esperado. Al detectarlo
   re-aplica el duck de inmediato: si subes el volumen a mano, se oyen ~60 ms de silencio
   en vez de un pepinazo.

Uso:

```
capture_process.exe <PID_chrome> --duck                  (bridge del juego)
capture_process.exe <PID_chrome> --test out.wav --duck   (prueba sin DLL)
```

Ventaja: funciona con **un solo dispositivo de salida** y sin instalar nada.

Contrapartida: mientras corre, el volumen de Chrome en el mezclador aparece a ~0%; se
restaura al cerrar. Si el proceso se mata a la fuerza (sin Ctrl+C), el volumen puede
quedar bajo y hay que subirlo a mano.

### Tiempo real y latencia

La captura es **continua**, no una grabación: el capturador lee paquetes y escribe al
anillo mientras el proceso reproduce. Cambiar de canción/vídeo/pestaña dentro de la misma
app no interrumpe nada; durante una pausa llegan tramas de silencio y al reanudar sigue
solo. Solo hay que relanzar el capturador si se cambia de **aplicación** (cambia el PID).

Latencia medida con `--monitor` (captura → dispositivo predeterminado):

- Cola del monitor: 0–20 ms.
- Buffer del dispositivo: 23.4 ms.
- Buffer de captura WASAPI: ~10 ms.
- **Total monitor: ~35–55 ms.**

En el juego se suma el buffer del DSP/FMOD: **~60–120 ms** en total. Para música es
imperceptible; para vídeo es 1–3 fotogramas. No hay “lag” de cortes: en 25 s se
capturaron 1.200.000 frames exactos (48 000/s), 2.500 paquetes, 0 pérdidas.

### Alternativa (si se prefiere no tocar el volumen de la app)

Usar **dos endpoints de salida**: Chrome → endpoint secundario muteado; ETS2 → endpoint
principal. El *process loopback* no está atado a un endpoint (README oficial del sample
`ApplicationLoopback`: *"the capture is not tied to a specific audio endpoint"*), así que
captura Chrome aunque renderice en el endpoint muteado. Requiere dos dispositivos.

### Herramientas añadidas

- `tools/capture_process.cpp` → `build/Release/capture_process.exe`
  - `capture_process.exe <PID> [volumen]` → modo bridge (juego + DLL).
  - `capture_process.exe <PID> --duck` → bridge + ducking automático (recomendado).
  - `capture_process.exe <PID> --test [salida.wav] [--seconds N]` → modo autónomo,
    graba WAV y muestra paquetes/s, frames/s, RMS y pico.
  - `capture_process.exe <PID> --monitor [--seconds N]` → reproduce en tiempo real por
    el dispositivo predeterminado (implica `--duck`). Sirve para probar continuidad y
    latencia sin ETS2.
- `tools/audio_sessions.py` → lista PID/proceso/mute de las sesiones de audio.
- `tools/mute_process.py <nombre.exe> <1|0>` → mutea/desmutea por proceso.
