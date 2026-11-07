# Plan — Audio externo (Chrome/YouTube) dentro de `bus:/radio` (opción B)

Fecha: 2026-11-07. Contexto previo: ver `HALLAZGOS.md`.

**Actualización:** el plugin maestro (`ets2_ds_plugin`) ya tiene captura WASAPI por proceso y un
sistema IPC con seqlock probado. Se reutiliza en vez de escribir un bridge desde cero.

## Objetivo

Que el audio de un programa externo (pestaña de Chrome, YouTube, Spotify…) suene **dentro de la
radio del camión** con todo el procesamiento de cabina: 4.1, `cabin_rot`, `cabin_out` y ventanillas.
La latencia tiene que ser baja (< 100 ms), sin pasar por el buffer de las emisoras del juego.

## Arquitectura propuesta (revisada)

```
 Chrome ──(WASAPI process loopback)──► ets2_ds_plugin GUI (audio_capture.cpp)
                                          │ float32 estéreo 48 kHz
                                          ▼
                         Canal IPC "Local\ds_plugin.audio_bridge"
                         (ring buffer SPSC con seqlock, como los otros canales)
                                          │
                                          ▼
 eurotrucks2.exe ── ds_core.dll (*) ── lee el canal ── pasa a ETS2CabinDSP.dll
                                          │
                                          ▼
               ETS2CabinDSP.dll ── FMOD "user sound" (pcmreadcallback)
                                          │ playSound(..., channelgroup de bus:/radio)
                                          ▼
                     bus:/radio ► THREE_EQ ► Spatializer ► COMPRESSOR ► salida

(*) ds_core.dll es el DLL que ya se inyecta en el juego. Puede crear el canal de audio bridge
    y exponerlo a ETS2CabinDSP.dll, o ETS2CabinDSP.dll puede abrir el canal directamente si
    la licencia no lo controla.
```

Decisión clave: el audio externo **no** se mezcla dentro de `cabin_spatial_dsp_read`. Se reproduce
como un **Sound de FMOD core** con `FMOD_OPENUSER | FMOD_CREATESTREAM` y un `pcmreadcallback` que
lee del ring buffer, y se toca con `FMOD_System_PlaySound` sobre el channel group de `bus:/radio`.
Así entra **antes** de la cadena de DSP existente y recibe gratis todo el procesamiento, además del
volumen y la pausa del bus.

---

## Fase 0 — Investigación con Ghidra sobre `eurotrucks2.exe` (próximo modelo)

Ruta típica: `<Steam>\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe`.
Si el launcher ya se usó, la ruta está en `HKCU\Software\ETS2CabinRadio\GameExePath`.
Analizar también `fmod.dll` y `fmodstudio.dll` de la misma carpeta.

Herramientas: MCP `ghidra` (`import_file`, `load_tool_group`, …). Cargar los grupos de
herramientas necesarios antes de usarlas.

Preguntas que hay que responder, con dirección o función de evidencia. El resultado se agrega como
sección nueva en `HALLAZGOS.md`:

1. **Versión exacta de FMOD.** Recurso de versión o strings de `fmod.dll` y `fmodstudio.dll`.
   Hace falta para el tamaño y layout de `FMOD_CREATESOUNDEXINFO` (cambia entre versiones 2.x) y
   para confirmar el layout de 0xD8 bytes de `FMOD_DSP_DESCRIPTION` que ya asume el código
   (`LiveCabinDSP.c:89`).
2. **Cómo reproduce el juego la radio.** Buscar los strings `bus:/radio`, `live_streams`,
   `radio`, `.mp3` y `.ogg`, y las referencias a imports de `fmod.dll`:
   - ¿Usa FMOD core (`System::createStream`/`createSound` + `playSound`) o un evento de Studio con
     *programmer sound*?
   - ¿Sobre qué channel group toca el `Channel`? ¿Es el mismo que devuelve
     `FMOD_Studio_Bus_GetChannelGroup("bus:/radio")`?
3. **Volumen y on/off de la radio.** ¿El juego aplica el volumen de radio de las opciones con
   `Bus::setVolume`, `ChannelGroup::setVolume` o `Channel::setVolume`? Al apagar la radio, ¿pausa o
   mutea el bus, o solo detiene el canal? Esto define si el audio externo respeta la perilla de
   volumen y el botón de apagado.
4. **¿El bus procesa sin radio activa?** ¿El channel group de `bus:/radio` existe y se mezcla
   cuando no suena ninguna emisora? (Hoy `try_install` usa `LockChannelGroup` para forzarlo,
   `LiveCabinDSP.c:895-912`). Si el juego pausa el grupo, el Sound externo también se pausa.
5. **Exports necesarios en `fmod.dll`.** Confirmar que existen: `FMOD_System_CreateSound`,
   `FMOD_System_PlaySound`, `FMOD_Sound_Release`, `FMOD_Channel_Stop`, `FMOD_Channel_SetVolume`,
   `FMOD_Channel_IsPlaying`, `FMOD_System_GetSoftwareFormat` y `FMOD_ChannelGroup_SetVolume`.
6. **Frecuencia de mezcla del juego.** `System::setSoftwareFormat` o el valor por defecto. Hoy el
   DSP asume 48 kHz. Si el juego usa otra frecuencia, el plugin capturador tiene que remuestrear.
7. **(Opcional) Estado de la radio en memoria.** Una variable u objeto que indique si la radio está
   encendida o qué emisora suena. Sirve para silenciar la emisora del juego mientras suena el
   audio externo, o al revés.

Criterio de salida de la fase: las preguntas 1–5 respondidas. Si la 4 resulta negativa (el bus no
procesa sin emisora), replantear: tocar el Sound en el master channel group con su propia copia de
la cadena de DSP.

---

## Fase 1 — Canal de audio raw en `ets2_ds_plugin`

Se reutiliza el sistema IPC existente (`ds_shared/ipc.hpp`). Cambios en el plugin maestro:

### 1.1 Nuevo estado `AudioBridgeState` (`shared/include/ds_shared/audio_bridge_ipc.hpp`)

```cpp
namespace ds_shared {

// Capacidad del ring buffer en frames (potencia de 2). ~340 ms a 48 kHz.
constexpr uint32_t kAudioBridgeCapacity = 16384;

struct AudioBridgeState {
    static constexpr const char* kChannelName = "audio_bridge";

    uint32_t sample_rate   = 48000;
    uint32_t channels      = 2;
    float    gain          = 1.0f;      // volumen del audio externo
    uint32_t enabled       = 0;         // 1 = reproducir en la radio

    // Ring buffer SPSC. El seqlock de Channel<T> no sirve para audio continuo (copia todo el
    // struct); se usa un contador monótono aparte.
    alignas(64) volatile int64_t write_pos = 0;   // frames escritos (monótono)
    alignas(64) volatile int64_t read_pos  = 0;   // frames leídos (lo escribe el lector)
    float data[kAudioBridgeCapacity * 2];         // estéreo intercalado
};

}  // namespace ds_shared
```

El canal se crea con `Channel<AudioBridgeState>` como los demás. La GUI escribe `write_pos` y
`data`; el DLL (o `ETS2CabinDSP.dll`) escribe `read_pos` con `store_readback`.

### 1.2 Modificar `audio_capture.cpp`

Además de analizar, copiar las muestras al ring buffer:

```cpp
// En thread_proc(), después de process_float/process_pcm16:
if (g_bridge_channel.valid() && g_bridge_enabled) {
    // Convertir a float estéreo 48 kHz si hace falta (resample simple)
    // Copiar al ring buffer con write_pos monótono
    // MemoryBarrier() antes de publicar write_pos
}
```

### 1.3 UI en la pestaña de video (o una nueva)

- Toggle "Audio externo a la radio"
- Selector de proceso (ya existe `audio_list_processes()`)
- Slider de ganancia
- Indicador: "Conectado / Esperando juego"

## Fase 2 — Lector en `ETS2CabinDSP.dll`

Cambios en `LiveCabinDSP.c`:

1. `CabinAPI` + `resolve_symbols()` (`:341-358`): agregar los símbolos de la pregunta 5 como
   **opcionales**. Si faltan, se desactiva solo el audio externo y la cadena actual sigue igual.
2. Abrir `Local\ETS2CabinBridge` con `OpenFileMappingA` de forma perezosa en `cabin_after_update`.
   Reintentar cada ~2 s si el plugin capturador no está corriendo.
3. Crear el Sound con `FMOD_OPENUSER | FMOD_CREATESTREAM | FMOD_LOOP_NORMAL`, usando
   `FMOD_CREATESOUNDEXINFO` con el layout correcto según la versión de FMOD (pregunta 1),
   `defaultfrequency = sample_rate`, `numchannels = 2`, `format = FMOD_SOUND_FORMAT_PCMFLOAT` y
   `decodebuffersize` chico (≈ 512–1024 frames).
4. `pcmreadcallback` (hilo del mixer: sin locks, sin logs, sin malloc):
   - Copia `min(disponible, pedido)` frames y rellena con silencio en underrun.
   - Control de deriva: si `write_pos - read_pos` supera ~2× el objetivo (≈ 60 ms), descarta hasta
     dejar el objetivo. Así la latencia no crece con el tiempo.
   - Aplica `gain`, con rampa al cambiar.
5. `FMOD_System_PlaySound(core, sound, radio_group, false, &channel)` una vez instalado
   `try_install`. Si `active == 0` durante más de 1 s, `Channel_SetVolume(0)` o stop. Retomar
   cuando vuelva.
6. Configuración en `live-settings.env` (hot reload ya existente, `read_settings` `:448-466`):
   `external=1|0` y `external_gain=0.0..2.0`.
7. Logs nuevos en `live-dsp.log`: bridge conectado o desconectado, formato, underruns por minuto.

## Fase 3 — Integración en el launcher

- `windows_main.c`: sección "Audio externo" con un toggle, un slider de volumen y un indicador de
  estado ("Plugin conectado / esperando"). Escribe en `live-settings.env` + registro, como los
  demás ajustes (`:95-117`). La elección del programa que se captura queda en el plugin del usuario.
- `build-windows.sh`: compilar `tools/bridge_test.c` solo como herramienta de prueba (opcional).
- macOS: fuera de alcance por ahora (no hay process loopback equivalente sin un driver virtual).

## Fase 4 — Pruebas

| Caso | Esperado |
|---|---|
| Juego + `bridge_test` (tono 440 Hz) | El tono suena con efecto cabina (según pregunta 4) |
| Juego + plugin capturador real | El audio de Chrome suena con efecto cabina |
| Girar la cámara / `cabin_out` / ventanillas | Mismo comportamiento que la radio normal |
| Cerrar el productor con el juego abierto | Silencio limpio, sin crash; reconecta al volver |
| Productor cae sin poner `active = 0` | Se detecta por `write_pos` detenido > 1 s |
| Abrir el productor antes o después del juego | Conecta en ambos órdenes |
| 1 h de reproducción | Latencia estable (sin deriva), sin aumento de memoria |
| `ETS2_CABIN_DRY_RUN=1` | Nada se adjunta, tampoco el Sound externo |
| Símbolos FMOD faltantes | Solo se desactiva el audio externo; la cadena 4.1 sigue funcionando |

## Riesgos y preguntas abiertas

- **Layout de `FMOD_CREATESOUNDEXINFO`:** un tamaño incorrecto provoca un crash dentro del juego.
  Depende de la Fase 0, pregunta 1.
- **Pausa o volumen del bus:** si el juego pausa `bus:/radio` al apagar la radio, el audio externo
  también se silencia. Puede ser deseable (la perilla de la radio lo controla) o no. Decidir con
  la evidencia de las preguntas 3 y 4.
- **Mezcla con la emisora del juego:** si las dos suenan a la vez, se pisan. MVP: el usuario apaga
  la radio del juego o pone una emisora en silencio. Mejora: la pregunta 7.
- **Contrato estable:** una vez que el plugin capturador use `cabin_bridge.h`, cualquier cambio de
  layout tiene que subir `version`. La DLL ignora mappings con `magic` o `version` desconocidos.
- **Multijugador (TruckersMP):** comprobar que la inyección sea tolerada. Ya aplica al mod actual.

## Orden de trabajo

1. Fase 0 (Ghidra) → actualizar `HALLAZGOS.md`.
2. Fase 1: `cabin_bridge.h` + `tools/bridge_test.c`.
3. Fase 2 con valores fijos (sin UI), probada con el tono de `bridge_test`.
4. El usuario conecta su plugin capturador al contrato.
5. Fase 3 (UI) y Fase 4 completa.
