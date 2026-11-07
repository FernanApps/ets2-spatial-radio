# Hallazgos — ETS2 Cabin Radio v1.2.0

Análisis del código fuente realizado el 2026-11-07.
Actualizado con análisis del plugin maestro `ets2_ds_plugin`.

## Origen

- El archivo `realistic-cabin-3d-audio-cabin-radio-v1_2-1_61_x.zip` (mod para ETS2 1.61) contiene
  la carpeta `ETS2-Cabin-Radio-v1.2.0-Source/`.
- Sus 10 archivos son **idénticos byte a byte** (MD5) a los de este repositorio:
  `LiveCabinDSP.c`, `windows_main.c`, `main.swift`, `build-windows.sh`, `build-macos.sh`,
  `Info.plist`, `LICENSE`, `app.res`, `app.ico`, `AppIcon.png`.
- Este repo agrega `README.md`, `RELEASE_NOTES.md`, `.github/` y `.gitignore`.
- El remoto es `FernanApps/ets2-spatial-radio`, pero el README enlaza a `uawannacry/ets2-spatial-radio`
  (probablemente el autor original; este repo sería un fork o una copia).
- Licencia: GPL v3.

## Qué es

Un procesador de audio en tiempo real para Euro Truck Simulator 2 (Windows x64 y macOS) que simula
una cabina con 4.1 altavoces alrededor de la cabeza del conductor:

- Altavoces delantero izquierdo/derecho (con retardo Haas entre lados), traseros izquierdo/derecho
  y subwoofer bajo el asiento (< 115 Hz).
- 4 reflexiones tempranas (parabrisas, cristales laterales, pared de la litera).
- Espacialización binaural a 48 kHz: ITD fraccional, sombra de cabeza en 2 bandas y
  direccionalidad de pinna delante/detrás según la rotación de la cámara.
- 4 presets: Premium, Warm, Stock Radio, Spatial. Se ajustan en vivo: intensidad, balance trasero
  y posición.

## Qué audio procesa

- **Solo** el bus de FMOD `bus:/radio` (`LiveCabinDSP.c:884`), es decir, la radio del juego:
  - Emisoras de radio por internet de ETS2.
  - MP3/OGG del reproductor del juego (`Documentos\Euro Truck Simulator 2\music`;
    el botón "Music Folder" solo abre esa carpeta, `windows_main.c:520-527`).
- **No** toca motor, claxon, GPS, tráfico ni ambiente (otros buses).
- **No** toca audio de otros programas (YouTube, Chrome, Spotify, Discord): solo existe dentro de
  `eurotrucks2.exe`.

## Parámetros del juego que lee

Lee cada frame, con `FMOD_Studio_System_GetParameterByName`, parámetros que ya expone el juego
(`LiveCabinDSP.c:836-839`):

| Parámetro | Significado | Efecto |
|---|---|---|
| `cabin_rot` | Rotación de la cámara interior | Gira los altavoces virtuales alrededor de la cabeza |
| `cabin_out` | Cámara fuera de la cabina (0–1) | Atenúa hasta 55 %, quita graves/medios/agudos, reduce subwoofer y reflexiones (`:703`, `:781-786`, `:860-863`) |
| `wnd_left` / `wnd_right` | Ventanilla izquierda/derecha abierta | Pierde graves y reflexiones del lado abierto |

- **No** detecta el tipo de vehículo: con mods de autobús debería funcionar si usan `bus:/radio`,
  pero siempre simula la geometría de una cabina de camión. Esto se deduce del código; no se probó.

## Cómo se engancha al juego (Windows)

1. **Launcher** (`windows_main.c`): busca `eurotrucks2.exe` vía Steam (`libraryfolders.vdf`) o con
   un diálogo, y luego:
   - **Inyección en vivo**: `VirtualAllocEx` + `WriteProcessMemory` +
     `CreateRemoteThread(LoadLibraryA)` (`:318-336`), o
   - **Auto-plugin**: copia `ETS2CabinDSP.dll` a `bin\win_x64\plugins\` (`:483-502`).
2. **DLL** (`LiveCabinDSP.c`): exporta `scs_telemetry_init`/`scs_telemetry_shutdown` (`:1151`)
   para que el juego la cargue como plugin de telemetría SCS. También arranca desde `DllMain`.
3. Un hilo espera a `fmodstudio.dll` (hasta 30 s) y parchea la **IAT** del ejecutable para
   interceptar `FMOD_Studio_System_Update` (API C y C++) (`:1053-1134`).
4. En cada update: resuelve `bus:/radio`, bloquea su channel group y le añade DSPs
   (DSP propio + `THREE_EQ` + `COMPRESSOR`).

En macOS el launcher es SwiftUI (`main.swift`) y la dylib x86_64 se carga en el proceso del juego
(bajo Rosetta).

## Archivos que escribe

- Registro: `HKCU\Software\ETS2CabinRadio` (Preset, IntensityPermille, RearPermille,
  PositionPermille, GameExePath).
- `%LOCALAPPDATA%\ETS2 Cabin Radio\live-dsp.log` (ruta alternativa con `ETS2_CABIN_LOG_PATH`)
  y `live-settings.env` (configuración que la DLL recarga en caliente).
- Opcional: `ETS2CabinDSP.dll` en `bin\win_x64\plugins\`.
- Variable de entorno `ETS2_CABIN_DRY_RUN=1`: carga sin conectar DSP (diagnóstico).

---

## Plugin maestro (`ets2_ds_plugin`) — lo que ya existe

El proyecto padre en `T:\FernanApps\Euro-Truck-Simulator-2-Lane-Assist-Plugin\ets2_ds_plugin\` ya tiene
implementado casi todo lo que hace falta para el audio externo.

### Captura de audio (`gui/src/modules/video/audio_capture.cpp`)

- **Loopback por proceso** (Win10 2004+): usa `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` +
  `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` vía `ActivateAudioInterfaceAsync` (`:105-130`).
- **Fallback a loopback global** si la activación por proceso falla (`:167-169`).
- `audio_list_processes()`: lista procesos con sesión de audio activa, excluyendo el juego y sistema.
- Hilo dedicado que captura, convierte a float/PCM16 y analiza. La GUI consume `audio_levels()`.
- Hoy pasa solo el **nivel analizado** (`audio_level`) al DLL, no las muestras. Para la radio
  espacial hace falta un canal nuevo con las muestras raw.

### Sistema IPC (`shared/include/ds_shared/ipc.hpp`)

- `Channel<T>`: plantilla de canal sobre `CreateFileMappingA` / `OpenFileMappingA`.
- Protocolo **seqlock**: el escritor sube `seq` a impar antes de escribir y a par después; el lector
  reintenta si `seq` es impar o cambió (`:136-179`).
- El **DLL crea** el canal (después de validar la licencia); la **GUI lo abre** (`:95-111`).
- Prefijo `Local\ds_plugin.`, versión `kIpcVersion = 10`, magic `'DSPL'`.
- `read()` nunca bloquea: o copia el valor, o se queda con el anterior.

### Estado de video/audio (`shared/include/ds_shared/video_ipc.hpp`)

- `VideoState` con campos de audio: `audio_sensitivity`, `audio_bass_filter`, `audio_beat_thresh`,
  y el resultado `audio_level` (0–1), más controles de luces y barra neon.
- El análisis se hace en la GUI; el DLL solo pinta según el nivel que le llega.

### Implicación para el plan

No hay que escribir `ETS2CabinBridge.exe` ni la captura WASAPI desde cero: se reutiliza
`audio_capture.cpp`. Solo hace falta:

1. Un **nuevo canal de audio raw** con ring buffer (`AudioBridgeState` o similar), independiente de
   `VideoState`, para no romper el layout existente.
2. Modificar el hilo de captura para que, además de analizar, copie las muestras al ring buffer.
3. En `LiveCabinDSP.dll`: abrir ese canal, leer las muestras y reproducirlas en FMOD.

Esto simplifica la Fase 1 del plan: no hay binario externo, el audio viaja por el sistema IPC que ya
funciona, y la GUI ya tiene el selector de proceso y los ajustes de sensibilidad.

---

## Análisis de Ghidra sobre `eurotrucks2-1.62.1.0s.exe`

Resultados de la Fase 0 del plan.

### P1: Versión de FMOD

**FMOD 2.03.17** — constante `0x20311` pasada a `FMOD::Studio::System::create`.

- DLLs cargadas dinámicamente: `fmod.dll`, `fmodstudio.dll` (via `LoadLibrary` + `GetProcAddress`).
- `FMOD_ADVANCEDSETTINGS.cbsize` = 0x68 (104 bytes), consistente con FMOD 2.x 64-bit.
- **`FMOD_CREATESOUNDEXINFO.cbsize` = 112 bytes (0x70)** para FMOD 2.03.x 64-bit. Crítico: el código
  actual en `LiveCabinDSP.c` asume un layout de `FMOD_DSP_DESCRIPTION` de 0xD8 bytes (216), que es
  correcto para FMOD 2.x 64-bit.

### P5: Exports FMOD necesarios

Todos los exports necesarios para reproducir audio externo **existen** en `fmod.dll`:

| Función | Estado | Notas |
|---------|--------|-------|
| `FMOD_System_CreateSound` | ✅ | C++ mangled, pero la C-API también existe |
| `FMOD_System_PlaySound` | ✅ | |
| `FMOD_Sound_Release` | ✅ | |
| `FMOD_Channel_Stop` | ✅ | Via `ChannelControl::stop` |
| `FMOD_Channel_SetVolume` | ✅ | Via `ChannelControl::setVolume` |
| `FMOD_ChannelGroup_SetVolume` | ✅ | Via `ChannelControl::setVolume` |
| `FMOD_System_GetSoftwareFormat` | ❓ | No lo importa el juego, pero existe en la DLL |

El juego obtiene el puntero `FMOD::System*` (core) via `FMOD::Studio::System::getCoreSystem`.

### P6: Frecuencia de mezcla

**48000 Hz** — el juego no llama a `setSoftwareFormat`, así que FMOD Studio usa su valor por defecto.

### P2: Cómo reproduce la radio

**Arquitectura híbrida:** FMOD Studio maneja buses/routing; FMOD Core maneja reproducción.

**NO usa Programmer Sound ni eventos de Studio.** El flujo es:
1. `System::createStream()` para cargar el archivo o URL
2. `Bus::getChannelGroup("bus:/radio")` para obtener el ChannelGroup destino
3. `System::playSound(sound, radioChannelGroup, paused, &channel)` — **reproduce en el ChannelGroup del bus**
4. `Channel::setPosition()`, `ChannelControl::setPaused()` para control
5. `Bus::setVolume()` o `ChannelControl::setVolume()` para volumen

**3 modos de radio:**
| Modo | Tipo | Fuente |
|------|------|--------|
| 1 | Online Streams | `/home/live_streams.sii` |
| 2 | Music Player | Archivos locales del usuario |
| 3 | Offline Radio | `/offline_radio/` |

**Buses encontrados (27 total):**
- `bus:/radio` — bus principal de la radio
- `bus:/cabin`, `bus:/cabin/interior`, `bus:/cabin/window_left`, `bus:/cabin/window_right`, `bus:/cabin/window_out`
- `bus:/outside`, `bus:/outside/exterior/truck_engine`, `bus:/outside/exterior/truck_exhaust`
- `bus:/game/music`, `bus:/game/navigation`, `bus:/game/voiceover/speaker`

**Implicación para el plan:** reproducir el audio externo con `playSound(sound, radioChannelGroup, ...)` es
correcto. El audio entrará al mismo canal que la radio y recibirá todo el procesamiento del bus.

### P3: Control de volumen de la radio

- **Se aplica con `FMOD::Studio::Bus::setVolume()`** sobre el bus `bus:/radio` (ID interno `0x1a`).
- Función principal: `FUN_14028e560` llama a `setVolume` con soporte para fade gradual.
- Variables de configuración:
  - `s_radio_volume` (0x1422e1270) — nivel de volumen
  - `s_radio_mute` (0x1422e14f8) — si está muteado

### P4: Qué pasa al apagar la radio

**El bus sigue activo pero silenciado.** NO hace pause ni stop.

Secuencia de apagado (`FUN_14028e690`):
1. `Bus::setVolume(bus, 0.0)` — baja volumen a 0
2. `Bus::setMute(bus, true)` — mutea el bus
3. Guarda flag de estado

Secuencia de encendido (`FUN_14028e720`):
1. `Bus::setVolume(bus, 0.0)` — empieza en 0
2. `Bus::setMute(bus, false)` — desmutea
3. Fade gradual hacia el volumen objetivo

**Implicación para el audio externo:** como el audio externo va al mismo bus, también se silenciará
cuando el usuario apague la radio en el juego. Esto puede ser:
- **Deseable:** la perilla de radio del camión controla todo, incluyendo el audio de Chrome.
- **No deseable:** el usuario quiere oír Chrome aunque la radio del juego esté apagada.

Decisión de diseño: por ahora, se acepta el comportamiento de que la radio controla todo. Si el
usuario quiere audio externo sin la radio del juego, simplemente no sintoniza ninguna emisora.

---

---

## Control de Radio desde Código Externo

Análisis adicional con Ghidra para controlar la radio programáticamente.

### Direcciones Globales (ETS2 1.62.1.0s)

| Elemento | Dirección | Descripción |
|----------|-----------|-------------|
| Sound Manager | `0x1430b5780` | `prism::sound_manager_t` |
| Estado del manager | `0x1430b5940` | 0=no init, 1=initializing, 2=ready |
| Array de buses | `0x1430b5850` | Puntero a array de `fmod_bus_t*` |
| Count de buses | `0x1430b5858` | 27 buses |
| FMOD Studio System | `0x1430b5948` | Puntero global |
| FMOD Core System | `0x1430b5950` | Puntero global |
| Bus de radio | índice **26** | `"bus:/radio"` |

### Estructura `fmod_bus_t` (0x50 bytes)

```c
struct fmod_bus_t {
    int type;                    // +0x00
    float volume;                // +0x04 (volumen guardado)
    uint32_t mute_flags;         // +0x08 (bitfield de mute)
    bool some_flag;              // +0x0C
    uint64_t fade_end;           // +0x10 (timestamp fin de fade)
    float target_volume;         // +0x18 (volumen objetivo)
    float volume_delta;          // +0x1C (incremento por tick)
    FMOD::Studio::Bus* fmod_bus; // +0x20
    FMOD_DSP* dsp_lowpass;       // +0x28 (tipo 7)
    FMOD_DSP* dsp_17;            // +0x30
    FMOD_DSP* dsp_25;            // +0x38
    FMOD_DSP* dsp_32;            // +0x40
    FMOD_CHANNELGROUP* group;    // +0x48
};
```

### Funciones del Juego

| Función | Dirección | Signatura | Descripción |
|---------|-----------|-----------|-------------|
| Apagar bus | `0x14028e690` | `(fmod_bus_t*, uint32_t flags, float fade)` | `setVolume(0)` + `setMute(true)` |
| Encender bus | `0x14028e720` | `(fmod_bus_t*, uint32_t flags, float fade)` | `setMute(false)` + fade |
| Set volumen | `0x14028e560` | `(fmod_bus_t*, float vol, float fade)` | Transición gradual |
| Control general | `0x14028ddc0` | `(sound_mgr*, bus_id, vol, mute, force)` | Set volumen y mute |

### Cómo Obtener el Bus de Radio

```c
// Leer el array de buses
uint64_t bus_array = *(uint64_t*)0x1430b5850;
// Obtener el bus de radio (índice 26)
fmod_bus_t* radio_bus = *(fmod_bus_t**)(bus_array + 26 * 8);
// Obtener el puntero FMOD
FMOD::Studio::Bus* fmod_bus = *(FMOD::Studio::Bus**)((char*)radio_bus + 0x20);
// Obtener el ChannelGroup
FMOD_CHANNELGROUP* group = *(FMOD_CHANNELGROUP**)((char*)radio_bus + 0x48);
```

### Estrategia para Audio Externo

**Problema:** si silenciamos el bus de radio, también silenciamos nuestro audio externo.

**Solución:** reproducir nuestro audio en el **ChannelGroup padre** (master) en vez del bus de radio,
aplicando los efectos de cabina manualmente. O:

1. Obtener el volumen actual de la emisora del juego
2. Cuando hay audio externo, bajar el volumen de la emisora a 0
3. Nuestro audio va al bus pero con volumen propio
4. Restaurar el volumen de la emisora cuando no hay audio externo

---

## Fase 0 completada

Todas las preguntas respondidas. Resumen:

| Pregunta | Respuesta |
|----------|-----------|
| P1: Versión FMOD | 2.03.17 (0x20311) |
| P2: Cómo reproduce la radio | `createStream()` + `playSound(sound, radioChannelGroup)` — FMOD Core, no Programmer Sound |
| P3: Control de volumen | `Bus::setVolume()` sobre `bus:/radio` |
| P4: Al apagar la radio | `setVolume(0)` + `setMute(true)` — bus activo pero silenciado |
| P5: Exports necesarios | ✅ Todos disponibles |
| P6: Frecuencia de mezcla | 48000 Hz |

---

## Revisión de seguridad (por palabras clave, no auditoría completa)

- Sin red: no hay sockets, HTTP, WinHTTP ni descargas.
- Sin ofuscación ni base64.
- La inyección por `CreateRemoteThread` y el parche de la IAT son normales en mods de audio, pero
  los antivirus pueden marcarlos como sospechosos.
- El zip trae solo código fuente, sin binarios: hay que compilarlo (`build-windows.sh` requiere Zig).
